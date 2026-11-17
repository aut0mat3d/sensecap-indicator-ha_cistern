/**
 * @file collapsible_list.c
 * @brief Modular accordion/tree-view list widget implementation using LVGL 9.
 *        Follows the 10x8 seamless tile grid specification with zero external outlines
 *        and strictly internal border rendering.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "collapsible_list.h"
#include "lv_port.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(ui_font_font0);

#define DEFAULT_BG_COLOR          lv_color_hex(0x101418)
#define DEFAULT_HEADER_BG_COLOR   lv_color_hex(0x20262C)
#define DEFAULT_ITEM_BG_COLOR     lv_color_hex(0x161B20)
#define DEFAULT_TEXT_COLOR        lv_color_hex(0xFFFFFF)
#define DEFAULT_MUTED_COLOR       lv_color_hex(0x9E9E9E)
#define DEFAULT_ACCENT_COLOR      lv_color_hex(0x52AAE5)
#define DEFAULT_BORDER_COLOR      lv_color_hex(0x2C3E50)

static inline bool is_color_empty(lv_color_t c) {
    static const lv_color_t zero = {0};
    return memcmp(&c, &zero, sizeof(lv_color_t)) == 0;
}

/* ── Internal Event Handlers ────────────────────────────────────────────── */

static void _on_header_clicked(lv_event_t *e) {
    collapsible_list_t *widget = (collapsible_list_t *)lv_event_get_user_data(e);
    lv_obj_t *header_btn = lv_event_get_target(e);
    if (!widget || !header_btn) return;

    /* Match the target header button to its group descriptor */
    for (uint8_t g = 0; g < widget->group_count; g++) {
        if (widget->groups[g].header_btn == header_btn) {
            bool next_state = !widget->groups[g].is_expanded;
            collapsible_list_set_group_expanded(widget, g, next_state);
            break;
        }
    }
}

static void _on_item_clicked(lv_event_t *e) {
    collapsible_list_t *widget = (collapsible_list_t *)lv_event_get_user_data(e);
    /* Retrieve the row button itself even if an inner label was touched */
    lv_obj_t *row_btn = lv_event_get_current_target(e);
    if (!widget || !row_btn || !widget->item_click_cb) return;

    /* Locate the group and item index */
    for (uint8_t g = 0; g < widget->group_count; g++) {
        collapsible_list_group_t *grp = &widget->groups[g];
        for (uint8_t i = 0; i < grp->item_count; i++) {
            if (grp->items[i].row_btn == row_btn) {
                widget->item_click_cb(widget, g, i, grp->items[i].user_data);
                return;
            }
        }
    }
}

/* ── Internal UI Builders ───────────────────────────────────────────────── */

