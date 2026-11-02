/*
ui.c: the screens.

  list     three sections (L1/R1), icon/title/author/version/size per row;
           Triangle searches (the system keyboard), L2/R2 jump a letter
  detail   the description, where it'll be saved, Download (X) with progress,
           speed and Cancel (O); the result, or why it failed
  profile  which user an avatar is for, when the PS3 has more than one

The catalogue on the HDD opens at once; the server's copy is fetched in the
background and replaces it when it's newer. Cross confirms and Circle goes
back, as in Resonant (swapped on consoles that confirm with Circle).
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

#define LIST_X		60
#define LIST_Y		150
#define LIST_W		(SCREEN_W - 2 * LIST_X)
#define ROW_H		68
#define ROWS		7
#define ICON_LIST	56
#define ICON_BIG	200
#define GLYPH		28

enum { SCR_START, SCR_LIST, SCR_DETAIL, SCR_PROFILE };

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

static int Contains(const char *hay, const char *needle)
{
	if (!*needle) return 1;
	for (; *hay; hay++) {
		int i = 0;
		while (needle[i] && Lower(hay[i]) == Lower(needle[i])) i++;
		if (!needle[i]) return 1;
	}
	return 0;
}

static char Letter_Of(const char *title)
{
	for (; *title; title++) {
		char c = *title;
		if (c >= 'a' && c <= 'z') return c - 32;
		if (c >= 'A' && c <= 'Z') return c;
		if (c >= '0' && c <= '9') return '#';
		if ((unsigned char)c >= 0x80) return '#';
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
Catalogue and sections
==================
*/
static catalog_t *catalog;
static catalog_t *pending;			// a newer one, swapped in when the detail screen isn't open
static job_t *fetch_job;
static int fetch_tries;
static char banner[200];			// "Offline: ..." over the list
static char start_msg[300];		// the start screen's line
static int start_err;

typedef struct {
	int *idx;						// items of this type that match the search
	int count;
	int sel, top;
	char query[128];
} section_t;
static section_t sections[NUM_TYPES];
static int cur;						// the section shown

static int detail = -1;				// the item on the detail screen (index into catalog->items)

static void Section_Build(int t, const char *keep_id)
{
	section_t *s = &sections[t];
	free(s->idx);
	s->idx = NULL;
	s->count = 0;
	if (!catalog) return;
	s->idx = malloc(sizeof(int) * (catalog->count ? catalog->count : 1));
	if (!s->idx) return;
	int sel = 0;
	for (int i = 0; i < catalog->count; i++) {
		const cat_item_t *it = &catalog->items[i];
		if (it->type != t) continue;
		if (s->query[0] && !Contains(it->title, s->query) && !Contains(it->author, s->query) && !Contains(it->group, s->query))
			continue;
		if (keep_id && !strcmp(it->id, keep_id)) sel = s->count;
		s->idx[s->count++] = i;
	}
	s->sel = keep_id ? sel : 0;
	if (s->sel >= s->count) s->sel = s->count ? s->count - 1 : 0;
	s->top = s->sel - ROWS / 2;
}

static void Catalog_Use(catalog_t *c)
{
	char keep[NUM_TYPES][64];
	for (int t = 0; t < NUM_TYPES; t++) {
		section_t *s = &sections[t];
		keep[t][0] = 0;
		if (catalog && s->count) snprintf(keep[t], 64, "%s", catalog->items[s->idx[s->sel]].id);
	}
	catalog_free(catalog);
	catalog = c;
	for (int t = 0; t < NUM_TYPES; t++)
		Section_Build(t, keep[t][0] ? keep[t] : NULL);
	if (screen == SCR_START) screen = SCR_LIST;
	dirty = 1;
}

typedef struct {
	catalog_t *cat;
	int kind;
} fetch_result_t;

static void Fetch_Run(job_t *j)
{
	http_res_t res;
	size_t len = 0;
	uint64_t t0 = plat_time_us();
	char *body = http_fetch(NULL, CATALOG_URL, NULL, NULL, 30, &j->cancel, &len, &res);

	if (!body || res.status != 200) {
		j->ival = download_error_kind(res.err, body ? res.status : 0);
		if (body) snprintf(j->err, sizeof(j->err), "HTTP %d", res.status);
		else snprintf(j->err, sizeof(j->err), "%s", res.err[0] ? res.err : "The catalogue didn't download");
		free(body);
		return;
	}
	plat_log("catalog: %lu bytes in %.1f s", (unsigned long)len, (plat_time_us() - t0) / 1e6);
	catalog_t *c = catalog_parse(body, len, j->err, sizeof(j->err));
	if (c) store_save_catalog(body, len);
	j->result = c;
	free(body);
}

