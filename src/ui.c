/*
ui.c: the screens.

  home     Homebrew, Themes and Avatars as cards, styled as ps3.world's home page
  picker   a section's categories: Homebrew's (Games, Ports, Tools...) as the site's
           cards; Themes' and Avatars' (All, then each category or region) as a grid
  list     one list: icon/title/author/version/size per row; Triangle searches
           (the system keyboard), L2/R2 jump a letter; L1/R1 change section.
           An avatar list's rows are packs: Cross opens one as a list (a folder)
  detail   the preview (a theme's 480x270, an avatar's 240x240; themes' rows show it
           small), the description, where it'll be saved, Download (X) with progress,
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
#include <time.h>
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
#include "lang.h"
#include "i18n.h"
#include "thirdparty/cJSON.h"

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
#define FAV_H		104			// the home screen's Favourites card, under the sections'
#define HOME_H		(CARD_H - FAV_H - CARD_GAP)	// the sections' cards
#define HOME_FAVS	NUM_TYPES	// home_sel: the Favourites card

enum { SCR_START, SCR_HOME, SCR_LIST, SCR_DETAIL, SCR_PROFILE, SCR_DOWNLOADS, SCR_LANG };

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
	image_t *img = image_decode_screen(data, (int)len, w, h);
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
		int len = 0, last_space = -1, cl = 1;
		while (s[len] && s[len] != '\n') {
			// a whole UTF-8 character at a time: a line never ends inside one
			for (cl = 1; (s[len + cl] & 0xc0) == 0x80; cl++);
			if (len + cl > 255) break;
			memcpy(line, s, len + cl);
			line[len + cl] = 0;
			if (font_width(font, px, line) > maxw) break;
			if (s[len] == ' ') last_space = len;
			len += cl;
		}
		int cut = len;
		if (s[len] && s[len] != '\n' && last_space > 0) cut = last_space;
		if (cut == 0) cut = len ? len : s[0] ? cl : 1;
		snprintf(lines[n++], 256, "%.*s", cut, s);
		s += cut;
		while (*s == ' ' || *s == '\n') s++;
	}
	if (*s && n) {		// more than fits: the last line ends in ...
		size_t l = strlen(lines[n - 1]);
		if (l > 3) {
			l -= 3;
			while (l && (lines[n - 1][l] & 0xc0) == 0x80) l--;		// (not inside a character)
			strcpy(lines[n - 1] + l, "...");
		}
	}
	return n;
}

// controller glyphs (Kenney's Input Prompts, CC0)
enum { G_CROSS, G_CIRCLE, G_SQUARE, G_TRIANGLE, G_L1, G_R1, G_L2, G_R2, G_DPAD_UD, G_SELECT, NUM_GLYPHS };
static const char *glyph_files[NUM_GLYPHS] = {"cross", "circle", "square", "triangle", "l1", "r1", "l2", "r2", "dpad_ud", "select"};
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

// L1, a row of pills (the one in use filled), R1: what L1/R1 move through. When they don't all
// fit before maxx, the ones around the one in use. Returns the x after R1.
#define TAB_H	34
static int Tabs(int x, int y, int maxx, const char *const *labels, int n, int sel)
{
	int w[32];
	if (n > 32) n = 32;
	if (n <= 0) return x;
	if (sel < 0) sel = 0;
	if (sel >= n) sel = n - 1;
	for (int i = 0; i < n; i++) w[i] = font_width(FONT_MEDIUM, 20, labels[i]) + 30;
	int room = maxx - x - 2 * (GLYPH + 12), a = sel, b = sel, used = w[sel];
	for (int grew = 1; grew; ) {
		grew = 0;
		if (b + 1 < n && used + 6 + w[b + 1] <= room) { used += 6 + w[++b]; grew = 1; }
		if (a > 0 && used + 6 + w[a - 1] <= room) { used += 6 + w[--a]; grew = 1; }
	}
	Glyph(G_L1, x, y + (TAB_H - GLYPH) / 2);
	x += GLYPH + 12;
	for (int i = a; i <= b; i++) {
		if (i == sel) gfx_round_rect(x, y, w[i], TAB_H, TAB_H / 2, C_ROW_ON);
		font_draw(FONT_MEDIUM, 20, x + 15, y + (TAB_H - 20) / 2, labels[i], i == sel ? C_TEXT : C_DIM, w[i] - 30);
		x += w[i] + 6;
	}
	Glyph(G_R1, x + 6, y + (TAB_H - GLYPH) / 2);
	return x + 6 + GLYPH;
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
	{"Edits", "cat-edits", "PS2 Classics", "Edits", "Modded PS2 classics: Budokai Tenkaichi 3, WWE 2K24, GTA Forelli Redemption and more."},
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

#define CHIP_H		36

static int Chip(int x, int y, const char *text)
{
	int w = font_width(FONT_MEDIUM, 18, text) + 32;
	gfx_round_rect(x, y, w, CHIP_H, CHIP_H / 2, 0x1fffffff);
	gfx_round_rect(x + 1, y + 1, w - 2, CHIP_H - 2, CHIP_H / 2 - 1, 0xbf05070d);
	font_draw(FONT_MEDIUM, 18, x + 16, y + (CHIP_H - 18) / 2, text, C_TEXT2, 0);	// (font_draw centres capitals in px)
	return w;
}

// font_draw centres the capitals in a box px high (font.c Ascent), Em 0.8 px and caps 0.7 Em:
// where the letters you see start and end below its y
static int Cap_Top(int px) { return px * 22 / 100; }
static int Cap_Bottom(int px) { return px * 78 / 100; }

static void Card(int x, int y, int w, int h, int on, const image_t *art, const char *kicker, const char *title,
	int title_px, const char *blurb, const char **chips, int num_chips)
{
	Card_Frame(x, y, w, h, on);
	if (art) gfx_image(art, x, y, w, h, 18, on ? 255 : 160);		// the site's 0.8 on hover, 0.5 off
	// spaced by the letters, not their boxes: 16 px from the kicker to the title, 20 to the text
	int tx = x + 28, ty = y + 24, chip_y = y + h - 26 - CHIP_H;
	if (kicker) {
		font_draw(FONT_BOLD, 20, tx, ty, tr(kicker), C_CYAN, w - 56);
		ty += Cap_Bottom(20) + 16 - Cap_Top(title_px);
	}
	font_draw(FONT_BOLD, title_px, tx, ty, tr(title), C_TEXT, w - 56);
	ty += Cap_Bottom(title_px) + 20 - Cap_Top(21);
	char lines[4][256];
	int max = (chip_y - 8 - ty) / 32;
	int bw = w * 2 / 3 < 340 ? w * 2 / 3 : 340;		// (the site's max-width: 34ch, about; the art shows to its right)
	int n = blurb && max > 0 ? Wrap(FONT_REGULAR, 21, tr(blurb), bw, lines, max > 4 ? 4 : max) : 0;
	for (int i = 0; i < n; i++)
		font_draw(FONT_REGULAR, 21, tx, ty + i * 32, lines[i], C_TEXT2, 0);
	// the chips that fit on one line
	int cx = tx;
	for (int i = 0; i < num_chips; i++) {
		if (cx + font_width(FONT_MEDIUM, 18, tr(chips[i])) + 32 > x + w - 28) break;
		cx += Chip(cx, chip_y, tr(chips[i])) + 8;
	}
}

// a small card: a category of themes or avatars, and how many
static void Tile(int x, int y, int w, int h, int on, const char *title, const char *count)
{
	Card_Frame(x, y, w, h, on);
	font_draw(FONT_BOLD, 26, x + 24, y + 22, tr(title), C_TEXT, w - 48);
	font_draw(FONT_MEDIUM, 19, x + 24, y + h - 44, count, on ? C_CYAN : C_TEXT2, w - 48);
}

static const char *Error_Text(int kind, const char *detail_msg)
{
	static char buf[300];
	switch (kind) {
	case ERR_NETWORK:
		return tr("No network connection. Check Settings > Network Settings on the PS3, then try again.");
	case ERR_TLS:
		return tr("The secure connection to dl.ps3.world failed. Check the PS3's date and time (Settings > Date and Time Settings), then try again.");
	case ERR_NOT_FOUND:
		return tr("Not found on the server (404). It may have moved: try again in a moment.");
	case ERR_DISK_FULL:
		snprintf(buf, sizeof(buf), tr("Not enough free space. %s"), detail_msg);
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
static int search_banner;			// (it's "Search isn't available")
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
static int fav_from = TYPE_HOMEBREW;	// the card Up goes back to from Favourites
static cat_list_t folder;			// an avatar pack opened from a list, shown as a list (pages NULL: none)
static view_t folder_view;

static cat_list_t *List(int t, int l) { return ix && l >= 0 && l < ix->num_lists[t] ? &ix->lists[t][l] : NULL; }
static int Picking(void) { return ix && ix->num_lists[cur] > 1 && cur_list[cur] < 0; }
static cat_list_t *Cur_List(void)
{
	if (folder.pages) return &folder;
	return Picking() ? NULL : List(cur, cur_list[cur] < 0 ? 0 : cur_list[cur]);
}
static view_t *Cur_View(void)
{
	if (folder.pages) return &folder_view;
	return Cur_List() && views[cur] ? &views[cur][cur_list[cur] < 0 ? 0 : cur_list[cur]] : NULL;
}

static void Folder_Close(void)
{
	free(folder_view.results);
	memset(&folder_view, 0, sizeof(folder_view));
	catalog_free_list(&folder);
}
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
	Folder_Close();		// (its pages may have changed)
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
		if (body) snprintf(j->err, sizeof(j->err), tr("The catalogue wasn't found on the server (HTTP %d)"), res.status);
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
		snprintf(start_msg, sizeof(start_msg), "%s", tr("Loading the catalogue..."));
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
			snprintf(banner, sizeof(banner), tr("Offline: showing the catalogue from %.10s"), ix->generated);
			search_banner = 0;
		} else {
			snprintf(start_msg, sizeof(start_msg), "%s", Error_Text(fetch_job->ival, fetch_job->err));
			start_err = 1;
		}
	}
	job_release(fetch_job);
	fetch_job = NULL;
	dirty = 1;
}

// "Search isn't available" goes with the search it was about (an offline banner stays)
static void Search_Banner_Clear(void)
{
	if (search_banner) banner[0] = search_banner = 0;
}

// the search, once the list's search.txt is here
static void Search_Run(view_t *v, const cat_list_t *l)
{
	file_slot_t *s = File_Get(&l->search, 1);
	if (!s || !s->lines) {
		v->search_wait = !(s && s->failed);
		if (s && s->failed) {
			// no search file (offline and never fetched): say so, and match nothing
			snprintf(banner, sizeof(banner), tr("Search isn't available: %s"), Error_Text(s->err_kind, s->err));
			search_banner = 1;
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
Icons and previews: from the HDD cache, or fetched once and cached
==================
*/
#define ICON_SLOTS		96
#define ICON_INFLIGHT	12

