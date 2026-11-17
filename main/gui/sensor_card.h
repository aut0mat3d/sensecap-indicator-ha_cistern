/**
 * @file sensor_card.h
 * @brief Modular sensor card widget matching the SenseCAP UI design language.
 */

#ifndef SENSOR_CARD_H
#define SENSOR_CARD_H

#include "lvgl.h"
#include "widget_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_obj_t *parent;
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;

    const char *title;
    const char *unit;
    const void *icon_src;      /* Can be an lv_image_dsc_t* or an LV_SYMBOL string */
    bool is_icon_symbol;       /* Set to true if icon_src is a text symbol */

    lv_color_t accent_color;
    uint8_t decimals;          /* Number of decimal places (e.g., 0 for CO2, 1 for Temp) */

    float current_value;
    bool is_alarm;

    /* LVGL internal objects */
    lv_obj_t *container;
    lv_obj_t *lbl_value;
    lv_obj_t *lbl_unit;
    lv_obj_t *lbl_title;
    lv_obj_t *obj_icon;

    void (*click_cb)(lv_event_t *e);
} sensor_card_t;

/**
 * @brief Creates a new sensor card wrapped in the generic widget_t interface.
 * 
 * @param parent        The LVGL parent object (usually a tile).
 * @param id            Unique string ID for the widget.
 * @param binding_key   The data key to bind to (e.g., "temp", "co2").
 * @param x             X position on parent.
 * @param y             Y position on parent.
 * @param w             Width of the card (default typically 214).
 * @param h             Height of the card (default typically 164).
 * @param title         The display name (e.g., "Temp", "CO2").
 * @param unit          The measurement unit (e.g., "°C", "ppm").
 * @param icon_src      Pointer to image descriptor or symbol string.
 * @param is_symbol     True if icon_src is a symbol string, false if image.
 * @param accent_color  The color used for the value and unit text.
 * @param decimals      Decimal precision for the displayed value.
 * @return widget_t*    The generic widget instance.
 */
widget_t* sensor_card_create(lv_obj_t *parent, const char *id, const char *binding_key,
                             int16_t x, int16_t y, uint16_t w, uint16_t h,
                             const char *title, const char *unit, 
                             const void *icon_src, bool is_symbol,
                             lv_color_t accent_color, uint8_t decimals);

/**
 * @brief Internal specific API to update the card value and alarm state.
 */
void sensor_card_update_value(sensor_card_t *card, float value, bool is_alarm);

/**
 * @brief Free resources and delete the widget from the display.
 */
void sensor_card_destroy(sensor_card_t *card);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SENSOR_CARD_H */