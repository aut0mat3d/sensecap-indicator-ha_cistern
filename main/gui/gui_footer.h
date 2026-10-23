/**
 * @file gui_footer.h
 * @brief Permanent bottom status bar for 10x8 grid system (480x480 display).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef GUI_FOOTER_H
#define GUI_FOOTER_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FOOTER_HEIGHT   27
#define FOOTER_POS_Y    453

/**
 * @brief Initialize the global footer container on the base screen.
 * @param parent Root display object (tileview or active screen).
 */
void gui_footer_init(lv_obj_t *parent);

/**
 * @brief Update the left-aligned status text (e.g. IP address, WiFi state).
 * @param text Status string.
 */
void gui_footer_set_left_text(const char *text);

/**
 * @brief Update the right-aligned status text (e.g. Last update timestamp).
 * @param text Timestamp or diagnostic string.
 */
void gui_footer_set_right_text(const char *text);

/**
 * @brief Set text color for alerts/alarms (e.g. red for alarm active).
 * @param color Target text color.
 */
void gui_footer_set_color(lv_color_t color);

/**
 * @brief Show or hide the footer (e.g. for full-screen dialogs).
 * @param visible True to show, false to hide.
 */
void gui_footer_set_visible(bool visible);

#ifdef __cplusplus
}
#endif

#endif /* GUI_FOOTER_H */