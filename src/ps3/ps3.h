#ifndef PS3_H
#define PS3_H

#include <stdint.h>

int ps3_video_init(void);
void ps3_video_present(const uint32_t *canvas);
void ps3_video_reflip(void);		// the last frame again (keeps the XMB alive)
int ps3_video_gpu(void);			// the RSX draws the UI
void ps3_video_shutdown(void);

void ps3_input_init(void);

extern int ps3_net_ok;

#include <ppu-types.h>
void ps3_osk_event(u64 status);		// the sysutil callback passes OSK events on

#endif
