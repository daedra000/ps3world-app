/*
video_ps3.c: the screen.

The RSX draws the UI: gfx.c hands over a list of quads (gpu.h) and they're
drawn straight into the display buffer at the display's own resolution
(1080p when the PS3 is set to it), with one pair of tiny shaders
(tools/shaders/ui.*). If the RSX can't be used - a self-test at start-up
fails, or the last start froze during it - the CPU draws the canvas at
1280x720 and it's copied into the display buffer, as before.

The rules (NZ:P PS3's ps3_video.c, Doom64-PS3, TyrQuakeCell and Quake2PS3
before it; RSX Rally for the drawing):
  - framebuffers on 1 MB boundaries (the XMB's screenshot froze the console
    with 64-byte aligned ones: Grid Siege v2.6.9);
  - no gcmSetWaitFlip: the CPU checks the flip has happened before it
    touches anything the last frame used (its buffer, vertices, textures);
  - room in the command buffer is made before each group of commands, with
    a correct call into libgcm (PSL1GHT's own segment change can leave gaps
    the RSX runs as garbage: found by rsxkit, used by RSX Rally);
  - every self-test step is written to disk (synced) before it runs, and a
    marker file stays until 300 frames have been drawn, so a freeze points
    at its step and the next start uses the CPU instead;
  - on the way out, the flip handler goes first.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <malloc.h>
#include <unistd.h>

#include <ppu-types.h>
#include <rsx/rsx.h>
#include <rsx/gcm_sys.h>
#include <sysutil/video.h>
#include <sys/file.h>
#include <lv2/sysfs.h>

#include "../plat.h"
#include "../input.h"
#include "../gfx.h"
#include "../gpu.h"
#include "ps3.h"
#include "shaders_ui.h"

#ifndef APP_DIR
#define APP_DIR "/dev_hdd0/game/PSWD00001/USRDIR"
#endif
#define GPU_LOG		APP_DIR "/ps3world_gpu_log.txt"
#define GPU_TRYING	APP_DIR "/gpu_trying"		// there while the RSX is still on trial
#define USE_CPU		APP_DIR "/use_cpu"		// Settings > Picture: the CPU draws
#define GPU_FAILED	APP_DIR "/gpu_failed"		// the RSX froze once: the CPU draws (delete it to try again)

#define FB_COUNT	2
#define CB_SIZE		(1 * 1024 * 1024)
#define IO_SIZE		(16 * 1024 * 1024)

static gcmContextData *ctx;
static void *io_buffer;
static u32 *fb[FB_COUNT];
static u32 fb_offset[FB_COUNT];
static int disp_w, disp_h, disp_pitch;
static int bar_h;		// a 4:3 TV: the 16:9 canvas letterboxed, this many black rows above and below
static int cur;
static volatile u32 flips_done;
static u32 flips_queued;
static int ready;
static int *xmap;		// canvas column for each screen column (when not 1280x720)

#define CANVAS_BYTES	(SCREEN_W * SCREEN_H * 4)
static uint32_t *cpu_canvas;		// the CPU path draws here; copied to the framebuffer

static void FlipHandler(const u32 head)
{
	(void)head;
	flips_done++;
}

static void Wait_Flips(void)
{
	for (int waited = 0; flips_done != flips_queued && waited < 2000; waited++)
		usleep(100);
}

static void Flip(void)
{
	gcmSetFlip(ctx, cur);
	rsxFlushBuffer(ctx);
	flips_queued++;
	cur ^= 1;
}

/*
==================
The log for the RSX path: each line on disk before the next step
==================
*/
static void Sync_Write(const char *path, const char *text, int trunc)
{
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | (trunc ? SYS_O_TRUNC : SYS_O_APPEND), &fd, 0666, NULL, 0) != 0 || fd < 0)
		return;
	u64 w = 0;
	sysLv2FsWrite(fd, text, strlen(text), &w);
	sysLv2FsFsync(fd);
	sysLv2FsClose(fd);
}

static void Glog(const char *fmt, ...)
{
	char line[256], out[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	snprintf(out, sizeof(out), "[%.2f] %s\n", plat_time_us() / 1e6, line);
	Sync_Write(GPU_LOG, out, 0);
	plat_log("gpu: %s", line);
}

static int File_Exists(const char *path)
{
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0 || fd < 0) return 0;
	sysLv2FsClose(fd);
	return 1;
}

/*
==================
Command buffer room (see the top)
==================
*/
static s32 __attribute__((noinline)) Fifo_Callback(gcmContextData *context, u32 count)
{
	register u64 r3 __asm__("r3") = (u64)(uintptr_t)context;
	register u64 r4 __asm__("r4") = count;
	u32 opd = (u32)(uintptr_t)context->callback;	// a 32-bit function descriptor: entry, TOC
	__asm__ __volatile__(
		"stdu 1,-128(1)\n\t"
		"std 2,40(1)\n\t"
		"lwz 0,0(%2)\n\t"
		"lwz 2,4(%2)\n\t"
		"mtctr 0\n\t"
		"bctrl\n\t"
		"ld 2,40(1)\n\t"
		"addi 1,1,128\n\t"
		: "+r"(r3), "+r"(r4)
		: "b"((u64)opd)
		: "r0", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "ctr", "lr", "xer",
		  "cr0", "cr1", "cr5", "cr6", "cr7", "memory");
	return (s32)r3;
}

