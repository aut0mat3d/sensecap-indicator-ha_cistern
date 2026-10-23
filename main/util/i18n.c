/* main/util/i18n.c */
#include "i18n.h"
#include <string.h>

#define MAX_I18N_ENTRIES 64

typedef struct {
    char page_id[24];
    char key[24];
    char text[48];
} i18n_entry_t;

static i18n_entry_t s_i18n_table[MAX_I18N_ENTRIES];
static size_t s_i18n_count = 0;
static char s_current_lang[8] = "de";

void i18n_clear(void) {
    s_i18n_count = 0;
}

void i18n_add_entry(const char *page_id, const char *key, const char *text) {
    if (!key || !text || s_i18n_count >= MAX_I18N_ENTRIES) return;
    strncpy(s_i18n_table[s_i18n_count].page_id, page_id ? page_id : "global", sizeof(s_i18n_table[0].page_id) - 1);
    strncpy(s_i18n_table[s_i18n_count].key, key, sizeof(s_i18n_table[0].key) - 1);
    strncpy(s_i18n_table[s_i18n_count].text, text, sizeof(s_i18n_table[0].text) - 1);
    s_i18n_count++;
}

void i18n_set_language(const char *lang_code) {
    if (lang_code) strncpy(s_current_lang, lang_code, sizeof(s_current_lang) - 1);
}

const char *i18n_get(const char *page_id, const char *key) {
    if (!key) return "";
    
    /* 1. Page-Spezifische Suche */
    if (page_id) {
        for (size_t i = 0; i < s_i18n_count; i++) {
            if (strcmp(s_i18n_table[i].page_id, page_id) == 0 && strcmp(s_i18n_table[i].key, key) == 0) {
                return s_i18n_table[i].text;
            }
        }
    }
    
    /* 2. Globale Suche */
    for (size_t i = 0; i < s_i18n_count; i++) {
        if (strcmp(s_i18n_table[i].page_id, "global") == 0 && strcmp(s_i18n_table[i].key, key) == 0) {
            return s_i18n_table[i].text;
        }
    }

    /* 3. Fallback: Key selbst zurückgeben */
    return key;
}