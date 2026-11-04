/*
gfx.c: software drawing on the canvas.
*/
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "gfx.h"
#include "gpu.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 2048	// artwork comes from the network (and is asked for at 400 px): refuse giant images
#define STB_IMAGE_IMPLEMENTATION
#include "thirdparty/stb_image.h"

static uint32_t *cv;
static int cw, ch;
static int clx0, cly0, clx1, cly1;	// clip, x1/y1 exclusive
static int gpu_on;					// the calls build the GPU's list (the end of this file)

// the GPU path's versions (below)
static void G_Rect(float x, float y, float w, float h, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3);
static void G_Round_Rect(int x, int y, int w, int h, int r, uint32_t c);
static void G_Mask(const uint8_t *mask, int mw, int mh, int pitch, float x, float y, float dw, float dh, uint32_t c);
static void G_Image(const image_t *img, int x, int y, int w, int h, int radius, int alpha);
static void G_Image_Part(const image_t *img, int sx, int sy, int sw, int sh, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
static void G_Tint(const image_t *img, int sx, int sw, int x, int y, uint32_t c);
static int G_Icon(int icon, int x, int y, int sz, uint32_t c);
static void G_Noise_Gradient(int x, int y, int w, int h, int slot);

void gfx_init(uint32_t *canvas, int w, int h)
{
	if (gpu_on && !canvas) { cw = w; ch = h; gfx_unclip(); return; }
	cv = canvas;
	cw = w;
	ch = h;
	gfx_unclip();
}

uint32_t *gfx_canvas(void) { return cv; }

static void Raw_gfx_clip(int x, int y, int w, int h)
{
	clx0 = x < 0 ? 0 : x;
	cly0 = y < 0 ? 0 : y;
	clx1 = x + w > cw ? cw : x + w;
	cly1 = y + h > ch ? ch : y + h;
}

static void Raw_gfx_clip_get(int *x, int *y, int *w, int *h)
{
	*x = clx0; *y = cly0; *w = clx1 - clx0; *h = cly1 - cly0;
}

void gfx_unclip(void)
{
	clx0 = cly0 = 0;
	clx1 = cw;
	cly1 = ch;
}

static inline uint32_t blend(uint32_t d, uint32_t s, int a)
{
	// a: 0..255
	if (a >= 255) return s | 0xff000000;
	if (a <= 0) return d;
	uint32_t rb = (((s & 0xff00ff) * a) + ((d & 0xff00ff) * (255 - a))) >> 8;
	uint32_t g = (((s & 0x00ff00) * a) + ((d & 0x00ff00) * (255 - a))) >> 8;
	return 0xff000000 | (rb & 0xff00ff) | (g & 0x00ff00);
}

static inline void plot(int x, int y, uint32_t c, int a)
{
	if (x < clx0 || x >= clx1 || y < cly0 || y >= cly1 || a <= 0)
		return;
	uint32_t *p = &cv[y * cw + x];
	*p = blend(*p, c, a);
}

// a row of one colour, two pixels a store
static inline void Fill(uint32_t *p, int n, uint32_t c)
{
	if (((uintptr_t)p & 4) && n > 0) { *p++ = c; n--; }
	uint64_t cc = ((uint64_t)c << 32) | c;
	uint64_t *q = (uint64_t *)p;
	int pairs = n >> 1;
	for (int i = 0; i < pairs; i++) q[i] = cc;
	if (n & 1) p[n - 1] = c;
}

void gfx_clear(uint32_t c)
{
	if (gpu_on) {
		int x0 = clx0, y0 = cly0, x1 = clx1, y1 = cly1;
		gfx_unclip();		// (a clear isn't clipped)
		c |= 0xff000000;
		G_Rect(0, 0, cw, ch, c, c, c, c);
		clx0 = x0; cly0 = y0; clx1 = x1; cly1 = y1;
		return;
	}
	Fill(cv, cw * ch, c | 0xff000000);
}

static void Raw_gfx_rect(int x, int y, int w, int h, uint32_t c)
{
	if (gpu_on) { if (c >> 24) G_Rect(x, y, w, h, c, c, c, c); return; }
	int a = c >> 24;
	int x0 = x < clx0 ? clx0 : x, y0 = y < cly0 ? cly0 : y;
	int x1 = x + w > clx1 ? clx1 : x + w, y1 = y + h > cly1 ? cly1 : y + h;
	if (x0 >= x1 || y0 >= y1 || !a)
		return;
	for (int j = y0; j < y1; j++) {
		uint32_t *p = &cv[j * cw + x0];
		if (a == 255) {
			Fill(p, x1 - x0, c | 0xff000000);
		} else {
			for (int i = x0; i < x1; i++, p++) *p = blend(*p, c, a);
		}
	}
}

// coverage of a pixel in a rounded box's corner (0..255)
static inline int corner_cov(float px, float py, float cx, float cy, float r)
{
	float dx = px - cx, dy = py - cy;
	float d = sqrtf(dx * dx + dy * dy);
	float cov = r + 0.5f - d;
	if (cov >= 1.0f) return 255;
	if (cov <= 0.0f) return 0;
	return (int)(cov * 255.0f);
}

// corner coverage tables, one per radius: [j * r + i] for the top-left
// corner's pixel (i, j). Built once: the PPU is slow at square roots.
#define MAX_LUT_R 256
static uint8_t *cov_lut[MAX_LUT_R + 1];

static const uint8_t *Cov(int r)
{
	if (r <= 0 || r > MAX_LUT_R) return NULL;
	if (!cov_lut[r]) {
		uint8_t *t = malloc(r * r);
		if (!t) return NULL;
		for (int j = 0; j < r; j++)
			for (int i = 0; i < r; i++)
				t[j * r + i] = (uint8_t)corner_cov(i + 0.5f, j + 0.5f, r, r, r);
		cov_lut[r] = t;
	}
	return cov_lut[r];
}

static void Raw_gfx_round_rect(int x, int y, int w, int h, int r, uint32_t c)
{
	int a = c >> 24;
	if (r <= 0) { Raw_gfx_rect(x, y, w, h, c); return; }
	if (gpu_on) { G_Round_Rect(x, y, w, h, r, c); return; }
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	// middle band and the side bands between the corners
	Raw_gfx_rect(x, y + r, w, h - 2 * r, c);
	Raw_gfx_rect(x + r, y, w - 2 * r, r, c);
	Raw_gfx_rect(x + r, y + h - r, w - 2 * r, r, c);
	const uint8_t *lut = Cov(r);
	for (int j = 0; j < r; j++) {
		for (int i = 0; i < r; i++) {
			int cov = lut ? lut[j * r + i] : corner_cov(i + 0.5f, j + 0.5f, r, r, r);
			if (!cov) continue;
			int aa = a * cov / 255;
			plot(x + i, y + j, c, aa);
			plot(x + w - 1 - i, y + j, c, aa);
			plot(x + i, y + h - 1 - j, c, aa);
			plot(x + w - 1 - i, y + h - 1 - j, c, aa);
		}
	}
}

static inline uint32_t lerp_c(uint32_t a, uint32_t b, int t)	// t 0..256
{
	int aa = a >> 24, ab = b >> 24;
	int ra = (a >> 16) & 255, rb = (b >> 16) & 255;
	int ga = (a >> 8) & 255, gb = (b >> 8) & 255;
	int ba = a & 255, bb = b & 255;
	return argb(aa + (ab - aa) * t / 256, ra + (rb - ra) * t / 256, ga + (gb - ga) * t / 256, ba + (bb - ba) * t / 256);
}

void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
	if (gpu_on) { if (w > 0 && h > 0) G_Rect(x, y, w, h, top, top, bottom, bottom); return; }
	for (int j = 0; j < h; j++)
		Raw_gfx_rect(x, y + j, w, 1, lerp_c(top, bottom, h > 1 ? j * 256 / (h - 1) : 0));
}

