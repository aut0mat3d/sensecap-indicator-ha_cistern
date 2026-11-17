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

/**
 * @brief Load /gui/media_standards.json from the SD card (blocking UART file request).
 *        Must not be called from the LVGL task. Increments the revision on success.
 * @return Number of media types loaded, or -1 on error (current table is kept).
 */
int media_standards_load_from_sd(void);

/**
 * @brief Revision counter, incremented on every successful load.
 *        Views compare it with their last applied revision to re-apply colors.
 */
uint32_t media_standards_get_revision(void);

void media_standards_clear(void);
void media_standards_add(const char *id, uint32_t color_hex, const char *unit);

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_STANDARDS_H */