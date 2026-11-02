/*
ui.c: the screens.

  home     Homebrew, Themes and Avatars as cards, styled as ps3.world's home page
  picker   a section's categories: Homebrew's (Games, Ports, Tools...) as the site's
           cards; Themes' and Avatars' (All, then each category or region) as a grid
  list     one list: icon/title/author/version/size per row; Triangle searches
           (the system keyboard), L2/R2 jump a letter; L1/R1 change section
  detail   the description, where it'll be saved, Download (X) with progress,
           speed and Cancel (O); the result, or why it failed
  profile  which user an avatar is for, when the PS3 has more than one

Lists are never loaded whole: the index says how many items a list has and
which page file holds each 100, and a page is fetched (or read from the HDD
cache) when the list reaches it, a few kept in memory. Search reads the list's
search.txt (one line an item) instead of every page.

The index on the HDD opens at once; the server's is fetched in the background
and replaces it when it's newer. Cross confirms and Circle goes back, as in
Resonant (swapped on consoles that confirm with Circle).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "input.h"
#include "jobs.h"
#include "catalog.h"
#include "download.h"
#include "store.h"
#include "version.h"
#include "ui.h"

#define C_BG		0xff05070d
#define C_TEXT		0xffffffff
#define C_DIM		0xff9aa4b8
#define C_ACCENT	0xff2b8fff
#define C_OK		0xff4cd08a
#define C_ERR		0xffff7a7a
#define C_WARN		0xffffc861
#define C_PANEL		0x8c0a0e18
#define C_ROW_ON	0x552b8fff
#define C_CYAN		0xff4ad9ff
#define C_TEXT2		0xffa2acc0
#define C_BORDER	0xff1f2533

#define LIST_X		60
#define LIST_Y		150
#define LIST_W		(SCREEN_W - 2 * LIST_X)
#define ROW_H		68
#define ROWS		7
#define ICON_LIST	56
#define ICON_BIG	200
#define GLYPH		28
#define MAX_RESULTS	2000
#define CARD_Y		104			// the home and picker cards fill CARD_Y..CARD_Y + CARD_H
#define CARD_H		528
#define CARD_GAP	16
#define TILE_H		112			// the picker's small cards (themes, avatars)
#define TILE_COLS	3

enum { SCR_START, SCR_HOME, SCR_LIST, SCR_DETAIL, SCR_PROFILE };

static int screen = SCR_START;
static int dirty = 1;
static uint64_t t_now;
static image_t *backdrop, *logo;

static const char *section_names[NUM_TYPES] = {"Homebrew", "Themes", "Avatars"};

/*
==================
Helpers
==================
*/
static image_t *Load_Image(const char *name, int w, int h)
{
	char path[512];
	size_t len;
	snprintf(path, sizeof(path), "%s/%s", plat_data_dir(), name);
	unsigned char *data = plat_read_file(path, &len);
	if (!data) { plat_log("ui: %s missing", path); return NULL; }
	image_t *img = image_decode(data, (int)len, w, h);
	free(data);
	return img;
}

static void Fmt_Size(long long b, char *out, int size)
{
	if (b >= 1024LL * 1024 * 1024) snprintf(out, size, "%.2f GB", b / 1073741824.0);
	else if (b >= 1024 * 1024) snprintf(out, size, "%.1f MB", b / 1048576.0);
	else if (b >= 1024) snprintf(out, size, "%lld KB", b / 1024);
	else snprintf(out, size, "%lld B", b);
}

static void Fmt_Count(int n, char *out, int size)
{
	if (n >= 1000) snprintf(out, size, "%d,%03d", n / 1000, n % 1000);
	else snprintf(out, size, "%d", n);
}

static char Lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// needle in the first len bytes of hay (any case)
static int Contains(const char *hay, int len, const char *needle)
{
	if (!*needle) return 1;
	for (int i = 0; i < len; i++) {
		int k = 0;
		while (needle[k] && i + k < len && Lower(hay[i + k]) == Lower(needle[k])) k++;
		if (!needle[k]) return 1;
	}
	return 0;
}

// the letter a title is filed under (generator/make-catalog.mjs letterOf)
static char Letter_Of(const char *title)
{
	for (; *title; title++) {
		char c = *title;
		if (c >= 'a' && c <= 'z') return c - 32;
		if (c >= 'A' && c <= 'Z') return c;
		if ((c >= '0' && c <= '9') || (unsigned char)c >= 0x80) return '#';
	}
	return '#';
}

// word-wraps s into lines of at most maxw; returns the number of lines
static int Wrap(int font, int px, const char *s, int maxw, char (*lines)[256], int maxlines)
{
	int n = 0;
	while (*s && n < maxlines) {
		char line[256];
		int len = 0, last_space = -1;
		while (s[len] && s[len] != '\n' && len < 255) {
			memcpy(line, s, len + 1);
			line[len + 1] = 0;
			if (font_width(font, px, line) > maxw) break;
			if (s[len] == ' ') last_space = len;
			len++;
		}
		int cut = len;
		if (s[len] && s[len] != '\n' && last_space > 0) cut = last_space;
		if (cut == 0) cut = len ? len : 1;
		snprintf(lines[n++], 256, "%.*s", cut, s);
		s += cut;
		while (*s == ' ' || *s == '\n') s++;
	}
	if (*s && n) {		// more than fits: the last line ends in ...
		size_t l = strlen(lines[n - 1]);
		if (l > 3) strcpy(lines[n - 1] + l - 3, "...");
	}
	return n;
}

// controller glyphs (Kenney's Input Prompts, CC0)
enum { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_L1, G_R1, G_L2, G_R2, G_DPAD_UD, NUM_GLYPHS };
static const char *glyph_files[NUM_GLYPHS] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "dpad_ud"};
static image_t *glyphs[NUM_GLYPHS];

static void Glyph(int g, int x, int y)
{
	// on a console that confirms with O, the button that does "X" things is O (input.c swaps them)
	if (plat_circle_confirms() && (g == G_CROSS || g == G_CIRCLE)) g = g == G_CROSS ? G_CIRCLE : G_CROSS;
	if (glyphs[g]) gfx_image(glyphs[g], x, y, GLYPH, GLYPH, 0, 255);
	else gfx_circle(x + GLYPH / 2, y + GLYPH / 2, GLYPH / 2 - 2, 0xff55585e);
}

// "<glyph> label" along the bottom; returns the x after it
static int Hint(int x, int g, const char *label)
{
	int y = SCREEN_H - 58;
	Glyph(g, x, y);
	return x + GLYPH + 8 + font_draw(FONT_MEDIUM, 19, x + GLYPH + 8, y + 3, label, C_DIM, 0) + 28;
}

static int Hint2(int x, int g1, int g2, const char *label)
{
	Glyph(g1, x, SCREEN_H - 58);
	return Hint(x + GLYPH + 4, g2, label);
}

/*
==================
Cards, as ps3.world's .explore-card: the art faded in from the right, a cyan
kicker, the title, a line or two, and chips (data/cards: tools/make-art.mjs)
==================
*/
typedef struct {
	const char *list, *art, *kicker, *title, *blurb;
} card_text_t;

