/**
 * @file device_config_controller.c
 * @brief Generic connector implementation mapping device thresholds to the generic config controller.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "device_config_controller.h"
#include "config_controller.h"
#include "lora_model.h"
#include "alarm_evaluator.h"
#include "rp2040.h"
#include "gui/i18n/gui_i18n.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "dev_cfg_ctrl";

/* Registered runtime update listener (e.g. view updating graph markers) */
static device_config_threshold_update_cb_t s_threshold_update_cb = NULL;

/* Static title buffer populated on open */
static char s_dynamic_title[96] = {0};

/* Allowed enum strings matching device JSON schema */
static const char *s_severity_options[] = { "danger", "warning", "info" };
static const char *s_buzzer_options[]   = { "intermittent", "continuous", "off" };

/* Field descriptors for standard threshold properties */
static const config_field_desc_t s_threshold_fields[] = {
    { "value",       "alarm_cfg.prop_val",  CONFIG_FIELD_FLOAT, -999.0f, 9999.0f, NULL, NULL, 0 },
    { "hysteresis",  "alarm_cfg.prop_hyst", CONFIG_FIELD_FLOAT,    0.0f,  100.0f, NULL, NULL, 0 },
    { "damping",     "alarm_cfg.prop_damp", CONFIG_FIELD_INT,      0.0f,   20.0f, "alarm_cfg.cycles", NULL, 0 },
    { "severity",    "alarm_cfg.prop_prio", CONFIG_FIELD_ENUM,     0.0f,    0.0f, NULL, s_severity_options, 3 },
    { "buzzer_mode", "alarm_cfg.prop_buzz", CONFIG_FIELD_ENUM,     0.0f,    0.0f, NULL, s_buzzer_options, 3 }
};

/* ── Connector Callbacks ────────────────────────────────────────────────── */

