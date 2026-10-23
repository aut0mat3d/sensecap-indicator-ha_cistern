/**
 * @file sensor_detect.cpp
 * @brief Non-blocking I2C/SPI sensor detection and presence tracking.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "sensor_detect.hpp"
#include "debug_log.hpp"
#include <string.h>

// ============================================================================
// Static State
// ============================================================================

static bool g_i2c_scan_complete = false;
static uint8_t g_i2c_scan_index = 0;
static uint8_t g_i2c_detected_count = 0;
static uint32_t g_last_i2c_scan_ms = 0;  // For rate-limiting I2C scans
static i2c_sensor_def_t g_i2c_sensors[16];  // Copy of known sensors with runtime state
static size_t g_i2c_sensor_count = 0;

// Built-in sensors (SPI or fixed I2C)
static builtin_sensor_def_t g_builtin_sensors[16];
static size_t g_builtin_sensor_count = 0;

// ============================================================================
// Forward Declarations
// ============================================================================

static bool add_builtin_sensor(const char* key, const char* name, sensor_type_t type);
static void sensor_detect_init_builtin_sensors(void);

// ============================================================================
// Initialization
// ============================================================================

void sensor_detect_init(void) {
    // Initialize I2C sensor list from known sensors
    g_i2c_sensor_count = 0;
    for (size_t i = 0; KNOWN_I2C_SENSORS[i].name != nullptr; i++) {
        if (g_i2c_sensor_count < 16) {
            g_i2c_sensors[g_i2c_sensor_count] = KNOWN_I2C_SENSORS[i];
            g_i2c_sensors[g_i2c_sensor_count].detected = false;
            g_i2c_sensor_count++;
        }
    }

    // Initialize built-in sensors
    g_builtin_sensor_count = 0;
    sensor_detect_init_builtin_sensors();

    // Initialize I2C scan state
    g_i2c_scan_complete = false;
    g_i2c_scan_index = 0;
    g_i2c_detected_count = 0;

    DBG_I2C_PRINTF("Sensor detection initialized. Known I2C sensors: %u, Built-in sensors: %u", 
                   g_i2c_sensor_count, g_builtin_sensor_count);
}

// ============================================================================
// I2C Scanning (Non-blocking)
// ============================================================================

/**
 * @brief Scan a single I2C address
 * @param address 7-bit I2C address to scan
 * @return true if device responded
 */
static bool i2c_scan_address(uint8_t address) {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();
    return (error == 0);
}

/**
 * @brief Scan next I2C address in sequence
 *        Addresses 0x08 to 0x77 (7-bit)
 */
static void scan_next_i2c_address(void) {
    if (g_i2c_scan_index > 0x77) {
        g_i2c_scan_complete = true;
        DBG_I2C_PRINTF("I2C scan completed. Detected %u devices.", g_i2c_detected_count);
        return;
    }

    // Skip reserved addresses
    uint8_t addr = g_i2c_scan_index;
    g_i2c_scan_index++;
    
    // Skip 0x00-0x07 and 0x78-0x7F
    if (addr < 0x08 || addr > 0x77) {
        return;
    }

    // Try to communicate with this address
    if (i2c_scan_address(addr)) {
        g_i2c_detected_count++;
        DBG_I2C_PRINTF("I2C device detected at 0x%02X", addr);
        
        // Mark known I2C sensors (from decoder configs) as detected
        for (size_t i = 0; i < g_i2c_sensor_count; i++) {
            if (g_i2c_sensors[i].address == addr) {
                g_i2c_sensors[i].detected = true;
                DBG_I2C_PRINTF("  -> Identified as %s", g_i2c_sensors[i].name);
            }
        }
        
        // Also mark built-in sensors at this address as present
        for (size_t i = 0; i < g_builtin_sensor_count; i++) {
            if (g_builtin_sensors[i].type == SENSOR_TYPE_I2C && 
                g_builtin_sensors[i].i2c_address == addr) {
                g_builtin_sensors[i].present = true;
                g_builtin_sensors[i].enabled = true;
                DBG_I2C_PRINTF("  -> Built-in sensor: %s (%s)", 
                              g_builtin_sensors[i].name, g_builtin_sensors[i].key);
            }
        }
    }
}

void sensor_detect_i2c_scan_step(void) {
    if (g_i2c_scan_complete) return;
    
    uint32_t now_ms = millis();
    // Rate limit: wait at least 10ms between I2C scans to avoid bus contention
    if (now_ms - g_last_i2c_scan_ms < 10) {
        return;
    }
    g_last_i2c_scan_ms = now_ms;
    
    scan_next_i2c_address();
}

bool sensor_detect_i2c_scan_complete(void) {
    return g_i2c_scan_complete;
}

// ============================================================================
// Built-in Sensor Management
// ============================================================================

/**
 * @brief Find built-in sensor by key
 */