/*
A big gradient with a fine grain over it: dark gradients across 720 rows
step only every 30 rows or so and show bands; adding noise below one
level before rounding hides the steps. Built once per size and colours
(two cached), then copied in rows, so it costs a memcpy a frame.
*/
static uint8_t noise_tile[64 * 64];
static int noise_ready;

static void Noise_Init(void)
{
	uint32_t r = 0x9e3779b9;
	for (int i = 0; i < 64 * 64; i++) {
		// triangular: the sum of two uniforms, 0..510, so the grain has no hard edges
		r ^= r << 13; r ^= r >> 17; r ^= r << 5;
		int a = r & 255;
		r ^= r << 13; r ^= r >> 17; r ^= r << 5;
		noise_tile[i] = (uint8_t)((a + (int)(r & 255)) >> 1);
	}
	noise_ready = 1;
}

static struct { int w, h; uint32_t top, bottom, used; uint32_t *px; int fresh; } grad_cache[2];
static uint32_t grad_clock;

void gfx_vgradient_noise(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
	if (w <= 0 || h <= 0) return;
	if (!noise_ready) Noise_Init();
	int k, slot = -1;
	for (k = 0; k < 2; k++)
		if (grad_cache[k].px && grad_cache[k].w == w && grad_cache[k].h == h
			&& grad_cache[k].top == top && grad_cache[k].bottom == bottom) slot = k;
	if (slot < 0) {
		slot = grad_cache[0].used <= grad_cache[1].used ? 0 : 1;
		if (grad_cache[slot].w * grad_cache[slot].h < w * h || !grad_cache[slot].px) {
			free(grad_cache[slot].px);
			grad_cache[slot].px = malloc((size_t)w * h * 4);
			if (!grad_cache[slot].px) { grad_cache[slot].w = 0; gfx_vgradient(x, y, w, h, top, bottom); return; }
		}
		grad_cache[slot].w = w; grad_cache[slot].h = h;
		grad_cache[slot].fresh = 1;
		grad_cache[slot].top = top; grad_cache[slot].bottom = bottom;
		uint32_t *o = grad_cache[slot].px;
		int r0 = (top >> 16) & 255, g0 = (top >> 8) & 255, b0 = top & 255;
		int r1 = (bottom >> 16) & 255, g1 = (bottom >> 8) & 255, b1 = bottom & 255;
		for (int j = 0; j < h; j++) {
			int t = h > 1 ? j * 65536 / (h - 1) : 0;
			// 8.8 fixed point
			int R = (r0 << 8) + (int)(((int64_t)(r1 - r0) * t) >> 8);
			int G = (g0 << 8) + (int)(((int64_t)(g1 - g0) * t) >> 8);
			int B = (b0 << 8) + (int)(((int64_t)(b1 - b0) * t) >> 8);
			const uint8_t *nrow = &noise_tile[(j & 63) * 64];
			for (int i = 0; i < w; i++) {
				// -1..+1 level: rounds the fraction away, plus a little grain
				int n = ((int)nrow[i & 63] - 128) * 2;
				int rr = (R + n + 128) >> 8, gg = (G + n + 128) >> 8, bb = (B + n + 128) >> 8;
				rr = rr < 0 ? 0 : rr > 255 ? 255 : rr;
				gg = gg < 0 ? 0 : gg > 255 ? 255 : gg;
				bb = bb < 0 ? 0 : bb > 255 ? 255 : bb;
				*o++ = 0xff000000 | (rr << 16) | (gg << 8) | bb;
			}
		}
	}
	grad_cache[slot].used = ++grad_clock;
	if (gpu_on) { G_Noise_Gradient(x, y, w, h, slot); return; }
	const uint32_t *src = grad_cache[slot].px;
	for (int j = 0; j < h; j++) {
		int yy = y + j;
		if (yy < cly0 || yy >= cly1) continue;
		int x0 = x < clx0 ? clx0 : x, x1 = x + w > clx1 ? clx1 : x + w;
		if (x1 > x0) memcpy(&cv[yy * cw + x0], &src[j * w + (x0 - x)], (size_t)(x1 - x0) * 4);
	}
}

void gfx_hgradient(int x, int y, int w, int h, uint32_t left, uint32_t right)
{
	if (gpu_on) { if (w > 0 && h > 0) G_Rect(x, y, w, h, left, right, left, right); return; }
	for (int i = 0; i < w; i++)
		Raw_gfx_rect(x + i, y, 1, h, lerp_c(left, right, w > 1 ? i * 256 / (w - 1) : 0));
}

static void Raw_gfx_circle(int cx, int cy, int r, uint32_t c)
{
	Raw_gfx_round_rect(cx - r, cy - r, 2 * r, 2 * r, r, c);
}

static void ring(float cx, float cy, float r, float thick, uint32_t c)
{
	int a = c >> 24;
	int R = (int)(r + thick) + 2;
	for (int j = -R; j <= R; j++)
		for (int i = -R; i <= R; i++) {
			float px = i + 0.5f, py = j + 0.5f;
			float d = sqrtf(px * px + py * py);
			float cov = thick * 0.5f + 0.5f - fabsf(d - r);
			if (cov <= 0) continue;
			if (cov > 1) cov = 1;
			plot((int)cx + i, (int)cy + j, c, (int)(a * cov));
		}
}

// a thick line with round ends, anti-aliased
static void capsule(float x0, float y0, float x1, float y1, float thick, uint32_t c)
{
	int a = c >> 24;
	float hr = thick * 0.5f;
	int bx0 = (int)floorf(fminf(x0, x1) - hr - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + hr + 1);
	int by0 = (int)floorf(fminf(y0, y1) - hr - 1), by1 = (int)ceilf(fmaxf(y0, y1) + hr + 1);
	float dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy;
	for (int j = by0; j <= by1; j++)
		for (int i = bx0; i <= bx1; i++) {
			float px = i + 0.5f - x0, py = j + 0.5f - y0;
			float t = ll > 0 ? (px * dx + py * dy) / ll : 0;
			if (t < 0) t = 0; else if (t > 1) t = 1;
			float ex = px - t * dx, ey = py - t * dy;
			float cov = hr + 0.5f - sqrtf(ex * ex + ey * ey);
			if (cov <= 0) continue;
			if (cov > 1) cov = 1;
			plot(i, j, c, (int)(a * cov));
		}
}

