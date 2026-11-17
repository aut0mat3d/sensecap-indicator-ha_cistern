/**
 * @file alarm_evaluator.c
 * @brief Autonomous background threshold evaluator with damping and hysteresis.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "alarm_evaluator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "view_data.h"
#include "gui/i18n/gui_i18n.h"

static const char *TAG = "alarm_eval";

static alarm_eval_rule_t s_rules[ALARM_MAX_EVAL_RULES];
static size_t s_rules_count = 0;
static SemaphoreHandle_t s_eval_mutex = NULL;
static esp_event_handler_instance_t s_telem_handler_instance = NULL;

/* ── Rule Management ────────────────────────────────────────────────────── */

void alarm_evaluator_clear_rules(void) {
    if (s_eval_mutex) xSemaphoreTake(s_eval_mutex, portMAX_DELAY);
    memset(s_rules, 0, sizeof(s_rules));
    s_rules_count = 0;
    if (s_eval_mutex) xSemaphoreGive(s_eval_mutex);
    ESP_LOGI(TAG, "All alarm evaluation rules cleared from RAM");
}

int alarm_evaluator_register_rule(const alarm_eval_rule_t *rule) {
    if (!rule) return -1;

    if (s_eval_mutex) xSemaphoreTake(s_eval_mutex, portMAX_DELAY);
    if (s_rules_count >= ALARM_MAX_EVAL_RULES) {
        if (s_eval_mutex) xSemaphoreGive(s_eval_mutex);
        ESP_LOGE(TAG, "Evaluation rules table full (max: %d)", ALARM_MAX_EVAL_RULES);
        return -1;
    }

    /* Check for duplicate rule ID for this device */
    for (size_t i = 0; i < s_rules_count; i++) {
        if (s_rules[i].dev_addr == rule->dev_addr && strcmp(s_rules[i].rule_id, rule->rule_id) == 0) {
            s_rules[i] = *rule;
            if (s_eval_mutex) xSemaphoreGive(s_eval_mutex);
            ESP_LOGI(TAG, "Updated rule '%s' for DevAddr 0x%08lX", rule->rule_id, (unsigned long)rule->dev_addr);
            return 0;
        }
    }

    s_rules[s_rules_count] = *rule;
    s_rules[s_rules_count].consecutive_hits = 0;
    s_rules[s_rules_count].is_tripped = false;
    s_rules_count++;

    if (s_eval_mutex) xSemaphoreGive(s_eval_mutex);

    ESP_LOGI(TAG, "Registered rule [%zu]: Dev=0x%08lX '%s' Key='%s' Op=%s Thr=%.2f Hyst=%.2f Damp=%u",
             s_rules_count - 1, (unsigned long)rule->dev_addr, rule->rule_id, rule->key,
             rule->op == THRESHOLD_OP_LESS_THAN ? "<" : ">",
             rule->threshold_val, rule->hysteresis_val, rule->damping);
    return 0;
}

uint8_t alarm_evaluator_get_rule_buzzer_mode(const char *source_name, const char *rule_id) {
    if (!source_name || !rule_id) return 0;

    uint8_t mode = 0;
    if (s_eval_mutex) xSemaphoreTake(s_eval_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_rules_count; i++) {
        if (strcmp(s_rules[i].source_name, source_name) == 0 &&
            strcmp(s_rules[i].rule_id, rule_id) == 0) {
            mode = s_rules[i].buzzer_mode;
            break;
        }
    }
    if (s_eval_mutex) xSemaphoreGive(s_eval_mutex);
    return mode;
}

/* ── JSON Parser (Dynamic SD-Card configuration) ───────────────────────── */