typedef struct {
	char id[64];
	int w, h, preview;
	image_t *img;
	job_t *job;
	int failed;
	uint64_t used, failed_at;
} icon_slot_t;
#define ART_RETRY_US	30000000		// art that failed is asked for again after this
static icon_slot_t icons[ICON_SLOTS];
static int icons_inflight;

typedef struct {
	char url[1100];
	char path[600];
	int w, h;
} icon_req_t;

static void Icon_Run(job_t *j)
{
	icon_req_t *r = j->arg;
	size_t len = 0;
	unsigned char *data = plat_read_file(r->path, &len);
	if (data) {
		j->result = image_decode_screen(data, (int)len, r->w, r->h);
		free(data);
		if (j->result) return;
		remove(r->path);		// damaged in the cache: fetch it again
	}
	http_res_t res;
	char *body = http_fetch(NULL, r->url, NULL, NULL, 20, &j->cancel, &len, &res);
	if (body && res.status == 200) {
		j->result = image_decode_screen(body, (int)len, r->w, r->h);
		if (j->result) plat_write_file(r->path, body, len);		// only what decodes is kept
	}
	free(body);
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
		s->failed_at = t_now;
		job_release(s->job);
		s->job = NULL;
		icons_inflight--;
		dirty = 1;
	}
}

// an item's icon, or its preview, at w x h: NULL until it's here (*failed: it won't be)
static image_t *Art_Get(const cat_item_t *it, int preview, int w, int h, int *failed)
{
	int lru = -1;
	const char *rel = preview ? it->preview : it->icon;
	if (failed) *failed = !rel[0];
	if (!rel[0]) return NULL;
	for (int i = 0; i < ICON_SLOTS; i++) {
		icon_slot_t *s = &icons[i];
		if (s->w == w && s->h == h && s->preview == preview && !strcmp(s->id, it->id)) {
			s->used = t_now;
			if (!s->failed || s->job || t_now - s->failed_at < ART_RETRY_US) {
				if (failed) *failed = s->failed;
				return s->img;
			}
			lru = i;		// failed a while ago: try again in the same slot
			break;
		}
		if (!s->job && (lru < 0 || s->used < icons[lru].used)) lru = i;
	}
	if (lru < 0 || icons_inflight >= ICON_INFLIGHT) return NULL;
	icon_req_t *r = calloc(1, sizeof(*r));
	job_t *j = r ? job_new(Icon_Run, Icon_Cleanup) : NULL;
	if (!j) { free(r); return NULL; }
	snprintf(r->url, sizeof(r->url), "%s%s", CATALOG_BASE, rel);
	if (preview) store_preview_path(it, r->path, sizeof(r->path));
	else store_icon_path(it, r->path, sizeof(r->path));
	r->w = w;
	r->h = h;
	j->arg = r;
	icon_slot_t *s = &icons[lru];
	image_free(s->img);
	memset(s, 0, sizeof(*s));
	snprintf(s->id, sizeof(s->id), "%s", it->id);
	s->w = w;
	s->h = h;
	s->preview = preview;
	s->used = t_now;
	s->job = j;
	icons_inflight++;
	job_post(j, LANE_ART);
	return NULL;
}

static image_t *Icon_Get(const cat_item_t *it, int size) { return Art_Get(it, 0, size, size, NULL); }

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

// a theme's row: its preview, small and 16:9 (a square icon letterboxes it to nothing)
#define THUMB_W		96
#define THUMB_H		54

static void Thumb_Draw(const cat_item_t *it, const char *title, int x, int y)
{
	image_t *img = it ? Art_Get(it, 1, THUMB_W, THUMB_H, NULL) : NULL;
	if (img) {
		gfx_image(img, x, y, THUMB_W, THUMB_H, 6, 255);
		return;
	}
	gfx_round_rect(x, y, THUMB_W, THUMB_H, 6, 0xff1a1c20);
	char ch[2] = {Letter_Of(title), 0};
	font_draw_center(FONT_BOLD, 26, x + THUMB_W / 2, y + 12, ch, 0xff3a4152, 0);
}

/*
==================
Downloads: a queue, one at a time. Each entry keeps how it went (a finished
one is dropped when the queue needs its place) for its detail screen.
==================
*/
#define QUEUE_MAX	128		// (a big avatar pack fits)
enum { Q_WAITING, Q_RUNNING, Q_DONE, Q_FAILED };

typedef struct {
	cat_item_t *it;				// a copy
	char dest[512];
	int state, ok;
	char msg[400];				// (Q_DONE, Q_FAILED) what happened
} queued_t;

static queued_t queue[QUEUE_MAX];
static int queue_n;
static cat_item_t *det;			// the detail screen's item (a copy: its page may go)
static download_t *dl;			// the running one, queue[dl_q]
static job_t *dl_job;
static int dl_q = -1;
static uint64_t dl_t_last;
static long long dl_last_got;
static double dl_speed;			// bytes a second, smoothed
static char result_msg[400];		// why the detail screen's item can't be queued (no profile...)
static int result_ok;
static int det_back = SCR_LIST;		// where Circle goes from the detail screen
static int preview_full;			// the detail screen's preview, filling the screen (Triangle)
static int big_asked;				// Cross pressed once on a big download: the second press starts it
static uint64_t dl_started;			// when the running download started (for the speed it got)
static double speed_seen;			// bytes a second, from finished downloads (USRDIR/speed.txt)
#define BIG_BYTES	(1024LL * 1024 * 1024)

// "about 40 minutes" at the speed downloads have been getting; "" if none's finished yet
static void Fmt_Time(long long bytes, char *out, int size)
{
	out[0] = 0;
	if (speed_seen <= 0) return;
	long long min = (long long)(bytes / speed_seen / 60 + 0.5);
	if (min < 1) snprintf(out, size, "%s", tr("under a minute"));
	else if (min < 90) snprintf(out, size, tr("about %d minutes"), (int)min);
	else snprintf(out, size, tr("about %d hours %d minutes"), (int)(min / 60), (int)(min % 60));
}

static void Speed_Load(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/speed.txt", plat_save_dir());
	char *t = (char *)plat_read_file(path, NULL);
	if (t) { speed_seen = atof(t); free(t); }
	if (!(speed_seen > 0 && speed_seen < 1e9)) speed_seen = 0;
}

static void Speed_Save(void)
{
	char path[512], buf[32];
	snprintf(path, sizeof(path), "%s/speed.txt", plat_save_dir());
	int n = snprintf(buf, sizeof(buf), "%.0f\n", speed_seen);
	plat_write_file(path, buf, n);
}
static long long free_bytes = -1;	// on /dev_hdd0, read every few seconds (home and My downloads)
static uint64_t free_checked;

static void Dl_Run(job_t *j) { download_run(j->arg); }
static void Profiles_Load(void);

// the newest entry for an item, or -1
static int Queue_Find(const char *id)
{
	for (int i = queue_n - 1; i >= 0; i--)
		if (!strcmp(queue[i].it->id, id)) return i;
	return -1;
}

// waiting or running
static int Queue_Active(int i) { return i >= 0 && i < queue_n && queue[i].state <= Q_RUNNING; }
static int Queue_Busy(void)
{
	int n = 0;
	for (int i = 0; i < queue_n; i++) n += Queue_Active(i);
	return n;
}

static void Queue_Remove(int i)
{
	free(queue[i].it);
	memmove(&queue[i], &queue[i + 1], (queue_n - 1 - i) * sizeof(queued_t));
	queue_n--;
	if (dl_q > i) dl_q--;
}

// the first waiting one, if nothing's running
static void Queue_Next(void)
{
	while (!dl) {
		int i;
		for (i = 0; i < queue_n && queue[i].state != Q_WAITING; i++);
		if (i == queue_n) return;
		queued_t *q = &queue[i];
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", q->dest);
		char *slash = strrchr(dir, '/');
		if (slash) *slash = 0;
		if (!plat_mkdirs(dir)) {
			snprintf(q->msg, sizeof(q->msg), tr("Couldn't make the folder %s"), dir);
			q->state = Q_FAILED;
			continue;
		}
		download_t *d = calloc(1, sizeof(*d));
		job_t *j = d ? job_new(Dl_Run, NULL) : NULL;
		if (!j) {
			free(d);
			snprintf(q->msg, sizeof(q->msg), "%s", tr("Out of memory"));
			q->state = Q_FAILED;
			continue;
		}
		snprintf(d->url, sizeof(d->url), "%s%s", CATALOG_BASE, q->it->url);
		snprintf(d->dest, sizeof(d->dest), "%s", q->dest);
		d->size = q->it->size;
		memcpy(d->sha256, q->it->sha256, 32);
		j->arg = d;
		dl = d;
		dl_job = j;
		dl_q = i;
		q->state = Q_RUNNING;
		dl_last_got = 0;
		dl_speed = 0;
		dl_t_last = t_now;
		dl_started = t_now;
		job_post(j, LANE_DL);
		plat_log("download: %s -> %s", d->url, d->dest);
	}
	dirty = 1;
}

