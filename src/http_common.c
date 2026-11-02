/*
http_common.c: http_fetch, a request whose body is gathered into memory.
*/
#include <stdlib.h>
#include <string.h>
#include "plat.h"

// no reply we ask for (JSON pages, lyrics, artwork) comes near this; anything
// bigger is refused rather than allowed to eat the PS3's memory
#define HTTP_BODY_MAX	(24u * 1024 * 1024)

typedef struct {
	char *buf;
	size_t len, cap;
} membuf_t;

static int MemSink(void *ctx, const void *data, int len)
{
	membuf_t *m = ctx;
	if (len < 0 || m->len + (size_t)len + 1 > HTTP_BODY_MAX) {
		plat_log("http: reply bigger than %u MB, dropped", HTTP_BODY_MAX >> 20);
		return 0;
	}
	if (m->len + len + 1 > m->cap) {
		size_t cap = m->cap ? m->cap * 2 : 16384;
		while (cap < m->len + len + 1)
			cap *= 2;
		char *nb = realloc(m->buf, cap);
		if (!nb)
			return 0;
		m->buf = nb;
		m->cap = cap;
	}
	memcpy(m->buf + m->len, data, len);
	m->len += len;
	return 1;
}

char *http_fetch(const char *method, const char *url, const char *headers, const char *body,
	int timeout_s, volatile int *cancel, size_t *len_out, http_res_t *res)
{
	return http_fetch_ex(method, url, headers, body, timeout_s, cancel, len_out, res, 0);
}

char *http_fetch_ex(const char *method, const char *url, const char *headers, const char *body,
	int timeout_s, volatile int *cancel, size_t *len_out, http_res_t *res, int flags)
{
	http_req_t req;
	http_res_t local;
	membuf_t m = {0};

	if (!res)
		res = &local;
	memset(&req, 0, sizeof(req));
	req.method = method;
	req.url = url;
	req.headers = headers;
	req.body = body;
	req.body_len = body ? (int)strlen(body) : 0;
	req.cancel = cancel;
	req.timeout_s = timeout_s;
	req.sink = MemSink;
	req.ctx = &m;
	req.no_redirect = flags & HTTP_NO_REDIRECT;
	req.total_s = (timeout_s > 0 ? timeout_s : 30) * 2 + 10;		// a reply that trickles in forever still ends

	if (!http_request(&req, res)) {
		free(m.buf);
		return NULL;
	}
	if (!m.buf) {
		m.buf = malloc(1);
		if (!m.buf)
			return NULL;
	}
	m.buf[m.len] = 0;
	if (len_out)
		*len_out = m.len;
	return m.buf;
}
