/*
osk_ps3.c: the system's on-screen keyboard (sysutil OSK), for Search.
As PSL1GHT's samples/sys/osk: load it into its own memory container, keep
flipping (the system draws it over our frames), and collect the text when
the sysutil callback says it's done.
*/
#include <stdio.h>
#include <string.h>
#include <sys/memory.h>
#include <sysutil/osk.h>
#include <sysutil/sysutil.h>
#include "../plat.h"
#include "ps3.h"

#define OSK_LEN 128

static uint16_t title16[OSK_LEN], start16[OSK_LEN], result16[OSK_LEN + 1];
static oskInputFieldInfo field;
static oskParam param;
static oskCallbackReturnParam ret;
static sys_mem_container_t container;
static volatile int state;		// 0 idle, 1 open, 2 entered, 3 cancelled
static int have_container;

static void To16(const char *s, uint16_t *d, int max)
{
	const unsigned char *p = (const unsigned char *)s;
	int o = 0;
	while (*p && o < max - 1) {
		uint32_t c = *p++;
		if (c >= 0xe0 && p[0] && p[1]) { c = ((c & 0x0f) << 12) | ((p[0] & 0x3f) << 6) | (p[1] & 0x3f); p += 2; }
		else if (c >= 0xc0 && p[0]) { c = ((c & 0x1f) << 6) | (p[0] & 0x3f); p += 1; }
		d[o++] = (uint16_t)c;
	}
	d[o] = 0;
}

static void To8(const uint16_t *s, char *d, int size)
{
	int o = 0;
	for (; *s && o < size - 4; s++) {
		uint32_t c = *s;
		if (c < 0x80) d[o++] = (char)c;
		else if (c < 0x800) { d[o++] = 0xc0 | (c >> 6); d[o++] = 0x80 | (c & 0x3f); }
		else { d[o++] = 0xe0 | (c >> 12); d[o++] = 0x80 | ((c >> 6) & 0x3f); d[o++] = 0x80 | (c & 0x3f); }
	}
	d[o] = 0;
}

// from main_ps3.c's sysutil callback
void ps3_osk_event(u64 status)
{
	switch (status) {
	case SYSUTIL_OSK_INPUT_CANCELED:
		oskAbort();
		// fall through: it still has to be unloaded
	case SYSUTIL_OSK_DONE:
		oskUnloadAsync(&ret);
		break;
	case SYSUTIL_OSK_UNLOADED:
		state = (ret.res == OSK_OK) ? 2 : 3;
		if (have_container) {
			sysMemContainerDestroy(container);
			have_container = 0;
		}
		break;
	}
}

int plat_osk_open(const char *title, const char *initial)
{
	s32 r;
	if (state == 1) return 0;
	memset(result16, 0, sizeof(result16));
	To16(title, title16, OSK_LEN);
	To16(initial ? initial : "", start16, OSK_LEN);
	field.message = title16;
	field.startText = start16;
	field.maxLength = OSK_LEN - 1;
	param.allowedPanels = OSK_PANEL_TYPE_DEFAULT;
	param.firstViewPanel = OSK_PANEL_TYPE_DEFAULT;
	param.controlPoint = (oskPoint){0, 0};
	param.prohibitFlags = OSK_PROHIBIT_RETURN;
	ret.res = OSK_OK;
	ret.len = OSK_LEN;
	ret.str = result16;
	if ((r = sysMemContainerCreate(&container, 4 * 1024 * 1024)) != 0) {
		plat_log("osk: sysMemContainerCreate = 0x%08x", (unsigned)r);
		return 0;
	}
	have_container = 1;
	oskSetInitialInputDevice(OSK_DEVICE_PAD);
	oskSetKeyLayoutOption(OSK_FULLKEY_PANEL);
	oskSetLayoutMode(OSK_LAYOUTMODE_HORIZONTAL_ALIGN_CENTER | OSK_LAYOUTMODE_VERTICAL_ALIGN_CENTER);
	if ((r = oskLoadAsync(container, &param, &field)) != 0) {
		plat_log("osk: oskLoadAsync = 0x%08x", (unsigned)r);
		sysMemContainerDestroy(container);
		have_container = 0;
		return 0;
	}
	state = 1;
	return 1;
}

int plat_osk_active(void)
{
	return state == 1;
}

int plat_osk_poll(char *out, int size)
{
	if (state == 2) {
		To8(result16, out, size);
		state = 0;
		return 1;
	}
	if (state == 3) {
		state = 0;
		return -1;
	}
	return 0;
}
