/**
 * @file gui_theme.c
 * @brief Language independent GUI colors, loaded from /gui/theme.json (see gui_theme.h).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "gui_theme.h"
#include "media_standards.h"
#include "cJSON.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "gui_theme";

#define THEME_PATH          "/gui/theme.json"
#define THEME_FILE_BUF_SIZE 2048

/* Media colors closer than this to an alarm color are reported (weighted RGB "redmean"
 * distance, 0..~765; danger red vs. warning yellow is ~250) */
#define THEME_MIN_ALARM_DISTANCE 120.0f

/* Built-in defaults; warning is yellow (clearly distinct from red, also for weaker eyes) */
#define THEME_DEFAULTS {                \
    .alarm_danger       = 0xE74C3C,     \
    .alarm_warning      = 0xF1C40F,     \
    .alarm_info         = 0x9E9E9E,     \
    .alarm_ok           = 0x2ECC71,     \
    .state_incoming     = 0xE74C3C,     \
    .state_going        = 0x2ECC71,     \
    .state_muted        = 0x95A5A6,     \
    .state_retriggered  = 0x3498DB,     \
    .state_acknowledged = 0x7F8C8D,     \
}

static const gui_theme_colors_t k_defaults = THEME_DEFAULTS;

/* Fields are read without a lock: 32-bit stores are atomic, a mix of old and new colors during a
 * reload lasts at most one frame */
static gui_theme_colors_t s_colors = THEME_DEFAULTS;
static volatile uint32_t s_revision = 0;

/* External UART file reader from main/rp2040/rp2040.c */
extern int rp2040_read_file_from_sd(const char *path, char *out_buf, size_t max_len, uint32_t timeout_ms);

void gui_theme_get_colors(gui_theme_colors_t *out) {
    if (out) *out = s_colors;
}

uint32_t gui_theme_alarm_level_color(gui_alarm_level_t level, uint32_t none_color) {
    switch (level) {
        case GUI_ALARM_DANGER:  return s_colors.alarm_danger;
        case GUI_ALARM_WARNING: return s_colors.alarm_warning;
        default:                return none_color;
    }
}

uint32_t gui_theme_severity_color(uint8_t severity) {
    switch (severity) {
        case 0:  return s_colors.alarm_danger;
        case 1:  return s_colors.alarm_warning;
        default: return s_colors.alarm_info;
    }
}

gui_alarm_level_t gui_theme_level_from_severity(uint8_t severity) {
    return (severity == 0) ? GUI_ALARM_DANGER : GUI_ALARM_WARNING;
}

uint32_t gui_theme_get_revision(void) {
    return s_revision;
}

/**
 * @brief Parse "0xRRGGBB", "#RRGGBB" or "RRGGBB" into *out; keeps *out if the item is missing.
 */
static void read_color(const cJSON *obj, const char *name, uint32_t *out) {
    const cJSON *item = cJSON_GetObjectItem(obj, name);
    if (!cJSON_IsString(item) || !item->valuestring) return;
    const char *c = item->valuestring;
    if (c[0] == '#') c++;
    char *end = NULL;
    unsigned long v = strtoul(c, &end, 16);
    if (end == c) {
        ESP_LOGW(TAG, "%s: invalid color '%s' for '%s', keeping default", THEME_PATH, item->valuestring, name);
        return;
    }
    *out = (uint32_t)v & 0xFFFFFF;
}

int gui_theme_load_from_sd(void) {
    char *buf = (char *)malloc(THEME_FILE_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "No memory to read %s", THEME_PATH);
        return -1;
    }
    int len = rp2040_read_file_from_sd(THEME_PATH, buf, THEME_FILE_BUF_SIZE, 1500);
    if (len <= 0) {
        free(buf);
        ESP_LOGW(TAG, "%s not found on SD card, using built-in colors", THEME_PATH);
        return -1;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse %s, using built-in colors", THEME_PATH);
        return -1;
    }

    gui_theme_colors_t c = k_defaults;
    const cJSON *alarm = cJSON_GetObjectItem(root, "alarm");
    if (cJSON_IsObject(alarm)) {
        read_color(alarm, "danger", &c.alarm_danger);
        read_color(alarm, "warning", &c.alarm_warning);
        read_color(alarm, "info", &c.alarm_info);
        read_color(alarm, "ok", &c.alarm_ok);
    }
    const cJSON *state = cJSON_GetObjectItem(root, "alarm_state");
    if (cJSON_IsObject(state)) {
        read_color(state, "incoming", &c.state_incoming);
        read_color(state, "going", &c.state_going);
        read_color(state, "muted", &c.state_muted);
        read_color(state, "retriggered", &c.state_retriggered);
        read_color(state, "acknowledged", &c.state_acknowledged);
    }
    cJSON_Delete(root);

    s_colors = c;
    s_revision++;
    ESP_LOGI(TAG, "Loaded %s (danger 0x%06lX, warning 0x%06lX, info 0x%06lX)", THEME_PATH,
             (unsigned long)c.alarm_danger, (unsigned long)c.alarm_warning, (unsigned long)c.alarm_info);
    return 0;
}

/**
 * @brief Perceptual color distance ("redmean" weighted RGB), 0 .. ~765.
 */
static float color_distance(uint32_t a, uint32_t b) {
    float r1 = (float)((a >> 16) & 0xFF), g1 = (float)((a >> 8) & 0xFF), b1 = (float)(a & 0xFF);
    float r2 = (float)((b >> 16) & 0xFF), g2 = (float)((b >> 8) & 0xFF), b2 = (float)(b & 0xFF);
    float rmean = (r1 + r2) / 2.0f;
    float dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
    return sqrtf((2.0f + rmean / 256.0f) * dr * dr + 4.0f * dg * dg +
                 (2.0f + (255.0f - rmean) / 256.0f) * db * db);
}

void gui_theme_check_media_contrast(void) {
    size_t n = media_standards_count();
    for (size_t i = 0; i < n; i++) {
        char id[16];
        uint32_t color;
        if (!media_standards_get_entry(i, id, sizeof(id), &color)) continue;

        float d_danger = color_distance(color, s_colors.alarm_danger);
        float d_warning = color_distance(color, s_colors.alarm_warning);
        if (d_danger < THEME_MIN_ALARM_DISTANCE) {
            ESP_LOGW(TAG, "Medium '%s' color 0x%06lX is close to the danger color 0x%06lX (distance %.0f < %.0f): "
                          "normal bars may look like an alarm", id, (unsigned long)color,
                     (unsigned long)s_colors.alarm_danger, d_danger, THEME_MIN_ALARM_DISTANCE);
        }
        if (d_warning < THEME_MIN_ALARM_DISTANCE) {
            ESP_LOGW(TAG, "Medium '%s' color 0x%06lX is close to the warning color 0x%06lX (distance %.0f < %.0f): "
                          "normal bars may look like a warning", id, (unsigned long)color,
                     (unsigned long)s_colors.alarm_warning, d_warning, THEME_MIN_ALARM_DISTANCE);
        }
    }
}