// a filled triangle, anti-aliased along its edges
static void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
	int a = c >> 24;
	int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))) - 1, bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2))) + 1;
	int by0 = (int)floorf(fminf(y0, fminf(y1, y2))) - 1, by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2))) + 1;
	float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
	float s = area < 0 ? -1.0f : 1.0f;
	float ex[3][3] = {{x0, y0, 0}, {x1, y1, 0}, {x2, y2, 0}};
	for (int j = by0; j <= by1; j++)
		for (int i = bx0; i <= bx1; i++) {
			float px = i + 0.5f, py = j + 0.5f, cov = 1.0f;
			for (int k = 0; k < 3; k++) {
				float ax = ex[k][0], ay = ex[k][1], bx = ex[(k + 1) % 3][0], by = ex[(k + 1) % 3][1];
				float nx = by - ay, ny = ax - bx, len = sqrtf(nx * nx + ny * ny);
				if (len <= 0) continue;
				// signed distance inside the edge, in pixels
				float d = -s * ((px - ax) * nx + (py - ay) * ny) / len;
				float cv_ = d + 0.5f;
				if (cv_ < cov) cov = cv_;
			}
			if (cov <= 0) continue;
			if (cov > 1) cov = 1;
			plot(i, j, c, (int)(a * cov));
		}
}

void gfx_mask(const uint8_t *mask, int mw, int mh, int pitch, int x, int y, uint32_t c)
{
	if (gpu_on) { G_Mask(mask, mw, mh, pitch, x, y, mw, mh, c); return; }
	int a = c >> 24;
	for (int j = 0; j < mh; j++) {
		int yy = y + j;
		if (yy < cly0 || yy >= cly1) continue;
		const uint8_t *m = mask + j * pitch;
		for (int i = 0; i < mw; i++) {
			int xx = x + i;
			if (!m[i] || xx < clx0 || xx >= clx1) continue;
			uint32_t *p = &cv[yy * cw + xx];
			*p = blend(*p, c, m[i] * a / 255);
		}
	}
}

static int img_flip;		// (Raw_gfx_image: the picture turned round, left to right)

static void Raw_gfx_image(const image_t *img, int x, int y, int w, int h, int radius, int alpha)
{
	if (!img || w <= 0 || h <= 0)
		return;
	if (gpu_on) { G_Image(img, x, y, w, h, radius, alpha); return; }
	int same = (img->w == w && img->h == h), flip = img_flip;
	const uint8_t *lut = radius > 0 ? Cov(radius) : NULL;
	if (same && !flip && img->opaque && alpha >= 255) {
		// cover art: whole rows copied; with rounded corners only the
		// corner pixels are blended (a circle's edge, for artists)
		int x0c = x < clx0 ? clx0 : x, x1c = x + w > clx1 ? clx1 : x + w;
		for (int j = 0; j < h; j++) {
			int yy = y + j;
			if (yy < cly0 || yy >= cly1 || x0c >= x1c) continue;
			const uint32_t *row = img->px + j * img->w;
			uint32_t *drow = &cv[yy * cw];
			int cj = (lut && j < radius) ? j : (lut && j >= h - radius) ? h - 1 - j : -1;
			int inset = 0;		// columns at each end handled one by one
			if (cj >= 0) {
				// past the first fully covered pixel of this corner row, it's a plain copy
				inset = radius;
				for (int i = 0; i < radius; i++)
					if (lut[cj * radius + i] == 255) { inset = i; break; }
				for (int i = 0; i < inset; i++) {
					int cov = lut[cj * radius + i];
					if (!cov) continue;
					int xl = x + i, xr = x + w - 1 - i;
					if (xl >= x0c && xl < x1c) drow[xl] = blend(drow[xl], row[i], cov);
					if (xr >= x0c && xr < x1c) drow[xr] = blend(drow[xr], row[w - 1 - i], cov);
				}
			}
			int a0 = x + inset > x0c ? x + inset : x0c, a1 = x + w - inset < x1c ? x + w - inset : x1c;
			if (a1 > a0)
				memcpy(&drow[a0], &row[a0 - x], (a1 - a0) * 4);
		}
		return;
	}
	if (same && !flip && radius <= 0 && alpha >= 255) {
		// straight copy where the image is opaque
		int x0 = x < clx0 ? clx0 : x, x1 = x + w > clx1 ? clx1 : x + w;
		for (int j = 0; j < h; j++) {
			int yy = y + j;
			if (yy < cly0 || yy >= cly1 || x0 >= x1) continue;
			const uint32_t *sp = img->px + j * img->w + (x0 - x);
			uint32_t *dp = &cv[yy * cw + x0];
			for (int i = x0; i < x1; i++, sp++, dp++) {
				uint32_t sv = *sp;
				int sa = sv >> 24;
				*dp = sa == 255 ? sv : blend(*dp, sv, sa);
			}
		}
		return;
	}
	// 16.16 steps for sampling
	int sx = (img->w << 16) / w, sy = (img->h << 16) / h;
	for (int j = 0; j < h; j++) {
		int yy = y + j;
		if (yy < cly0 || yy >= cly1) continue;
		int fy = same ? (j << 16) : (j * sy + sy / 2 - 32768);
		if (fy < 0) fy = 0;
		int y0 = fy >> 16, y1 = y0 + 1 < img->h ? y0 + 1 : y0, ty = (fy >> 8) & 255;
		for (int i = 0; i < w; i++) {
			int xx = x + i;
			if (xx < clx0 || xx >= clx1) continue;
			int a = alpha;
			if (radius > 0) {
				int ci = i < radius ? i : (i >= w - radius ? w - 1 - i : -1);
				int cj = j < radius ? j : (j >= h - radius ? h - 1 - j : -1);
				if (ci >= 0 && cj >= 0) {
					int cov = lut ? lut[cj * radius + ci] : corner_cov(ci + 0.5f, cj + 0.5f, radius, radius, radius);
					a = a * cov / 255;
					if (!a) continue;
				}
			}
			uint32_t s;
			int si = flip ? w - 1 - i : i;		// (the source column)
			if (same) {
				s = img->px[j * img->w + si];
			} else {
				int fx = si * sx + sx / 2 - 32768;
				if (fx < 0) fx = 0;
				int x0 = fx >> 16, x1 = x0 + 1 < img->w ? x0 + 1 : x0, tx = (fx >> 8) & 255;
				uint32_t top = lerp_c(img->px[y0 * img->w + x0], img->px[y0 * img->w + x1], tx);
				uint32_t bot = lerp_c(img->px[y1 * img->w + x0], img->px[y1 * img->w + x1], tx);
				s = lerp_c(top, bot, ty);
			}
			int sa = (s >> 24) * a / 255;
			uint32_t *p = &cv[yy * cw + xx];
			*p = blend(*p, s, sa);
		}
	}
}

