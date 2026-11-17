/**
 * @file input_dialog.c
 * @brief Universal modal touch input dialog for numeric values (float/int) and text/passwords.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "input_dialog.h"
#include "lvgl.h"
#include "gui/i18n/gui_i18n.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static const char *TAG = "input_dialog";

LV_FONT_DECLARE(ui_font_font0);

static struct {
    lv_obj_t              *overlay;
    lv_obj_t              *container;
    lv_obj_t              *ta;
    lv_obj_t              *range_label;
    lv_obj_t              *numpad;
    lv_obj_t              *btn_ok;
    lv_obj_t              *btn_cancel;
    lv_obj_t              *kb;
    input_dialog_config_t  cfg;
    bool                   is_valid;
} s_dlg = {0};

/* ── 4x4 Numpad Maps (Nur Eingabe & Editier-Tasten) ──────────────────────── */

static const char *s_numpad_map_float[] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", "+/-", "\n",
    "7", "8", "9", "C", "\n",
    ".", "0", "00", " ", ""
};

static const char *s_numpad_map_int[] = {
    "1", "2", "3", LV_SYMBOL_BACKSPACE, "\n",
    "4", "5", "6", "+/-", "\n",
    "7", "8", "9", "C", "\n",
    " ", "0", "00", " ", ""
};

static const lv_buttonmatrix_ctrl_t s_numpad_ctrl[] = {
    1, 1, 1, 1,
    1, 1, 1, 1,
    1, 1, 1, 1,
    1, 1, 1, 1
};

/* ── German QWERTZ Map for Text Keyboard ─────────────────────────────────── */

static const char *s_kb_map_de_lc[] = {
    "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "ü", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "ö", "ä", "\n",
    "ABC", "y", "x", "c", "v", "b", "n", "m", "ß", LV_SYMBOL_BACKSPACE, "\n",
    "1#", " ", LV_SYMBOL_CLOSE, LV_SYMBOL_OK, ""
};

static const lv_buttonmatrix_ctrl_t s_kb_ctrl_de_lc[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    LV_BUTTONMATRIX_CTRL_CUSTOM_1 | 2, 1, 1, 1, 1, 1, 1, 1, 1, 2,
    LV_BUTTONMATRIX_CTRL_CUSTOM_2 | 2, 6, 2, 2
};

/* ── Validation Helper ──────────────────────────────────────────────────── */

static void _update_numeric_validation(void) {
    if (s_dlg.cfg.mode != INPUT_DIALOG_MODE_FLOAT && s_dlg.cfg.mode != INPUT_DIALOG_MODE_INT) {
        return;
    }

    const char *text = lv_textarea_get_text(s_dlg.ta);
    if (!text || text[0] == '\0' || strcmp(text, "-") == 0) {
        s_dlg.is_valid = false;
        lv_label_set_text(s_dlg.range_label, "Bitte Wert eingeben");
        lv_obj_set_style_text_color(s_dlg.range_label, lv_color_hex(0xE74C3C), LV_PART_MAIN);
        if (s_dlg.btn_ok) lv_obj_add_state(s_dlg.btn_ok, LV_STATE_DISABLED);
        return;
    }

    char *endptr = NULL;
    float val = strtof(text, &endptr);
    if (endptr == text) {
        s_dlg.is_valid = false;
        if (s_dlg.btn_ok) lv_obj_add_state(s_dlg.btn_ok, LV_STATE_DISABLED);
        return;
    }

    char buf[64];
    const char *u = (s_dlg.cfg.unit && s_dlg.cfg.unit[0] != '\0') ? s_dlg.cfg.unit : "";

    if (val < s_dlg.cfg.min_val || val > s_dlg.cfg.max_val) {
        s_dlg.is_valid = false;
        if (s_dlg.cfg.mode == INPUT_DIALOG_MODE_INT) {
            snprintf(buf, sizeof(buf), "Ungültig! [%ld .. %ld %s]", 
                     (long)s_dlg.cfg.min_val, (long)s_dlg.cfg.max_val, u);
        } else {
            snprintf(buf, sizeof(buf), "Ungültig! [%.2f .. %.2f %s]", 
                     s_dlg.cfg.min_val, s_dlg.cfg.max_val, u);
        }
        lv_label_set_text(s_dlg.range_label, buf);
        lv_obj_set_style_text_color(s_dlg.range_label, lv_color_hex(0xE74C3C), LV_PART_MAIN);
        if (s_dlg.btn_ok) lv_obj_add_state(s_dlg.btn_ok, LV_STATE_DISABLED);
    } else {
        s_dlg.is_valid = true;
        if (s_dlg.cfg.mode == INPUT_DIALOG_MODE_INT) {
            snprintf(buf, sizeof(buf), "Bereich: %ld .. %ld %s", 
                     (long)s_dlg.cfg.min_val, (long)s_dlg.cfg.max_val, u);
        } else {
            snprintf(buf, sizeof(buf), "Bereich: %.2f .. %.2f %s", 
                     s_dlg.cfg.min_val, s_dlg.cfg.max_val, u);
        }
        lv_label_set_text(s_dlg.range_label, buf);
        lv_obj_set_style_text_color(s_dlg.range_label, lv_color_hex(0xBDC3C7), LV_PART_MAIN);
        if (s_dlg.btn_ok) lv_obj_remove_state(s_dlg.btn_ok, LV_STATE_DISABLED);
    }
}