static void _build_group_header_ui(collapsible_list_t *widget, uint8_t group_idx) {
    collapsible_list_group_t *grp = &widget->groups[group_idx];

    /* 1. Header Button (Row layout) */
    grp->header_btn = lv_button_create(widget->container);
    lv_obj_set_width(grp->header_btn, lv_pct(100));
    lv_obj_set_height(grp->header_btn, 38);
    lv_obj_remove_flag(grp->header_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(grp->header_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(grp->header_btn, widget->header_bg_color, LV_PART_MAIN);
    
    /* Strict internal border: zero external outline */
    lv_obj_set_style_border_width(grp->header_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(grp->header_btn, DEFAULT_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_outline_width(grp->header_btn, 0, LV_PART_MAIN);

    lv_obj_set_style_pad_hor(grp->header_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(grp->header_btn, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(grp->header_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grp->header_btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grp->header_btn, 8, LV_PART_MAIN);

    lv_obj_add_event_cb(grp->header_btn, _on_header_clicked, LV_EVENT_CLICKED, widget);

    /* Arrow icon indicating fold state */
    grp->lbl_arrow = lv_label_create(grp->header_btn);
    lv_label_set_text(grp->lbl_arrow, grp->is_expanded ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(grp->lbl_arrow, DEFAULT_MUTED_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(grp->lbl_arrow, widget->font, LV_PART_MAIN);

    /* Group title */
    grp->lbl_title = lv_label_create(grp->header_btn);
    lv_label_set_text(grp->lbl_title, grp->title);
    lv_obj_set_style_text_color(grp->lbl_title, widget->text_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(grp->lbl_title, widget->font, LV_PART_MAIN);
    lv_obj_set_flex_grow(grp->lbl_title, 1);

    /* 2. Indented Items Container */
    grp->items_cont = lv_obj_create(widget->container);
    lv_obj_set_width(grp->items_cont, lv_pct(100));
    lv_obj_set_height(grp->items_cont, LV_SIZE_CONTENT);
    lv_obj_remove_flag(grp->items_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(grp->items_cont, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(grp->items_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(grp->items_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(grp->items_cont, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(grp->items_cont, 14, LV_PART_MAIN); /* Visual indentation */
    lv_obj_set_flex_flow(grp->items_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(grp->items_cont, 4, LV_PART_MAIN);

    if (!grp->is_expanded) {
        lv_obj_add_flag(grp->items_cont, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _build_item_row_ui(collapsible_list_t *widget, uint8_t group_idx, uint8_t item_idx) {
    collapsible_list_group_t *grp = &widget->groups[group_idx];
    collapsible_list_item_t  *itm = &grp->items[item_idx];

    /* Property Row Button */
    itm->row_btn = lv_button_create(grp->items_cont);
    lv_obj_set_width(itm->row_btn, lv_pct(100));
    lv_obj_set_height(itm->row_btn, 34);
    lv_obj_remove_flag(itm->row_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(itm->row_btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(itm->row_btn, widget->item_bg_color, LV_PART_MAIN);
    
    /* Strict internal border: zero external outline */
    lv_obj_set_style_border_width(itm->row_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(itm->row_btn, lv_color_hex(0x242E38), LV_PART_MAIN);
    lv_obj_set_style_outline_width(itm->row_btn, 0, LV_PART_MAIN);

    lv_obj_set_style_pad_hor(itm->row_btn, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(itm->row_btn, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(itm->row_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(itm->row_btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_add_event_cb(itm->row_btn, _on_item_clicked, LV_EVENT_CLICKED, widget);

    /* Left side: Property Label */
    itm->lbl_label = lv_label_create(itm->row_btn);
    lv_label_set_text(itm->lbl_label, itm->label ? itm->label : "");
    lv_obj_set_style_text_color(itm->lbl_label, DEFAULT_MUTED_COLOR, LV_PART_MAIN);
    lv_obj_set_style_text_font(itm->lbl_label, widget->font, LV_PART_MAIN);
    lv_obj_remove_flag(itm->lbl_label, LV_OBJ_FLAG_CLICKABLE);

    /* Right side container: Value + Edit Icon */
    lv_obj_t *val_box = lv_obj_create(itm->row_btn);
    lv_obj_set_size(val_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(val_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(val_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(val_box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(val_box, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(val_box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(val_box, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(val_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(val_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(val_box, 8, LV_PART_MAIN);

    /* Value text */
    itm->lbl_value = lv_label_create(val_box);
    lv_label_set_text(itm->lbl_value, itm->value_text);
    lv_obj_set_style_text_color(itm->lbl_value, itm->value_color, LV_PART_MAIN);
    lv_obj_set_style_text_font(itm->lbl_value, widget->font, LV_PART_MAIN);
    lv_obj_remove_flag(itm->lbl_value, LV_OBJ_FLAG_CLICKABLE);

    /* Edit symbol */
    itm->lbl_icon = lv_label_create(val_box);
    lv_label_set_text(itm->lbl_icon, LV_SYMBOL_EDIT);
    lv_obj_set_style_text_color(itm->lbl_icon, lv_color_hex(0x7F8C8D), LV_PART_MAIN);
    lv_obj_set_style_text_font(itm->lbl_icon, widget->font, LV_PART_MAIN);
    lv_obj_remove_flag(itm->lbl_icon, LV_OBJ_FLAG_CLICKABLE);
}

/* ── Public API Implementation ─────────────────────────────────────────── */

int collapsible_list_init(collapsible_list_t *widget) {
    if (!widget || !widget->parent) return -1;

    lv_port_sem_take();

    /* Apply default styling */
    if (!widget->font) widget->font = &ui_font_font0;
    if (is_color_empty(widget->bg_color)) widget->bg_color = DEFAULT_BG_COLOR;
    if (is_color_empty(widget->header_bg_color)) widget->header_bg_color = DEFAULT_HEADER_BG_COLOR;
    if (is_color_empty(widget->item_bg_color)) widget->item_bg_color = DEFAULT_ITEM_BG_COLOR;
    if (is_color_empty(widget->text_color)) widget->text_color = DEFAULT_TEXT_COLOR;

    /* Snap to default grid dimensions if unspecified */
    if (widget->width == 0) widget->width = COLLAPSIBLE_LIST_DEFAULT_W;
    if (widget->height == 0) widget->height = COLLAPSIBLE_LIST_DEFAULT_H;

    /* Root Scrollable Container */
    widget->container = lv_obj_create(widget->parent);
    lv_obj_set_size(widget->container, widget->width, widget->height);
    lv_obj_set_pos(widget->container, widget->x, widget->y);
    lv_obj_add_flag(widget->container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(widget->container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(widget->container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_radius(widget->container, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(widget->container, widget->bg_color, LV_PART_MAIN);
    
    /* Strict internal border: 1px subtle separation, zero external outline */
    lv_obj_set_style_border_width(widget->container, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(widget->container, DEFAULT_BORDER_COLOR, LV_PART_MAIN);
    lv_obj_set_style_outline_width(widget->container, 0, LV_PART_MAIN);
    
    lv_obj_set_style_pad_all(widget->container, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(widget->container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(widget->container, 6, LV_PART_MAIN);

    /* Render any existing groups and items configured prior to init */
    for (uint8_t g = 0; g < widget->group_count; g++) {
        _build_group_header_ui(widget, g);
        for (uint8_t i = 0; i < widget->groups[g].item_count; i++) {
            _build_item_row_ui(widget, g, i);
        }
    }

    lv_port_sem_give();
    return 0;
}

void collapsible_list_destroy(collapsible_list_t *widget) {
    if (!widget) return;
    lv_port_sem_take();

    if (widget->container) {
        lv_obj_delete(widget->container);
        widget->container = NULL;
    }

    /* Reset all internal widget pointers */
    for (uint8_t g = 0; g < COLLAPSIBLE_LIST_MAX_GROUPS; g++) {
        widget->groups[g].header_btn = NULL;
        widget->groups[g].lbl_arrow = NULL;
        widget->groups[g].lbl_title = NULL;
        widget->groups[g].items_cont = NULL;
        for (uint8_t i = 0; i < COLLAPSIBLE_LIST_MAX_ITEMS_PER_GROUP; i++) {
            widget->groups[g].items[i].row_btn = NULL;
            widget->groups[g].items[i].lbl_label = NULL;
            widget->groups[g].items[i].lbl_value = NULL;
            widget->groups[g].items[i].lbl_icon = NULL;
        }
    }

    lv_port_sem_give();
}

void collapsible_list_clear(collapsible_list_t *widget) {
    if (!widget) return;
    lv_port_sem_take();

    if (widget->container) {
        lv_obj_clean(widget->container);
    }

    for (uint8_t g = 0; g < widget->group_count; g++) {
        widget->groups[g].item_count = 0;
    }
    widget->group_count = 0;

    lv_port_sem_give();
}

int collapsible_list_add_group(collapsible_list_t *widget, const char *group_id,
                               const char *title, bool start_expanded) {
    if (!widget || widget->group_count >= COLLAPSIBLE_LIST_MAX_GROUPS) return -1;

    uint8_t idx = widget->group_count;
    collapsible_list_group_t *grp = &widget->groups[idx];
    memset(grp, 0, sizeof(collapsible_list_group_t));

    if (group_id) strlcpy(grp->group_id, group_id, sizeof(grp->group_id));
    if (title) strlcpy(grp->title, title, sizeof(grp->title));
    grp->is_expanded = start_expanded;
    grp->item_count = 0;

    if (widget->container) {
        lv_port_sem_take();
        _build_group_header_ui(widget, idx);
        lv_port_sem_give();
    }

    widget->group_count++;
    return idx;
}

int collapsible_list_add_item(collapsible_list_t *widget, uint8_t group_idx,
                              const char *item_id, const char *label,
                              const char *initial_value, lv_color_t value_color,
                              void *user_data) {
    if (!widget || group_idx >= widget->group_count) return -1;

    collapsible_list_group_t *grp = &widget->groups[group_idx];
    if (grp->item_count >= COLLAPSIBLE_LIST_MAX_ITEMS_PER_GROUP) return -1;

    uint8_t idx = grp->item_count;
    collapsible_list_item_t *itm = &grp->items[idx];
    memset(itm, 0, sizeof(collapsible_list_item_t));

    if (item_id) strlcpy(itm->item_id, item_id, sizeof(itm->item_id));
    itm->label = label;
    if (initial_value) strlcpy(itm->value_text, initial_value, sizeof(itm->value_text));
    else strlcpy(itm->value_text, "--", sizeof(itm->value_text));

    itm->value_color = !is_color_empty(value_color) ? value_color : DEFAULT_ACCENT_COLOR;
    itm->user_data = user_data;

    if (widget->container && grp->items_cont) {
        lv_port_sem_take();
        _build_item_row_ui(widget, group_idx, idx);
        lv_port_sem_give();
    }

    grp->item_count++;
    return idx;
}

void collapsible_list_set_item_value(collapsible_list_t *widget, uint8_t group_idx,
                                     uint8_t item_idx, const char *value) {
    if (!widget || group_idx >= widget->group_count) return;
    collapsible_list_group_t *grp = &widget->groups[group_idx];
    if (item_idx >= grp->item_count) return;

    lv_port_sem_take();
    strlcpy(grp->items[item_idx].value_text, value ? value : "--", sizeof(grp->items[item_idx].value_text));
    if (grp->items[item_idx].lbl_value) {
        lv_label_set_text(grp->items[item_idx].lbl_value, grp->items[item_idx].value_text);
    }
    lv_port_sem_give();
}

void collapsible_list_set_item_color(collapsible_list_t *widget, uint8_t group_idx,
                                     uint8_t item_idx, lv_color_t color) {
    if (!widget || group_idx >= widget->group_count) return;
    collapsible_list_group_t *grp = &widget->groups[group_idx];
    if (item_idx >= grp->item_count) return;

    lv_port_sem_take();
    grp->items[item_idx].value_color = color;
    if (grp->items[item_idx].lbl_value) {
        lv_obj_set_style_text_color(grp->items[item_idx].lbl_value, color, LV_PART_MAIN);
    }
    lv_port_sem_give();
}

void collapsible_list_set_group_expanded(collapsible_list_t *widget, uint8_t group_idx, bool expanded) {
    if (!widget || group_idx >= widget->group_count) return;
    collapsible_list_group_t *grp = &widget->groups[group_idx];

    lv_port_sem_take();
    grp->is_expanded = expanded;

    if (grp->lbl_arrow) {
        lv_label_set_text(grp->lbl_arrow, expanded ? LV_SYMBOL_DOWN : LV_SYMBOL_RIGHT);
    }
    if (grp->items_cont) {
        if (expanded) {
            lv_obj_remove_flag(grp->items_cont, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(grp->items_cont, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (widget->group_toggle_cb) {
        widget->group_toggle_cb(widget, group_idx, expanded);
    }
    lv_port_sem_give();
}

bool collapsible_list_is_group_expanded(const collapsible_list_t *widget, uint8_t group_idx) {
    if (!widget || group_idx >= widget->group_count) return false;
    return widget->groups[group_idx].is_expanded;
}