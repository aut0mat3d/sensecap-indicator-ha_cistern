/**
 * @file text_widget.h
 * @brief Flexible multi-row text and telemetry card module.
 *        Supports vertical auto-scrolling, word-wrapping, dynamic headings,
 *        individual binding keys, and the generic widget_base interface.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef TEXT_WIDGET_H
#define TEXT_WIDGET_H

#include "lvgl.h"
#include "widget_base.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Maximum number of label-value pairs in a single text widget card
 */
#define TEXT_WIDGET_MAX_PAIRS 8

/**
 * @brief Configuration for a single text value pair / item
 */
typedef struct {
    char        binding_key[32];   ///< Channel/binding key (e.g. "depth", "volume", "flowrate")
    const char* label;             ///< Label text (e.g. "Füllstand:", "Flow Rate:")
    const char* value;             ///< Initial or static value text (e.g. "3.42 m³")
    const char* unit;              ///< Optional engineering unit string (e.g. "m³", "cm")
    lv_color_t  label_color;       ///< Color for label text
    lv_color_t  value_color;       ///< Color for value text
    const lv_font_t* font;         ///< Font override for value (optional, e.g. &ui_font_font2)
} text_widget_pair_t;

/**
 * @brief Configuration structure for text widget module
 */
typedef struct {
    lv_obj_t* parent;           ///< Parent LVGL object (tile or view container)
    int16_t x;                  ///< X position in pixels
    int16_t y;                  ///< Y position in pixels
    uint16_t width;             ///< Width in pixels
    uint16_t height;            ///< Height in pixels
    
    const char* title;          ///< Title or card header text (optional)
    text_widget_pair_t pairs[TEXT_WIDGET_MAX_PAIRS];  ///< Array of label-value pairs
    uint8_t pair_count;         ///< Number of pairs configured
    
    const lv_font_t* font;      ///< Default font for labels and values
    lv_color_t bg_color;        ///< Background card color
    lv_color_t text_color;      ///< Default text color
    lv_color_t title_color;     ///< Title color
    
    lv_event_cb_t click_cb;     ///< Click callback for configuration subpage
    void* user_data;            ///< User data passed to click callback
    
    /* Internal LVGL state */
    lv_obj_t* container;        ///< Scrollable card container
    lv_obj_t* lbl_title;        ///< Title label object (optional)
    lv_obj_t* pair_conts[TEXT_WIDGET_MAX_PAIRS]; ///< Row containers for each pair
    lv_obj_t* labels[TEXT_WIDGET_MAX_PAIRS];     ///< Label text objects
    lv_obj_t* values[TEXT_WIDGET_MAX_PAIRS];     ///< Value text objects
} text_widget_t;

/**
 * @brief Factory function for dynamic instantiation via Page Manager.
 * @param parent Parent LVGL object.
 * @param id Unique widget ID from JSON.
 * @param binding_key Optional default binding key.
 * @param x Pixel X coordinate.
 * @param y Pixel Y coordinate.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return Generic widget_t pointer, or NULL on failure.
 */
widget_t* text_widget_create(lv_obj_t *parent, const char *id, const char *binding_key,
                             int16_t x, int16_t y, uint16_t w, uint16_t h);

/**
 * @brief Initialize a pre-allocated text widget module (backward compatibility).
 * @param widget Pointer to widget configuration structure.
 * @return 0 on success, -1 on error.
 */
int text_widget_init(text_widget_t* widget);

/**
 * @brief Add a label-value pair dynamically to the widget.
 * @param widget Pointer to widget configuration structure.
 * @param binding_key Key matching live telemetry (e.g. "volume", "flowrate").
 * @param label Human-readable label (e.g. "Füllstand:").
 * @param initial_value Starting value string (e.g. "--").
 * @param unit Unit string (optional).
 * @param value_color Accent color for the value text.
 * @return 0 on success, -1 if maximum pairs reached.
 */
int text_widget_add_pair(text_widget_t *widget, const char *binding_key, const char *label,
                         const char *initial_value, const char *unit, lv_color_t value_color);

/**
 * @brief Update a value by index (backward compatibility).
 * @param widget Pointer to widget configuration structure.
 * @param pair_index Index of the pair to update (0-based).
 * @param value New value text string.
 */
void text_widget_update_value(text_widget_t* widget, uint8_t pair_index, const char* value);

/**
 * @brief Update telemetry value across all pairs matching a specific binding key.
 * @param widget Pointer to widget configuration structure.
 * @param key Target telemetry key (e.g. "depth", "volume").
 * @param value New numeric float value.
 * @param unit Unit string (optional, NULL to use pair default).
 * @param is_estimated True if value is an interpolated replacement.
 * @param is_alarm True if value violates an active alarm threshold.
 */
void text_widget_update_binding(text_widget_t *widget, const char *key, float value,
                                const char *unit, bool is_estimated, bool is_alarm);

/**
 * @brief Update label text by index.
 * @param widget Pointer to widget configuration structure.
 * @param pair_index Index of the pair to update (0-based).
 * @param label New label text.
 */
void text_widget_update_label(text_widget_t* widget, uint8_t pair_index, const char* label);

/**
 * @brief Set or update title for the text widget card.
 * @param widget Pointer to widget configuration structure.
 * @param title New title text (NULL to hide).
 */
void text_widget_set_title(text_widget_t* widget, const char* title);

/**
 * @brief Set value color for a specific pair.
 * @param widget Pointer to widget configuration structure.
 * @param pair_index Index of the pair to update (0-based).
 * @param color New color for the value text.
 */
void text_widget_set_value_color(text_widget_t* widget, uint8_t pair_index, lv_color_t color);

/**
 * @brief Set label color for a specific pair.
 * @param widget Pointer to widget configuration structure.
 * @param pair_index Index of the pair to update (0-based).
 * @param color New color for the label text.
 */
void text_widget_set_label_color(text_widget_t* widget, uint8_t pair_index, lv_color_t color);

/**
 * @brief Destroy/cleanup text widget and free internal LVGL objects.
 * @param widget Pointer to widget configuration structure.
 */
void text_widget_destroy(text_widget_t* widget);

#ifdef __cplusplus
}
#endif

#endif /* TEXT_WIDGET_H */