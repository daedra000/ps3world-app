#ifndef LANG_H
#define LANG_H

/*
lang.h: the UI's words in the user's language.

tr("English text") gives the current language's version of it, or the English
when there's none. The translations are data/lang/<code>.txt, loaded by
i18n.c (the PS3 build); a table, once in use, is never changed or freed, so
worker threads can call tr() while the language is switched.
*/
const char *tr(const char *english);
#define N_(s) s

enum { LANG_EN, LANG_ES, LANG_PT, LANG_FR, LANG_DE, LANG_IT, LANG_NL, LANG_PL, LANG_TR, LANG_DA, LANG_ID, LANG_AR, LANG_RU, LANG_ZH, LANG_JA, LANG_KO, LANG_COUNT };

const char *lang_code(int i);		// "en", "es"...
const char *lang_name(int i);		// in its own language: "Español"...
const char *lang_english(int i);	// in English: "Spanish"...
const char *lang_flag(int i);		// its flag's file in data/flags: "es"...
int lang_current(void);

// a loaded table: pairs of English and its translation, hashed (i18n.c builds it)
typedef struct {
	int size;					// slots, a power of two
	const char **en, **tx;		// NULL en: an empty slot
} lang_table_t;
void lang_install(int lang, const lang_table_t *t);		// t NULL: English
unsigned lang_hash(const char *s);

#endif
