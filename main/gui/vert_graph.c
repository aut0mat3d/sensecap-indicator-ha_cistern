/**
 * @file vert_graph.c
 * @brief Vertical graph/bar module supporting 4x5 to 10x8 grid dimensions.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "vert_graph.h"
#include "lv_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_font0);

#define DEFAULT_BG_COLOR      lv_color_hex(0x282828)
#define DEFAULT_BAR_COLOR     lv_color_hex(0x3498db)
#define DEFAULT_TEXT_COLOR    lv_color_hex(0xFFFFFF)
#define DEFAULT_ALARM_COLOR   lv_color_hex(0xE74C3C)

/**
 * @brief Helper to check if an lv_color_t is uninitialized (all zeros).
 */
static inline bool is_color_empty(lv_color_t c) {
    static const lv_color_t zero = {0};
    return memcmp(&c, &zero, sizeof(lv_color_t)) == 0;
}

/**
 * @brief Unified click handler to dispatch events to the user-provided callback.
 */
static void vert_graph_click_handler(lv_event_t *e) {
    lv_obj_t *container = lv_event_get_target(e);
    vert_graph_t *graph = (vert_graph_t *)lv_obj_get_user_data(container);
    if (graph && graph->click_cb) {
        graph->click_cb(e);
    }
}

/**
 * @brief Internal lock-free update function to prevent semaphore deadlocks when 
 *        called by other API functions that already hold the LVGL lock.
 */
