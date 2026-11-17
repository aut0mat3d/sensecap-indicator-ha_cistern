/* main/util/media_standards.c */
/**
 * @file media_standards.c
 * @brief Medium registry (water, oil, ...) with user-defined main colors and default units.
 *        Loaded from /gui/media_standards.json on the SD card (read via the RP2040 file proxy).
 */
#include "media_standards.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "media_std";

#define MAX_DYNAMIC_MEDIA 16
#define MEDIA_STANDARDS_PATH "/gui/media_standards.json"
#define MEDIA_FILE_BUF_SIZE 4096

typedef struct {
    char id[16];
    uint32_t color_hex;
    char unit[12];
} media_entry_t;

static media_entry_t s_media_table[MAX_DYNAMIC_MEDIA];
static size_t s_media_count = 0;
static SemaphoreHandle_t s_media_mutex = NULL;
static volatile uint32_t s_media_revision = 0;

/* External UART file reader from main/rp2040/rp2040.c */
extern int rp2040_read_file_from_sd(const char *path, char *out_buf, size_t max_len, uint32_t timeout_ms);

static void media_lock(void) {
    if (!s_media_mutex) s_media_mutex = xSemaphoreCreateMutex();
    if (s_media_mutex) xSemaphoreTake(s_media_mutex, portMAX_DELAY);
}

static void media_unlock(void) {
    if (s_media_mutex) xSemaphoreGive(s_media_mutex);
}

void media_standards_clear(void) {
    media_lock();
    s_media_count = 0;
    memset(s_media_table, 0, sizeof(s_media_table));
    media_unlock();
}

void media_standards_add(const char *id, uint32_t color_hex, const char *unit) {
    if (!id) return;
    media_lock();
    if (s_media_count < MAX_DYNAMIC_MEDIA) {
        strncpy(s_media_table[s_media_count].id, id, sizeof(s_media_table[0].id) - 1);
        s_media_table[s_media_count].color_hex = color_hex;
        strncpy(s_media_table[s_media_count].unit, unit ? unit : "", sizeof(s_media_table[0].unit) - 1);
        s_media_count++;
    }
    media_unlock();
}

int media_standards_load_from_sd(void) {
    char *json_buf = (char *)malloc(MEDIA_FILE_BUF_SIZE);
    if (!json_buf) {
        ESP_LOGE(TAG, "Failed to allocate buffer for %s", MEDIA_STANDARDS_PATH);
        return -1;
    }

    int len = rp2040_read_file_from_sd(MEDIA_STANDARDS_PATH, json_buf, MEDIA_FILE_BUF_SIZE, 1500);
    if (len <= 0) {
        ESP_LOGW(TAG, "%s not found on SD card, keeping current media colors", MEDIA_STANDARDS_PATH);
        free(json_buf);
        return -1;
    }

    cJSON *root = cJSON_Parse(json_buf);
    free(json_buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse %s", MEDIA_STANDARDS_PATH);
        return -1;
    }

    cJSON *types = cJSON_GetObjectItem(root, "media_types");
    if (!cJSON_IsObject(types)) {
        ESP_LOGE(TAG, "%s has no 'media_types' object", MEDIA_STANDARDS_PATH);
        cJSON_Delete(root);
        return -1;
    }

    media_lock();
    s_media_count = 0;
    memset(s_media_table, 0, sizeof(s_media_table));

    cJSON *medium = NULL;
    cJSON_ArrayForEach(medium, types) {
        if (s_media_count >= MAX_DYNAMIC_MEDIA) {
            ESP_LOGW(TAG, "More than %d media types, remaining entries ignored", MAX_DYNAMIC_MEDIA);
            break;
        }
        if (!medium->string) continue;

        const cJSON *color = cJSON_GetObjectItem(medium, "color_hex");
        const cJSON *unit = cJSON_GetObjectItem(medium, "unit_default");

        media_entry_t *e = &s_media_table[s_media_count];
        strncpy(e->id, medium->string, sizeof(e->id) - 1);
        /* Accepts "0x3498DB", "#3498DB" and "3498DB" */
        if (cJSON_IsString(color) && color->valuestring) {
            const char *c = color->valuestring;
            if (c[0] == '#') c++;
            e->color_hex = (uint32_t)strtoul(c, NULL, 16) & 0xFFFFFF;
        } else {
            e->color_hex = 0x7F8C8D;
        }
        if (cJSON_IsString(unit) && unit->valuestring) {
            strncpy(e->unit, unit->valuestring, sizeof(e->unit) - 1);
        }
        s_media_count++;
    }
    int count = (int)s_media_count;
    s_media_revision++;
    media_unlock();

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d media types from %s", count, MEDIA_STANDARDS_PATH);
    return count;
}

uint32_t media_standards_get_revision(void) {
    return s_media_revision;
}

uint32_t media_get_color(const char *medium_id) {
    uint32_t color = 0x7F8C8D; /* Neutral grey fallback */
    if (medium_id) {
        media_lock();
        for (size_t i = 0; i < s_media_count; i++) {
            if (strcmp(s_media_table[i].id, medium_id) == 0) {
                color = s_media_table[i].color_hex;
                break;
            }
        }
        media_unlock();
    }
    return color;
}

const char *media_get_unit(const char *medium_id) {
    /* Returned pointer stays valid until the next reload from SD */
    if (medium_id) {
        for (size_t i = 0; i < s_media_count; i++) {
            if (strcmp(s_media_table[i].id, medium_id) == 0) {
                return s_media_table[i].unit;
            }
        }
    }
    return "";
}
