#ifndef UI_H
#define UI_H

void ui_init(void);
void ui_update(void);			// input and background results, once a frame
void ui_render(void);
int ui_needs_redraw(void);
void ui_force_redraw(void);
void ui_shutdown(void);			// pauses a download (it resumes next time)

#endif