void gfx_tint(const image_t *img, int sx, int sw, int x, int y, uint32_t c)
{
	// columns sx..sx+sw of img (a frame of a strip) at x, y
	int a = c >> 24;
	if (!img) return;
	if (gpu_on) { G_Tint(img, sx, sw, x, y, c); return; }
	for (int j = 0; j < img->h; j++) {
		int yy = y + j;
		if (yy < cly0 || yy >= cly1) continue;
		const uint32_t *sp = img->px + j * img->w + sx;
		for (int i = 0; i < sw; i++) {
			int xx = x + i;
			int m = sp[i] >> 24;
			if (!m || xx < clx0 || xx >= clx1) continue;
			uint32_t *p = &cv[yy * cw + xx];
			*p = blend(*p, c, m * a / 255);
		}
	}
}

void gfx_icon(int icon, int x, int y, int sz, uint32_t c)
{
	if (icon == ICON_REPEAT_ONE) icon = ICON_REPEAT;
	if (gpu_on && G_Icon(icon, x, y, sz, c)) return;
	float s = sz, X = x, Y = y;
	float t = s * 0.1f;		// line weight
	if (t < 1.6f) t = 1.6f;
	switch (icon) {
	case ICON_PLAY:
		triangle(X + s * 0.25f, Y + s * 0.15f, X + s * 0.25f, Y + s * 0.85f, X + s * 0.85f, Y + s * 0.5f, c);
		break;
	case ICON_PAUSE:
		Raw_gfx_round_rect(x + (int)(s * 0.22f), y + (int)(s * 0.15f), (int)(s * 0.2f), (int)(s * 0.7f), (int)(s * 0.04f), c);
		Raw_gfx_round_rect(x + (int)(s * 0.58f), y + (int)(s * 0.15f), (int)(s * 0.2f), (int)(s * 0.7f), (int)(s * 0.04f), c);
		break;
	case ICON_NEXT:
		triangle(X + s * 0.18f, Y + s * 0.2f, X + s * 0.18f, Y + s * 0.8f, X + s * 0.68f, Y + s * 0.5f, c);
		Raw_gfx_rect(x + (int)(s * 0.7f), y + (int)(s * 0.2f), (int)(s * 0.12f) + 1, (int)(s * 0.6f), c);
		break;
	case ICON_PREV:
		triangle(X + s * 0.82f, Y + s * 0.2f, X + s * 0.82f, Y + s * 0.8f, X + s * 0.32f, Y + s * 0.5f, c);
		Raw_gfx_rect(x + (int)(s * 0.18f), y + (int)(s * 0.2f), (int)(s * 0.12f) + 1, (int)(s * 0.6f), c);
		break;
	case ICON_SHUFFLE:
		capsule(X + s * 0.12f, Y + s * 0.3f, X + s * 0.35f, Y + s * 0.3f, t, c);
		capsule(X + s * 0.35f, Y + s * 0.3f, X + s * 0.62f, Y + s * 0.7f, t, c);
		capsule(X + s * 0.62f, Y + s * 0.7f, X + s * 0.78f, Y + s * 0.7f, t, c);
		capsule(X + s * 0.12f, Y + s * 0.7f, X + s * 0.35f, Y + s * 0.7f, t, c);
		capsule(X + s * 0.35f, Y + s * 0.7f, X + s * 0.62f, Y + s * 0.3f, t, c);
		capsule(X + s * 0.62f, Y + s * 0.3f, X + s * 0.78f, Y + s * 0.3f, t, c);
		triangle(X + s * 0.76f, Y + s * 0.18f, X + s * 0.76f, Y + s * 0.42f, X + s * 0.92f, Y + s * 0.3f, c);
		triangle(X + s * 0.76f, Y + s * 0.58f, X + s * 0.76f, Y + s * 0.82f, X + s * 0.92f, Y + s * 0.7f, c);
		break;
	case ICON_REPEAT:
		capsule(X + s * 0.2f, Y + s * 0.55f, X + s * 0.2f, Y + s * 0.35f, t, c);
		capsule(X + s * 0.2f, Y + s * 0.35f, X + s * 0.72f, Y + s * 0.35f, t, c);
		capsule(X + s * 0.8f, Y + s * 0.45f, X + s * 0.8f, Y + s * 0.65f, t, c);
		capsule(X + s * 0.8f, Y + s * 0.65f, X + s * 0.28f, Y + s * 0.65f, t, c);
		triangle(X + s * 0.7f, Y + s * 0.22f, X + s * 0.7f, Y + s * 0.48f, X + s * 0.86f, Y + s * 0.35f, c);
		triangle(X + s * 0.3f, Y + s * 0.52f, X + s * 0.3f, Y + s * 0.78f, X + s * 0.14f, Y + s * 0.65f, c);
		break;
	case ICON_NOTE:
		Raw_gfx_circle(x + (int)(s * 0.36f), y + (int)(s * 0.72f), (int)(s * 0.15f), c);
		capsule(X + s * 0.49f, Y + s * 0.72f, X + s * 0.49f, Y + s * 0.15f, t, c);
		capsule(X + s * 0.49f, Y + s * 0.15f, X + s * 0.75f, Y + s * 0.28f, t, c);
		break;
	case ICON_PERSON:
		Raw_gfx_circle(x + sz / 2, y + (int)(s * 0.33f), (int)(s * 0.17f), c);
		Raw_gfx_round_rect(x + (int)(s * 0.2f), y + (int)(s * 0.56f), (int)(s * 0.6f), (int)(s * 0.32f), (int)(s * 0.16f), c);
		break;
	case ICON_ALBUM:
		ring(X + s * 0.5f, Y + s * 0.5f, s * 0.3f, t, c);
		Raw_gfx_circle(x + sz / 2, y + sz / 2, (int)(s * 0.08f) + 1, c);
		break;
	case ICON_LIST:
		for (int k = 0; k < 3; k++) {
			float yy = Y + s * (0.28f + 0.22f * k);
			Raw_gfx_circle(x + (int)(s * 0.18f), (int)yy, (int)(t * 0.6f) + 1, c);
			capsule(X + s * 0.34f, yy, X + s * 0.84f, yy, t, c);
		}
		break;
	case ICON_GEAR:
		for (int k = 0; k < 8; k++) {
			float an = k * 3.14159265f / 4;
			capsule(X + s * 0.5f + cosf(an) * s * 0.26f, Y + s * 0.5f + sinf(an) * s * 0.26f,
				X + s * 0.5f + cosf(an) * s * 0.38f, Y + s * 0.5f + sinf(an) * s * 0.38f, t * 1.4f, c);
		}
		ring(X + s * 0.5f, Y + s * 0.5f, s * 0.22f, t * 1.2f, c);
		break;
	case ICON_WAVE: {
		static const float hgt[5] = {0.3f, 0.6f, 0.9f, 0.55f, 0.35f};
		for (int k = 0; k < 5; k++) {
			float xx = X + s * (0.18f + 0.16f * k), hh = s * hgt[k] * 0.38f;
			capsule(xx, Y + s * 0.5f - hh, xx, Y + s * 0.5f + hh, t, c);
		}
		break;
	}
	case ICON_HOME:
		triangle(X + s * 0.5f, Y + s * 0.14f, X + s * 0.1f, Y + s * 0.5f, X + s * 0.9f, Y + s * 0.5f, c);
		Raw_gfx_round_rect(x + (int)(s * 0.22f), y + (int)(s * 0.46f), (int)(s * 0.56f), (int)(s * 0.4f), (int)(s * 0.05f), c);
		break;
	case ICON_GRID:
		for (int k = 0; k < 4; k++)
			Raw_gfx_round_rect(x + (int)(s * (0.16f + 0.38f * (k & 1))), y + (int)(s * (0.16f + 0.38f * (k >> 1))),
				(int)(s * 0.3f), (int)(s * 0.3f), (int)(s * 0.06f), c);
		break;
	case ICON_QUEUE:	// Up Next: a play mark and three lines
		triangle(X + s * 0.1f, Y + s * 0.16f, X + s * 0.1f, Y + s * 0.44f, X + s * 0.34f, Y + s * 0.3f, c);
		capsule(X + s * 0.46f, Y + s * 0.3f, X + s * 0.88f, Y + s * 0.3f, t, c);
		capsule(X + s * 0.14f, Y + s * 0.53f, X + s * 0.88f, Y + s * 0.53f, t, c);
		capsule(X + s * 0.14f, Y + s * 0.76f, X + s * 0.88f, Y + s * 0.76f, t, c);
		break;
	case ICON_SEARCH:
		ring(X + s * 0.42f, Y + s * 0.42f, s * 0.22f, t, c);
		capsule(X + s * 0.6f, Y + s * 0.6f, X + s * 0.82f, Y + s * 0.82f, t * 1.2f, c);
		break;
	}
}

