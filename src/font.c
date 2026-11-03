/*
font.c: TrueType text with stb_truetype and a glyph cache.

Poppins for Latin text, DejaVu Sans (and its bold) for what Poppins lacks
(Cyrillic, Greek, symbols), then the console's own fonts in /dev_flash for Japanese, Korean
and Chinese (loaded the first time a title needs them).
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "lang.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "thirdparty/stb_truetype.h"

typedef struct {
	unsigned char *data;
	stbtt_fontinfo info;
	int ok;
	const char *path;
	int tried;
} face_t;

static face_t faces[FONT_COUNT];
static const char *face_files[FONT_COUNT] = {"Poppins-Regular.ttf", "Poppins-Medium.ttf", "Poppins-Bold.ttf"};

// the bold weight's own fallback for Cyrillic and Greek, so headings stay bold
static face_t fallback_bold = { .path = "DejaVuSans-Bold.ttf" };

// fallbacks, in order; the dev_flash ones only exist on a PS3
static face_t fallbacks[] = {
	{ .path = "DejaVuSans.ttf" },
	{ .path = "/dev_flash/data/font/SCE-PS3-NR-R-JPN.TTF" },
	{ .path = "/dev_flash/data/font/SCE-PS3-YG-R-KOR.TTF" },
	{ .path = "/dev_flash/data/font/SCE-PS3-DH-R-CGB.TTF" },
	{ .path = "/dev_flash/data/font/SCE-PS3-CP-R-KANA.TTF" },
	{ .path = NULL },		// the simulator's stand-in for those (SIM_CJK_FONT)
};
#define NUM_FALLBACKS (int)(sizeof(fallbacks) / sizeof(fallbacks[0]))

typedef struct {
	uint32_t key;		// font(4) | size(8) | codepoint(20)
	uint32_t cp_hi;		// codepoints past 20 bits
	int used;
	int16_t w, h, xoff, yoff;
	int16_t advance;
	unsigned char *bmp;
} glyph_t;

#define CACHE_SIZE 8192
static glyph_t cache[CACHE_SIZE];

// the size asked for -> the em size drawn (the small ones a little bigger:
// they're read from across a room)
static float Em(int px)
{
	return (px < 20 ? px + 2 : px) * 0.80f;
}

// coverage lifted a touch: light text on a dark background reads thin and
// broken at the edges with linear blending on a TV
static unsigned char gamma_lut[256];
static int cache_count;

static int Face_Load(face_t *f, const char *path)
{
	char full[512];
	size_t len;
	if (path[0] == '/')
		snprintf(full, sizeof(full), "%s", path);
	else
		snprintf(full, sizeof(full), "%s/fonts/%s", plat_data_dir(), path);
	f->tried = 1;
	f->data = plat_read_file(full, &len);
	if (!f->data)
		return 0;
	if (!stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
		free(f->data);
		f->data = NULL;
		plat_log("font: %s isn't a font stb_truetype reads", full);
		return 0;
	}
	f->ok = 1;
	plat_log("font: %s (%u KB)", full, (unsigned)(len / 1024));
	return 1;
}

int font_init(void)
{
	for (int i = 0; i < 256; i++)
		gamma_lut[i] = (unsigned char)(255.0f * powf(i / 255.0f, 0.82f) + 0.5f);
	int ok = 1;
	for (int i = 0; i < FONT_COUNT; i++)
		if (!Face_Load(&faces[i], face_files[i]))
			ok = 0;
	Face_Load(&fallbacks[0], fallbacks[0].path);
	if (getenv("SIM_CJK_FONT")) fallbacks[NUM_FALLBACKS - 1].path = getenv("SIM_CJK_FONT");	// NULL on a PS3
	return ok;
}

static void Cache_Clear(void);
void font_flush(void) { Cache_Clear(); }		// after a language change (another font may draw the same characters)

static void Cache_Clear(void)
{
	gfx_masks_reset();		// (the GPU's copies are keyed by these bitmaps)
	for (int i = 0; i < CACHE_SIZE; i++) {
		free(cache[i].bmp);
		cache[i].bmp = NULL;
		cache[i].used = 0;
	}
	cache_count = 0;
}

// the face that has this codepoint
static face_t *Face_For(int font, uint32_t cp, int *glyph)
{
	face_t *f = &faces[font];
	int g;
	if (f->ok && (g = stbtt_FindGlyphIndex(&f->info, cp)) != 0) {
		*glyph = g;
		return f;
	}
	if (font == FONT_BOLD) {
		if (!fallback_bold.tried)
			Face_Load(&fallback_bold, fallback_bold.path);
		if (fallback_bold.ok && (g = stbtt_FindGlyphIndex(&fallback_bold.info, cp)) != 0) {
			*glyph = g;
			return &fallback_bold;
		}
	}
	// Chinese and Korean text from their own fonts first: the Japanese font has many of the
	// same characters, drawn the Japanese way (and mixing typefaces within a word)
	static const int order_default[] = {0, 1, 2, 3, 4, 5}, order_zh[] = {0, 3, 1, 2, 4, 5}, order_ko[] = {0, 2, 1, 3, 4, 5};
	const char *lc = lang_code(lang_current());
	const int *order = !strncmp(lc, "zh", 2) ? order_zh : !strcmp(lc, "ko") ? order_ko : order_default;
	for (int k = 0; k < NUM_FALLBACKS; k++) {
		face_t *fb = &fallbacks[order[k]];
		if (!fb->path) continue;
		if (!fb->tried)
			Face_Load(fb, fb->path);
		if (fb->ok && (g = stbtt_FindGlyphIndex(&fb->info, cp)) != 0) {
			*glyph = g;
			return fb;
		}
	}
	*glyph = 0;
	return f->ok ? f : NULL;
}

static glyph_t *Glyph(int font, int px, uint32_t cp)
{
	uint32_t key = ((uint32_t)font << 28) | ((uint32_t)(px & 255) << 20) | (cp & 0xfffff);
	uint32_t hi = cp >> 20;
	uint32_t h = (key * 2654435761u) % CACHE_SIZE;
	for (int probe = 0; probe < CACHE_SIZE; probe++) {
		glyph_t *g = &cache[(h + probe) % CACHE_SIZE];
		if (!g->used) {
			if (cache_count > CACHE_SIZE * 3 / 4) {
				Cache_Clear();
				return Glyph(font, px, cp);
			}
			int gi, adv, lsb, x0, y0, x1, y1;
			face_t *f = Face_For(font, cp, &gi);
			memset(g, 0, sizeof(*g));
			g->used = 1;
			g->key = key;
			g->cp_hi = hi;
			cache_count++;
			if (!f)
				return g;
			float sc = stbtt_ScaleForMappingEmToPixels(&f->info, Em(px));
			stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
			g->advance = (int16_t)(adv * sc + 0.5f);
			// drawn at twice the size and averaged down: smoother edges than
			// stb_truetype's own antialiasing at small sizes
			stbtt_GetGlyphBitmapBox(&f->info, gi, sc * 2, sc * 2, &x0, &y0, &x1, &y1);
			if (x1 > x0 && y1 > y0) {
				int ex0 = x0 - (((x0 % 2) + 2) % 2), ey0 = y0 - (((y0 % 2) + 2) % 2);	// down to even
				int W2 = x1 - ex0, H2 = y1 - ey0;
				W2 += W2 & 1;
				H2 += H2 & 1;
				unsigned char *big = calloc(W2 * H2, 1);
				g->w = W2 / 2;
				g->h = H2 / 2;
				g->xoff = ex0 / 2;
				g->yoff = ey0 / 2;
				g->bmp = malloc(g->w * g->h);
				if (big && g->bmp) {
					stbtt_MakeGlyphBitmap(&f->info, big + (y0 - ey0) * W2 + (x0 - ex0), x1 - x0, y1 - y0, W2, sc * 2, sc * 2, gi);
					for (int j = 0; j < g->h; j++)
						for (int i = 0; i < g->w; i++) {
							const unsigned char *b = big + (j * 2) * W2 + i * 2;
							g->bmp[j * g->w + i] = gamma_lut[(b[0] + b[1] + b[W2] + b[W2 + 1] + 2) >> 2];
						}
				} else {
					free(g->bmp);
					g->bmp = NULL;
					g->w = g->h = 0;
				}
				free(big);
			}
			return g;
		}
		if (g->key == key && g->cp_hi == hi)
			return g;
	}
	return NULL;
}

static uint32_t Utf8_Next(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	uint32_t c = *s++;
	int n = 0;
	if (c < 0x80) n = 0;
	else if ((c & 0xe0) == 0xc0) { c &= 0x1f; n = 1; }
	else if ((c & 0xf0) == 0xe0) { c &= 0x0f; n = 2; }
	else if ((c & 0xf8) == 0xf0) { c &= 0x07; n = 3; }
	else { *ps = (const char *)s; return 0xfffd; }
	while (n--) {
		if ((*s & 0xc0) != 0x80) { *ps = (const char *)s; return 0xfffd; }
		c = (c << 6) | (*s++ & 0x3f);
	}
	*ps = (const char *)s;
	return c;
}

/*
==================
Arabic: each letter takes the form its neighbours join it in (DejaVu Sans has the
presentation forms), and a line with Arabic in it reads right to left; a run of
other text in it (numbers, Latin) keeps its own order.
==================
*/
#define MAX_CPS 384		// a line's characters (on the stack, a few times over)

