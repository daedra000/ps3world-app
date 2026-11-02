/*
catalog.c: catalog.json into a catalog_t.

The whole file is parsed with cJSON (~6 MB for the full archive), copied into
compact structs and one block of strings, and the cJSON tree freed. Every item
is checked: an id, a title, a size, a sha256 of 64 hex digits and a relative
URL that stays under dl.ps3.world. One that fails is skipped, not trusted.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "catalog.h"
#include "thirdparty/cJSON.h"

static const char *type_names[NUM_TYPES] = {"homebrew", "theme", "avatar"};

const char *catalog_type_name(int type)
{
	return type >= 0 && type < NUM_TYPES ? type_names[type] : "?";
}

static const char *Str(const cJSON *o, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

// a path under the base: no scheme, no leading slash, no ".." segment (plain or
// percent-encoded), no backslashes or control characters ("Wow...p3t" is fine)
static int Safe_Path(const char *p)
{
	if (!*p || *p == '/' || strstr(p, "://"))
		return 0;
	for (const char *seg = p; *seg; ) {
		const char *end = strchr(seg, '/');
		size_t l = end ? (size_t)(end - seg) : strlen(seg);
		char low[8] = {0};
		for (size_t i = 0; i < l && i < sizeof(low) - 1; i++)
			low[i] = (seg[i] >= 'A' && seg[i] <= 'Z') ? seg[i] + 32 : seg[i];
		if ((l == 2 && !strcmp(low, "..")) || (l == 4 && (!strcmp(low, ".%2e") || !strcmp(low, "%2e."))) ||
			(l == 6 && !strcmp(low, "%2e%2e")))
			return 0;
		seg += l + (end ? 1 : 0);
	}
	for (; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == '\\' || *p == 0x7f)
			return 0;
	return 1;
}

static int Hex_Digest(const char *s, unsigned char out[32])
{
	if (strlen(s) != 64)
		return 0;
	for (int i = 0; i < 32; i++) {
		unsigned v = 0;
		for (int k = 0; k < 2; k++) {
			char c = s[i * 2 + k];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return 0;
		}
		out[i] = (unsigned char)v;
	}
	return 1;
}

static const char *const fields[] = {"id", "title", "author", "version", "description", "group", "url", "icon"};
#define NUM_FIELDS (int)(sizeof(fields) / sizeof(fields[0]))

// checks one item; fills what doesn't need the string block
static int Check(const cJSON *o, cat_item_t *it)
{
	const char *type = Str(o, "type"), *req = Str(o, "requires");
	const cJSON *size = cJSON_GetObjectItemCaseSensitive(o, "size");
	int t;

	for (t = 0; t < NUM_TYPES; t++)
		if (!strcmp(type, type_names[t])) break;
	if (t == NUM_TYPES || !*Str(o, "id") || !*Str(o, "title") || !cJSON_IsNumber(size) || size->valuedouble < 0)
		return 0;
	if (!Safe_Path(Str(o, "url")) || (*Str(o, "icon") && !Safe_Path(Str(o, "icon"))))
		return 0;
	if (!Hex_Digest(Str(o, "sha256"), it->sha256))
		return 0;
	it->type = (unsigned char)t;
	it->size = (long long)size->valuedouble;
	it->requires = !strcmp(req, "cfw") ? REQ_CFW : !strcmp(req, "hen") ? REQ_HEN : REQ_ANY;
	return 1;
}

catalog_t *catalog_parse(const char *json, size_t len, char *err, int errsize)
{
	cJSON *root = cJSON_ParseWithLength(json, len);
	catalog_t *c = NULL;
	const cJSON *items, *o;
	size_t total = 0;
	int n = 0, skipped = 0;

	err[0] = 0;
	if (!root) {
		snprintf(err, errsize, "The catalogue couldn't be read (not valid JSON)");
		return NULL;
	}
	const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
	items = cJSON_GetObjectItemCaseSensitive(root, "items");
	if (!cJSON_IsNumber(schema) || !cJSON_IsArray(items)) {
		snprintf(err, errsize, "The catalogue couldn't be read (no schema or items)");
		goto out;
	}
	if (schema->valueint != CATALOG_SCHEMA) {
		snprintf(err, errsize, "This catalogue needs a newer version of the app (schema %d)", schema->valueint);
		goto out;
	}
	if (strcmp(Str(root, "base_url"), CATALOG_BASE)) {
		snprintf(err, errsize, "The catalogue points somewhere other than %s", CATALOG_BASE);
		goto out;
	}

	// first pass: what passes, and how much string space it needs
	cJSON_ArrayForEach(o, items) {
		cat_item_t tmp;
		if (!Check(o, &tmp)) { skipped++; continue; }
		for (int f = 0; f < NUM_FIELDS; f++)
			total += strlen(Str(o, fields[f])) + 1;
		n++;
	}
	c = calloc(1, sizeof(*c));
	if (!c || !(c->items = calloc(n ? n : 1, sizeof(cat_item_t))) || !(c->strings = malloc(total ? total : 1))) {
		snprintf(err, errsize, "Out of memory for the catalogue (%d items)", n);
		catalog_free(c);
		c = NULL;
		goto out;
	}
	snprintf(c->generated, sizeof(c->generated), "%s", Str(root, "generated"));

	// second pass: copy
	char *s = c->strings;
	cJSON_ArrayForEach(o, items) {
		cat_item_t *it = &c->items[c->count];
		if (c->count >= n || !Check(o, it)) continue;
		const char **dst[NUM_FIELDS] = {&it->id, &it->title, &it->author, &it->version,
			&it->description, &it->group, &it->url, &it->icon};
		for (int f = 0; f < NUM_FIELDS; f++) {
			size_t l = strlen(Str(o, fields[f])) + 1;
			memcpy(s, Str(o, fields[f]), l);
			*dst[f] = s;
			s += l;
		}
		c->per_type[it->type]++;
		c->count++;
	}
	if (skipped)
		plat_log("catalog: %d item(s) skipped (missing or bad fields)", skipped);

out:
	cJSON_Delete(root);
	return c;
}

void catalog_free(catalog_t *c)
{
	if (!c) return;
	free(c->items);
	free(c->strings);
	free(c);
}
