/*
store.c: see store.h.

Install targets (v1):
  homebrew  /dev_hdd0/packages/<file>.pkg, for Package Manager > Install Package Files
  theme     /dev_hdd0/theme/<file>.p3t, where the XMB lists themes (Settings > Theme
            Settings > Theme), as themes.ps3.world's guide
  avatar    /dev_hdd0/home/<profile>/psn_avatar/<file>.edat, as avatars.ps3.world's guide
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mbedtls/sha256.h"
#include "plat.h"
#include "download.h"
#include "store.h"
#include "lang.h"

#define MAX_RECORDS	4096
#define HOME_DIR	"/dev_hdd0/home"

typedef struct {
	char id[64];
	char sha[65];
	char path[300];
	char title[128];		// (lines from before 0.6.0 have none)
	long long when;			// time() it finished, 0 if not known
	long long size;			// on disk, read at start
} record_t;

static record_t *records;
static int num_records;
static char avatar_profile[64];
static char cache_dir[512], icons_dir[512], previews_dir[512], files_dir[512];

static void Hex(const unsigned char *in, char *out)
{
	for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", in[i]);
}

/*
==================
Settings and records
==================
*/
static void Settings_Load(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/settings.txt", plat_save_dir());
	char *s = (char *)plat_read_file(path, NULL);
	if (!s) return;
	const char *v = strstr(s, "avatar_profile=");
	if (v) {
		v += 15;
		int n = 0;
		while (v[n] && v[n] != '\n' && v[n] != '\r' && n < (int)sizeof(avatar_profile) - 1) n++;
		snprintf(avatar_profile, sizeof(avatar_profile), "%.*s", n, v);
	}
	free(s);
}

static void Settings_Save(void)
{
	char path[512], buf[128];
	snprintf(path, sizeof(path), "%s/settings.txt", plat_save_dir());
	int n = snprintf(buf, sizeof(buf), "avatar_profile=%s\n", avatar_profile);
	plat_write_file(path, buf, n);
}

static void Records_Load(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/downloaded.txt", plat_save_dir());
	char *s = (char *)plat_read_file(path, NULL);
	if (!s) return;
	for (char *line = s, *next; line && *line && num_records < MAX_RECORDS; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		char *cr = strchr(line, '\r');
		if (cr) *cr = 0;
		// id, sha256, path, then (0.6.0 on) when and the title
		char *f[5] = {line};
		int nf = 1;
		for (char *t = line; nf < 5 && (t = strchr(t, '\t')); nf++) { *t++ = 0; f[nf] = t; }
		if (nf < 3) continue;
		record_t *r = &records[num_records];
		memset(r, 0, sizeof(*r));
		snprintf(r->id, sizeof(r->id), "%s", f[0]);
		snprintf(r->sha, sizeof(r->sha), "%s", f[1]);
		snprintf(r->path, sizeof(r->path), "%s", f[2]);
		if (nf > 3) r->when = atoll(f[3]);
		if (nf > 4) snprintf(r->title, sizeof(r->title), "%s", f[4]);
		if ((r->size = plat_file_size(r->path)) >= 0)		// only what's still there
			num_records++;
	}
	free(s);
	plat_log("store: %d downloads recorded", num_records);
}

static void Records_Save(void)
{
	char path[512];
	size_t cap = (size_t)num_records * (sizeof(record_t) + 32) + 1, n = 0;		// a line: the fields, a number and tabs
	char *buf = malloc(cap);
	if (!buf) return;
	for (int i = 0; i < num_records; i++) {
		int r = snprintf(buf + n, cap - n, "%s\t%s\t%s\t%lld\t%s\n", records[i].id, records[i].sha, records[i].path,
			records[i].when, records[i].title);
		if (r < 0 || (size_t)r >= cap - n) break;
		n += r;
	}
	snprintf(path, sizeof(path), "%s/downloaded.txt", plat_save_dir());
	if (!plat_write_file(path, buf, n))
		plat_log("store: couldn't write %s", path);
	free(buf);
}

