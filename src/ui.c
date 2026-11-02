/*
ui.c: the screens.

Phase 3: one status screen. The catalogue is fetched over HTTPS on a worker
thread, parsed, and its item count shown and logged (X tries again after an
error).
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
#include "version.h"
#include "ui.h"

#define C_TEXT		0xffffffff
#define C_DIM		0xff9aa4b8
#define C_ERR		0xffff7a7a

static int dirty = 1;
static image_t *backdrop, *logo;

static catalog_t *catalog;
static job_t *fetch_job;
static char status[200];
static int status_err;

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

/*
==================
Catalogue
==================
*/
static void Fetch_Run(job_t *j)
{
	http_res_t res;
	size_t len = 0;
	uint64_t t0 = plat_time_us();
	char *body = http_fetch(NULL, CATALOG_URL, NULL, NULL, 30, &j->cancel, &len, &res);

	if (!body) {
		snprintf(j->err, sizeof(j->err), "%s", res.err[0] ? res.err : "The catalogue didn't download");
		return;
	}
	if (res.status != 200) {
		snprintf(j->err, sizeof(j->err), "The catalogue wasn't found (HTTP %d)", res.status);
		free(body);
		return;
	}
	plat_log("catalog: %lu bytes in %.1f s", (unsigned long)len, (plat_time_us() - t0) / 1e6);
	j->result = catalog_parse(body, len, j->err, sizeof(j->err));
	free(body);
}

static void Fetch_Cleanup(job_t *j)
{
	catalog_free(j->result);
}

static void Fetch_Start(void)
{
	snprintf(status, sizeof(status), "Loading the catalogue...");
	status_err = 0;
	fetch_job = job_new(Fetch_Run, Fetch_Cleanup);
	if (fetch_job) job_post(fetch_job, LANE_API);
	dirty = 1;
}

static void Fetch_Tick(void)
{
	if (!job_done(fetch_job))
		return;
	if (fetch_job->result) {
		catalog_free(catalog);
		catalog = fetch_job->result;
		fetch_job->result = NULL;
		plat_log("catalog: %d items (%d homebrew, %d themes, %d avatars), generated %s",
			catalog->count, catalog->per_type[TYPE_HOMEBREW], catalog->per_type[TYPE_THEME],
			catalog->per_type[TYPE_AVATAR], catalog->generated);
		snprintf(status, sizeof(status), "%d items: %d homebrew, %d themes, %d avatars",
			catalog->count, catalog->per_type[TYPE_HOMEBREW], catalog->per_type[TYPE_THEME], catalog->per_type[TYPE_AVATAR]);
	} else {
		plat_log("catalog: %s", fetch_job->err);
		snprintf(status, sizeof(status), "%s", fetch_job->err);
		status_err = 1;
	}
	job_release(fetch_job);
	fetch_job = NULL;
	dirty = 1;
}

/*
==================
Frame
==================
*/
void ui_init(void)
{
	backdrop = Load_Image("backdrop.png", SCREEN_W, SCREEN_H);
	logo = Load_Image("logo.png", 360, 51);
	Fetch_Start();
}

void ui_update(void)
{
	input_update();
	Fetch_Tick();
	if (status_err && !fetch_job && (input_pressed() & BTN_CROSS))
		Fetch_Start();
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

void ui_render(void)
{
	if (backdrop) gfx_image(backdrop, 0, 0, SCREEN_W, SCREEN_H, 0, 255);
	else gfx_clear(0xff05070d);
	if (logo) gfx_image(logo, (SCREEN_W - 360) / 2, 230, 360, 51, 0, 255);
	font_draw_center(FONT_MEDIUM, 26, SCREEN_W / 2, 330, status, status_err ? C_ERR : C_TEXT, SCREEN_W - 160);
	if (status_err)
		font_draw_center(FONT_REGULAR, 22, SCREEN_W / 2, 380, "Press X to try again", C_DIM, 0);
	font_draw_right(FONT_REGULAR, 18, SCREEN_W - 48, SCREEN_H - 44, APP_NAME " " APP_VERSION, C_DIM);
}
