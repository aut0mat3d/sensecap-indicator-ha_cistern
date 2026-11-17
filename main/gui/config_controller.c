/**
 * @file config_controller.c
 * @brief Implementation of the schema-driven configuration controller.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "config_controller.h"
#include "lv_port.h"
#include "gui/i18n/gui_i18n.h"
#include "input_dialog.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

LV_FONT_DECLARE(ui_font_font0);

static const char *TAG = "config_ctrl";

/* Dynamic pointers allocated in PSRAM to conserve internal DRAM */
static lv_obj_t                 *s_modal_root = NULL;
static collapsible_list_t        *s_col_list = NULL;
static config_controller_desc_t  s_active_desc;
static uint32_t                  s_active_dev_addr = 0;

static config_group_data_t       *s_draft_groups = NULL;
static size_t                    s_draft_group_count = 0;

/* Context helper passed to input dialog callback */
typedef struct {
    size_t group_idx;
    size_t field_idx;
} config_edit_ctx_t;

static config_edit_ctx_t s_edit_ctx;

static void _on_input_dialog_finished(bool confirmed, float f_val, int32_t i_val, const char *str_val, void *user_data) {
    if (!confirmed || !s_draft_groups) return;

    config_edit_ctx_t *ctx = (config_edit_ctx_t *)user_data;
    if (!ctx || ctx->group_idx >= s_draft_group_count) return;

    /* Updates the draft buffer, formats text, and updates the collapsible list item */
    config_controller_update_draft_value((uint8_t)ctx->group_idx, (uint8_t)ctx->field_idx, f_val, i_val, NULL);
}

/* Forward declarations */
static void _populate_ui_from_draft(void);

/* ── UI Helpers & Formatting ────────────────────────────────────────────── */

static void _format_field_value_text(const config_field_value_t *val, const config_field_desc_t *desc,
                                     char *out_buf, size_t out_size, lv_color_t *out_color) {
    if (!val || !out_buf || out_size == 0) return;

    *out_color = lv_color_hex(0x52AAE5); /* Default accent blue */

    switch (val->type) {
        case CONFIG_FIELD_FLOAT: {
            const char *u = (val->unit[0] != '\0') ? val->unit : (desc && desc->default_unit ? desc->default_unit : "");
            if (u[0] != '\0') {
                snprintf(out_buf, out_size, "%.2f %s", val->f_val, u);
            } else {
                snprintf(out_buf, out_size, "%.2f", val->f_val);
            }
            break;
        }
        case CONFIG_FIELD_INT: {
            const char *u = (desc && desc->default_unit) ? desc->default_unit : "";
            if (u[0] != '\0') {
                const char *i18n_u = gui_i18n_get(u);
                snprintf(out_buf, out_size, "%ld %s", (long)val->i_val, (i18n_u && strcmp(i18n_u, u) != 0) ? i18n_u : u);
            } else {
                snprintf(out_buf, out_size, "%ld", (long)val->i_val);
            }
            break;
        }
        case CONFIG_FIELD_ENUM: {
            snprintf(out_buf, out_size, "%s", val->enum_val);
            if (strcasecmp(val->enum_val, "danger") == 0)       *out_color = lv_color_hex(0xE74C3C);
            else if (strcasecmp(val->enum_val, "warning") == 0) *out_color = lv_color_hex(0xF39C12);
            else if (strcasecmp(val->enum_val, "info") == 0)    *out_color = lv_color_hex(0x9E9E9E);
            break;
        }
    }
}

/* ── Callbacks ──────────────────────────────────────────────────────────── */