/* ── Event Handlers ──────────────────────────────────────────────────────── */

static void _on_numpad_clicked(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    uint32_t btn_id = lv_buttonmatrix_get_selected_button(obj);
    if (btn_id == LV_BUTTONMATRIX_BUTTON_NONE) return;

    const char *txt = lv_buttonmatrix_get_button_text(obj, btn_id);
    if (!txt || txt[0] == '\0' || strcmp(txt, " ") == 0) return;

    if (strcmp(txt, "C") == 0) {
        lv_textarea_set_text(s_dlg.ta, "");
        _update_numeric_validation();
        return;
    }

    if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(s_dlg.ta);
        _update_numeric_validation();
        return;
    }

    if (strcmp(txt, "+/-") == 0) {
        const char *cur = lv_textarea_get_text(s_dlg.ta);
        char buf[64];
        if (cur[0] == '-') {
            snprintf(buf, sizeof(buf), "%s", cur + 1);
        } else {
            snprintf(buf, sizeof(buf), "-%s", cur);
        }
        lv_textarea_set_text(s_dlg.ta, buf);
        _update_numeric_validation();
        return;
    }

    if (strcmp(txt, ".") == 0) {
        const char *cur = lv_textarea_get_text(s_dlg.ta);
        if (strchr(cur, '.') == NULL) {
            lv_textarea_add_text(s_dlg.ta, ".");
            _update_numeric_validation();
        }
        return;
    }

    if (strcmp(txt, "00") == 0) {
        lv_textarea_add_text(s_dlg.ta, "00");
        _update_numeric_validation();
        return;
    }

    if (txt[0] >= '0' && txt[0] <= '9') {
        lv_textarea_add_text(s_dlg.ta, txt);
        _update_numeric_validation();
    }
}

static void _on_ok_action(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _update_numeric_validation();
    if (!s_dlg.is_valid) return;

    const char *val_str = lv_textarea_get_text(s_dlg.ta);
    float f_val = strtof(val_str, NULL);
    int32_t i_val = (int32_t)lroundf(f_val);

    if (s_dlg.cfg.callback) s_dlg.cfg.callback(true, f_val, i_val, val_str, s_dlg.cfg.user_data);
    input_dialog_dismiss();
}

static void _on_cancel_action(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_dlg.cfg.callback) s_dlg.cfg.callback(false, 0.0f, 0, NULL, s_dlg.cfg.user_data);
    input_dialog_dismiss();
}