static void Fetch_Cleanup(job_t *j)
{
	catalog_free(j->result);
}

static void Fetch_Start(void)
{
	if (fetch_job) return;
	fetch_tries++;
	if (!catalog) {
		snprintf(start_msg, sizeof(start_msg), "Loading the catalogue...");
		start_err = 0;
	}
	fetch_job = job_new(Fetch_Run, Fetch_Cleanup);
	if (fetch_job) job_post(fetch_job, LANE_API);
	dirty = 1;
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
		return "Not found on the server (404). It may have moved: refresh the catalogue and try again.";
	case ERR_CHECKSUM:
		return "The download didn't match its checksum, so it was deleted. Try again.";
	case ERR_DISK_FULL:
		snprintf(buf, sizeof(buf), "Not enough free space. %s", detail_msg);
		return buf;
	}
	snprintf(buf, sizeof(buf), "%s", detail_msg);
	return buf;
}

static void Fetch_Tick(void)
{
	if (!job_done(fetch_job))
		return;
	catalog_t *c = fetch_job->result;
	fetch_job->result = NULL;
	if (c) {
		plat_log("catalog: %d items (%d homebrew, %d themes, %d avatars), generated %s",
			c->count, c->per_type[TYPE_HOMEBREW], c->per_type[TYPE_THEME], c->per_type[TYPE_AVATAR], c->generated);
		banner[0] = 0;
		if (catalog && !strcmp(catalog->generated, c->generated)) {
			catalog_free(c);		// the one we have
		} else if (screen == SCR_DETAIL || screen == SCR_PROFILE) {
			catalog_free(pending);
			pending = c;
		} else
			Catalog_Use(c);
	} else {
		const char *why = Error_Text(fetch_job->ival, fetch_job->err);
		plat_log("catalog: %s", fetch_job->err);
		if (catalog) {
			snprintf(banner, sizeof(banner), "Offline: showing the catalogue from %.10s", catalog->generated);
		} else {
			snprintf(start_msg, sizeof(start_msg), "%s", why);
			start_err = 1;
		}
	}
	job_release(fetch_job);
	fetch_job = NULL;
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

static void Icon_Draw(const cat_item_t *it, int x, int y, int size)
{
	image_t *img = Icon_Get(it, size);
	if (img) {
		gfx_image(img, x, y, size, size, size / 10, 255);
		return;
	}
	gfx_round_rect(x, y, size, size, size / 10, 0xff1a1c20);
	char ch[2] = {Letter_Of(it->title), 0};
	font_draw_center(FONT_BOLD, size / 2, x + size / 2, y + size / 4, ch, 0xff3a4152, 0);
}

/*
==================
Downloads (one at a time)
==================
*/
static download_t *dl;
static job_t *dl_job;
static char dl_item_id[64];
static int dl_type;
static uint64_t dl_t_last;
static long long dl_last_got;
static double dl_speed;			// bytes a second, smoothed
static char result_msg[400];		// what happened, on the detail screen
static int result_ok;

static void Dl_Run(job_t *j) { download_run(j->arg); }
static void Profiles_Load(void);

static void Download_Start(const cat_item_t *it)
{
	char dest[512];
	int code = store_dest(it, dest, sizeof(dest));
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
	snprintf(dl->url, sizeof(dl->url), "%s%s", CATALOG_BASE, it->url);
	snprintf(dl->dest, sizeof(dl->dest), "%s", dest);
	dl->size = it->size;
	memcpy(dl->sha256, it->sha256, 32);
	snprintf(dl_item_id, sizeof(dl_item_id), "%s", it->id);
	dl_type = it->type;
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
		if (catalog)
			for (int i = 0; i < catalog->count; i++)
				if (!strcmp(catalog->items[i].id, dl_item_id)) { store_mark_downloaded(&catalog->items[i], dl->dest); break; }
		snprintf(result_msg, sizeof(result_msg), "Checksum OK (%s). Saved to %s\n%s", size, dl->dest, Install_Hint(dl_type));
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
		snprintf(result_msg, sizeof(result_msg), "%s%s", Error_Text(dl->err_kind, dl->err),
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
static void Clamp_List(section_t *s)
{
	if (s->sel < 0) s->sel = 0;
	if (s->sel >= s->count) s->sel = s->count ? s->count - 1 : 0;
	if (s->top > s->sel) s->top = s->sel;
	if (s->top < s->sel - ROWS + 1) s->top = s->sel - ROWS + 1;
	if (s->top > s->count - ROWS) s->top = s->count - ROWS;
	if (s->top < 0) s->top = 0;
}

static void Letter_Jump(section_t *s, int dir)
{
	if (!s->count) return;
	char now = Letter_Of(catalog->items[s->idx[s->sel]].title);
	int i = s->sel;
	if (dir > 0) {
		while (i < s->count - 1 && Letter_Of(catalog->items[s->idx[i]].title) == now) i++;
	} else {
		// back to the start of this letter, or of the one before
		while (i > 0 && Letter_Of(catalog->items[s->idx[i - 1]].title) == now) i--;
		if (i == s->sel && i > 0) {
			char prev = Letter_Of(catalog->items[s->idx[i - 1]].title);
			while (i > 0 && Letter_Of(catalog->items[s->idx[i - 1]].title) == prev) i--;
		}
	}
	s->sel = i;
	s->top = i;
}

static int searching;

static void Input_List(uint32_t p)
{
	section_t *s = &sections[cur];
	if (p & BTN_R1) { cur = (cur + 1) % NUM_TYPES; dirty = 1; return; }
	if (p & BTN_L1) { cur = (cur + NUM_TYPES - 1) % NUM_TYPES; dirty = 1; return; }
	if (p & BTN_DOWN) s->sel++;
	if (p & BTN_UP) s->sel--;
	if (p & BTN_RIGHT) s->sel += ROWS;
	if (p & BTN_LEFT) s->sel -= ROWS;
	if (p & BTN_R2) Letter_Jump(s, 1);
	if (p & BTN_L2) Letter_Jump(s, -1);
	if (p & BTN_TRIANGLE) {
		char title[64];
		snprintf(title, sizeof(title), "Search %s", section_names[cur]);
		if (plat_osk_open(title, s->query)) searching = 1;
	}
	if ((p & (BTN_SQUARE | BTN_CIRCLE)) && s->query[0]) {
		s->query[0] = 0;
		Section_Build(cur, s->count ? catalog->items[s->idx[s->sel]].id : NULL);
	}
	if (p & BTN_CROSS && s->count) {
		detail = s->idx[s->sel];
		result_msg[0] = 0;
		screen = SCR_DETAIL;
	}
	Clamp_List(s);
	if (p) dirty = 1;
}

static void Leave_Detail(void)
{
	screen = SCR_LIST;
	if (pending) {
		Catalog_Use(pending);
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
	if ((p & BTN_CROSS) && catalog && detail >= 0) Download_Start(&catalog->items[detail]);
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
		if (catalog && detail >= 0) Download_Start(&catalog->items[detail]);
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
	store_init();
	catalog_t *c = store_load_catalog();
	if (c) {
		plat_log("catalog: %d items from the HDD cache (generated %s)", c->count, c->generated);
		Catalog_Use(c);
	}
	Fetch_Start();		// the server's copy, in the background
}

void ui_update(void)
{
	t_now = plat_time_us();
	input_update();
	Fetch_Tick();
	Icons_Tick();
	Download_Tick();

	if (searching) {
		char q[128];
		int r = plat_osk_poll(q, sizeof(q));
		if (r) {
			searching = 0;
			if (r > 0) {
				section_t *s = &sections[cur];
				snprintf(s->query, sizeof(s->query), "%s", q);
				Section_Build(cur, NULL);
			}
			dirty = 1;
		}
		return;
	}
	uint32_t p = input_pressed();
	switch (screen) {
	case SCR_START:
		if ((p & BTN_CROSS) && start_err) Fetch_Start();
		break;
	case SCR_LIST: Input_List(p); break;
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
		Fmt_Count(catalog ? catalog->per_type[t] : 0, n, sizeof(n));
		snprintf(label, sizeof(label), "%s  %s", section_names[t], n);
		int w = font_width(FONT_MEDIUM, 22, label) + 32;
		if (t == cur) gfx_round_rect(x, 88, w, 36, 18, C_ROW_ON);
		font_draw(FONT_MEDIUM, 22, x + 16, 94, label, t == cur ? C_TEXT : C_DIM, 0);
		x += w + 8;
	}
	Glyph(G_R1, x + 8, 92);
	const section_t *s = &sections[cur];
	if (s->query[0]) {
		char q[180];
		snprintf(q, sizeof(q), "Search: \"%s\"  (%d)", s->query, s->count);
		font_draw_right(FONT_MEDIUM, 20, SCREEN_W - LIST_X, 96, q, C_WARN);
	} else if (banner[0])
		font_draw_right(FONT_REGULAR, 18, SCREEN_W - LIST_X, 98, banner, C_WARN);
	else if (fetch_job && catalog)
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

static void Draw_List(void)
{
	section_t *s = &sections[cur];
	Draw_Header();
	gfx_round_rect(LIST_X - 12, LIST_Y - 8, LIST_W + 24, ROWS * ROW_H + 16, 14, C_PANEL);
	if (!s->count) {
		font_draw_center(FONT_MEDIUM, 22, SCREEN_W / 2, LIST_Y + 180,
			s->query[0] ? "Nothing matches the search. Press Square to clear it." : "Nothing here yet.", C_DIM, 0);
	}
	for (int r = 0; r < ROWS && s->top + r < s->count; r++) {
		int i = s->top + r;
		const cat_item_t *it = &catalog->items[s->idx[i]];
		int y = LIST_Y + r * ROW_H;
		if (i == s->sel) gfx_round_rect(LIST_X - 4, y, LIST_W + 8, ROW_H - 4, 10, C_ROW_ON);
		Icon_Draw(it, LIST_X + 6, y + (ROW_H - 4 - ICON_LIST) / 2, ICON_LIST);
		int tx = LIST_X + 6 + ICON_LIST + 16;
		char size[32], sub[300];
		Fmt_Size(it->size, size, sizeof(size));
		int rx = LIST_X + LIST_W - 12;
		rx -= font_draw_right(FONT_MEDIUM, 19, rx, y + 22, size, C_DIM) + 14;
		if (store_downloaded(it)) Badge(&rx, y + 20, "Downloaded", C_OK);
		if (it->requires == REQ_CFW) Badge(&rx, y + 20, "CFW only", C_WARN);
		else if (it->requires == REQ_HEN) Badge(&rx, y + 20, "HEN / CFW", C_DIM);
		font_draw(FONT_MEDIUM, 22, tx, y + 8, it->title, C_TEXT, rx - tx - 10);
		snprintf(sub, sizeof(sub), "%s%s%s%s%s", it->author[0] ? it->author : "Unknown author",
			it->version[0] ? "  ·  v" : "", it->version, it->group[0] ? "  ·  " : "", it->group);
		font_draw(FONT_REGULAR, 17, tx, y + 37, sub, C_DIM, rx - tx - 10);
	}
	// where in the list, and the letter
	if (s->count > ROWS) {
		int h = ROWS * ROW_H, bh = h * ROWS / s->count;
		if (bh < 24) bh = 24;
		int by = LIST_Y + (int)((long long)(h - bh) * s->sel / (s->count - 1));
		gfx_round_rect(LIST_X + LIST_W + 2, by, 4, bh, 2, 0x99ffffff);
	}
	int x = LIST_X;
	x = Hint(x, G_CROSS, "Open");
	x = Hint(x, G_TRIANGLE, "Search");
	if (s->query[0]) x = Hint(x, G_SQUARE, "Clear search");
	if (s->count > 50) x = Hint2(x, G_L2, G_R2, "A-Z");
	x = Hint2(x, G_L1, G_R1, "Section");
	if (s->count) {
		char pos[48], letter[2] = {Letter_Of(catalog->items[s->idx[s->sel]].title), 0};
		snprintf(pos, sizeof(pos), "%s   %d / %d", letter, s->sel + 1, s->count);
		font_draw_right(FONT_MEDIUM, 19, SCREEN_W - LIST_X, SCREEN_H - 55, pos, C_DIM);
	}
}

static void Draw_Detail(void)
{
	const cat_item_t *it = &catalog->items[detail];
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
	if (dl && !strcmp(dl_item_id, it->id)) {
		const char *what = dl->state == DL_CHECKING ? "Checking what's there already..." :
			dl->state == DL_VERIFYING ? "Checking the sha256..." : "Downloading";
		float frac = dl->size ? (float)dl->got / (float)dl->size : 0;
		char got[32], tot[32], spd[32];
		Fmt_Size(dl->got, got, sizeof(got));
		Fmt_Size(dl->size, tot, sizeof(tot));
		Fmt_Size((long long)dl_speed, spd, sizeof(spd));
		gfx_round_rect(x, by, w, 14, 7, 0x40ffffff);
		gfx_round_rect(x, by, (int)(w * frac), 14, 7, C_ACCENT);
		snprintf(line, sizeof(line), "%s  %d%%  ·  %s of %s%s%s/s", what, (int)(frac * 100), got, tot,
			dl->state == DL_RUNNING ? "  ·  " : "", dl->state == DL_RUNNING ? spd : "");
		if (dl->state != DL_RUNNING) line[strlen(line) - 2] = 0;		// no "/s" without a speed
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
	font_draw_center(FONT_REGULAR, 19, SCREEN_W / 2, 186, "It's saved in that user's psn_avatar folder. You can change this later.", C_DIM, 0);
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
	case SCR_LIST: Draw_List(); break;
	case SCR_DETAIL: Draw_Detail(); break;
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
