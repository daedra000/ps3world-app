/*
input_ps3.c: DualShock 3 / DualShock 4 through PSL1GHT's ioPad. Bits of
padData.button[2..3] as NZ:P PS3's in_ps3.c (from TyrQuakeCell) reads them.
*/
#include <io/pad.h>
#include "../plat.h"
#include "../input.h"
#include "ps3.h"

#define MAX_PADS 7

#define P_L2       0x0001
#define P_R2       0x0002
#define P_L1       0x0004
#define P_R1       0x0008
#define P_TRIANGLE 0x0010
#define P_CIRCLE   0x0020
#define P_CROSS    0x0040
#define P_SQUARE   0x0080
#define P_SELECT   0x0100
#define P_L3       0x0200
#define P_R3       0x0400
#define P_START    0x0800
#define P_UP       0x1000
#define P_RIGHT    0x2000
#define P_DOWN     0x4000
#define P_LEFT     0x8000

static const struct { unsigned pad; uint32_t btn; } map[] = {
	{P_UP, BTN_UP}, {P_DOWN, BTN_DOWN}, {P_LEFT, BTN_LEFT}, {P_RIGHT, BTN_RIGHT},
	{P_CROSS, BTN_CROSS}, {P_CIRCLE, BTN_CIRCLE}, {P_SQUARE, BTN_SQUARE}, {P_TRIANGLE, BTN_TRIANGLE},
	{P_L1, BTN_L1}, {P_R1, BTN_R1}, {P_L2, BTN_L2}, {P_R2, BTN_R2},
	{P_START, BTN_START}, {P_SELECT, BTN_SELECT}, {P_L3, BTN_L3}, {P_R3, BTN_R3},
};

// ioPadGetData only has news when something changed: keep the last
static unsigned last_buttons[MAX_PADS];
static unsigned char last_lx[MAX_PADS], last_ly[MAX_PADS];

void ps3_input_init(void)
{
	plat_log("input: ioPadInit = %d", (int)ioPadInit(MAX_PADS));
	for (int i = 0; i < MAX_PADS; i++) {
		last_lx[i] = last_ly[i] = 128;
	}
}

uint32_t plat_input_held(void)
{
	padInfo info;
	padData data;
	uint32_t held = 0;

	if (ioPadGetInfo(&info) != 0)
		return 0;
	for (int p = 0; p < MAX_PADS; p++) {
		if (!info.status[p]) {
			last_buttons[p] = 0;
			last_lx[p] = last_ly[p] = 128;
			continue;
		}
		if (ioPadGetData(p, &data) == 0 && data.len > 0) {
			last_buttons[p] = ((unsigned)data.button[2] << 8) | (data.button[3] & 0xff);
			last_lx[p] = (unsigned char)data.button[6];
			last_ly[p] = (unsigned char)data.button[7];
		}
		for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
			if (last_buttons[p] & map[i].pad)
				held |= map[i].btn;
		// the left stick as a d-pad
		if (last_lx[p] < 40) held |= BTN_LEFT;
		if (last_lx[p] > 215) held |= BTN_RIGHT;
		if (last_ly[p] < 40) held |= BTN_UP;
		if (last_ly[p] > 215) held |= BTN_DOWN;
	}
	return held;
}
