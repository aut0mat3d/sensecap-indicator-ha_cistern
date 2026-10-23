/* main/util/media_standards.c */
#include "media_standards.h"
#include <string.h>

#define MAX_DYNAMIC_MEDIA 16

typedef struct {
    char id[16];
    uint32_t color_hex;
    char unit[12];
} media_entry_t;

static media_entry_t s_media_table[MAX_DYNAMIC_MEDIA];
static size_t s_media_count = 0;

void media_standards_clear(void) {
    s_media_count = 0;
    memset(s_media_table, 0, sizeof(s_media_table));
}

void media_standards_add(const char *id, uint32_t color_hex, const char *unit) {
    if (!id || s_media_count >= MAX_DYNAMIC_MEDIA) return;
    strncpy(s_media_table[s_media_count].id, id, sizeof(s_media_table[0].id) - 1);
    s_media_table[s_media_count].color_hex = color_hex;
    strncpy(s_media_table[s_media_count].unit, unit ? unit : "", sizeof(s_media_table[0].unit) - 1);
    s_media_count++;
}

uint32_t media_get_color(const char *medium_id) {
    if (medium_id) {
        for (size_t i = 0; i < s_media_count; i++) {
            if (strcmp(s_media_table[i].id, medium_id) == 0) {
                return s_media_table[i].color_hex;
            }
        }
    }
    return 0x7F8C8D; /* Neutraler Fallback-Grauton */
}

const char *media_get_unit(const char *medium_id) {
    if (medium_id) {
        for (size_t i = 0; i < s_media_count; i++) {
            if (strcmp(s_media_table[i].id, medium_id) == 0) {
                return s_media_table[i].unit;
            }
        }
    }
    return "";
}