static const card_text_t home_cards[NUM_TYPES] = {
	{NULL, "home-homebrew", "Apps · Games · Ports", "Homebrew", "Homebrew tools and games, PC ports and media players for HEN & CFW."},
	{NULL, "home-themes", "Customise your XMB", "Themes", "Custom themes from the full theme archive, by category."},
	{NULL, "home-avatars", "Your profile picture", "Avatars", "XMB avatars from every region, saved to your PS3 user."},
};
static const card_text_t cat_cards[] = {
	{"Games", "cat-games", "Play", "Homebrew Games", "Grid Siege, Super Mario War, PS3Sweeper+ and more."},
	{"Ports", "cat-ports", "PC & retro", "Ports", "Undertale, Nazi Zombies: Portable, Mario Kart 64 and more. No game files needed."},
	{"Tools", "cat-tools", "Essentials", "Homebrew Tools", "Apollo, IRISMAN, webMAN MOD, PKGi, multiMAN and more."},
	{"Media", "cat-media", "Watch & listen", "Media Players", "Movian, Yo! Player, Jellyfin, Spotify and Resonant for Plex."},
	{"Misc", "cat-misc", "Prototypes & demos", "Miscellaneous", "Unreleased prototypes, demos and more."},
};
#define NUM_CAT_CARDS	((int)(sizeof(cat_cards) / sizeof(cat_cards[0])))

static image_t *card_bg, *home_art[NUM_TYPES], *cat_art[NUM_CAT_CARDS];

static const card_text_t *Cat_Card(const char *list, image_t **art)
{
	for (int i = 0; i < NUM_CAT_CARDS; i++)
		if (!strcmp(cat_cards[i].list, list)) { *art = cat_art[i]; return &cat_cards[i]; }
	*art = NULL;
	return NULL;
}

// the card's frame: a blue ring when it's selected, the site's 1px border when not, and its gradient
static void Card_Frame(int x, int y, int w, int h, int on)
{
	if (on) gfx_round_rect(x - 3, y - 3, w + 6, h + 6, 21, C_ACCENT);
	else gfx_round_rect(x - 1, y - 1, w + 2, h + 2, 19, C_BORDER);
	if (card_bg) gfx_image(card_bg, x, y, w, h, 18, 255);
	else gfx_round_rect(x, y, w, h, 18, 0xff0e131f);
}

static int Chip(int x, int y, const char *text)
{
	int w = font_width(FONT_MEDIUM, 18, text) + 28;
	gfx_round_rect(x, y, w, 36, 18, 0x14ffffff);
	gfx_round_rect(x + 1, y + 1, w - 2, 34, 17, 0xbf05070d);
	font_draw(FONT_MEDIUM, 18, x + 14, y + 7, text, C_TEXT2, 0);
	return w;
}

static void Card(int x, int y, int w, int h, int on, const image_t *art, const char *kicker, const char *title,
	int title_px, const char *blurb, const char **chips, int num_chips)
{
	Card_Frame(x, y, w, h, on);
	if (art) gfx_image(art, x, y, w, h, 18, on ? 255 : 160);		// the site's 0.8 on hover, 0.5 off
	int tx = x + 28, ty = y + 26, chip_y = y + h - 26 - 36;
	if (kicker) {
		font_draw(FONT_BOLD, 20, tx, ty, kicker, C_CYAN, w - 56);
		ty += 34;
	}
	font_draw(FONT_BOLD, title_px, tx, ty, title, C_TEXT, w - 56);
	ty += title_px + 16;
	char lines[4][256];
	int max = (chip_y - 8 - ty) / 30;
	int n = blurb && max > 0 ? Wrap(FONT_REGULAR, 21, blurb, w * 2 / 3, lines, max > 4 ? 4 : max) : 0;
	for (int i = 0; i < n; i++)
		font_draw(FONT_REGULAR, 21, tx, ty + i * 30, lines[i], C_TEXT2, 0);
	// the chips that fit on one line
	int cx = tx;
	for (int i = 0; i < num_chips; i++) {
		if (cx + font_width(FONT_MEDIUM, 18, chips[i]) + 28 > x + w - 28) break;
		cx += Chip(cx, chip_y, chips[i]) + 8;
	}
}

// a small card: a category of themes or avatars, and how many
static void Tile(int x, int y, int w, int h, int on, const char *title, const char *count)
{
	Card_Frame(x, y, w, h, on);
	font_draw(FONT_BOLD, 26, x + 24, y + 22, title, C_TEXT, w - 48);
	font_draw(FONT_MEDIUM, 19, x + 24, y + h - 44, count, on ? C_CYAN : C_TEXT2, w - 48);
}

static const char *Error_Text(int kind, const char *detail_msg)
{
	static char buf[300];
	switch (kind) {
	case ERR_NETWORK:
		return "No network connection. Check Settings > Network Settings on the PS3, then try again.";
	case ERR_TLS:
		return "The secure connection to dl.ps3.world failed. Check the PS3's date and time (Settings > Date and Time Settings), then try again.";
	case ERR_NOT_FOUND:
		return "Not found on the server (404). It may have moved: try again in a moment.";
	case ERR_DISK_FULL:
		snprintf(buf, sizeof(buf), "Not enough free space. %s", detail_msg);
		return buf;
	}
	snprintf(buf, sizeof(buf), "%s", detail_msg);
	return buf;
}

/*
==================
Pages: fetched (or read from the HDD) as the lists reach them, a few kept
==================
*/
#define PAGE_SLOTS	16
#define RETRY_US	5000000

typedef struct {
	unsigned char sha[32];		// which file (the index's sha256 names it)
	int search;					// a search.txt rather than a page
	catalog_t *page;
	struct search_s *lines;
	job_t *job;
	int failed, err_kind;
	char err[160];
	uint64_t used, failed_at;
} file_slot_t;

typedef struct search_s {
	char *text;
	const char **line;		// each item's line ("title\tauthor\tgroup")
	int *len;
	int count;
} search_t;

static file_slot_t files[PAGE_SLOTS];

typedef struct {
	cat_file_t f;
	int search;
} file_req_t;

static void Search_Free(search_t *s)
{
	if (!s) return;
	free(s->text);
	free(s->line);
	free(s->len);
	free(s);
}

static void File_Run(job_t *j)
{
	file_req_t *r = j->arg;
	size_t len;
	char *body = store_get_file(&r->f, &len, &j->cancel, &j->ival, j->err, sizeof(j->err));
	if (!body) return;
	if (!r->search) {
		j->result = catalog_parse_page(body, len, j->err, sizeof(j->err));
		free(body);
		return;
	}
	search_t *s = calloc(1, sizeof(*s));
	int n = 0;
	for (size_t i = 0; i < len; i++) if (body[i] == '\n') n++;
	if (!s || !(s->line = malloc(sizeof(char *) * (n + 1))) || !(s->len = malloc(sizeof(int) * (n + 1)))) {
		Search_Free(s);
		free(body);
		return;
	}
	s->text = body;
	for (char *p = body; *p && s->count <= n; ) {
		char *e = strchr(p, '\n');
		s->line[s->count] = p;
		s->len[s->count] = e ? (int)(e - p) : (int)strlen(p);
		s->count++;
		if (!e) break;
		p = e + 1;
	}
	j->result = s;
}

static void File_Cleanup(job_t *j)
{
	file_req_t *r = j->arg;
	if (r && r->search) Search_Free(j->result);
	else catalog_free(j->result);
	free(r);
}

static void Files_Tick(void)
{
	for (int i = 0; i < PAGE_SLOTS; i++) {
		file_slot_t *s = &files[i];
		if (!job_done(s->job)) continue;
		if (s->search) s->lines = s->job->result;
		else s->page = s->job->result;
		s->job->result = NULL;
		s->failed = !s->page && !s->lines;
		s->err_kind = s->job->ival;
		snprintf(s->err, sizeof(s->err), "%s", s->job->err);
		if (s->failed) {
			s->failed_at = t_now;
			plat_log("catalog: %s", s->err);
		}
		job_release(s->job);
		s->job = NULL;
		dirty = 1;
	}
}