// U+0621..U+064A: the isolated presentation form; joins 2: both sides, 1: only to the letter before, 0: neither
static const uint16_t ar_iso[] = {
	0xFE80, 0xFE81, 0xFE83, 0xFE85, 0xFE87, 0xFE89, 0xFE8D, 0xFE8F, 0xFE93, 0xFE95, 0xFE99, 0xFE9D, 0xFEA1, 0xFEA5,
	0xFEA9, 0xFEAB, 0xFEAD, 0xFEAF, 0xFEB1, 0xFEB5, 0xFEB9, 0xFEBD, 0xFEC1, 0xFEC5, 0xFEC9, 0xFECD,
	0, 0, 0, 0, 0,				// 063B..063F
	0x0640,						// tatweel: joins, no forms of its own
	0xFED1, 0xFED5, 0xFED9, 0xFEDD, 0xFEE1, 0xFEE5, 0xFEE9, 0xFEED, 0xFEEF, 0xFEF1,
};
static const uint8_t ar_joins[] = {
	0, 1, 1, 1, 1, 2, 1, 2, 1, 2, 2, 2, 2, 2, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
	0, 0, 0, 0, 0,
	2,
	2, 2, 2, 2, 2, 2, 2, 1, 1, 2,
};

static int Ar_Letter(uint32_t c) { return c >= 0x621 && c <= 0x64a; }
static int Ar_Joins(uint32_t c) { return Ar_Letter(c) ? ar_joins[c - 0x621] : 0; }
static int Ar_Mark(uint32_t c) { return (c >= 0x64b && c <= 0x65f) || c == 0x670; }		// harakat: don't break a join
static int Rtl(uint32_t c) { return (c >= 0x600 && c <= 0x6ff) || (c >= 0xfb50 && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfefc); }

