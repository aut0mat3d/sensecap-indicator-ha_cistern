/**
 * @file gui_i18n.c
 * @brief Internationalization engine implementation with cascading resolution and cJSON flattening.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "gui_i18n.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "gui-i18n";

extern const i18n_entry_t g_i18n_builtin_en[];
extern const i18n_entry_t g_i18n_builtin_de[];

typedef struct dynamic_entry_s {
    char *key;
    char *val;
    struct dynamic_entry_s *next;
} dynamic_entry_t;

static gui_lang_t s_current_lang = GUI_LANG_DE;
static dynamic_entry_t *s_sd_overrides_head = NULL;
static SemaphoreHandle_t s_i18n_mutex = NULL;

static const char* _search_static_table(const i18n_entry_t *table, const char *key) {
    if (!table || !key) return NULL;
    for (size_t i = 0; table[i].key != NULL; i++) {
        if (strcmp(table[i].key, key) == 0) {
            return table[i].val;
        }
    }
    return NULL;
}

static const char* _search_dynamic_table(const char *key) {
    dynamic_entry_t *curr = s_sd_overrides_head;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            return curr->val;
        }
        curr = curr->next;
    }
    return NULL;
}

static void _insert_dynamic_entry(const char *key, const char *val) {
    dynamic_entry_t *curr = s_sd_overrides_head;
    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            free(curr->val);
            curr->val = strdup(val);
            return;
        }
        curr = curr->next;
    }

    dynamic_entry_t *new_node = (dynamic_entry_t *)malloc(sizeof(dynamic_entry_t));
    if (!new_node) return;

    new_node->key = strdup(key);
    new_node->val = strdup(val);
    new_node->next = s_sd_overrides_head;
    s_sd_overrides_head = new_node;
}

static void _flatten_json_object(cJSON *item, const char *prefix, int *count) {
    if (!item) return;

    cJSON *child = item->child;
    while (child) {
        char key_buf[128];
        if (prefix && prefix[0] != '\0') {
            snprintf(key_buf, sizeof(key_buf), "%s.%s", prefix, child->string);
        } else {
            snprintf(key_buf, sizeof(key_buf), "%s", child->string);
        }

        if (cJSON_IsObject(child)) {
            _flatten_json_object(child, key_buf, count);
        } else if (cJSON_IsString(child) && child->valuestring) {
            _insert_dynamic_entry(key_buf, child->valuestring);
            if (count) (*count)++;
        }
        child = child->next;
    }
}

void gui_i18n_init(void) {
    if (!s_i18n_mutex) {
        s_i18n_mutex = xSemaphoreCreateMutex();
    }
}

/* External UART file reader from main/rp2040/rp2040.c */
extern int rp2040_read_file_from_sd(const char *path, char *out_buf, size_t max_len, uint32_t timeout_ms);

int gui_i18n_reload_from_sd(const char *lang_code) {
    if (!lang_code || lang_code[0] == '\0') return 0;

    /* 1. Map target language enum */
    if (strcasecmp(lang_code, "de") == 0) {
        gui_i18n_set_language(GUI_LANG_DE);
    } else if (strcasecmp(lang_code, "en") == 0) {
        gui_i18n_set_language(GUI_LANG_EN);
    } else {
        gui_i18n_set_language(GUI_LANG_CUSTOM);
    }

    /* 2. Purge previous dynamic RAM entries */
    gui_i18n_clear_sd_overrides();

    /* 3. Allocate temporary buffer in PSRAM/heap */
    const size_t buf_sz = 4096;
    char *json_buf = (char *)malloc(buf_sz);
    if (!json_buf) {
        ESP_LOGE(TAG, "Failed to allocate buffer for i18n file reload");
        return 0;
    }

    int total_keys = 0;
    char path[128];

    /* 4. Request /gui/lang/<lang>/common.json */
    snprintf(path, sizeof(path), "/gui/lang/%s/common.json", lang_code);
    int len = rp2040_read_file_from_sd(path, json_buf, buf_sz, 1500);
    if (len > 0) {
        int keys = gui_i18n_load_sd_overrides(json_buf);
        if (keys > 0) total_keys += keys;
        ESP_LOGI(TAG, "Loaded %s (%d bytes, %d keys)", path, len, keys);
    } else {
        ESP_LOGD(TAG, "%s not found on SD card", path);
    }

    /* 5. Request /gui/lang/<lang>/alarm_config.json */
    snprintf(path, sizeof(path), "/gui/lang/%s/alarm_config.json", lang_code);
    len = rp2040_read_file_from_sd(path, json_buf, buf_sz, 1500);
    if (len > 0) {
        int keys = gui_i18n_load_sd_overrides(json_buf);
        if (keys > 0) total_keys += keys;
        ESP_LOGI(TAG, "Loaded %s (%d bytes, %d keys)", path, len, keys);
    } else {
        ESP_LOGD(TAG, "%s not found on SD card", path);
    }

    free(json_buf);
    ESP_LOGI(TAG, "i18n reload complete for '%s': %d dynamic overrides active", lang_code, total_keys);
    return total_keys;
}

void gui_i18n_set_language(gui_lang_t lang) {
    if (s_i18n_mutex) xSemaphoreTake(s_i18n_mutex, portMAX_DELAY);
    s_current_lang = lang;
    if (s_i18n_mutex) xSemaphoreGive(s_i18n_mutex);
}

gui_lang_t gui_i18n_get_language(void) {
    return s_current_lang;
}

const char* gui_i18n_get(const char *key) {
    if (!key) return "";

    if (s_i18n_mutex) xSemaphoreTake(s_i18n_mutex, portMAX_DELAY);

    /* 1. Check SD-Card RAM overrides */
    const char *res = _search_dynamic_table(key);

    /* 2. Check active built-in table */
    if (!res) {
        if (s_current_lang == GUI_LANG_DE) {
            res = _search_static_table(g_i18n_builtin_de, key);
        } else if (s_current_lang == GUI_LANG_EN) {
            res = _search_static_table(g_i18n_builtin_en, key);
        }
    }

    /* 3. Fallback to English base table */
    if (!res) {
        res = _search_static_table(g_i18n_builtin_en, key);
    }

    if (s_i18n_mutex) xSemaphoreGive(s_i18n_mutex);

    /* 4. Not found: return raw key so the UI never crashes or displays blank text */
    return res ? res : key;
}

int gui_i18n_load_sd_overrides(const char *json_str) {
    if (!json_str) return -1;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse i18n JSON override string");
        return -1;
    }

    if (s_i18n_mutex) xSemaphoreTake(s_i18n_mutex, portMAX_DELAY);
    int imported_count = 0;
    _flatten_json_object(root, "", &imported_count);
    if (s_i18n_mutex) xSemaphoreGive(s_i18n_mutex);

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Imported %d i18n keys from SD-Card JSON", imported_count);
    return imported_count;
}

void gui_i18n_clear_sd_overrides(void) {
    if (s_i18n_mutex) xSemaphoreTake(s_i18n_mutex, portMAX_DELAY);

    dynamic_entry_t *curr = s_sd_overrides_head;
    while (curr) {
        dynamic_entry_t *next = curr->next;
        free(curr->key);
        free(curr->val);
        free(curr);
        curr = next;
    }
    s_sd_overrides_head = NULL;

    if (s_i18n_mutex) xSemaphoreGive(s_i18n_mutex);
}