image_t *image_new(int w, int h)
{
	image_t *img = malloc(sizeof(*img));
	if (!img) return NULL;
	img->w = w;
	img->h = h;
	img->px = malloc((size_t)w * h * 4);
	img->opaque = 0;
	img->rev = 0;
	img->gpu = NULL;
	if (!img->px) { free(img); return NULL; }
	return img;
}

static void G_Image_Forget(image_t *img);

void image_touch(image_t *img) { if (img) img->rev++; }

void image_free(image_t *img)
{
	if (!img) return;
	G_Image_Forget(img);
	free(img->px);
	free(img);
}

// for a w x h box (canvas units): at the screen's own resolution when the GPU draws
// (1080p: 1.5 times), but never past the picture's own: an upscale is the GPU's to filter
image_t *image_decode_screen(const void *data, int len, int w, int h)
{
	float s = gfx_scale();
	int iw, ih, n;
	if (s != 1.0f && w > 0 && h > 0 && stbi_info_from_memory(data, len, &iw, &ih, &n) && iw > 0 && ih > 0) {
		double sc = fmax((double)w * s / iw, (double)h * s / ih);		// the cover scale, at the screen's size
		double k = sc > 1 ? s / sc : s;
		w = (int)(w * k + 0.5);
		h = (int)(h * k + 0.5);
	}
	return image_decode(data, len, w > 0 ? w : 1, h > 0 ? h : 1);
}

image_t *image_decode(const void *data, int len, int w, int h)
{
	int iw, ih, n;
	unsigned char *rgba = stbi_load_from_memory(data, len, &iw, &ih, &n, 4);
	if (!rgba)
		return NULL;
	image_t *img = image_new(w, h);
	if (!img) { stbi_image_free(rgba); return NULL; }

	// cover: the largest centred crop with the box's shape
	double sc = fmax((double)w / iw, (double)h / ih);
	double cw_ = w / sc, ch_ = h / sc;
	double ox = (iw - cw_) / 2, oy = (ih - ch_) / 2;
	for (int j = 0; j < h; j++) {
		// box filter over the source pixels behind each output pixel
		int sy0 = (int)(oy + j / sc), sy1 = (int)(oy + (j + 1) / sc);
		if (sy1 <= sy0) sy1 = sy0 + 1;
		if (sy1 > ih) sy1 = ih;
		if (sy0 >= ih) sy0 = ih - 1;
		for (int i = 0; i < w; i++) {
			int sx0 = (int)(ox + i / sc), sx1 = (int)(ox + (i + 1) / sc);
			if (sx1 <= sx0) sx1 = sx0 + 1;
			if (sx1 > iw) sx1 = iw;
			if (sx0 >= iw) sx0 = iw - 1;
			unsigned r = 0, g = 0, b = 0, a = 0, cnt = 0;
			for (int yy = sy0; yy < sy1; yy++) {
				const unsigned char *p = rgba + ((size_t)yy * iw + sx0) * 4;
				for (int xx = sx0; xx < sx1; xx++, p += 4) {
					r += p[0]; g += p[1]; b += p[2]; a += p[3]; cnt++;
				}
			}
			if (!cnt) cnt = 1;
			img->px[j * w + i] = argb(a / cnt, r / cnt, g / cnt, b / cnt);
		}
	}
	stbi_image_free(rgba);
	img->opaque = 1;
	for (int i = 0; i < w * h; i++)
		if ((img->px[i] >> 24) != 255) { img->opaque = 0; break; }
	return img;
}

uint32_t image_average(const image_t *img)
{
	unsigned long long r = 0, g = 0, b = 0, n = 0;
	if (!img) return 0xff202020;
	for (int i = 0; i < img->w * img->h; i += 7) {
		uint32_t p = img->px[i];
		r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255; n++;
	}
	if (!n) return 0xff202020;
	return argb(255, (int)(r / n), (int)(g / n), (int)(b / n));
}

/*
==================
gfx_image_part: part of an image, its colour multiplied by a vertical blend
==================
*/
static inline uint32_t Mul_C(uint32_t a, uint32_t b)		// per channel a * b / 255
{
	uint32_t r = 0;
	for (int sh = 0; sh < 32; sh += 8) r |= ((((a >> sh) & 255) * ((b >> sh) & 255) + 127) / 255) << sh;
	return r;
}

