/*
gpu.h: what a platform does to draw the UI on its graphics chip (the PS3's
RSX). gfx.c turns the drawing calls into a list of quads; the platform
draws the list. The simulator draws it in software, the same way, so the
list can be checked against the CPU renderer pixel by pixel.

Every quad is a rectangle on the canvas (1280x720 units) with a colour per
corner and two textures: colour = tex0 * colour, its alpha times tex1.alpha, blended over
what's there (src alpha, 1 - src alpha). Textures are A8R8G8B8, linear,
sampled with bilinear filtering and clamped at the edges.
*/
#ifndef GPU_H
#define GPU_H

#include <stdint.h>

typedef struct {
	void *px;			// where the CPU writes the pixels (in RSX memory on a PS3)
	uint32_t off;		// what the GPU calls it (an RSX offset)
	int w, h;
	int pitch;			// bytes from one row to the next (the RSX wants them 64-byte aligned)
} gtex_t;

typedef struct {
	float x0, y0, x1, y1;		// canvas units
	float u0, v0, u1, v1;		// tex0, 0..1
	float s0, t0, s1, t1;		// tex1, 0..1
	uint32_t c[4];				// ARGB: top left, top right, bottom left, bottom right
} gquad_t;

typedef struct {
	int first, count;			// quads
	const gtex_t *tex0, *tex1;
	int sx, sy, sw, sh;			// scissor, canvas units
} gbatch_t;

// the platform's side
int gpu_tex_alloc(gtex_t *t, int w, int h);	// 0 if there's no memory
void gpu_tex_free(gtex_t *t);
void gpu_frame_begin(void);		// the previous frame is finished with every texture
void gpu_frame_submit(const gquad_t *q, int nq, const gbatch_t *b, int nb);

#endif