static int _threshold_load_fn(uint32_t dev_addr, config_group_data_t *groups, size_t max_groups, size_t *out_count) {
    lora_device_config_t *dev = lora_model_get_device_by_addr(dev_addr);
    if (!dev || !dev->enabled) {
        ESP_LOGW(TAG, "Device 0x%08lX not found in active RAM table", (unsigned long)dev_addr);
        return -1;
    }

    size_t total_groups = 0;

    /* 1. Load Threshold Groups */
    size_t th_count = (dev->threshold_count <= max_groups) ? dev->threshold_count : max_groups;
    for (size_t t = 0; t < th_count; t++) {
        lora_threshold_config_t *th = &dev->thresholds[t];
        config_group_data_t     *grp = &groups[total_groups++];
        memset(grp, 0, sizeof(config_group_data_t));

        strlcpy(grp->group_id, th->rule_id, sizeof(grp->group_id));
        strlcpy(grp->sub_key, th->key, sizeof(grp->sub_key));

        /* Internationalized Header Title: e.g. "Dry Run Protection (Level)" */
        char th_key_i18n[64];
        snprintf(th_key_i18n, sizeof(th_key_i18n), "threshold.%s", th->rule_id);
        const char *th_title = gui_i18n_get(th_key_i18n);
        if (strcmp(th_title, th_key_i18n) == 0) th_title = th->rule_id;

        char ch_key_i18n[64];
        snprintf(ch_key_i18n, sizeof(ch_key_i18n), "channel.%s", th->key);
        const char *ch_title = gui_i18n_get(ch_key_i18n);
        if (strcmp(ch_title, ch_key_i18n) == 0) {
            snprintf(ch_key_i18n, sizeof(ch_key_i18n), "telemetry.%s", th->key);
            ch_title = gui_i18n_get(ch_key_i18n);
            if (strcmp(ch_title, ch_key_i18n) == 0) ch_title = th->key;
        }

        snprintf(grp->title, sizeof(grp->title), "%s (%s)", th_title, ch_title);

        grp->field_count = 5;

        /* Field 0: Value (Raw value from JSON, exact unit provided by RP2040) */
        strlcpy(grp->fields[0].field_id, "value", sizeof(grp->fields[0].field_id));
        grp->fields[0].type = CONFIG_FIELD_FLOAT;
        grp->fields[0].f_val = th->threshold_val;
        strlcpy(grp->fields[0].unit, th->unit, sizeof(grp->fields[0].unit));

        /* Field 1: Hysteresis */
        strlcpy(grp->fields[1].field_id, "hysteresis", sizeof(grp->fields[1].field_id));
        grp->fields[1].type = CONFIG_FIELD_FLOAT;
        grp->fields[1].f_val = th->hysteresis_val;
        strlcpy(grp->fields[1].unit, th->unit, sizeof(grp->fields[1].unit));

        /* Field 2: Damping */
        strlcpy(grp->fields[2].field_id, "damping", sizeof(grp->fields[2].field_id));
        grp->fields[2].type = CONFIG_FIELD_INT;
        grp->fields[2].i_val = th->damping;
        strlcpy(grp->fields[2].unit, "alarm_cfg.cycles", sizeof(grp->fields[2].unit));

        /* Field 3: Severity */
        strlcpy(grp->fields[3].field_id, "severity", sizeof(grp->fields[3].field_id));
        grp->fields[3].type = CONFIG_FIELD_ENUM;
        if (th->severity == 1) strlcpy(grp->fields[3].enum_val, "warning", sizeof(grp->fields[3].enum_val));
        else if (th->severity == 2) strlcpy(grp->fields[3].enum_val, "info", sizeof(grp->fields[3].enum_val));
        else strlcpy(grp->fields[3].enum_val, "danger", sizeof(grp->fields[3].enum_val));

        /* Field 4: Buzzer Mode */
        strlcpy(grp->fields[4].field_id, "buzzer_mode", sizeof(grp->fields[4].field_id));
        grp->fields[4].type = CONFIG_FIELD_ENUM;
        if (th->buzzer_mode == 1) strlcpy(grp->fields[4].enum_val, "continuous", sizeof(grp->fields[4].enum_val));
        else if (th->buzzer_mode == 2) strlcpy(grp->fields[4].enum_val, "off", sizeof(grp->fields[4].enum_val));
        else strlcpy(grp->fields[4].enum_val, "intermittent", sizeof(grp->fields[4].enum_val));
    }

    /* 2. Load Sensor Offset Groups */
    for (uint8_t o = 0; o < dev->offset_count && total_groups < max_groups; o++) {
        lora_offset_config_t *off = &dev->offsets[o];
        if (!off->valid) continue;

        config_group_data_t *grp = &groups[total_groups++];
        memset(grp, 0, sizeof(config_group_data_t));

        snprintf(grp->group_id, sizeof(grp->group_id), "offset:%s", off->key);
        strlcpy(grp->sub_key, "offset", sizeof(grp->sub_key));

        const char *off_label = gui_i18n_get("alarm_cfg.offset");
        if (strcmp(off_label, "alarm_cfg.offset") == 0) off_label = "Offset";

        char ch_key_i18n[64];
        snprintf(ch_key_i18n, sizeof(ch_key_i18n), "channel.%s", off->key);
        const char *ch_title = gui_i18n_get(ch_key_i18n);
        if (strcmp(ch_title, ch_key_i18n) == 0) {
            snprintf(ch_key_i18n, sizeof(ch_key_i18n), "telemetry.%s", off->key);
            ch_title = gui_i18n_get(ch_key_i18n);
            if (strcmp(ch_title, ch_key_i18n) == 0) ch_title = off->key;
        }

        snprintf(grp->title, sizeof(grp->title), "%s (%s)", off_label, ch_title);

        grp->field_count = 1;
        strlcpy(grp->fields[0].field_id, off->key, sizeof(grp->fields[0].field_id));
        grp->fields[0].type = CONFIG_FIELD_FLOAT;
        grp->fields[0].f_val = off->value;
        strlcpy(grp->fields[0].unit, off->unit, sizeof(grp->fields[0].unit));
    }

    *out_count = total_groups;
    return (int)total_groups;
}