// the slot for a file, asked for if it isn't there; NULL if every slot is busy
static file_slot_t *File_Get(const cat_file_t *f, int search)
{
	int lru = -1;
	for (int i = 0; i < PAGE_SLOTS; i++) {
		file_slot_t *s = &files[i];
		if ((s->page || s->lines || s->job || s->failed) && s->search == search && !memcmp(s->sha, f->sha256, 32)) {
			s->used = t_now;
			if (!s->failed || t_now - s->failed_at < RETRY_US) return s;
			lru = i;		// failed a while ago: try again in the same slot
			break;
		}
		if (!s->job && (lru < 0 || s->used < files[lru].used)) lru = i;
	}
	if (lru < 0) return NULL;
	file_req_t *r = calloc(1, sizeof(*r));
	job_t *j = r ? job_new(File_Run, File_Cleanup) : NULL;
	if (!j) { free(r); return NULL; }
	r->f = *f;
	r->search = search;
	j->arg = r;
	file_slot_t *s = &files[lru];
	catalog_free(s->page);
	Search_Free(s->lines);
	memset(s, 0, sizeof(*s));
	memcpy(s->sha, f->sha256, 32);
	s->search = search;
	s->used = t_now;
	s->job = j;
	job_post(j, LANE_API);
	return s;
}

/*
==================
The index, sections and their lists
==================
*/
static cat_index_t *ix;
static cat_index_t *pending;		// a newer one, used when the detail screen isn't open
static job_t *fetch_job;
static char banner[200];			// "Offline: ..." over the list
static char start_msg[300];		// the start screen's line
static int start_err;

typedef struct {
	int sel, top;
	char query[128];
	int *results;					// positions that match the search (NULL: the whole list)
	int num_results;
	int search_wait;				// the search.txt is on its way
} view_t;

static view_t *views[NUM_TYPES];	// one per list
static int cur;						// the section shown
static int cur_list[NUM_TYPES];		// its list, or -1 for its category picker
static int picker_sel, picker_top;	// (picker_top: the first row of tiles shown)
static int home_sel;

static cat_list_t *List(int t, int l) { return ix && l >= 0 && l < ix->num_lists[t] ? &ix->lists[t][l] : NULL; }
static int Picking(void) { return ix && ix->num_lists[cur] > 1 && cur_list[cur] < 0; }
static cat_list_t *Cur_List(void) { return Picking() ? NULL : List(cur, cur_list[cur] < 0 ? 0 : cur_list[cur]); }
static view_t *Cur_View(void) { return Cur_List() && views[cur] ? &views[cur][cur_list[cur] < 0 ? 0 : cur_list[cur]] : NULL; }
static int View_Count(const view_t *v, const cat_list_t *l) { return v->results ? v->num_results : l->count; }
static int View_Pos(const view_t *v, int row) { return v->results ? v->results[row] : row; }

static void Clamp(view_t *v, int count)
{
	if (v->sel >= count) v->sel = count - 1;
	if (v->sel < 0) v->sel = 0;
	if (v->top > v->sel) v->top = v->sel;
	if (v->top < v->sel - ROWS + 1) v->top = v->sel - ROWS + 1;
	if (v->top > count - ROWS) v->top = count - ROWS;
	if (v->top < 0) v->top = 0;
}

// the item at a position of a list: NULL until its page is here (*slot says how it's going)
static const cat_item_t *Item_At(const cat_list_t *l, int pos, file_slot_t **slot)
{
	int p = pos / ix->page_size;
	*slot = NULL;
	if (pos < 0 || pos >= l->count || p >= l->num_pages) return NULL;
	file_slot_t *s = File_Get(&l->pages[p], 0);
	*slot = s;
	if (!s || !s->page) return NULL;
	int k = pos % ix->page_size;
	return k < s->page->count ? &s->page->items[k] : NULL;
}

// the item's search line (title, author, group), if the list's search.txt is here
static const char *Search_Line(const cat_list_t *l, int pos, int *len)
{
	for (int i = 0; i < PAGE_SLOTS; i++)
		if (files[i].lines && files[i].search && !memcmp(files[i].sha, l->search.sha256, 32) && pos < files[i].lines->count) {
			*len = files[i].lines->len[pos];
			return files[i].lines->line[pos];
		}
	return NULL;
}

static void Views_Free(void)
{
	for (int t = 0; t < NUM_TYPES; t++) {
		if (views[t] && ix)
			for (int l = 0; l < ix->num_lists[t]; l++) free(views[t][l].results);
		free(views[t]);
		views[t] = NULL;
	}
}

static void Index_Use(cat_index_t *n)
{
	// where each list was, kept when the lists are the same ones
	int same = ix != NULL;
	for (int t = 0; same && t < NUM_TYPES; t++)
		same = ix->num_lists[t] == n->num_lists[t];
	view_t *keep[NUM_TYPES] = {0};
	for (int t = 0; t < NUM_TYPES; t++) {
		if (same && views[t]) {
			keep[t] = views[t];
			for (int l = 0; l < n->num_lists[t]; l++) {
				free(keep[t][l].results);		// searches start again on the new lists
				keep[t][l].results = NULL;
				keep[t][l].query[0] = 0;
				Clamp(&keep[t][l], n->lists[t][l].count);
			}
			views[t] = NULL;
		}
	}
	Views_Free();
	catalog_free_index(ix);
	ix = n;
	for (int t = 0; t < NUM_TYPES; t++) {
		views[t] = keep[t] ? keep[t] : calloc(ix->num_lists[t] ? ix->num_lists[t] : 1, sizeof(view_t));
		if (!same) cur_list[t] = ix->num_lists[t] > 1 ? -1 : 0;
	}
	if (!same) picker_sel = picker_top = 0;
	if (screen == SCR_START) screen = SCR_HOME;
	dirty = 1;
}

static void Fetch_Run(job_t *j)
{
	http_res_t res;
	size_t len = 0;
	char *body = http_fetch(NULL, CATALOG_URL, NULL, NULL, 30, &j->cancel, &len, &res);

	if (!body || res.status != 200) {
		j->ival = download_error_kind(res.err, body ? res.status : 0);
		if (body) snprintf(j->err, sizeof(j->err), "The catalogue wasn't found on the server (HTTP %d)", res.status);
		else snprintf(j->err, sizeof(j->err), "%s", res.err[0] ? res.err : "The catalogue didn't download");
		free(body);
		return;
	}
	cat_index_t *n = catalog_parse_index(body, len, j->err, sizeof(j->err));
	if (n) store_save_index(body, len);
	j->result = n;
	free(body);
}

static void Fetch_Cleanup(job_t *j)
{
	catalog_free_index(j->result);
}

static void Fetch_Start(void)
{
	if (fetch_job) return;
	if (!ix) {
		snprintf(start_msg, sizeof(start_msg), "Loading the catalogue...");
		start_err = 0;
	}
	fetch_job = job_new(Fetch_Run, Fetch_Cleanup);
	if (fetch_job) job_post(fetch_job, LANE_API);
	dirty = 1;
}

