/*
catalog.c: the index and pages of catalog.json (see catalog.h).

Both are parsed with cJSON (the index is ~30 KB, a page under 100 KB), copied
into compact structs, and the cJSON tree freed. Every item is checked: an id,
a title, a size, a sha256 of 64 hex digits and a relative URL that stays under
dl.ps3.world. One that fails is skipped, not trusted.
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

int catalog_letter_slot(char l)
{
	return l >= 'A' && l <= 'Z' ? l - 'A' + 1 : 0;
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

static int Schema_Ok(const cJSON *root, char *err, int errsize)
{
	const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
	if (!cJSON_IsNumber(schema)) {
		snprintf(err, errsize, "The catalogue couldn't be read (no schema)");
		return 0;
	}
	if (schema->valueint != CATALOG_SCHEMA) {
		snprintf(err, errsize, "This catalogue needs a %s version of the app (schema %d)",
			schema->valueint > CATALOG_SCHEMA ? "newer" : "different", schema->valueint);
		return 0;
	}
	return 1;
}

/*
==================
Index
==================
*/
static int File_Of(const cJSON *o, cat_file_t *f)
{
	const char *url = Str(o, "url");
	if (!Safe_Path(url) || strlen(url) >= sizeof(f->url) || !Hex_Digest(Str(o, "sha256"), f->sha256))
		return 0;
	snprintf(f->url, sizeof(f->url), "%s", url);
	return 1;
}

static int List_Of(const cJSON *o, cat_list_t *l)
{
	const cJSON *count = cJSON_GetObjectItemCaseSensitive(o, "count");
	const cJSON *pages = cJSON_GetObjectItemCaseSensitive(o, "pages");
	const cJSON *letters = cJSON_GetObjectItemCaseSensitive(o, "letters");
	const cJSON *p;
	if (!cJSON_IsNumber(count) || count->valueint < 0 || !cJSON_IsArray(pages))
		return 0;
	snprintf(l->name, sizeof(l->name), "%s", Str(o, "name"));
	l->count = count->valueint;
	l->num_pages = cJSON_GetArraySize(pages);
	l->pages = calloc(l->num_pages ? l->num_pages : 1, sizeof(cat_file_t));
	if (!l->pages) return 0;
	int i = 0;
	cJSON_ArrayForEach(p, pages)
		if (!File_Of(p, &l->pages[i++])) return 0;
	if (!File_Of(cJSON_GetObjectItemCaseSensitive(o, "search"), &l->search))
		return 0;
	for (i = 0; i < NUM_LETTERS; i++) l->letter_first[i] = -1;
	cJSON_ArrayForEach(p, letters)
		if (p->string && cJSON_IsNumber(p) && p->valueint >= 0 && p->valueint < l->count)
			l->letter_first[catalog_letter_slot(p->string[0])] = p->valueint;
	return 1;
}

cat_index_t *catalog_parse_index(const char *json, size_t len, char *err, int errsize)
{
	cJSON *root = cJSON_ParseWithLength(json, len);
	cat_index_t *ix = NULL;
	const cJSON *sections, *s, *l;

	err[0] = 0;
	if (!root) {
		snprintf(err, errsize, "The catalogue couldn't be read (not valid JSON)");
		return NULL;
	}
	if (!Schema_Ok(root, err, errsize))
		goto out;
	if (strcmp(Str(root, "base_url"), CATALOG_BASE)) {
		snprintf(err, errsize, "The catalogue points somewhere other than %s", CATALOG_BASE);
		goto out;
	}
	const cJSON *ps = cJSON_GetObjectItemCaseSensitive(root, "page_size");
	sections = cJSON_GetObjectItemCaseSensitive(root, "sections");
	if (!cJSON_IsNumber(ps) || ps->valueint <= 0 || !cJSON_IsArray(sections)) {
		snprintf(err, errsize, "The catalogue couldn't be read (no page size or sections)");
		goto out;
	}
	ix = calloc(1, sizeof(*ix));
	if (!ix) { snprintf(err, errsize, "Out of memory"); goto out; }
	snprintf(ix->generated, sizeof(ix->generated), "%s", Str(root, "generated"));
	ix->page_size = ps->valueint;
	cJSON_ArrayForEach(s, sections) {
		int t;
		for (t = 0; t < NUM_TYPES; t++)
			if (!strcmp(Str(s, "type"), type_names[t])) break;
		const cJSON *lists = cJSON_GetObjectItemCaseSensitive(s, "lists");
		if (t == NUM_TYPES || ix->lists[t] || !cJSON_IsArray(lists)) continue;
		int n = cJSON_GetArraySize(lists), k = 0;
		ix->lists[t] = calloc(n ? n : 1, sizeof(cat_list_t));
		if (!ix->lists[t]) continue;
		int sum = 0;
		cJSON_ArrayForEach(l, lists) {
			if (List_Of(l, &ix->lists[t][k])) {
				sum += ix->lists[t][k].count;
				k++;
			} else {
				free(ix->lists[t][k].pages);
				memset(&ix->lists[t][k], 0, sizeof(cat_list_t));
				plat_log("catalog: a %s list was skipped (bad fields)", type_names[t]);
			}
		}
		ix->num_lists[t] = k;
		// the section's own count: an "All" list and its categories hold the same items twice
		const cJSON *count = cJSON_GetObjectItemCaseSensitive(s, "count");
		ix->count[t] = cJSON_IsNumber(count) ? count->valueint : sum;
	}
out:
	cJSON_Delete(root);
	return ix;
}

void catalog_free_index(cat_index_t *ix)
{
	if (!ix) return;
	for (int t = 0; t < NUM_TYPES; t++) {
		for (int i = 0; i < ix->num_lists[t]; i++)
			free(ix->lists[t][i].pages);
		free(ix->lists[t]);
	}
	free(ix);
}

/*
==================
Pages
==================
*/
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

catalog_t *catalog_parse_page(const char *json, size_t len, char *err, int errsize)
{
	cJSON *root = cJSON_ParseWithLength(json, len);
	catalog_t *c = NULL;
	const cJSON *items, *o;
	size_t total = 0;
	int n = 0, skipped = 0;

	err[0] = 0;
	if (!root) {
		snprintf(err, errsize, "A catalogue page couldn't be read (not valid JSON)");
		return NULL;
	}
	if (!Schema_Ok(root, err, errsize))
		goto out;
	items = cJSON_GetObjectItemCaseSensitive(root, "items");
	if (!cJSON_IsArray(items)) {
		snprintf(err, errsize, "A catalogue page couldn't be read (no items)");
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
		snprintf(err, errsize, "Out of memory for a catalogue page (%d items)", n);
		catalog_free(c);
		c = NULL;
		goto out;
	}

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

cat_item_t *catalog_item_copy(const cat_item_t *it)
{
	const char *src[NUM_FIELDS] = {it->id, it->title, it->author, it->version, it->description, it->group, it->url, it->icon};
	size_t total = sizeof(cat_item_t);
	for (int f = 0; f < NUM_FIELDS; f++) total += strlen(src[f]) + 1;
	cat_item_t *c = malloc(total);		// the struct, then its strings, in one block (one free)
	if (!c) return NULL;
	*c = *it;
	char *s = (char *)(c + 1);
	const char **dst[NUM_FIELDS] = {&c->id, &c->title, &c->author, &c->version, &c->description, &c->group, &c->url, &c->icon};
	for (int f = 0; f < NUM_FIELDS; f++) {
		size_t l = strlen(src[f]) + 1;
		memcpy(s, src[f], l);
		*dst[f] = s;
		s += l;
	}
	return c;
}
