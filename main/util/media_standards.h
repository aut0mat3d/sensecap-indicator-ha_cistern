#ifndef MEDIA_STANDARDS_H
#define MEDIA_STANDARDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    const char *name_de;
    const char *name_en;
    uint32_t    color_hex;
    const char *unit_default;
} media_standard_t;

/**
 * @brief Get primary color for a given medium ID.
 * @param medium_id Medium identifier string (e.g. "water", "oil")
 * @return LVGL-compatible 24-bit hex color code
 */
uint32_t media_get_color(const char *medium_id);

/**
 * @brief Get default unit string for a given medium ID.
 */
const char *media_get_unit(const char *medium_id);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_STANDARDS_H */