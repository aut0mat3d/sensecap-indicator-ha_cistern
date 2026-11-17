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