static void _threshold_apply_runtime_fn(uint32_t dev_addr, const config_group_data_t *groups, size_t count) {
    lora_device_config_t *dev = lora_model_get_device_by_addr(dev_addr);
    if (!dev) return;

    float max_depth_cm = dev->geometry.max_depth_cm;

    for (size_t g = 0; g < count; g++) {
        const config_group_data_t *grp = &groups[g];

        /* Handle Sensor Offset Groups */
        if (strncmp(grp->group_id, "offset:", 7) == 0) {
            const char *off_key = grp->fields[0].field_id;
            for (uint8_t o = 0; o < dev->offset_count; o++) {
                if (strcmp(dev->offsets[o].key, off_key) == 0) {
                    dev->offsets[o].value = grp->fields[0].f_val;
                    break;
                }
            }
            continue;
        }

        /* Handle Threshold Groups */
        for (uint8_t t = 0; t < dev->threshold_count; t++) {
            if (strcmp(dev->thresholds[t].rule_id, grp->group_id) == 0) {
                /* Update in-memory values */
                dev->thresholds[t].threshold_val = grp->fields[0].f_val;
                dev->thresholds[t].hysteresis_val = grp->fields[1].f_val;
                dev->thresholds[t].damping = (uint8_t)grp->fields[2].i_val;

                if (strcasecmp(grp->fields[3].enum_val, "warning") == 0) dev->thresholds[t].severity = 1;
                else if (strcasecmp(grp->fields[3].enum_val, "info") == 0) dev->thresholds[t].severity = 2;
                else dev->thresholds[t].severity = 0;

                if (strcasecmp(grp->fields[4].enum_val, "continuous") == 0) dev->thresholds[t].buzzer_mode = 1;
                else if (strcasecmp(grp->fields[4].enum_val, "off") == 0) dev->thresholds[t].buzzer_mode = 2;
                else dev->thresholds[t].buzzer_mode = 0;

                /* Compute physical value for background evaluator */
                float eval_val = dev->thresholds[t].threshold_val;
                float eval_hyst = dev->thresholds[t].hysteresis_val;
                if (dev->thresholds[t].is_percent && max_depth_cm > 0.0f) {
                    eval_val = (dev->thresholds[t].threshold_val / 100.0f) * max_depth_cm;
                    eval_hyst = (dev->thresholds[t].hysteresis_val / 100.0f) * max_depth_cm;
                }

                /* Register updated rule in live alarm evaluator */
                alarm_eval_rule_t rule = {
                    .dev_addr = dev->dev_addr,
                    .severity = (alarm_severity_t)dev->thresholds[t].severity,
                    .op = (threshold_op_t)dev->thresholds[t].op,
                    .threshold_val = eval_val,
                    .hysteresis_val = eval_hyst,
                    .damping = dev->thresholds[t].damping,
                    .buzzer_mode = dev->thresholds[t].buzzer_mode
                };
                strlcpy(rule.source_name, dev->device_name, sizeof(rule.source_name));
                strlcpy(rule.rule_id, dev->thresholds[t].rule_id, sizeof(rule.rule_id));
                strlcpy(rule.key, dev->thresholds[t].key, sizeof(rule.key));

                alarm_evaluator_register_rule(&rule);

                /* Notify registered listener with physical units */
                if (s_threshold_update_cb) {
                    s_threshold_update_cb(dev_addr, 
                                          dev->thresholds[t].key, 
                                          dev->thresholds[t].op, 
                                          eval_val, 
                                          eval_hyst);
                }
                break;
            }
        }
    }
    ESP_LOGI(TAG, "Applied %u updated configuration groups to active ESP32 runtime", (unsigned)count);
}