// the item at the end of the queue (if it isn't in it already)
static void Queue_Add(const cat_item_t *it, const char *dest)
{
	int i = Queue_Find(it->id);
	if (Queue_Active(i)) return;
	if (i >= 0) Queue_Remove(i);		// (how it went last time)
	for (i = 0; queue_n == QUEUE_MAX && i < queue_n; i++)		// room: the oldest finished one goes
		if (!Queue_Active(i)) { Queue_Remove(i); break; }
	if (queue_n == QUEUE_MAX) {
		snprintf(result_msg, sizeof(result_msg), tr("%d downloads are waiting already: add this one when some have finished."), QUEUE_MAX);
		result_ok = 0;
		return;
	}
	queued_t *q = &queue[queue_n];
	memset(q, 0, sizeof(*q));
	if (!(q->it = catalog_item_copy(it))) return;
	snprintf(q->dest, sizeof(q->dest), "%s", dest);
	q->state = Q_WAITING;
	queue_n++;
	Queue_Next();
}

// a waiting one leaves the queue; the running one stops (and what's arrived is deleted)
static void Queue_Cancel(int i)
{
	if (!Queue_Active(i)) return;
	if (queue[i].state == Q_RUNNING) dl->cancel = 1;
	else Queue_Remove(i);
	dirty = 1;
}

// how many are ahead of it
static int Queue_Ahead(int i)
{
	int n = 0;
	for (int k = 0; k < i; k++) n += Queue_Active(k);
	return n;
}

// the detail screen's item into the queue, once it's known where it goes
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
	result_msg[0] = 0;
	if (code != DEST_OK) {
		snprintf(result_msg, sizeof(result_msg), "%s", tr(store_dest_message(code)));
		result_ok = 0;
		dirty = 1;
		return;
	}
	// room for it, and for what's queued before it
	char dir[512], need_s[32], free_s[32], when[64];
	snprintf(dir, sizeof(dir), "%s", dest);
	char *slash = strrchr(dir, '/');
	if (slash) *slash = 0;
	long long need = det->size, fb = plat_free_bytes(dir[0] ? dir : "/dev_hdd0");
	for (int i = 0; i < queue_n; i++)
		if (Queue_Active(i)) need += queue[i].it->size - (i == dl_q && dl ? dl->got : 0);
	if (fb >= 0 && need > fb) {
		Fmt_Size(need, need_s, sizeof(need_s));
		Fmt_Size(fb, free_s, sizeof(free_s));
		snprintf(result_msg, sizeof(result_msg), tr("Not enough free space: this needs %s, and %s is free."), need_s, free_s);
		result_ok = 0;
		dirty = 1;
		return;
	}
	// a big one: say how big, and how long, and start it on the second press
	if (det->size >= BIG_BYTES && !big_asked) {
		Fmt_Size(det->size, need_s, sizeof(need_s));
		Fmt_Time(det->size, when, sizeof(when));
		if (when[0]) snprintf(result_msg, sizeof(result_msg), tr("This is %s (%s). Press X again to download it."), need_s, when);
		else snprintf(result_msg, sizeof(result_msg), tr("This is %s. Press X again to download it."), need_s);
		result_ok = 2;
		big_asked = 1;
		dirty = 1;
		return;
	}
	big_asked = 0;
	result_msg[0] = 0;
	Queue_Add(det, dest);
	dirty = 1;
}

static void Detail_Open(const cat_item_t *it, int back)
{
	cat_item_t *c = catalog_item_copy(it);
	if (!c) return;
	free(det);
	det = c;
	result_msg[0] = 0;		// (the last item's says nothing about this one)
	result_ok = 0;
	det_back = back;
	preview_full = 0;
	big_asked = 0;
	screen = SCR_DETAIL;
	dirty = 1;
}

// a message in the header that goes with the next move (a pack queued, a file that wouldn't delete)
static void Note(const char *msg)
{
	snprintf(banner, sizeof(banner), "%s", msg);
	search_banner = 1;
	dirty = 1;
}

/*
==================
Favourites: items starred on the detail screen (Select), kept in USRDIR/favourites.json
as a catalogue page, so they open and download without their own pages
==================
*/
static catalog_t *favs;

static void Favs_Load(void)
{
	char path[512], err[160];
	size_t len;
	snprintf(path, sizeof(path), "%s/favourites.json", plat_save_dir());
	char *t = (char *)plat_read_file(path, &len);
	if (!t) return;
	catalog_t *c = catalog_parse_page(t, len, err, sizeof(err));
	free(t);
	if (c) { catalog_free(favs); favs = c; }
	else plat_log("favourites: %s", err);
}

static int Fav_Find(const char *id)
{
	for (int i = 0; favs && i < favs->count; i++)
		if (!strcmp(favs->items[i].id, id)) return i;
	return -1;
}

static cJSON *Fav_Json(const cat_item_t *it)
{
	cJSON *o = cJSON_CreateObject();
	char sha[65];
	for (int i = 0; i < 32; i++) snprintf(sha + i * 2, 3, "%02x", it->sha256[i]);
	cJSON_AddStringToObject(o, "id", it->id);
	cJSON_AddStringToObject(o, "type", catalog_type_name(it->type));
	cJSON_AddStringToObject(o, "title", it->title);
	cJSON_AddStringToObject(o, "author", it->author);
	cJSON_AddStringToObject(o, "version", it->version);
	cJSON_AddStringToObject(o, "description", it->description);
	cJSON_AddStringToObject(o, "group", it->group);
	cJSON_AddStringToObject(o, "url", it->url);
	cJSON_AddStringToObject(o, "icon", it->icon);
	cJSON_AddStringToObject(o, "preview", it->preview);
	cJSON_AddNumberToObject(o, "size", (double)it->size);
	cJSON_AddStringToObject(o, "sha256", sha);
	cJSON_AddStringToObject(o, "requires", it->requires == REQ_CFW ? "cfw" : it->requires == REQ_HEN ? "hen" : "any");
	return o;
}

// adds it (first), or takes it off if it's there; saved, then read back as the page it's kept as
static void Fav_Toggle(const cat_item_t *it)
{
	int had = Fav_Find(it->id);
	cJSON *root = cJSON_CreateObject(), *items = cJSON_CreateArray();
	if (!root || !items) { cJSON_Delete(root); cJSON_Delete(items); return; }
	cJSON_AddNumberToObject(root, "schema", CATALOG_SCHEMA);
	cJSON_AddItemToObject(root, "items", items);
	if (had < 0) cJSON_AddItemToArray(items, Fav_Json(it));
	for (int i = 0; favs && i < favs->count; i++)
		if (i != had) cJSON_AddItemToArray(items, Fav_Json(&favs->items[i]));
	char *t = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!t) return;
	char path[512], err[160];
	snprintf(path, sizeof(path), "%s/favourites.json", plat_save_dir());
	if (!plat_write_file(path, t, strlen(t))) plat_log("favourites: couldn't write %s", path);
	catalog_t *c = catalog_parse_page(t, strlen(t), err, sizeof(err));
	free(t);
	catalog_free(favs);
	favs = c;
	dirty = 1;
}

/*
==================
A whole avatar pack: its pages fetched, then each avatar not already here queued
==================
*/
static cat_list_t pack;				// the pack being queued (pages NULL: none)
static int pack_profile, pack_back;	// the profile screen is up for it; the screen it came from

static void Pack_End(void)
{
	catalog_free_list(&pack);
	pack_profile = 0;
}

static void Pack_Start(const cat_list_t *l)
{
	Pack_End();
	cat_file_t *pages = malloc((l->num_pages ? l->num_pages : 1) * sizeof(cat_file_t));
	if (!pages) return;
	memcpy(pages, l->pages, l->num_pages * sizeof(cat_file_t));
	pack = *l;
	pack.pages = pages;
	Note(tr("Getting the pack's avatars..."));
}

static void Pack_Tick(void)
{
	if (!pack.pages || pack_profile) return;
	for (int p = 0; p < pack.num_pages; p++) {
		file_slot_t *fs = File_Get(&pack.pages[p], 0);
		if (fs && fs->failed) { Note(Error_Text(fs->err_kind, fs->err)); Pack_End(); return; }
		if (!fs || !fs->page) return;
	}
	char dest[512], msg[200];
	int added = 0;
	for (int p = 0; p < pack.num_pages; p++) {
		file_slot_t *fs = File_Get(&pack.pages[p], 0);
		if (!fs || !fs->page) return;
		for (int k = 0; k < fs->page->count; k++) {
			const cat_item_t *it = &fs->page->items[k];
			if (store_downloaded(it) || Queue_Active(Queue_Find(it->id))) continue;
			int code = store_dest(it, dest, sizeof(dest));
			if (code == DEST_PICK_PROFILE) {		// which user first: then on from here
				Profiles_Load();
				pack_back = screen;
				pack_profile = 1;
				screen = SCR_PROFILE;
				dirty = 1;
				return;
			}
			if (code != DEST_OK) { Note(tr(store_dest_message(code))); Pack_End(); return; }
			Queue_Add(it, dest);
			if (!Queue_Active(Queue_Find(it->id))) {		// the queue's full
				snprintf(msg, sizeof(msg), tr("%d avatars queued: the queue is full, so add the rest later."), added);
				Note(msg);
				Pack_End();
				return;
			}
			added++;
		}
	}
	if (added) snprintf(msg, sizeof(msg), tr("%d avatars queued: they download one after another."), added);
	else snprintf(msg, sizeof(msg), "%s", tr("Every avatar in this pack is downloaded or queued already."));
	Note(msg);
	Pack_End();
}