static int32_t find_builtin_sensor(const char* key) {
    for (size_t i = 0; i < g_builtin_sensor_count; i++) {
        if (strcmp(g_builtin_sensors[i].key, key) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

/**
 * @brief Add or update a built-in sensor
 */
static bool add_builtin_sensor(const char* key, const char* name, sensor_type_t type, uint8_t i2c_address = 0) {
    int32_t idx = find_builtin_sensor(key);
    if (idx >= 0) {
        g_builtin_sensors[idx].present = false;  // Reset presence
        return true;
    }
    
    if (g_builtin_sensor_count >= 16) return false;
    
    builtin_sensor_def_t* sensor = &g_builtin_sensors[g_builtin_sensor_count++];
    sensor->key = key;
    sensor->name = name;
    sensor->type = type;
    sensor->i2c_address = i2c_address;
    sensor->present = false;
    sensor->enabled = false;  // Default to disabled - will be set to true when I2C scan detects or sensor responds
    
    return true;
}

static void sensor_detect_init_builtin_sensors(void) {
    // Register all built-in sensors from builtin.json
    // I2C addresses for built-in sensors on SenseCAP Indicator
    // Note: These are the DEFAULT addresses - actual presence determined by I2C scan
    add_builtin_sensor("AHT20_TEMP", "AHT20 Temperature", SENSOR_TYPE_I2C, 0x38);
    add_builtin_sensor("AHT20_HUMIDITY", "AHT20 Humidity", SENSOR_TYPE_I2C, 0x38);
    add_builtin_sensor("SHT41_TEMP", "SHT41 Temperature", SENSOR_TYPE_I2C, 0x44);
    add_builtin_sensor("SHT41_HUMIDITY", "SHT41 Humidity", SENSOR_TYPE_I2C, 0x44);
    add_builtin_sensor("SCD41_CO2", "SCD4X CO2", SENSOR_TYPE_I2C, 0x62);
    add_builtin_sensor("SGP40_VOC", "SGP40 VOC Index", SENSOR_TYPE_I2C, 0x59);
    add_builtin_sensor("LIGHT_LUX", "Light Sensor", SENSOR_TYPE_I2C, 0x23);
}

void sensor_detect_set_builtin_present(const char* key, bool present) {
    int32_t idx = find_builtin_sensor(key);
    if (idx >= 0) {
        // For I2C sensors: trust the I2C scan result only after scan is complete
        // This prevents false positives from sensor_*_get() functions
        if (g_builtin_sensors[idx].type == SENSOR_TYPE_I2C && g_i2c_scan_complete) {
            // I2C scan already determined presence - don't override from main.cpp
            return;
        }
        
        // Only log if state changed to avoid spam
        if (g_builtin_sensors[idx].present != present) {
            g_builtin_sensors[idx].present = present;
            g_builtin_sensors[idx].enabled = present;  // Enable sensor if present
            if (present) {
                DBG_I2C_PRINTF("Built-in sensor present: %s (%s)", g_builtin_sensors[idx].name, key);
            } else {
                DBG_I2C_PRINTF("Built-in sensor NOT present: %s (%s)", g_builtin_sensors[idx].name, key);
            }
        }
    }
}

bool sensor_detect_is_builtin_present(const char* key) {
    int32_t idx = find_builtin_sensor(key);
    return (idx >= 0) ? g_builtin_sensors[idx].present : false;
}

// ============================================================================
// Built-in Configuration Loading from SD Card
// ============================================================================

// ============================================================================
// Summary & Reporting
// ============================================================================

// Static for periodic summary
static uint32_t g_last_summary_ms = 0;

void sensor_detect_print_summary(void) {
    DBG_I2C_PRINTF("=== Sensor Detection Summary ===");
    DBG_I2C_PRINTF("I2C Sensors:");
    DBG_I2C_PRINTF("  Detected: %u / Known: %u", 
                   sensor_detect_get_i2c_detected_count(), 
                   sensor_detect_get_i2c_known_count());
    
    // Known I2C sensors
    for (size_t i = 0; i < g_i2c_sensor_count; i++) {
        if (g_i2c_sensors[i].detected) {
            DBG_I2C_PRINTF("  [OK]  %s (0x%02X) - detected", 
                          g_i2c_sensors[i].name, g_i2c_sensors[i].address);
        } else {
            DBG_I2C_PRINTF("  [---] %s (0x%02X) - not detected", 
                          g_i2c_sensors[i].name, g_i2c_sensors[i].address);
        }
    }
    
    // Built-in sensors
    DBG_I2C_PRINTF("Built-in Sensors:");
    for (size_t i = 0; i < g_builtin_sensor_count; i++) {
        if (g_builtin_sensors[i].present) {
            DBG_I2C_PRINTF("  [OK]  %s (%s)", 
                          g_builtin_sensors[i].name, g_builtin_sensors[i].key);
        } else {
            DBG_I2C_PRINTF("  [---] %s (%s)", 
                          g_builtin_sensors[i].name, g_builtin_sensors[i].key);
        }
    }
    DBG_I2C_PRINTF("===============================");
}

size_t sensor_detect_get_i2c_detected_count(void) {
    return g_i2c_detected_count;
}

size_t sensor_detect_get_i2c_known_count(void) {
    return g_i2c_sensor_count;
}

bool sensor_detect_is_i2c_address_detected(uint8_t address) {
    if (address < 0x08 || address > 0x77) return false;
    
    // Check if any known sensor has this address and is detected
    for (size_t i = 0; i < g_i2c_sensor_count; i++) {
        if (g_i2c_sensors[i].address == address && g_i2c_sensors[i].detected) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// Periodic Summary
// ============================================================================

void sensor_detect_print_summary_periodic(void) {
    uint32_t now_ms = millis();
    
    // Print summary every 31 seconds (prime number) - ALWAYS, not just when scan is complete
    if (now_ms - g_last_summary_ms >= 31000) {
        g_last_summary_ms = now_ms;
        sensor_detect_print_summary();
    }
}
