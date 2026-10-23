/**
 * @file gui_footer.c
 * @brief Permanent bottom status bar implementation.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "gui_footer.h"
#include "lv_port.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(lv_font_montserrat_20);

#define FOOTER_BG_COLOR     lv_color_hex(0x000000) /* Pure black seamless bezel */
#define FOOTER_DEFAULT_TEXT lv_color_hex(0x7F8C8D) /* Subtle muted grey */

static lv_obj_t *s_footer_cont = NULL;
static lv_obj_t *s_lbl_left = NULL;
static lv_obj_t *s_lbl_right = NULL;

void gui_footer_init(lv_obj_t *parent) {
    if (s_footer_cont || !parent) return;

    lv_port_sem_take();

    /* 1. Main Footer Container */
    s_footer_cont = lv_obj_create(parent);
    lv_obj_set_size(s_footer_cont, 480, FOOTER_HEIGHT);
    lv_obj_set_pos(s_footer_cont, 0, FOOTER_POS_Y);
    lv_obj_remove_flag(s_footer_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_footer_cont, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_set_style_radius(s_footer_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_footer_cont, FOOTER_BG_COLOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_footer_cont, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_footer_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_footer_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_footer_cont, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_footer_cont, 10, LV_PART_MAIN);

    /* 2. Left Label (IP / Network status) */
    s_lbl_left = lv_label_create(s_footer_cont);
    lv_label_set_text(s_lbl_left, "");
    lv_obj_set_style_text_font(s_lbl_left, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_left, FOOTER_DEFAULT_TEXT, LV_PART_MAIN);
    lv_obj_align(s_lbl_left, LV_ALIGN_LEFT_MID, 0, 0);

    /* 3. Right Label (Last update / Telemetry heartbeat) */
    s_lbl_right = lv_label_create(s_footer_cont);
    lv_label_set_text(s_lbl_right, "");
    lv_obj_set_style_text_font(s_lbl_right, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_right, FOOTER_DEFAULT_TEXT, LV_PART_MAIN);
    lv_obj_align(s_lbl_right, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_port_sem_give();
}

void gui_footer_set_left_text(const char *text) {
    if (!s_lbl_left) return;
    lv_port_sem_take();
    lv_label_set_text(s_lbl_left, text ? text : "");
    lv_port_sem_give();
}

void gui_footer_set_right_text(const char *text) {
    if (!s_lbl_right) return;
    lv_port_sem_take();
    lv_label_set_text(s_lbl_right, text ? text : "");
    lv_port_sem_give();
}

void gui_footer_set_color(lv_color_t color) {
    if (!s_lbl_left || !s_lbl_right) return;
    lv_port_sem_take();
    lv_obj_set_style_text_color(s_lbl_left, color, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_right, color, LV_PART_MAIN);
    lv_port_sem_give();
}

void gui_footer_set_visible(bool visible) {
    if (!s_footer_cont) return;
    lv_port_sem_take();
    if (visible) {
        lv_obj_remove_flag(s_footer_cont, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_footer_cont, LV_OBJ_FLAG_HIDDEN);
    }
    lv_port_sem_give();
}