/**
 * @file key_detail_view.h
 * @brief Full-screen detail page for one data point (dev_addr + key): long-term trend graph and
 *        current value, time of the value, status, expected interval and device.
 *
 * Generic for any key; opened e.g. by tapping the battery indicator in the header.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#ifndef KEY_DETAIL_VIEW_H
#define KEY_DETAIL_VIEW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open the detail page for a data point (closes an open one first).
 *        Call from LVGL context or with the LVGL lock held.
 */
void key_detail_view_show(uint32_t dev_addr, const char *key);

/** Close the page (no-op if not open) */
void key_detail_view_hide(void);

/** true while the page is open */
bool key_detail_view_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* KEY_DETAIL_VIEW_H */
