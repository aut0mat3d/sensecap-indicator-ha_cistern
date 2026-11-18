/**
 * @file gui_theme.h
 * @brief Language independent GUI colors (alarm severities, alarm journal states).
 *
 * Built-in defaults, overridable from /gui/theme.json on the SD card (read via the RP2040 file
 * proxy, reloaded together with the language files). All alarm related coloring of widgets,
 * the alarm list and the header goes through this module.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#ifndef GUI_THEME_H
#define GUI_THEME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Alarm level of a displayed value (ordered: a higher level wins) */
typedef enum {
    GUI_ALARM_NONE    = 0,
    GUI_ALARM_WARNING = 1,
    GUI_ALARM_DANGER  = 2,
} gui_alarm_level_t;

/** Theme colors as 0xRRGGBB */
typedef struct {
    uint32_t alarm_danger;
    uint32_t alarm_warning;
    uint32_t alarm_info;
    uint32_t alarm_ok;            /* level indicators (e.g. battery) when no alarm level is reached */
    uint32_t state_incoming;
    uint32_t state_going;
    uint32_t state_muted;
    uint32_t state_retriggered;
    uint32_t state_acknowledged;
} gui_theme_colors_t;

/** Copy of the current theme colors */
void gui_theme_get_colors(gui_theme_colors_t *out);

/**
 * @brief Color for an alarm level.
 * @param level      Alarm level of the value.
 * @param none_color Color returned for GUI_ALARM_NONE (e.g. the widget's normal color).
 */
uint32_t gui_theme_alarm_level_color(gui_alarm_level_t level, uint32_t none_color);

/**
 * @brief Color for an alarm severity as used in thresholds and the alarm journal
 *        (0 = danger, 1 = warning, 2 = info, see alarm_severity_t).
 */
uint32_t gui_theme_severity_color(uint8_t severity);

/**
 * @brief Map a threshold severity (0 = danger, 1 = warning, 2 = info) to a display alarm level.
 *        Info thresholds are shown like warnings.
 */
gui_alarm_level_t gui_theme_level_from_severity(uint8_t severity);

/**
 * @brief Load /gui/theme.json from the SD card. Missing keys keep their built-in default.
 * @return 0 on success, -1 if the file is missing or invalid (defaults stay active).
 */
int gui_theme_load_from_sd(void);

/** Incremented on every successful load (late binding in views) */
uint32_t gui_theme_get_revision(void);

/**
 * @brief Log a warning for every medium whose main color is hard to tell apart from an alarm
 *        color (danger, warning). Call after the theme and the media table are loaded.
 */
void gui_theme_check_media_contrast(void);

#ifdef __cplusplus
}
#endif

#endif /* GUI_THEME_H */
