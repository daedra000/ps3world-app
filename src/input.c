/*
input.c: presses and auto-repeat on top of the backend's held buttons.
*/
#include "plat.h"
#include "input.h"

#define REPEAT_MASK (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)		// not L1/R1: held, they would skip track after track
#define REPEAT_DELAY_US 380000
#define REPEAT_RATE_US 70000

static uint32_t held, pressed, fresh;
static uint64_t since[16], last_rep[16];

void input_update(void)
{
	uint32_t now = plat_input_held();
	// a console that confirms with O: the app's "X" is that button, its "O" the other
	if (plat_circle_confirms() && ((now & BTN_CROSS) != 0) != ((now & BTN_CIRCLE) != 0))
		now ^= BTN_CROSS | BTN_CIRCLE;
	uint64_t t = plat_time_us();
	pressed = now & ~held;
	fresh = pressed;
	for (int b = 0; b < 16; b++) {
		uint32_t bit = 1u << b;
		if (pressed & bit) {
			since[b] = t;
			last_rep[b] = t;
		} else if ((now & bit) && (bit & REPEAT_MASK)) {
			// held longer, it speeds up: long lists don't take all day
			uint64_t held_for = t - since[b];
			uint64_t rate = held_for > 2500000 ? REPEAT_RATE_US / 3 : held_for > 1200000 ? REPEAT_RATE_US / 2 : REPEAT_RATE_US;
			if (held_for > REPEAT_DELAY_US && t - last_rep[b] > rate) {
				pressed |= bit;
				last_rep[b] = t;
			}
		}
	}
	held = now;
}

uint32_t input_pressed(void) { return pressed; }
uint32_t input_held(void) { return held; }
uint32_t input_fresh(void) { return fresh; }