static void Reserve(u32 words)
{
	if (!ctx || ctx->current + words <= ctx->end) return;
	Fifo_Callback(ctx, words);
}

/*
==================
The RSX path
==================
*/
typedef struct { float x, y, z, w, r, g, b, a, u, v, s, t; } vert_t;	// 48 bytes
#define MAX_VERTS	(24576 * 6 + 64)

static int gpu_mode;				// the RSX draws the UI
static vert_t *vb;
static u32 vb_off;
static u32 *depth;					// display-size depth buffer (unused, but the surface wants one)
static u32 depth_off;
// vertex programs: the UI's, and RSX Rally's HUD one (for the self-test)
typedef struct { const char *name; const rsxVertexProgram *vpo; void *ucode; } vprog_t;
enum { VP_UI, VP_RR, NUM_VP };
static vprog_t vprogs[NUM_VP] = {
	{"ui", (const rsxVertexProgram *)UI_VPO, NULL}, {"rrtex", (const rsxVertexProgram *)RRTEX_VPO, NULL},
};
static int cur_vp = VP_UI;
// fragment programs (they run from RSX memory): the UI's, one without its swizzle, and the
// self-test's cut-down ones
typedef struct { const char *name; const rsxFragmentProgram *fpo; u32 off; } fprog_t;
enum { FP_UI, FP_UI2, FP_COL, FP_T0, FP_T1, FP_RR, FP_TC0, FP_TC1, NUM_FP };
static fprog_t fprogs[NUM_FP] = {
	{"ui", (const rsxFragmentProgram *)UI_FPO, 0}, {"ui2", (const rsxFragmentProgram *)UI2_FPO, 0},
	{"col", (const rsxFragmentProgram *)COL_FPO, 0}, {"t0", (const rsxFragmentProgram *)T0_FPO, 0},
	{"t1", (const rsxFragmentProgram *)T1_FPO, 0}, {"rrtex", (const rsxFragmentProgram *)RRTEX_FPO, 0},
	{"tc0", (const rsxFragmentProgram *)TC0_FPO, 0}, {"tc1", (const rsxFragmentProgram *)TC1_FPO, 0},
};
static int cur_fp = FP_UI;			// the one drawing uses
static int blend_on = 1;
static int scissor_flip;			// the RSX's scissor counts from the bottom (found by the self-test)
static int good_frames;
static const gquad_t *last_q;		// the last frame's list, drawn again for each reflip
static const gbatch_t *last_b;
static int last_nq, last_nb;
static u32 label_val = 1;
#define LABEL_INDEX	253

// where the quads go: a surface and the canvas size its corners stand for
// (oy: canvas units the canvas is moved down by, for the letterbox's top bar)
typedef struct { u32 off; int pitch, w, h, fmt; u32 depth_off; int depth_pitch; float cw, ch, oy; } target_t;

// how textures are read: found by the self-test (smooth filtering and plain rows, if they work)
static int tex_nearest, tex_swz, tex_min_w, tex_min_pitch = 64;

int gpu_tex_alloc(gtex_t *t, int w, int h)
{
	int pitch = (w * 4 + 63) & ~63;
	if (pitch < tex_min_pitch) pitch = tex_min_pitch;
	void *p = rsxMemalign(128, (u32)pitch * h);
	u32 off;
	if (!p) return 0;
	if (rsxAddressToOffset(p, &off) != 0) { rsxFree(p); return 0; }
	t->px = p;
	t->off = off;
	t->w = w;
	t->h = h;
	t->pitch = pitch;
	return 1;
}

void gpu_tex_free(gtex_t *t)
{
	if (t->px) rsxFree(t->px);
	t->px = NULL;
}

void gpu_frame_begin(void)
{
	Wait_Flips();		// the last frame is on screen: the RSX is done with all it used
}

static u32 Remap(void)
{
	return (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) | (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT) |
		(GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) | (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) | (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT) |
		(GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) | (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT);
}