static void Fetch_Tick(void)
{
	if (!job_done(fetch_job))
		return;
	cat_index_t *n = fetch_job->result;
	fetch_job->result = NULL;
	if (n) {
		plat_log("catalog: index of %d homebrew (%d lists), %d themes, %d avatars, generated %s",
			n->count[TYPE_HOMEBREW], n->num_lists[TYPE_HOMEBREW], n->count[TYPE_THEME], n->count[TYPE_AVATAR], n->generated);
		banner[0] = 0;
		if (ix && !strcmp(ix->generated, n->generated)) {
			catalog_free_index(n);		// the one we have
		} else if (screen == SCR_DETAIL || screen == SCR_PROFILE) {
			catalog_free_index(pending);
			pending = n;
		} else
			Index_Use(n);
	} else {
		plat_log("catalog: %s", fetch_job->err);
		if (ix) {
			snprintf(banner, sizeof(banner), "Offline: showing the catalogue from %.10s", ix->generated);
		} else {
			snprintf(start_msg, sizeof(start_msg), "%s", Error_Text(fetch_job->ival, fetch_job->err));
			start_err = 1;
		}
	}
	job_release(fetch_job);
	fetch_job = NULL;
	dirty = 1;
}

// the search, once the list's search.txt is here
static void Search_Run(view_t *v, const cat_list_t *l)
{
	file_slot_t *s = File_Get(&l->search, 1);
	if (!s || !s->lines) {
		v->search_wait = !(s && s->failed);
		if (s && s->failed) {
			// no search file (offline and never fetched): say so, and match nothing
			snprintf(banner, sizeof(banner), "Search isn't available: %s", Error_Text(s->err_kind, s->err));
			free(v->results);
			v->results = malloc(sizeof(int));
			v->num_results = 0;
			dirty = 1;
		}
		return;
	}
	v->search_wait = 0;
	free(v->results);
	v->results = malloc(sizeof(int) * MAX_RESULTS);
	v->num_results = 0;
	for (int i = 0; v->results && i < s->lines->count && v->num_results < MAX_RESULTS; i++)
		if (Contains(s->lines->line[i], s->lines->len[i], v->query))
			v->results[v->num_results++] = i;
	v->sel = v->top = 0;
	dirty = 1;
}

/*
==================
Icons: from the HDD cache, or fetched once and cached
==================
*/
#define ICON_SLOTS		96
#define ICON_INFLIGHT	12

typedef struct {
	char id[64];
	int size;
	image_t *img;
	job_t *job;
	int failed;
	uint64_t used;
} icon_slot_t;
static icon_slot_t icons[ICON_SLOTS];
static int icons_inflight;

typedef struct {
	char url[1100];
	char path[600];
	int size;
} icon_req_t;

static void Icon_Run(job_t *j)
{
	icon_req_t *r = j->arg;
	size_t len = 0;
	unsigned char *data = plat_read_file(r->path, &len);
	if (!data) {
		http_res_t res;
		char *body = http_fetch(NULL, r->url, NULL, NULL, 20, &j->cancel, &len, &res);
		if (body && res.status == 200) {
			plat_write_file(r->path, body, len);
			data = (unsigned char *)body;
		} else
			free(body);
	}
	if (data) {
		j->result = image_decode(data, (int)len, r->size, r->size);
		free(data);
	}
}

static void Icon_Cleanup(job_t *j)
{
	free(j->arg);
	image_free(j->result);
}

static void Icons_Tick(void)
{
	for (int i = 0; i < ICON_SLOTS; i++) {
		icon_slot_t *s = &icons[i];
		if (!job_done(s->job)) continue;
		s->img = s->job->result;
		s->job->result = NULL;
		s->failed = !s->img;
		job_release(s->job);
		s->job = NULL;
		icons_inflight--;
		dirty = 1;
	}
}

static image_t *Icon_Get(const cat_item_t *it, int size)
{
	int lru = -1;
	if (!it->icon[0]) return NULL;
	for (int i = 0; i < ICON_SLOTS; i++) {
		icon_slot_t *s = &icons[i];
		if (s->size == size && !strcmp(s->id, it->id)) {
			s->used = t_now;
			return s->img;
		}
		if (!s->job && (lru < 0 || s->used < icons[lru].used)) lru = i;
	}
	if (lru < 0 || icons_inflight >= ICON_INFLIGHT) return NULL;
	icon_req_t *r = calloc(1, sizeof(*r));
	job_t *j = r ? job_new(Icon_Run, Icon_Cleanup) : NULL;
	if (!j) { free(r); return NULL; }
	snprintf(r->url, sizeof(r->url), "%s%s", CATALOG_BASE, it->icon);
	store_icon_path(it, r->path, sizeof(r->path));
	r->size = size;
	j->arg = r;
	icon_slot_t *s = &icons[lru];
	image_free(s->img);
	memset(s, 0, sizeof(*s));
	snprintf(s->id, sizeof(s->id), "%s", it->id);
	s->size = size;
	s->used = t_now;
	s->job = j;
	icons_inflight++;
	job_post(j, LANE_ART);
	return NULL;
}

static void Placeholder(const char *title, int x, int y, int size)
{
	gfx_round_rect(x, y, size, size, size / 10, 0xff1a1c20);
	char ch[2] = {Letter_Of(title), 0};
	font_draw_center(FONT_BOLD, size / 2, x + size / 2, y + size / 4, ch, 0xff3a4152, 0);
}

static void Icon_Draw(const cat_item_t *it, int x, int y, int size)
{
	image_t *img = Icon_Get(it, size);
	if (img) gfx_image(img, x, y, size, size, size / 10, 255);
	else Placeholder(it->title, x, y, size);
}

/*
==================
Downloads (one at a time, from the detail screen)
==================
*/
static cat_item_t *det;			// the detail screen's item (a copy: its page may go)
static download_t *dl;
static job_t *dl_job;
static uint64_t dl_t_last;
static long long dl_last_got;
static double dl_speed;			// bytes a second, smoothed
static char result_msg[400];		// what happened, on the detail screen
static int result_ok;

static void Dl_Run(job_t *j) { download_run(j->arg); }
static void Profiles_Load(void);

static void Download_Start(void)
{
	char dest[512];
	int code = store_dest(det, dest, sizeof(dest));
	if (code == DEST_PICK_PROFILE) {
		Profiles_Load();
		screen = SCR_PROFILE;
		dirty = 1;
		return;
	}
	if (code != DEST_OK) {
		snprintf(result_msg, sizeof(result_msg), "%s", store_dest_message(code));
		result_ok = 0;
		dirty = 1;
		return;
	}
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", dest);
	char *slash = strrchr(dir, '/');
	if (slash) *slash = 0;
	if (!plat_mkdirs(dir)) {
		snprintf(result_msg, sizeof(result_msg), "Couldn't make the folder %s", dir);
		result_ok = 0;
		dirty = 1;
		return;
	}
	dl = calloc(1, sizeof(*dl));
	if (!dl) return;
	snprintf(dl->url, sizeof(dl->url), "%s%s", CATALOG_BASE, det->url);
	snprintf(dl->dest, sizeof(dl->dest), "%s", dest);
	dl->size = det->size;
	memcpy(dl->sha256, det->sha256, 32);
	dl_last_got = 0;
	dl_speed = 0;
	dl_t_last = t_now;
	result_msg[0] = 0;
	dl_job = job_new(Dl_Run, NULL);
	if (!dl_job) { free(dl); dl = NULL; return; }
	dl_job->arg = dl;
	job_post(dl_job, LANE_DL);
	plat_log("download: %s -> %s", dl->url, dl->dest);
	dirty = 1;
}

static const char *Install_Hint(int type)
{
	switch (type) {
	case TYPE_HOMEBREW: return "Install it from the XMB: Package Manager > Install Package Files.";
	case TYPE_THEME: return "Install it from the XMB: Settings > Theme Settings > Theme > Install.";
	case TYPE_AVATAR: return "Choose it from the XMB: PlayStation Network > Account Management > Avatar.";
	}
	return "";
}

