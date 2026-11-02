/*
app.c: start-up and the frame.
*/
#include <stdlib.h>
#include "plat.h"
#include "gfx.h"
#include "font.h"
#include "jobs.h"
#include "ui.h"
#include "version.h"
#include "app.h"

static uint32_t *canvas;

int app_start(void)
{
	plat_log(APP_NAME " " APP_VERSION " starting");
	canvas = plat_canvas_next();		// (none when the GPU draws)
	if (!canvas && !gfx_gpu()) {
		plat_log("FATAL: no canvas");
		return 0;
	}
	gfx_init(canvas, SCREEN_W, SCREEN_H);
	if (!gfx_gpu()) gfx_clear(0xff05070d);
	if (!font_init())
		plat_log("WARNING: fonts missing from %s/fonts", plat_data_dir());
	http_init();
	jobs_init();
	ui_init();
	return 1;
}

static uint64_t t_render, t_frames, t_window;

uint32_t *app_frame(int *drawn)
{
	ui_update();
	*drawn = 0;
	if (gfx_wants_redraw()) ui_force_redraw();		// (the GPU's list ran out of room last time)
	if (ui_needs_redraw()) {
		uint64_t t0 = plat_time_us();
		canvas = plat_canvas_next();		// the one not being shown (NULL when the GPU draws)
		gfx_frame_begin();
		gfx_init(canvas, SCREEN_W, SCREEN_H);
		ui_render();
		gfx_frame_end();
		t_render += plat_time_us() - t0;
		t_frames++;
		*drawn = 1;
	}
	// how long drawing takes, every 10 s (for the log)
	uint64_t now = plat_time_us();
	if (!t_window) t_window = now;
	if (now - t_window > 10000000) {
		if (t_frames)
			plat_log("frames: redrawn %llu times in 10 s (only on changes), %.1f ms to draw each",
				(unsigned long long)t_frames, t_render / 1000.0 / t_frames);
		t_render = t_frames = 0;
		t_window = now;
	}
	return canvas;
}

void app_background(void)
{
}

void app_redraw(void)
{
	ui_force_redraw();
}

void app_shutdown(void)
{
	ui_shutdown();
	plat_log("bye");
}
