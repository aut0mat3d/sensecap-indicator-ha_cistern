/**
 * @file virtual_decoder.cpp
 * @brief Virtual decoder system for calculated values (volume, flowrate, etc.)
 *        Implements flexible computation pipelines from raw sensor data.
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

#include "virtual_decoder.hpp"
#include "debug_log.hpp"
#include "indicator_rp2040.hpp"
#include "units.h"
#include <string.h>
#include <math.h>
#include <strings.h>

// ============================================================================
// Static State
// ============================================================================

#define MAX_VIRTUAL_DECODERS 16

static virtual_decoder_t g_virtual_decoders[MAX_VIRTUAL_DECODERS];
static size_t g_virtual_decoder_count = 0;

typedef struct {
    char key[32];
    char unit[16];
    float value;
    uint32_t timestamp;
    bool valid;
} vd_output_t;

static vd_output_t g_vd_outputs[MAX_VIRTUAL_DECODERS];
static size_t g_vd_output_count = 0;

// ============================================================================
// Helper Functions
// ============================================================================

static virtual_decoder_t* find_virtual_decoder(const char* output_key) {
    for (size_t i = 0; i < g_virtual_decoder_count; i++) {
        if (strncmp(g_virtual_decoders[i].config.output_key, output_key, sizeof(g_virtual_decoders[i].config.output_key)) == 0) {
            return &g_virtual_decoders[i];
        }
    }
    return nullptr;
}

static void store_vd_output(const char* key, const char* unit, float value, uint32_t timestamp) {
    for (size_t i = 0; i < g_vd_output_count; i++) {
        if (strncmp(g_vd_outputs[i].key, key, sizeof(g_vd_outputs[i].key)) == 0) {
            g_vd_outputs[i].value = value;
            g_vd_outputs[i].timestamp = timestamp;
            if (unit) strncpy(g_vd_outputs[i].unit, unit, sizeof(g_vd_outputs[i].unit) - 1);
            g_vd_outputs[i].valid = true;
            return;
        }
    }
    if (g_vd_output_count < MAX_VIRTUAL_DECODERS) {
        strncpy(g_vd_outputs[g_vd_output_count].key, key, sizeof(g_vd_outputs[g_vd_output_count].key) - 1);
        g_vd_outputs[g_vd_output_count].key[sizeof(g_vd_outputs[g_vd_output_count].key) - 1] = '\0';
        if (unit) strncpy(g_vd_outputs[g_vd_output_count].unit, unit, sizeof(g_vd_outputs[g_vd_output_count].unit) - 1);
        g_vd_outputs[g_vd_output_count].value = value;
        g_vd_outputs[g_vd_output_count].timestamp = timestamp;
        g_vd_outputs[g_vd_output_count].valid = true;
        g_vd_output_count++;
    }
}

static bool get_vd_output(const char* key, float* value, uint32_t* timestamp, char* out_unit = nullptr) {
    for (size_t i = 0; i < g_vd_output_count; i++) {
        if (g_vd_outputs[i].valid && strncmp(g_vd_outputs[i].key, key, sizeof(g_vd_outputs[i].key)) == 0) {
            if (value) *value = g_vd_outputs[i].value;
            if (timestamp) *timestamp = g_vd_outputs[i].timestamp;
            if (out_unit) strncpy(out_unit, g_vd_outputs[i].unit, 15);
            return true;
        }
    }
    return false;
}

// ============================================================================
// Rate Unit Parser (<qty>/<time>)
// ============================================================================

static void parse_rate_unit(const char* rate_unit, char* out_qty, size_t qty_len, float* out_time_factor) {
    if (!rate_unit || strlen(rate_unit) == 0) {
        if (out_qty && qty_len > 0) out_qty[0] = '\0';
        if (out_time_factor) *out_time_factor = 1.0f;
        return;
    }

    const char* slash = strchr(rate_unit, '/');
    if (!slash) {
        if (out_qty) {
            strncpy(out_qty, rate_unit, qty_len - 1);
            out_qty[qty_len - 1] = '\0';
        }
        if (out_time_factor) *out_time_factor = 1.0f;
        return;
    }

    if (out_qty) {
        size_t q_len = slash - rate_unit;
        if (q_len >= qty_len) q_len = qty_len - 1;
        strncpy(out_qty, rate_unit, q_len);
        out_qty[q_len] = '\0';
    }

    const char* t_part = slash + 1;
    float tf = 1.0f;
    if (strcasecmp(t_part, "h") == 0 || strcasecmp(t_part, "std") == 0 || strcasecmp(t_part, "hour") == 0) {
        tf = 3600.0f;
    } else if (strcasecmp(t_part, "min") == 0) {
        tf = 60.0f;
    } else if (strcasecmp(t_part, "s") == 0 || strcasecmp(t_part, "sec") == 0) {
        tf = 1.0f;
    } else if (strcasecmp(t_part, "d") == 0) {
        tf = 86400.0f;
    }
    if (out_time_factor) *out_time_factor = tf;
}

// ============================================================================
// Volume Calculation
// ============================================================================

static float calc_volume(const geometry_config_t& geo, float depth_cm) {
    if (geo.max_depth_cm <= 0.0f) return 0.0f;

    float valid_depth = fminf(geo.max_depth_cm, fmaxf(0.0f, depth_cm));
    float volume_m3 = 0.0f;

    switch (geo.type) {
        case GEO_RECTANGLE: {
            volume_m3 = (geo.width_cm / 100.0f) * (geo.length_cm / 100.0f) * (valid_depth / 100.0f);
            break;
        }
        case GEO_CYLINDER_VERTICAL: {
            float r = (geo.diameter_cm / 200.0f);
            volume_m3 = M_PI * r * r * (valid_depth / 100.0f);
            break;
        }
        case GEO_CYLINDER_HORIZONTAL: {
            float r = geo.diameter_cm / 200.0f;
            float h = valid_depth / 100.0f;
            float L = geo.length_cm / 100.0f;

            if (h <= 0.0f) {
                volume_m3 = 0.0f;
            } else if (h >= (2.0f * r)) {
                volume_m3 = M_PI * r * r * L;
            } else {
                float term1 = r * r * acosf((r - h) / r);
                float term2 = (r - h) * sqrtf(2.0f * r * h - h * h);
                volume_m3 = L * (term1 - term2);
            }

            if (geo.dish_height_cm > 0.0f && h > 0.0f) {
                float hd = geo.dish_height_cm / 100.0f;
                float h_ratio = fminf(1.0f, h / (2.0f * r));
                float dish_vol_max = (2.0f / 3.0f) * M_PI * r * r * hd;
                volume_m3 += dish_vol_max * (3.0f * h_ratio * h_ratio - 2.0f * h_ratio * h_ratio * h_ratio);
            }
            break;
        }
        case GEO_CONE_FRUSTUM: {
            float r1 = geo.bottom_diameter_cm / 200.0f;
            float r2 = geo.top_diameter_cm / 200.0f;
            float H  = geo.height_cm / 100.0f;
            float h  = valid_depth / 100.0f;

            if (H > 0.0f) {
                float r_h = r1 + (r2 - r1) * (h / H);
                volume_m3 = (M_PI * h / 3.0f) * (r1 * r1 + r1 * r_h + r_h * r_h);
                if (geo.cone_compensation > 0.0f) {
                    volume_m3 *= geo.cone_compensation;
                }
            }
            break;
        }
        default:
            break;
    }
    return volume_m3;
}

// ============================================================================
// Derivative Calculation with Unit & Time-Base Conversion
// ============================================================================

static float calc_derivative(virtual_decoder_t* decoder, float current_value, 
                             uint32_t current_timestamp, uint8_t source_index, const char* source_unit) {
    virtual_decoder_state_t* state = &decoder->state;

    // First value: store as historical baseline and return 0
    if (!state->has_last_value[source_index]) {
        state->last_values[source_index] = current_value;
        state->last_timestamps[source_index] = current_timestamp;
        state->has_last_value[source_index] = true;
        return 0.0f;
    }

    uint32_t time_diff_sec = 0;
    if (current_timestamp > state->last_timestamps[source_index]) {
        time_diff_sec = current_timestamp - state->last_timestamps[source_index];
    } else {
        return 0.0f; // Identical or backwards timestamps: no derivation possible
    }

    float delta = current_value - state->last_values[source_index];

    // Parse target rate unit (e.g., "cm/h" -> qty="cm", tf=3600; "m³/h" -> qty="m³", tf=3600)
    char target_qty[16] = {0};
    float time_factor = 1.0f;
    parse_rate_unit(decoder->config.unit, target_qty, sizeof(target_qty), &time_factor);

    // Convert Delta from source unit to target quantity unit
    if (source_unit && strlen(source_unit) > 0 && strlen(target_qty) > 0) {
        delta = units_convert(delta, source_unit, target_qty);
    }

    float rate = (delta / (float)time_diff_sec) * time_factor;

    // Update history with the CURRENT input value for the next packet
    state->last_values[source_index] = current_value;
    state->last_timestamps[source_index] = current_timestamp;

    return rate * decoder->config.corr_factor;
}

// ============================================================================
// Cascading Processing
// ============================================================================

static bool process_virtual_decoders(PacketSerial* ps, uint32_t dev_addr, uint32_t fcnt, 
                                    const char* trigger_key, uint32_t timestamp) {
    bool produced_output = false;
    char updated_keys[MAX_VIRTUAL_DECODERS + 1][32];
    size_t updated_count = 0;

    if (trigger_key != nullptr) {
        strncpy(updated_keys[updated_count], trigger_key, sizeof(updated_keys[0]) - 1);
        updated_keys[updated_count][sizeof(updated_keys[0]) - 1] = '\0';
        updated_count++;
    }

    bool decoder_processed[MAX_VIRTUAL_DECODERS] = {false};
    bool any_computed;

    do {
        any_computed = false;

        for (size_t i = 0; i < g_virtual_decoder_count; i++) {
            virtual_decoder_t* decoder = &g_virtual_decoders[i];
            if (!decoder->enabled || decoder_processed[i]) continue;

            bool needs_calc = false;
            for (uint8_t j = 0; j < decoder->config.source_key_count; j++) {
                for (size_t u = 0; u < updated_count; u++) {
                    if (strncmp(decoder->config.source_keys[j], updated_keys[u], 32) == 0) {
                        needs_calc = true;
                        break;
                    }
                }
                if (needs_calc) break;
            }
            if (!needs_calc) continue;

            bool all_available = true;
            float source_values[MAX_SOURCE_KEYS];
            uint32_t source_timestamps[MAX_SOURCE_KEYS];
            char source_units[MAX_SOURCE_KEYS][16] = {{0}};

            for (uint8_t j = 0; j < decoder->config.source_key_count; j++) {
                const char* s_key = decoder->config.source_keys[j];
                // Read input directly from global blackboard (g_vd_outputs)
                if (get_vd_output(s_key, &source_values[j], &source_timestamps[j], source_units[j])) {
                    // Ready
                } else {
                    all_available = false;
                    break;
                }
            }
            if (!all_available) continue;

            float result = 0.0f;
            bool should_compute = true;
            uint32_t result_ts = timestamp;

            switch (decoder->config.calculation) {
                case CALC_DERIVATIVE: {
                    if (decoder->config.source_key_count > 0) {
                        result = calc_derivative(decoder, source_values[0], source_timestamps[0], 0, source_units[0]);
                    }
                    break;
                }
                case CALC_VOLUME: {
                    if (decoder->config.source_key_count > 0) {
                        float vol_m3 = calc_volume(decoder->config.geometry, source_values[0]);
                        const char* target_unit = (strlen(decoder->config.unit) > 0) ? decoder->config.unit : "m3";
                        result = units_convert(vol_m3, "m3", target_unit);
                    }
                    break;
                }
                case CALC_SUM: {
                    result = 0.0f;
                    for (uint8_t j = 0; j < decoder->config.source_key_count; j++) result += source_values[j];
                    break;
                }
                case CALC_AVERAGE: {
                    result = 0.0f;
                    for (uint8_t j = 0; j < decoder->config.source_key_count; j++) result += source_values[j];
                    if (decoder->config.source_key_count > 0) result /= decoder->config.source_key_count;
                    break;
                }
                case CALC_SUBTRACT: {
                    if (decoder->config.source_key_count == 0) break;
                    result = source_values[0];
                    for (uint8_t j = 1; j < decoder->config.source_key_count; j++) {
                        result -= source_values[j];
                    }
                    break;
                }
                case CALC_MULTIPLY: {
                    if (decoder->config.source_key_count == 0) break;
                    result = source_values[0];
                    for (uint8_t j = 1; j < decoder->config.source_key_count; j++) {
                        result *= source_values[j];
                    }
                    break;
                }
                case CALC_DIVIDE: {
                    if (decoder->config.source_key_count == 0) break;
                    result = source_values[0];
                    for (uint8_t j = 1; j < decoder->config.source_key_count; j++) {
                        if (source_values[j] != 0.0f) {
                            result /= source_values[j];
                        } else {
                            result = 0.0f;
                            break;
                        }
                    }
                    break;
                }
                default:
                    should_compute = false;
                    break;
            }

            if (!should_compute) continue;

            // Store result ONLY in global blackboard
            store_vd_output(decoder->config.output_key, decoder->config.unit, result, result_ts);

            if (updated_count < (MAX_VIRTUAL_DECODERS + 1)) {
                strncpy(updated_keys[updated_count], decoder->config.output_key, sizeof(updated_keys[0]) - 1);
                updated_keys[updated_count][sizeof(updated_keys[0]) - 1] = '\0';
                updated_count++;
            }

            decoder_processed[i] = true;
            any_computed = true;
            produced_output = true;

            DBG_LORA_PRINTF("Virtual decoder computed %s = %.4f %s",
                           decoder->config.output_key, result, decoder->config.unit);

            if (ps) {
                uint8_t tx_buf[64];
                size_t idx = 0;
                
                /* Select packet type: local I2C (< 0x01000000) vs external LoRa */
                uint8_t pkt_id = (dev_addr < 0x01000000) ? 
                                 static_cast<uint8_t>(PKT_TYPE_LOCAL_TELEMETRY) : 
                                 static_cast<uint8_t>(PKT_TYPE_LORA_TELEMETRY);
                tx_buf[idx++] = pkt_id;
                
                memcpy(&tx_buf[idx], &dev_addr, 4); idx += 4;
                memcpy(&tx_buf[idx], &fcnt, 4); idx += 4;
                tx_buf[idx++] = 0; // float type
                tx_buf[idx++] = 0; // measurement context

                size_t key_len = strlen(decoder->config.output_key) + 1;
                if (idx + key_len <= sizeof(tx_buf)) {
                    memcpy(&tx_buf[idx], decoder->config.output_key, key_len);
                    idx += key_len;
                }

                size_t unit_len = strlen(decoder->config.unit) + 1;
                if (idx + unit_len <= sizeof(tx_buf)) {
                    memcpy(&tx_buf[idx], decoder->config.unit, unit_len);
                    idx += unit_len;
                }

                if (idx + sizeof(float) <= sizeof(tx_buf)) {
                    memcpy(&tx_buf[idx], &result, sizeof(float));
                    idx += sizeof(float);
                }

                uint8_t checksum = 0;
                for (size_t j = 0; j < idx; j++) checksum ^= tx_buf[j];
                tx_buf[idx++] = checksum;
                ps->send(tx_buf, idx);
            }
        }
    } while (any_computed);

    return produced_output;
}