void store_init(void)
{
	records = calloc(MAX_RECORDS, sizeof(record_t));
	snprintf(cache_dir, sizeof(cache_dir), "%s/cache", plat_save_dir());
	snprintf(icons_dir, sizeof(icons_dir), "%s/icons", cache_dir);
	snprintf(previews_dir, sizeof(previews_dir), "%s/previews", cache_dir);
	snprintf(files_dir, sizeof(files_dir), "%s/files", cache_dir);
	plat_mkdirs(icons_dir);
	plat_mkdirs(previews_dir);
	plat_mkdirs(files_dir);
	Settings_Load();
	if (records) Records_Load();
}

const char *store_downloaded(const cat_item_t *it)
{
	char sha[65];
	Hex(it->sha256, sha);
	for (int i = num_records - 1; i >= 0; i--)
		if (!strcmp(records[i].id, it->id))
			return strcmp(records[i].sha, sha) ? NULL : records[i].path;		// an older version isn't this one
	return NULL;
}

void store_mark_downloaded(const cat_item_t *it, const char *path)
{
	int i;
	if (!records) return;
	for (i = 0; i < num_records; i++)
		if (!strcmp(records[i].id, it->id)) break;
	if (i == num_records) {
		if (num_records == MAX_RECORDS) return;
		num_records++;
	}
	record_t *r = &records[i];
	snprintf(r->id, sizeof(r->id), "%s", it->id);
	Hex(it->sha256, r->sha);
	snprintf(r->path, sizeof(r->path), "%s", path);
	snprintf(r->title, sizeof(r->title), "%s", it->title);
	for (char *c = r->title; *c; c++)		// (a field of a tab-separated line)
		if (*c == '\t' || *c == '\n' || *c == '\r') *c = ' ';
	r->when = (long long)time(NULL);
	r->size = it->size;
	Records_Save();
}

int store_num_downloads(void) { return num_records; }

// newest first
int store_download_at(int i, store_rec_t *out)
{
	if (i < 0 || i >= num_records) return 0;
	const record_t *r = &records[num_records - 1 - i];
	out->id = r->id;
	out->sha = r->sha;
	out->path = r->path;
	out->title = r->title;
	out->when = r->when;
	out->size = r->size;
	return 1;
}

int store_delete_download(int i)
{
	if (i < 0 || i >= num_records) return 0;
	int k = num_records - 1 - i;
	if (remove(records[k].path) != 0 && plat_file_size(records[k].path) >= 0) {
		plat_log("store: couldn't delete %s", records[k].path);
		return 0;
	}
	plat_log("store: deleted %s", records[k].path);
	memmove(&records[k], &records[k + 1], (num_records - 1 - k) * sizeof(record_t));
	num_records--;
	Records_Save();
	return 1;
}

/*
==================
Cache
==================
*/
cat_index_t *store_load_index(void)
{
	char path[600], err[160];
	size_t len;
	snprintf(path, sizeof(path), "%s/catalog.json", cache_dir);
	char *s = (char *)plat_read_file(path, &len);
	if (!s) return NULL;
	cat_index_t *ix = catalog_parse_index(s, len, err, sizeof(err));
	free(s);
	if (!ix) plat_log("store: cached catalogue unusable: %s", err);
	return ix;
}

void store_save_index(const char *json, size_t len)
{
	char path[600];
	snprintf(path, sizeof(path), "%s/catalog.json", cache_dir);
	if (!plat_write_file(path, json, len))
		plat_log("store: couldn't cache the catalogue in %s", path);
}

char *store_get_file(const cat_file_t *f, size_t *len, volatile int *cancel, int *err_kind, char *err, int errsize)
{
	char path[600], hex[65], url[300];
	unsigned char got[32];
	Hex(f->sha256, hex);
	snprintf(path, sizeof(path), "%s/%s", files_dir, hex);
	*err_kind = ERR_NONE;
	err[0] = 0;

	char *s = (char *)plat_read_file(path, len);
	if (s) {
		mbedtls_sha256((const unsigned char *)s, *len, got, 0);
		if (!memcmp(got, f->sha256, 32))
			return s;
		free(s);		// damaged: fetch it again
	}
	http_res_t res;
	snprintf(url, sizeof(url), "%s%s", CATALOG_BASE, f->url);
	s = http_fetch(NULL, url, NULL, NULL, 30, cancel, len, &res);
	if (!s || res.status != 200) {
		*err_kind = download_error_kind(res.err, s ? res.status : 0);
		snprintf(err, errsize, "%s", s ? "HTTP error" : res.err);
		if (s) snprintf(err, errsize, "HTTP %d", res.status);
		free(s);
		return NULL;
	}
	mbedtls_sha256((const unsigned char *)s, *len, got, 0);
	if (memcmp(got, f->sha256, 32)) {
		// the index and the page are from different uploads (or the upload is half done)
		*err_kind = ERR_CHECKSUM;
		snprintf(err, errsize, tr("%s doesn't match the catalogue (it may be being updated): try again later"), f->url);
		free(s);
		return NULL;
	}
	if (!plat_write_file(path, s, *len))
		plat_log("store: couldn't cache %s", path);
	return s;
}