int alarm_evaluator_load_device_json(const char *json_str) {
    if (!json_str) return -1;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse device JSON");
        return -1;
    }

    /* 1. Extract Device Address and Display Name */
    cJSON *dev_addr_obj = cJSON_GetObjectItem(root, "dev_addr");
    cJSON *name_obj = cJSON_GetObjectItem(root, "device_name");
    if (!dev_addr_obj || !cJSON_IsString(dev_addr_obj)) {
        cJSON_Delete(root);
        return -1;
    }

    uint32_t dev_addr = (uint32_t)strtoul(dev_addr_obj->valuestring, NULL, 16);
    const char *dev_name = (name_obj && cJSON_IsString(name_obj)) ? name_obj->valuestring : "Unknown";

    /* 2. Extract Tank Geometry for Percent Resolution */
    float max_depth_cm = 100.0f; /* Fallback scale */
    cJSON *v_dec = cJSON_GetObjectItem(root, "virtual_decoder");
    if (v_dec) {
        cJSON *vol = cJSON_GetObjectItem(v_dec, "volume");
        if (vol) {
            cJSON *md = cJSON_GetObjectItem(vol, "max_depth");
            if (md) {
                cJSON *v = cJSON_GetObjectItem(md, "value");
                if (v && cJSON_IsNumber(v)) max_depth_cm = (float)v->valuedouble;
            }
        }
    }

    /* 3. Parse Threshold Array */
    cJSON *thresholds = cJSON_GetObjectItem(root, "thresholds");
    if (!thresholds || !cJSON_IsArray(thresholds)) {
        cJSON_Delete(root);
        return 0;
    }

    int registered = 0;
    cJSON *t_item = NULL;
    cJSON_ArrayForEach(t_item, thresholds) {
        cJSON *id_obj   = cJSON_GetObjectItem(t_item, "id");
        cJSON *key_obj  = cJSON_GetObjectItem(t_item, "key");
        cJSON *sev_obj  = cJSON_GetObjectItem(t_item, "severity");
        cJSON *damp_obj = cJSON_GetObjectItem(t_item, "damping");

        if (!id_obj || !key_obj) continue;

        alarm_eval_rule_t rule = {0};
        rule.dev_addr = dev_addr;
        strlcpy(rule.source_name, dev_name, sizeof(rule.source_name));
        strlcpy(rule.rule_id, id_obj->valuestring, sizeof(rule.rule_id));
        strlcpy(rule.key, key_obj->valuestring, sizeof(rule.key));

        /* Severity */
        rule.severity = ALARM_SEVERITY_DANGER;
        if (sev_obj && cJSON_IsString(sev_obj)) {
            if (strcasecmp(sev_obj->valuestring, "warning") == 0) rule.severity = ALARM_SEVERITY_WARNING;
            else if (strcasecmp(sev_obj->valuestring, "info") == 0) rule.severity = ALARM_SEVERITY_INFO;
        }

        /* Damping */
        rule.damping = (damp_obj && cJSON_IsNumber(damp_obj)) ? (uint8_t)damp_obj->valueint : 0;

        /* Buzzer mode (0=intermittent, 1=continuous, 2=off) */
        cJSON *buzz_obj = cJSON_GetObjectItem(t_item, "buzzer_mode");
        rule.buzzer_mode = 0;
        if (buzz_obj && cJSON_IsString(buzz_obj)) {
            if (strcasecmp(buzz_obj->valuestring, "continuous") == 0) rule.buzzer_mode = 1;
            else if (strcasecmp(buzz_obj->valuestring, "off") == 0) rule.buzzer_mode = 2;
        }

        /* Value & Hysteresis Calculation (Percent vs Absolute) */
        cJSON *v_pct = cJSON_GetObjectItem(t_item, "value_percent");
        cJSON *v_abs = cJSON_GetObjectItem(t_item, "value_threshold");
        cJSON *h_pct = cJSON_GetObjectItem(t_item, "hysteresis_percent");
        cJSON *h_abs = cJSON_GetObjectItem(t_item, "hysteresis");

        if (v_pct && cJSON_IsNumber(v_pct)) {
            rule.threshold_val = ((float)v_pct->valuedouble / 100.0f) * max_depth_cm;
            rule.hysteresis_val = (h_pct && cJSON_IsNumber(h_pct))
                                  ? (((float)h_pct->valuedouble / 100.0f) * max_depth_cm)
                                  : 2.0f;
        } else if (v_abs && cJSON_IsNumber(v_abs)) {
            rule.threshold_val = (float)v_abs->valuedouble;
            rule.hysteresis_val = (h_abs && cJSON_IsNumber(h_abs)) ? fabsf((float)h_abs->valuedouble) : 0.5f;
        }

        /* Determine Evaluation Direction ("falling" / "rising") */
        cJSON *dir_obj = cJSON_GetObjectItem(t_item, "direction");
        if (dir_obj && cJSON_IsString(dir_obj)) {
            if (strcasecmp(dir_obj->valuestring, "falling") == 0) {
                rule.op = THRESHOLD_OP_LESS_THAN;
            } else {
                rule.op = THRESHOLD_OP_GREATER_THAN;
            }
        } else {
            /* Legacy fallback heuristic if no direction is specified */
            if (strstr(rule.rule_id, "low") || strstr(rule.rule_id, "min") || rule.threshold_val < 0.0f) {
                rule.op = THRESHOLD_OP_LESS_THAN;
            } else {
                rule.op = THRESHOLD_OP_GREATER_THAN;
            }
        }

        if (alarm_evaluator_register_rule(&rule) == 0) {
            registered++;
        }
    }

    cJSON_Delete(root);
    return registered;
}

/* ── Mathematical Evaluation & Alarm Dispatch ──────────────────────────── */

