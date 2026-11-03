#ifndef I18N_H
#define I18N_H

void i18n_init(void);		// the saved choice, or the PS3's language
int i18n_choice(void);		// a LANG_*, or -1 for the PS3's language
int i18n_system(void);		// the PS3's language, as a LANG_* (English if we haven't it)
void i18n_set(int lang);	// a LANG_*, or -1 for the PS3's; saved

#endif