static void Download_Tick(void)
{
	if (!dl) return;
	// progress: the speed over the last half second, smoothed
	if (t_now - dl_t_last >= 500000) {
		double inst = (dl->got - dl_last_got) * 1e6 / (double)(t_now - dl_t_last);
		if (dl->state == DL_RUNNING) dl_speed = dl_speed ? dl_speed * 0.6 + inst * 0.4 : inst;
		dl_last_got = dl->got;
		dl_t_last = t_now;
		dirty = 1;
	}
	if (!job_done(dl_job)) return;
	switch (dl->state) {
	case DL_DONE: {
		char size[32];
		Fmt_Size(dl->size, size, sizeof(size));
		store_mark_downloaded(det, dl->dest);
		snprintf(result_msg, sizeof(result_msg), "Checksum OK (%s). Saved to %s\n%s", size, dl->dest, Install_Hint(det->type));
		result_ok = 1;
		break;
	}
	case DL_CANCELLED:
		snprintf(result_msg, sizeof(result_msg), "Download cancelled.");
		result_ok = 0;
		break;
	case DL_PAUSED:
		snprintf(result_msg, sizeof(result_msg), "Download paused. Press X to resume.");
		result_ok = 0;
		break;
	default:
		snprintf(result_msg, sizeof(result_msg), "%s%s",
			dl->err_kind == ERR_CHECKSUM ? "The download didn't match its checksum, so it was deleted. Try again." :
			Error_Text(dl->err_kind, dl->err),
			dl->err_kind == ERR_NETWORK ? "\nPress X to resume where it stopped." : "");
		result_ok = 0;
	}
	job_release(dl_job);
	dl_job = NULL;
	free(dl);
	dl = NULL;
	dirty = 1;
}

/*
==================
Profiles (avatars)
==================
*/
static char profiles[16][64], profile_names[16][64];
static int num_profiles, profile_sel;

static void Profiles_Load(void)
{
	num_profiles = store_profiles(profiles, 16);
	for (int i = 0; i < num_profiles; i++) {
		char path[128];
		snprintf(path, sizeof(path), "/dev_hdd0/home/%s/localusername", profiles[i]);
		char *name = (char *)plat_read_file(path, NULL);
		snprintf(profile_names[i], 64, "%s", name && name[0] ? name : profiles[i]);
		free(name);
	}
	profile_sel = 0;
}

/*
==================
Input
==================
*/
static int searching;

static void Letter_Jump(view_t *v, const cat_list_t *l, int dir)
{
	int pos = v->sel, slot = 0;
	for (int i = 0; i < NUM_LETTERS; i++)		// the letter we're in: the last one starting at or before pos
		if (l->letter_first[i] >= 0 && l->letter_first[i] <= pos) slot = i;
	if (dir > 0) {
		for (int i = slot + 1; i < NUM_LETTERS; i++)
			if (l->letter_first[i] >= 0) { v->sel = l->letter_first[i]; break; }
	} else {
		// to the start of this letter, or of the one before when already there
		if (l->letter_first[slot] >= 0 && l->letter_first[slot] < pos) v->sel = l->letter_first[slot];
		else for (int i = slot - 1; i >= 0; i--)
			if (l->letter_first[i] >= 0) { v->sel = l->letter_first[i]; break; }
	}
	v->top = v->sel;
}

// Homebrew's few categories are big cards, two a row; Themes' and Avatars' are tiles, TILE_COLS a row
static int Picker_Cols(void) { return cur == TYPE_HOMEBREW ? 2 : TILE_COLS; }
static int Picker_Rows_Shown(void) { return cur == TYPE_HOMEBREW ? 99 : (CARD_H + CARD_GAP) / (TILE_H + CARD_GAP); }

static void Open_Section(int t)
{
	cur = t;
	screen = SCR_LIST;
	if (ix->num_lists[t] > 1) {		// its categories first, on the last one opened
		picker_sel = cur_list[t] > 0 ? cur_list[t] : 0;
		picker_top = 0;
		cur_list[t] = -1;
	}
	dirty = 1;
}

static void Section_Step(int dir)
{
	Open_Section((cur + NUM_TYPES + dir) % NUM_TYPES);
}

static void Input_Home(uint32_t p)
{
	// Homebrew on the left, Themes over Avatars on the right
	if ((p & BTN_RIGHT) && home_sel == TYPE_HOMEBREW) home_sel = TYPE_THEME;
	else if (p & BTN_LEFT) home_sel = TYPE_HOMEBREW;
	else if ((p & BTN_DOWN) && home_sel == TYPE_THEME) home_sel = TYPE_AVATAR;
	else if ((p & BTN_UP) && home_sel == TYPE_AVATAR) home_sel = TYPE_THEME;
	if (p & BTN_CROSS) Open_Section(home_sel);
	if (p) dirty = 1;
}

static void Input_Picker(uint32_t p)
{
	int n = ix->num_lists[cur], cols = Picker_Cols(), shown = Picker_Rows_Shown();
	if (p & BTN_R1) { Section_Step(1); return; }
	if (p & BTN_L1) { Section_Step(-1); return; }
	if (p & BTN_CIRCLE) {
		home_sel = cur;
		screen = SCR_HOME;
		dirty = 1;
		return;
	}
	if (p & BTN_RIGHT) picker_sel++;
	if (p & BTN_LEFT) picker_sel--;
	if ((p & BTN_DOWN) && picker_sel + cols < n) picker_sel += cols;
	else if ((p & BTN_DOWN) && picker_sel / cols < (n - 1) / cols) picker_sel = n - 1;	// onto a shorter last row
	if ((p & BTN_UP) && picker_sel >= cols) picker_sel -= cols;
	if (picker_sel >= n) picker_sel = n - 1;
	if (picker_sel < 0) picker_sel = 0;
	int row = picker_sel / cols;
	if (row < picker_top) picker_top = row;
	if (row >= picker_top + shown) picker_top = row - shown + 1;
	if (p & BTN_CROSS) cur_list[cur] = picker_sel;
	if (p) dirty = 1;
}

static void Input_List(uint32_t p)
{
	cat_list_t *l = Cur_List();
	view_t *v = Cur_View();
	if (!l || !v) return;
	if (p & BTN_R1) { Section_Step(1); return; }
	if (p & BTN_L1) { Section_Step(-1); return; }
	int count = View_Count(v, l);
	if (p & BTN_DOWN) v->sel++;
	if (p & BTN_UP) v->sel--;
	if (p & BTN_RIGHT) v->sel += ROWS;
	if (p & BTN_LEFT) v->sel -= ROWS;
	if (!v->results && (p & BTN_R2)) Letter_Jump(v, l, 1);
	if (!v->results && (p & BTN_L2)) Letter_Jump(v, l, -1);
	if (p & BTN_TRIANGLE) {
		char title[64];
		snprintf(title, sizeof(title), "Search %s%s%s", section_names[cur], l->name[0] ? ": " : "", l->name);
		if (plat_osk_open(title, v->query)) searching = 1;
	}
	if ((p & (BTN_SQUARE | BTN_CIRCLE)) && (v->query[0] || v->search_wait)) {
		v->query[0] = 0;
		v->search_wait = 0;
		free(v->results);
		v->results = NULL;
		v->sel = v->top = 0;
		dirty = 1;
		return;
	}
	if ((p & BTN_CIRCLE) && ix->num_lists[cur] > 1) {		// back to the section's categories
		picker_sel = cur_list[cur];
		picker_top = picker_sel / Picker_Cols() - Picker_Rows_Shown() + 1;
		if (picker_top < 0) picker_top = 0;
		cur_list[cur] = -1;
		dirty = 1;
		return;
	}
	if (p & BTN_CIRCLE) {		// back to the home cards
		home_sel = cur;
		screen = SCR_HOME;
		dirty = 1;
		return;
	}
	if ((p & BTN_CROSS) && count) {
		file_slot_t *slot;
		const cat_item_t *it = Item_At(l, View_Pos(v, v->sel), &slot);
		if (it && (det = catalog_item_copy(it))) {
			result_msg[0] = 0;
			screen = SCR_DETAIL;
		}
	}
	Clamp(v, View_Count(v, l));
	if (p) dirty = 1;
}

