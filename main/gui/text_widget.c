/**
 * @file text_widget.c
 * @brief Multi-row text and telemetry card supporting 3x1 to 10x8 grid dimensions.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "text_widget.h"
#include "lv_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_font0);

#define DEFAULT_BG_COLOR      lv_color_hex(0x282828)
#define DEFAULT_TEXT_COLOR    lv_color_hex(0xFFFFFF)
#define DEFAULT_TITLE_COLOR   lv_color_hex(0x9E9E9E)
#define DEFAULT_LABEL_COLOR   lv_color_hex(0x9E9E9E)
#define DEFAULT_VALUE_COLOR   lv_color_hex(0x52AAE5)

static inline bool is_color_empty(lv_color_t c) {
    static const lv_color_t zero = {0};
    return memcmp(&c, &zero, sizeof(lv_color_t)) == 0;
}

static void text_widget_click_handler(lv_event_t *e) {
    lv_obj_t *container = lv_event_get_target_obj(e);
    text_widget_t *widget = (text_widget_t *)lv_obj_get_user_data(container);
    if (widget && widget->click_cb) widget->click_cb(e);
}

/**
 * @brief Internal helper to create the LVGL sub-elements for a single pair (Compact Row Layout).
 */
static void _create_pair_ui(text_widget_t *widget, uint8_t idx) {
    if (!widget || !widget->container || idx >= TEXT_WIDGET_MAX_PAIRS) return;

    text_widget_pair_t *pair = &widget->pairs[idx];
    lv_color_t label_col = !is_color_empty(pair->label_color) ? pair->label_color : DEFAULT_LABEL_COLOR;
    lv_color_t value_col = !is_color_empty(pair->value_color) ? pair->value_color : DEFAULT_VALUE_COLOR;

    /* Row container with WRAP flow: breaks into two lines if text is too wide */
    lv_obj_t *box = lv_obj_create(widget->container);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    
    /* Allow wrapping when label + value exceed card width */
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    widget->pair_conts[idx] = box;

    /* 1. Label Object with Wrap enabled */
    if (pair->label && pair->label[0] != '\0') {
        widget->labels[idx] = lv_label_create(box);
        lv_label_set_text(widget->labels[idx], pair->label);
        lv_obj_set_style_text_font(widget->labels[idx], widget->font, LV_PART_MAIN);
        lv_obj_set_style_text_color(widget->labels[idx], label_col, LV_PART_MAIN);
        lv_label_set_long_mode(widget->labels[idx], LV_LABEL_LONG_WRAP);
        lv_obj_remove_flag(widget->labels[idx], LV_OBJ_FLAG_CLICKABLE);
    } else {
        widget->labels[idx] = NULL;
    }

    /* 2. Value Object with Wrap enabled */
    widget->values[idx] = lv_label_create(box);
    lv_label_set_text(widget->values[idx], (pair->value && pair->value[0] != '\0') ? pair->value : "--");
    const lv_font_t *val_font = pair->font ? pair->font : widget->font;
    lv_obj_set_style_text_font(widget->values[idx], val_font, LV_PART_MAIN);
    lv_obj_set_style_text_color(widget->values[idx], value_col, LV_PART_MAIN);
    lv_obj_set_style_text_align(widget->values[idx], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_label_set_long_mode(widget->values[idx], LV_LABEL_LONG_WRAP);
    lv_obj_remove_flag(widget->values[idx], LV_OBJ_FLAG_CLICKABLE);
}

int text_widget_init(text_widget_t *widget) {
    if (!widget || !widget->parent) return -1;

    lv_port_sem_take();

    if (!widget->font) widget->font = &ui_font_font0;
    if (is_color_empty(widget->bg_color)) widget->bg_color = DEFAULT_BG_COLOR;
    if (is_color_empty(widget->text_color)) widget->text_color = DEFAULT_TEXT_COLOR;
    if (is_color_empty(widget->title_color)) widget->title_color = DEFAULT_TITLE_COLOR;
    if (widget->width == 0) widget->width = 280;  /* 6x5 default */
    if (widget->height == 0) widget->height = 232;

    bool is_single_row = (widget->height <= 45);

    /* 1. Main Card Container */
    widget->container = lv_obj_create(widget->parent);
    lv_obj_set_size(widget->container, widget->width, widget->height);
    lv_obj_set_pos(widget->container, widget->x, widget->y);
    
    if (is_single_row) {
        lv_obj_remove_flag(widget->container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_pad_all(widget->container, 6, LV_PART_MAIN);
    } else {
        lv_obj_add_flag(widget->container, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(widget->container, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(widget->container, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_pad_all(widget->container, 10, LV_PART_MAIN);
        lv_obj_set_style_pad_row(widget->container, 8, LV_PART_MAIN);
    }

    lv_obj_set_style_radius(widget->container, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->container, widget->bg_color, LV_PART_MAIN);
    lv_obj_set_style_border_width(widget->container, 0, LV_PART_MAIN);

    lv_obj_set_flex_flow(widget->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(widget->container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_set_user_data(widget->container, widget);
    lv_obj_add_event_cb(widget->container, text_widget_click_handler, LV_EVENT_CLICKED, NULL);

    widget->lbl_title = NULL;
    for (uint8_t i = 0; i < TEXT_WIDGET_MAX_PAIRS; i++) {
        widget->pair_conts[i] = NULL;
        widget->labels[i] = NULL;
        widget->values[i] = NULL;
    }

    /* 2. Title (suppressed on single-row 3x1 cards) */
    if (!is_single_row && widget->title && widget->title[0] != '\0') {
        widget->lbl_title = lv_label_create(widget->container);
        lv_label_set_text(widget->lbl_title, widget->title);
        lv_obj_set_style_text_font(widget->lbl_title, widget->font, LV_PART_MAIN);
        lv_obj_set_style_text_color(widget->lbl_title, widget->title_color, LV_PART_MAIN);
        lv_obj_set_width(widget->lbl_title, lv_pct(100));
        lv_label_set_long_mode(widget->lbl_title, LV_LABEL_LONG_WRAP);
        lv_obj_remove_flag(widget->lbl_title, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 3. Create pairs */
    for (uint8_t i = 0; i < widget->pair_count && i < TEXT_WIDGET_MAX_PAIRS; i++) {
        _create_pair_ui(widget, i);
    }

    lv_port_sem_give();
    return 0;
}

int text_widget_add_pair(text_widget_t *widget, const char *binding_key, const char *label,
                         const char *initial_value, const char *unit, lv_color_t value_color) {
    if (!widget || widget->pair_count >= TEXT_WIDGET_MAX_PAIRS) return -1;

    uint8_t idx = widget->pair_count;
    text_widget_pair_t *pair = &widget->pairs[idx];
    memset(pair, 0, sizeof(text_widget_pair_t));

    if (binding_key) strncpy(pair->binding_key, binding_key, sizeof(pair->binding_key) - 1);
    pair->label = label;
    pair->value = initial_value ? initial_value : "--";
    pair->unit = unit;
    pair->label_color = DEFAULT_LABEL_COLOR;
    pair->value_color = !is_color_empty(value_color) ? value_color : DEFAULT_VALUE_COLOR;

    if (widget->container) {
        lv_port_sem_take();
        _create_pair_ui(widget, idx);
        lv_port_sem_give();
    }
    widget->pair_count++;
    return 0;
}

void text_widget_update_value(text_widget_t *widget, uint8_t pair_index, const char* value) {
    if (!widget || pair_index >= TEXT_WIDGET_MAX_PAIRS || pair_index >= widget->pair_count) return;
    lv_port_sem_take();
    if (widget->values[pair_index]) lv_label_set_text(widget->values[pair_index], value ? value : "");
    lv_port_sem_give();
}

void text_widget_update_binding(text_widget_t *widget, const char *key, float value,
                                const char *unit, bool is_estimated, bool is_alarm) {
    if (!widget || !key) return;

    lv_port_sem_take();
    for (uint8_t i = 0; i < widget->pair_count && i < TEXT_WIDGET_MAX_PAIRS; i++) {
        if (widget->pairs[i].binding_key[0] != '\0' && strcmp(widget->pairs[i].binding_key, key) == 0) {
            char buf[64];
            const char *u = (unit && unit[0] != '\0') ? unit : (widget->pairs[i].unit ? widget->pairs[i].unit : "");
            if (u[0] != '\0') snprintf(buf, sizeof(buf), "%.2f %s", value, u);
            else snprintf(buf, sizeof(buf), "%.2f", value);

            if (widget->values[i]) {
                lv_label_set_text(widget->values[i], buf);
                if (is_alarm) {
                    lv_obj_set_style_text_color(widget->values[i], lv_color_hex(0xE74C3C), LV_PART_MAIN);
                } else if (is_estimated) {
                    lv_obj_set_style_text_color(widget->values[i], lv_color_hex(0xBDC3C7), LV_PART_MAIN);
                } else {
                    lv_color_t def = !is_color_empty(widget->pairs[i].value_color) ? widget->pairs[i].value_color : DEFAULT_VALUE_COLOR;
                    lv_obj_set_style_text_color(widget->values[i], def, LV_PART_MAIN);
                }
            }
        }
    }
    lv_port_sem_give();
}

void text_widget_update_label(text_widget_t *widget, uint8_t pair_index, const char* label) {
    if (!widget || pair_index >= TEXT_WIDGET_MAX_PAIRS || pair_index >= widget->pair_count) return;
    lv_port_sem_take();
    if (widget->labels[pair_index]) lv_label_set_text(widget->labels[pair_index], label ? label : "");
    lv_port_sem_give();
}

void text_widget_set_title(text_widget_t *widget, const char* title) {
    if (!widget || widget->height <= 45) return;
    lv_port_sem_take();
    widget->title = title;
    if (widget->container) {
        if (!widget->lbl_title && title && title[0] != '\0') {
            widget->lbl_title = lv_label_create(widget->container);
            lv_obj_set_style_text_font(widget->lbl_title, widget->font, LV_PART_MAIN);
            lv_obj_set_style_text_color(widget->lbl_title, widget->title_color, LV_PART_MAIN);
            lv_obj_set_width(widget->lbl_title, lv_pct(100));
            lv_label_set_long_mode(widget->lbl_title, LV_LABEL_LONG_WRAP);
            lv_obj_move_to_index(widget->lbl_title, 0);
        }
        if (widget->lbl_title) {
            lv_label_set_text(widget->lbl_title, title ? title : "");
            if (!title || title[0] == '\0') lv_obj_add_flag(widget->lbl_title, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(widget->lbl_title, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_port_sem_give();
}

void text_widget_set_value_color(text_widget_t *widget, uint8_t pair_index, lv_color_t color) {
    if (!widget || pair_index >= TEXT_WIDGET_MAX_PAIRS || pair_index >= widget->pair_count) return;
    lv_port_sem_take();
    widget->pairs[pair_index].value_color = color;
    if (widget->values[pair_index]) lv_obj_set_style_text_color(widget->values[pair_index], color, LV_PART_MAIN);
    lv_port_sem_give();
}

void text_widget_set_label_color(text_widget_t *widget, uint8_t pair_index, lv_color_t color) {
    if (!widget || pair_index >= TEXT_WIDGET_MAX_PAIRS || pair_index >= widget->pair_count) return;
    lv_port_sem_take();
    widget->pairs[pair_index].label_color = color;
    if (widget->labels[pair_index]) lv_obj_set_style_text_color(widget->labels[pair_index], color, LV_PART_MAIN);
    lv_port_sem_give();
}

void text_widget_destroy(text_widget_t *widget) {
    if (!widget) return;
    lv_port_sem_take();
    if (widget->container) {
        lv_obj_delete(widget->container);
        widget->container = NULL;
    }
    widget->lbl_title = NULL;
    for (uint8_t i = 0; i < TEXT_WIDGET_MAX_PAIRS; i++) {
        widget->pair_conts[i] = NULL;
        widget->labels[i] = NULL;
        widget->values[i] = NULL;
    }
    lv_port_sem_give();
}

static void _text_widget_widget_update(widget_t *self, float value, const char *unit) {
    if (!self || !self->user_data) return;
    text_widget_t *widget = (text_widget_t *)self->user_data;
    if (self->binding_key[0] != '\0') {
        text_widget_update_binding(widget, self->binding_key, value, unit, false, false);
    } else {
        char buf[64];
        if (unit && unit[0] != '\0') snprintf(buf, sizeof(buf), "%.2f %s", value, unit);
        else snprintf(buf, sizeof(buf), "%.2f", value);
        text_widget_update_value(widget, 0, buf);
    }
}

static void _text_widget_widget_destroy(widget_t *self) {
    if (!self) return;
    if (self->user_data) {
        text_widget_destroy((text_widget_t *)self->user_data);
        free(self->user_data);
        self->user_data = NULL;
    }
    free(self);
}

widget_t* text_widget_create(lv_obj_t *parent, const char *id, const char *binding_key,
                             int16_t x, int16_t y, uint16_t w, uint16_t h) {
    if (!parent || !id) return NULL;
    widget_t *base = (widget_t *)calloc(1, sizeof(widget_t));
    if (!base) return NULL;
    text_widget_t *widget = (text_widget_t *)calloc(1, sizeof(text_widget_t));
    if (!widget) { free(base); return NULL; }

    strncpy(base->id, id, sizeof(base->id) - 1);
    if (binding_key) strncpy(base->binding_key, binding_key, sizeof(base->binding_key) - 1);

    widget->parent = parent;
    widget->x = x;
    widget->y = y;
    widget->width = w;
    widget->height = h;

    if (text_widget_init(widget) != 0) {
        free(widget);
        free(base);
        return NULL;
    }

    base->container = widget->container;
    base->user_data = widget;
    base->update_val = _text_widget_widget_update;
    base->update_st = NULL;
    base->destroy = _text_widget_widget_destroy;
    return base;
}