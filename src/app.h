#ifndef APP_H
#define APP_H

#include <stdint.h>

int app_start(void);
uint32_t *app_frame(int *drawn);	// runs a frame; *drawn says whether the canvas was redrawn
void app_background(void);		// while the system menu is up (downloads run by themselves)
void app_redraw(void);			// draw the next frame whatever happens
void app_shutdown(void);

void http_init(void);			// in the backend's http file

#endif
