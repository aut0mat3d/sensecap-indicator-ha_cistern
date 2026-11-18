/**
 * @file sensor_card.c
 * @brief Implementation of the modular sensor card widget.
 */

#include "gui_theme.h"
#include "sensor_card.h"
#include "lv_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

LV_FONT_DECLARE(ui_font_font0);
LV_FONT_DECLARE(ui_font_font2);

#define SENSOR_BG_COLOR       lv_color_hex(0x282828)
#define SENSOR_TITLE_COLOR    lv_color_hex(0x9E9E9E)
/* Danger color from the theme (/gui/theme.json) */
#define SENSOR_ALARM_COLOR    lv_color_hex(gui_theme_severity_color(0))
#define SENSOR_IDLE_BORDER    lv_color_hex(0x000000)

static void sensor_card_click_handler(lv_event_t *e) {
    lv_obj_t *container = lv_event_get_target(e);
    sensor_card_t *card = (sensor_card_t *)lv_obj_get_user_data(container);
    if (card && card->click_cb) {
        card->click_cb(e);
    }
}

static int sensor_card_init(sensor_card_t *card) {
    if (!card || !card->parent) return -1;

    lv_port_sem_take();

    /* 1. Main Container */
    card->container = lv_obj_create(card->parent);
    lv_obj_set_size(card->container, card->width, card->height);
    lv_obj_set_pos(card->container, card->x, card->y);
    lv_obj_remove_flag(card->container, LV_OBJ_FLAG_SCROLLABLE);
    
    lv_obj_set_style_radius(card->container, 12, LV_PART_MAIN);
    lv_obj_set_style_bg_color(card->container, SENSOR_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card->container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card->container, 0, LV_PART_MAIN);

    /* 2px internal seamless border for alarms */
    lv_obj_set_style_border_width(card->container, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(card->container, SENSOR_IDLE_BORDER, LV_PART_MAIN);
    lv_obj_set_style_outline_width(card->container, 0, LV_PART_MAIN);

    lv_obj_set_user_data(card->container, card);
    lv_obj_add_event_cb(card->container, sensor_card_click_handler, LV_EVENT_CLICKED, NULL);

    /* 2. Icon (Top Center) */
    if (card->icon_src) {
        if (card->is_icon_symbol) {
            card->obj_icon = lv_label_create(card->container);
            lv_label_set_text(card->obj_icon, (const char *)card->icon_src);
            lv_obj_set_style_text_font(card->obj_icon, &ui_font_font2, LV_PART_MAIN);
            lv_obj_set_style_text_color(card->obj_icon, card->accent_color, LV_PART_MAIN);
        } else {
            card->obj_icon = lv_image_create(card->container);
            lv_image_set_src(card->obj_icon, card->icon_src);
        }
        lv_obj_align(card->obj_icon, LV_ALIGN_TOP_MID, 0, 16);
        lv_obj_remove_flag(card->obj_icon, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 3. Value Label (Center-Left) */
    card->lbl_value = lv_label_create(card->container);
    lv_label_set_text(card->lbl_value, "--");
    lv_obj_set_style_text_color(card->lbl_value, card->accent_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(card->lbl_value, &ui_font_font2, LV_PART_MAIN);
    
    /* Auto-size and align the value label */
    lv_obj_set_width(card->lbl_value, LV_SIZE_CONTENT);
    lv_obj_align(card->lbl_value, LV_ALIGN_CENTER, -15, 0);

    /* 4. Unit Label (Center-Right, next to value) */
    card->lbl_unit = lv_label_create(card->container);
    lv_label_set_text(card->lbl_unit, card->unit ? card->unit : "");
    lv_obj_set_style_text_color(card->lbl_unit, card->accent_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(card->lbl_unit, &ui_font_font0, LV_PART_MAIN);
    lv_obj_align_to(card->lbl_unit, card->lbl_value, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);

    /* 5. Title Label (Bottom Center) */
    if (card->title) {
        card->lbl_title = lv_label_create(card->container);
        lv_label_set_text(card->lbl_title, card->title);
        lv_obj_set_style_text_color(card->lbl_title, SENSOR_TITLE_COLOR, LV_PART_MAIN);
        lv_obj_set_style_text_font(card->lbl_title, &ui_font_font0, LV_PART_MAIN);
        lv_obj_align(card->lbl_title, LV_ALIGN_BOTTOM_MID, 0, -12);
    }

    lv_port_sem_give();
    return 0;
}

void sensor_card_update_value(sensor_card_t *card, float value, bool is_alarm) {
    if (!card || !card->lbl_value) return;

    bool is_invalid = isnan(value) || isinf(value);
    card->current_value = value;
    card->is_alarm = is_invalid ? false : is_alarm;

    lv_port_sem_take();

    /* Format string dynamically based on required decimals (NAN != 0) */
    char buf[32];
    if (is_invalid) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        char fmt[10];
        snprintf(fmt, sizeof(fmt), "%%.%df", card->decimals);
        snprintf(buf, sizeof(buf), fmt, value);
    }

    lv_label_set_text(card->lbl_value, buf);

    /* Realign unit to stay perfectly next to the potentially resized value */
    lv_obj_align_to(card->lbl_unit, card->lbl_value, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, 0);

    /* Handle Alert coloring */
    if (card->is_alarm) {
        lv_obj_set_style_text_color(card->lbl_value, SENSOR_ALARM_COLOR, LV_PART_MAIN);
        lv_obj_set_style_text_color(card->lbl_unit, SENSOR_ALARM_COLOR, LV_PART_MAIN);
        if (card->container) {
            lv_obj_set_style_border_color(card->container, SENSOR_ALARM_COLOR, LV_PART_MAIN);
        }
    } else {
        lv_obj_set_style_text_color(card->lbl_value, card->accent_color, LV_PART_MAIN);
        lv_obj_set_style_text_color(card->lbl_unit, card->accent_color, LV_PART_MAIN);
        if (card->container) {
            lv_obj_set_style_border_color(card->container, SENSOR_IDLE_BORDER, LV_PART_MAIN);
        }
    }

    lv_port_sem_give();
}

void sensor_card_destroy(sensor_card_t *card) {
    if (!card) return;
    lv_port_sem_take();
    if (card->container) {
        lv_obj_delete(card->container);
        card->container = NULL;
    }
    lv_port_sem_give();
}

/* ── Generic widget_base.h Bridge ────────────────────────── */

static void _sensor_card_widget_update(widget_t *self, float value, const char *unit) {
    if (!self || !self->user_data) return;
    sensor_card_t *card = (sensor_card_t *)self->user_data;
    
    /* Update unit if dynamically provided and different */
    if (unit && unit[0] != '\0' && strcmp(card->unit, unit) != 0) {
        lv_port_sem_take();
        card->unit = unit; /* Note: assumes persistent string literal or managed memory */
        if (card->lbl_unit) lv_label_set_text(card->lbl_unit, card->unit);
        lv_port_sem_give();
    }
    
    sensor_card_update_value(card, value, card->is_alarm);
}

static void _sensor_card_widget_destroy(widget_t *self) {
    if (!self) return;
    if (self->user_data) {
        sensor_card_destroy((sensor_card_t *)self->user_data);
        free(self->user_data);
        self->user_data = NULL;
    }
    free(self);
}

widget_t* sensor_card_create(lv_obj_t *parent, const char *id, const char *binding_key,
                             int16_t x, int16_t y, uint16_t w, uint16_t h,
                             const char *title, const char *unit, 
                             const void *icon_src, bool is_symbol,
                             lv_color_t accent_color, uint8_t decimals) {
    if (!parent || !id) return NULL;

    widget_t *base = (widget_t *)calloc(1, sizeof(widget_t));
    if (!base) return NULL;

    sensor_card_t *card = (sensor_card_t *)calloc(1, sizeof(sensor_card_t));
    if (!card) {
        free(base);
        return NULL;
    }

    strncpy(base->id, id, sizeof(base->id) - 1);
    if (binding_key) {
        strncpy(base->binding_key, binding_key, sizeof(base->binding_key) - 1);
    }

    card->parent = parent;
    card->x = x;
    card->y = y;
    card->width = w;
    card->height = h;
    card->title = title;
    card->unit = unit;
    card->icon_src = icon_src;
    card->is_icon_symbol = is_symbol;
    card->accent_color = accent_color;
    card->decimals = decimals;

    if (sensor_card_init(card) != 0) {
        free(card);
        free(base);
        return NULL;
    }

    base->container = card->container;
    base->user_data = card;
    base->update_val = _sensor_card_widget_update;
    base->update_st = NULL;
    base->destroy = _sensor_card_widget_destroy;

    return base;
}