/*
store.h: what the app keeps on the PS3, and where downloads go.

  USRDIR/cache/catalog.json     the last index that parsed (the app opens offline)
  USRDIR/cache/files/<sha256>   pages and search.txt files, named by their sha256
                                (a cached one is good exactly when the index's sha256 matches)
  USRDIR/cache/icons/<id>.jpg   icons, fetched once
  USRDIR/downloaded.txt         "id<TAB>sha256<TAB>path" for each finished download
  USRDIR/settings.txt           "avatar_profile=00000001"
*/
#ifndef STORE_H
#define STORE_H

#include "catalog.h"

void store_init(void);

// the cached index (NULL if none, or it no longer parses)
cat_index_t *store_load_index(void);
void store_save_index(const char *json, size_t len);	// only after it parsed

// a page or search.txt: from the cache if its sha256 matches, else fetched, checked and
// cached. The bytes (NUL-terminated, caller frees) or NULL with *err_kind / err set
char *store_get_file(const cat_file_t *f, size_t *len, volatile int *cancel, int *err_kind, char *err, int errsize);

void store_icon_path(const cat_item_t *it, char *out, int size);

// where a finished download of it is, if it's there and still the catalogue's version
const char *store_downloaded(const cat_item_t *it);
void store_mark_downloaded(const cat_item_t *it, const char *path);

// where it goes, or why it can't yet (no profile picked...)
enum { DEST_OK, DEST_NO_PROFILE, DEST_PICK_PROFILE };
int store_dest(const cat_item_t *it, char *out, int size);
const char *store_dest_message(int code);

// avatars go into a profile's psn_avatar folder
int store_profiles(char (*names)[64], int max);		// the folders in /dev_hdd0/home
const char *store_avatar_profile(void);				// "" until picked (or there's only one)
void store_set_avatar_profile(const char *name);

#endif
