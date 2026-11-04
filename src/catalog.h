/*
catalog.h: the catalogue (schema 3), an index and pages.

catalog.json is the index: for each section (homebrew, theme, avatar) its
lists (homebrew: one per site category; themes and avatars: all, then one per
category or region), and for each list its pages of up to page_size items, a
search.txt and where each letter starts. Pages are fetched as the list reaches
them. An avatar list's items are packs: folders, each with a list of its own.
It may also name the newest PS3 World PKG ("app": version, url, size, sha256).
*/
#ifndef CATALOG_H
#define CATALOG_H

#include <stddef.h>

#define CATALOG_URL		"https://dl.ps3.world/catalog/v1/catalog.json"
#define CATALOG_BASE	"https://dl.ps3.world/"		// every page, download and icon is under this
#define CATALOG_SCHEMA	3		// 3: avatar packs as folders (an older app would download a pack's page as an avatar)

enum { TYPE_HOMEBREW, TYPE_THEME, TYPE_AVATAR, NUM_TYPES };
enum { REQ_ANY, REQ_HEN, REQ_CFW };

// a file the index lists, with the sha256 it must have
typedef struct {
	char url[160];				// relative to CATALOG_BASE
	unsigned char sha256[32];
} cat_file_t;

#define NUM_LETTERS 27			// '#', then A..Z
enum { SORT_AZ, SORT_NEWEST, SORT_LARGEST, NUM_SORTS };
typedef struct cat_list_s {
	char name[32];				// "Games", "Ports"...; "All themes", "Anime & Comics"...; "All avatars", "US"...
	int count;
	int num_pages;
	cat_file_t *pages;
	cat_file_t search;
	int letter_first[NUM_LETTERS];	// where each letter starts in the list, -1 if none
	struct cat_list_s *sorted;		// the same items newest first, then largest first (NULL: none; 1.2.0 on)
	int num_sorted;
} cat_list_t;

typedef struct {
	char version[16];			// "" when the catalogue offers none
	cat_file_t file;			// the PKG
	long long size;
} cat_app_t;

#define MAX_DESC_LANGS	16
typedef struct {
	char code[8];				// "es"...
	cat_file_t file;			// id: description, for homebrew
} cat_desc_t;

typedef struct {
	char generated[32];
	cat_app_t app;				// the newest PS3 World (the app checks it against its own version)
	cat_desc_t desc[MAX_DESC_LANGS];	// homebrew's descriptions in other languages (1.2.0 on)
	int num_desc;
	int page_size;
	int count[NUM_TYPES];
	int num_lists[NUM_TYPES];
	cat_list_t *lists[NUM_TYPES];
} cat_index_t;

typedef struct {
	// all in the page's string block; "" when the catalogue has none
	const char *id, *title, *author, *version, *description, *group;
	const char *url;			// relative to CATALOG_BASE
	const char *icon;			// relative to CATALOG_BASE, "" for none
	const char *preview;		// a bigger picture (a theme's 480x270, an avatar's 240x240), "" for none
	const char *list;			// a folder (an avatar pack): its own list, as the index's JSON; "" for a file
	long long size;				// (a folder: all its files)
	unsigned char sha256[32];
	unsigned char type, requires;
} cat_item_t;

// one page of a list
typedef struct {
	int count;
	cat_item_t *items;
	char *strings;
} catalog_t;

// NULL on failure, with the reason in err; items that don't check out are skipped (and logged)
cat_index_t *catalog_parse_index(const char *json, size_t len, char *err, int errsize);
void catalog_free_index(cat_index_t *ix);
catalog_t *catalog_parse_page(const char *json, size_t len, char *err, int errsize);
void catalog_free(catalog_t *c);
// a folder's list (its item's "list"); 0 if it doesn't check out. catalog_free_list frees and zeroes it
int catalog_parse_list(const char *json, cat_list_t *l);
void catalog_free_list(cat_list_t *l);

const char *catalog_type_name(int type);	// "homebrew", "theme", "avatar"
int catalog_letter_slot(char letter);		// '#' 0, 'A'..'Z' 1..26
// a copy of an item that owns its strings (for the detail screen, after its page has gone)
cat_item_t *catalog_item_copy(const cat_item_t *it);

#endif