static void Bind(int unit, const gtex_t *t)
{
	gcmTexture tx;
	Reserve(128);
	memset(&tx, 0, sizeof(tx));
	tx.format = GCM_TEXTURE_FORMAT_A8R8G8B8 | (tex_swz ? GCM_TEXTURE_FORMAT_SWZ : GCM_TEXTURE_FORMAT_LIN);
	tx.mipmap = 1;
	tx.dimension = GCM_TEXTURE_DIMS_2D;
	tx.cubemap = GCM_FALSE;
	tx.remap = Remap();
	tx.width = t->w;
	tx.height = t->h;
	tx.depth = 1;
	tx.location = GCM_LOCATION_RSX;
	tx.pitch = tex_swz ? 0 : t->pitch;
	tx.offset = t->off;
	rsxLoadTexture(ctx, unit, &tx);
	rsxTextureControl(ctx, unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	u8 f = tex_nearest ? GCM_TEXTURE_NEAREST : GCM_TEXTURE_LINEAR;
	rsxTextureFilter(ctx, unit, 0, f, f, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, unit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

static void Scissor(const target_t *tg, int x, int y, int w, int h)
{
	// canvas units to the surface's pixels (rounded outwards)
	float sx = tg->w / tg->cw, sy = tg->h / tg->ch;
	int x0 = (int)(x * sx), y0 = (int)((y + tg->oy) * sy);
	int x1 = (int)((x + w) * sx + 0.999f), y1 = (int)((y + h + tg->oy) * sy + 0.999f);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > tg->w) x1 = tg->w;
	if (y1 > tg->h) y1 = tg->h;
	if (x1 < x0) x1 = x0;
	if (y1 < y0) y1 = y0;
	if (scissor_flip) { int t = tg->h - y1; y1 = tg->h - y0; y0 = t; }
	Reserve(16);
	rsxSetScissor(ctx, x0, y0, x1 - x0, y1 - y0);
}

static void Write_Vertices(const target_t *tg, const gquad_t *q, int nq)
{
	if (nq * 6 > MAX_VERTS) nq = MAX_VERTS / 6;
	float kx = 2.0f / tg->cw, ky = 2.0f / tg->ch;
	vert_t *v = vb;
	for (int i = 0; i < nq; i++, q++) {
		float X0 = q->x0 * kx - 1, X1 = q->x1 * kx - 1;
		float Y0 = 1 - (q->y0 + tg->oy) * ky, Y1 = 1 - (q->y1 + tg->oy) * ky;
		// corners: top left, top right, bottom left, bottom right; two triangles
		const float px[4] = {X0, X1, X0, X1}, py[4] = {Y0, Y0, Y1, Y1};
		const float pu[4] = {q->u0, q->u1, q->u0, q->u1}, pv[4] = {q->v0, q->v0, q->v1, q->v1};
		const float ps[4] = {q->s0, q->s1, q->s0, q->s1}, pt[4] = {q->t0, q->t0, q->t1, q->t1};
		static const int order[6] = {0, 1, 2, 1, 3, 2};
		for (int k = 0; k < 6; k++, v++) {
			int c = order[k];
			uint32_t col = q->c[c];
			v->x = px[c]; v->y = py[c]; v->z = 0; v->w = 1;
			v->r = ((col >> 16) & 255) / 255.0f;
			v->g = ((col >> 8) & 255) / 255.0f;
			v->b = (col & 255) / 255.0f;
			v->a = (col >> 24) / 255.0f;
			v->u = pu[c]; v->v = pv[c];
			v->s = ps[c]; v->t = pt[c];
		}
	}
	__sync_synchronize();
}

// the surface, the state and the programs: everything a list needs before its first quad
static void Begin_Target(const target_t *tg, uint32_t clear)
{
	gcmSurface sf;
	Reserve(512);
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat = tg->fmt;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = tg->off;
	sf.colorPitch[0] = tg->pitch;
	for (int i = 1; i < 4; i++) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = tg->depth_off;
	sf.depthPitch = tg->depth_pitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = tg->w;
	sf.height = tg->h;
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(ctx, &sf);
	rsxSetColorMask(ctx, GCM_COLOR_MASK_B | GCM_COLOR_MASK_G | GCM_COLOR_MASK_R | GCM_COLOR_MASK_A);
	rsxSetColorMaskMrt(ctx, 0);
	f32 scale[4] = {tg->w * 0.5f, tg->h * -0.5f, 0.5f, 0.0f};
	f32 offset[4] = {tg->w * 0.5f, tg->h * 0.5f, 0.5f, 0.0f};
	rsxSetViewport(ctx, 0, 0, tg->w, tg->h, 0.0f, 1.0f, scale, offset);
	for (int i = 0; i < 8; i++) rsxSetViewportClip(ctx, i, tg->w, tg->h);
	rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetScissor(ctx, 0, 0, tg->w, tg->h);
	rsxSetClearColor(ctx, clear | 0xff000000);
	rsxSetClearDepthStencil(ctx, 0xffffff00);
	rsxClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S);

	Reserve(512);
	rsxSetDepthTestEnable(ctx, GCM_FALSE);
	rsxSetDepthWriteEnable(ctx, GCM_FALSE);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
		GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	rsxSetBlendEnable(ctx, blend_on ? GCM_TRUE : GCM_FALSE);
	rsxSetBlendFunc(ctx, GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_ONE, GCM_ONE_MINUS_SRC_ALPHA);
	rsxSetBlendEquation(ctx, GCM_FUNC_ADD, GCM_FUNC_ADD);
	rsxLoadVertexProgram(ctx, vprogs[cur_vp].vpo, vprogs[cur_vp].ucode);
	rsxLoadFragmentProgramLocation(ctx, fprogs[cur_fp].fpo, fprogs[cur_fp].off, GCM_LOCATION_RSX);
	rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);

	// every input off, then the four the program reads, all inside vb
	Reserve(256);
	for (int i = 0; i < 16; i++)
		rsxBindVertexArrayAttrib(ctx, i, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, vb_off, sizeof(vert_t), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, vb_off + 16, sizeof(vert_t), 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX0, 0, vb_off + 32, sizeof(vert_t), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_TEX1, 0, vb_off + 40, sizeof(vert_t), 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxInvalidateVertexCache(ctx);
}

// the batches (vertices already written)
static void Draw_Batches(const target_t *tg, const gbatch_t *b, int nb, int nq)
{
	const gtex_t *bound[2] = {NULL, NULL};
	int sc[4] = {-1, -1, -1, -1};
	for (int k = 0; k < nb; k++) {
		const gbatch_t *bt = &b[k];
		int first = bt->first, count = bt->count;
		if (first >= nq || count <= 0) continue;
		if (first + count > nq) count = nq - first;
		if ((first + count) * 6 > MAX_VERTS) continue;
		if (bt->tex0 != bound[0]) { Bind(0, bt->tex0); bound[0] = bt->tex0; }
		if (bt->tex1 != bound[1]) { Bind(1, bt->tex1); bound[1] = bt->tex1; }
		if (sc[0] != bt->sx || sc[1] != bt->sy || sc[2] != bt->sw || sc[3] != bt->sh) {
			Scissor(tg, bt->sx, bt->sy, bt->sw, bt->sh);
			sc[0] = bt->sx; sc[1] = bt->sy; sc[2] = bt->sw; sc[3] = bt->sh;
		}
		Reserve(32 + count * 6 / 256 * 2);
		rsxDrawVertexArray(ctx, GCM_TYPE_TRIANGLES, first * 6, count * 6);
	}
}

static target_t Screen_Target(void)
{
	target_t tg = {fb_offset[cur], disp_pitch, disp_w, disp_h, GCM_SURFACE_X8R8G8B8, depth_off, disp_w * 4, SCREEN_W, SCREEN_H, 0};
	if (bar_h) {		// the surface stands for a taller canvas, the 1280x720 in its middle
		tg.ch = (float)SCREEN_H * disp_h / (disp_h - 2 * bar_h);
		tg.oy = bar_h * tg.ch / disp_h;
	}
	return tg;
}

// frames and reflips both count: the UI only draws when something changes
static void Frame_Good(void)
{
	if (++good_frames == 300) {
		sysLv2FsUnlink(GPU_TRYING);
		Glog("300 frames: the RSX stays on");
	}
}

void gpu_frame_submit(const gquad_t *q, int nq, const gbatch_t *b, int nb)
{
	if (!gpu_mode || !ready) return;
	target_t tg = Screen_Target();
	Write_Vertices(&tg, q, nq);
	Begin_Target(&tg, 0x000000);
	Draw_Batches(&tg, b, nb, nq);
	Flip();
	last_q = q; last_nq = nq; last_b = b; last_nb = nb;
	Frame_Good();
}

/*
==================
Self-test: a small surface the CPU reads back
==================
*/
#define TW 64
#define TH 64
static u32 *test_px, *test_depth;
static u32 test_off, test_depth_off;

static int Finish(int max_us)
{
	Reserve(16);
	rsxSetWriteBackendLabel(ctx, LABEL_INDEX, label_val);
	__sync_synchronize();
	rsxFlushBuffer(ctx);
	for (int waited = 0; *(vu32 *)gcmGetLabelAddress(LABEL_INDEX) != label_val; waited += 50) {
		if (waited > max_us) { label_val++; return 0; }
		usleep(50);
	}
	label_val++;
	__sync_synchronize();
	return 1;
}

static u32 Test_Pixel(int x, int y) { return ((volatile u32 *)test_px)[y * TW + x] & 0xffffff; }
static u32 Test_Raw(int x, int y) { return ((volatile u32 *)test_px)[y * TW + x]; }

static int Near(u32 a, u32 b)
{
	for (int sh = 0; sh <= 16; sh += 8)
		if (abs((int)((a >> sh) & 255) - (int)((b >> sh) & 255)) > 12) return 0;
	return 1;
}

// draws a list on the test surface, waits for it
static int Test_Draw(const gquad_t *q, int nq, const gbatch_t *b, int nb, uint32_t clear)
{
	target_t tg = {test_off, TW * 4, TW, TH, GCM_SURFACE_A8R8G8B8, test_depth_off, TW * 4, TW, TH};
	Write_Vertices(&tg, q, nq);
	Begin_Target(&tg, clear);
	Draw_Batches(&tg, b, nb, nq);
	return Finish(300000);
}

static gquad_t Test_Quad(float x0, float y0, float x1, float y1, uint32_t c)
{
	gquad_t q;
	memset(&q, 0, sizeof(q));
	q.x0 = x0; q.y0 = y0; q.x1 = x1; q.y1 = y1;
	q.u0 = 0; q.v0 = 0; q.u1 = 1; q.v1 = 1;
	q.s0 = 0; q.t0 = 0; q.s1 = 1; q.t1 = 1;
	q.c[0] = q.c[1] = q.c[2] = q.c[3] = c;
	return q;
}

static void Fill_Test_Textures(gtex_t *white, gtex_t *pat, gtex_t *half)
{
	for (int j = 0; j < 4; j++)
		for (int i = 0; i < 4; i++) {
			((u32 *)white->px)[j * (white->pitch / 4) + i] = 0xffffffff;
			// quadrants: red, green / blue, white
			((u32 *)pat->px)[j * (pat->pitch / 4) + i] = j < 2 ? (i < 2 ? 0xffff0000 : 0xff00ff00) : (i < 2 ? 0xff0000ff : 0xffffffff);
			// clear on the left, solid on the right
			((u32 *)half->px)[j * (half->pitch / 4) + i] = i < 2 ? 0x00ffffff : 0xffffffff;
		}
	__sync_synchronize();
}

static int Self_Test(char *why, int len)
{
	gtex_t white, pat, half;
	if (!gpu_tex_alloc(&white, 4, 4) || !gpu_tex_alloc(&pat, 4, 4) || !gpu_tex_alloc(&half, 4, 4)) {
		snprintf(why, len, "no RSX memory for the test textures");
		return 0;
	}
	Fill_Test_Textures(&white, &pat, &half);
	gquad_t q[2];
	gbatch_t b;

	Glog("test 1: label");
	if (!Finish(300000)) { snprintf(why, len, "test 1 (label) timed out"); return 0; }

	// a red quad through each program in turn (the cut-down ones say which part fails)
	static const struct { int fp, blend; const char *what; } steps[] = {
		{FP_COL, 0, "colour only, no blending"}, {FP_COL, 1, "colour only, blended"},
		{FP_T0, 1, "first texture"}, {FP_T1, 1, "second texture"},
		{FP_UI, 1, "the UI's program"}, {FP_UI2, 1, "the UI's program without the swizzle"},
	};
	int works[6] = {0};
	for (int k = 0; k < 6; k++) {
		Glog("test 2.%d: a red quad, %s", k + 1, steps[k].what);
		cur_fp = steps[k].fp;
		blend_on = steps[k].blend;
		q[0] = Test_Quad(0, 0, TW, TH, 0xffff0000);
		b = (gbatch_t){0, 1, &white, &white, 0, 0, TW, TH};
		if (!Test_Draw(q, 1, &b, 1, 0x123456)) { snprintf(why, len, "test 2.%d (%s) timed out", k + 1, steps[k].what); return 0; }
		u32 px = Test_Pixel(32, 32);
		works[k] = Near(px, 0xff0000);
		Glog("test 2.%d: read %06x (%s)", k + 1, (unsigned)px, works[k] ? "good" : "wanted ff0000");
	}
	blend_on = 1;
	if (!works[2]) {
		// what the texture programs see: the coordinates as colour, RSX Rally's own HUD programs,
		// and ours unblended (black: the texture reads as nothing; red: it reads)
		gtex_t big;
		if (gpu_tex_alloc(&big, 64, 64)) {
			for (int j = 0; j < 64; j++)
				for (int i = 0; i < 64; i++) ((u32 *)big.px)[j * (big.pitch / 4) + i] = 0xffffffff;
			__sync_synchronize();
			static const struct { int vp, fp, nearest; const char *what; } d[] = {
				{VP_UI, FP_TC0, 0, "first texture coordinate as colour (want about 7f7f00, alpha ff)"},
				{VP_UI, FP_TC1, 0, "second texture coordinate as colour"},
				{VP_RR, FP_RR, 1, "RSX Rally's HUD programs, 64x64 white, nearest (want ffffff)"},
				{VP_UI, FP_RR, 1, "our vertex program, RSX Rally's fragment program"},
				{VP_RR, FP_RR, 0, "RSX Rally's HUD programs, smooth"},
				{VP_UI, FP_T0, 0, "ours, unblended (black: texture read as nothing)"},
			};
			blend_on = 0;
			for (int k = 0; k < 6; k++) {
				cur_vp = d[k].vp;
				cur_fp = d[k].fp;
				tex_nearest = d[k].nearest;
				q[0] = Test_Quad(0, 0, TW, TH, 0xffff0000);
				b = (gbatch_t){0, 1, &big, &big, 0, 0, TW, TH};
				int ok = Test_Draw(q, 1, &b, 1, 0x123456);
				Glog("test 4.%d: %s: read %08x%s", k + 1, d[k].what, (unsigned)Test_Raw(32, 32), ok ? "" : " (timed out)");
			}
			blend_on = 1;
			cur_vp = VP_UI;
			tex_nearest = 0;
			gpu_tex_free(&big);
		}
		// textures read nothing: try the ways RSX Rally's work (nearest filtering, bigger,
		// swizzled) and keep the first that draws
		static const struct { int size, nearest, swz, pitch; const char *what; } ways[] = {
			{4, 1, 0, 64, "4x4, plain rows, nearest"}, {4, 0, 0, 256, "4x4, rows 256 bytes apart, smooth"},
			{4, 0, 0, 1024, "4x4, rows 1024 bytes apart, smooth"}, {64, 0, 0, 64, "64x64, plain rows, smooth"},
			{64, 1, 0, 64, "64x64, plain rows, nearest"}, {256, 1, 0, 64, "256x256, plain rows, nearest"},
			{4, 0, 1, 64, "4x4, swizzled, smooth"}, {64, 0, 1, 64, "64x64, swizzled, smooth"},
		};
		int found = -1;
		for (int k = 0; k < 8; k++) {
			gtex_t t;
			tex_min_pitch = ways[k].pitch;
			if (!gpu_tex_alloc(&t, ways[k].size, ways[k].size)) { Glog("test 3.%d: no memory", k + 1); continue; }
			for (int j = 0; j < t.h; j++)
				for (int i = 0; i < t.w; i++) ((u32 *)t.px)[j * (t.pitch / 4) + i] = 0xffffffff;	// (all white: the layout doesn't matter)
			__sync_synchronize();
			tex_nearest = ways[k].nearest;
			tex_swz = ways[k].swz;
			cur_fp = FP_T0;
			q[0] = Test_Quad(0, 0, TW, TH, 0xffff0000);
			b = (gbatch_t){0, 1, &t, &t, 0, 0, TW, TH};
			int ok = Test_Draw(q, 1, &b, 1, 0x123456);
			u32 px = Test_Pixel(32, 32);
			Glog("test 3.%d: texture %s: read %06x%s", k + 1, ways[k].what, (unsigned)px, ok ? "" : " (timed out)");
			gpu_tex_free(&t);
			if (ok && Near(px, 0xff0000)) { found = k; break; }
		}
		tex_nearest = tex_swz = 0;
		tex_min_pitch = 64;
		if (found < 0) { snprintf(why, len, "no way of reading textures worked"); return 0; }
		if (ways[found].swz) { snprintf(why, len, "textures only work swizzled (%s): not done yet", ways[found].what); return 0; }
		tex_nearest = ways[found].nearest;
		tex_min_w = ways[found].size;
		tex_min_pitch = ways[found].pitch;
		if (tex_min_w > 4 && ways[found].pitch == 64) {
			snprintf(why, len, "textures only work at %dx%d and up: not done yet", tex_min_w, tex_min_w);
			return 0;
		}
		Glog("textures: %s", ways[found].what);
		// the test textures again, made the way that works
		gpu_tex_free(&white); gpu_tex_free(&pat); gpu_tex_free(&half);
		if (!gpu_tex_alloc(&white, 4, 4) || !gpu_tex_alloc(&pat, 4, 4) || !gpu_tex_alloc(&half, 4, 4)) { snprintf(why, len, "no memory"); return 0; }
		Fill_Test_Textures(&white, &pat, &half);
		// the programs again, the way that works
		for (int k = 2; k < 6; k++) {
			cur_fp = steps[k].fp;
			gtex_t t;
			if (!gpu_tex_alloc(&t, 4, 4)) break;
			for (int j = 0; j < t.h; j++)
				for (int i = 0; i < t.w; i++) ((u32 *)t.px)[j * (t.pitch / 4) + i] = 0xffffffff;
			__sync_synchronize();
			q[0] = Test_Quad(0, 0, TW, TH, 0xffff0000);
			b = (gbatch_t){0, 1, &t, &t, 0, 0, TW, TH};
			Test_Draw(q, 1, &b, 1, 0x123456);
			u32 px = Test_Pixel(32, 32);
			works[k] = Near(px, 0xff0000);
			Glog("test 2.%d again: read %06x (%s)", k + 1, (unsigned)px, works[k] ? "good" : "wanted ff0000");
			gpu_tex_free(&t);
		}
	}
	if (works[4]) cur_fp = FP_UI;
	else if (works[5]) cur_fp = FP_UI2;
	else { snprintf(why, len, "no program drew the red quad (see the steps above)"); return 0; }
	Glog("drawing with the \"%s\" program", fprogs[cur_fp].name);

	Glog("test 3: texture");
	q[0] = Test_Quad(0, 0, TW, TH, 0xffffffff);
	b = (gbatch_t){0, 1, &pat, &white, 0, 0, TW, TH};
	if (!Test_Draw(q, 1, &b, 1, 0)) { snprintf(why, len, "test 3 (texture) timed out"); return 0; }
	u32 p0 = Test_Pixel(16, 16), p1 = Test_Pixel(48, 16), p2 = Test_Pixel(16, 48), p3 = Test_Pixel(48, 48);
	if (!Near(p0, 0xff0000) || !Near(p1, 0x00ff00) || !Near(p2, 0x0000ff) || !Near(p3, 0xffffff)) {
		snprintf(why, len, "test 3 read %06x %06x %06x %06x", (unsigned)p0, (unsigned)p1, (unsigned)p2, (unsigned)p3);
		return 0;
	}

	Glog("test 4: second texture's alpha, and blending");
	q[0] = Test_Quad(0, 0, TW, TH, 0xffffffff);
	q[1] = Test_Quad(0, 0, TW, TH, 0x80ff0000);
	gbatch_t bb[2] = {{0, 1, &white, &half, 0, 0, TW, TH}, {1, 1, &white, &white, 0, 0, TW, TH}};
	if (!Test_Draw(q, 2, bb, 2, 0x00ff00)) { snprintf(why, len, "test 4 timed out"); return 0; }
	// left: green under half red; right: white under half red
	p0 = Test_Pixel(8, 32); p1 = Test_Pixel(56, 32);
	if (!Near(p0, 0x807f00) || !Near(p1, 0xff7f7f)) { snprintf(why, len, "test 4 read %06x %06x", (unsigned)p0, (unsigned)p1); return 0; }

	Glog("test 5: scissor");
	q[0] = Test_Quad(0, 0, TW, TH, 0xffff0000);
	b = (gbatch_t){0, 1, &white, &white, 0, 0, TW, TH / 2};
	scissor_flip = 0;
	if (!Test_Draw(q, 1, &b, 1, 0)) { snprintf(why, len, "test 5 timed out"); return 0; }
	p0 = Test_Pixel(32, 8); p1 = Test_Pixel(32, 56);
	if (Near(p0, 0x000000) && Near(p1, 0xff0000)) scissor_flip = 1;		// it counts from the bottom
	else if (!(Near(p0, 0xff0000) && Near(p1, 0x000000))) { snprintf(why, len, "test 5 read %06x %06x", (unsigned)p0, (unsigned)p1); return 0; }
	Glog("scissor from the %s", scissor_flip ? "bottom" : "top");

	gpu_tex_free(&white);
	gpu_tex_free(&pat);
	gpu_tex_free(&half);
	return 1;
}

static int Gpu_Init(void)
{
	char why[160] = "";
	Sync_Write(GPU_LOG, "", 1);
	Glog("init: display %dx%d", disp_w, disp_h);
	vb = rsxMemalign(128, MAX_VERTS * sizeof(vert_t));
	depth = rsxMemalign(128, disp_w * 4 * disp_h);
	test_px = rsxMemalign(128, TW * TH * 4);
	test_depth = rsxMemalign(128, TW * TH * 4);
	if (!vb || !depth || !test_px || !test_depth) { Glog("no RSX memory"); return 0; }
	if (rsxAddressToOffset(vb, &vb_off) || rsxAddressToOffset(depth, &depth_off) ||
		rsxAddressToOffset(test_px, &test_off) || rsxAddressToOffset(test_depth, &test_depth_off)) {
		Glog("no RSX offsets");
		return 0;
	}
	for (int i = 0; i < NUM_VP; i++) {
		u32 vpsize = 0;
		if (vprogs[i].vpo->magic != (('V' << 8) | 'P')) { Glog("bad vertex program %s", vprogs[i].name); return 0; }
		rsxVertexProgramGetUCode(vprogs[i].vpo, &vprogs[i].ucode, &vpsize);
		if (!vprogs[i].ucode || !vpsize) { Glog("empty vertex program %s", vprogs[i].name); return 0; }
	}
	for (int i = 0; i < NUM_FP; i++) {
		void *ucode = NULL;
		u32 size = 0;
		if (fprogs[i].fpo->magic != (('F' << 8) | 'P')) { Glog("bad fragment program %s", fprogs[i].name); return 0; }
		rsxFragmentProgramGetUCode(fprogs[i].fpo, &ucode, &size);
		void *mem = size ? rsxMemalign(64, size) : NULL;		// a fragment program runs from RSX memory
		if (!ucode || !mem || rsxAddressToOffset(mem, &fprogs[i].off)) { Glog("no RSX memory for program %s", fprogs[i].name); return 0; }
		memcpy(mem, ucode, size);
	}
	__sync_synchronize();
	if (!Self_Test(why, sizeof(why))) { Glog("self-test failed: %s", why); return 0; }
	Glog("self-test passed");
	return 1;
}

/*
==================
Start-up
==================
*/
// the display at a mode, and its two framebuffers (any old ones let go of first)
static int Setup_Display(s32 vid_res, u8 vid_aspect)
{
	videoResolution res;
	videoConfiguration vconfig;
	videoState state;
	if (videoGetResolution(vid_res, &res) != 0) {
		plat_log("video: videoGetResolution failed");
		return 0;
	}
	for (int i = 0; i < FB_COUNT; i++)
		if (fb[i]) { rsxFree(fb[i]); fb[i] = NULL; }
	disp_w = res.width;
	disp_h = res.height;
	disp_pitch = disp_w * 4;
	// a 4:3 picture: the 16:9 canvas across its full width is 3/4 of its height
	bar_h = vid_aspect == VIDEO_ASPECT_4_3 ? disp_h / 8 : 0;

	memset(&vconfig, 0, sizeof(vconfig));
	vconfig.resolution = vid_res;
	vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
	vconfig.pitch = disp_pitch;
	vconfig.aspect = vid_aspect;
	if (videoConfigure(0, &vconfig, NULL, 0) != 0) {
		plat_log("video: videoConfigure failed");
		return 0;
	}
	for (int waited = 0; waited < 300; waited++) {
		usleep(10000);
		if (videoGetState(0, 0, &state) != 0 || state.state != 3)
			break;
	}
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	for (int i = 0; i < FB_COUNT; i++) {
		fb[i] = (u32 *)rsxMemalign(1024 * 1024, disp_pitch * disp_h);
		if (!fb[i]) {
			plat_log("video: no RSX memory for framebuffer %d", i);
			return 0;
		}
		memset(fb[i], 0, disp_pitch * disp_h);
		rsxAddressToOffset(fb[i], &fb_offset[i]);
		gcmSetDisplayBuffer(i, fb_offset[i], disp_pitch, disp_w, disp_h);
	}
	cur = 0;
	return 1;
}

int ps3_video_init(void)
{
	s32 vid_res = VIDEO_RESOLUTION_720;
	u8 vid_aspect = VIDEO_ASPECT_16_9;
	videoState state;

	// L1 held while it starts: the CPU draws this time. R1: the RSX is tried again, even
	// after a freeze (the pads take a moment to answer)
	uint32_t held = 0;
	for (int i = 0; i < 15 && !(held & (BTN_L1 | BTN_R1)); i++) {
		held = plat_input_held();
		usleep(20000);
	}
	if (held & BTN_R1) {
		plat_log("video: R1 held: the RSX is tried again");
		plat_renderer_set_cpu(0);
		sysLv2FsUnlink(GPU_TRYING);
	}

	// the RSX, unless it froze last time (the marker's still there) or once before
	int try_gpu = !File_Exists(GPU_FAILED) && !File_Exists(USE_CPU);
	if (try_gpu && File_Exists(GPU_TRYING)) {
		sysLv2FsRename(GPU_TRYING, GPU_FAILED);
		plat_log("video: the RSX didn't make it through the last start: the CPU draws (hold R1 while it starts to try again)");
		try_gpu = 0;
	} else if (!try_gpu) {
		plat_log("video: the CPU draws (%s is there; hold R1 while it starts to try the RSX again)", GPU_FAILED);
	}
	if (try_gpu && (held & BTN_L1)) {
		plat_log("video: L1 held: the CPU draws this time");
		try_gpu = 0;
	}

	io_buffer = memalign(1024 * 1024, IO_SIZE);
	if (!io_buffer || rsxInit(&ctx, CB_SIZE, IO_SIZE, io_buffer) != 0 || !ctx) {
		plat_log("video: rsxInit failed");
		return 0;
	}
	videoGetState(0, 0, &state);
	int have_720 = videoGetResolutionAvailability(VIDEO_PRIMARY, VIDEO_RESOLUTION_720, VIDEO_ASPECT_16_9, 0);
	if (try_gpu) {
		// the RSX draws at whatever the PS3 is set to (1080p if it is)
		vid_res = state.displayMode.resolution;
		vid_aspect = state.displayMode.aspect;
	} else if (!have_720) {
		vid_res = state.displayMode.resolution;
		vid_aspect = state.displayMode.aspect;
		plat_log("video: 720p not available, using the TV's mode (%d)", (int)vid_res);
	}
	if (!Setup_Display(vid_res, vid_aspect))
		return 0;
	gcmSetFlipHandler(FlipHandler);
	flips_done = flips_queued = 0;

	if (try_gpu) {
		Sync_Write(GPU_TRYING, "trying the RSX\n", 1);
		if (Gpu_Init()) {
			gpu_mode = 1;
			gfx_gpu_enable((disp_h - 2 * bar_h) / (float)SCREEN_H);
			if (!gfx_gpu()) { Glog("no RSX memory for the atlas"); gpu_mode = 0; }
		}
		if (!gpu_mode) {
			sysLv2FsUnlink(GPU_TRYING);		// (failed cleanly: no freeze, try again next time)
			// the CPU's canvas is 1280x720: back to 720p if the TV takes it (copying it out
			// stretched to 1080p took 33 ms a frame)
			if (have_720 && (disp_w != SCREEN_W || disp_h != SCREEN_H)) {
				Finish(300000);
				if (!Setup_Display(VIDEO_RESOLUTION_720, VIDEO_ASPECT_16_9))
					return 0;
			}
		}
	}
	if (!gpu_mode) {
		if (disp_w != SCREEN_W || disp_h != SCREEN_H) {
			xmap = malloc(disp_w * sizeof(int));
			for (int x = 0; x < disp_w; x++)
				xmap[x] = x * SCREEN_W / disp_w;
		}
		// the CPU copies each finished frame into the framebuffer (an RSX copy
		// from IO-mapped memory was tried in 0.3.1: it hung the RSX at the first
		// frame and the screen stayed black)
		cpu_canvas = memalign(128, CANVAS_BYTES);
		if (!cpu_canvas) {
			plat_log("video: no memory for the canvas");
			return 0;
		}
	}
	ready = 1;
	plat_log("video: %dx%d%s, drawn by the %s", disp_w, disp_h, bar_h ? " (4:3, letterboxed)" : "", gpu_mode ? "RSX" : "CPU");
	return 1;
}

int ps3_video_gpu(void) { return gpu_mode; }

int plat_renderer_gpu_now(void) { return gpu_mode; }
int plat_renderer_cpu(void) { return File_Exists(USE_CPU) || File_Exists(GPU_FAILED); }
void plat_renderer_set_cpu(int cpu)
{
	if (cpu) {
		s32 fd = -1;
		if (sysLv2FsOpen(USE_CPU, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0644, NULL, 0) == 0 && fd >= 0) sysLv2FsClose(fd);
	} else {
		// the chip again: and a freeze last time is forgiven (it's tried, and checked, as at first)
		sysLv2FsUnlink(USE_CPU);
		sysLv2FsUnlink(GPU_FAILED);
	}
}

uint32_t *plat_canvas_next(void)
{
	return gpu_mode ? NULL : cpu_canvas;
}

void ps3_video_present(const uint32_t *src)
{
	if (!ready || gpu_mode || !src)
		return;		// (the RSX path flipped when the list was handed over)
	// the buffer we're about to fill must not be on its way to the screen
	Wait_Flips();

	u32 *dst = fb[cur];
	if (!xmap) {
		memcpy(dst, src, SCREEN_W * SCREEN_H * 4);
	} else {
		int ch = disp_h - 2 * bar_h;		// the rows the canvas fills (all, unless letterboxed)
		for (int y = 0; y < disp_h; y++) {
			u32 *d = dst + y * (disp_pitch / 4);
			if (y < bar_h || y >= bar_h + ch) {
				memset(d, 0, disp_w * 4);
				continue;
			}
			const uint32_t *row = src + ((y - bar_h) * SCREEN_H / ch) * SCREEN_W;
			for (int x = 0; x < disp_w; x++)
				d[x] = row[xmap[x]];
		}
	}
	Flip();
}

// shows the last frame again: while the XMB is up the system draws it on our
// flips, so they must keep coming (stopping them froze the screen with the
// XMB never appearing). Each one is a fresh copy of the last frame into the
// other buffer: re-flipping the same buffer let the overlay draw over what
// it had already drawn there, and it flickered. On the RSX path the last
// list is drawn again (nothing it uses has changed: textures are only let go
// of when the next frame starts).
void ps3_video_reflip(void)
{
	if (!ready || !flips_queued)
		return;
	if (gpu_mode) {
		if (!last_q) return;
		Wait_Flips();
		target_t tg = Screen_Target();
		Begin_Target(&tg, 0x000000);
		Draw_Batches(&tg, last_b, last_nb, last_nq);
		Flip();
		Frame_Good();
		return;
	}
	if (cpu_canvas) ps3_video_present(cpu_canvas);
}

void ps3_video_shutdown(void)
{
	if (!ready)
		return;
	ready = 0;
	if (gpu_mode)
		sysLv2FsUnlink(GPU_TRYING);		// it got to a clean quit: no freeze
	gcmSetFlipHandler(NULL);
	for (int waited = 0; flips_done != flips_queued && waited < 2000; waited++)
		usleep(100);
}
