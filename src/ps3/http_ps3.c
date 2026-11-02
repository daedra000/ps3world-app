/*
http_ps3.c: HTTP/1.1 over the PS3's sockets, with TLS 1.2 from mbedTLS.

From NZ:P PS3's ps3_https.c (daedra000/nzp-ps3), changed for a music player:
  - every request has its own connection, TLS context and random generator,
    so the browser, the album art and the music download run side by side;
  - extra request headers (Plex's X-Plex-* and Accept: application/json);
  - the body of any status reaches the sink (Plex explains a 400 in it).

The system's own HTTPS (cellHttps) only speaks the TLS of ~2015, which
plex.tv and the *.plex.direct certificates refuse. mbedTLS checks servers
against the Mozilla roots (ca_bundle.h, made by CI).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include <net/net.h>
#include <net/netdb.h>
#include <net/socket.h>
#include <net/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <lv2/system.h>
#include <lv2/systime.h>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform_time.h"

#include "../plat.h"
#include "../lang.h"
#include "../version.h"

#ifndef EWOULDBLOCK
#define EWOULDBLOCK EAGAIN
#endif

#if __has_include("ca_bundle.h")
#include "ca_bundle.h"
#define HAVE_CA_BUNDLE 1
#elif !defined(ALLOW_NO_HTTPS)
// a build without root certificates can't talk to plex.tv at all: stop here
// rather than ship one (CI writes ca_bundle.h; -DALLOW_NO_HTTPS for a test build)
#error "src/ps3/ca_bundle.h is missing: see the mbedTLS step in .github/workflows/ps3.yml"
#endif

#define MAX_REDIRECTS	8

static plat_mutex_t		*init_lock;		// TLS roots, DNS
static int				tls_state;		// 0 not tried, 1 ok, -1 failed
static mbedtls_x509_crt	cacert;			// read-only once parsed: shared
static int				num_roots;

typedef struct {
	int						fd;
	int						tls;
	mbedtls_ssl_context		ssl;
	mbedtls_ssl_config		conf;
	mbedtls_entropy_context	entropy;
	mbedtls_ctr_drbg_context drbg;
	int						tls_made;
	unsigned char			buf[16384];
	int						pos, len;
	volatile int			*cancel;
	int						read_timeout_s;	// this long with nothing arriving: give up
	uint64_t				deadline_us;	// the whole request (0: none, a stream)
} conn_t;

// mbedTLS 3.6 asks the platform for a millisecond clock (MBEDTLS_PLATFORM_MS_TIME_ALT)
mbedtls_ms_time_t mbedtls_ms_time(void)
{
	return (mbedtls_ms_time_t)(sysGetSystemTime() / 1000);
}

#define SLICE_S		1		// recv waits this long at a time, so a cancel is seen within a second

static int Expired(conn_t *c, uint64_t since)
{
	uint64_t now = sysGetSystemTime();
	if (c->deadline_us && now > c->deadline_us) return 1;
	return now - since > (uint64_t)c->read_timeout_s * 1000000;
}

typedef struct {
	int		tls;
	int		port;
	char	host[256];
	char	path[4096];
} url_t;

static void SetErr(char *err, int size, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(err, size, fmt, ap);
	va_end(ap);
}

static int PS3Entropy(void *data, unsigned char *out, size_t len, size_t *olen)
{
	size_t done = 0;
	(void)data;
	while (done < len) {
		size_t n = len - done > 0x1000 ? 0x1000 : len - done;
		if (sysGetRandomNumber(out + done, n) != 0)
			return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
		done += n;
	}
	*olen = len;
	return 0;
}

// the shared part: the root certificates, parsed once
static int Tls_Roots(void)
{
	int ret;

	plat_mutex_lock(init_lock);
	if (tls_state) {
		plat_mutex_unlock(init_lock);
		return tls_state > 0;
	}
	tls_state = -1;
	mbedtls_x509_crt_init(&cacert);
#ifdef HAVE_CA_BUNDLE
	ret = mbedtls_x509_crt_parse(&cacert, (const unsigned char *)ca_bundle_pem, sizeof(ca_bundle_pem));
	for (mbedtls_x509_crt *c = &cacert; c && c->raw.len; c = c->next)
		num_roots++;
	plat_log("https: %d root certificates (%d skipped)", num_roots, ret > 0 ? ret : 0);
#else
	(void)ret;
#endif
	// fail closed: no roots means no HTTPS, not HTTPS that trusts anyone
	if (num_roots)
		tls_state = 1;
	else
		plat_log("https: no root certificates: HTTPS is off");
	plat_mutex_unlock(init_lock);
	return tls_state > 0;
}

static int BioSend(void *ctx, const unsigned char *buf, size_t len)
{
	conn_t *c = ctx;
	int n = send(c->fd, buf, len, 0);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
	return n;
}

static int BioRecv(void *ctx, unsigned char *buf, size_t len)
{
	conn_t *c = ctx;
	int n = recv(c->fd, buf, len, 0);
	if (n < 0)
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
	return n;
}

static void Conn_Close(conn_t *c)
{
	if (c->tls) {
		mbedtls_ssl_close_notify(&c->ssl);
		mbedtls_ssl_free(&c->ssl);
		c->tls = 0;
	}
	if (c->tls_made) {
		mbedtls_ssl_config_free(&c->conf);
		mbedtls_ctr_drbg_free(&c->drbg);
		mbedtls_entropy_free(&c->entropy);
		c->tls_made = 0;
	}
	if (c->fd >= 0) {
		close(c->fd);
		c->fd = -1;
	}
}

// numbers-and-dots addresses skip DNS (most Plex connections are)
static int Resolve(const char *host, struct in_addr *out)
{
	struct hostent *he;
	int ok = 0;

	if (inet_aton(host, out))
		return 1;
	// lv2's resolver isn't known to be reentrant: one lookup at a time
	plat_mutex_lock(init_lock);
	he = gethostbyname(host);
	if (he && he->h_addr_list && he->h_addr_list[0]) {
		memcpy(out, he->h_addr_list[0], sizeof(*out));
		ok = 1;
	}
	plat_mutex_unlock(init_lock);
	return ok;
}

static int Conn_Open(conn_t *c, const char *host, int port, int tls, int timeout_s, char *err, int errsize)
{
	struct sockaddr_in sa;
	struct timeval tv;
	int ret;
	uint64_t t0 = sysGetSystemTime();

	c->fd = -1;
	c->tls = 0;
	c->tls_made = 0;
	c->pos = c->len = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	if (!Resolve(host, &sa.sin_addr)) {
		SetErr(err, errsize, "Can't find %s (is the PS3 online?)", host);
		return 0;
	}
	if ((c->fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		SetErr(err, errsize, "socket() failed (%d)", errno);
		return 0;
	}
	c->read_timeout_s = timeout_s;
	tv.tv_sec = SLICE_S;
	tv.tv_usec = 0;
	setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	tv.tv_sec = timeout_s;
	setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	// connect without blocking, so an address that never answers (a stale LAN IP) gives
	// up after the timeout instead of the stack's minute and more; a cancel is seen too
	int nb = 1, nbset = setsockopt(c->fd, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb)) == 0;
	if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		int ok = 0, e = errno;
		if (nbset) {
			// (in progress, whatever errno says it as): wait until it's writable, a second at a time
			for (;;) {
				struct pollfd pf = { c->fd, POLLOUT, 0 };
				int r = poll(&pf, 1, SLICE_S * 1000);
				if (r > 0) {
					int soerr = 0;
					socklen_t sl = sizeof(soerr);
					ok = getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr == 0;
					if (!ok) e = soerr;
					break;
				}
				if (r < 0 || (c->cancel && *c->cancel) || sysGetSystemTime() - t0 > (uint64_t)timeout_s * 1000000) {
					e = r < 0 ? errno : ETIMEDOUT;
					break;
				}
			}
		}
		if (!ok) {
			SetErr(err, errsize, "Can't connect to %s:%d (%d)", host, port, e);
			Conn_Close(c);
			return 0;
		}
	}
	if (nbset) {
		nb = 0;
		setsockopt(c->fd, SOL_SOCKET, SO_NBIO, &nb, sizeof(nb));
	}
	if (!tls)
		return 1;

	if (!Tls_Roots()) {
		SetErr(err, errsize, "Secure connections are unavailable");
		Conn_Close(c);
		return 0;
	}
	mbedtls_entropy_init(&c->entropy);
	mbedtls_ctr_drbg_init(&c->drbg);
	mbedtls_ssl_config_init(&c->conf);
	c->tls_made = 1;
	mbedtls_entropy_add_source(&c->entropy, PS3Entropy, NULL, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
	if ((ret = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
			(const unsigned char *)"cellplex", 8)) != 0 ||
		(ret = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
			MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
		SetErr(err, errsize, "TLS setup -0x%04x", -ret);
		Conn_Close(c);
		return 0;
	}
	mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&c->conf, &cacert, NULL);
	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

	mbedtls_ssl_init(&c->ssl);
	c->tls = 1;
	if ((ret = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0 || (ret = mbedtls_ssl_set_hostname(&c->ssl, host)) != 0) {
		SetErr(err, errsize, "TLS setup -0x%04x", -ret);
		Conn_Close(c);
		return 0;
	}
	mbedtls_ssl_set_bio(&c->ssl, c, BioSend, BioRecv, NULL);
	while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		if (ret == MBEDTLS_ERR_SSL_TIMEOUT && !(c->cancel && *c->cancel) && !Expired(c, t0))
			continue;		// a quiet second: still within the timeout
		if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			char info[256];
			mbedtls_x509_crt_verify_info(info, sizeof(info), "", mbedtls_ssl_get_verify_result(&c->ssl));
			for (char *p = info; *p; p++) if (*p == '\n') *p = ' ';
			plat_log("https: %s: certificate rejected: %s", host, info);
			SetErr(err, errsize, "%s's certificate was rejected (is the PS3's clock right?)", host);
		} else {
			char e[96];
			mbedtls_strerror(ret, e, sizeof(e));
			plat_log("https: %s: handshake -0x%04x %s", host, -ret, e);
			SetErr(err, errsize, "Secure connection to %s failed (-0x%04x)", host, -ret);
		}
		Conn_Close(c);
		return 0;
	}
	return 1;
}

static int Conn_Write(conn_t *c, const char *buf, int len)
{
	while (len > 0) {
		int n = c->tls ? mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf, len) : send(c->fd, buf, len, 0);
		if (c->tls && (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE))
			continue;
		if (n <= 0)
			return 0;
		buf += n;
		len -= n;
	}
	return 1;
}

// refills c->buf: >0 bytes, 0 at the end of the stream, <0 an error (-2 cancelled)
static int Conn_Fill(conn_t *c)
{
	int n;
	uint64_t since = sysGetSystemTime();
	for (;;) {
		if (c->cancel && *c->cancel)
			return -2;
		n = c->tls ? mbedtls_ssl_read(&c->ssl, c->buf, sizeof(c->buf)) : recv(c->fd, c->buf, sizeof(c->buf), 0);
		if (c->tls) {
			if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
			if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
				n = 0;
		}
		// a quiet second (recv waits one at a time): again, unless the time's up
		int quiet = c->tls ? n == MBEDTLS_ERR_SSL_TIMEOUT : (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
		if (quiet && !Expired(c, since))
			continue;
		break;
	}
	c->pos = 0;
	c->len = n > 0 ? n : 0;
	return n;
}

static int Conn_Line(conn_t *c, char *out, int size)
{
	int o = 0;
	for (;;) {
		if (c->pos >= c->len && Conn_Fill(c) <= 0)
			return 0;
		char ch = c->buf[c->pos++];
		if (ch == '\n') {
			if (o && out[o - 1] == '\r') o--;
			out[o] = 0;
			return 1;
		}
		if (o < size - 1)
			out[o++] = ch;
	}
}

static int Url_Parse(const char *in, url_t *u)
{
	const char *p, *slash, *colon;
	int hlen, o = 0;

	if (!strncasecmp(in, "https://", 8)) { u->tls = 1; u->port = 443; p = in + 8; }
	else if (!strncasecmp(in, "http://", 7)) { u->tls = 0; u->port = 80; p = in + 7; }
	else return 0;

	slash = strchr(p, '/');
	if (!slash) slash = p + strlen(p);
	colon = memchr(p, ':', slash - p);
	hlen = (colon ? colon : slash) - p;
	if (hlen <= 0 || hlen >= (int)sizeof(u->host))
		return 0;
	memcpy(u->host, p, hlen);
	u->host[hlen] = 0;
	for (int i = 0; i < hlen; i++)		// a host name, not a way to slip more into the request
		if ((unsigned char)u->host[i] <= 0x20 || u->host[i] == 0x7f || u->host[i] == '/' || u->host[i] == '@')
			return 0;
	if (colon)
		u->port = atoi(colon + 1);
	if (!*slash) slash = "/";
	// spaces, control characters (a CR/LF in a key from a server would start a
	// new header) and non-ASCII bytes all go percent-encoded
	for (; *slash && o < (int)sizeof(u->path) - 4; slash++) {
		unsigned char ch = (unsigned char)*slash;
		if (ch <= 0x20 || ch >= 0x7f) { snprintf(u->path + o, 4, "%%%02X", ch); o += 3; }
		else u->path[o++] = *slash;
	}
	u->path[o] = 0;
	return 1;
}

static int Net_Up(void)
{
	// main_ps3.c brings the network up at start-up; this is the check
	extern int ps3_net_ok;
	return ps3_net_ok;
}

void http_init(void)
{
	init_lock = plat_mutex_new();
}

int http_request(const http_req_t *req, http_res_t *res)
{
	conn_t *c;
	url_t *u;
	char *line, *location, *head;
	int redirects = 0, status, chunked, ok = 0;
	int timeout = req->timeout_s > 0 ? req->timeout_s : 30;
	const char *method = req->method ? req->method : "GET";
	long long length, got;

	memset(res, 0, sizeof(*res));
	res->length = -1;

	if (!Net_Up()) {
		SetErr(res->err, sizeof(res->err), "%s", tr("The network isn't available"));
		return 0;
	}

	c = calloc(1, sizeof(*c));
	u = calloc(1, sizeof(*u));
	line = malloc(4096);
	location = malloc(4096);
	head = malloc(8192);
	if (!c || !u || !line || !location || !head) {
		SetErr(res->err, sizeof(res->err), "%s", tr("Out of memory"));
		goto out;
	}
	c->fd = -1;
	snprintf(location, 4096, "%s", req->url);

again:
	if (!Url_Parse(location, u)) {
		SetErr(res->err, sizeof(res->err), "%s", tr("Bad URL"));
		goto out;
	}
	c->cancel = req->cancel;
	if (req->total_s > 0 && !c->deadline_us)
		c->deadline_us = sysGetSystemTime() + (uint64_t)req->total_s * 1000000;	// across redirects too
	if (!Conn_Open(c, u->host, u->port, u->tls, timeout, res->err, sizeof(res->err)))
		goto out;

	{
		int n = snprintf(head, 8192, "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PS3World/" APP_VERSION
			" (PLAYSTATION 3)\r\nAccept-Encoding: identity\r\nConnection: close\r\n%s",
			method, u->path, u->host, req->headers ? req->headers : "");
		if (req->body)
			n += snprintf(head + n, 8192 - n, "Content-Length: %d\r\n", req->body_len);
		snprintf(head + n, 8192 - n, "\r\n");
	}
	if (!Conn_Write(c, head, strlen(head)) ||
		(req->body && req->body_len && !Conn_Write(c, req->body, req->body_len))) {
		SetErr(res->err, sizeof(res->err), tr("Request to %s failed"), u->host);
		goto done;
	}

	if (!Conn_Line(c, line, 4096) || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
		SetErr(res->err, sizeof(res->err), tr("No reply from %s"), u->host);
		goto done;
	}
	length = -1;
	chunked = 0;
	location[0] = 0;
	res->content_type[0] = 0;
	for (;;) {
		if (!Conn_Line(c, line, 4096)) {
			SetErr(res->err, sizeof(res->err), tr("Reply from %s cut short"), u->host);
			goto done;
		}
		if (!line[0])
			break;
		if (!strncasecmp(line, "Content-Length:", 15)) {
			char *end;
			length = strtoll(line + 15, &end, 10);
			if (end == line + 15 || length < 0) {		// not a length: don't guess
				SetErr(res->err, sizeof(res->err), "%s", tr("The reply wasn't understood"));
				goto done;
			}
		}
		else if (!strncasecmp(line, "Transfer-Encoding:", 18)) {
			for (char *q = line + 18; *q; q++) if (*q >= 'A' && *q <= 'Z') *q += 32;	// names are case-insensitive
			if (strstr(line + 18, "chunked")) chunked = 1;
		}
		else if (!strncasecmp(line, "Content-Type:", 13)) {
			const char *v = line + 13;
			while (*v == ' ') v++;
			snprintf(res->content_type, sizeof(res->content_type), "%s", v);
		} else if (!strncasecmp(line, "Location:", 9)) {
			const char *v = line + 9;
			while (*v == ' ') v++;
			if (*v == '/')
				snprintf(location, 4096, "%s://%s:%d%s", u->tls ? "https" : "http", u->host, u->port, v);
			else
				snprintf(location, 4096, "%s", v);
		}
	}

	res->status = status;
	if (status >= 300 && status < 400 && location[0] && !req->body && !req->no_redirect) {
		Conn_Close(c);
		if (u->tls && strncasecmp(location, "https://", 8)) {
			SetErr(res->err, sizeof(res->err), "%s", tr("Insecure redirect refused"));
			goto out;
		}
		// the token only ever goes to the host it was meant for
		if (req->headers && strstr(req->headers, "X-Plex-Token")) {
			url_t *nu = malloc(sizeof(*nu));
			int same = nu && Url_Parse(location, nu) && !strcasecmp(nu->host, u->host) && nu->port == u->port && nu->tls == u->tls;
			free(nu);
			if (!same) {
				SetErr(res->err, sizeof(res->err), "%s", tr("Insecure redirect refused"));
				goto out;
			}
		}
		if (++redirects > MAX_REDIRECTS) {
			SetErr(res->err, sizeof(res->err), "%s", tr("Too many redirects"));
			goto out;
		}
		goto again;
	}
	res->length = length;

	got = 0;
	for (;;) {
		long long left = chunked ? 0 : (length >= 0 ? length - got : -1);
		if (chunked) {
			if (!Conn_Line(c, line, 4096)) { SetErr(res->err, sizeof(res->err), "%s", tr("Download cut short")); goto done; }
			// a chunk's size in hex, maybe with ";extensions"; anything else is a broken reply
			char *end;
			left = strtoll(line, &end, 16);
			while (*end == ' ' || *end == '\t') end++;
			if (!isxdigit((unsigned char)line[0]) || end == line || left < 0 || left > (1ll << 40) || (*end && *end != ';')) {
				SetErr(res->err, sizeof(res->err), "%s", tr("The reply wasn't understood"));
				goto done;
			}
			if (left == 0) { ok = 1; break; }
		}
		if (left == 0) { ok = 1; break; }
		while (left != 0) {
			int n;
			if (c->pos >= c->len) {
				int r = Conn_Fill(c);
				if (r == -2) { SetErr(res->err, sizeof(res->err), "%s", tr("Cancelled")); goto done; }
				if (r <= 0) {
					if (left < 0 && r == 0) { ok = 1; goto done; }	// no length: the end is the close
					SetErr(res->err, sizeof(res->err), "%s", tr("Download cut short"));
					goto done;
				}
			}
			n = c->len - c->pos;
			if (left > 0 && n > left)
				n = (int)left;
			if (req->sink && !req->sink(req->ctx, c->buf + c->pos, n)) {
				if (!res->err[0]) SetErr(res->err, sizeof(res->err), "%s", tr("Stopped"));
				goto done;
			}
			c->pos += n;
			got += n;
			if (left > 0)
				left -= n;
		}
		if (!chunked) { ok = 1; break; }
		// the CRLF after a chunk, and nothing else
		if (!Conn_Line(c, line, 4096) || line[0]) {
			SetErr(res->err, sizeof(res->err), "%s", tr("The reply wasn't understood"));
			goto done;
		}
	}

done:
	Conn_Close(c);
out:
	free(c);
	free(u);
	free(line);
	free(location);
	free(head);
	return ok;
}
