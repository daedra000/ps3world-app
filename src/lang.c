/*
lang.c: English only (v1).
*/
#include "lang.h"

const char *tr(const char *english) { return english; }
const char *lang_code(int i) { (void)i; return "en"; }
int lang_current(void) { return 0; }
