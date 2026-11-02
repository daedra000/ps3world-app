#ifndef FONT_H
#define FONT_H

#include <stdint.h>

enum { FONT_REGULAR, FONT_MEDIUM, FONT_BOLD, FONT_COUNT };

int font_init(void);	// 0 if the bundled fonts are missing
// draws UTF-8 text with its top at y; cut with "…" past maxw (0: no limit). Returns the width.
int font_draw(int font, int px, int x, int y, const char *s, uint32_t argb, int maxw);
int font_width(int font, int px, const char *s);
int font_line_height(int font, int px);
// draws text centred on cx
int font_draw_center(int font, int px, int cx, int y, const char *s, uint32_t argb, int maxw);
void font_draw_glyph_centered(int font, int px, int cx, int cy, const char *ch, uint32_t argb);
int font_draw_right(int font, int px, int rx, int y, const char *s, uint32_t argb);
void font_flush(void);		// forget the cached glyphs

#endif
