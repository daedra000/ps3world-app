/*
download.c: see download.h.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/sha256.h"
#include "plat.h"
#include "download.h"

#define FREE_MARGIN		(1024 * 1024)	// left free on the drive beyond the file itself
#define READ_CHUNK		65536

typedef struct {
	download_t *d;
	FILE *f;
	mbedtls_sha256_context sha;
	http_res_t *res;
	const char *part;
	long long want_from;		// the Range we asked for (0: the whole file)
	int bad_range;				// a 206 that isn't the rest of this file
	int started;				// the first piece of the body has been seen
	int write_failed;
} ctx_t;

int download_error_kind(const char *e, int status)
{
	if (status == 404) return ERR_NOT_FOUND;
	if (status && (status < 200 || status >= 300)) return ERR_HTTP;
	// http_ps3.c's messages
	if (strstr(e, "certificate") || strstr(e, "Secure connection")) return ERR_TLS;
	if (strstr(e, "network isn't available") || strstr(e, "Can't find") || strstr(e, "Can't connect") ||
		strstr(e, "No reply") || strstr(e, "cut short") || strstr(e, "Request to"))
		return ERR_NETWORK;
	return ERR_HTTP;
}

static void Fail(download_t *d, int kind, const char *fmt, const char *arg)
{
	d->err_kind = kind;
	snprintf(d->err, sizeof(d->err), fmt, arg ? arg : "");
	d->state = DL_FAILED;
}

static void Hex(const unsigned char *in, char *out)
{
	for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", in[i]);
}

static void Remove_Partial(const char *part, const char *side)
{
	remove(part);
	remove(side);
}

// the body, a piece at a time: to disk and into the hash, nothing kept in memory
static int Sink(void *p, const void *data, int len)
{
	ctx_t *c = p;
	download_t *d = c->d;
	if (d->cancel || d->pause)
		return 0;
	if (!c->started) {
		c->started = 1;
		if (c->res->status != 200 && c->res->status != 206)
			return 0;		// an error page: not part of the file
		if (c->res->status == 206 && c->res->length >= 0 && c->res->length != d->size - c->want_from) {
			c->bad_range = 1;
			return 0;
		}
		if (c->want_from && c->res->status == 200) {
			// the server sent the whole file instead of the rest: start again from 0
			plat_log("download: Range ignored (200): starting %s again", d->dest);
			fclose(c->f);
			c->f = fopen(c->part, "wb");
			mbedtls_sha256_free(&c->sha);
			mbedtls_sha256_init(&c->sha);
			mbedtls_sha256_starts(&c->sha, 0);
			d->got = 0;
			d->resumed_from = 0;
			if (!c->f) { c->write_failed = 1; return 0; }
		}
	}
	if (d->got + len > d->size)
		return 0;		// more than the catalogue says: it isn't this file
	if (fwrite(data, 1, len, c->f) != (size_t)len) {
		c->write_failed = 1;
		return 0;
	}
	mbedtls_sha256_update(&c->sha, data, len);
	d->got += len;
	return 1;
}

// hashes what's in the partial file already; 0 if it couldn't be read
static int Hash_Existing(const char *part, long long n, mbedtls_sha256_context *sha, download_t *d)
{
	FILE *f = fopen(part, "rb");
	unsigned char *buf = malloc(READ_CHUNK);
	long long done = 0;
	if (!f || !buf) { if (f) fclose(f); free(buf); return 0; }
	while (done < n && !d->cancel && !d->pause) {
		size_t want = n - done > READ_CHUNK ? READ_CHUNK : (size_t)(n - done);
		size_t r = fread(buf, 1, want, f);
		if (r == 0) break;
		mbedtls_sha256_update(sha, buf, r);
		done += r;
		d->got = done;
	}
	fclose(f);
	free(buf);
	return done == n;
}

void download_run(download_t *d)
{
	char part[540], side[560], want_hex[65], got_hex[65], range[64], dir[512];
	unsigned char digest[32];
	http_res_t res;
	http_req_t req;
	ctx_t c;

	memset(&c, 0, sizeof(c));
	c.d = d;
	c.res = &res;
	c.part = part;
	d->err_kind = ERR_NONE;
	d->err[0] = 0;
	d->got = 0;
	d->resumed_from = 0;
	snprintf(part, sizeof(part), "%s.part", d->dest);
	snprintf(side, sizeof(side), "%s.part.sha", d->dest);
	Hex(d->sha256, want_hex);
	snprintf(dir, sizeof(dir), "%s", d->dest);
	char *slash = strrchr(dir, '/');
	if (slash) *slash = 0;

	// what's there from last time, if it's this same file
	d->state = DL_CHECKING;
	long long have = plat_file_size(part);
	if (have > 0) {
		char *s = (char *)plat_read_file(side, NULL);
		int same = s && !strncmp(s, want_hex, 64);
		free(s);
		if (!same || have > d->size) {
			plat_log("download: %s.part is from another version: starting again", d->dest);
			Remove_Partial(part, side);
			have = 0;
		}
	} else
		have = 0;

	// room for the rest (and a little more)
	long long free_b = plat_free_bytes(dir);
	if (free_b >= 0 && free_b < d->size - have + FREE_MARGIN) {
		char need[96];
		snprintf(need, sizeof(need), "%.1f MB needed, %.1f MB free", (d->size - have) / 1048576.0, free_b / 1048576.0);
		Fail(d, ERR_DISK_FULL, "Not enough free space (%s)", need);
		return;
	}

	mbedtls_sha256_init(&c.sha);
	mbedtls_sha256_starts(&c.sha, 0);
	if (have && !Hash_Existing(part, have, &c.sha, d)) {
		if (d->cancel || d->pause) goto stopped;
		plat_log("download: %s.part couldn't be read: starting again", d->dest);
		mbedtls_sha256_free(&c.sha);
		mbedtls_sha256_init(&c.sha);
		mbedtls_sha256_starts(&c.sha, 0);
		Remove_Partial(part, side);
		have = 0;
	}
	d->got = have;
	d->resumed_from = have;

	if (have < d->size) {
		if (!plat_write_file(side, want_hex, 64) || !(c.f = fopen(part, have ? "ab" : "wb"))) {
			Fail(d, ERR_WRITE, "Couldn't write to %s", dir);
			goto out;
		}
		memset(&req, 0, sizeof(req));
		req.url = d->url;
		req.cancel = &d->cancel;		// (pause stops it through the sink)
		req.timeout_s = 30;
		req.sink = Sink;
		req.ctx = &c;
		if (have) {
			snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n", have);
			req.headers = range;
			c.want_from = have;
			plat_log("download: resuming %s at %lld of %lld", d->dest, have, d->size);
		}
		d->state = DL_RUNNING;
		int ok = http_request(&req, &res);
		if (c.f && fclose(c.f) != 0) c.write_failed = 1;
		c.f = NULL;

		if (d->cancel || d->pause) goto stopped;
		if (c.write_failed) {
			long long fb = plat_free_bytes(dir);
			if (fb >= 0 && fb < READ_CHUNK) Fail(d, ERR_DISK_FULL, "The drive is full%s", "");
			else Fail(d, ERR_WRITE, "Couldn't write to %s", dir);
			goto out;
		}
		if (c.bad_range) {
			Remove_Partial(part, side);
			Fail(d, ERR_HTTP, "The server sent the wrong part of the file. Try again%s", "");
			goto out;
		}
		if (res.status == 404) { Remove_Partial(part, side); Fail(d, ERR_NOT_FOUND, "Not found on the server (404)%s", ""); goto out; }
		if (res.status == 416) {
			// the partial file is longer than the server's: not this file
			Remove_Partial(part, side);
			Fail(d, ERR_HTTP, "The server's copy is different (416). Try again%s", "");
			goto out;
		}
		if (!ok || (res.status != 200 && res.status != 206)) {
			char e[180];
			if (res.status && res.status != 200 && res.status != 206) snprintf(e, sizeof(e), "HTTP %d", res.status);
			else snprintf(e, sizeof(e), "%s", res.err[0] ? res.err : "Download cut short");
			Fail(d, download_error_kind(res.err, res.status), "%s", e);
			goto out;		// (the partial file stays: the next try resumes)
		}
		if (d->got != d->size) {
			Fail(d, ERR_NETWORK, "Download cut short%s", "");
			goto out;
		}
	}

	// the whole file: is it the one the catalogue means?
	d->state = DL_VERIFYING;
	mbedtls_sha256_finish(&c.sha, digest);
	Hex(digest, got_hex);
	long long on_disk = plat_file_size(part);
	if (on_disk != d->size || memcmp(digest, d->sha256, 32)) {
		plat_log("download: %s: checksum mismatch (%lld bytes, sha256 %s, wanted %s)", d->dest, on_disk, got_hex, want_hex);
		Remove_Partial(part, side);
		Fail(d, ERR_CHECKSUM, "Checksum mismatch: the file was deleted. Try again%s", "");
		goto out;
	}
	remove(d->dest);
	if (rename(part, d->dest) != 0) {
		Fail(d, ERR_WRITE, "Couldn't rename the finished file in %s", dir);
		goto out;
	}
	remove(side);
	plat_log("download: %s done, %lld bytes, sha256 %s", d->dest, d->size, got_hex);
	d->state = DL_DONE;
	goto out;

stopped:
	if (c.f) { fclose(c.f); c.f = NULL; }
	if (d->cancel) {
		Remove_Partial(part, side);
		d->state = DL_CANCELLED;
	} else
		d->state = DL_PAUSED;		// the partial file stays for next time
out:
	if (c.f) fclose(c.f);
	mbedtls_sha256_free(&c.sha);
}
