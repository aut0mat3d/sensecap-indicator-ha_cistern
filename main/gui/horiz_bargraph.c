/**
 * @file horiz_bargraph.c
 * @brief Horizontal bar/line graph with dynamic scaling, compact mode (5x2),
 *        multi-zone time segmentation (1-4 zones), and LVGL 9 draw-task interception.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "horiz_bargraph.h"
#include "lv_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_font0);

#define DEFAULT_BG_COLOR      lv_color_hex(0x282828)
#define DEFAULT_BAR_COLOR     lv_color_hex(0x3498db)
#define DEFAULT_TEXT_COLOR    lv_color_hex(0x9E9E9E)
#define DEFAULT_ALARM_COLOR   lv_color_hex(0xE74C3C)

static inline bool is_color_empty(lv_color_t c) {
    static const lv_color_t zero = {0};
    return memcmp(&c, &zero, sizeof(lv_color_t)) == 0;
}

static void horiz_bargraph_click_handler(lv_event_t *e) {
    lv_obj_t *container = lv_event_get_target_obj(e);
    horiz_bargraph_t *graph = (horiz_bargraph_t *)lv_obj_get_user_data(container);
    if (graph && graph->click_cb) {
        graph->click_cb(e);
    }
}

/**
 * @brief LVGL 9 draw task interception to dynamically color bars based on alarm and quality state.
 */
static void horiz_bargraph_draw_task_cb(lv_event_t *e) {
    lv_draw_task_t *draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t *base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    if (!base_dsc || base_dsc->part != LV_PART_ITEMS) return;

    horiz_bargraph_t *graph = (horiz_bargraph_t *)lv_event_get_user_data(e);
    if (!graph) return;

    lv_draw_fill_dsc_t *fill_dsc = lv_draw_task_get_fill_dsc(draw_task);
    if (fill_dsc) {
        uint32_t pt_idx = base_dsc->id2;
        if (pt_idx < HORIZ_BARGRAPH_MAX_POINTS) {
            /* Empty historical slots: 100% transparent (no draw artifact) */
            if (!graph->has_data[pt_idx]) {
                fill_dsc->opa = LV_OPA_TRANSP;
            } else if (graph->is_alarm[pt_idx]) {
                fill_dsc->color = !is_color_empty(graph->alarm_color) ? graph->alarm_color : DEFAULT_ALARM_COLOR;
                fill_dsc->opa = LV_OPA_COVER;
            } else if (graph->is_estimated[pt_idx]) {
                fill_dsc->color = graph->bar_color;
                fill_dsc->opa = LV_OPA_50;
            } else {
                fill_dsc->color = graph->bar_color;
                fill_dsc->opa = LV_OPA_COVER;
            }
        }
    }
}

static void _horiz_bargraph_update_internal(horiz_bargraph_t *graph, const float *new_data, 
                                            const bool *is_estimated, const bool *is_alarm, uint8_t count) {
    if (!graph || !graph->chart || !new_data) return;

    uint8_t copy_count = (count < HORIZ_BARGRAPH_MAX_POINTS) ? count : HORIZ_BARGRAPH_MAX_POINTS;
    graph->data_count = copy_count;

    for (uint8_t i = 0; i < copy_count; i++) {
        graph->data[i] = new_data[i];
        graph->is_estimated[i] = is_estimated ? is_estimated[i] : false;
        graph->is_alarm[i] = is_alarm ? is_alarm[i] : false;
    }

    lv_chart_set_point_count(graph->chart, copy_count > 0 ? copy_count : 1);
    for (uint8_t i = 0; i < copy_count; i++) {
        lv_chart_set_value_by_id(graph->chart, graph->series, i, (int32_t)(new_data[i] + 0.5f));
    }
    lv_chart_refresh(graph->chart);
}