static void Leave_Detail(void)
{
	free(det);
	det = NULL;
	screen = SCR_LIST;
	if (pending) {
		Index_Use(pending);
		pending = NULL;
	}
	dirty = 1;
}

static void Input_Detail(uint32_t p)
{
	if (dl) {
		if (p & BTN_CIRCLE) dl->cancel = 1;		// O cancels a download in progress
		return;
	}
	if (p & BTN_CIRCLE) { Leave_Detail(); return; }
	if ((p & BTN_CROSS) && det) Download_Start();
	if (p) dirty = 1;
}

static void Input_Profile(uint32_t p)
{
	if (p & BTN_DOWN && profile_sel < num_profiles - 1) profile_sel++;
	if (p & BTN_UP && profile_sel > 0) profile_sel--;
	if (p & BTN_CIRCLE) screen = SCR_DETAIL;
	if (p & BTN_CROSS && num_profiles) {
		store_set_avatar_profile(profiles[profile_sel]);
		screen = SCR_DETAIL;
		Download_Start();
	}
	if (p) dirty = 1;
}

/*
==================
Frame
==================
*/
void ui_init(void)
{
	char name[64];
	backdrop = Load_Image("backdrop.png", SCREEN_W, SCREEN_H);
	logo = Load_Image("logo.png", 220, 31);
	for (int i = 0; i < NUM_GLYPHS; i++) {
		snprintf(name, sizeof(name), "glyphs/%s.png", glyph_files[i]);
		int px = (int)(GLYPH * gfx_scale() + 0.5f);
		glyphs[i] = Load_Image(name, px, px);
	}
	// the cards' art, at the sizes Draw_Home and Draw_Picker draw them (tools/make-art.mjs makes them so)
	card_bg = Load_Image("cards/bg.png", 256, 256);
	for (int t = 0; t < NUM_TYPES; t++) {
		snprintf(name, sizeof(name), "cards/%s.png", home_cards[t].art);
		home_art[t] = Load_Image(name, t == TYPE_HOMEBREW ? LIST_W - 504 - CARD_GAP : 504, t == TYPE_HOMEBREW ? CARD_H : (CARD_H - CARD_GAP) / 2);
	}
	for (int i = 0; i < NUM_CAT_CARDS; i++) {
		snprintf(name, sizeof(name), "cards/%s.png", cat_cards[i].art);
		cat_art[i] = Load_Image(name, (LIST_W - CARD_GAP) / 2, (CARD_H - CARD_GAP) / 2);
	}
	store_init();
	cat_index_t *c = store_load_index();
	if (c) {
		plat_log("catalog: index from the HDD cache (generated %s)", c->generated);
		Index_Use(c);
	}
	Fetch_Start();		// the server's copy, in the background
}

void ui_update(void)
{
	t_now = plat_time_us();
	input_update();
	Fetch_Tick();
	Files_Tick();
	Icons_Tick();
	Download_Tick();

	if (searching) {
		char q[128];
		int r = plat_osk_poll(q, sizeof(q));
		if (r) {
			searching = 0;
			view_t *v = Cur_View();
			if (r > 0 && v && q[0]) {
				snprintf(v->query, sizeof(v->query), "%s", q);
				Search_Run(v, Cur_List());
			}
			dirty = 1;
		}
		return;
	}
	view_t *v = Cur_View();
	if (v && v->search_wait) Search_Run(v, Cur_List());		// (once the search.txt is here)

	uint32_t p = input_pressed();
	switch (screen) {
	case SCR_START:
		if ((p & BTN_CROSS) && start_err) Fetch_Start();
		break;
	case SCR_HOME: Input_Home(p); break;
	case SCR_LIST:
		if (Picking()) Input_Picker(p);
		else Input_List(p);
		break;
	case SCR_DETAIL: Input_Detail(p); break;
	case SCR_PROFILE: Input_Profile(p); break;
	}
}

int ui_needs_redraw(void)
{
	int d = dirty;
	dirty = 0;
	return d;
}

void ui_force_redraw(void)
{
	dirty = 1;
}

static void Draw_Header(void)
{
	if (logo) gfx_image(logo, LIST_X, 40, 220, 31, 0, 255);
	int x = LIST_X;
	Glyph(G_L1, x, 92);
	x += GLYPH + 16;
	for (int t = 0; t < NUM_TYPES; t++) {
		char label[64], n[16];
		Fmt_Count(ix->count[t], n, sizeof(n));
		snprintf(label, sizeof(label), "%s  %s", section_names[t], n);
		int w = font_width(FONT_MEDIUM, 22, label) + 32;
		if (t == cur) gfx_round_rect(x, 88, w, 36, 18, C_ROW_ON);
		font_draw(FONT_MEDIUM, 22, x + 16, 94, label, t == cur ? C_TEXT : C_DIM, 0);
		x += w + 8;
	}
	Glyph(G_R1, x + 8, 92);
	const view_t *v = Cur_View();
	const cat_list_t *l = Cur_List();
	char line[200];
	if (v && (v->query[0] || v->search_wait)) {
		if (v->search_wait) snprintf(line, sizeof(line), "Searching for \"%s\"...", v->query);
		else snprintf(line, sizeof(line), "Search: \"%s\"  (%d%s)", v->query, v->num_results, v->num_results == MAX_RESULTS ? "+" : "");
		font_draw_right(FONT_MEDIUM, 20, SCREEN_W - LIST_X, 96, line, C_WARN);
	} else if (banner[0])
		font_draw_right(FONT_REGULAR, 18, SCREEN_W - LIST_X, 98, banner, C_WARN);
	else if (l && l->name[0])
		font_draw_right(FONT_MEDIUM, 20, SCREEN_W - LIST_X, 96, l->name, C_TEXT);
	else if (fetch_job)
		font_draw_right(FONT_REGULAR, 18, SCREEN_W - LIST_X, 98, "Checking for updates...", C_DIM);
}

static void Badge(int *rx, int y, const char *text, uint32_t c)
{
	int w = font_width(FONT_MEDIUM, 15, text) + 16;
	*rx -= w;
	gfx_round_rect(*rx, y, w, 24, 12, with_alpha(c, 50));
	font_draw(FONT_MEDIUM, 15, *rx + 8, y + 3, text, c, 0);
	*rx -= 8;
}

// the logo, where we are (as the site's crumbs), and the catalogue's state, over the cards
static void Draw_Top(const char *crumb)
{
	if (logo) gfx_image(logo, LIST_X, 40, 220, 31, 0, 255);
	if (crumb) {
		int w = font_width(FONT_BOLD, 18, crumb) + 28;
		gfx_round_rect(LIST_X + 236, 38, w, 34, 17, 0x404ad9ff);
		gfx_round_rect(LIST_X + 237, 39, w - 2, 32, 16, 0xff0a1622);
		font_draw(FONT_BOLD, 18, LIST_X + 250, 44, crumb, C_CYAN, 0);
	}
	if (banner[0]) font_draw_right(FONT_REGULAR, 18, SCREEN_W - LIST_X, 68, banner, C_WARN);
	else if (fetch_job) font_draw_right(FONT_REGULAR, 18, SCREEN_W - LIST_X, 68, "Checking for updates...", C_DIM);
}

