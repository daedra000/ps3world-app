/*
https_test.c: the app's HTTPS client (src/ps3/http_ps3.c, the same Mbed TLS
build and root certificates) run on Linux against dl.ps3.world, before any UI
is built on it. CI's "HTTPS test" job; see .github/workflows/ps3.yml.

  ./https_test tests/sample-catalog.json

1. a whole file, streamed through the sink, against its known sha256
2. the same file resumed with Range from the middle: 206, and the two parts
   together hash to the same sha256
3. a missing file: a 404 that comes back as a status, not a hang or a crash
4. a server with an expired certificate: refused
5. catalog.json: the live one if it's been uploaded, and the sample, parsed
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include "mbedtls/sha256.h"
#include "plat.h"
#include "app.h"
#include "catalog.h"

int ps3_net_ok = 1;

void plat_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	printf("    log: ");
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
}

struct plat_mutex { pthread_mutex_t m; };
plat_mutex_t *plat_mutex_new(void)
{
	plat_mutex_t *m = malloc(sizeof(*m));
	pthread_mutex_init(&m->m, NULL);
	return m;
}
void plat_mutex_lock(plat_mutex_t *m) { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(plat_mutex_t *m) { pthread_mutex_unlock(&m->m); }

#define FILE_URL	"https://dl.ps3.world/homebrewgames/Avoidance%20v1.3.pkg"
#define FILE_SIZE	285360
#define FILE_SHA	"4df8da76f006b61f5755806d2497e02bdf4ca25738b5d0610cc6f51fab3c1511"
#define RESUME_AT	100000

static int failures;

static void Result(int ok, const char *name, const char *fmt, ...)
{
	va_list ap;
	printf("%s %s: ", ok ? "PASS" : "FAIL", name);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	if (!ok) failures++;
}

// the download sink: hashes as it goes and counts, like the app's will (nothing kept in memory)
typedef struct {
	mbedtls_sha256_context sha;
	long long got;
} sink_t;

static int Sink(void *ctx, const void *data, int len)
{
	sink_t *s = ctx;
	mbedtls_sha256_update(&s->sha, data, len);
	s->got += len;
	return 1;
}

static void Hex(const unsigned char *d, char *out)
{
	for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", d[i]);
}

static int Get(const char *url, const char *headers, sink_t *s, http_res_t *res)
{
	http_req_t req;
	memset(&req, 0, sizeof(req));
	req.url = url;
	req.headers = headers;
	req.sink = Sink;
	req.ctx = s;
	req.timeout_s = 30;
	req.total_s = 120;
	return http_request(&req, res);
}

int main(int argc, char **argv)
{
	http_res_t res;
	sink_t s;
	unsigned char d[32];
	char hex[65];
	http_init();

	// 1. whole file
	mbedtls_sha256_init(&s.sha);
	mbedtls_sha256_starts(&s.sha, 0);
	s.got = 0;
	int ok = Get(FILE_URL, NULL, &s, &res);
	mbedtls_sha256_finish(&s.sha, d);
	Hex(d, hex);
	Result(ok && res.status == 200 && s.got == FILE_SIZE && !strcmp(hex, FILE_SHA), "whole file",
		"HTTP %d, %lld bytes, sha256 %s%s%s", res.status, s.got, hex, res.err[0] ? ", " : "", res.err);

	// 2. resume: the first RESUME_AT bytes, then Range from there
	mbedtls_sha256_init(&s.sha);
	mbedtls_sha256_starts(&s.sha, 0);
	s.got = 0;
	char range[64];
	snprintf(range, sizeof(range), "Range: bytes=0-%d\r\n", RESUME_AT - 1);
	int ok1 = Get(FILE_URL, range, &s, &res);
	int st1 = res.status;
	snprintf(range, sizeof(range), "Range: bytes=%d-\r\n", RESUME_AT);
	int ok2 = Get(FILE_URL, range, &s, &res);
	mbedtls_sha256_finish(&s.sha, d);
	Hex(d, hex);
	Result(ok1 && ok2 && st1 == 206 && res.status == 206 && res.length == FILE_SIZE - RESUME_AT && s.got == FILE_SIZE &&
		!strcmp(hex, FILE_SHA), "Range resume", "HTTP %d then %d (second part %lld bytes), together %lld bytes, sha256 %s",
		st1, res.status, res.length, s.got, hex);

	// 3. 404
	mbedtls_sha256_init(&s.sha);
	mbedtls_sha256_starts(&s.sha, 0);
	ok = Get("https://dl.ps3.world/catalog/v1/no-such-file.json", NULL, &s, &res);
	Result(ok && res.status == 404, "404", "returned %d, HTTP %d", ok, res.status);

	// 4. a certificate that must be refused
	ok = Get("https://expired.badssl.com/", NULL, &s, &res);
	Result(!ok && strstr(res.err, "certificate"), "expired certificate refused", "returned %d: %s", ok, res.err);
	mbedtls_sha256_free(&s.sha);

	// 5. catalog.json: live (once uploaded) and the sample
	size_t len;
	char err[160];
	char *body = http_fetch(NULL, CATALOG_URL, NULL, NULL, 60, NULL, &len, &res);
	if (body && res.status == 200) {
		catalog_t *c = catalog_parse(body, len, err, sizeof(err));
		Result(c != NULL, "live catalog.json", "%lu bytes: %d items (%d homebrew, %d themes, %d avatars) %s",
			(unsigned long)len, c ? c->count : 0, c ? c->per_type[TYPE_HOMEBREW] : 0, c ? c->per_type[TYPE_THEME] : 0,
			c ? c->per_type[TYPE_AVATAR] : 0, err);
		catalog_free(c);
	} else {
		printf("SKIP live catalog.json: HTTP %d %s (not uploaded yet?)\n", res.status, res.err);
	}
	free(body);
	if (argc > 1) {
		FILE *f = fopen(argv[1], "rb");
		char *buf = NULL;
		long n = 0;
		if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); buf = malloc(n); fread(buf, 1, n, f); fclose(f); }
		catalog_t *c = buf ? catalog_parse(buf, n, err, sizeof(err)) : NULL;
		Result(c != NULL, "sample catalog parsed", "%s: %d items (%d homebrew, %d themes, %d avatars) %s", argv[1],
			c ? c->count : 0, c ? c->per_type[TYPE_HOMEBREW] : 0, c ? c->per_type[TYPE_THEME] : 0,
			c ? c->per_type[TYPE_AVATAR] : 0, err);
		if (c) printf("    first item: %s \"%s\" %lld bytes, %s\n", catalog_type_name(c->items[0].type), c->items[0].title,
			c->items[0].size, c->items[0].url);
		catalog_free(c);
		free(buf);
	}

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