static void Shape(const uint32_t *in, int n, uint32_t *out, int *nout)
{
	int k = 0;
	for (int i = 0; i < n; i++) {
		uint32_t c = in[i];
		if (!Ar_Letter(c) || !ar_iso[c - 0x621] || c == 0x640) { out[k++] = c; continue; }
		int p = i - 1, q = i + 1;
		while (p >= 0 && Ar_Mark(in[p])) p--;
		while (q < n && Ar_Mark(in[q])) q++;
		int before = p >= 0 && Ar_Joins(in[p]) == 2;
		uint32_t iso = ar_iso[c - 0x621];
		// lam then alef: one ligature
		if (c == 0x644 && q < n && (in[q] == 0x622 || in[q] == 0x623 || in[q] == 0x625 || in[q] == 0x627)) {
			uint32_t lig = in[q] == 0x622 ? 0xFEF5 : in[q] == 0x623 ? 0xFEF7 : in[q] == 0x625 ? 0xFEF9 : 0xFEFB;
			out[k++] = lig + before;
			for (int m = i + 1; m < q; m++) out[k++] = in[m];		// (marks between them)
			i = q;
			continue;
		}
		int after = Ar_Joins(c) == 2 && q < n && Ar_Joins(in[q]) >= 1;
		if (Ar_Joins(c) == 2) out[k++] = iso + (before && after ? 3 : before ? 1 : after ? 2 : 0);
		else out[k++] = iso + (before ? 1 : 0);
	}
	*nout = k;
}

