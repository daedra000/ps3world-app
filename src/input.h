#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

enum {
	BTN_UP = 1 << 0, BTN_DOWN = 1 << 1, BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
	BTN_CROSS = 1 << 4, BTN_CIRCLE = 1 << 5, BTN_SQUARE = 1 << 6, BTN_TRIANGLE = 1 << 7,
	BTN_L1 = 1 << 8, BTN_R1 = 1 << 9, BTN_L2 = 1 << 10, BTN_R2 = 1 << 11,
	BTN_START = 1 << 12, BTN_SELECT = 1 << 13, BTN_L3 = 1 << 14, BTN_R3 = 1 << 15,
};

// the backend: buttons held now on any pad (the left stick counts as the d-pad)
uint32_t plat_input_held(void);

void input_update(void);
// pressed this frame; the d-pad and L1/R1 repeat while held
uint32_t input_pressed(void);
uint32_t input_held(void);
// pressed this frame by a real press, not auto-repeat: for moves that leave an area
// (into the bar or the sidebar), so holding a direction stops at the edge
uint32_t input_fresh(void);

#endif