static void _vert_graph_update_internal(vert_graph_t *graph, float value, const char *unit, bool is_estimated, bool is_alarm) {
    if (!graph || !graph->bar) return;

    /* 1. Update unit strings if provided and different */
    if (unit && unit[0] != '\0' && strncmp(graph->unit, unit, sizeof(graph->unit) - 1) != 0) {
        strncpy(graph->unit, unit, sizeof(graph->unit) - 1);
        char buf[32];
        if (graph->lbl_max) {
            snprintf(buf, sizeof(buf), "%.2f %s", graph->max_value, graph->unit);
            lv_label_set_text(graph->lbl_max, buf);
        }
        if (graph->lbl_min) {
            snprintf(buf, sizeof(buf), "%.2f %s", graph->min_value, graph->unit);
            lv_label_set_text(graph->lbl_min, buf);
        }
    }

    /* 2. Clamp incoming value to valid scale range */
    float clamped = (value < graph->min_value) ? graph->min_value : ((value > graph->max_value) ? graph->max_value : value);
    float range = graph->max_value - graph->min_value;
    
    /* 3. Compute accurate fill percentage for the bar widget */
    float pct = (range > 0.0001f) ? (((clamped - graph->min_value) / range) * 100.0f) : 0.0f;
    int32_t bar_val = (int32_t)(pct + 0.5f);
    if (bar_val < 0) bar_val = 0;
    if (bar_val > 100) bar_val = 100;

    lv_bar_set_value(graph->bar, bar_val, LV_ANIM_ON);

    /* 4. Update the center percentage & current value label */
    if (graph->lbl_percent) {
        char val_buf[32];
        const char *u = (graph->unit[0] != '\0') ? graph->unit : "cm";
        snprintf(val_buf, sizeof(val_buf), "%.1f %s\n(%.1f %%)", (double)clamped, u, (double)pct);
        lv_label_set_text(graph->lbl_percent, val_buf);
    }
    
    graph->current_value = clamped;
    graph->is_estimated = is_estimated;
    graph->is_alarm = is_alarm;

    /* 5. Autonomous Threshold & Status Color Evaluation */
    bool is_danger = is_alarm;
    bool is_warn = false;

    /* Automatic threshold check against configured alarm levels */
    if (graph->alarm_level_min > 0.0f && clamped < graph->alarm_level_min) {
        is_danger = true;
    } else if (graph->alarm_level_max > 0.0f && clamped > graph->alarm_level_max) {
        is_danger = true;
    }

    /* Apply colors: Danger (Red/E74C3C) > Warning (Orange/F39C12) > Normal (Bar Media Color) */
    if (is_danger) {
        lv_color_t alarm_col = !is_color_empty(graph->alarm_color) ? graph->alarm_color : DEFAULT_ALARM_COLOR;
        lv_obj_set_style_bg_color(graph->bar, alarm_col, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(graph->bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(graph->bar, 2, LV_PART_INDICATOR);
        lv_obj_set_style_border_color(graph->bar, lv_color_white(), LV_PART_INDICATOR);
    } else if (is_warn) {
        lv_obj_set_style_bg_color(graph->bar, lv_color_hex(0xF39C12), LV_PART_INDICATOR); /* Orange */
        lv_obj_set_style_bg_opa(graph->bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(graph->bar, 2, LV_PART_INDICATOR);
        lv_obj_set_style_border_color(graph->bar, lv_color_white(), LV_PART_INDICATOR);
    } else if (is_estimated) {
        lv_obj_set_style_bg_color(graph->bar, graph->bar_color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(graph->bar, LV_OPA_50, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(graph->bar, 2, LV_PART_INDICATOR);
        lv_obj_set_style_border_color(graph->bar, lv_color_hex(0xBDC3C7), LV_PART_INDICATOR);
    } else {
        lv_obj_set_style_bg_color(graph->bar, graph->bar_color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(graph->bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(graph->bar, 0, LV_PART_INDICATOR);
    }

    /* 6. Position both alarm threshold markers (Min & Max) relative to dynamic bar height */
    uint16_t bar_h = graph->bar_height ? graph->bar_height : 180;

    /* Evaluate Minimum Alarm Marker (e.g. Dry Run Protection) */
    if (graph->alarm_marker_min) {
        if (graph->alarm_level_min > graph->min_value && graph->alarm_level_min <= graph->max_value && range > 0.0001f) {
            float min_pct = ((graph->alarm_level_min - graph->min_value) / range) * 100.0f;
            int16_t marker_y = (int16_t)(bar_h - (min_pct * bar_h / 100.0f) - 1.5f);
            if (marker_y < 0) marker_y = 0;
            if (marker_y > bar_h - 3) marker_y = bar_h - 3;
            lv_obj_set_pos(graph->alarm_marker_min, 0, marker_y);
            lv_obj_remove_flag(graph->alarm_marker_min, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(graph->alarm_marker_min, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* Evaluate Maximum Alarm Marker (e.g. Overflow Protection) */
    if (graph->alarm_marker_max) {
        if (graph->alarm_level_max > graph->min_value && graph->alarm_level_max <= graph->max_value && range > 0.0001f) {
            float max_pct = ((graph->alarm_level_max - graph->min_value) / range) * 100.0f;
            int16_t marker_y = (int16_t)(bar_h - (max_pct * bar_h / 100.0f) - 1.5f);
            if (marker_y < 0) marker_y = 0;
            if (marker_y > bar_h - 3) marker_y = bar_h - 3;
            lv_obj_set_pos(graph->alarm_marker_max, 0, marker_y);
            lv_obj_remove_flag(graph->alarm_marker_max, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(graph->alarm_marker_max, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

int vert_graph_init(vert_graph_t *graph) {
    if (!graph || !graph->parent) return -1;

    lv_port_sem_take();

    /* Apply default styling if empty */
    if (!graph->font) graph->font = &ui_font_font0;
    if (is_color_empty(graph->bg_color)) graph->bg_color = DEFAULT_BG_COLOR;
    if (is_color_empty(graph->bar_color)) graph->bar_color = DEFAULT_BAR_COLOR;
    if (is_color_empty(graph->text_color)) graph->text_color = DEFAULT_TEXT_COLOR;
    if (is_color_empty(graph->alarm_color)) graph->alarm_color = DEFAULT_ALARM_COLOR;
    if (graph->width == 0) graph->width = 184;  /* Default for 4x5 grid cells */
    if (graph->height == 0) graph->height = 232;
    if (graph->min_value == 0.0f && graph->max_value == 0.0f) graph->max_value = 100.0f;

    /* 1. Main Card Container */
    graph->container = lv_obj_create(graph->parent);
    lv_obj_set_size(graph->container, graph->width, graph->height);
    lv_obj_set_pos(graph->container, graph->x, graph->y);
    lv_obj_remove_flag(graph->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(graph->container, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(graph->container, graph->bg_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(graph->container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(graph->container, 8, LV_PART_MAIN);
    lv_obj_set_user_data(graph->container, graph);
    lv_obj_add_event_cb(graph->container, vert_graph_click_handler, LV_EVENT_CLICKED, NULL);

    /* 2. Scaled Inner Geometry for the Bar */
    uint16_t bar_w = (graph->width >= 220) ? 46 : 38;
    uint16_t bar_x = 10;
    uint16_t bar_y = 12;
    uint16_t bar_h = (graph->height > 30) ? (graph->height - 24) : 100;
    graph->bar_height = bar_h;

    /* 3. Bar Widget */
    graph->bar = lv_bar_create(graph->container);
    lv_obj_set_size(graph->bar, bar_w, bar_h);
    lv_obj_set_pos(graph->bar, bar_x, bar_y);
    lv_bar_set_range(graph->bar, 0, 100);
    lv_bar_set_value(graph->bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(graph->bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(graph->bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(graph->bar, lv_color_hex(0x1a2126), LV_PART_MAIN);
    lv_obj_set_style_bg_color(graph->bar, graph->bar_color, LV_PART_INDICATOR);

    /* 4. Threshold Markers inside bar */
    graph->alarm_marker_min = lv_obj_create(graph->bar);
    lv_obj_set_size(graph->alarm_marker_min, bar_w, 3);
    lv_obj_set_style_radius(graph->alarm_marker_min, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(graph->alarm_marker_min, graph->alarm_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(graph->alarm_marker_min, 0, LV_PART_MAIN);
    lv_obj_add_flag(graph->alarm_marker_min, LV_OBJ_FLAG_HIDDEN);

    graph->alarm_marker_max = lv_obj_create(graph->bar);
    lv_obj_set_size(graph->alarm_marker_max, bar_w, 3);
    lv_obj_set_style_radius(graph->alarm_marker_max, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(graph->alarm_marker_max, graph->alarm_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(graph->alarm_marker_max, 0, LV_PART_MAIN);
    lv_obj_add_flag(graph->alarm_marker_max, LV_OBJ_FLAG_HIDDEN);

    /* 5. Scale Labels */
    uint16_t label_x = bar_x + bar_w + 10;
    const char *unit_str = (graph->unit[0] != '\0') ? graph->unit : (graph->title ? graph->title : "");

    /* Maximum Label (Top) */
    graph->lbl_max = lv_label_create(graph->container);
    char max_text[32];
    if (unit_str[0] != '\0') snprintf(max_text, sizeof(max_text), "%.1f %s", graph->max_value, unit_str);
    else snprintf(max_text, sizeof(max_text), "%.1f", graph->max_value);
    lv_label_set_text(graph->lbl_max, max_text);
    lv_obj_set_style_text_font(graph->lbl_max, graph->font, LV_PART_MAIN);
    lv_obj_set_style_text_color(graph->lbl_max, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
    lv_obj_set_pos(graph->lbl_max, label_x, bar_y + 2);

    /* Percentage & Current Value Label (Middle) */
    graph->lbl_percent = lv_label_create(graph->container);
    lv_label_set_text(graph->lbl_percent, "--\n(0.0 %)");
    lv_obj_set_style_text_font(graph->lbl_percent, graph->font, LV_PART_MAIN);
    lv_obj_set_style_text_color(graph->lbl_percent, graph->text_color, LV_PART_MAIN);
    lv_obj_set_style_text_align(graph->lbl_percent, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(graph->lbl_percent, label_x, bar_y + (bar_h / 2) - 18);

    /* Minimum Label (Bottom) */
    graph->lbl_min = lv_label_create(graph->container);
    char min_text[32];
    if (unit_str[0] != '\0') snprintf(min_text, sizeof(min_text), "%.1f %s", graph->min_value, unit_str);
    else snprintf(min_text, sizeof(min_text), "%.1f", graph->min_value);
    lv_label_set_text(graph->lbl_min, min_text);
    lv_obj_set_style_text_font(graph->lbl_min, graph->font, LV_PART_MAIN);
    lv_obj_set_style_text_color(graph->lbl_min, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
    lv_obj_set_pos(graph->lbl_min, label_x, bar_y + bar_h - 20);

    /* Apply initial configured value immediately */
    _vert_graph_update_internal(graph, graph->current_value, graph->unit, graph->is_estimated, graph->is_alarm);
    lv_port_sem_give();
    return 0;
}

void vert_graph_update(vert_graph_t *graph, float value) {
    if (!graph) return;
    lv_port_sem_take();
    _vert_graph_update_internal(graph, value, NULL, graph->is_estimated, graph->is_alarm);
    lv_port_sem_give();
}

void vert_graph_update_value(vert_graph_t *graph, float value, const char *unit, bool is_estimated, bool is_alarm) {
    if (!graph) return;
    lv_port_sem_take();
    _vert_graph_update_internal(graph, value, unit, is_estimated, is_alarm);
    lv_port_sem_give();
}

void vert_graph_set_status(vert_graph_t *graph, bool is_estimated, bool is_alarm) {
    if (!graph) return;
    lv_port_sem_take();
    _vert_graph_update_internal(graph, graph->current_value, NULL, is_estimated, is_alarm);
    lv_port_sem_give();
}

void vert_graph_set_range(vert_graph_t *graph, float min_val, float max_val) {
    if (!graph) return;
    lv_port_sem_take();
    graph->min_value = min_val;
    graph->max_value = max_val;
    _vert_graph_update_internal(graph, graph->current_value, NULL, graph->is_estimated, graph->is_alarm);
    lv_port_sem_give();
}

void vert_graph_set_media_color(vert_graph_t *graph, lv_color_t color) {
    if (!graph) return;
    lv_port_sem_take();
    graph->bar_color = color;
    _vert_graph_update_internal(graph, graph->current_value, NULL, graph->is_estimated, graph->is_alarm);
    lv_port_sem_give();
}

void vert_graph_set_alarm_levels(vert_graph_t *graph, float min_alarm, float max_alarm, float hysteresis_min, float hysteresis_max) {
    if (!graph) return;
    lv_port_sem_take();
    graph->alarm_level_min = min_alarm;
    graph->alarm_level_max = max_alarm;
    graph->hysteresis_min = hysteresis_min;
    graph->hysteresis_max = hysteresis_max;
    _vert_graph_update_internal(graph, graph->current_value, NULL, graph->is_estimated, graph->is_alarm);
    lv_port_sem_give();
}

void vert_graph_destroy(vert_graph_t *graph) {
    if (!graph) return;
    lv_port_sem_take();
    
    /* Clean up all allocated LVGL components */
    if (graph->container) {
        lv_obj_delete(graph->container);
        graph->container = NULL;
    }
    
    graph->bar = NULL;
    graph->lbl_percent = NULL;
    graph->lbl_max = NULL;
    graph->lbl_min = NULL;
    graph->alarm_marker_min = NULL;
    graph->alarm_marker_max = NULL;
    
    lv_port_sem_give();
}

/* ── Generic widget_base.h Bridge Implementation ────────────────────────── */

static void _vert_graph_widget_update(widget_t *self, float value, const char *unit) {
    if (!self || !self->user_data) return;
    vert_graph_t *graph = (vert_graph_t *)self->user_data;
    vert_graph_update_value(graph, value, unit, graph->is_estimated, graph->is_alarm);
}

static void _vert_graph_widget_destroy(widget_t *self) {
    if (!self) return;
    if (self->user_data) {
        vert_graph_destroy((vert_graph_t *)self->user_data);
        free(self->user_data);
        self->user_data = NULL;
    }
    free(self);
}

widget_t* vert_graph_create(lv_obj_t *parent, const char *id, const char *binding_key,
                            int16_t x, int16_t y, uint16_t w, uint16_t h) {
    if (!parent || !id) return NULL;
    
    widget_t *base = (widget_t *)calloc(1, sizeof(widget_t));
    if (!base) return NULL;
    
    vert_graph_t *graph = (vert_graph_t *)calloc(1, sizeof(vert_graph_t));
    if (!graph) { 
        free(base); 
        return NULL; 
    }

    strncpy(base->id, id, sizeof(base->id) - 1);
    if (binding_key) {
        strncpy(base->binding_key, binding_key, sizeof(base->binding_key) - 1);
    }

    graph->parent = parent;
    graph->x = x;
    graph->y = y;
    graph->width = w;
    graph->height = h;

    if (vert_graph_init(graph) != 0) {
        free(graph);
        free(base);
        return NULL;
    }

    base->container = graph->container;
    base->user_data = graph;
    base->update_val = _vert_graph_widget_update;
    base->update_st = NULL;
    base->destroy = _vert_graph_widget_destroy;
    
    return base;
}