static uint32_t Mirror(uint32_t c)
{
	switch (c) {
	case '(': return ')'; case ')': return '(';
	case '[': return ']'; case ']': return '[';
	case '<': return '>'; case '>': return '<';
	case '{': return '}'; case '}': return '{';
	}
	return c;
}

// a left-to-right character: letters other than Arabic, and digits
static int Ltr(uint32_t c)
{
	if (c >= '0' && c <= '9') return 1;
	if ((c | 32) >= 'a' && (c | 32) <= 'z') return 1;
	return c >= 0xc0 && !Rtl(c) && !(c >= 0x2000 && c <= 0x206f);
}

// a line into the codepoints drawn, left to right (shaped and reordered if it has Arabic)
static int Visual(const char *s, uint32_t *out, int max)
{
	uint32_t in[MAX_CPS];
	int n = 0, rtl = 0;
	while (*s && n < MAX_CPS) {
		uint32_t c = Utf8_Next(&s);
		rtl |= Rtl(c);
		in[n++] = c;
	}
	if (!rtl) {
		if (n > max) n = max;
		memcpy(out, in, n * sizeof(uint32_t));
		return n;
	}
	uint32_t sh[MAX_CPS];
	int m;
	Shape(in, n, sh, &m);
	// what reads left to right: Latin and digits, and punctuation stuck to them ("/dev_hdd0", "(12)")
	unsigned char ltr[MAX_CPS];
	for (int i = 0; i < m; i++) ltr[i] = Ltr(sh[i]);
	for (int changed = 1; changed; ) {
		changed = 0;
		for (int i = 0; i < m; i++)
			if (!ltr[i] && !Rtl(sh[i]) && sh[i] != ' ' && ((i > 0 && ltr[i - 1]) || (i + 1 < m && ltr[i + 1]))) {
				ltr[i] = 1;
				changed = 1;
			}
	}
	// right to left, but each run of left-to-right text (with the spaces between its words) kept in order
	int k = 0;
	for (int i = m - 1; i >= 0 && k < max; ) {
		if (!ltr[i]) { out[k++] = Mirror(sh[i]); i--; continue; }
		int e = i, b = i;
		while (b > 0 && !Rtl(sh[b - 1])) b--;
		while (!ltr[b]) b++;		// (spaces before the run stay right to left)
		for (int j = b; j <= e && k < max; j++) out[k++] = sh[j];
		i = b - 1;
	}
	return k;
}

// the baseline below the top of the line, for this size
static int Ascent(int font, int px)
{
	face_t *f = &faces[font];
	int asc, desc, gap;
	if (!f->ok) return px;
	stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
	float sc = 0;
	// Poppins' ascent is generous: centre the cap height in the line instead
	int cap = (int)(Em(px) * 0.70f);
	(void)asc; (void)desc; (void)gap; (void)sc;
	return (px + cap) / 2;
}

int font_line_height(int font, int px) { (void)font; return px; }

int font_width(int font, int px, const char *s)
{
	int w = 0;
	if (!s) return 0;
	uint32_t cps[MAX_CPS];
	int n = Visual(s, cps, MAX_CPS);
	for (int i = 0; i < n; i++) {
		glyph_t *g = Glyph(font, px, cps[i]);
		if (g) w += g->advance;
	}
	return w;
}

/*
A glyph at pen, base. When the GPU draws at a higher resolution (1080p),
the bitmap is made at that size and drawn back down to this one, so text
is as sharp as the screen; the layout (pen, advances) stays the canvas's,
so nothing moves.
*/
static void Put(int font, int px, uint32_t cp, const glyph_t *g, int pen, int base, uint32_t c)
{
	float sc = gfx_scale();
	if (sc != 1.0f) {
		int spx = (int)(px * sc + 0.5f);
		if (spx > 1 && spx < 256) {
			glyph_t *gs = Glyph(font, spx, cp);
			if (gs && gs->bmp) {
				gfx_mask_scaled(gs->bmp, gs->w, gs->h, gs->w, pen + gs->xoff / sc, base + gs->yoff / sc, c);
				return;
			}
		}
	}
	if (g && g->bmp)
		gfx_mask(g->bmp, g->w, g->h, g->w, pen + g->xoff, base + g->yoff, c);
}

