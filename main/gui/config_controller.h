/**
 * @file config_controller.h
 * @brief Schema-driven configuration controller bridging UI components
 *        (collapsible_list) with runtime data models and persistence connectors.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef CONFIG_CONTROLLER_H
#define CONFIG_CONTROLLER_H

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>
#include "collapsible_list.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CONFIG_MAX_FIELDS_PER_GROUP  8
#define CONFIG_MAX_GROUPS            8

/**
 * @brief Supported editable property field types.
 */
typedef enum {
    CONFIG_FIELD_FLOAT = 0,    /**< Floating point number (e.g., threshold value, hysteresis) */
    CONFIG_FIELD_INT,          /**< Integer number (e.g., damping cycles) */
    CONFIG_FIELD_ENUM          /**< Discrete string/enum selection (e.g., severity, buzzer mode) */
} config_field_type_t;

/**
 * @brief Field descriptor schema defining presentation and editing constraints.
 */
typedef struct {
    char                field_id[32];      /**< JSON attribute key (e.g. "value", "damping") */
    const char         *i18n_label_key;    /**< Localization dictionary key (e.g. "alarm_cfg.prop_val") */
    config_field_type_t type;              /**< Field type */
    float               min_val;           /**< Lower bound */
    float               max_val;           /**< Upper bound */
    const char         *default_unit;      /**< Fallback unit (e.g. "cycles", "%") */
    const char        **enum_options;      /**< Allowed enum string options */
    uint8_t             enum_count;        /**< Number of enum options */
} config_field_desc_t;

/**
 * @brief Runtime value container for a single field inside a group.
 */
typedef struct {
    char                field_id[32];
    config_field_type_t type;
    union {
        float           f_val;
        int32_t         i_val;
        char            enum_val[32];
    };
    char                unit[16];          /**< Resolved engineering unit (e.g. "cm", "m³/h", "%") */
} config_field_value_t;

/**
 * @brief Runtime container for an entire property group (e.g., one threshold).
 */
typedef struct {
    char                 group_id[32];     /**< Group ID (e.g. "alarm_low") */
    char                 title[64];        /**< Header text (e.g. "-- alarm_low - depth --") */
    char                 sub_key[32];      /**< Primary channel/key (e.g. "depth") */
    uint8_t              field_count;
    config_field_value_t fields[CONFIG_MAX_FIELDS_PER_GROUP];
    void                *user_data;        /**< Optional context pointer */
} config_group_data_t;

/**
 * @brief Data connector interface for reading, applying, and saving parameters.
 */
typedef struct {
    /**
     * @brief Load existing device parameters into working draft groups.
     * @return Number of groups loaded, or negative on error.
     */
    int  (*load_fn)(uint32_t dev_addr, config_group_data_t *groups, size_t max_groups, size_t *out_count);

    /**
     * @brief Apply changes directly to active ESP32 runtime (evaluator, widgets).
     */
    void (*apply_runtime_fn)(uint32_t dev_addr, const config_group_data_t *groups, size_t count);

    /**
     * @brief Persist changes to storage (RP2040 UART -> SD card JSON).
     */
    bool (*persist_fn)(uint32_t dev_addr, const config_group_data_t *groups, size_t count);
} config_connector_t;

/**
 * @brief Full controller configuration descriptor.
 */
typedef struct {
    const char                *title;            /**< Header title text */
    uint8_t                    field_desc_count; /**< Number of property fields per group */
    const config_field_desc_t *field_descriptors;/**< Array of field schemas */
    config_connector_t         connector;        /**< Read/Write connector */
} config_controller_desc_t;

/**
 * @brief Open the configuration controller modal for a given device.
 *
 * @param desc Pointer to controller descriptor.
 * @param dev_addr Target device address.
 * @return 0 on success, negative error code on failure.
 */
int config_controller_open(const config_controller_desc_t *desc, uint32_t dev_addr);

/**
 * @brief Close and clean up the active configuration controller modal.
 */
void config_controller_close(void);

/**
 * @brief Programmatically update a working draft value and refresh the list row.
 *
 * @param group_idx Group index (0-based).
 * @param field_idx Field index within group (0-based).
 * @param new_f_val New float value (if FLOAT type).
 * @param new_i_val New integer value (if INT type).
 * @param new_enum_val New enum string (if ENUM type).
 */
void config_controller_update_draft_value(uint8_t group_idx, uint8_t field_idx,
                                          float new_f_val, int32_t new_i_val, const char *new_enum_val);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_CONTROLLER_H */