static void Count_Text(int t, int n, char *out, int size)
{
	char num[16];
	Fmt_Count(n, num, sizeof(num));
	snprintf(out, size, "%s %s%s", num, t == TYPE_THEME ? "theme" : t == TYPE_AVATAR ? "avatar" : "item", n == 1 ? "" : "s");
}

static void Draw_Home(void)
{
	Draw_Top(NULL);
	int rw = 504, lw = LIST_W - rw - CARD_GAP, rh = (CARD_H - CARD_GAP) / 2;
	for (int t = 0; t < NUM_TYPES; t++) {
		const card_text_t *c = &home_cards[t];
		const char *chips[16];
		int n = 0;
		for (int l = 0; l < ix->num_lists[t] && n < 16; l++)		// its categories ("All ..." is the whole section)
			if (strncmp(ix->lists[t][l].name, "All ", 4)) chips[n++] = ix->lists[t][l].name;
		if (t == TYPE_HOMEBREW)
			Card(LIST_X, CARD_Y, lw, CARD_H, home_sel == t, home_art[t], c->kicker, c->title, 64, c->blurb, chips, n);
		else
			Card(LIST_X + lw + CARD_GAP, CARD_Y + (t - 1) * (rh + CARD_GAP), rw, rh, home_sel == t, home_art[t],
				c->kicker, c->title, 44, c->blurb, chips, n);
	}
	Hint(LIST_X, G_CROSS, "Open");
}

static void Draw_Picker(void)
{
	Draw_Top(section_names[cur]);
	int n = ix->num_lists[cur], cols = Picker_Cols();
	char count[48];
	if (cur == TYPE_HOMEBREW) {
		int rows = (n + cols - 1) / cols;
		int w = (LIST_W - CARD_GAP) / cols, h = (CARD_H - CARD_GAP * (rows - 1)) / rows;
		for (int i = 0; i < n; i++) {
			const cat_list_t *l = &ix->lists[cur][i];
			image_t *art;
			const card_text_t *c = Cat_Card(l->name, &art);
			const char *chips[1] = {count};
			Count_Text(cur, l->count, count, sizeof(count));
			Card(LIST_X + (i % cols) * (w + CARD_GAP), CARD_Y + (i / cols) * (h + CARD_GAP), w, h, i == picker_sel, art,
				c ? c->kicker : NULL, c ? c->title : l->name, 40, c ? c->blurb : NULL, chips, 1);
		}
	} else {
		int w = (LIST_W - CARD_GAP * (cols - 1)) / cols, shown = Picker_Rows_Shown();
		for (int i = picker_top * cols; i < n && i < (picker_top + shown) * cols; i++) {
			const cat_list_t *l = &ix->lists[cur][i];
			int r = i / cols - picker_top;
			Count_Text(cur, l->count, count, sizeof(count));
			Tile(LIST_X + (i % cols) * (w + CARD_GAP), CARD_Y + r * (TILE_H + CARD_GAP), w, TILE_H, i == picker_sel, l->name, count);
		}
		int rows = (n + cols - 1) / cols;
		if (rows > shown) {		// where in the grid
			int h = shown * (TILE_H + CARD_GAP) - CARD_GAP, bh = h * shown / rows;
			int by = CARD_Y + (h - bh) * picker_top / (rows - shown);
			gfx_round_rect(LIST_X + LIST_W + 8, by, 4, bh, 2, 0x99ffffff);
		}
	}
	int x = Hint(LIST_X, G_CROSS, "Open");
	x = Hint(x, G_CIRCLE, "Back");
	Hint2(x, G_L1, G_R1, "Section");
}

static void Draw_List(void)
{
	cat_list_t *l = Cur_List();
	view_t *v = Cur_View();
	Draw_Header();
	gfx_round_rect(LIST_X - 12, LIST_Y - 8, LIST_W + 24, ROWS * ROW_H + 16, 14, C_PANEL);
	if (!l || !v) return;
	int count = View_Count(v, l);
	Clamp(v, count);		// (every path in: a section's view is never drawn out of range)
	if (!count && !v->search_wait)
		font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, LIST_Y + 180,
			!v->query[0] ? "Nothing here yet." : !strncmp(banner, "Search isn't", 12) ? banner :
			"Nothing matches the search. Press Square to clear it.", C_DIM, SCREEN_W - 200);
	for (int r = 0; r < ROWS && v->top + r < count; r++) {
		int row = v->top + r, pos = View_Pos(v, row);
		int y = LIST_Y + r * ROW_H;
		file_slot_t *slot;
		const cat_item_t *it = Item_At(l, pos, &slot);
		if (row == v->sel) gfx_round_rect(LIST_X - 4, y, LIST_W + 8, ROW_H - 4, 10, C_ROW_ON);
		int tx = LIST_X + 6 + ICON_LIST + 16, iy = y + (ROW_H - 4 - ICON_LIST) / 2;
		if (!it) {
			// its page isn't here yet: the title from the search file if we have it
			int len = 0;
			const char *line = Search_Line(l, pos, &len);
			char title[200] = "";
			if (line) {
				const char *tab = memchr(line, '\t', len);
				snprintf(title, sizeof(title), "%.*s", (int)(tab ? tab - line : len), line);
			}
			Placeholder(title[0] ? title : "?", LIST_X + 6, iy, ICON_LIST);
			if (title[0]) font_draw(FONT_MEDIUM, 22, tx, y + 8, title, C_TEXT, LIST_W - tx);
			const char *state = slot && slot->failed ? Error_Text(slot->err_kind, slot->err) : "Loading...";
			font_draw(FONT_REGULAR, 17, tx, title[0] ? y + 37 : y + 22, state, slot && slot->failed ? C_ERR : C_DIM, LIST_X + LIST_W - tx - 12);
			continue;
		}
		Icon_Draw(it, LIST_X + 6, iy, ICON_LIST);
		char size[32], sub[300];
		Fmt_Size(it->size, size, sizeof(size));
		int rx = LIST_X + LIST_W - 12;
		rx -= font_draw_right(FONT_MEDIUM, 19, rx, y + 22, size, C_DIM) + 14;
		if (store_downloaded(it)) Badge(&rx, y + 20, "Downloaded", C_OK);
		if (it->requires == REQ_CFW) Badge(&rx, y + 20, "CFW only", C_WARN);
		else if (it->requires == REQ_HEN) Badge(&rx, y + 20, "HEN / CFW", C_DIM);
		font_draw(FONT_MEDIUM, 22, tx, y + 8, it->title, C_TEXT, rx - tx - 10);
		snprintf(sub, sizeof(sub), "%s%s%s%s%s", it->author[0] ? it->author : "Unknown author",
			it->version[0] ? "  ·  v" : "", it->version, it->group[0] && it->type != TYPE_HOMEBREW ? "  ·  " : "",
			it->type != TYPE_HOMEBREW ? it->group : "");
		font_draw(FONT_REGULAR, 17, tx, y + 37, sub, C_DIM, rx - tx - 10);
	}
	// where in the list, and the letter
	if (count > ROWS) {
		int h = ROWS * ROW_H, bh = h * ROWS / count;
		if (bh < 24) bh = 24;
		int by = LIST_Y + (int)((long long)(h - bh) * v->sel / (count - 1));
		gfx_round_rect(LIST_X + LIST_W + 2, by, 4, bh, 2, 0x99ffffff);
	}
	int x = LIST_X;
	x = Hint(x, G_CROSS, "Open");
	x = Hint(x, G_TRIANGLE, "Search");
	if (v->query[0]) x = Hint(x, G_SQUARE, "Clear search");
	else if (ix->num_lists[cur] > 1) x = Hint(x, G_CIRCLE, "Categories");
	else x = Hint(x, G_CIRCLE, "Back");
	if (!v->results && count > 50) x = Hint2(x, G_L2, G_R2, "A-Z");
	x = Hint2(x, G_L1, G_R1, "Section");
	if (count) {
		char pos[64], letter = '#';
		int at = View_Pos(v, v->sel);
		for (int i = 0; i < NUM_LETTERS; i++)
			if (l->letter_first[i] >= 0 && l->letter_first[i] <= at) letter = i ? 'A' + i - 1 : '#';
		snprintf(pos, sizeof(pos), "%c   %d / %d", v->results ? ' ' : letter, v->sel + 1, count);
		font_draw_right(FONT_MEDIUM, 19, SCREEN_W - LIST_X, SCREEN_H - 55, pos, C_DIM);
	}
}