static int Draw(int font, int px, int x, int y, const char *s, uint32_t c, int maxw)
{
	int base = y + Ascent(font, px);
	int pen = x;
	const char *ell = "\xe2\x80\xa6";
	int ellw = 0, cut = 0;

	if (!s) return 0;
	uint32_t cps[MAX_CPS];
	int n = Visual(s, cps, MAX_CPS), first = 0, total = 0;
	int rtl = 0;
	for (int i = 0; i < n; i++) rtl |= Rtl(cps[i]);
	if (maxw > 0 && (total = font_width(font, px, s)) > maxw) {
		ellw = font_width(font, px, ell);
		cut = 1;
	}
	// right to left, the end of the text is on the left: that's what goes, the "…" first
	if (cut && rtl) {
		for (int w = total; first < n && w > maxw - ellw; first++) {
			glyph_t *g = Glyph(font, px, cps[first]);
			if (g) w -= g->advance;
		}
		const char *e = ell;
		uint32_t ecp = Utf8_Next(&e);
		glyph_t *g = Glyph(font, px, ecp);
		int adv = g ? g->advance : 0;
		if (g && g->bmp && (c >> 24))
			Put(font, px, ecp, g, pen, base, c);
		pen += adv;
		cut = 0;
	}
	for (int i = first; i < n; i++) {
		uint32_t cp = cps[i];
		glyph_t *g = Glyph(font, px, cp);
		if (!g) continue;
		if (cut && pen - x + g->advance > maxw - ellw)
			break;
		int adv = g->advance;		// (Put may refill the cache under g)
		if (g->bmp && (c >> 24))
			Put(font, px, cp, g, pen, base, c);
		pen += adv;
	}
	if (cut) {
		const char *e = ell;
		uint32_t ecp = Utf8_Next(&e);
		glyph_t *g = Glyph(font, px, ecp);
		int adv = g ? g->advance : 0;
		if (g && g->bmp && (c >> 24))
			Put(font, px, ecp, g, pen, base, c);
		pen += adv;
	}
	return pen - x;
}

int font_draw(int font, int px, int x, int y, const char *s, uint32_t c, int maxw)
{
	return Draw(font, px, x, y, s, c, maxw);
}

int font_draw_center(int font, int px, int cx, int y, const char *s, uint32_t c, int maxw)
{
	int w = font_width(font, px, s);
	if (maxw > 0 && w > maxw) w = maxw;
	return Draw(font, px, cx - w / 2, y, s, c, maxw);
}

int font_draw_right(int font, int px, int rx, int y, const char *s, uint32_t c)
{
	int w = font_width(font, px, s);
	return Draw(font, px, rx - w, y, s, c, 0);
}

// one character with its drawn shape (not its line box) centred on (cx, cy)
void font_draw_glyph_centered(int font, int px, int cx, int cy, const char *ch, uint32_t c)
{
	const char *p = ch;
	uint32_t cp = Utf8_Next(&p);
	glyph_t *g = Glyph(font, px, cp);
	if (!g || !g->bmp) return;
	float sc = gfx_scale();
	if (sc != 1.0f) {
		int spx = (int)(px * sc + 0.5f);
		glyph_t *gs = spx > 1 && spx < 256 ? Glyph(font, spx, cp) : NULL;
		if (gs && gs->bmp) {
			gfx_mask_scaled(gs->bmp, gs->w, gs->h, gs->w, cx - gs->w / sc / 2, cy - gs->h / sc / 2, c);
			return;
		}
		g = Glyph(font, px, cp);		// (the cache may have been refilled)
		if (!g || !g->bmp) return;
	}
	gfx_mask(g->bmp, g->w, g->h, g->w, cx - g->w / 2, cy - g->h / 2, c);
}