static bool _threshold_persist_fn(uint32_t dev_addr, const config_group_data_t *groups, size_t count) {
    ESP_LOGI(TAG, "Persisting %u configuration groups for device 0x%08lX to storage...", (unsigned)count, (unsigned long)dev_addr);

    lora_device_config_t *dev = lora_model_get_device_by_addr(dev_addr);
    if (!dev) return false;

    bool all_ok = true;
    for (size_t g = 0; g < count; g++) {
        const config_group_data_t *grp = &groups[g];

        /* Persist Sensor Offset */
        if (strncmp(grp->group_id, "offset:", 7) == 0) {
            const char *off_key = grp->fields[0].field_id;
            float off_val = grp->fields[0].f_val;
            const char *off_unit = grp->fields[0].unit;

            int ret = rp2040_send_set_offset(dev_addr, off_key, off_val, off_unit, 1000);
            if (ret != 0) {
                ESP_LOGE(TAG, "Failed to persist offset '%s' to RP2040", off_key);
                all_ok = false;
            } else {
                ESP_LOGI(TAG, "Successfully persisted offset '%s' to RP2040 SD card", off_key);
            }
            continue;
        }

        /* Persist Alarm Threshold */
        bool is_percent = false;
        for (uint8_t t = 0; t < dev->threshold_count; t++) {
            if (strcmp(dev->thresholds[t].rule_id, grp->group_id) == 0) {
                is_percent = dev->thresholds[t].is_percent;
                break;
            }
        }

        float threshold_val = grp->fields[0].f_val;
        float hysteresis_val = grp->fields[1].f_val;
        uint8_t damping = (uint8_t)grp->fields[2].i_val;

        uint8_t severity = 0;
        if (strcasecmp(grp->fields[3].enum_val, "warning") == 0) severity = 1;
        else if (strcasecmp(grp->fields[3].enum_val, "info") == 0) severity = 2;

        uint8_t buzzer_mode = 0;
        if (strcasecmp(grp->fields[4].enum_val, "continuous") == 0) buzzer_mode = 1;
        else if (strcasecmp(grp->fields[4].enum_val, "off") == 0) buzzer_mode = 2;

        int ret = rp2040_send_set_threshold(dev_addr, grp->group_id,
                                            is_percent, threshold_val, hysteresis_val,
                                            damping, severity, buzzer_mode, 1000);
        if (ret != 0) {
            ESP_LOGE(TAG, "Failed to persist threshold '%s' to RP2040", grp->group_id);
            all_ok = false;
        } else {
            ESP_LOGI(TAG, "Successfully persisted threshold '%s' to RP2040 SD card", grp->group_id);
        }
    }

    return all_ok;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void device_config_controller_register_threshold_cb(device_config_threshold_update_cb_t cb) {
    s_threshold_update_cb = cb;
}

int device_config_controller_open_thresholds(uint32_t dev_addr) {
    lora_device_config_t *dev = lora_model_get_device_by_addr(dev_addr);
    if (!dev) {
        ESP_LOGE(TAG, "Cannot open threshold config: Device 0x%08lX not found", (unsigned long)dev_addr);
        return -1;
    }

    /* Build dynamic localized header title: "<Device Name>: <Localized Title>" */
    const char *i18n_title = gui_i18n_get("alarm_cfg.title");
    if (dev->device_name[0] != '\0') {
        snprintf(s_dynamic_title, sizeof(s_dynamic_title), "%s: %s", dev->device_name, i18n_title);
    } else {
        snprintf(s_dynamic_title, sizeof(s_dynamic_title), "0x%08lX: %s", (unsigned long)dev_addr, i18n_title);
    }

    config_controller_desc_t desc = {
        .title = s_dynamic_title,
        .field_desc_count = 5,
        .field_descriptors = s_threshold_fields,
        .connector = {
            .load_fn = _threshold_load_fn,
            .apply_runtime_fn = _threshold_apply_runtime_fn,
            .persist_fn = _threshold_persist_fn
        }
    };

    return config_controller_open(&desc, dev_addr);
}