static void Draw_Detail(void)
{
	const cat_item_t *it = det;
	char line[400], size[32], dest[512];
	int x = LIST_X + ICON_BIG + 40, w = SCREEN_W - LIST_X - x;
	gfx_round_rect(LIST_X - 12, 60, SCREEN_W - 2 * LIST_X + 24, SCREEN_H - 150, 16, C_PANEL);
	Icon_Draw(it, LIST_X + 8, 84, ICON_BIG);
	int y = 84;
	font_draw(FONT_BOLD, 34, x, y, it->title, C_TEXT, w);
	y += 50;
	Fmt_Size(it->size, size, sizeof(size));
	snprintf(line, sizeof(line), "%s%s%s  ·  %s", it->author[0] ? it->author : "Unknown author",
		it->version[0] ? "  ·  v" : "", it->version, size);
	font_draw(FONT_MEDIUM, 21, x, y, line, C_DIM, w);
	y += 32;
	snprintf(line, sizeof(line), "%s%s%s", it->group, it->group[0] ? "  ·  " : "",
		it->requires == REQ_CFW ? "Needs CFW" : it->requires == REQ_HEN ? "Needs HEN or CFW" : "Any PS3");
	font_draw(FONT_REGULAR, 19, x, y, line, C_DIM, w);
	y += 42;
	if (it->description[0]) {
		char lines[5][256];
		int n = Wrap(FONT_REGULAR, 21, it->description, w, lines, 5);
		for (int i = 0; i < n; i++, y += 30)
			font_draw(FONT_REGULAR, 21, x, y, lines[i], C_TEXT, 0);
		y += 14;
	}
	int code = store_dest(it, dest, sizeof(dest));
	if (code == DEST_OK) snprintf(line, sizeof(line), "Saves to %s", dest);
	else snprintf(line, sizeof(line), "%s", store_dest_message(code));
	font_draw(FONT_REGULAR, 18, x, y, line, code == DEST_OK ? C_DIM : C_WARN, w);
	y += 30;
	const char *have = store_downloaded(it);
	if (have && !dl && !result_msg[0]) {
		snprintf(line, sizeof(line), "Downloaded: %s", have);
		font_draw(FONT_MEDIUM, 19, x, y, line, C_OK, w);
	}

	// the download, or how it went
	int by = SCREEN_H - 230;
	if (dl) {
		const char *what = dl->state == DL_CHECKING ? "Checking what's there already..." :
			dl->state == DL_VERIFYING ? "Checking the sha256..." : "Downloading";
		float frac = dl->size ? (float)dl->got / (float)dl->size : 0;
		char got[32], tot[32], spd[32];
		Fmt_Size(dl->got, got, sizeof(got));
		Fmt_Size(dl->size, tot, sizeof(tot));
		Fmt_Size((long long)dl_speed, spd, sizeof(spd));
		gfx_round_rect(x, by, w, 14, 7, 0x40ffffff);
		gfx_round_rect(x, by, (int)(w * frac), 14, 7, C_ACCENT);
		if (dl->state == DL_RUNNING)
			snprintf(line, sizeof(line), "%s  %d%%  ·  %s of %s  ·  %s/s", what, (int)(frac * 100), got, tot, spd);
		else
			snprintf(line, sizeof(line), "%s  %d%%  ·  %s of %s", what, (int)(frac * 100), got, tot);
		font_draw(FONT_MEDIUM, 20, x, by + 26, line, C_TEXT, w);
		if (dl->resumed_from) {
			Fmt_Size(dl->resumed_from, got, sizeof(got));
			snprintf(line, sizeof(line), "Resumed from %s", got);
			font_draw(FONT_REGULAR, 17, x, by + 56, line, C_DIM, w);
		}
	} else if (result_msg[0]) {
		char lines[4][256];
		int n = Wrap(FONT_MEDIUM, 20, result_msg, w, lines, 4);
		for (int i = 0; i < n; i++)
			font_draw(FONT_MEDIUM, 20, x, by + i * 28, lines[i], result_ok ? C_OK : C_ERR, 0);
	}

	int hx = LIST_X;
	if (dl) Hint(hx, G_CIRCLE, "Cancel download");
	else {
		hx = Hint(hx, G_CROSS, have || result_ok ? "Download again" : result_msg[0] && !result_ok ? "Try again" : "Download");
		Hint(hx, G_CIRCLE, "Back");
	}
}

static void Draw_Profile(void)
{
	font_draw_center(FONT_BOLD, 30, SCREEN_W / 2, 140, "Which user is the avatar for?", C_TEXT, 0);
	font_draw_center(FONT_REGULAR, 19, SCREEN_W / 2, 186, "It's saved in that user's psn_avatar folder.", C_DIM, 0);
	for (int i = 0; i < num_profiles; i++) {
		int y = 250 + i * 56;
		if (i == profile_sel) gfx_round_rect(SCREEN_W / 2 - 300, y - 8, 600, 48, 12, C_ROW_ON);
		char line[160];
		snprintf(line, sizeof(line), "%s   (%s)", profile_names[i], profiles[i]);
		font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, y, line, C_TEXT, 580);
	}
	int x = Hint(LIST_X, G_CROSS, "Choose");
	Hint(x, G_CIRCLE, "Back");
}

void ui_render(void)
{
	if (backdrop) gfx_image(backdrop, 0, 0, SCREEN_W, SCREEN_H, 0, 255);
	else gfx_clear(C_BG);
	switch (screen) {
	case SCR_START:
		if (logo) gfx_image(logo, (SCREEN_W - 360) / 2, 230, 360, 51, 0, 255);
		{
			char lines[4][256];
			int n = Wrap(FONT_MEDIUM, 24, start_msg, SCREEN_W - 300, lines, 4);
			for (int i = 0; i < n; i++)
				font_draw_center(FONT_MEDIUM, 24, SCREEN_W / 2, 330 + i * 34, lines[i], start_err ? C_ERR : C_TEXT, 0);
		}
		if (start_err) Hint(SCREEN_W / 2 - 90, G_CROSS, "Try again");
		break;
	case SCR_HOME: Draw_Home(); break;
	case SCR_LIST:
		if (Picking()) Draw_Picker();
		else Draw_List();
		break;
	case SCR_DETAIL: if (det) Draw_Detail(); break;
	case SCR_PROFILE: Draw_Profile(); break;
	}
	font_draw_right(FONT_REGULAR, 16, SCREEN_W - LIST_X, 40, APP_NAME " " APP_VERSION, C_DIM);
}

void ui_shutdown(void)
{
	// a download in progress keeps what's arrived: it resumes next time
	if (dl) dl->pause = 1;
	jobs_wait_lane(LANE_DL, 3000000);
}