static const char *Install_Hint(int type)
{
	switch (type) {
	case TYPE_HOMEBREW: return tr("Install it from the XMB: Package Manager > Install Package Files.");
	case TYPE_THEME: return tr("Choose it on the XMB: Settings > Theme Settings > Theme.");
	case TYPE_AVATAR: return tr("Choose it from the XMB: PlayStation Network > Account Management > Avatar.");
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
	queued_t *q = &queue[dl_q];
	q->ok = 0;
	q->state = Q_FAILED;
	switch (dl->state) {
	case DL_DONE: {
		char size[32];
		Fmt_Size(dl->size, size, sizeof(size));
		store_mark_downloaded(q->it, dl->dest);
		// the speed it got (a long enough one), for the next big download's estimate
		double secs = (t_now - dl_started) / 1e6, got = (double)(dl->size - dl->resumed_from);
		if (secs > 10 && got > 5 * 1024 * 1024) {
			speed_seen = speed_seen > 0 ? speed_seen * 0.5 + got / secs * 0.5 : got / secs;
			Speed_Save();
		}
		snprintf(q->msg, sizeof(q->msg), tr("Checksum OK (%s). Saved to %s\n%s"), size, dl->dest, Install_Hint(q->it->type));
		q->ok = 1;
		q->state = Q_DONE;
		break;
	}
	case DL_CANCELLED:
		snprintf(q->msg, sizeof(q->msg), "%s", tr("Download cancelled."));
		break;
	case DL_PAUSED:
		snprintf(q->msg, sizeof(q->msg), "%s", tr("Download paused. Press X to resume."));
		break;
	default:
		snprintf(q->msg, sizeof(q->msg), "%s%s",
			dl->err_kind == ERR_CHECKSUM ? tr("The download didn't match its checksum, so it was deleted. Try again.") :
			Error_Text(dl->err_kind, dl->err),
			dl->err_kind == ERR_NETWORK ? tr("\nPress X to resume where it stopped.") : "");
	}
	job_release(dl_job);
	dl_job = NULL;
	free(dl);
	dl = NULL;
	dl_q = -1;
	free_checked = 0;		// (the space it took)
	Queue_Next();
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
My downloads: the queue (what hasn't finished), then what's on the HDD, newest first
==================
*/
static int dls_sel, dls_top, dls_confirm = -1;		// dls_confirm: the row Square was pressed on once
static int dls_tab;			// 0 the downloads, 1 the favourites (L1/R1)

static int Dls_Queue_Rows(void)
{
	int n = 0;
	for (int i = 0; i < queue_n; i++) n += queue[i].state != Q_DONE;
	return n;
}

// a queue row's entry
static int Dls_Queue_At(int row)
{
	for (int i = 0; i < queue_n; i++)
		if (queue[i].state != Q_DONE && row-- == 0) return i;
	return -1;
}

static int Dls_Count(void) { return Dls_Queue_Rows() + store_num_downloads(); }

// a homebrew item in the catalogue by id (its pages asked for as needed): NULL if it isn't (yet)
static const cat_item_t *Homebrew_Find(const char *id)
{
	if (strncmp(id, "hb-", 3)) return NULL;		// (only homebrew gets new versions)
	for (int l = 0; ix && l < ix->num_lists[TYPE_HOMEBREW]; l++)
		for (int p = 0; p < ix->lists[TYPE_HOMEBREW][l].num_pages; p++) {
			file_slot_t *s = File_Get(&ix->lists[TYPE_HOMEBREW][l].pages[p], 0);
			if (!s || !s->page) continue;
			for (int k = 0; k < s->page->count; k++)
				if (!strcmp(s->page->items[k].id, id)) return &s->page->items[k];
		}
	return NULL;
}

// the catalogue has a different file for a download (a new version)
static int Dls_Newer(const store_rec_t *r)
{
	const cat_item_t *it = Homebrew_Find(r->id);
	char sha[65];
	if (!it) return 0;
	for (int i = 0; i < 32; i++) snprintf(sha + i * 2, 3, "%02x", it->sha256[i]);
	return strcmp(sha, r->sha) != 0;
}

static void Input_Downloads(uint32_t p)
{
	int n = Dls_Count(), qrows = Dls_Queue_Rows();
	if (p & BTN_CIRCLE) {
		dls_confirm = -1;
		Search_Banner_Clear();
		screen = SCR_HOME;
		dirty = 1;
		return;
	}
	if (p & (BTN_L1 | BTN_R1)) {
		dls_tab ^= 1;
		dls_sel = dls_top = 0;
		dls_confirm = -1;
		dirty = 1;
		return;
	}
	if (dls_tab == 1) {		// favourites: Cross opens one, Square takes it off
		n = favs ? favs->count : 0;
		if (p & BTN_DOWN) dls_sel++;
		if (p & BTN_UP) dls_sel--;
		if (p & BTN_RIGHT) dls_sel += ROWS;
		if (p & BTN_LEFT) dls_sel -= ROWS;
		if (dls_sel >= n) dls_sel = n - 1;
		if (dls_sel < 0) dls_sel = 0;
		if ((p & BTN_CROSS) && n) Detail_Open(&favs->items[dls_sel], SCR_DOWNLOADS);
		else if ((p & BTN_SQUARE) && n) Fav_Toggle(&favs->items[dls_sel]);
		n = favs ? favs->count : 0;
		if (dls_sel >= n) dls_sel = n - 1;
		if (dls_sel < 0) dls_sel = 0;
		if (dls_top > dls_sel) dls_top = dls_sel;
		if (dls_top < dls_sel - ROWS + 1) dls_top = dls_sel - ROWS + 1;
		if (p) dirty = 1;
		return;
	}
	if (p & ~BTN_SQUARE) dls_confirm = -1;		// (anything else and the delete's off)
	if (p & BTN_DOWN) dls_sel++;
	if (p & BTN_UP) dls_sel--;
	if (p & BTN_RIGHT) dls_sel += ROWS;
	if (p & BTN_LEFT) dls_sel -= ROWS;
	if (dls_sel >= n) dls_sel = n - 1;
	if (dls_sel < 0) dls_sel = 0;
	if (p & BTN_CROSS) {
		store_rec_t r;
		const cat_item_t *it;
		if (dls_sel < qrows) Detail_Open(queue[Dls_Queue_At(dls_sel)].it, SCR_DOWNLOADS);
		else if (store_download_at(dls_sel - qrows, &r) && (it = Homebrew_Find(r.id)))
			Detail_Open(it, SCR_DOWNLOADS);
	}
	if ((p & BTN_SQUARE) && dls_sel < qrows) {
		int q = Dls_Queue_At(dls_sel);
		if (Queue_Active(q)) Queue_Cancel(q);
		else if (q >= 0) Queue_Remove(q);		// a failed one: off the list
	} else if ((p & BTN_SQUARE) && dls_sel < n) {
		// twice to delete: the first press asks
		if (dls_confirm == dls_sel) {
			if (!store_delete_download(dls_sel - qrows))
				Note(tr("Couldn't delete that file"));
			dls_confirm = -1;
			free_checked = 0;
		} else
			dls_confirm = dls_sel;
	}
	n = Dls_Count();
	if (dls_sel >= n) dls_sel = n - 1;
	if (dls_sel < 0) dls_sel = 0;
	if (dls_top > dls_sel) dls_top = dls_sel;
	if (dls_top < dls_sel - ROWS + 1) dls_top = dls_sel - ROWS + 1;
	if (p) dirty = 1;
}

/*
==================
The app's own update, from the catalogue
==================
*/
static int Version_Newer(const char *a, const char *b)
{
	int x[3] = {0}, y[3] = {0};
	sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
	sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
	for (int i = 0; i < 3; i++)
		if (x[i] != y[i]) return x[i] > y[i];
	return 0;
}

static int Update_Available(void) { return ix && ix->app.version[0] && Version_Newer(ix->app.version, APP_VERSION); }

// the update, as a homebrew item on the detail screen
static void Update_Open(void)
{
	char title[64];
	snprintf(title, sizeof(title), APP_NAME " %s", ix->app.version);
	cat_item_t it;
	memset(&it, 0, sizeof(it));
	it.id = "ps3world-app";
	it.title = title;
	it.author = "PS3 World";
	it.version = ix->app.version;
	it.description = tr("A new version of this app. Once it's downloaded, install it from Package Manager > Install Package Files: "
		"it installs over this one.");
	it.group = it.icon = it.preview = it.list = "";
	it.url = ix->app.file.url;
	it.size = ix->app.size;
	memcpy(it.sha256, ix->app.file.sha256, 32);
	it.type = TYPE_HOMEBREW;
	it.requires = REQ_HEN;
	Detail_Open(&it, SCR_HOME);
}

/*
==================
Input
==================
*/
static int searching;
static int lang_sel;		// the language screen's row: 0 the PS3's, then LANG_* + 1

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

// Homebrew's few categories are big cards, three a row; Themes' and Avatars' are tiles, TILE_COLS a row
static int Picker_Cols(void) { return cur == TYPE_HOMEBREW ? 3 : TILE_COLS; }
static int Picker_Rows_Shown(void) { return cur == TYPE_HOMEBREW ? 99 : (CARD_H + CARD_GAP) / (TILE_H + CARD_GAP); }

static void Open_Section(int t)
{
	Search_Banner_Clear();
	Folder_Close();
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

// in a list of a section with categories: the next one (round from the last to the first)
static void List_Step(int dir)
{
	int n = ix->num_lists[cur];
	Search_Banner_Clear();
	Folder_Close();
	cur_list[cur] = (cur_list[cur] + n + dir) % n;
	dirty = 1;
}

static void Input_Home(uint32_t p)
{
	// Homebrew on the left, Themes over Avatars on the right
	// Favourites along the bottom
	if (home_sel == HOME_FAVS) { if (p & BTN_UP) home_sel = fav_from; }
	else if ((p & BTN_RIGHT) && home_sel == TYPE_HOMEBREW) home_sel = TYPE_THEME;
	else if (p & BTN_LEFT) home_sel = TYPE_HOMEBREW;
	else if ((p & BTN_DOWN) && home_sel == TYPE_THEME) home_sel = TYPE_AVATAR;
	else if ((p & BTN_UP) && home_sel == TYPE_AVATAR) home_sel = TYPE_THEME;
	else if (p & BTN_DOWN) { fav_from = home_sel; home_sel = HOME_FAVS; }
	if ((p & BTN_CROSS) && home_sel == HOME_FAVS) {
		dls_tab = 1;
		dls_sel = dls_top = 0;
		dls_confirm = -1;
		screen = SCR_DOWNLOADS;
	} else if (p & BTN_CROSS) Open_Section(home_sel);
	if (p & BTN_TRIANGLE) {
		dls_sel = dls_top = 0;
		dls_confirm = -1;
		screen = SCR_DOWNLOADS;
	}
	if ((p & BTN_SQUARE) && Update_Available()) Update_Open();
	if (p & BTN_SELECT) {
		lang_sel = i18n_choice() + 1;
		screen = SCR_LANG;
	}
	if (p) dirty = 1;
}

/*
==================
Language: the PS3's (the first row), or one of ours
==================
*/
static void Input_Lang(uint32_t p)
{
	if (p & BTN_DOWN && lang_sel < LANG_COUNT) lang_sel++;
	if (p & BTN_UP && lang_sel > 0) lang_sel--;
	if (p & BTN_CIRCLE) screen = SCR_HOME;
	if (p & BTN_CROSS) {
		i18n_set(lang_sel - 1);
		font_flush();		// (another language may draw characters from other fonts)
		screen = SCR_HOME;
	}
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
	// L1/R1: the section's categories in a list (its sections from the categories screen)
	int step = (p & BTN_R1) ? 1 : (p & BTN_L1) ? -1 : 0;
	if (step && l && ix->num_lists[cur] > 1) { List_Step(step); return; }
	if (step) { Section_Step(step); return; }
	if (!l || !v) {		// an empty section: only the way out
		if (p & BTN_CIRCLE) {
			home_sel = cur;
			screen = SCR_HOME;
			dirty = 1;
		}
		return;
	}
	int count = View_Count(v, l);
	if (p & BTN_DOWN) v->sel++;
	if (p & BTN_UP) v->sel--;
	if (p & BTN_RIGHT) v->sel += ROWS;
	if (p & BTN_LEFT) v->sel -= ROWS;
	if (!v->results && (p & BTN_R2)) Letter_Jump(v, l, 1);
	if (!v->results && (p & BTN_L2)) Letter_Jump(v, l, -1);
	if (p & BTN_TRIANGLE) {
		char title[64];
		snprintf(title, sizeof(title), "%s %s%s%s", tr("Search"), tr(section_names[cur]), l->name[0] ? ": " : "", tr(l->name));
		if (plat_osk_open(title, v->query)) searching = 1;
	}
	if ((p & (BTN_SQUARE | BTN_CIRCLE)) && (v->query[0] || v->search_wait)) {
		Search_Banner_Clear();
		v->query[0] = 0;
		v->search_wait = 0;
		free(v->results);
		v->results = NULL;
		v->sel = v->top = 0;
		dirty = 1;
		return;
	}
	if ((p & BTN_SQUARE) && cur == TYPE_AVATAR) {		// a whole pack into the queue
		if (folder.pages) Pack_Start(&folder);
		else {
			file_slot_t *slot;
			const cat_item_t *it = Item_At(l, View_Pos(v, v->sel), &slot);
			cat_list_t f;
			if (it && it->list[0] && catalog_parse_list(it->list, &f)) {
				Pack_Start(&f);
				catalog_free_list(&f);
			}
		}
		dirty = 1;
		return;
	}
	if ((p & BTN_CIRCLE) && folder.pages) {		// out of the pack, back to the packs
		Folder_Close();
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
		if (it && it->list[0]) {		// a pack: its avatars, as a list
			cat_list_t f;
			if (catalog_parse_list(it->list, &f)) {
				Folder_Close();
				folder = f;
			} else
				plat_log("catalog: %s's folder didn't check out", it->id);
			dirty = 1;
			return;
		}
		if (it) Detail_Open(it, SCR_LIST);
	}
	Clamp(v, View_Count(v, l));
	if (p) dirty = 1;
}

static void Leave_Detail(void)
{
	free(det);
	det = NULL;
	screen = det_back;
	if (pending) {
		Index_Use(pending);
		pending = NULL;
	}
	dirty = 1;
}

static void Input_Detail(uint32_t p)
{
	int q = det ? Queue_Find(det->id) : -1;
	if (preview_full) {
		if (p & (BTN_CIRCLE | BTN_TRIANGLE | BTN_CROSS)) preview_full = 0;
		if (p) dirty = 1;
		return;
	}
	if ((p & BTN_TRIANGLE) && det && det->preview[0]) { preview_full = 1; dirty = 1; return; }
	if (p & BTN_CIRCLE) { Leave_Detail(); return; }		// (a download carries on)
	if ((p & BTN_SELECT) && det && strcmp(det->id, "ps3world-app")) Fav_Toggle(det);
	if ((p & BTN_SQUARE) && Queue_Active(q)) Queue_Cancel(q);
	else if ((p & BTN_CROSS) && det && !Queue_Active(q)) Download_Start();
	if (p) dirty = 1;
}

static void Input_Profile(uint32_t p)
{
	if (p & BTN_DOWN && profile_sel < num_profiles - 1) profile_sel++;
	if (p & BTN_UP && profile_sel > 0) profile_sel--;
	if (p & BTN_CIRCLE) {
		if (pack_profile) { Pack_End(); screen = pack_back; }
		else screen = SCR_DETAIL;
	}
	if (p & BTN_CROSS && num_profiles) {
		store_set_avatar_profile(profiles[profile_sel]);
		if (pack_profile) { pack_profile = 0; screen = pack_back; }		// (Pack_Tick goes on)
		else { screen = SCR_DETAIL; Download_Start(); }
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
	i18n_init();
	Speed_Load();
	char name[64];
	backdrop = Load_Image("backdrop.png", SCREEN_W, SCREEN_H);
	logo = Load_Image("logo.png", 220, 31);
	for (int i = 0; i < NUM_GLYPHS; i++) {
		snprintf(name, sizeof(name), "glyphs/%s.png", glyph_files[i]);
		glyphs[i] = Load_Image(name, GLYPH, GLYPH);		// (at the screen's size: Load_Image sees to it)
	}
	// the cards' art, at the sizes Draw_Home and Draw_Picker draw them (tools/make-art.mjs makes them so)
	card_bg = Load_Image("cards/bg.png", 256, 256);
	for (int t = 0; t < NUM_TYPES; t++) {
		snprintf(name, sizeof(name), "cards/%s.png", home_cards[t].art);
		home_art[t] = Load_Image(name, t == TYPE_HOMEBREW ? LIST_W - 504 - CARD_GAP : 504, t == TYPE_HOMEBREW ? HOME_H : (HOME_H - CARD_GAP) / 2);
	}
	for (int i = 0; i < NUM_CAT_CARDS; i++) {
		snprintf(name, sizeof(name), "cards/%s.png", cat_cards[i].art);
		cat_art[i] = Load_Image(name, (LIST_W - 2 * CARD_GAP) / 3, (CARD_H - CARD_GAP) / 2);
	}
	store_init();
	Favs_Load();
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
	Pack_Tick();
	if ((screen == SCR_HOME || screen == SCR_DOWNLOADS) && (!free_checked || t_now - free_checked > 10000000)) {
		long long f = plat_free_bytes("/dev_hdd0");
		free_checked = t_now;
		if (f != free_bytes) { free_bytes = f; dirty = 1; }
	}

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
	case SCR_DOWNLOADS: Input_Downloads(p); break;
	case SCR_LANG: Input_Lang(p); break;
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

// the sections, as tabs (what L1/R1 change on the categories screen)
static int Section_Tabs(int x, int y, int maxx)
{
	const char *labels[NUM_TYPES];
	for (int t = 0; t < NUM_TYPES; t++) labels[t] = tr(section_names[t]);
	return Tabs(x, y, maxx, labels, NUM_TYPES, cur);
}

static void Draw_Header(void)
{
	if (logo) gfx_image(logo, LIST_X, 40, 220, 31, 0, 255);
	const view_t *v = Cur_View();
	char line[200], n[16];
	uint32_t c = C_DIM;
	int px = 18;
	// on the right: the search, a message, or the section and how much is in it (in a pack, its name)
	if (v && (v->query[0] || v->search_wait)) {
		if (v->search_wait) snprintf(line, sizeof(line), tr("Searching for \"%s\"..."), v->query);
		else snprintf(line, sizeof(line), tr("Search: \"%s\"  (%d%s)"), v->query, v->num_results, v->num_results == MAX_RESULTS ? "+" : "");
		c = C_WARN;
	} else if (banner[0]) {
		snprintf(line, sizeof(line), "%s", banner);
		c = C_WARN;
	} else if (folder.pages) {
		snprintf(line, sizeof(line), "%s", folder.name);
		c = C_TEXT;
	} else {
		Fmt_Count(ix->count[cur], n, sizeof(n));
		snprintf(line, sizeof(line), "%s  %s", tr(section_names[cur]), n);
	}
	int rw = font_width(FONT_MEDIUM, px, line);
	if (rw > 420) rw = 420;
	font_draw(FONT_MEDIUM, px, SCREEN_W - LIST_X - rw, 96, line, c, rw);
	// L1/R1: the section's categories (the sections, if it has only the one list)
	int nl = ix->num_lists[cur], maxx = SCREEN_W - LIST_X - rw - 24;
	if (nl > 1) {
		const char *labels[32];
		for (int i = 0; i < nl && i < 32; i++) labels[i] = tr(ix->lists[cur][i].name);
		Tabs(LIST_X, 87, maxx, labels, nl, cur_list[cur]);
	} else
		Section_Tabs(LIST_X, 87, maxx);
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
#define TOP_TABS_MAX	(SCREEN_W - LIST_X - 170)		// (the version is drawn top right)
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
	else if (fetch_job) font_draw_right(FONT_REGULAR, 18, SCREEN_W - LIST_X, 68, tr("Checking for updates..."), C_DIM);
}

// the download going on, bottom right (where the list's position goes otherwise): 1 if drawn
static int Draw_Strip(void)
{
	if (!dl) return 0;
	int right = SCREEN_W - LIST_X, bw = 120, y = SCREEN_H - 55, more = Queue_Busy() - 1;
	float frac = dl->size ? (float)dl->got / (float)dl->size : 0;
	if (frac > 1) frac = 1;
	gfx_round_rect(right - bw, y + 9, bw, 8, 4, 0x40ffffff);
	gfx_round_rect(right - bw, y + 9, (int)(bw * frac), 8, 4, C_ACCENT);
	char name[1][256], line[320];
	Wrap(FONT_MEDIUM, 19, queue[dl_q].it->title, 180, name, 1);
	if (more > 0) snprintf(line, sizeof(line), tr("%s  %d%%  ·  %d more"), name[0], (int)(frac * 100), more);
	else snprintf(line, sizeof(line), "%s  %d%%", name[0], (int)(frac * 100));
	font_draw_right(FONT_MEDIUM, 19, right - bw - 14, y, line, C_TEXT);
	return 1;
}

// "3 Nov 2026", or "" when it isn't known
static void Fmt_Date(long long t, char *out, int size)
{
	time_t tt = (time_t)t;
	struct tm *tm = t > 0 ? localtime(&tt) : NULL;
	out[0] = 0;
	if (tm) strftime(out, size, "%d %b %Y", tm);
}

// "212 GB free on the HDD  ·  14 downloaded"
static void Space_Line(char *out, int size)
{
	char fr[32];
	int n = store_num_downloads();
	Fmt_Size(free_bytes, fr, sizeof(fr));
	if (free_bytes >= 0) snprintf(out, size, tr("%s free on the HDD  ·  %d downloaded"), fr, n);
	else snprintf(out, size, tr("%d downloaded"), n);
}

// a list's count: an avatar list's rows are packs
static void Count_Text(int t, int n, char *out, int size)
{
	char num[16];
	Fmt_Count(n, num, sizeof(num));
	const char *f = t == TYPE_THEME ? (n == 1 ? "%s theme" : "%s themes") : t == TYPE_AVATAR ? (n == 1 ? "%s pack" : "%s packs") :
		(n == 1 ? "%s item" : "%s items");
	snprintf(out, size, tr(f), num);
}

// the home screen's Favourites card: how many, and the first of them
static void Draw_Fav_Card(int x, int y, int w, int h, int on)
{
	int n = favs ? favs->count : 0;
	char line[200];
	Card_Frame(x, y, w, h, on);
	snprintf(line, sizeof(line), "\xe2\x98\x85  %s", tr("Favourites"));
	font_draw(FONT_BOLD, 30, x + 28, y + 20, line, n ? C_TEXT : C_DIM, 320);
	if (n) snprintf(line, sizeof(line), tr(n == 1 ? "%d favourite" : "%d favourites"), n);
	else snprintf(line, sizeof(line), "%s", tr("No favourites yet. Press Select on an item to add it."));
	font_draw(FONT_REGULAR, 19, x + 28, y + 62, line, C_TEXT2, n ? 300 : w - 56);
	// the first few, as their pictures
	int px = x + 360, py = y + (h - THUMB_H) / 2;
	for (int i = 0; i < n && px + THUMB_W <= x + w - 28; i++) {
		const cat_item_t *it = &favs->items[i];
		if (it->type == TYPE_THEME) { Thumb_Draw(it, it->title, px, py); px += THUMB_W + 12; }
		else { Icon_Draw(it, px, py, THUMB_H); px += THUMB_H + 12; }
	}
}

static void Draw_Home(void)
{
	Draw_Top(NULL);
	int rw = 504, lw = LIST_W - rw - CARD_GAP, rh = (HOME_H - CARD_GAP) / 2;
	for (int t = 0; t < NUM_TYPES; t++) {
		const card_text_t *c = &home_cards[t];
		const char *chips[16];
		int n = 0;
		for (int l = 0; l < ix->num_lists[t] && n < 16; l++)		// its categories ("All ..." is the whole section)
			if (strncmp(ix->lists[t][l].name, "All ", 4)) chips[n++] = ix->lists[t][l].name;
		if (t == TYPE_HOMEBREW)
			Card(LIST_X, CARD_Y, lw, HOME_H, home_sel == t, home_art[t], c->kicker, c->title, 64, c->blurb, chips, n);
		else
			Card(LIST_X + lw + CARD_GAP, CARD_Y + (t - 1) * (rh + CARD_GAP), rw, rh, home_sel == t, home_art[t],
				c->kicker, c->title, 44, c->blurb, chips, n);
	}
	Draw_Fav_Card(LIST_X, CARD_Y + HOME_H + CARD_GAP, LIST_W, FAV_H, home_sel == HOME_FAVS);
	if (!banner[0] && !fetch_job && Update_Available()) {
		char line[96];
		snprintf(line, sizeof(line), tr("PS3 World %s is out: press Square to get it"), ix->app.version);
		font_draw_right(FONT_MEDIUM, 19, SCREEN_W - LIST_X, 67, line, C_CYAN);
	}
	int x = Hint(LIST_X, G_CROSS, tr("Open"));
	x = Hint(x, G_TRIANGLE, tr("My downloads"));
	if (Update_Available()) x = Hint(x, G_SQUARE, tr("Update"));
	Hint(x, G_SELECT, tr("Language"));
	if (!Draw_Strip()) {
		char line[128];
		Space_Line(line, sizeof(line));
		font_draw_right(FONT_MEDIUM, 19, SCREEN_W - LIST_X, SCREEN_H - 55, line, C_DIM);
	}
}

static void Draw_Picker(void)
{
	Draw_Top(NULL);
	Section_Tabs(LIST_X + 236, 38, TOP_TABS_MAX);
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
	int x = Hint(LIST_X, G_CROSS, tr("Open"));
	Hint(x, G_CIRCLE, tr("Back"));
	Draw_Strip();
}

static void Draw_List(void)
{
	cat_list_t *l = Cur_List();
	view_t *v = Cur_View();
	Draw_Header();
	gfx_round_rect(LIST_X - 12, LIST_Y - 8, LIST_W + 24, ROWS * ROW_H + 16, 14, C_PANEL);
	if (!l || !v) {		// a section with no lists (none in the catalogue, or none passed)
		font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, LIST_Y + 180, tr("Nothing here yet."), C_DIM, SCREEN_W - 200);
		Hint(LIST_X, G_CIRCLE, tr("Back"));
		return;
	}
	int count = View_Count(v, l);
	Clamp(v, count);		// (every path in: a section's view is never drawn out of range)
	if (!count && !v->search_wait)
		font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, LIST_Y + 180,
			!v->query[0] ? tr("Nothing here yet.") : search_banner ? banner :
			tr("Nothing matches the search. Press Square to clear it."), C_DIM, SCREEN_W - 200);
	for (int r = 0; r < ROWS && v->top + r < count; r++) {
		int row = v->top + r, pos = View_Pos(v, row);
		int y = LIST_Y + r * ROW_H;
		file_slot_t *slot;
		const cat_item_t *it = Item_At(l, pos, &slot);
		if (row == v->sel) gfx_round_rect(LIST_X - 4, y, LIST_W + 8, ROW_H - 4, 10, C_ROW_ON);
		int thumbs = cur == TYPE_THEME;		// (themes: a 16:9 preview, not a square icon)
		int pw = thumbs ? THUMB_W : ICON_LIST, ph = thumbs ? THUMB_H : ICON_LIST;
		int tx = LIST_X + 6 + pw + 16, iy = y + (ROW_H - 4 - ph) / 2;
		if (!it) {
			// its page isn't here yet: the title from the search file if we have it
			int len = 0;
			const char *line = Search_Line(l, pos, &len);
			char title[200] = "";
			if (line) {
				const char *tab = memchr(line, '\t', len);
				snprintf(title, sizeof(title), "%.*s", (int)(tab ? tab - line : len), line);
			}
			if (thumbs) Thumb_Draw(NULL, title[0] ? title : "?", LIST_X + 6, iy);
			else Placeholder(title[0] ? title : "?", LIST_X + 6, iy, ICON_LIST);
			if (title[0]) font_draw(FONT_MEDIUM, 22, tx, y + 8, title, C_TEXT, LIST_W - tx);
			const char *state = slot && slot->failed ? Error_Text(slot->err_kind, slot->err) : tr("Loading...");
			font_draw(FONT_REGULAR, 17, tx, title[0] ? y + 37 : y + 22, state, slot && slot->failed ? C_ERR : C_DIM, LIST_X + LIST_W - tx - 12);
			continue;
		}
		if (thumbs) Thumb_Draw(it, it->title, LIST_X + 6, iy);
		else Icon_Draw(it, LIST_X + 6, iy, ICON_LIST);
		char size[32], sub[300];
		Fmt_Size(it->size, size, sizeof(size));
		int rx = LIST_X + LIST_W - 12;
		rx -= font_draw_right(FONT_MEDIUM, 19, rx, y + 22, size, C_DIM) + 14;
		if (it->list[0])		// a pack: how many avatars, and its regions
			snprintf(sub, sizeof(sub), "%s%s%s", it->description, it->group[0] ? "  ·  " : "", it->group);
		else {
			if (store_downloaded(it)) Badge(&rx, y + 20, tr("Downloaded"), C_OK);
			if (Fav_Find(it->id) >= 0) Badge(&rx, y + 20, "\xe2\x98\x85", C_WARN);		// a star
			if (it->requires == REQ_CFW) Badge(&rx, y + 20, tr("CFW only"), C_WARN);
			else if (it->requires == REQ_HEN) Badge(&rx, y + 20, tr("HEN / CFW"), C_DIM);
			snprintf(sub, sizeof(sub), "%s%s%s%s%s", it->author[0] ? it->author : tr("Unknown author"),
				it->version[0] ? "  ·  v" : "", it->version, it->group[0] && it->type != TYPE_HOMEBREW ? "  ·  " : "",
				it->type != TYPE_HOMEBREW ? tr(it->group) : "");
		}
		font_draw(FONT_MEDIUM, 22, tx, y + 8, it->title, C_TEXT, rx - tx - 10);
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
	x = Hint(x, G_CROSS, tr("Open"));
	x = Hint(x, G_TRIANGLE, tr("Search"));
	if (cur == TYPE_AVATAR && !v->query[0]) x = Hint(x, G_SQUARE, tr("Download pack"));
	if (v->query[0]) x = Hint(x, G_SQUARE, tr("Clear search"));
	else if (folder.pages) x = Hint(x, G_CIRCLE, tr("Packs"));
	else if (ix->num_lists[cur] > 1) x = Hint(x, G_CIRCLE, tr("Categories"));
	else x = Hint(x, G_CIRCLE, tr("Back"));
	if (!v->results && count > 50) x = Hint2(x, G_L2, G_R2, tr("A-Z"));
	if (!Draw_Strip() && count) {
		char pos[64], letter = '#';
		int at = View_Pos(v, v->sel);
		for (int i = 0; i < NUM_LETTERS; i++)
			if (l->letter_first[i] >= 0 && l->letter_first[i] <= at) letter = i ? 'A' + i - 1 : '#';
		snprintf(pos, sizeof(pos), "%c   %d / %d", v->results ? ' ' : letter, v->sel + 1, count);
		font_draw_right(FONT_MEDIUM, 19, SCREEN_W - LIST_X, SCREEN_H - 55, pos, C_DIM);
	}
}

// a "key  value" row of the detail screen (the value wraps); returns the y after it
static int Info_Row(int x, int y, int w, const char *key, const char *value, uint32_t c)
{
	char lines[3][256];
	font_draw(FONT_REGULAR, 19, x, y, key, C_DIM, 0);
	int n = Wrap(FONT_MEDIUM, 19, value, w - 124, lines, 3);
	for (int i = 0; i < n; i++)
		font_draw(FONT_MEDIUM, 19, x + 124, y + i * 26, lines[i], c, 0);
	return y + (n ? n : 1) * 26 + 10;
}

/*
The detail screen, in two columns: on the left the preview (640x360) and under it
the download, how it went, or how to use it after; on the right the title, what
it is (author, size, where it saves...) and the description.
*/
#define DET_PX		(LIST_X + 8)
#define DET_PY		84
#define DET_PW		640
#define DET_PH		360

static void Draw_Detail(void)
{
	const cat_item_t *it = det;
	char line[400], size[128], dest[512];
	gfx_round_rect(LIST_X - 12, 60, SCREEN_W - 2 * LIST_X + 24, SCREEN_H - 150, 16, C_PANEL);

	// the preview: a theme's or homebrew's filling the box, an avatar's square in the middle of it
	int sq = it->type == TYPE_AVATAR, pw = sq ? DET_PH : DET_PW, px = DET_PX + (DET_PW - pw) / 2, failed = 1;
	image_t *img = it->preview[0] ? Art_Get(it, 1, pw, DET_PH, &failed) : NULL;
	if (!img || sq) gfx_round_rect(DET_PX, DET_PY, DET_PW, DET_PH, 12, 0xff11151f);
	if (img)
		gfx_image(img, px, DET_PY, pw, DET_PH, sq ? 0 : 12, 255);
	else if (failed)
		Icon_Draw(it, DET_PX + (DET_PW - ICON_BIG) / 2, DET_PY + (DET_PH - ICON_BIG) / 2, ICON_BIG);
	else
		font_draw_center(FONT_REGULAR, 18, DET_PX + DET_PW / 2, DET_PY + DET_PH / 2 - 12, tr("Loading the preview..."), C_DIM, 0);

	// what it is
	int x = DET_PX + DET_PW + 32, w = SCREEN_W - LIST_X - 8 - x, y = DET_PY;
	char lines[8][256];
	int n = Wrap(FONT_BOLD, 32, it->title, w, lines, 2);
	for (int i = 0; i < n; i++, y += 40)
		font_draw(FONT_BOLD, 32, x, y, lines[i], C_TEXT, 0);
	y += 14;
	if (it->author[0]) y = Info_Row(x, y, w, tr("Author"), it->author, C_TEXT);
	if (it->version[0]) y = Info_Row(x, y, w, tr("Version"), it->version, C_TEXT);
	Fmt_Size(it->size, size, sizeof(size));
	if (it->size >= 50 * 1024 * 1024) {		// a big one: how long it takes at the speed downloads get here
		char when[64];
		Fmt_Time(it->size, when, sizeof(when));
		if (when[0]) snprintf(size + strlen(size), sizeof(size) - strlen(size), " (%s)", when);
	}
	y = Info_Row(x, y, w, tr("Size"), size, C_TEXT);
	if (it->group[0]) y = Info_Row(x, y, w, tr(it->type == TYPE_AVATAR ? "Pack" : "Category"), tr(it->group), C_TEXT);
	y = Info_Row(x, y, w, tr("Works on"), tr(it->requires == REQ_CFW ? "CFW only" : it->requires == REQ_HEN ? "HEN or CFW" : "Any PS3"), C_TEXT);
	int code = store_dest(it, dest, sizeof(dest));
	y = Info_Row(x, y, w, tr("Saves to"), code == DEST_OK ? dest : tr(store_dest_message(code)), code == DEST_OK ? C_TEXT : C_WARN);
	const char *have = store_downloaded(it);
	if (have) y = Info_Row(x, y, w, tr("Downloaded"), tr("Yes"), C_OK);
	if (it->description[0]) {		// as much of it as fits above the panel's foot
		int max = (SCREEN_H - 112 - (y + 8)) / 28;
		n = max > 0 ? Wrap(FONT_REGULAR, 20, it->description, w, lines, max > 8 ? 8 : max) : 0;
		for (int i = 0; i < n; i++)
			font_draw(FONT_REGULAR, 20, x, y + 8 + i * 28, lines[i], C_TEXT2, 0);
	}

	// under the preview: the download, how it went, or how to use it after
	x = DET_PX;
	w = DET_PW;
	int by = DET_PY + DET_PH + 24, q = Queue_Find(it->id);
	if (q >= 0 && q == dl_q) {
		const char *what = tr(dl->state == DL_CHECKING ? "Checking what's there already..." :
			dl->state == DL_VERIFYING ? "Checking the sha256..." : "Downloading");
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
			snprintf(line, sizeof(line), tr("Resumed from %s"), got);
			font_draw(FONT_REGULAR, 17, x, by + 56, line, C_DIM, w);
		}
	} else if (Queue_Active(q)) {
		int ahead = Queue_Ahead(q);
		snprintf(line, sizeof(line), tr(ahead == 1 ? "Queued: %d download ahead of it. You can go back and keep browsing." :
			"Queued: %d downloads ahead of it. You can go back and keep browsing."), ahead);
		n = Wrap(FONT_MEDIUM, 20, line, w, lines, 3);
		for (int i = 0; i < n; i++)
			font_draw(FONT_MEDIUM, 20, x, by + i * 28, lines[i], C_TEXT, 0);
	} else if (result_msg[0] || q >= 0) {
		const char *msg = result_msg[0] ? result_msg : queue[q].msg;
		int ok = result_msg[0] ? result_ok : queue[q].ok;
		n = Wrap(FONT_MEDIUM, 20, msg, w, lines, 4);
		for (int i = 0; i < n; i++)
			font_draw(FONT_MEDIUM, 20, x, by + i * 28, lines[i], ok == 1 ? C_OK : ok == 2 ? C_WARN : C_ERR, 0);
	} else {
		snprintf(line, sizeof(line), tr("After it downloads: %s"), Install_Hint(it->type));
		n = Wrap(FONT_REGULAR, 19, line, w, lines, 3);
		for (int i = 0; i < n; i++)
			font_draw(FONT_REGULAR, 19, x, by + i * 27, lines[i], C_DIM, 0);
	}

	// the download first, then the rest
	int hx = LIST_X;
	if (Queue_Active(q)) hx = Hint(hx, G_SQUARE, tr("Cancel download"));
	else {
		int failed = result_msg[0] ? !result_ok : q >= 0 && !queue[q].ok;		// (result_ok 2: a question, not a failure)
		hx = Hint(hx, G_CROSS, tr(failed ? "Try again" : have ? "Download again" : "Download"));
	}
	if (it->preview[0]) hx = Hint(hx, G_TRIANGLE, tr("Full screen"));
	if (strcmp(it->id, "ps3world-app")) hx = Hint(hx, G_SELECT, tr(Fav_Find(it->id) >= 0 ? "Unfavourite" : "Favourite"));
	Hint(hx, G_CIRCLE, tr("Back"));
	if (q != dl_q) Draw_Strip();		// another one downloading
}

// the preview as big as the screen takes it (an avatar's square, in the middle)
static void Draw_Preview_Full(void)
{
	const cat_item_t *it = det;
	int sq = it->type == TYPE_AVATAR, w = sq ? 600 : SCREEN_W, h = sq ? 600 : SCREEN_H, failed = 1;
	gfx_rect(0, 0, SCREEN_W, SCREEN_H, 0xff000000);
	image_t *img = Art_Get(it, 1, w, h, &failed);
	if (img) gfx_image(img, (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h, 0, 255);
	else font_draw_center(FONT_REGULAR, 22, SCREEN_W / 2, SCREEN_H / 2 - 12,
		failed ? tr("The preview couldn't be loaded.") : tr("Loading the preview..."), C_DIM, 0);
	Hint(LIST_X, G_CIRCLE, tr("Back"));
}

static void Draw_Profile(void)
{
	font_draw_center(FONT_BOLD, 30, SCREEN_W / 2, 140, tr("Which user is the avatar for?"), C_TEXT, 0);
	font_draw_center(FONT_REGULAR, 19, SCREEN_W / 2, 186, tr("It's saved in that user's psn_avatar folder."), C_DIM, 0);
	// 7 rows fit above the hints: the list scrolls to keep the chosen one on screen
	int top = profile_sel > 6 ? profile_sel - 6 : 0;
	for (int i = top; i < num_profiles && i < top + 7; i++) {
		int y = 250 + (i - top) * 56;
		if (i == profile_sel) gfx_round_rect(SCREEN_W / 2 - 300, y - 8, 600, 48, 12, C_ROW_ON);
		char line[160];
		snprintf(line, sizeof(line), "%s   (%s)", profile_names[i], profiles[i]);
		font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, y, line, C_TEXT, 580);
	}
	int x = Hint(LIST_X, G_CROSS, tr("Choose"));
	Hint(x, G_CIRCLE, tr("Back"));
}

static void Draw_Lang(void)
{
	Draw_Top(tr("Language"));
	char line[160];
	for (int i = 0; i <= LANG_COUNT; i++) {
		int y = 120 + i * 38;
		if (i == lang_sel) gfx_round_rect(SCREEN_W / 2 - 320, y - 6, 640, 36, 10, C_ROW_ON);
		if (i == 0) snprintf(line, sizeof(line), tr("The PS3's language (%s)"), lang_name(i18n_system()));
		else snprintf(line, sizeof(line), "%s", lang_name(i - 1));
		int chosen = i == i18n_choice() + 1;
		font_draw_center(chosen ? FONT_BOLD : FONT_MEDIUM, 22, SCREEN_W / 2, y, line, chosen ? C_CYAN : C_TEXT, 600);
	}
	int x = Hint(LIST_X, G_CROSS, tr("Choose"));
	Hint(x, G_CIRCLE, tr("Back"));
}

static void Draw_Favourites(void)
{
	const char *tabs[2] = {tr("My downloads"), tr("Favourites")};
	Draw_Top(NULL);
	Tabs(LIST_X + 236, 38, TOP_TABS_MAX, tabs, 2, 1);
	int n = favs ? favs->count : 0;
	char line[300], size[32];
	snprintf(line, sizeof(line), tr(n == 1 ? "%d favourite" : "%d favourites"), n);
	font_draw(FONT_MEDIUM, 20, LIST_X, 104, line, C_DIM, 0);
	gfx_round_rect(LIST_X - 12, LIST_Y - 8, LIST_W + 24, ROWS * ROW_H + 16, 14, C_PANEL);
	if (!n)
		font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, LIST_Y + 180, tr("No favourites yet. Press Select on an item to add it."), C_DIM, SCREEN_W - 200);
	for (int row = dls_top; row < n && row < dls_top + ROWS; row++) {
		const cat_item_t *it = &favs->items[row];
		int y = LIST_Y + (row - dls_top) * ROW_H, rx = LIST_X + LIST_W - 12;
		int thumbs = it->type == TYPE_THEME, pw = thumbs ? THUMB_W : ICON_LIST, ph = thumbs ? THUMB_H : ICON_LIST;
		int tx = LIST_X + 6 + pw + 16, iy = y + (ROW_H - 4 - ph) / 2;
		if (row == dls_sel) gfx_round_rect(LIST_X - 4, y, LIST_W + 8, ROW_H - 4, 10, C_ROW_ON);
		if (thumbs) Thumb_Draw(it, it->title, LIST_X + 6, iy);
		else Icon_Draw(it, LIST_X + 6, iy, ICON_LIST);
		Fmt_Size(it->size, size, sizeof(size));
		rx -= font_draw_right(FONT_MEDIUM, 19, rx, y + 22, size, C_DIM) + 14;
		if (store_downloaded(it)) Badge(&rx, y + 20, tr("Downloaded"), C_OK);
		snprintf(line, sizeof(line), "%s  ·  %s", tr(section_names[it->type]), it->type == TYPE_HOMEBREW ? (it->author[0] ? it->author : tr("Unknown author")) : tr(it->group));
		font_draw(FONT_MEDIUM, 22, tx, y + 8, it->title, C_TEXT, rx - tx - 10);
		font_draw(FONT_REGULAR, 17, tx, y + 37, line, C_DIM, rx - tx - 10);
	}
	int x = LIST_X;
	if (n) x = Hint(x, G_CROSS, tr("Open"));
	if (n) x = Hint(x, G_SQUARE, tr("Unfavourite"));
	Hint(x, G_CIRCLE, tr("Back"));
}

static void Draw_Downloads(void)
{
	if (dls_tab == 1) { Draw_Favourites(); return; }
	const char *tabs[2] = {tr("My downloads"), tr("Favourites")};
	Draw_Top(NULL);
	Tabs(LIST_X + 236, 38, TOP_TABS_MAX, tabs, 2, 0);
	int n = Dls_Count(), qrows = Dls_Queue_Rows();
	long long total = 0;
	store_rec_t r;
	for (int i = 0; store_download_at(i, &r); i++) total += r.size;
	char line[400], size[32], when[32], fr[32];
	Fmt_Size(total, size, sizeof(size));
	Fmt_Size(free_bytes, fr, sizeof(fr));
	if (free_bytes >= 0) snprintf(line, sizeof(line), tr("%d on the HDD (%s)  ·  %s free"), store_num_downloads(), size, fr);
	else snprintf(line, sizeof(line), tr("%d on the HDD (%s)"), store_num_downloads(), size);
	font_draw(FONT_MEDIUM, 20, LIST_X, 104, line, C_DIM, 0);
	gfx_round_rect(LIST_X - 12, LIST_Y - 8, LIST_W + 24, ROWS * ROW_H + 16, 14, C_PANEL);
	if (!n)
		font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, LIST_Y + 180, tr("Nothing downloaded yet."), C_DIM, SCREEN_W - 200);
	int can_open = 0, is_queue = dls_sel < qrows, q_active = 0;
	for (int row = dls_top; row < n && row < dls_top + ROWS; row++) {
		int y = LIST_Y + (row - dls_top) * ROW_H, tx = LIST_X + 6 + ICON_LIST + 16, rx = LIST_X + LIST_W - 12;
		const char *title, *sub;
		char subbuf[400];
		if (row == dls_sel) gfx_round_rect(LIST_X - 4, y, LIST_W + 8, ROW_H - 4, 10, C_ROW_ON);
		if (row < qrows) {		// in the queue: waiting, downloading, or failed
			const queued_t *q = &queue[Dls_Queue_At(row)];
			title = q->it->title;
			Fmt_Size(q->it->size, size, sizeof(size));
			rx -= font_draw_right(FONT_MEDIUM, 19, rx, y + 22, size, C_DIM) + 14;
			if (q->state == Q_RUNNING && dl) {
				snprintf(subbuf, sizeof(subbuf), tr("Downloading %d%%"), dl->size ? (int)(dl->got * 100 / dl->size) : 0);
				Badge(&rx, y + 20, subbuf, C_ACCENT);
			} else if (q->state == Q_WAITING) Badge(&rx, y + 20, tr("Queued"), C_DIM);
			else Badge(&rx, y + 20, tr("Failed"), C_ERR);
			sub = q->state == Q_FAILED ? q->msg : q->dest;
			if (row == dls_sel) { can_open = 1; q_active = q->state <= Q_RUNNING; }
		} else {
			store_download_at(row - qrows, &r);
			const char *file = strrchr(r.path, '/');
			title = r.title[0] ? r.title : file ? file + 1 : r.path;
			Fmt_Size(r.size, size, sizeof(size));
			rx -= font_draw_right(FONT_MEDIUM, 19, rx, y + 22, size, C_DIM) + 14;
			if (Dls_Newer(&r)) Badge(&rx, y + 20, tr("New version"), C_WARN);
			Fmt_Date(r.when, when, sizeof(when));
			snprintf(subbuf, sizeof(subbuf), "%s%s%s", r.path, when[0] ? "  ·  " : "", when);
			sub = subbuf;
			if (row == dls_sel) can_open = Homebrew_Find(r.id) != NULL;
		}
		Placeholder(title, LIST_X + 6, y + (ROW_H - 4 - ICON_LIST) / 2, ICON_LIST);
		font_draw(FONT_MEDIUM, 22, tx, y + 8, title, C_TEXT, rx - tx - 10);
		if (row == dls_confirm)
			font_draw(FONT_MEDIUM, 17, tx, y + 37, tr("Press Square again to delete this file from the HDD."), C_WARN, rx - tx - 10);
		else
			font_draw(FONT_REGULAR, 17, tx, y + 37, sub, C_DIM, rx - tx - 10);
	}
	if (n > ROWS) {
		int h = ROWS * ROW_H, bh = h * ROWS / n;
		if (bh < 24) bh = 24;
		int by = LIST_Y + (int)((long long)(h - bh) * dls_sel / (n - 1));
		gfx_round_rect(LIST_X + LIST_W + 2, by, 4, bh, 2, 0x99ffffff);
	}
	int x = LIST_X;
	if (can_open) x = Hint(x, G_CROSS, tr("Open"));
	if (n) x = Hint(x, G_SQUARE, tr(!is_queue ? "Delete" : q_active ? "Cancel download" : "Clear"));
	Hint(x, G_CIRCLE, tr("Back"));
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
		if (start_err) Hint(SCREEN_W / 2 - 90, G_CROSS, tr("Try again"));
		break;
	case SCR_HOME: Draw_Home(); break;
	case SCR_LIST:
		if (Picking()) Draw_Picker();
		else Draw_List();
		break;
	case SCR_DETAIL: if (det) { if (preview_full) Draw_Preview_Full(); else Draw_Detail(); } break;
	case SCR_PROFILE: Draw_Profile(); break;
	case SCR_DOWNLOADS: Draw_Downloads(); break;
	case SCR_LANG: Draw_Lang(); break;
	}
	font_draw_right(FONT_REGULAR, 16, SCREEN_W - LIST_X, 40, APP_NAME " " APP_VERSION, C_DIM);
}

void ui_shutdown(void)
{
	// a download in progress keeps what's arrived: it resumes next time (the queue is forgotten)
	if (dl) dl->pause = 1;
	jobs_wait_lane(LANE_DL, 3000000);
}