void alarm_evaluator_evaluate(uint32_t dev_addr, const char *key, float value) {
    if (!key || !s_eval_mutex || isnan(value) || isinf(value)) return;

    /* Gate: Do not evaluate thresholds until system time is synchronized */
    if (!alarm_time_is_valid()) {
        return;
    }

    xSemaphoreTake(s_eval_mutex, portMAX_DELAY);

    for (size_t i = 0; i < s_rules_count; i++) {
        alarm_eval_rule_t *r = &s_rules[i];
        if (r->dev_addr != dev_addr || strcmp(r->key, key) != 0) continue;

        bool condition_met = false;
        bool recovery_met = false;

        /* Direction 1: Lower Threshold (e.g. Dry Run / Negative Draw) */
        if (r->op == THRESHOLD_OP_LESS_THAN) {
            if (value < r->threshold_val) {
                condition_met = true;
            } else if (value >= (r->threshold_val + r->hysteresis_val)) {
                recovery_met = true;
            }
        }
        /* Direction 2: Upper Threshold (e.g. Overflow / High Temp) */
        else {
            if (value > r->threshold_val) {
                condition_met = true;
            } else if (value <= (r->threshold_val - r->hysteresis_val)) {
                recovery_met = true;
            }
        }

        /* 1. Threshold Out-of-Bounds Condition */
        if (condition_met) {
            r->consecutive_hits++;

            /* Damping satisfied (N+1 samples required) */
            if (r->consecutive_hits >= (r->damping + 1)) {
                if (!r->is_tripped) {
                    r->is_tripped = true;

                    /* Build i18n descriptive message (e.g. "Hochbehälter: Trockenlaufschutz") */
                    char msg[64];
                    char i18n_key[64];
                    snprintf(i18n_key, sizeof(i18n_key), "threshold.%s", r->rule_id);
                    const char *desc = gui_i18n_get(i18n_key);
                    if (strcmp(desc, i18n_key) == 0) desc = r->rule_id; /* Fallback to raw ID */

                    snprintf(msg, sizeof(msg), "%s (%.1f)", desc, (double)value);

                    ESP_LOGW(TAG, "THRESHOLD TRIPPED: [%s] %s -> Triggering Alarm!", r->source_name, msg);
                    alarm_add_ex(r->source_name, msg, r->severity, r->buzzer_mode);
                }
            } else {
                ESP_LOGD(TAG, "Damping in progress for [%s]: %u/%u hits",
                         r->rule_id, r->consecutive_hits, r->damping + 1);
            }
        }
        /* 2. Normal / Recovery Condition (Hysteresis Passed) */
        else if (recovery_met) {
            r->consecutive_hits = 0;

            char i18n_key[64];
            snprintf(i18n_key, sizeof(i18n_key), "threshold.%s", r->rule_id);
            const char *desc = gui_i18n_get(i18n_key);
            if (strcmp(desc, i18n_key) == 0) desc = r->rule_id;

            if (r->is_tripped) {
                r->is_tripped = false;
                ESP_LOGI(TAG, "THRESHOLD RECOVERED: [%s] %s (%.1f) -> Resolving Alarm!",
                         r->source_name, desc, (double)value);
            }

            /* Always attempt to resolve matching alarm in alarm_model (handles coldboot recovery) */
            alarm_resolve(r->source_name, desc);
        }
        /* 3. Deadband zone inside hysteresis */
        else {
            /* Keep consecutive_hits steady or clear if bounce occurs */
            if (!r->is_tripped) r->consecutive_hits = 0;
        }
    }

    xSemaphoreGive(s_eval_mutex);
}

/* ── Telemetry Event Listener ──────────────────────────────────────────── */

static void _alarm_evaluator_telem_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id != VIEW_EVENT_LORA_TELEMETRY || !data) return;

    struct view_data_lora_telemetry *telem = (struct view_data_lora_telemetry *)data;

    float val = 0.0f;
    if (telem->val_type == LORA_VAL_TYPE_FLOAT) {
        val = telem->val.f_val;
    } else if (telem->val_type == LORA_VAL_TYPE_INT) {
        val = (float)telem->val.i_val;
    } else {
        return;
    }

    alarm_evaluator_evaluate(telem->dev_addr, telem->key, val);
}

int alarm_evaluator_init(void) {
    if (!s_eval_mutex) {
        s_eval_mutex = xSemaphoreCreateMutex();
    }

    alarm_evaluator_clear_rules();

    /* Register listener on the central ESP-IDF view_event_handle */
    esp_err_t err = esp_event_handler_instance_register_with(
        view_event_handle,
        VIEW_EVENT_BASE,
        VIEW_EVENT_LORA_TELEMETRY,
        _alarm_evaluator_telem_handler,
        NULL,
        &s_telem_handler_instance
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register telemetry listener: %s", esp_err_to_name(err));
        return -1;
    }

    ESP_LOGI(TAG, "Autonomous Alarm Evaluator initialized successfully");
    return 0;
}