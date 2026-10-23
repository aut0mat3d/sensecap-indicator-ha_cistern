/**
 * @file gui_header.h
 * @brief Resilient, globally managed header for flexible GUI pages.
 *        Prevents layout shifting and ghost icons between page transitions.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef GUI_HEADER_H
#define GUI_HEADER_H

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEADER_SLOT_LEFT    = 0, /* Leftmost: Usually Back or Settings */
    HEADER_SLOT_RIGHT_1 = 1, /* Rightmost: Default WiFi Status */
    HEADER_SLOT_RIGHT_2 = 2, /* Second from right: Default LoRa Status */
    HEADER_SLOT_RIGHT_3 = 3  /* Third from right: Extra action button */
} header_slot_t;

typedef enum {
    LORA_STATUS_UNINIT  = 0,
    LORA_STATUS_WAITING = 1,
    LORA_STATUS_ACTIVE  = 2,
    LORA_STATUS_TIMEOUT = 3
} lora_header_status_t;

void gui_header_set_alarm_state(bool has_active_alarms);

/**
 * @brief Initialize the global header on the parent screen.
 * @param parent Typically the main screen or tileview container.
 */
void gui_header_init(lv_obj_t *parent);

/**
 * @brief Clear the header (hides all buttons, removes callbacks, clears title).
 *        MUST be called at the start of every page transition!
 */
void gui_header_clear(void);

/**
 * @brief Set the main title of the page.
 */
void gui_header_set_title(const char *title);

/**
 * @brief Configure a specific slot with text/icon and a callback.
 * @param slot The slot to configure.
 * @param text The label text (e.g. LV_SYMBOL_LEFT " Back" or LV_SYMBOL_SETTINGS).
 * @param cb The event callback for click events.
 * @param user_data Optional user data passed to the callback.
 */
void gui_header_set_slot(header_slot_t slot, const char *text, lv_event_cb_t cb, void *user_data);
void gui_header_set_slot_image(header_slot_t slot, const lv_image_dsc_t *img_src, lv_event_cb_t cb, void *user_data);

/**
 * @brief Show or hide the entire header (useful for custom full-screen pages).
 */
void gui_header_set_visible(bool visible);

/* Global status icon updates for Slot 1 (Rightmost: WiFi) and Slot 2 (LoRa) */
void gui_header_update_wifi(bool is_connected, int8_t rssi);
void gui_header_update_lora(lora_header_status_t status);


#ifdef __cplusplus
}
#endif

#endif /* GUI_HEADER_H */