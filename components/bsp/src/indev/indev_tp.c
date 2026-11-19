/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "bsp_i2c.h"
#include "bsp_board.h"
#include "indev_tp.h"
#include "esp_log.h"
#include "esp_err.h"

#include "ft5x06.h"
#include "tt21100.h"
#include "gt1151.h"
#include "gt911.h"
#include "xpt2046.h"

static const char *TAG = "indev_tp";
typedef enum {
    TP_VENDOR_NONE = -1,
    TP_VENDOR_FT = 0,
    TP_VENDOR_TT,
    TP_VENDOR_GOODIX,
    TP_VENDOR_MAX,
    TP_VENDOR_XPT,
} tp_vendor_t;

typedef struct {
    char *dev_name;
    uint8_t dev_addr;
    tp_vendor_t dev_vendor;
} tp_dev_t;

static tp_dev_t tp_dev_list[] = {
#if CONFIG_SENSECAP_INDICATOR_SCREEN_GX
    { "Focal Tech", 0x48, TP_VENDOR_FT },  //FT6336U
#else
    { "Focal Tech", 0x38, TP_VENDOR_FT },
#endif
    { "Parade Tech", 0x24, TP_VENDOR_TT },
    { "Goodix Tech GT1151", 0x14, TP_VENDOR_GOODIX },
    { "Goodix Tech GT911", 0x5D, TP_VENDOR_GOODIX }, // The GT911 address can also be set to 0x14, so this is not very appropriate
#ifdef CONFIG_LCD_EV_SUB_BOARD1_LCD_320x240
    { "XPT2046", 0xff, TP_VENDOR_XPT },
#endif
};

static int tp_dev_id = -1;