// a cached picture is named by its item and where it came from: when the catalogue
// points it somewhere new (a sharper one), it's fetched again
static unsigned Url_Hash(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

void store_icon_path(const cat_item_t *it, char *out, int size)
{
	snprintf(out, size, "%s/%s-%08x.jpg", icons_dir, it->id, Url_Hash(it->icon));
}

void store_preview_path(const cat_item_t *it, char *out, int size)
{
	snprintf(out, size, "%s/%s-%08x", previews_dir, it->id, Url_Hash(it->preview));
}

/*
==================
Install targets
==================
*/

// the file name from the URL's last part: %XX decoded, and nothing FAT32 refuses
static void File_Name(const char *url, char *out, int size)
{
	const char *b = strrchr(url, '/');
	b = b ? b + 1 : url;
	int o = 0;
	for (; *b && o < size - 1; b++) {
		char ch = *b;
		if (ch == '%' && b[1] && b[2]) {
			char hex[3] = {b[1], b[2], 0};
			ch = (char)strtol(hex, NULL, 16);
			b += 2;
		}
		if ((unsigned char)ch < 0x20 || strchr("\\/:*?\"<>|", ch)) ch = '_';
		out[o++] = ch;
	}
	out[o] = 0;
	if (!o) snprintf(out, size, "download");
}

int store_profiles(char (*names)[64], int max)
{
	char all[16][64];
	int n = plat_list_dirs(HOME_DIR, all, 16), k = 0;
	for (int i = 0; i < n && k < max; i++)
		if (strlen(all[i]) == 8 && strspn(all[i], "0123456789") == 8)		// 00000001 ...
			snprintf(names[k++], 64, "%s", all[i]);
	return k;
}

const char *store_avatar_profile(void)
{
	if (avatar_profile[0]) {
		char p[128];
		snprintf(p, sizeof(p), HOME_DIR "/%s", avatar_profile);
		if (plat_dir_exists(p)) return avatar_profile;
	}
	char names[16][64];
	if (store_profiles(names, 16) != 1)
		return "";		// none, or more than one: the user picks
	snprintf(avatar_profile, sizeof(avatar_profile), "%s", names[0]);
	return avatar_profile;
}

void store_set_avatar_profile(const char *name)
{
	snprintf(avatar_profile, sizeof(avatar_profile), "%s", name);
	Settings_Save();
}

int store_dest(const cat_item_t *it, char *out, int size)
{
	char name[256];
	File_Name(it->url, name, sizeof(name));
	switch (it->type) {
	case TYPE_HOMEBREW:
		snprintf(out, size, "/dev_hdd0/packages/%s", name);
		return DEST_OK;
	case TYPE_THEME:
		snprintf(out, size, "/dev_hdd0/theme/%s", name);
		return DEST_OK;
	case TYPE_AVATAR: {
		const char *p = store_avatar_profile();
		if (!p[0]) {
			char names[16][64];
			return store_profiles(names, 16) ? DEST_PICK_PROFILE : DEST_NO_PROFILE;
		}
		snprintf(out, size, HOME_DIR "/%s/psn_avatar/%s", p, name);
		return DEST_OK;
	}
	}
	return DEST_NO_PROFILE;
}

const char *store_dest_message(int code)
{
	switch (code) {
	case DEST_NO_PROFILE:
		return "No user profiles were found in /dev_hdd0/home.";
	case DEST_PICK_PROFILE:
		return "Choose which user the avatar is for.";
	}
	return "";
}