static void _on_keyboard_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        const char *text = lv_textarea_get_text(s_dlg.ta);
        if (s_dlg.cfg.callback) s_dlg.cfg.callback(true, 0.0f, 0, text, s_dlg.cfg.user_data);
        input_dialog_dismiss();
    } else if (code == LV_EVENT_CANCEL) {
        if (s_dlg.cfg.callback) s_dlg.cfg.callback(false, 0.0f, 0, NULL, s_dlg.cfg.user_data);
        input_dialog_dismiss();
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void input_dialog_dismiss(void) {
    if (s_dlg.overlay) {
        lv_obj_delete(s_dlg.overlay);
        memset(&s_dlg, 0, sizeof(s_dlg));
    }
}

int input_dialog_open(const input_dialog_config_t *cfg) {
    if (!cfg) return -1;
    input_dialog_dismiss();
    s_dlg.cfg = *cfg;

    /* 1. Modal Scrim Overlay */
    s_dlg.overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_dlg.overlay, 480, 480);
    lv_obj_set_style_bg_color(s_dlg.overlay, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_dlg.overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dlg.overlay, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_dlg.overlay, LV_OBJ_FLAG_SCROLLABLE);

    /* 2. Container Card */
    s_dlg.container = lv_obj_create(s_dlg.overlay);
    lv_obj_set_size(s_dlg.container, 440, 460);
    lv_obj_center(s_dlg.container);
    lv_obj_set_style_bg_color(s_dlg.container, lv_color_hex(0x1C1C24), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_dlg.container, lv_color_hex(0x3A3A48), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dlg.container, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_dlg.container, 12, LV_PART_MAIN);
    lv_obj_remove_flag(s_dlg.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_dlg.container, 8, LV_PART_MAIN);

    /* 3. Title */
    lv_obj_t *lbl_title = lv_label_create(s_dlg.container);
    lv_label_set_text(lbl_title, cfg->title ? cfg->title : "Eingabe");
    lv_obj_set_style_text_font(lbl_title, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 2);

    /* 4. Textarea & Badge Row */
    lv_obj_t *row = lv_obj_create(s_dlg.container);
    lv_obj_set_size(row, 416, 48);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    s_dlg.ta = lv_textarea_create(row);
    lv_textarea_set_one_line(s_dlg.ta, true);
    lv_obj_set_style_text_font(s_dlg.ta, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_dlg.ta, lv_color_hex(0x2C2C38), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_dlg.ta, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_dlg.ta, lv_color_hex(0x52AAE5), LV_PART_MAIN);
    lv_obj_set_style_radius(s_dlg.ta, 6, LV_PART_MAIN);

    bool has_unit = (cfg->unit && cfg->unit[0] != '\0');
    if (has_unit) {
        lv_obj_set_size(s_dlg.ta, 326, 46);
        lv_obj_align(s_dlg.ta, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *badge = lv_obj_create(row);
        lv_obj_set_size(badge, 82, 46);
        lv_obj_align(badge, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(badge, lv_color_hex(0x2C3E50), LV_PART_MAIN);
        lv_obj_set_style_border_width(badge, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(badge, 6, LV_PART_MAIN);
        lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl_unit = lv_label_create(badge);
        lv_label_set_text(lbl_unit, cfg->unit);
        lv_obj_set_style_text_font(lbl_unit, &ui_font_font0, LV_PART_MAIN);
        lv_obj_set_style_text_color(lbl_unit, lv_color_hex(0x52AAE5), LV_PART_MAIN);
        lv_obj_center(lbl_unit);
    } else {
        lv_obj_set_size(s_dlg.ta, 416, 46);
        lv_obj_align(s_dlg.ta, LV_ALIGN_CENTER, 0, 0);
    }

    /* Initial value */
    char init_buf[64] = {0};
    if (cfg->mode == INPUT_DIALOG_MODE_FLOAT) {
        snprintf(init_buf, sizeof(init_buf), "%.2f", cfg->initial_f);
        lv_textarea_set_text(s_dlg.ta, init_buf);
    } else if (cfg->mode == INPUT_DIALOG_MODE_INT) {
        snprintf(init_buf, sizeof(init_buf), "%ld", (long)cfg->initial_i);
        lv_textarea_set_text(s_dlg.ta, init_buf);
    } else if (cfg->initial_str) {
        lv_textarea_set_text(s_dlg.ta, cfg->initial_str);
    }

    /* 5. Range / Validation Sub-Label */
    s_dlg.range_label = lv_label_create(s_dlg.container);
    lv_obj_set_style_text_font(s_dlg.range_label, &ui_font_font0, LV_PART_MAIN);
    lv_obj_align(s_dlg.range_label, LV_ALIGN_TOP_LEFT, 6, 84);

    /* 6. Mode-Specific Keypad */
    if (cfg->mode == INPUT_DIALOG_MODE_FLOAT || cfg->mode == INPUT_DIALOG_MODE_INT) {
        /* Numpad in Mausgrau */
        s_dlg.numpad = lv_buttonmatrix_create(s_dlg.container);
        lv_obj_set_size(s_dlg.numpad, 416, 250);
        lv_obj_align(s_dlg.numpad, LV_ALIGN_TOP_MID, 0, 110);
        lv_obj_set_style_text_font(s_dlg.numpad, &ui_font_font0, LV_PART_ITEMS);

        if (cfg->mode == INPUT_DIALOG_MODE_FLOAT) {
            lv_buttonmatrix_set_map(s_dlg.numpad, s_numpad_map_float);
        } else {
            lv_buttonmatrix_set_map(s_dlg.numpad, s_numpad_map_int);
            lv_buttonmatrix_set_button_ctrl(s_dlg.numpad, 12, LV_BUTTONMATRIX_CTRL_DISABLED | LV_BUTTONMATRIX_CTRL_HIDDEN);
        }
        lv_buttonmatrix_set_ctrl_map(s_dlg.numpad, s_numpad_ctrl);

        /* Keypad-Farbe: Mausgrau (0x3A3A48) */
        lv_obj_set_style_bg_color(s_dlg.numpad, lv_color_hex(0x2C2C38), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_dlg.numpad, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_dlg.numpad, lv_color_hex(0x3A3A48), LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_dlg.numpad, lv_color_hex(0xFFFFFF), LV_PART_ITEMS | LV_STATE_DEFAULT);
        lv_obj_add_event_cb(s_dlg.numpad, _on_numpad_clicked, LV_EVENT_CLICKED, NULL);

        /* 7. Action Footer: Abbrechen (Rot) & OK (Grün) */
        lv_obj_t *action_row = lv_obj_create(s_dlg.container);
        lv_obj_set_size(action_row, 416, 50);
        lv_obj_align(action_row, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_obj_set_style_bg_opa(action_row, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(action_row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(action_row, 0, LV_PART_MAIN);
        lv_obj_remove_flag(action_row, LV_OBJ_FLAG_SCROLLABLE);

        /* Cancel Button (Rot) */
        s_dlg.btn_cancel = lv_button_create(action_row);
        lv_obj_set_size(s_dlg.btn_cancel, 198, 46);
        lv_obj_align(s_dlg.btn_cancel, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(s_dlg.btn_cancel, lv_color_hex(0xC0392B), LV_PART_MAIN);
        lv_obj_set_style_radius(s_dlg.btn_cancel, 6, LV_PART_MAIN);
        lv_obj_add_event_cb(s_dlg.btn_cancel, _on_cancel_action, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl_c = lv_label_create(s_dlg.btn_cancel);
        const char *cancel_txt = gui_i18n_get("common.cancel");
        if (strcmp(cancel_txt, "common.cancel") == 0) cancel_txt = "Abbrechen";
        lv_label_set_text(lbl_c, cancel_txt);
        lv_obj_set_style_text_font(lbl_c, &ui_font_font0, LV_PART_MAIN);
        lv_obj_center(lbl_c);

        /* OK Button (Grün) */
        s_dlg.btn_ok = lv_button_create(action_row);
        lv_obj_set_size(s_dlg.btn_ok, 198, 46);
        lv_obj_align(s_dlg.btn_ok, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(s_dlg.btn_ok, lv_color_hex(0x27AE60), LV_PART_MAIN);
        lv_obj_set_style_radius(s_dlg.btn_ok, 6, LV_PART_MAIN);
        lv_obj_add_event_cb(s_dlg.btn_ok, _on_ok_action, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl_ok = lv_label_create(s_dlg.btn_ok);
        const char *ok_txt = gui_i18n_get("common.ok");
        if (strcmp(ok_txt, "common.ok") == 0) ok_txt = "OK";
        lv_label_set_text(lbl_ok, ok_txt);
        lv_obj_set_style_text_font(lbl_ok, &ui_font_font0, LV_PART_MAIN);
        lv_obj_center(lbl_ok);

        _update_numeric_validation();

    } else {
        /* Text- / Passwort-Modus */
        if (cfg->mode == INPUT_DIALOG_MODE_PASSWORD) {
            lv_textarea_set_password_mode(s_dlg.ta, true);
        }
        lv_obj_add_flag(s_dlg.range_label, LV_OBJ_FLAG_HIDDEN);

        s_dlg.kb = lv_keyboard_create(s_dlg.container);
        lv_obj_set_size(s_dlg.kb, 416, 290);
        lv_obj_align(s_dlg.kb, LV_ALIGN_BOTTOM_MID, 0, -4);
        lv_keyboard_set_textarea(s_dlg.kb, s_dlg.ta);
        lv_keyboard_set_popovers(s_dlg.kb, true);
        lv_obj_set_style_text_font(s_dlg.kb, &ui_font_font0, LV_PART_ITEMS);

        if (gui_i18n_get_language() == GUI_LANG_DE) {
            lv_keyboard_set_map(s_dlg.kb, LV_KEYBOARD_MODE_TEXT_LOWER, s_kb_map_de_lc, s_kb_ctrl_de_lc);
        }

        lv_obj_add_event_cb(s_dlg.kb, _on_keyboard_event, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(s_dlg.kb, _on_keyboard_event, LV_EVENT_CANCEL, NULL);
    }

    ESP_LOGI(TAG, "Input dialog opened: '%s' (mode=%d)", cfg->title ? cfg->title : "", cfg->mode);
    return 0;
}