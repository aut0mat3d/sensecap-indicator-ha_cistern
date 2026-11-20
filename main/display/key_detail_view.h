/**
 * @file key_detail_view.h
 * @brief Full-screen detail page for data points of one device:
 *        - one key: long-term trend graph and current value, time of the value, status, expected
 *          interval and device (opened e.g. by tapping the battery indicator in the header);
 *        - two keys: two coupled trend graphs (same time range, cursor at the same time), e.g.
 *          flow rate and level of a cistern (opened by tapping its level history).
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

#define KEY_DETAIL_MAX_TRENDS 2

/** Time range presets of the trend graphs (index for key_detail_opts_t.preset_idx) */
enum { KEY_DETAIL_PRESET_24H = 0, KEY_DETAIL_PRESET_7D, KEY_DETAIL_PRESET_1M };

/** One trend graph of the page */
typedef struct {
    const char *key;
    bool        include_zero;   ///< Automatic Y scale always contains 0 (signed rates)
} key_detail_trend_t;

typedef struct {
    uint32_t           dev_addr;
    key_detail_trend_t trends[KEY_DETAIL_MAX_TRENDS];
    uint8_t            trend_count;  ///< 1: graph 10 x 5 + info rows, Y scale from limits[key];
                                     ///< 2: two graphs 10 x 4, automatic Y scale, window min / max lines
    uint8_t            preset_idx;   ///< Initial time range (KEY_DETAIL_PRESET_*)
} key_detail_opts_t;

/**
 * @brief Open the detail page (closes an open one first).
 *        Call from LVGL context or with the LVGL lock held.
 */
void key_detail_view_show_ex(const key_detail_opts_t *opts);

/** Single key page starting with the 31 day range (e.g. battery) */
void key_detail_view_show(uint32_t dev_addr, const char *key);

/** Close the page (no-op if not open) */
void key_detail_view_hide(void);

/** true while the page is open */
bool key_detail_view_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* KEY_DETAIL_VIEW_H */
