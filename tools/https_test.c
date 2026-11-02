/*
https_test.c: the app's HTTPS client (src/ps3/http_ps3.c, the same Mbed TLS
build and root certificates) run on Linux against dl.ps3.world, before any UI
is built on it. CI's "HTTPS test" job; see .github/workflows/ps3.yml.

  ./https_test tests/sample

1. a whole file, streamed through the sink, against its known sha256
2. the same file resumed with Range from the middle: 206, and the two parts
   together hash to the same sha256
3. a missing file: a 404 that comes back as a status, not a hang or a crash
4. a server with an expired certificate: refused
5. catalog.json: the live one if it's been uploaded, and the sample, parsed
6. download.c: a whole file to disk; one stopped part way and resumed with
   Range; a wrong sha256 (deleted); not enough space; a 404; a cancel

Each result is also printed as a GitHub annotation, readable without signing in.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "mbedtls/sha256.h"
#include "plat.h"
#include "app.h"
#include "catalog.h"
#include "download.h"

int ps3_net_ok = 1;

// ---- the plat_ file functions, on Linux ----
unsigned char *plat_read_file(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = malloc(n + 1);
	if (fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
	fclose(f);
	buf[n] = 0;
	if (len) *len = n;
	return buf;
}

int plat_write_file(const char *path, const void *data, size_t len)
{
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	int ok = fwrite(data, 1, len, f) == len;
	return (fclose(f) == 0) && ok;
}

long long plat_file_size(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
}

int plat_dir_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int plat_mkdirs(const char *path)
{
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *p = strchr(buf + 1, '/'); p; p = strchr(p + 1, '/')) { *p = 0; mkdir(buf, 0777); *p = '/'; }
	mkdir(buf, 0777);
	return plat_dir_exists(buf);
}

long long plat_free_bytes(const char *path)
{
	struct statvfs v;
	return statvfs(path, &v) == 0 ? (long long)v.f_bavail * (long long)v.f_frsize : -1;
}

int plat_list_dirs(const char *path, char (*names)[64], int max)
{
	DIR *d = opendir(path);
	struct dirent *e;
	int n = 0;
	if (!d) return 0;
	while (n < max && (e = readdir(d)))
		if (e->d_name[0] != '.' && e->d_type == DT_DIR) snprintf(names[n++], 64, "%s", e->d_name);
	closedir(d);
	return n;
}

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
static char summary[16384];

static void Result(int ok, const char *name, const char *fmt, ...)
{
	char msg[600];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	printf("%s %s: %s\n", ok ? "PASS" : "FAIL", name, msg);
	// GitHub shows 10 annotations a step: failures each get one, the passes share one at the end
	if (getenv("GITHUB_ACTIONS") && !ok)
		printf("::error title=FAIL %s::%s\n", name, msg);
	size_t n = strlen(summary);
	snprintf(summary + n, sizeof(summary) - n, "%s %s: %s%%0A", ok ? "PASS" : "FAIL", name, msg);
	if (!ok) failures++;
}

// ---- download.c ----
static const unsigned char *Want_Sha(void)
{
	static unsigned char d[32];
	for (int i = 0; i < 32; i++) sscanf(FILE_SHA + i * 2, "%2hhx", &d[i]);
	return d;
}

static void Dl_Setup(download_t *d, const char *url, const char *dest, long long size)
{
	memset(d, 0, sizeof(*d));
	snprintf(d->url, sizeof(d->url), "%s", url);
	snprintf(d->dest, sizeof(d->dest), "%s", dest);
	d->size = size;
	memcpy(d->sha256, Want_Sha(), 32);
}

// stops a download (pause or cancel) once this much has arrived
typedef struct { download_t *d; long long at; int cancel; } stopper_t;
static void *Stopper(void *p)
{
	stopper_t *s = p;
	while (s->d->state != DL_DONE && s->d->state != DL_FAILED && s->d->got < s->at) usleep(1000);
	if (s->cancel) s->d->cancel = 1; else s->d->pause = 1;
	return NULL;
}

static void Download_Tests(void)
{
	download_t d;
	char part[600];
	const char *dir = "/tmp/ps3world-dltest";
	char dest[256];
	plat_mkdirs(dir);
	snprintf(dest, sizeof(dest), "%s/Avoidance v1.3.pkg", dir);
	snprintf(part, sizeof(part), "%s.part", dest);
	remove(dest);
	remove(part);

	// whole file
	Dl_Setup(&d, FILE_URL, dest, FILE_SIZE);
	download_run(&d);
	Result(d.state == DL_DONE && plat_file_size(dest) == FILE_SIZE && plat_file_size(part) < 0, "download: whole file",
		"state %d, %lld bytes on disk, %s", d.state, plat_file_size(dest), d.err);
	remove(dest);

	// stopped part way (as the network dropping or the app quitting), then resumed
	pthread_t th;
	stopper_t st = {&d, 120000, 0};
	Dl_Setup(&d, FILE_URL, dest, FILE_SIZE);
	pthread_create(&th, NULL, Stopper, &st);
	download_run(&d);
	pthread_join(th, NULL);
	long long left = plat_file_size(part);
	int st1 = d.state;
	Dl_Setup(&d, FILE_URL, dest, FILE_SIZE);
	download_run(&d);
	Result(st1 == DL_PAUSED && left > 0 && d.state == DL_DONE && d.resumed_from == left && plat_file_size(dest) == FILE_SIZE,
		"download: resume with Range", "stopped at %lld bytes (state %d), resumed from %lld, then state %d, %lld bytes, sha256 checked; %s",
		left, st1, d.resumed_from, d.state, plat_file_size(dest), d.err);
	remove(dest);

	// a wrong sha256: refused and deleted
	Dl_Setup(&d, FILE_URL, dest, FILE_SIZE);
	d.sha256[0] ^= 0xff;
	download_run(&d);
	Result(d.state == DL_FAILED && d.err_kind == ERR_CHECKSUM && plat_file_size(dest) < 0 && plat_file_size(part) < 0,
		"download: checksum mismatch deleted", "state %d, kind %d: %s", d.state, d.err_kind, d.err);

	// more than the drive has
	Dl_Setup(&d, FILE_URL, dest, 1LL << 50);
	download_run(&d);
	Result(d.state == DL_FAILED && d.err_kind == ERR_DISK_FULL, "download: not enough space", "kind %d: %s", d.err_kind, d.err);

	// 404
	Dl_Setup(&d, "https://dl.ps3.world/homebrewgames/no-such-file.pkg", dest, 1000);
	download_run(&d);
	Result(d.state == DL_FAILED && d.err_kind == ERR_NOT_FOUND && plat_file_size(part) < 0, "download: 404",
		"kind %d: %s", d.err_kind, d.err);

	// cancel: what arrived is deleted
	stopper_t sc = {&d, 50000, 1};
	Dl_Setup(&d, FILE_URL, dest, FILE_SIZE);
	pthread_create(&th, NULL, Stopper, &sc);
	download_run(&d);
	pthread_join(th, NULL);
	Result(d.state == DL_CANCELLED && plat_file_size(part) < 0 && plat_file_size(dest) < 0, "download: cancel",
		"state %d, partial %lld", d.state, plat_file_size(part));
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

	// 5. the catalogue: the live index (once uploaded) and one of its pages, then the sample
	size_t len;
	char err[160];
	char *body = http_fetch(NULL, CATALOG_URL, NULL, NULL, 60, NULL, &len, &res);
	if (body && res.status == 200) {
		cat_index_t *ix = catalog_parse_index(body, len, err, sizeof(err));
		if (ix && ix->num_lists[TYPE_THEME]) {
			Result(1, "live index", "%lu bytes: %d homebrew in %d lists, %d themes in %d pages, %d avatars, generated %s",
				(unsigned long)len, ix->count[TYPE_HOMEBREW], ix->num_lists[TYPE_HOMEBREW], ix->count[TYPE_THEME],
				ix->lists[TYPE_THEME][0].num_pages, ix->count[TYPE_AVATAR], ix->generated);
			const cat_file_t *pf = &ix->lists[TYPE_THEME][0].pages[0];
			char url[300];
			size_t plen;
			snprintf(url, sizeof(url), "%s%s", CATALOG_BASE, pf->url);
			char *page = http_fetch(NULL, url, NULL, NULL, 60, NULL, &plen, &res);
			mbedtls_sha256((const unsigned char *)(page ? page : ""), page ? plen : 0, d, 0);
			catalog_t *c = page ? catalog_parse_page(page, plen, err, sizeof(err)) : NULL;
			Result(c && !memcmp(d, pf->sha256, 32), "live theme page 1", "%s: %lu bytes, sha256 %s the index, %d items, first \"%s\" %s",
				pf->url, (unsigned long)plen, memcmp(d, pf->sha256, 32) ? "DIFFERS from" : "matches", c ? c->count : 0,
				c && c->count ? c->items[0].title : "", err);
			catalog_free(c);
			free(page);
		} else
			Result(0, "live index", "%s", ix ? "no theme list" : err);
		catalog_free_index(ix);
	} else {
		printf("SKIP live index: HTTP %d %s (not uploaded yet?)\n", res.status, res.err);
	}
	free(body);
	if (argc > 1) {
		// argv[1]: the sample's root (tests/sample), its index at catalog/v1/catalog.json
		char path[600];
		size_t n;
		snprintf(path, sizeof(path), "%s/catalog/v1/catalog.json", argv[1]);
		char *buf = (char *)plat_read_file(path, &n);
		cat_index_t *ix = buf ? catalog_parse_index(buf, n, err, sizeof(err)) : NULL;
		int pages = 0, items = 0, want = 0, lists = 0, bad = 0;	// (want: the lists' counts; "All" repeats the rest)
		for (int t = 0; ix && t < NUM_TYPES; t++)
			for (int l = 0; l < ix->num_lists[t]; l++) {
				lists++;
				want += ix->lists[t][l].count;
				for (int p = 0; p < ix->lists[t][l].num_pages; p++) {
					const cat_file_t *pf = &ix->lists[t][l].pages[p];
					size_t plen;
					snprintf(path, sizeof(path), "%s/%s", argv[1], pf->url);
					char *page = (char *)plat_read_file(path, &plen);
					mbedtls_sha256((const unsigned char *)(page ? page : ""), page ? plen : 0, d, 0);
					catalog_t *c = page && !memcmp(d, pf->sha256, 32) ? catalog_parse_page(page, plen, err, sizeof(err)) : NULL;
					if (c) { pages++; items += c->count; } else bad++;
					catalog_free(c);
					free(page);
				}
			}
		Result(ix && !bad && items == want, "sample catalogue parsed",
			"%s: %d lists, %d pages, %d items (the lists say %d; %d homebrew, %d themes, %d avatars), %d bad %s", argv[1],
			lists, pages, items, want, ix ? ix->count[0] : 0, ix ? ix->count[1] : 0, ix ? ix->count[2] : 0, bad, err);
		catalog_free_index(ix);
		free(buf);
	}

	Download_Tests();

	printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all passed", failures, failures == 1 ? "" : "s");
	if (getenv("GITHUB_ACTIONS"))
		printf("::notice title=HTTPS and download tests: %s::%s\n", failures ? "FAILED" : "all passed", summary);
	return failures ? 1 : 0;
}