void gfx_image_part(const image_t *img, int sx, int sy, int sw, int sh, int x, int y, int w, int h,
	uint32_t top, uint32_t bottom)
{
	if (!img || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
	if (gpu_on) { G_Image_Part(img, sx, sy, sw, sh, x, y, w, h, top, bottom); return; }
	int plain = top == 0xffffffff && bottom == 0xffffffff && sw == w && sh == h;
	for (int j = 0; j < h; j++) {
		int yy = y + j;
		if (yy < cly0 || yy >= cly1) continue;
		int sj = sy + (sh == h ? j : (int)(((long long)j * 2 + 1) * sh / (2 * h)));
		if (sj < 0 || sj >= img->h) continue;
		const uint32_t *row = img->px + sj * img->w;
		int x0 = x < clx0 ? clx0 : x, x1 = x + w > clx1 ? clx1 : x + w;
		if (x0 >= x1) continue;
		if (plain && img->opaque && sx + (x0 - x) >= 0 && sx + (x1 - x) <= img->w) {
			memcpy(&cv[yy * cw + x0], &row[sx + (x0 - x)], (size_t)(x1 - x0) * 4);
			continue;
		}
		// the row's multiplier: top..bottom at pixel centres
		uint32_t m = lerp_c(top, bottom, (int)(((long long)j * 2 + 1) * 256 / (2 * h)));
		for (int i = x0; i < x1; i++) {
			int si = sx + (sw == w ? i - x : (int)(((long long)(i - x) * 2 + 1) * sw / (2 * w)));
			if (si < 0 || si >= img->w) continue;
			uint32_t s = Mul_C(row[si], m);
			uint32_t *p = &cv[yy * cw + i];
			*p = blend(*p, s, s >> 24);
		}
	}
}

/*
==================
The GPU path: the calls above as a list of quads (gpu.h)

Most of the UI uses one texture, the atlas: a white block (plain colour),
then glyphs, icons and rounded corners, each made once on the CPU (at the
display's scale) and kept. So text, panels and icons go in one batch;
only cover art and other pictures add batches of their own.
==================
*/
#define ATLAS_W		2048
#define ATLAS_H		2048
#define MAX_QUADS	24576
#define MAX_BATCHES	4096
#define MAP_SIZE	16384		// atlas entries (a power of two)

static gquad_t gq[MAX_QUADS];
static gbatch_t gb[MAX_BATCHES];
static int nq, nb, g_dropped;
static float g_scale = 1.0f;
static gtex_t atlas;
static int atlas_ok, shelf_x, shelf_y, shelf_h, atlas_reset_due, want_redraw;
static float WU, WV;		// the white block's middle
static float g_ox, g_oy;	// gfx_offset

enum { K_MASK = 1, K_CORNER, K_ICON };
typedef struct { const void *key; int kind, a, b; int x, y, w, h; } aslot_t;
static aslot_t amap[MAP_SIZE];
static int acount;

typedef struct { gtex_t t; int rev; } gimg_t;
static gtex_t *freeing;			// textures let go of: freed once the GPU is done with the frame using them
static int nfreeing, capfreeing;

int gfx_gpu(void) { return gpu_on; }
void gfx_offset(float dx, float dy) { g_ox = dx; g_oy = dy; }
float gfx_scale(void) { return gpu_on ? g_scale : 1.0f; }
int gfx_wants_redraw(void) { int w = want_redraw; want_redraw = 0; return w; }

static void Atlas_Reset(void)
{
	memset(amap, 0, sizeof(amap));
	acount = 0;
	// the white block: 4x4 in the corner
	uint32_t *px = atlas.px;
	int stride = atlas.pitch / 4;
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++) px[j * stride + i] = 0xffffffff;
	shelf_x = 5; shelf_y = 0; shelf_h = 5;		// (after the block, and a pixel of space)
	atlas_reset_due = 0;
}

void gfx_gpu_enable(float scale)
{
	if (!gpu_tex_alloc(&atlas, ATLAS_W, ATLAS_H)) return;
	memset(atlas.px, 0, (size_t)atlas.pitch * ATLAS_H);
	atlas_ok = 1;
	WU = 2.0f / ATLAS_W;
	WV = 2.0f / ATLAS_H;
	Atlas_Reset();
	g_scale = scale > 0 ? scale : 1.0f;
	gpu_on = 1;
}

void gfx_masks_reset(void) { if (gpu_on) atlas_reset_due = 1; }

void gfx_frame_begin(void)
{
	if (!gpu_on) return;
	gpu_frame_begin();		// the last frame's done with everything
	for (int i = 0; i < nfreeing; i++) gpu_tex_free(&freeing[i]);
	nfreeing = 0;
	if (atlas_reset_due) Atlas_Reset();
	nq = nb = 0;
	g_dropped = 0;
	g_ox = g_oy = 0;
}

void gfx_frame_end(void)
{
	if (!gpu_on) return;
	gpu_frame_submit(gq, nq, gb, nb);
}

static void Free_Later(const gtex_t *t)
{
	if (nfreeing == capfreeing) {
		int nc = capfreeing ? capfreeing * 2 : 32;
		gtex_t *n = realloc(freeing, nc * sizeof(gtex_t));
		if (!n) { gpu_tex_free((gtex_t *)t); return; }		// (better than a leak)
		freeing = n;
		capfreeing = nc;
	}
	freeing[nfreeing++] = *t;
}

static void G_Image_Forget(image_t *img)
{
	gimg_t *g = img->gpu;
	if (!g) return;
	Free_Later(&g->t);
	free(g);
	img->gpu = NULL;
}

// the GPU's copy of img, made or brought up to date
static const gtex_t *Tex_Of(const image_t *img_c)
{
	image_t *img = (image_t *)img_c;		// (the copy is a cache: drawing doesn't change the image)
	gimg_t *g = img->gpu;
	if (g && (g->t.w != img->w || g->t.h != img->h)) { G_Image_Forget(img); g = NULL; }
	if (!g) {
		g = calloc(1, sizeof(*g));
		if (!g) return NULL;
		if (!gpu_tex_alloc(&g->t, img->w, img->h)) { free(g); want_redraw = 1; return NULL; }
		g->rev = img->rev - 1;
		img->gpu = g;
	}
	if (g->rev != img->rev) {
		if (g->t.pitch == img->w * 4) memcpy(g->t.px, img->px, (size_t)img->w * img->h * 4);
		else for (int j = 0; j < img->h; j++) memcpy((uint8_t *)g->t.px + (size_t)j * g->t.pitch, img->px + (size_t)j * img->w, (size_t)img->w * 4);
		g->rev = img->rev;
	}
	return &g->t;
}

// one quad; a new batch when the textures or the clip change
static void Quad(float x0, float y0, float x1, float y1, const gtex_t *t0, float u0, float v0, float u1, float v1,
	const gtex_t *t1, float s0, float tt0, float s1, float tt1, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
	if (x1 <= x0 || y1 <= y0) return;
	x0 += g_ox; x1 += g_ox; y0 += g_oy; y1 += g_oy;
	if (x1 <= clx0 || x0 >= clx1 || y1 <= cly0 || y0 >= cly1 || clx1 <= clx0 || cly1 <= cly0) return;
	if (!((c0 | c1 | c2 | c3) >> 24)) return;
	if (nq >= MAX_QUADS) { g_dropped++; want_redraw = 1; return; }
	gbatch_t *b = nb ? &gb[nb - 1] : NULL;
	if (!b || b->tex0 != t0 || b->tex1 != t1 || b->sx != clx0 || b->sy != cly0 || b->sw != clx1 - clx0 || b->sh != cly1 - cly0) {
		if (nb >= MAX_BATCHES) { g_dropped++; want_redraw = 1; return; }
		b = &gb[nb++];
		b->first = nq;
		b->count = 0;
		b->tex0 = t0;
		b->tex1 = t1;
		b->sx = clx0; b->sy = cly0; b->sw = clx1 - clx0; b->sh = cly1 - cly0;
	}
	gquad_t *q = &gq[nq++];
	q->x0 = x0; q->y0 = y0; q->x1 = x1; q->y1 = y1;
	q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1;
	q->s0 = s0; q->t0 = tt0; q->s1 = s1; q->t1 = tt1;
	q->c[0] = c0; q->c[1] = c1; q->c[2] = c2; q->c[3] = c3;
	b->count++;
}

