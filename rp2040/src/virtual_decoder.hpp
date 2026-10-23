/**
 * @file virtual_decoder.hpp
 * @brief Virtual decoder system for calculated values (volume, flowrate, etc.)
 *        Enables flexible computation pipelines from raw sensor data.
 *
 * @details Supported calculation types:
 *          - CALC_DERIVATIVE: Rate of change (current - previous) / time_diff
 *          - CALC_VOLUME: Volume calculation using geometry (rectangle, cylinder, cone)
 *          - CALC_DEWPOINT: Dew point from temperature and humidity
 *          - CALC_LUT: Look-up table interpolation
 *          - CALC_SUM: Sum of multiple source values
 *          - CALC_AVERAGE: Average of multiple source values
 *          - CALC_SUBTRACT: Subtraction of source values (source[0] - source[1] - ...)
 *          - CALC_MULTIPLY: Multiplication of source values (source[0] * source[1] * ...)
 *          - CALC_DIVIDE: Division of source values (source[0] / source[1] / ...)
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef VIRTUAL_DECODER_HPP
#define VIRTUAL_DECODER_HPP

#include <stdint.h>
#include <stdbool.h>
#include <PacketSerial.h>

// ============================================================================
// Calculation Types
// ============================================================================

typedef enum {
    CALC_NONE = 0,
    CALC_DERIVATIVE,      /// Rate of change: (current - previous) / (time_diff)
    CALC_VOLUME,          /// Volume calculation (uses geometry type)
    CALC_DEWPOINT,        /// Dew point calculation (requires temp + humidity)
    CALC_LUT,             /// Look-up table interpolation
    CALC_SUM,             /// Sum of multiple sources
    CALC_AVERAGE,         /// Average of multiple sources
    CALC_SUBTRACT,        /// Subtraction: source[0] - source[1] - ... - source[n]
    CALC_MULTIPLY,        /// Multiplication: source[0] * source[1] * ... * source[n]
    CALC_DIVIDE           /// Division: source[0] / source[1] / ... / source[n]
} calc_type_t;

// ============================================================================
// Geometry Types (for volume calculations)
// ============================================================================

typedef enum {
    GEO_NONE = 0,
    GEO_RECTANGLE,
    GEO_CYLINDER_VERTICAL,
    GEO_CYLINDER_HORIZONTAL,
    GEO_CONE_FRUSTUM
} geo_type_t;

// ============================================================================
// Value with Unit
// ============================================================================

typedef struct {
    float value;
    char unit[16];
} value_with_unit_t;

// ============================================================================
// Geometry Configuration
// ============================================================================

typedef struct {
    geo_type_t type;
    float max_depth_cm;
    float width_cm;
    float length_cm;
    float diameter_cm;
    float dish_height_cm;
    float bottom_diameter_cm;
    float top_diameter_cm;
    float height_cm;
    float cone_compensation;
} geometry_config_t;

// ============================================================================
// Maximum number of source keys for multi-input calculations
// ============================================================================

#define MAX_SOURCE_KEYS 4

// ============================================================================
// Virtual Decoder Configuration
// ============================================================================

typedef struct {
    char source_keys[MAX_SOURCE_KEYS][32]; /// Keys of the source values (e.g., ["temp", "humidity"])
    uint8_t source_key_count;             /// Number of source keys used
    calc_type_t calculation;               /// Type of calculation
    geometry_config_t geometry;           /// Geometry config (for volume calculations)
    uint32_t interval_sec;                 /// 0 = on new data, >0 = fixed interval
    char output_key[32];                  /// Key for the computed output (e.g., "volume")
    char unit[16];                        /// Unit of the output
    float corr_factor;                    /// Correction factor (default: 1.0)
} virtual_decoder_config_t;

// ============================================================================
// Virtual Decoder State (runtime)
// ============================================================================

typedef struct {
    float last_values[MAX_SOURCE_KEYS];     /// Last values for each source key
    uint32_t last_timestamps[MAX_SOURCE_KEYS]; /// Timestamps for each source key
    bool has_last_value[MAX_SOURCE_KEYS]; /// Flag for each source key availability
} virtual_decoder_state_t;

// ============================================================================
// Virtual Decoder Instance
// ============================================================================

typedef struct {
    virtual_decoder_config_t config;
    virtual_decoder_state_t state;
    bool enabled;
} virtual_decoder_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Initialize virtual decoder system
 */
void virtual_decoder_init(void);

/**
 * @brief Clear all virtual decoder outputs (call on config reload)
 */
void virtual_decoder_clear_outputs(void);

/**
 * @brief Register a virtual decoder from JSON configuration
 * @param config Pointer to virtual decoder configuration
 * @return true on success
 */
bool virtual_decoder_register(const virtual_decoder_config_t* config);

/**
 * @brief Process a new raw value through all virtual decoders
 * @param key The key of the raw value (e.g., "depth")
 * @param value The raw value
 * @param timestamp Unix timestamp of the measurement
 * @return true if any virtual decoder produced output
 */
bool virtual_decoder_process(const char* key, float value, uint32_t timestamp);

/**
 * @brief Get computed value for a virtual decoder output key
 * @param output_key The output key (e.g., "volume", "flowrate")
 * @param value Output: computed value
 * @param unit Output: unit string (max 16 chars)
 * @return true if virtual decoder exists and has computed a value
 */
bool virtual_decoder_get_output(const char* output_key, float* value, char* unit);

/**
 * @brief Process a new raw value through all virtual decoders and send outputs to ESP32
 * @param ps PacketSerial instance for sending to ESP32
 * @param device_name Name of the device (for LoRa telemetry) or NULL for built-in sensors
 * @param dev_addr Device address (for LoRa telemetry) or 0 for built-in sensors
 * @param fcnt Frame counter (for LoRa telemetry) or 0 for built-in sensors
 * @param key The key of the raw value (e.g., "depth")
 * @param value The raw value
 * @param timestamp Unix timestamp of the measurement
 * @return true if any virtual decoder produced output
 */
bool virtual_decoder_process_and_send(PacketSerial* ps, const char* device_name, uint32_t dev_addr, uint32_t fcnt, 
                                       const char* key, float value, uint32_t timestamp, const char* source_unit = nullptr);

/**
 * @brief Reference overload for backward compatibility with existing callers
 */
inline bool virtual_decoder_process_and_send(PacketSerial& ps, const char* device_name, uint32_t dev_addr, uint32_t fcnt, 
                                              const char* key, float value, uint32_t timestamp, const char* source_unit = nullptr) {
    return virtual_decoder_process_and_send(&ps, device_name, dev_addr, fcnt, key, value, timestamp, source_unit);
}

/**
 * @brief Check if a virtual decoder has all required source keys available
 * @param decoder Pointer to virtual decoder instance
 * @return true if all required source keys have values
 */
bool virtual_decoder_has_all_sources(const virtual_decoder_t* decoder);

#endif /* VIRTUAL_DECODER_HPP */