static esp_err_t tp_prob(int *tp_dev_id)
{
    for (size_t i = 0; i < sizeof(tp_dev_list) / sizeof(tp_dev_list[0]); i++) {
        if (tp_dev_list[i].dev_addr != 0xff) {
            if (ESP_OK == bsp_i2c_probe_addr(tp_dev_list[i].dev_addr)) {
                *tp_dev_id = tp_dev_list[i].dev_vendor;
                ESP_LOGI(TAG, "Detected touch panel at 0x%02X. Vendor : %s",
                        tp_dev_list[i].dev_addr, tp_dev_list[i].dev_name);
                return ESP_OK;
            }
        }
        else {
            *tp_dev_id = tp_dev_list[i].dev_vendor;
            ESP_LOGI(TAG, "Detected touch panel at 0x%02X. Vendor : %s",
                    tp_dev_list[i].dev_addr, tp_dev_list[i].dev_name);
            return ESP_OK;
        }
    }

    *tp_dev_id = -1;
    ESP_LOGW(TAG, "Touch panel not detected");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t indev_tp_init(void)
{
    esp_err_t ret_val = ESP_OK;

#if CONFIG_SENSECAP_INDICATOR_SCREEN_GX || CONFIG_SENSECAP_INDICATOR_SCREEN_DX
    tp_dev_id = TP_VENDOR_FT;
#endif

    if (tp_dev_id < 0) {
        ret_val |= tp_prob(&tp_dev_id);
    }

    switch (tp_dev_list[tp_dev_id].dev_vendor) {
    case TP_VENDOR_TT:
        ret_val |= tt21100_tp_init();
        break;
    case TP_VENDOR_FT:
        ret_val |= ft5x06_init();
        break;
    case TP_VENDOR_GOODIX:
        ret_val = gt1151_init(tp_dev_list[tp_dev_id].dev_addr);
        if (ESP_OK == ret_val) {
            tp_dev_id = 2;
            break;
        }
        ret_val = gt911_init(tp_dev_list[tp_dev_id].dev_addr);
        if (ESP_OK == ret_val) {
            tp_dev_id = 3;
            break;
        }
        break;
    case TP_VENDOR_XPT:
        ret_val |= xpt2046_init();
        xpt2046_calibration_run(true);
        break;
    default:
        break;
    }

    return ret_val;
}

esp_err_t indev_tp_get_dev(char **dev_name, uint8_t *dev_addr)
{
    if (tp_dev_id >= 0) {
        *dev_name = tp_dev_list[tp_dev_id].dev_name;
        *dev_addr = tp_dev_list[tp_dev_id].dev_addr;
        return ESP_OK;
    }

    return ESP_FAIL;
}

esp_err_t indev_tp_read(uint8_t *tp_num, uint16_t *x, uint16_t *y, uint8_t *btn_val)
{
    if (tp_dev_id < 0 || !tp_num || !x || !y || !btn_val) {
        return ESP_FAIL;
    }
    *tp_num = 0; /* Default: no touch detected */
    esp_err_t ret_val = ESP_OK;
    uint16_t btn_signal = 0;
    touch_panel_points_t pos;

    switch (tp_dev_list[tp_dev_id].dev_vendor) {
    case TP_VENDOR_TT:
        do {
            ret_val |= tt21100_tp_read();
        } while (tt21100_data_avaliable());

        ret_val |= tt21100_get_touch_point(tp_num, x, y);
        ret_val |= tt21100_get_btn_val(btn_val, &btn_signal);
        break;
    case TP_VENDOR_FT:
        ret_val |= ft5x06_read_pos(tp_num, x, y);
        break;
    case TP_VENDOR_GOODIX:
        if (0x14 == tp_dev_list[tp_dev_id].dev_addr) {
            *tp_num = gt1151_pos_read(x, y);
        } else if (0x5D == tp_dev_list[tp_dev_id].dev_addr) {
            *tp_num = gt911_pos_read(x, y);
        }
        ret_val = ESP_OK;
        break;
    case TP_VENDOR_XPT:
        xpt2046_sample(&pos);
        *tp_num = pos.point_num;
        *x = pos.curx[0];
        *y = pos.cury[0];
        ret_val = ESP_OK;
        break;
    default:
        return ESP_ERR_NOT_FOUND;
        break;
    }

    /* A failed read carries no valid point: do not feed it into the debounce filter */
    if (ret_val != ESP_OK) {
        *tp_num = 0;
        return ret_val;
    }

    const board_res_desc_t *brd = bsp_board_get_description();
    if (brd->TOUCH_PANEL_SWAP_XY) {
        uint16_t swap = *x;
        *x = *y;
        *y = swap;
    }

    /* Guard against coordinate underflow before axis inversion */
    if (*x >= brd->LCD_WIDTH) {
        *x = brd->LCD_WIDTH - 1;
    }
    if (*y >= brd->LCD_HEIGHT) {
        *y = brd->LCD_HEIGHT - 1;
    }

    if (brd->TOUCH_PANEL_INVERSE_X) {
        *x = brd->LCD_WIDTH - (*x + 1);
    }

    if (brd->TOUCH_PANEL_INVERSE_Y) {
        *y = brd->LCD_HEIGHT - (*y + 1);
    }

    /* ── Temporal Debounce & Capacitive Ghost-Touch Filter ──────────────── */
    #define TP_PRESS_CONFIRM_SAMPLES   3   /* Require 3 consecutive hits (~30-45 ms) to confirm press */
    #define TP_RELEASE_CONFIRM_SAMPLES 2   /* Require 2 consecutive idle reads to confirm release */
    #define TP_MAX_JUMP_PX             60  /* Reject erratic single-frame coordinate jumps */

    static uint8_t  s_press_streak   = 0;
    static uint8_t  s_release_streak = 0;
    static bool     s_is_pressed     = false;
    static uint16_t s_cand_x         = 0;
    static uint16_t s_cand_y         = 0;
    static uint16_t s_stable_x       = 0;
    static uint16_t s_stable_y       = 0;

    if (*tp_num > 0) {
        s_release_streak = 0;

        if (!s_is_pressed) {
            if (s_press_streak == 0) {
                s_cand_x = *x;
                s_cand_y = *y;
                s_press_streak = 1;
            } else {
                int32_t dx = (int32_t)*x - (int32_t)s_cand_x;
                int32_t dy = (int32_t)*y - (int32_t)s_cand_y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;

                if (dx <= TP_MAX_JUMP_PX && dy <= TP_MAX_JUMP_PX) {
                    s_cand_x = *x;
                    s_cand_y = *y;
                    s_press_streak++;
                    if (s_press_streak >= TP_PRESS_CONFIRM_SAMPLES) {
                        s_is_pressed = true;
                        s_stable_x = *x;
                        s_stable_y = *y;
                    }
                } else {
                    /* Unstable spike: restart confirmation from new point */
                    s_cand_x = *x;
                    s_cand_y = *y;
                    s_press_streak = 1;
                }
            }
        } else {
            /* Active press: filter out single-frame coordinate glitches */
            int32_t dx = (int32_t)*x - (int32_t)s_stable_x;
            int32_t dy = (int32_t)*y - (int32_t)s_stable_y;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;

            if (dx <= TP_MAX_JUMP_PX && dy <= TP_MAX_JUMP_PX) {
                s_stable_x = *x;
                s_stable_y = *y;
            }
        }
    } else {
        if (s_press_streak > 0 && !s_is_pressed) {
            ESP_LOGD(TAG, "Suppressed transient ghost touch (%u samples) at [%3u, %3u]",
                     s_press_streak, s_cand_x, s_cand_y);
        }
        s_press_streak = 0;

        if (s_is_pressed) {
            s_release_streak++;
            if (s_release_streak >= TP_RELEASE_CONFIRM_SAMPLES) {
                s_is_pressed = false;
                s_release_streak = 0;
            }
        }
    }

    if (s_is_pressed) {
        *tp_num = 1;
        *x = s_stable_x;
        *y = s_stable_y;
    } else {
        *tp_num = 0;
    }

    ESP_LOGV(TAG, "[%3u, %3u] (pressed=%d)", *x, *y, s_is_pressed ? 1 : 0);

    return ret_val;
}