static void G_Rect(float x, float y, float w, float h, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3)
{
	if (!atlas_ok) return;
	Quad(x, y, x + w, y + h, &atlas, WU, WV, WU, WV, &atlas, WU, WV, WU, WV, c0, c1, c2, c3);
}

/*
The atlas: shelves left to right, a row at a time. Each entry has a
border a pixel wide that repeats its edge, so filtering at the edge reads
the entry's own pixels (as a texture of its own clamped at the edge would),
never a neighbour or an empty gap: a corner's straight edges meet the band
beside it without a seam, wherever the chip puts its pixel centres.
*/
static aslot_t *Find(const void *key, int kind, int a, int b, int *fresh)
{
	uint32_t h = (uint32_t)(uintptr_t)key * 2654435761u ^ (uint32_t)kind * 40503u ^ (uint32_t)a * 9176u ^ (uint32_t)b * 31u;
	for (int probe = 0; probe < MAP_SIZE; probe++) {
		aslot_t *s = &amap[(h + probe) & (MAP_SIZE - 1)];
		if (!s->kind) {
			if (acount >= MAP_SIZE * 3 / 4) { atlas_reset_due = 1; want_redraw = 1; return NULL; }
			s->key = key; s->kind = kind; s->a = a; s->b = b;
			s->w = 0;
			acount++;
			*fresh = 1;
			return s;
		}
		if (s->key == key && s->kind == kind && s->a == a && s->b == b) { *fresh = 0; return s; }
	}
	return NULL;
}

// room for w x h; 0 when the atlas is full (it's emptied before the next frame)
static int Place(aslot_t *s, int w, int h)
{
	int bw = w + 2, bh = h + 2;		// with the border
	if (shelf_x + bw > ATLAS_W) { shelf_y += shelf_h; shelf_x = 0; shelf_h = 0; }
	if (shelf_y + bh > ATLAS_H || bw > ATLAS_W) {
		atlas_reset_due = 1;
		want_redraw = 1;
		s->kind = 0;		// (not kept: the slot stays free)
		acount--;
		return 0;
	}
	s->x = shelf_x + 1; s->y = shelf_y + 1; s->w = w; s->h = h;
	shelf_x += bw;
	if (bh > shelf_h) shelf_h = bh;
	return 1;
}

// coverage (0..255) into the atlas as white with that alpha
static void Put_Coverage(const aslot_t *s, const uint8_t *cov, int pitch)
{
	uint32_t *px = atlas.px;
	int stride = atlas.pitch / 4;
	// the entry and its border (the nearest edge pixel again)
	for (int j = -1; j <= s->h; j++) {
		uint32_t *d = &px[(s->y + j) * stride + s->x];
		const uint8_t *m = cov + (j < 0 ? 0 : j >= s->h ? s->h - 1 : j) * pitch;
		for (int i = -1; i <= s->w; i++) d[i] = ((uint32_t)m[i < 0 ? 0 : i >= s->w ? s->w - 1 : i] << 24) | 0xffffff;
	}
}

static void Atlas_UV(const aslot_t *s, float *u0, float *v0, float *u1, float *v1)
{
	*u0 = (float)s->x / ATLAS_W; *v0 = (float)s->y / ATLAS_H;
	*u1 = (float)(s->x + s->w) / ATLAS_W; *v1 = (float)(s->y + s->h) / ATLAS_H;
}

static void G_Mask(const uint8_t *mask, int mw, int mh, int pitch, float x, float y, float dw, float dh, uint32_t c)
{
	if (!atlas_ok || mw <= 0 || mh <= 0 || !(c >> 24)) return;
	int fresh;
	aslot_t *s = Find(mask, K_MASK, mw, mh, &fresh);
	if (!s) return;
	if (fresh) {
		if (!Place(s, mw, mh)) return;
		Put_Coverage(s, mask, pitch);
	}
	float u0, v0, u1, v1;
	Atlas_UV(s, &u0, &v0, &u1, &v1);
	Quad(x, y, x + dw, y + dh, &atlas, WU, WV, WU, WV, &atlas, u0, v0, u1, v1, c, c, c, c);
}

void gfx_mask_scaled(const uint8_t *mask, int mw, int mh, int pitch, float x, float y, uint32_t c)
{
	if (gpu_on) { G_Mask(mask, mw, mh, pitch, x, y, mw / g_scale, mh / g_scale, c); return; }
	gfx_mask(mask, mw, mh, pitch, (int)floorf(x + 0.5f), (int)floorf(y + 0.5f), c);
}

// a rounded corner, radius r canvas units (made at the display's scale): the top-left one
static aslot_t *Corner(int r)
{
	int rp = (int)ceilf(r * g_scale);
	if (rp < 1) rp = 1;
	int fresh;
	aslot_t *s = Find(NULL, K_CORNER, rp, 0, &fresh);
	if (!s) return NULL;
	if (fresh) {
		if (!Place(s, rp, rp)) return NULL;
		uint8_t *cov = malloc(rp * rp);
		if (!cov) { s->kind = 0; acount--; return NULL; }
		for (int j = 0; j < rp; j++)
			for (int i = 0; i < rp; i++) cov[j * rp + i] = (uint8_t)corner_cov(i + 0.5f, j + 0.5f, rp, rp, rp);
		Put_Coverage(s, cov, rp);
		free(cov);
	}
	return s;
}

// the four corners of a w x h box at x, y, radius r: tex0 (t0, its own coordinates across
// the box) times the corner shape
static void Corners(int x, int y, int w, int h, int r, const gtex_t *t0, float u0, float v0, float u1, float v1, uint32_t c)
{
	aslot_t *s = Corner(r);
	if (!s) return;
	float a0, b0, a1, b1;
	Atlas_UV(s, &a0, &b0, &a1, &b1);
	float du = (u1 - u0) * r / w, dv = (v1 - v0) * r / h;
	// top left, top right (mirrored across), bottom left (mirrored down), bottom right
	Quad(x, y, x + r, y + r, t0, u0, v0, u0 + du, v0 + dv, &atlas, a0, b0, a1, b1, c, c, c, c);
	Quad(x + w - r, y, x + w, y + r, t0, u1 - du, v0, u1, v0 + dv, &atlas, a1, b0, a0, b1, c, c, c, c);
	Quad(x, y + h - r, x + r, y + h, t0, u0, v1 - dv, u0 + du, v1, &atlas, a0, b1, a1, b0, c, c, c, c);
	Quad(x + w - r, y + h - r, x + w, y + h, t0, u1 - du, v1 - dv, u1, v1, &atlas, a1, b1, a0, b0, c, c, c, c);
}

