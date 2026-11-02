#ifndef LANG_H
#define LANG_H

// the UI's words: English only in v1. tr() stays so the code taken from
// Resonant (http_ps3.c, font.c) reads the same.
const char *tr(const char *english);
#define N_(s) s

const char *lang_code(int i);		// "en"
int lang_current(void);				// 0

#endif
