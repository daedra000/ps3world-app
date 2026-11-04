/*
i18n.c: which language the UI is in, and its words from data/lang/<code>.txt.

The PS3's own language unless one was picked in the app (USRDIR/language.txt:
a code, or "auto"). Arabic and Indonesian aren't PS3 system languages: they're
only there by picking them. (Russian, Chinese, Japanese and Korean are: they follow the PS3.)

A line of a language file is "English<TAB>translation"; "\n" in either is a
new line and a line starting with # is a comment. A translation whose printf
conversions (%s, %d...) aren't the English's, in the same order, is left out:
it would read the wrong arguments.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "lang.h"
#include "i18n.h"

static int choice = -1;		// -1: the PS3's language
static const lang_table_t *tables[LANG_COUNT];		// loaded once each, kept (tr() may be reading one)

// the conversions of a format, in order ("%s %d" -> "sd")
static void Specs(const char *s, char *out, int size)
{
	int n = 0;
	for (; *s && n < size - 1; s++) {
		if (*s != '%') continue;
		s++;
		if (*s == '%') continue;
		while (*s && strchr("-+ #0123456789.lhz", *s)) s++;
		if (!*s) break;
		out[n++] = *s;
	}
	out[n] = 0;
}

// "\n" -> a new line, "\\" -> "\", in place
static void Unescape(char *s)
{
	char *d = s;
	for (; *s; s++) {
		if (*s == '\\' && s[1] == 'n') { *d++ = '\n'; s++; }
		else if (*s == '\\' && s[1] == '\\') { *d++ = '\\'; s++; }
		else *d++ = *s;
	}
	*d = 0;
}

static const lang_table_t *Load(int lang)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/lang/%s.txt", plat_data_dir(), lang_code(lang));
	char *text = (char *)plat_read_file(path, NULL);		// (kept: the table's strings are in it)
	if (!text) {
		plat_log("lang: no %s: English", path);
		return NULL;
	}
	int lines = 1;
	for (const char *p = text; *p; p++) lines += *p == '\n';
	int size = 16;
	while (size < lines * 2) size *= 2;
	lang_table_t *t = calloc(1, sizeof(*t));
	const char **en = calloc(size, sizeof(char *)), **tx = calloc(size, sizeof(char *));
	if (!t || !en || !tx) {
		free(t); free(en); free(tx); free(text);
		return NULL;
	}
	t->size = size;
	t->en = en;
	t->tx = tx;
	int n = 0, bad = 0;
	for (char *line = text, *next; line && *line; line = next) {
		next = strchr(line, '\n');
		if (next) *next++ = 0;
		char *cr = strchr(line, '\r');
		if (cr) *cr = 0;
		char *tab = strchr(line, '\t');
		if (line[0] == '#' || !tab || !tab[1]) continue;
		*tab = 0;
		char *e = line, *x = tab + 1;
		Unescape(e);
		Unescape(x);
		char se[32], sx[32];
		Specs(e, se, sizeof(se));
		Specs(x, sx, sizeof(sx));
		if (strcmp(se, sx)) {
			plat_log("lang: %s: \"%s\" left out (its %% conversions differ)", lang_code(lang), e);
			bad++;
			continue;
		}
		unsigned i = lang_hash(e) & (size - 1);
		while (en[i] && strcmp(en[i], e)) i = (i + 1) & (size - 1);
		en[i] = e;
		tx[i] = x;
		n++;
	}
	plat_log("lang: %s, %d phrases%s", lang_code(lang), n, bad ? " (some left out)" : "");
	return t;
}

// the PS3's language (sysutil's numbering), as one of ours
int i18n_system(void)
{
	switch (plat_system_lang()) {
	case 0: return LANG_JA;
	case 2: return LANG_FR;
	case 3: return LANG_ES;
	case 4: return LANG_DE;
	case 5: return LANG_IT;
	case 6: return LANG_NL;
	case 7: case 17: return LANG_PT;
	case 8: return LANG_RU;
	case 9: return LANG_KO;
	case 10: case 11: return LANG_ZH;		// (traditional too: simplified is closer than English)
	case 14: return LANG_DA;
	case 16: return LANG_PL;
	case 19: return LANG_TR;
	}
	return LANG_EN;
}

static void Apply(void)
{
	int l = choice >= 0 ? choice : i18n_system();
	if (l != LANG_EN && !tables[l]) tables[l] = Load(l);
	lang_install(l, l == LANG_EN ? NULL : tables[l]);
}

void i18n_init(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/language.txt", plat_save_dir());
	char *s = (char *)plat_read_file(path, NULL);
	if (s) {
		for (int i = 0; i < LANG_COUNT; i++)
			if (!strncmp(s, lang_code(i), 2) && (s[2] == 0 || s[2] == '\n' || s[2] == '\r')) choice = i;
		free(s);
	}
	Apply();
}

int i18n_choice(void) { return choice; }

void i18n_set(int lang)
{
	char path[512];
	choice = lang >= 0 && lang < LANG_COUNT ? lang : -1;
	snprintf(path, sizeof(path), "%s/language.txt", plat_save_dir());
	const char *v = choice >= 0 ? lang_code(choice) : "auto";
	plat_write_file(path, v, strlen(v));
	Apply();
}