// ============================================================================
// Public API
// ============================================================================

void virtual_decoder_init(void) {
    g_virtual_decoder_count = 0;
    g_vd_output_count = 0;
    memset(g_virtual_decoders, 0, sizeof(g_virtual_decoders));
    memset(g_vd_outputs, 0, sizeof(g_vd_outputs));
}

void virtual_decoder_clear_outputs(void) {
    for (size_t i = 0; i < MAX_VIRTUAL_DECODERS; i++) g_vd_outputs[i].valid = false;
    g_vd_output_count = 0;
    for (size_t i = 0; i < g_virtual_decoder_count; i++) {
        for (uint8_t j = 0; j < MAX_SOURCE_KEYS; j++) g_virtual_decoders[i].state.has_last_value[j] = false;
    }
}

bool virtual_decoder_register(const virtual_decoder_config_t* config) {
    if (!config || g_virtual_decoder_count >= MAX_VIRTUAL_DECODERS) return false;

    for (size_t i = 0; i < g_virtual_decoder_count; i++) {
        if (strncmp(g_virtual_decoders[i].config.output_key, config->output_key, 32) == 0) {
            g_virtual_decoders[i].config = *config;
            return true;
        }
    }

    virtual_decoder_t* decoder = &g_virtual_decoders[g_virtual_decoder_count++];
    decoder->config = *config;
    memset(&decoder->state, 0, sizeof(decoder->state));
    decoder->enabled = true;
    return true;
}

bool virtual_decoder_process(const char* key, float value, uint32_t timestamp) {
    return virtual_decoder_process_and_send((PacketSerial*)nullptr, nullptr, 0, 0, key, value, timestamp, nullptr);
}

bool virtual_decoder_get_output(const char* output_key, float* value, char* unit) {
    return get_vd_output(output_key, value, nullptr, unit);
}

bool virtual_decoder_process_and_send(PacketSerial* ps, const char* device_name, uint32_t dev_addr, uint32_t fcnt, 
                                       const char* key, float value, uint32_t timestamp, const char* source_unit) {
    if (!key) return false;

    // Cache source value and unit on the blackboard
    store_vd_output(key, source_unit, value, timestamp);

    // Process cascade
    return process_virtual_decoders(ps, dev_addr, fcnt, key, timestamp);
}