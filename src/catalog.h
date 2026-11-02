/*
catalog.h: catalog.json (schema 1), parsed into one array and one block of strings.
*/
#ifndef CATALOG_H
#define CATALOG_H

#include <stddef.h>

#define CATALOG_URL		"https://dl.ps3.world/catalog/v1/catalog.json"
#define CATALOG_BASE	"https://dl.ps3.world/"		// every download and icon is under this
#define CATALOG_SCHEMA	1

enum { TYPE_HOMEBREW, TYPE_THEME, TYPE_AVATAR, NUM_TYPES };
enum { REQ_ANY, REQ_HEN, REQ_CFW };

typedef struct {
	// all in the catalogue's string block; "" when the catalogue has none
	const char *id, *title, *author, *version, *description, *group;
	const char *url;			// relative to CATALOG_BASE
	const char *icon;			// relative to CATALOG_BASE, "" for none
	long long size;
	unsigned char sha256[32];
	unsigned char type, requires;
} cat_item_t;

typedef struct {
	char generated[32];
	int count;
	int per_type[NUM_TYPES];
	cat_item_t *items;
	char *strings;
} catalog_t;

// NULL on failure, with the reason in err; items that don't check out are skipped (and logged)
catalog_t *catalog_parse(const char *json, size_t len, char *err, int errsize);
void catalog_free(catalog_t *c);
const char *catalog_type_name(int type);	// "homebrew", "theme", "avatar"

#endif