int horiz_bargraph_init(horiz_bargraph_t *graph) {
    if (!graph || !graph->parent) return -1;

    lv_port_sem_take();

    if (!graph->font) graph->font = &ui_font_font0;
    if (is_color_empty(graph->bg_color)) graph->bg_color = DEFAULT_BG_COLOR;
    if (is_color_empty(graph->bar_color)) graph->bar_color = DEFAULT_BAR_COLOR;
    if (is_color_empty(graph->text_color)) graph->text_color = DEFAULT_TEXT_COLOR;
    if (is_color_empty(graph->alarm_color)) graph->alarm_color = DEFAULT_ALARM_COLOR;
    if (graph->width == 0) graph->width = 472;
    if (graph->height == 0) graph->height = 136;
    if (graph->min_value == 0.0f && graph->max_value == 0.0f) graph->max_value = 100.0f;

    /* 1. Main Card Container */
    graph->container = lv_obj_create(graph->parent);
    lv_obj_set_size(graph->container, graph->width, graph->height);
    lv_obj_set_pos(graph->container, graph->x, graph->y);
    lv_obj_remove_flag(graph->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(graph->container, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(graph->container, graph->bg_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(graph->container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(graph->container, 6, LV_PART_MAIN);
    
    lv_obj_set_user_data(graph->container, graph);
    lv_obj_add_event_cb(graph->container, horiz_bargraph_click_handler, LV_EVENT_CLICKED, NULL);

    bool is_compact = (graph->height <= 95);
    uint16_t chart_top_offset = (is_compact) ? 18 : 24;

    /* 2. Title Label */
    if (graph->title && graph->title[0] != '\0') {
        graph->lbl_title = lv_label_create(graph->container);
        lv_label_set_text(graph->lbl_title, graph->title);
        lv_obj_set_style_text_font(graph->lbl_title, graph->font, LV_PART_MAIN);
        lv_obj_set_style_text_color(graph->lbl_title, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_pos(graph->lbl_title, 8, 2);
    } else {
        graph->lbl_title = NULL;
    }

    /* 3. Initialize label & divider pointers */
    for (uint8_t i = 0; i < HORIZ_BARGRAPH_MAX_LABELS; i++) {
        graph->axis_labels_obj[i] = NULL;
    }
    for (uint8_t i = 0; i < HORIZ_BARGRAPH_MAX_ZONES - 1; i++) {
        graph->zone_dividers[i] = NULL;
    }

    uint16_t chart_w = (graph->width > 12) ? (graph->width - 12) : 50;
    uint16_t chart_h = (graph->height > chart_top_offset + 6) ? (graph->height - chart_top_offset - 6) : 30;

    /* 4. Chart Widget */
    graph->chart = lv_chart_create(graph->container);
    lv_obj_set_size(graph->chart, chart_w, chart_h);
    lv_obj_set_pos(graph->chart, 0, chart_top_offset);
    
    switch (graph->chart_type) {
        case HORIZ_BARGRAPH_TYPE_LINE:
            lv_chart_set_type(graph->chart, LV_CHART_TYPE_LINE);
            break;
        case HORIZ_BARGRAPH_TYPE_POINT:
            lv_chart_set_type(graph->chart, LV_CHART_TYPE_LINE);
            lv_obj_set_style_line_width(graph->chart, 0, LV_PART_ITEMS);
            break;
        case HORIZ_BARGRAPH_TYPE_BAR:
        default:
            lv_chart_set_type(graph->chart, LV_CHART_TYPE_BAR);
            break;
    }

    /* Clear internal paddings to guarantee exact 1:1 bar-to-header alignment */
    lv_obj_set_style_pad_left(graph->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(graph->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(graph->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(graph->chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(graph->chart, is_compact ? 2 : 3, LV_PART_MAIN);

    lv_chart_set_point_count(graph->chart, graph->data_count > 0 ? graph->data_count : 31);
    lv_chart_set_range(graph->chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)graph->min_value, (int32_t)graph->max_value);
    
    lv_obj_set_style_bg_opa(graph->chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(graph->chart, 0, LV_PART_MAIN);

    /* 5. Draw Task Interception for Dynamic Quality/Alarm Coloring */
    lv_obj_add_event_cb(graph->chart, horiz_bargraph_draw_task_cb, LV_EVENT_DRAW_TASK_ADDED, graph);
    lv_obj_set_send_draw_task_events(graph->chart, true);

    /* 6. Series Allocation */
    graph->series = lv_chart_add_series(graph->chart, graph->bar_color, LV_CHART_AXIS_PRIMARY_Y);

    lv_port_sem_give();
    return 0;
}

void horiz_bargraph_update_data(horiz_bargraph_t *graph, const float *new_data, uint8_t count) {
    if (!graph) return;
    lv_port_sem_take();
    _horiz_bargraph_update_internal(graph, new_data, NULL, NULL, count);
    lv_port_sem_give();
}

void horiz_bargraph_update_series(horiz_bargraph_t *graph, const float *values, 
                                  const bool *is_estimated, const bool *is_alarm, uint8_t count) {
    if (!graph) return;
    lv_port_sem_take();
    _horiz_bargraph_update_internal(graph, values, is_estimated, is_alarm, count);
    lv_port_sem_give();
}

void horiz_bargraph_update_point(horiz_bargraph_t *graph, uint8_t index, float value, bool is_estimated, bool is_alarm) {
    if (!graph || !graph->chart || index >= HORIZ_BARGRAPH_MAX_POINTS) return;
    lv_port_sem_take();
    graph->data[index] = value;
    graph->is_estimated[index] = is_estimated;
    graph->is_alarm[index] = is_alarm;
    if (index >= graph->data_count) graph->data_count = index + 1;
    if (index < lv_chart_get_point_count(graph->chart)) {
        lv_chart_set_value_by_id(graph->chart, graph->series, index, (int32_t)(value + 0.5f));
        lv_chart_refresh(graph->chart);
    }
    lv_port_sem_give();
}

void horiz_bargraph_set_axis_labels(horiz_bargraph_t *graph, const char **labels, uint8_t count) {
    if (!graph || !labels || graph->height <= 95) return;
    lv_port_sem_take();

    graph->axis_label_count = (count <= HORIZ_BARGRAPH_MAX_LABELS) ? count : HORIZ_BARGRAPH_MAX_LABELS;
    for (uint8_t i = 0; i < graph->axis_label_count; i++) {
        graph->axis_labels[i] = labels[i];
    }

    /* Restore title visibility if active */
    if (graph->lbl_title && graph->title && graph->title[0] != '\0') {
        lv_obj_remove_flag(graph->lbl_title, LV_OBJ_FLAG_HIDDEN);
    }

    uint16_t chart_w = (graph->width > 16) ? (graph->width - 12) : 50;

    if (graph->container && graph->axis_label_count > 0) {
        for (uint8_t i = 0; i < graph->axis_label_count; i++) {
            if (!graph->axis_labels_obj[i]) {
                graph->axis_labels_obj[i] = lv_label_create(graph->container);
                lv_obj_set_style_text_font(graph->axis_labels_obj[i], graph->font, LV_PART_MAIN);
                lv_obj_set_style_text_color(graph->axis_labels_obj[i], graph->text_color, LV_PART_MAIN);
            }
            if (graph->axis_labels_obj[i]) {
                lv_label_set_text(graph->axis_labels_obj[i], labels[i] ? labels[i] : "");
                lv_obj_remove_flag(graph->axis_labels_obj[i], LV_OBJ_FLAG_HIDDEN);

                if (i == 0) {
                    int16_t x0 = (graph->lbl_title && !lv_obj_has_flag(graph->lbl_title, LV_OBJ_FLAG_HIDDEN)) ? 140 : 8;
                    lv_obj_set_pos(graph->axis_labels_obj[i], x0, 2);
                } else if (i == graph->axis_label_count - 1) {
                    lv_obj_align(graph->axis_labels_obj[i], LV_ALIGN_TOP_RIGHT, -8, 2);
                } else {
                    int16_t x_pos = 8 + (int16_t)((i * chart_w) / (graph->axis_label_count - 1)) - 12;
                    lv_obj_set_pos(graph->axis_labels_obj[i], x_pos, 2);
                }
            }
        }
        for (uint8_t i = graph->axis_label_count; i < HORIZ_BARGRAPH_MAX_LABELS; i++) {
            if (graph->axis_labels_obj[i]) {
                lv_obj_add_flag(graph->axis_labels_obj[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    lv_port_sem_give();
}

void horiz_bargraph_set_zones(horiz_bargraph_t *graph, const horiz_bargraph_zone_t *zones, uint8_t zone_count) {
    if (!graph || !zones || zone_count == 0) return;

    lv_port_sem_take();

    graph->zone_count = (zone_count <= HORIZ_BARGRAPH_MAX_ZONES) ? zone_count : HORIZ_BARGRAPH_MAX_ZONES;
    uint8_t total_bars = 0;

    for (uint8_t z = 0; z < graph->zone_count; z++) {
        graph->zones[z] = zones[z];
        total_bars += zones[z].count;
    }
    if (total_bars > HORIZ_BARGRAPH_MAX_POINTS) total_bars = HORIZ_BARGRAPH_MAX_POINTS;
    graph->data_count = total_bars;

    if (graph->lbl_title) {
        lv_obj_add_flag(graph->lbl_title, LV_OBJ_FLAG_HIDDEN);
    }

    uint16_t chart_w = (graph->width > 12) ? (graph->width - 12) : 50;
    bool is_compact = (graph->height <= 95);
    uint8_t accumulated_bars = 0;
    uint16_t divider_height = graph->height / 4; /* Top 25% height */

    if (!is_compact && graph->container && total_bars > 0) {
        for (uint8_t z = 0; z < graph->zone_count; z++) {
            int16_t start_x = (int16_t)((accumulated_bars * chart_w) / total_bars);
            accumulated_bars += graph->zones[z].count;
            int16_t end_x = (int16_t)((accumulated_bars * chart_w) / total_bars);
            int16_t zone_w = end_x - start_x;

            /* 1. Header label: render only if zone title is non-empty */
            if (graph->zones[z].title[0] != '\0') {
                if (!graph->axis_labels_obj[z]) {
                    graph->axis_labels_obj[z] = lv_label_create(graph->container);
                    lv_obj_set_style_text_font(graph->axis_labels_obj[z], graph->font, LV_PART_MAIN);
                    lv_obj_set_style_text_color(graph->axis_labels_obj[z], graph->text_color, LV_PART_MAIN);
                    lv_label_set_long_mode(graph->axis_labels_obj[z], LV_LABEL_LONG_CLIP);
                }
                lv_label_set_text(graph->axis_labels_obj[z], graph->zones[z].title);
                lv_obj_remove_flag(graph->axis_labels_obj[z], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(graph->axis_labels_obj[z], start_x, 2);
                lv_obj_set_width(graph->axis_labels_obj[z], zone_w);
                lv_obj_set_style_text_align(graph->axis_labels_obj[z], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
            } else if (graph->axis_labels_obj[z]) {
                lv_obj_add_flag(graph->axis_labels_obj[z], LV_OBJ_FLAG_HIDDEN);
            }

            /* 2. Vertical divider line between this zone and next zone */
            if (z < graph->zone_count - 1) {
                if (!graph->zone_dividers[z]) {
                    graph->zone_dividers[z] = lv_obj_create(graph->container);
                    lv_obj_remove_flag(graph->zone_dividers[z], LV_OBJ_FLAG_SCROLLABLE);
                    lv_obj_set_style_radius(graph->zone_dividers[z], 0, LV_PART_MAIN);
                    lv_obj_set_style_border_width(graph->zone_dividers[z], 0, LV_PART_MAIN);
                    lv_obj_set_style_bg_color(graph->zone_dividers[z], lv_color_hex(0x555555), LV_PART_MAIN);
                    lv_obj_set_style_bg_opa(graph->zone_dividers[z], LV_OPA_COVER, LV_PART_MAIN);
                }
                /* Thin 1px line from y=2 down to divider_height */
                lv_obj_set_size(graph->zone_dividers[z], 1, divider_height);
                lv_obj_set_pos(graph->zone_dividers[z], end_x, 2);
                lv_obj_remove_flag(graph->zone_dividers[z], LV_OBJ_FLAG_HIDDEN);
            }
        }

        /* Hide unused labels & dividers */
        for (uint8_t z = graph->zone_count; z < HORIZ_BARGRAPH_MAX_LABELS; z++) {
            if (graph->axis_labels_obj[z]) lv_obj_add_flag(graph->axis_labels_obj[z], LV_OBJ_FLAG_HIDDEN);
        }
        for (uint8_t z = graph->zone_count - 1; z < HORIZ_BARGRAPH_MAX_ZONES - 1; z++) {
            if (graph->zone_dividers[z]) lv_obj_add_flag(graph->zone_dividers[z], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (graph->chart) {
        lv_chart_set_point_count(graph->chart, total_bars > 0 ? total_bars : 1);
        lv_chart_refresh(graph->chart);
    }

    lv_port_sem_give();
}

void horiz_bargraph_update_zone_data(horiz_bargraph_t *graph, uint8_t zone_idx, 
                                     const chart_data_point_t *points, uint16_t count) {
    if (!graph || !graph->chart || !graph->series || zone_idx >= graph->zone_count) return;

    lv_port_sem_take();

    /* 1. Calculate zone starting slot */
    uint8_t start_offset = 0;
    for (uint8_t z = 0; z < zone_idx; z++) {
        start_offset += graph->zones[z].count;
    }

    uint8_t zone_capacity = graph->zones[zone_idx].count;

    /* 2. Clear or populate slots */
    if (!points || count == 0) {
        for (uint8_t i = 0; i < zone_capacity; i++) {
            uint8_t target_idx = start_offset + i;
            if (target_idx < HORIZ_BARGRAPH_MAX_POINTS) {
                graph->data[target_idx] = 0.0f;
                graph->has_data[target_idx] = false; /* Invisible gap */
                graph->is_estimated[target_idx] = false;
                graph->is_alarm[target_idx] = false;
                lv_chart_set_value_by_id(graph->chart, graph->series, target_idx, 0);
            }
        }
    } else {
        uint8_t to_copy = (count < zone_capacity) ? (uint8_t)count : zone_capacity;
        uint8_t pad_left = zone_capacity - to_copy;
        uint16_t points_start_idx = (count > zone_capacity) ? (count - zone_capacity) : 0;

        /* Fill unused slots on the left (if any) as empty gaps */
        for (uint8_t p = 0; p < pad_left; p++) {
            uint8_t target_idx = start_offset + p;
            if (target_idx < HORIZ_BARGRAPH_MAX_POINTS) {
                graph->data[target_idx] = 0.0f;
                graph->has_data[target_idx] = false;
                graph->is_estimated[target_idx] = false;
                graph->is_alarm[target_idx] = false;
                lv_chart_set_value_by_id(graph->chart, graph->series, target_idx, 0);
            }
        }

        /* Populate points: NO_DATA becomes an invisible physical gap */
        for (uint8_t i = 0; i < to_copy; i++) {
            uint8_t target_idx = start_offset + pad_left + i;
            const chart_data_point_t *pt = &points[points_start_idx + i];

            if (target_idx < HORIZ_BARGRAPH_MAX_POINTS) {
                bool is_no_data = (pt->flags & DATA_FLAG_NO_DATA) != 0;

                graph->data[target_idx] = is_no_data ? 0.0f : pt->value;
                graph->has_data[target_idx] = !is_no_data; /* FALSE = 100% transparent in draw callback! */
                graph->is_estimated[target_idx] = !is_no_data && ((pt->flags & DATA_FLAG_ESTIMATED) != 0);
                graph->is_alarm[target_idx] = !is_no_data && ((pt->flags & (DATA_FLAG_ALARM_DANGER | DATA_FLAG_ALARM_WARN)) != 0);

                lv_chart_set_value_by_id(graph->chart, graph->series, target_idx, is_no_data ? 0 : (int32_t)(pt->value + 0.5f));
            }
        }
    }

    lv_chart_refresh(graph->chart);
    lv_port_sem_give();
}

void horiz_bargraph_set_range(horiz_bargraph_t *graph, float min_val, float max_val) {
    if (!graph || !graph->chart) return;
    lv_port_sem_take();
    graph->min_value = min_val;
    graph->max_value = max_val;
    lv_chart_set_range(graph->chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)min_val, (int32_t)max_val);
    lv_chart_refresh(graph->chart);
    lv_port_sem_give();
}

void horiz_bargraph_set_type(horiz_bargraph_t *graph, horiz_bargraph_type_t chart_type) {
    if (!graph || !graph->chart) return;
    lv_port_sem_take();
    graph->chart_type = chart_type;
    switch (chart_type) {
        case HORIZ_BARGRAPH_TYPE_LINE:
            lv_chart_set_type(graph->chart, LV_CHART_TYPE_LINE);
            lv_obj_set_style_line_width(graph->chart, 2, LV_PART_ITEMS);
            break;
        case HORIZ_BARGRAPH_TYPE_POINT:
            lv_chart_set_type(graph->chart, LV_CHART_TYPE_LINE);
            lv_obj_set_style_line_width(graph->chart, 0, LV_PART_ITEMS);
            break;
        case HORIZ_BARGRAPH_TYPE_BAR:
        default:
            lv_chart_set_type(graph->chart, LV_CHART_TYPE_BAR);
            break;
    }
    lv_chart_refresh(graph->chart);
    lv_port_sem_give();
}

void horiz_bargraph_set_media_color(horiz_bargraph_t *graph, lv_color_t color) {
    if (!graph) return;
    lv_port_sem_take();
    graph->bar_color = color;
    if (graph->chart && graph->series) {
        lv_chart_set_series_color(graph->chart, graph->series, color);
        lv_chart_refresh(graph->chart);
    }
    lv_port_sem_give();
}

void horiz_bargraph_destroy(horiz_bargraph_t *graph) {
    if (!graph) return;
    lv_port_sem_take();
    if (graph->container) {
        lv_obj_delete(graph->container);
        graph->container = NULL;
    }
    graph->chart = NULL;
    graph->series = NULL;
    graph->lbl_title = NULL;
    for (uint8_t i = 0; i < HORIZ_BARGRAPH_MAX_LABELS; i++) {
        graph->axis_labels_obj[i] = NULL;
    }
    lv_port_sem_give();
}

/* ── Generic widget_base.h Bridge Implementation ────────────────────────── */

static void _horiz_bargraph_widget_update(widget_t *self, float value, const char *unit) {
    if (!self || !self->user_data) return;
    horiz_bargraph_t *graph = (horiz_bargraph_t *)self->user_data;
    uint8_t target_idx = (graph->data_count > 0) ? (graph->data_count - 1) : 0;
    horiz_bargraph_update_point(graph, target_idx, value, false, false);
}

static void _horiz_bargraph_widget_destroy(widget_t *self) {
    if (!self) return;
    if (self->user_data) {
        horiz_bargraph_destroy((horiz_bargraph_t *)self->user_data);
        free(self->user_data);
        self->user_data = NULL;
    }
    free(self);
}

widget_t* horiz_bargraph_create(lv_obj_t *parent, const char *id, const char *binding_key,
                                int16_t x, int16_t y, uint16_t w, uint16_t h) {
    if (!parent || !id) return NULL;
    widget_t *base = (widget_t *)calloc(1, sizeof(widget_t));
    if (!base) return NULL;
    horiz_bargraph_t *graph = (horiz_bargraph_t *)calloc(1, sizeof(horiz_bargraph_t));
    if (!graph) { free(base); return NULL; }

    strncpy(base->id, id, sizeof(base->id) - 1);
    if (binding_key) strncpy(base->binding_key, binding_key, sizeof(base->binding_key) - 1);

    graph->parent = parent;
    graph->x = x;
    graph->y = y;
    graph->width = w;
    graph->height = h;

    if (horiz_bargraph_init(graph) != 0) {
        free(graph);
        free(base);
        return NULL;
    }

    base->container = graph->container;
    base->user_data = graph;
    base->update_val = _horiz_bargraph_widget_update;
    base->update_st = NULL;
    base->destroy = _horiz_bargraph_widget_destroy;
    return base;
}