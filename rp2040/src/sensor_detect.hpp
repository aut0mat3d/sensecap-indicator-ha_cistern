/**
 * @file sensor_detect.hpp
 * @brief Non-blocking I2C/SPI sensor detection and presence tracking.
 *        Provides plug-and-play support for external sensors and built-in sensors.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef SENSOR_DETECT_HPP
#define SENSOR_DETECT_HPP

#include <stdint.h>
#include <stdbool.h>
#include <Wire.h>

// ============================================================================
// Sensor Types
// ============================================================================

typedef enum {
    SENSOR_TYPE_NONE = 0,
    SENSOR_TYPE_I2C,
    SENSOR_TYPE_SPI,
    SENSOR_TYPE_BUILTIN
} sensor_type_t;

// ============================================================================
// Known I2C Sensor Definitions
// ============================================================================

typedef struct {
    const char* name;          /// Human-readable name (e.g., "AHT20")
    uint8_t address;          /// I2C address (7-bit)
    bool detected;           /// Runtime detection flag
    bool enabled;            /// User enable flag (from builtin.json)
} i2c_sensor_def_t;

// Known I2C sensors
static const i2c_sensor_def_t KNOWN_I2C_SENSORS[] = {
    {"AHT20", 0x38, false, true},
    {"AHT20_alt", 0x39, false, true},  // Some variants use 0x39
    {"SHT41", 0x44, false, true},
    {"SGP40", 0x59, false, true},
    {"SCD4X", 0x62, false, true},
    {"BME280", 0x76, false, false},    // Optional external
    {"BME280_alt", 0x77, false, false},
    {"BH1750", 0x23, false, false},    // External light sensor
    {"BH1750_alt", 0x5C, false, false},
    {nullptr, 0, false, false}  // Terminator
};

// ============================================================================
// Built-in Sensor Definitions (SPI or fixed I2C)
// ============================================================================

typedef struct {
    const char* key;           /// Key for data_storage (e.g., "AHT20_TEMP")
    const char* name;         /// Sensor name
    sensor_type_t type;       /// SENSOR_TYPE_SPI or SENSOR_TYPE_I2C
    uint8_t i2c_address;      /// I2C address (0 if not applicable / SPI sensor)
    bool present;            /// Runtime presence flag
    bool enabled;            /// User enable flag
} builtin_sensor_def_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Initialize sensor detection system
 */
void sensor_detect_init(void);

/**
 * @brief Non-blocking I2C scan step
 *        Call this periodically (e.g., every 100ms) from loop()
 */
void sensor_detect_i2c_scan_step(void);

/**
 * @brief Check if I2C scan is complete
 * @return true if scan completed
 */
bool sensor_detect_i2c_scan_complete(void);

/**
 * @brief Update built-in sensor presence
 * @param key Sensor key (e.g., "AHT20_TEMP")
 * @param present true if sensor is present and responding
 */
void sensor_detect_set_builtin_present(const char* key, bool present);

/**
 * @brief Check if a built-in sensor is present
 * @param key Sensor key
 * @return true if present
 */
bool sensor_detect_is_builtin_present(const char* key);

/**
 * @brief Print sensor detection summary to serial
 */
void sensor_detect_print_summary(void);

/**
 * @brief Periodically print sensor detection summary (every 31 seconds)
 *        Call this from loop()
 */
void sensor_detect_print_summary_periodic(void);

/**
 * @brief Get detected I2C sensor count
 * @return Number of detected I2C sensors
 */
size_t sensor_detect_get_i2c_detected_count(void);

/**
 * @brief Get total known I2C sensor count
 * @return Total number of known I2C sensors
 */
size_t sensor_detect_get_i2c_known_count(void);

/**
 * @brief Check if a specific I2C address is detected
 * @param address 7-bit I2C address
 * @return true if detected
 */
bool sensor_detect_is_i2c_address_detected(uint8_t address);

#endif /* SENSOR_DETECT_HPP */
