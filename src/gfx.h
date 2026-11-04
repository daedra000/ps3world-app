/*
gfx.h: a software canvas (1280x720, 0xAARRGGBB in native u32s) and what
the UI draws on it. The PS3 backend copies the finished canvas to the screen.
*/
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

typedef struct {
	int w, h;
	uint32_t *px;		// 0xAARRGGBB
	int opaque;			// every pixel's alpha is 255 (rows can be copied whole)
	int rev;			// bumped (image_touch) when px changes: the GPU's copy is made again
	void *gpu;			// the GPU's copy (gfx.c's)
} image_t;

void gfx_init(uint32_t *canvas, int w, int h);
uint32_t *gfx_canvas(void);

void gfx_clip(int x, int y, int w, int h);		// later draws stay inside
void gfx_unclip(void);
void gfx_clip_get(int *x, int *y, int *w, int *h);

void gfx_clear(uint32_t argb);
void gfx_rect(int x, int y, int w, int h, uint32_t argb);			// alpha-blended
void gfx_round_rect(int x, int y, int w, int h, int r, uint32_t argb);
void gfx_vgradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
// opaque, with a fine grain so dark gradients don't band (cached, copied in rows)
void gfx_vgradient_noise(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_hgradient(int x, int y, int w, int h, uint32_t left, uint32_t right);
void gfx_circle(int cx, int cy, int r, uint32_t argb);

// draws img scaled into the box (bilinear when it shrinks or grows); radius rounds the corners
void gfx_image(const image_t *img, int x, int y, int w, int h, int radius, int alpha);
// the same, but turned round (left to right) when the mirror's on: art that has a side
void gfx_image_flip(const image_t *img, int x, int y, int w, int h, int radius, int alpha);

// right to left (Arabic): clip, rect, round_rect, circle and image are drawn mirrored across
// the canvas; text isn't (font.c places it); the other calls aren't mirrored (the UI doesn't use them)
void gfx_mirror(int on);
int gfx_mirrored(void);
// part of img (sx, sy, sw, sh) into the box, its colour multiplied by top at the top and bottom at
// the bottom, blended between (0xffffffff: as it is; an alpha fading down fades it in)
void gfx_image_part(const image_t *img, int sx, int sy, int sw, int sh, int x, int y, int w, int h,
	uint32_t top, uint32_t bottom);

// img's alpha as a stencil, filled with one colour (white icons, tinted)
void gfx_tint(const image_t *img, int sx, int sw, int x, int y, uint32_t argb);

// a coverage mask (fonts): 8 bit alpha, w*h, drawn in one colour
void gfx_mask(const uint8_t *mask, int mw, int mh, int pitch, int x, int y, uint32_t argb);

// icons drawn from shapes, size = box side
enum { ICON_PLAY, ICON_PAUSE, ICON_NEXT, ICON_PREV, ICON_SHUFFLE, ICON_REPEAT, ICON_NOTE,
	ICON_PERSON, ICON_ALBUM, ICON_LIST, ICON_GEAR, ICON_WAVE, ICON_SEARCH, ICON_REPEAT_ONE, ICON_HOME, ICON_GRID, ICON_QUEUE, NUM_ICONS };
void gfx_icon(int icon, int x, int y, int size, uint32_t argb);

/*
The GPU path: with it on, the calls above build a list of quads (gpu.h)
that the platform draws, instead of drawing on the canvas. scale is the
display's pixels per canvas unit (1.5 at 1080p): masks (text, icons,
rounded corners) are made that much finer.
*/
void gfx_gpu_enable(float scale);
int gfx_gpu(void);
float gfx_scale(void);			// 1 without the GPU
// with the GPU, later draws move by dx, dy canvas units (a fraction of one, to land on a
// display pixel between canvas units: smooth scrolling at 1080p); nothing without it
void gfx_offset(float dx, float dy);
void gfx_frame_begin(void);		// before a frame's drawing
void gfx_frame_end(void);		// after it: the list goes to the platform
void gfx_masks_reset(void);		// the glyph bitmaps were freed (their pointers are stale)
int gfx_wants_redraw(void);		// something didn't fit this frame: draw the next one in full
// a glyph made at gfx_scale(): drawn mw/scale x mh/scale canvas units at x, y (canvas units)
void gfx_mask_scaled(const uint8_t *mask, int mw, int mh, int pitch, float x, float y, uint32_t argb);

image_t *image_new(int w, int h);
void image_touch(image_t *img);		// its pixels changed
void image_free(image_t *img);
// decodes JPEG/PNG and fits it into w x h (cover: crops to fill); NULL if it can't
image_t *image_decode(const void *data, int len, int w, int h);
// the same for drawing at w x h: at the screen's resolution with the GPU at 1080p (up to the picture's own)
image_t *image_decode_screen(const void *data, int len, int w, int h);
uint32_t image_average(const image_t *img);	// for tinting backgrounds

static inline uint32_t argb(int a, int r, int g, int b)
{
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static inline uint32_t with_alpha(uint32_t c, int a)
{
	return (c & 0xffffff) | ((uint32_t)a << 24);
}

#endif