static void _on_item_clicked_cb(collapsible_list_t *list, uint8_t group_idx, uint8_t item_idx, void *user_data) {
    if (!s_draft_groups || group_idx >= s_draft_group_count) return;
    config_group_data_t *grp = &s_draft_groups[group_idx];
    if (item_idx >= grp->field_count) return;

    config_field_value_t *fld = &grp->fields[item_idx];
    const config_field_desc_t *desc = (item_idx < s_active_desc.field_desc_count) 
                                      ? &s_active_desc.field_descriptors[item_idx] : NULL;

    ESP_LOGI(TAG, "Item clicked: Group '%s', Field '%s' (Type: %d)",
             grp->group_id, fld->field_id, fld->type);

    /* Open Numeric Numpad Dialog for Float and Int fields */
    if (fld->type == CONFIG_FIELD_FLOAT || fld->type == CONFIG_FIELD_INT) {
        s_edit_ctx.group_idx = group_idx;
        s_edit_ctx.field_idx = item_idx;

        /* Resolve title and localized unit (e.g. translate "alarm_cfg.cycles" -> "Zyklen") */
        char dlg_title[96];
        const char *prop_name = desc ? gui_i18n_get(desc->i18n_label_key) : fld->field_id;
        snprintf(dlg_title, sizeof(dlg_title), "%s: %s", grp->title, prop_name);

        const char *resolved_unit = fld->unit;
        if (resolved_unit[0] != '\0') {
            resolved_unit = gui_i18n_get(resolved_unit);
        } else if (desc && desc->default_unit) {
            resolved_unit = gui_i18n_get(desc->default_unit);
        }

        input_dialog_config_t id_cfg = {
            .title = dlg_title,
            .mode = (fld->type == CONFIG_FIELD_FLOAT) ? INPUT_DIALOG_MODE_FLOAT : INPUT_DIALOG_MODE_INT,
            .initial_f = fld->f_val,
            .initial_i = fld->i_val,
            .unit = resolved_unit,
            .min_val = desc ? desc->min_val : -999.0f,
            .max_val = desc ? desc->max_val : 9999.0f,
            .callback = _on_input_dialog_finished,
            .user_data = &s_edit_ctx
        };
        input_dialog_open(&id_cfg);
        return;
    }

    /* Hook point for discrete enum selection */
    if (fld->type == CONFIG_FIELD_ENUM && desc && desc->enum_count > 1) {
        for (uint8_t e = 0; e < desc->enum_count; e++) {
            if (strcasecmp(fld->enum_val, desc->enum_options[e]) == 0) {
                uint8_t next = (e + 1) % desc->enum_count;
                strlcpy(fld->enum_val, desc->enum_options[next], sizeof(fld->enum_val));
                break;
            }
        }
        char disp_buf[32];
        lv_color_t col;
        _format_field_value_text(fld, desc, disp_buf, sizeof(disp_buf), &col);
        collapsible_list_set_item_value(s_col_list, group_idx, item_idx, disp_buf);
        collapsible_list_set_item_color(s_col_list, group_idx, item_idx, col);
    }
}

static void _on_revert_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Reverting configuration draft to saved values...");
    if (s_active_desc.connector.load_fn && s_draft_groups) {
        s_draft_group_count = 0;
        s_active_desc.connector.load_fn(s_active_dev_addr, s_draft_groups, CONFIG_MAX_GROUPS, &s_draft_group_count);
        _populate_ui_from_draft();
    }
}

static void _on_exit_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Exiting without saving changes");
    config_controller_close();
}

static void _on_save_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    ESP_LOGI(TAG, "Committing configuration: Applying runtime & persisting to SD...");

    /* 1. Apply to live ESP32 runtime */
    if (s_active_desc.connector.apply_runtime_fn && s_draft_groups) {
        s_active_desc.connector.apply_runtime_fn(s_active_dev_addr, s_draft_groups, s_draft_group_count);
    }

    /* 2. Persist to SD card via RP2040 UART */
    if (s_active_desc.connector.persist_fn && s_draft_groups) {
        s_active_desc.connector.persist_fn(s_active_dev_addr, s_draft_groups, s_draft_group_count);
    }

    config_controller_close();
}

/* ── UI Population ──────────────────────────────────────────────────────── */

