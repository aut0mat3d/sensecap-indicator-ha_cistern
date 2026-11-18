/**
 * @file alarm_evaluator.h
 * @brief Autonomous background threshold evaluator with damping and hysteresis.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef ALARM_EVALUATOR_H
#define ALARM_EVALUATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "alarm_model.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_MAX_EVAL_RULES 32

/**
 * @brief Comparison operators for threshold detection.
 */
typedef enum {
    THRESHOLD_OP_LESS_THAN = 0,    /**< Triggers when value < threshold (e.g. low level, negative flow) */
    THRESHOLD_OP_GREATER_THAN      /**< Triggers when value > threshold (e.g. overflow, temperature) */
} threshold_op_t;

/**
 * @brief Discrete evaluation rule parsed from device configuration.
 */
typedef struct {
    uint32_t dev_addr;             /**< LoRaWAN DevAddr */
    char key[32];                  /**< Telemetry key (e.g. "depth", "flowrate") */
    char rule_id[32];              /**< Threshold ID from JSON (e.g. "alarm_low") */
    char source_name[32];          /**< Device display name (e.g. "Hochbehälter") */
    alarm_severity_t severity;     /**< Alarm severity */
    threshold_op_t op;             /**< Comparison direction */
    float threshold_val;           /**< Calculated absolute threshold */
    float hysteresis_val;          /**< Absolute hysteresis */
    uint8_t damping;               /**< Required consecutive out-of-bounds hits */
    uint8_t buzzer_mode;           /**< Acoustic pattern (0=intermittent, 1=continuous, 2=off) */

    /* Runtime tracking state */
    uint16_t consecutive_hits;     /**< Current filter streak */
    bool is_tripped;               /**< True if currently posted to alarm_model */
} alarm_eval_rule_t;

/**
 * @brief Initialize the autonomous alarm evaluator and bind to telemetry events.
 * @return 0 on success, negative error code on failure.
 */
int alarm_evaluator_init(void);

/**
 * @brief Clear all registered evaluation rules from RAM.
 */
void alarm_evaluator_clear_rules(void);

/**
 * @brief Manually register an evaluation rule into the engine.
 * @param rule Pointer to populated rule structure.
 * @return 0 on success, -1 if table is full.
 */
int alarm_evaluator_register_rule(const alarm_eval_rule_t *rule);

/**
 * @brief Look up configured buzzer_mode for a registered rule by source name and rule ID.
 * @param source_name Device display name (e.g. "Hochbehälter").
 * @param rule_id Threshold rule ID (e.g. "warn_high").
 * @return Buzzer mode (0=intermittent, 1=continuous, 2=off), defaults to 0 if not found.
 */
uint8_t alarm_evaluator_get_rule_buzzer_mode(const char *source_name, const char *rule_id);

/**
 * @brief Autonomous evaluation entrypoint called on incoming telemetry.
 * @param dev_addr Source device address.
 * @param key Telemetry key.
 * @param value Measured or derived value.
 */
void alarm_evaluator_evaluate(uint32_t dev_addr, const char *key, float value);

/**
 * @brief Current alarm state of a device key: most severe tripped rule (hysteresis and damping applied).
 * @param has_rules Optional: set to true if at least one rule exists for the key.
 * @return Severity of the most severe tripped rule (0 = danger, 1 = warning, 2 = info), -1 if none.
 */
int alarm_evaluator_get_key_severity(uint32_t dev_addr, const char *key, bool *has_rules);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_EVALUATOR_H */