// a box with rounded corners, tex0 across it (u0..u1, v0..v1) times c: the bands between
// the corners, then the corners
static void Rounded(int x, int y, int w, int h, int r, const gtex_t *t0, float u0, float v0, float u1, float v1, uint32_t c)
{
	if (r * 2 > w) r = w / 2;
	if (r * 2 > h) r = h / 2;
	if (r <= 0) { Quad(x, y, x + w, y + h, t0, u0, v0, u1, v1, &atlas, WU, WV, WU, WV, c, c, c, c); return; }
	float du = (u1 - u0) * r / w, dv = (v1 - v0) * r / h;
	Quad(x, y + r, x + w, y + h - r, t0, u0, v0 + dv, u1, v1 - dv, &atlas, WU, WV, WU, WV, c, c, c, c);
	Quad(x + r, y, x + w - r, y + r, t0, u0 + du, v0, u1 - du, v0 + dv, &atlas, WU, WV, WU, WV, c, c, c, c);
	Quad(x + r, y + h - r, x + w - r, y + h, t0, u0 + du, v1 - dv, u1 - du, v1, &atlas, WU, WV, WU, WV, c, c, c, c);
	Corners(x, y, w, h, r, t0, u0, v0, u1, v1, c);
}

static void G_Round_Rect(int x, int y, int w, int h, int r, uint32_t c)
{
	if (!atlas_ok || !(c >> 24) || w <= 0 || h <= 0) return;
	Rounded(x, y, w, h, r, &atlas, WU, WV, WU, WV, c);
}

static void G_Image(const image_t *img, int x, int y, int w, int h, int radius, int alpha)
{
	if (!atlas_ok || alpha <= 0) return;
	const gtex_t *t = Tex_Of(img);
	if (!t) return;
	uint32_t c = ((uint32_t)(alpha > 255 ? 255 : alpha) << 24) | 0xffffff;
	if (img_flip) Rounded(x, y, w, h, radius, t, 1, 0, 0, 1, c);		// (u from 1 to 0: turned round)
	else Rounded(x, y, w, h, radius, t, 0, 0, 1, 1, c);
}

static void G_Image_Part(const image_t *img, int sx, int sy, int sw, int sh, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
	if (!atlas_ok) return;
	const gtex_t *t = Tex_Of(img);
	if (!t) return;
	Quad(x, y, x + w, y + h, t, (float)sx / img->w, (float)sy / img->h, (float)(sx + sw) / img->w, (float)(sy + sh) / img->h,
		&atlas, WU, WV, WU, WV, top, top, bottom, bottom);
}

// a strip's frame as a stencil (its alpha), in one colour
static void G_Tint(const image_t *img, int sx, int sw, int x, int y, uint32_t c)
{
	if (!atlas_ok) return;
	const gtex_t *t = Tex_Of(img);
	if (!t) return;
	Quad(x, y, x + sw, y + img->h, &atlas, WU, WV, WU, WV, t, (float)sx / img->w, 0, (float)(sx + sw) / img->w, 1, c, c, c, c);
}

// an icon: drawn once in software (white, at the display's scale) and kept in the atlas
static int G_Icon(int icon, int x, int y, int sz, uint32_t c)
{
	if (!atlas_ok) return 0;
	if (!(c >> 24)) return 1;
	int sp = (int)ceilf(sz * g_scale), pad = 2, box = sp + pad * 2;
	int fresh;
	aslot_t *s = Find(NULL, K_ICON, icon, sp, &fresh);
	if (!s) return 1;
	if (fresh) {
		if (!Place(s, box, box)) return 1;
		uint32_t *tmp = calloc((size_t)box * box, 4);
		uint8_t *cov = malloc((size_t)box * box);
		if (!tmp || !cov) { free(tmp); free(cov); s->kind = 0; acount--; return 0; }
		// the software drawing, on a small black canvas of our own
		uint32_t *ocv = cv;
		int ocw = cw, och = ch, ox0 = clx0, oy0 = cly0, ox1 = clx1, oy1 = cly1;
		cv = tmp; cw = box; ch = box;
		clx0 = cly0 = 0; clx1 = cly1 = box;
		gpu_on = 0;
		gfx_icon(icon, pad, pad, sp, 0xffffffff);
		gpu_on = 1;
		cv = ocv; cw = ocw; ch = och; clx0 = ox0; cly0 = oy0; clx1 = ox1; cly1 = oy1;
		for (int i = 0; i < box * box; i++) cov[i] = (uint8_t)((tmp[i] >> 8) & 255);	// white over black: any channel is the coverage
		Put_Coverage(s, cov, box);
		free(tmp);
		free(cov);
	}
	float u0, v0, u1, v1;
	Atlas_UV(s, &u0, &v0, &u1, &v1);
	float p = pad / g_scale;
	Quad(x - p, y - p, x - p + box / g_scale, y - p + box / g_scale, &atlas, WU, WV, WU, WV, &atlas, u0, v0, u1, v1, c, c, c, c);
	return 1;
}

// the grained gradient behind pages: the cached picture, as a texture
static void G_Noise_Gradient(int x, int y, int w, int h, int slot)
{
	static image_t pics[2];
	image_t *p = &pics[slot];
	if (p->px != grad_cache[slot].px || p->w != w || p->h != h || grad_cache[slot].fresh) {
		if (p->px != grad_cache[slot].px || p->w != w || p->h != h) G_Image_Forget(p);
		p->px = grad_cache[slot].px;
		p->w = w; p->h = h;
		p->opaque = 1;
		p->rev++;
		grad_cache[slot].fresh = 0;
	}
	G_Image(p, x, y, w, h, 0, 255);
}

/*
==================
Right to left (Arabic): with the mirror on, the calls above are drawn mirrored across the
canvas: a box at x is drawn at the canvas's width - x - its width. Text isn't mirrored here
(font.c places each line where it goes, and its letters read the right way); pictures keep
their way round unless drawn with gfx_image_flip (the UI's card art).
==================
*/
static int mirror;
void gfx_mirror(int on) { mirror = on; }
int gfx_mirrored(void) { return mirror; }
#define MX(x, w) (mirror ? cw - (x) - (w) : (x))

void gfx_clip(int x, int y, int w, int h) { Raw_gfx_clip(MX(x, w), y, w, h); }
void gfx_clip_get(int *x, int *y, int *w, int *h)
{
	Raw_gfx_clip_get(x, y, w, h);
	if (mirror) *x = cw - *x - *w;
}
void gfx_rect(int x, int y, int w, int h, uint32_t c) { Raw_gfx_rect(MX(x, w), y, w, h, c); }
void gfx_round_rect(int x, int y, int w, int h, int r, uint32_t c) { Raw_gfx_round_rect(MX(x, w), y, w, h, r, c); }
void gfx_circle(int cx, int cy, int r, uint32_t c) { Raw_gfx_circle(mirror ? cw - cx : cx, cy, r, c); }
void gfx_image(const image_t *img, int x, int y, int w, int h, int radius, int alpha)
{
	Raw_gfx_image(img, MX(x, w), y, w, h, radius, alpha);
}
void gfx_image_flip(const image_t *img, int x, int y, int w, int h, int radius, int alpha)
{
	img_flip = mirror;
	Raw_gfx_image(img, MX(x, w), y, w, h, radius, alpha);
	img_flip = 0;
}
