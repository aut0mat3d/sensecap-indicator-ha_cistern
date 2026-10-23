/**
 * @file widget_base.h
 * @brief Unified interface for all dynamic GUI widgets (Sensors & Home Assistant).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef WIDGET_BASE_H
#define WIDGET_BASE_H

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct widget_s widget_t;

/**
 * @brief Standardized update function for telemetry/sensor widgets.
 * @param self The widget instance.
 * @param value The new numeric value.
 * @param unit The unit string (e.g. "°C", "m3").
 */
typedef void (*widget_update_fn)(widget_t *self, float value, const char *unit);

/**
 * @brief Standardized update function for Home Assistant / MQTT switch widgets.
 * @param self The widget instance.
 * @param state Boolean state (true = ON, false = OFF).
 */
typedef void (*widget_update_state_fn)(widget_t *self, bool state);

/**
 * @brief Cleanup function to free LVGL objects and internal memory.
 */
typedef void (*widget_destroy_fn)(widget_t *self);

struct widget_s {
    char              id[32];          /* Unique widget ID (from JSON) */
    char              binding_key[32]; /* Data binding (e.g. "SHT41_TEMP" or "light.living_room") */
    char              source_dev[32];  /* Filter for origin device (optional) */
    
    lv_obj_t         *container;       /* The LVGL root object of this widget */
    void             *user_data;       /* Pointer to the specific implementation (e.g. vert_graph_t*) */
    
    widget_update_fn       update_val; /* Called when telemetry data arrives */
    widget_update_state_fn update_st;  /* Called when HA switch states arrive */
    widget_destroy_fn      destroy;    /* Called when page is unloaded */
};

#ifdef __cplusplus
}
#endif

#endif /* WIDGET_BASE_H */