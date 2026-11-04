/*
lang.c: tr() and the languages there are (the loading is i18n.c's).
*/
#include <string.h>
#include "lang.h"

static const char *const codes[LANG_COUNT] = {"en", "es", "pt", "fr", "de", "it", "nl", "pl", "tr", "da", "id", "ar", "ru"};
static const char *const names[LANG_COUNT] = {"English", "Español", "Português", "Français", "Deutsch", "Italiano",
	"Nederlands", "Polski", "Türkçe", "Dansk", "Bahasa Indonesia", "العربية", "Русский"};

static const lang_table_t *volatile table;
static volatile int current;

unsigned lang_hash(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
	return h;
}

const char *tr(const char *english)
{
	const lang_table_t *t = table;
	if (!t || !english) return english;
	for (unsigned i = lang_hash(english) & (t->size - 1), n = 0; n < (unsigned)t->size; i = (i + 1) & (t->size - 1), n++) {
		if (!t->en[i]) break;
		if (!strcmp(t->en[i], english)) return t->tx[i];
	}
	return english;
}

const char *lang_code(int i) { return i >= 0 && i < LANG_COUNT ? codes[i] : "en"; }
const char *lang_name(int i) { return i >= 0 && i < LANG_COUNT ? names[i] : "English"; }
int lang_current(void) { return current; }

void lang_install(int lang, const lang_table_t *t)
{
	table = t;
	current = lang;
}