static void _populate_ui_from_draft(void) {
    if (!s_col_list || !s_draft_groups) return;
    collapsible_list_clear(s_col_list);

    for (uint8_t g = 0; g < s_draft_group_count; g++) {
        config_group_data_t *grp = &s_draft_groups[g];
        int g_idx = collapsible_list_add_group(s_col_list, grp->group_id, grp->title, false);
        if (g_idx < 0) continue;

        for (uint8_t f = 0; f < grp->field_count; f++) {
            config_field_value_t *val = &grp->fields[f];
            const config_field_desc_t *desc = (f < s_active_desc.field_desc_count) 
                                              ? &s_active_desc.field_descriptors[f] : NULL;

            const char *label_text = desc ? gui_i18n_get(desc->i18n_label_key) : val->field_id;
            if (label_text && desc && strcmp(label_text, desc->i18n_label_key) == 0) {
                label_text = val->field_id;
            }

            char val_str[32];
            lv_color_t val_col;
            _format_field_value_text(val, desc, val_str, sizeof(val_str), &val_col);

            collapsible_list_add_item(s_col_list, (uint8_t)g_idx, val->field_id, label_text, val_str, val_col, NULL);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int config_controller_open(const config_controller_desc_t *desc, uint32_t dev_addr) {
    if (!desc || s_modal_root) return -1;

    s_active_desc = *desc;
    s_active_dev_addr = dev_addr;

    /* 1. Allocate working buffers in PSRAM (conserves ~13.5 KB internal DRAM) */
    s_draft_groups = (config_group_data_t *)heap_caps_calloc(CONFIG_MAX_GROUPS, 
                                                             sizeof(config_group_data_t), 
                                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_draft_groups) {
        s_draft_groups = (config_group_data_t *)calloc(CONFIG_MAX_GROUPS, sizeof(config_group_data_t));
    }
    if (!s_draft_groups) {
        ESP_LOGE(TAG, "Failed to allocate draft groups memory");
        return -1;
    }

    s_col_list = (collapsible_list_t *)heap_caps_calloc(1, 
                                                        sizeof(collapsible_list_t), 
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_col_list) {
        s_col_list = (collapsible_list_t *)calloc(1, sizeof(collapsible_list_t));
    }
    if (!s_col_list) {
        free(s_draft_groups);
        s_draft_groups = NULL;
        ESP_LOGE(TAG, "Failed to allocate collapsible list structure");
        return -1;
    }

    /* 2. Load initial data into draft buffer */
    s_draft_group_count = 0;
    if (s_active_desc.connector.load_fn) {
        int ret = s_active_desc.connector.load_fn(dev_addr, s_draft_groups, CONFIG_MAX_GROUPS, &s_draft_group_count);
        if (ret < 0) {
            ESP_LOGE(TAG, "Connector load_fn failed for dev 0x%08lX", (unsigned long)dev_addr);
            free(s_col_list);
            s_col_list = NULL;
            free(s_draft_groups);
            s_draft_groups = NULL;
            return -1;
        }
    }

    lv_port_sem_take();

    /* 3. Create Modal Fullscreen Root on lv_layer_top() */
    s_modal_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal_root, 480, 480);
    lv_obj_set_pos(s_modal_root, 0, 0);
    lv_obj_remove_flag(s_modal_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_modal_root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_modal_root, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_modal_root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_modal_root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_modal_root, 0, LV_PART_MAIN);

    /* 4. Header Bar (0..50 px) */
    lv_obj_t *header = lv_obj_create(s_modal_root);
    lv_obj_set_size(header, 480, 50);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x161B20), LV_PART_MAIN);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(header, lv_color_hex(0x2C3E50), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(header, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(header, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 10, LV_PART_MAIN);

    /* Back button in header */
    lv_obj_t *btn_back = lv_button_create(header);
    lv_obj_set_size(btn_back, 75, 36);
    lv_obj_set_style_radius(btn_back, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2C3E50), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_back, _on_exit_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_back);

    /* Title label */
    lv_obj_t *lbl_title = lv_label_create(header);
    lv_label_set_text(lbl_title, desc->title ? desc->title : gui_i18n_get("alarm_cfg.title"));
    lv_obj_set_style_text_color(lbl_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_title, &ui_font_font0, LV_PART_MAIN);

    /* 5. Center: Collapsible List Container (y = 54, h = 368 px) */
    memset(s_col_list, 0, sizeof(collapsible_list_t));
    s_col_list->parent = s_modal_root;
    s_col_list->x = 8;
    s_col_list->y = 54;
    s_col_list->width = 464;
    s_col_list->height = 368;
    s_col_list->item_click_cb = _on_item_clicked_cb;
    collapsible_list_init(s_col_list);

    /* Populate list with data */
    _populate_ui_from_draft();

    /* 6. Footer Bar with 3 Action Buttons (y = 426, h = 54 px) */
    lv_obj_t *footer = lv_obj_create(s_modal_root);
    lv_obj_set_size(footer, 480, 54);
    lv_obj_set_pos(footer, 0, 426);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(footer, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0x161B20), LV_PART_MAIN);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_border_width(footer, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(footer, lv_color_hex(0x2C3E50), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(footer, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(footer, 6, LV_PART_MAIN);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Button 1: Revert Changes */
    lv_obj_t *btn_revert = lv_button_create(footer);
    lv_obj_set_size(btn_revert, 138, 40);
    lv_obj_set_style_radius(btn_revert, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_revert, lv_color_hex(0x7F8C8D), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_revert, _on_revert_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_rev = lv_label_create(btn_revert);
    lv_label_set_text(lbl_rev, gui_i18n_get("alarm_cfg.revert"));
    lv_obj_set_style_text_font(lbl_rev, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(lbl_rev);

    /* Button 2: Exit (No Save) */
    lv_obj_t *btn_exit = lv_button_create(footer);
    lv_obj_set_size(btn_exit, 138, 40);
    lv_obj_set_style_radius(btn_exit, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_exit, lv_color_hex(0xC0392B), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_exit, _on_exit_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ex = lv_label_create(btn_exit);
    lv_label_set_text(lbl_ex, gui_i18n_get("alarm_cfg.exit_no_save"));
    lv_obj_set_style_text_font(lbl_ex, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(lbl_ex);

    /* Button 3: Save & Exit */
    lv_obj_t *btn_save = lv_button_create(footer);
    lv_obj_set_size(btn_save, 146, 40);
    lv_obj_set_style_radius(btn_save, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x27AE60), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_save, _on_save_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_sv = lv_label_create(btn_save);
    lv_label_set_text(lbl_sv, gui_i18n_get("alarm_cfg.save_exit"));
    lv_obj_set_style_text_font(lbl_sv, &ui_font_font0, LV_PART_MAIN);
    lv_obj_center(lbl_sv);

    lv_port_sem_give();
    ESP_LOGI(TAG, "Config controller modal opened for device 0x%08lX", (unsigned long)dev_addr);
    return 0;
}

void config_controller_close(void) {
    if (!s_modal_root) return;

    lv_port_sem_take();

    if (s_col_list) {
        collapsible_list_destroy(s_col_list);
        free(s_col_list);
        s_col_list = NULL;
    }

    if (s_draft_groups) {
        free(s_draft_groups);
        s_draft_groups = NULL;
    }

    lv_obj_delete(s_modal_root);
    s_modal_root = NULL;

    s_draft_group_count = 0;
    s_active_dev_addr = 0;

    lv_port_sem_give();
    ESP_LOGI(TAG, "Config controller modal closed and memory freed");
}

void config_controller_update_draft_value(uint8_t group_idx, uint8_t field_idx,
                                          float new_f_val, int32_t new_i_val, const char *new_enum_val) {
    if (!s_draft_groups || group_idx >= s_draft_group_count) return;
    config_group_data_t *grp = &s_draft_groups[group_idx];
    if (field_idx >= grp->field_count) return;

    config_field_value_t *fld = &grp->fields[field_idx];
    const config_field_desc_t *desc = (field_idx < s_active_desc.field_desc_count) 
                                      ? &s_active_desc.field_descriptors[field_idx] : NULL;

    switch (fld->type) {
        case CONFIG_FIELD_FLOAT: fld->f_val = new_f_val; break;
        case CONFIG_FIELD_INT:   fld->i_val = new_i_val; break;
        case CONFIG_FIELD_ENUM:  if (new_enum_val) strlcpy(fld->enum_val, new_enum_val, sizeof(fld->enum_val)); break;
    }

    char disp_buf[32];
    lv_color_t col;
    _format_field_value_text(fld, desc, disp_buf, sizeof(disp_buf), &col);

    if (s_col_list) {
        collapsible_list_set_item_value(s_col_list, group_idx, field_idx, disp_buf);
        collapsible_list_set_item_color(s_col_list, group_idx, field_idx, col);
    }
}