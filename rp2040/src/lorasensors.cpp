/**
 * @file lorasensors.cpp
 * @brief Parsing engine for global system & LoRa settings, multi-device JSON configs,
 *        and case-insensitive RAM-cached IPSO decoders.
 */

#include "lorasensors.hpp"
#include "debug_log.hpp"
#include "uart_link.hpp"
#include "data_storage.hpp"
#include "virtual_decoder.hpp"
#include <math.h>
#include <time.h>
#include "units.h"

// Generic device offset table in RAM (preserves lora_device_config_t binary layout)
typedef struct {
    char  key[24];
    float value;
    char  unit[16];
    bool  valid;
} device_offset_t;
static device_offset_t g_device_offsets[MAX_LORA_DEVICES][MAX_DEVICE_OFFSETS];

typedef struct {
    char    key[24];
    float   val_min;
    float   val_max;
    uint8_t cap_min; /* 0 = Drop, 1 = Clamp, 2 = Pass-Through */
    uint8_t cap_max; /* 0 = Drop, 1 = Clamp, 2 = Pass-Through */
    bool    valid;
} device_limit_t;
static device_limit_t g_device_limits[MAX_LORA_DEVICES][MAX_DEVICE_LIMITS];

static lora_global_config_t g_global_cfg = {
    .rf_frequency_hz = 868300000,
    .rf_spreading_factor = 10,
    .rf_bandwidth_khz = 125,
    .rf_coding_rate = 5,
    .rf_preamble_len = 8,
    .sync_word = 0x34
};

static lora_device_config_t g_devices[MAX_LORA_DEVICES];
static int8_t               g_device_decoder_map[MAX_LORA_DEVICES];
static size_t               g_device_count = 0;

static lora_decoder_t       g_decoders[MAX_LORA_DECODERS];
static size_t               g_decoder_count = 0;

static inline double round_6dp(double val) {
    return round(val * 1000000.0) / 1000000.0;
}

static uint8_t parse_hex_u8(const char* str) {
    if (!str) return 0;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
    return static_cast<uint8_t>(strtoul(str, NULL, 16));
}

static uint32_t parse_hex_u32(const char* str) {
    if (!str) return 0;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
    return static_cast<uint32_t>(strtoul(str, NULL, 16));
}

static bool parse_hex_bytes(const char* str, uint8_t* out_bytes, size_t max_len) {
    if (!str) return false;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
    
    size_t str_len = strlen(str);
    if (str_len < max_len * 2) return false;

    for (size_t i = 0; i < max_len; i++) {
        char buf[3] = { str[i * 2], str[i * 2 + 1], '\0' };
        out_bytes[i] = static_cast<uint8_t>(strtoul(buf, NULL, 16));
    }
    return true;
}

float lorasensors_calc_tank_volume(const tank_geometry_t &geo, float depth_cm, float &out_percent) {
    if (geo.max_depth_cm <= 0.0f) {
        out_percent = 0.0f;
        return 0.0f;
    }

    float valid_depth = fminf(geo.max_depth_cm, fmaxf(0.0f, depth_cm));
    out_percent = (valid_depth / geo.max_depth_cm) * 100.0f;
    float volume_m3 = 0.0f;

    switch (geo.type) {
        case TANK_TYPE_RECTANGLE: {
            volume_m3 = (geo.width_cm / 100.0f) * (geo.length_cm / 100.0f) * (valid_depth / 100.0f);
            break;
        }
        case TANK_TYPE_CYLINDER_VERTICAL: {
            float r = (geo.diameter_cm / 200.0f);
            volume_m3 = M_PI * r * r * (valid_depth / 100.0f);
            break;
        }
        case TANK_TYPE_CYLINDER_HORIZONTAL: {
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
        case TANK_TYPE_CONE_FRUSTUM: {
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
        case TANK_TYPE_LUT: {
            if (geo.lut_count == 0) return 0.0f;
            if (valid_depth <= geo.lut[0].depth_cm) return geo.lut[0].liters / 1000.0f;
            if (valid_depth >= geo.lut[geo.lut_count - 1].depth_cm) return geo.lut[geo.lut_count - 1].liters / 1000.0f;

            for (uint8_t i = 0; i < geo.lut_count - 1; i++) {
                if (valid_depth >= geo.lut[i].depth_cm && valid_depth <= geo.lut[i+1].depth_cm) {
                    float span_d = geo.lut[i+1].depth_cm - geo.lut[i].depth_cm;
                    float span_v = geo.lut[i+1].liters - geo.lut[i].liters;
                    float frac = (span_d > 0.0f) ? ((valid_depth - geo.lut[i].depth_cm) / span_d) : 0.0f;
                    return (geo.lut[i].liters + frac * span_v) / 1000.0f;
                }
            }
            break;
        }
        default:
            break;
    }

    return volume_m3;
}



const lora_command_t* lorasensors_find_command(const char* decoder_name, const char* cmd_key) {
    if (!decoder_name || !cmd_key) return NULL;

    for (size_t i = 0; i < g_decoder_count; i++) {
        if (strcasecmp(g_decoders[i].name, decoder_name) == 0) {
            for (uint8_t c = 0; c < g_decoders[i].command_count; c++) {
                if (strcmp(g_decoders[i].commands[c].key, cmd_key) == 0) {
                    return &g_decoders[i].commands[c];
                }
            }
            break;
        }
    }
    return NULL;
}

size_t lorasensors_build_command_payload(const lora_command_t* cmd, uint32_t val, uint8_t* out_bytes, size_t max_len) {
    if (!cmd || !out_bytes || max_len == 0) return 0;

    if (!cmd->is_dynamic) {
        size_t hex_len = strlen(cmd->raw_hex);
        if (hex_len == 0 || hex_len % 2 != 0) return 0;
        size_t bytes_needed = hex_len / 2;
        if (bytes_needed > max_len) return 0;

        for (size_t i = 0; i < bytes_needed; i++) {
            char buf[3] = { cmd->raw_hex[i * 2], cmd->raw_hex[i * 2 + 1], '\0' };
            out_bytes[i] = static_cast<uint8_t>(strtoul(buf, NULL, 16));
        }
        return bytes_needed;
    } else {
        size_t prefix_len = strlen(cmd->prefix);
        if (prefix_len == 0 || prefix_len % 2 != 0) return 0;
        size_t prefix_bytes = prefix_len / 2;

        size_t val_bytes = 0;
        if (strcmp(cmd->datatype, "uint8") == 0) val_bytes = 1;
        else if (strcmp(cmd->datatype, "uint16_le") == 0) val_bytes = 2;
        else if (strcmp(cmd->datatype, "uint32_le") == 0) val_bytes = 4;
        else val_bytes = 2;

        if (prefix_bytes + val_bytes > max_len) return 0;

        for (size_t i = 0; i < prefix_bytes; i++) {
            char buf[3] = { cmd->prefix[i * 2], cmd->prefix[i * 2 + 1], '\0' };
            out_bytes[i] = static_cast<uint8_t>(strtoul(buf, NULL, 16));
        }

        for (size_t b = 0; b < val_bytes; b++) {
            out_bytes[prefix_bytes + b] = static_cast<uint8_t>((val >> (b * 8)) & 0xFF);
        }

        return prefix_bytes + val_bytes;
    }
}

/* ── Case-Insensitive Decoder Resolver & RAM Cacher ─────────────────────── */

static int8_t lorasensors_get_or_load_decoder(SdFat32 &sd, const char* decoder_name) {
    if (!decoder_name || strlen(decoder_name) == 0) return -1;

    for (size_t i = 0; i < g_decoder_count; i++) {
        if (strcasecmp(g_decoders[i].name, decoder_name) == 0) {
            return static_cast<int8_t>(i);
        }
    }

    if (g_decoder_count >= MAX_LORA_DECODERS) {
        LogSerial.println("RP2040 Error: Maximum RAM decoder limit reached!");
        return -1;
    }

    char target_filename[64];
    snprintf(target_filename, sizeof(target_filename), "%s.json", decoder_name);

    /* Updated SD Path: /decoder/<decoder_name>.json */
    char full_path[100];
    snprintf(full_path, sizeof(full_path), "/decoder/%s", target_filename);

    File32 file = sd.open(full_path, O_READ);

    if (!file) {
        File32 dec_dir = sd.open("/decoder/");
        if (dec_dir && dec_dir.isDir()) {
            File32 entry;
            while (entry.openNext(&dec_dir, O_READ)) {
                if (!entry.isDir()) {
                    char entry_name[64];
                    entry.getName(entry_name, sizeof(entry_name));
                    if (strcasecmp(entry_name, target_filename) == 0) {
                        file = entry;
                        DBG_LORA_PRINTF("Case-insensitive match found for decoder '%s' -> '%s'",
                                        decoder_name, entry_name);
                        break;
                    }
                }
                entry.close();
            }
            dec_dir.close();
        }
    }

    if (!file) {
        LogSerial.printf("RP2040 Error: Cannot decode! Decoder file '/decoder/%s.json' missing on SD card!\r\n",
                      decoder_name);
        return -1;
    }

    static StaticJsonDocument<3072> doc;
    doc.clear();
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        LogSerial.printf("RP2040 Error: Failed to parse '/decoder/%s.json' (%s)\r\n",
                      decoder_name, err.c_str());
        return -1;
    }

    lora_decoder_t &dec = g_decoders[g_decoder_count];
    memset(&dec, 0, sizeof(lora_decoder_t));
    strncpy(dec.name, decoder_name, sizeof(dec.name) - 1);
    strncpy(dec.format, doc["format"] | "IPSO_TLV", sizeof(dec.format) - 1);

    JsonArray fport_cfgs = doc["fport_config"];
    if (fport_cfgs && fport_cfgs.size() > 0) {
        JsonObject cfg = fport_cfgs[0];
        dec.fport_cfg.fport = cfg["fport"] | 0;
        
        const char* enc_str = cfg["encryption"] | "full";
        if (strcmp(enc_str, "none") == 0) dec.fport_cfg.encryption_mode = 1;
        else if (strcmp(enc_str, "partial") == 0) dec.fport_cfg.encryption_mode = 2;
        else dec.fport_cfg.encryption_mode = 0;

        dec.fport_cfg.cipher_offset = cfg["cipher_offset"] | 0;
        
        const char* key_str = cfg["key"] | "app_skey";
        dec.fport_cfg.key_type = (strcmp(key_str, "nwk_skey") == 0) ? 1 : 0;
        
        dec.fport_cfg.payload_offset = cfg["payload_offset"] | 0;
        dec.fport_cfg.valid = true;
    }

    JsonArray mappings = doc["mappings"];
    if (mappings) {
        for (JsonObject rule : mappings) {
            if (dec.rule_count >= MAX_RULES_PER_DECODER) break;
            decoder_rule_t &r = dec.rules[dec.rule_count++];

            r.fport   = rule["fport"] | 0;
            r.channel = parse_hex_u8(rule["channel"] | "0x00");
            r.type    = parse_hex_u8(rule["type"] | "0x00");
            r.offset  = rule["offset"] | 0;
            r.length  = rule["length"] | 0;

            strncpy(r.key, rule["key"] | "unknown", sizeof(r.key) - 1);
            strncpy(r.unit, rule["unit"] | "", sizeof(r.unit) - 1);

            const char* tgt_unit = rule["target_unit"] | r.unit;
            strncpy(r.target_unit, tgt_unit, sizeof(r.target_unit) - 1);

            strncpy(r.datatype, rule["datatype"] | "uint8", sizeof(r.datatype) - 1);
            strncpy(r.context, rule["context"] | "measurement", sizeof(r.context) - 1);
            
            if (rule.containsKey("val_min") && rule.containsKey("val_max")) {
                r.val_min = rule["val_min"].as<float>();
                r.val_max = rule["val_max"].as<float>();
                r.has_bounds = true;
            } else {
                r.val_min = 0.0f;
                r.val_max = 0.0f;
                r.has_bounds = false;
            }
            
            if (rule.containsKey("scale")) {
                r.scale = rule["scale"] | 1.0f;
            } else if (rule.containsKey("corr_factor")) {
                r.scale = rule["corr_factor"] | 1.0f;
            } else {
                r.scale = 1.0f;
            }

            if (rule.containsKey("target_type")) {
                strncpy(r.target_type, rule["target_type"], sizeof(r.target_type) - 1);
            } else if (strncmp(r.datatype, "bytes_", 6) == 0) {
                strncpy(r.target_type, "hex", sizeof(r.target_type) - 1);
            } else if (r.scale != 1.0f) {
                strncpy(r.target_type, "float", sizeof(r.target_type) - 1);
            } else {
                strncpy(r.target_type, "int", sizeof(r.target_type) - 1);
            }
        }
    }

    JsonObject commands = doc["commands"];
    if (commands) {
        for (JsonPair kv : commands) {
            if (dec.command_count >= MAX_LORA_COMMANDS_PER_DECODER) break;
            lora_command_t &cmd = dec.commands[dec.command_count++];
            memset(&cmd, 0, sizeof(lora_command_t));

            strncpy(cmd.key, kv.key().c_str(), sizeof(cmd.key) - 1);
            JsonObject cmd_obj = kv.value().as<JsonObject>();
            if (cmd_obj) {
                cmd.fport = cmd_obj["fport"] | 85;
                if (cmd_obj.containsKey("raw_hex")) {
                    strncpy(cmd.raw_hex, cmd_obj["raw_hex"] | "", sizeof(cmd.raw_hex) - 1);
                    cmd.is_dynamic = false;
                } else {
                    strncpy(cmd.prefix, cmd_obj["prefix"] | "", sizeof(cmd.prefix) - 1);
                    strncpy(cmd.datatype, cmd_obj["datatype"] | "uint16_le", sizeof(cmd.datatype) - 1);
                    strncpy(cmd.unit, cmd_obj["unit"] | "", sizeof(cmd.unit) - 1);
                    cmd.default_val = cmd_obj["default_val"] | 0;
                    cmd.is_dynamic = true;
                }
            }
        }
    }

    JsonArray reactions = doc["reactions"];
    if (reactions) {
        for (JsonObject react_item : reactions) {
            if (dec.reaction_count >= MAX_REACTIONS_PER_DECODER) break;
            lora_reaction_t &r = dec.reactions[dec.reaction_count++];
            memset(&r, 0, sizeof(lora_reaction_t));

            JsonObject trig = react_item["trigger"];
            if (trig) {
                r.trigger_fport = trig["fport"] | 0;
                r.trigger_prefix = parse_hex_u8(trig["raw_prefix"] | "0x00");
                r.trigger_encrypted = trig.containsKey("encrypted") ? trig["encrypted"].as<bool>() : true;
            }

            JsonObject act = react_item["action"];
            if (act) {
                strncpy(r.action_command, act["command"] | "", sizeof(r.action_command) - 1);
                r.target_fport = act["target_fport"] | 85;
                strncpy(r.timing_mode, act["timing_mode"] | "rx1_exact", sizeof(r.timing_mode) - 1);
                r.max_retries = act["max_retries"] | 3;
            }
        }
    }

    dec.valid = true;
    DBG_LORA_PRINTF("Cached Decoder '%s' (Format: %s, %u rules, %u commands, %u reactions loaded)",
                    dec.name, dec.format, dec.rule_count, dec.command_count, dec.reaction_count);

    return static_cast<int8_t>(g_decoder_count++);
}

/* ── Public API Implementation ─────────────────────────────────────────── */
void lorasensors_dispatch_local(PacketSerial &ps, uint32_t dev_addr, 
                                const char* key, float raw_val, const char* unit) {
    const lora_device_config_t *dev = lorasensors_find_device(dev_addr);
    if (!dev || !dev->enabled) return;

    // 1. Generic Offset Conversion from device configuration
    float scaled_val = raw_val;
    int dev_idx = dev - g_devices;
    if (dev_idx >= 0 && dev_idx < static_cast<int>(g_device_count)) {
        for (size_t o = 0; o < MAX_DEVICE_OFFSETS; o++) {
            if (g_device_offsets[dev_idx][o].valid && strcmp(g_device_offsets[dev_idx][o].key, key) == 0) {
                float converted_offset = units_convert(g_device_offsets[dev_idx][o].value, 
                                                       g_device_offsets[dev_idx][o].unit, unit);
                scaled_val += converted_offset;
                break;
            }
        }
    }

    // 2. Transmit local telemetry packet (0xBC) to ESP32
    uint8_t tx_buf[64];
    size_t idx = 0;
    tx_buf[idx++] = static_cast<uint8_t>(PKT_TYPE_LOCAL_TELEMETRY);
    memcpy(&tx_buf[idx], &dev_addr, 4); idx += 4;
    uint32_t fcnt = 0;
    memcpy(&tx_buf[idx], &fcnt, 4); idx += 4;
    tx_buf[idx++] = 0; // Float
    tx_buf[idx++] = 0; // Measurement context

    size_t key_len = strlen(key) + 1;
    memcpy(&tx_buf[idx], key, key_len); idx += key_len;
    size_t unit_len = strlen(unit) + 1;
    memcpy(&tx_buf[idx], unit, unit_len); idx += unit_len;
    memcpy(&tx_buf[idx], &scaled_val, sizeof(float)); idx += sizeof(float);

    uint8_t checksum = 0;
    for (size_t i = 0; i < idx; i++) checksum ^= tx_buf[i];
    tx_buf[idx++] = checksum;
    uart_link_send(tx_buf, idx);

    // 3. Store into LittleFS using device address and decoder name
    char dev_addr_str[16];
    snprintf(dev_addr_str, sizeof(dev_addr_str), "%08X", dev_addr);
    data_storage_add_point(dev_addr_str, dev->decoder, key, (uint32_t)time(NULL), scaled_val, unit);

    // 4. Feed into Virtual Decoder cascade (e.g., dew point)
    virtual_decoder_process_and_send(&ps, dev->device_name, dev_addr, 0, key, scaled_val, (uint32_t)time(NULL), unit);
}

void lora_telemetry_send_dynamic(PacketSerial& ps, uint32_t dev_addr, uint32_t fcnt, 
                                 uint8_t val_type, uint8_t context, 
                                 const char* key, const char* unit, 
                                 const void* val_data, size_t val_len)
{
    uint8_t tx_buf[256];
    size_t idx = 0;

    tx_buf[idx++] = static_cast<uint8_t>(PKT_TYPE_LORA_TELEMETRY);
    memcpy(&tx_buf[idx], &dev_addr, 4); idx += 4;
    memcpy(&tx_buf[idx], &fcnt, 4);     idx += 4;

    tx_buf[idx++] = val_type;
    tx_buf[idx++] = context;

    size_t key_len = strlen(key) + 1;
    if (idx + key_len > sizeof(tx_buf)) return;
    memcpy(&tx_buf[idx], key, key_len);
    idx += key_len;

    size_t unit_len = strlen(unit) + 1;
    if (idx + unit_len > sizeof(tx_buf)) return;
    memcpy(&tx_buf[idx], unit, unit_len);
    idx += unit_len;

    if (idx + val_len > sizeof(tx_buf)) return;
    memcpy(&tx_buf[idx], val_data, val_len);
    idx += val_len;

    uint8_t checksum = 0;
    for (size_t i = 0; i < idx; i++) {
        checksum ^= tx_buf[i];
    }
    tx_buf[idx++] = checksum;

    uart_link_send(tx_buf, idx);
}

void lorasensors_push_globals(PacketSerial &ps, bool valid_cfg) {
    if (!valid_cfg) {
        uint8_t no_cfg_buf[2] = { PKT_TYPE_PUSH_LORA_CFG, 0 };
        uart_link_send(no_cfg_buf, sizeof(no_cfg_buf));
        LogSerial.printf("RP2040: Pushed NO_CONFIG status to ESP32-S3\r\n");
        return;
    }

    /* Packet 0x35: Globals + Expected Device Count (~163 bytes) */
    uint8_t glob_buf[2 + sizeof(lora_global_config_t) + 1];
    glob_buf[0] = PKT_TYPE_PUSH_LORA_CFG;
    glob_buf[1] = 1;
    memcpy(&glob_buf[2], &g_global_cfg, sizeof(lora_global_config_t));
    glob_buf[2 + sizeof(lora_global_config_t)] = static_cast<uint8_t>(g_device_count);
    uart_link_send(glob_buf, sizeof(glob_buf));

    LogSerial.printf("RP2040: Pushed Globals (0x35) with %u devices announced\r\n", (unsigned)g_device_count);
}

bool lorasensors_stream_single_device(PacketSerial &ps, size_t dev_idx) {
    if (dev_idx >= g_device_count) {
        return false; /* All devices sent */
    }

    /* Packet 0x3F: Stream single device chunk */
    static_assert(3 + sizeof(lora_device_config_t) <= UART_LINK_MAX_PACKET,
                  "lora_device_config_t no longer fits into one UART link packet");
    uint8_t dev_buf[3 + sizeof(lora_device_config_t)];
    dev_buf[0] = PKT_TYPE_PUSH_LORA_DEV;
    dev_buf[1] = static_cast<uint8_t>(dev_idx);
    dev_buf[2] = static_cast<uint8_t>(g_device_count);
    memcpy(&dev_buf[3], &g_devices[dev_idx], sizeof(lora_device_config_t));
    uart_link_send(dev_buf, sizeof(dev_buf));

    LogSerial.printf("RP2040: Streamed Device [%u/%u]: '%s' (0x%08lX)\r\n",
                  (unsigned)(dev_idx + 1), (unsigned)g_device_count,
                  g_devices[dev_idx].device_name, (unsigned long)g_devices[dev_idx].dev_addr);

    return (dev_idx + 1 < g_device_count);
}

/**
 * @brief Resolves the engineering unit for a threshold key from virtual decoders or loaded decoder rules.
 * 
 * @param doc Device JSON document (used to inspect virtual_decoder definitions)
 * @param dec_idx Index of cached RAM decoder for this device (-1 if none)
 * @param key Telemetry key (e.g. "depth", "flowrate", "battery_soc")
 * @param is_percent True if threshold was defined with value_percent
 * @param out_unit Destination buffer for unit string
 * @param max_len Maximum length of out_unit buffer
 */
static void _resolve_threshold_unit(const JsonDocument &doc, int8_t dec_idx, const char *key, 
                                     bool is_percent, char *out_unit, size_t max_len) {
    if (!key || !out_unit || max_len == 0) return;

    /* 1. Percentage-based thresholds always use "%" */
    if (is_percent) {
        strncpy(out_unit, "%", max_len - 1);
        out_unit[max_len - 1] = '\0';
        return;
    }

    /* 2. Check virtual decoders defined in the same device JSON */
    if (doc.containsKey("virtual_decoder")) {
        JsonObjectConst v_dec = doc["virtual_decoder"];
        if (v_dec.containsKey(key)) {
            JsonObjectConst target_vd = v_dec[key];
            const char *vd_unit = target_vd["unit"] | "";
            if (vd_unit && vd_unit[0] != '\0') {
                strncpy(out_unit, vd_unit, max_len - 1);
                out_unit[max_len - 1] = '\0';
                return;
            }
        }
    }

    /* 3. Check loaded hardware decoder rules (mappings) */
    if (dec_idx >= 0 && dec_idx < static_cast<int8_t>(g_decoder_count)) {
        const lora_decoder_t &dec = g_decoders[dec_idx];
        for (uint8_t r = 0; r < dec.rule_count; r++) {
            if (strcmp(dec.rules[r].key, key) == 0) {
                const char *u = (strlen(dec.rules[r].target_unit) > 0) 
                                ? dec.rules[r].target_unit 
                                : dec.rules[r].unit;
                strncpy(out_unit, u, max_len - 1);
                out_unit[max_len - 1] = '\0';
                return;
            }
        }
    }

    /* 4. Fallback if no matching unit found */
    out_unit[0] = '\0';
}

bool lorasensors_load_all(SdFat32 &sd) {
    g_device_count = 0;
    g_decoder_count = 0;
    memset(g_device_decoder_map, -1, sizeof(g_device_decoder_map));
    memset(g_device_offsets, 0, sizeof(g_device_offsets));
    memset(g_device_limits, 0, sizeof(g_device_limits));

    // Initialize units database from SD with builtin fallback
    units_init("/config/units.json");
    
    // Clear virtual decoder outputs when reloading configs
    virtual_decoder_clear_outputs();

    /* 1a. Parse /config/globals.json (System Base & Language Config) */
    bool sys_glob_ok = false;
    File32 sys_file = sd.open("/config/globals.json", O_READ);
    if (sys_file) {
        static StaticJsonDocument<512> doc;
        doc.clear();
        DeserializationError err = deserializeJson(doc, sys_file);
        sys_file.close();

        if (!err) {
            sys_glob_ok = true;
            strncpy(g_global_cfg.time_cfg.language, doc["language"] | "de", sizeof(g_global_cfg.time_cfg.language) - 1);

            JsonObject tcfg = doc["time_config"];
            if (tcfg) {
                strncpy(g_global_cfg.time_cfg.posix_timezone, 
                        tcfg["posix_timezone"] | "CET-1CEST,M3.5.0,M10.5.0/3", 
                        sizeof(g_global_cfg.time_cfg.posix_timezone) - 1);
                /* Apply POSIX timezone to RP2040 C runtime library */
                setenv("TZ", g_global_cfg.time_cfg.posix_timezone, 1);
                tzset();
                
                strncpy(g_global_cfg.time_cfg.ntp_server, 
                        tcfg["ntp_server"] | "pool.ntp.org", 
                        sizeof(g_global_cfg.time_cfg.ntp_server) - 1);
                strncpy(g_global_cfg.time_cfg.time_format, 
                        tcfg["time_format"] | "24h", 
                        sizeof(g_global_cfg.time_cfg.time_format) - 1);
                g_global_cfg.time_cfg.p2p_sync_enabled = tcfg["p2p_time_sync_enabled"] | true;
            }

            LogSerial.printf("RP2040: Loaded /config/globals.json (Lang: %s, TZ: %s, NTP: %s)\r\n",
                          g_global_cfg.time_cfg.language,
                          g_global_cfg.time_cfg.posix_timezone,
                          g_global_cfg.time_cfg.ntp_server);
        } else {
            LogSerial.printf("RP2040 Error: Failed to parse /config/globals.json (%s)\r\n", err.c_str());
        }
    } else {
        LogSerial.println("RP2040 Warning: /config/globals.json not found, using system defaults.");
    }

    /* 1b. Parse /config/lora_globals.json (LoRa RF & Mesh Settings) */
    bool lora_glob_ok = false;
    File32 lora_file = sd.open("/config/lora_globals.json", O_READ);
    if (lora_file) {
        static StaticJsonDocument<768> doc;
        doc.clear();
        DeserializationError err = deserializeJson(doc, lora_file);
        lora_file.close();

        if (!err) {
            lora_glob_ok = true;
            g_global_cfg.rf_frequency_hz = doc["rf_frequency_hz"] | 868300000;
            g_global_cfg.rf_spreading_factor = doc["rf_spreading_factor"] | 10;
            g_global_cfg.rf_bandwidth_khz = doc["rf_bandwidth_khz"] | 125;
            g_global_cfg.rf_coding_rate = doc["rf_coding_rate"] | 5;
            g_global_cfg.rf_preamble_len = doc["rf_preamble_len"] | 8;

            const char* sync_str = doc["sync_word"] | "0x34";
            g_global_cfg.sync_word = static_cast<uint8_t>(parse_hex_u32(sync_str));

            JsonObject mesh = doc["p2p_mesh"];
            if (mesh) {
                g_global_cfg.mesh.enabled = mesh["enabled"] | true;
                strncpy(g_global_cfg.mesh.device_id, mesh["device_id"] | "AUTO", sizeof(g_global_cfg.mesh.device_id) - 1);
                
                const char* net_id_str = mesh["network_id"] | "0x4A6F";
                g_global_cfg.mesh.network_id = static_cast<uint16_t>(parse_hex_u32(net_id_str));
                
                const char* aes_key_str = mesh["aes_key"] | "4A6F696E5468654F70656E536F757263";
                parse_hex_bytes(aes_key_str, g_global_cfg.mesh.aes_key, 16);
                
                g_global_cfg.mesh.broadcast_ttl = mesh["broadcast_ttl"] | 2;
            }

            LogSerial.printf("RP2040: Loaded /config/lora_globals.json (Freq: %lu Hz, SF: %u, Sync: 0x%02X, Mesh: %s, NetID: 0x%04X)\r\n",
                          g_global_cfg.rf_frequency_hz,
                          g_global_cfg.rf_spreading_factor,
                          g_global_cfg.sync_word,
                          g_global_cfg.mesh.enabled ? "ACTIVE" : "DISABLED",
                          g_global_cfg.mesh.network_id);
        } else {
            LogSerial.printf("RP2040 Error: Failed to parse /config/lora_globals.json (%s)\r\n", err.c_str());
        }
    } else {
        LogSerial.println("RP2040 Info: /config/lora_globals.json not present (LoRa RF mesh disabled or defaults active).");
    }

    /* 2. Scan /devices/ directory for *.json files */
    File32 dev_dir = sd.open("/devices/");
    if (!dev_dir || !dev_dir.isDir()) {
        LogSerial.println("RP2040 Warning: Directory /devices/ missing!");
        return sys_glob_ok;
    }

    File32 entry;
    while (entry.openNext(&dev_dir, O_READ)) {
        char filename[64];
        entry.getName(filename, sizeof(filename));

        if (!entry.isDir() && strstr(filename, ".json") != NULL) {
            static StaticJsonDocument<4096> doc;
            doc.clear();
            DeserializationError err = deserializeJson(doc, entry);

            if (!err) {
                bool enabled = doc["enabled"] | false;
                if (enabled) {
                    uint32_t parsed_addr = parse_hex_u32(doc["dev_addr"] | "");
                    if (parsed_addr == 0) {
                        entry.close();
                        continue;
                    }

                    int target_idx = -1;
                    for (size_t i = 0; i < g_device_count; i++) {
                        if (g_devices[i].dev_addr == parsed_addr) {
                            target_idx = static_cast<int>(i);
                            break;
                        }
                    }

                    bool is_new_device = false;
                    if (target_idx < 0) {
                        if (g_device_count >= MAX_LORA_DEVICES) {
                            LogSerial.println("RP2040 Warning: Maximum device limit reached during merging!");
                            entry.close();
                            break;
                        }
                        target_idx = static_cast<int>(g_device_count);
                        is_new_device = true;
                    }

                    lora_device_config_t &dev = g_devices[target_idx];
                    if (is_new_device) {
                        memset(&dev, 0, sizeof(lora_device_config_t));
                    }

                    dev.enabled = true;
                    dev.dev_addr = parsed_addr;

                    if (doc.containsKey("device_name")) {
                        strncpy(dev.device_name, doc["device_name"] | "Unknown", sizeof(dev.device_name) - 1);
                    }
                    if (doc.containsKey("medium")) {
                        strncpy(dev.medium, doc["medium"] | "water", sizeof(dev.medium) - 1);
                    }
                    /* Configured transmit interval of the sensor. Basis for packet-loss detection
                     * (ESP32 watchdog) and the expected update interval of archived keys. */
                    {
                        uint32_t report_interval = doc["report_interval_sec"] | 300;
                        if (report_interval < 60 || report_interval > 86400) {
                            LogSerial.printf("RP2040 Warning: report_interval_sec=%lu out of range (60..86400), using 300\r\n",
                                             (unsigned long)report_interval);
                            report_interval = 300;
                        }
                        dev.report_interval_sec = report_interval;
                    }
                    if (doc.containsKey("dev_sn")) {
                        strncpy(dev.dev_sn, doc["dev_sn"] | "", sizeof(dev.dev_sn) - 1);
                    }
                    if (doc.containsKey("dev_eui")) {
                        strncpy(dev.dev_eui, doc["dev_eui"] | "", sizeof(dev.dev_eui) - 1);
                    }
                    if (doc.containsKey("decoder")) {
                        strncpy(dev.decoder, doc["decoder"] | "", sizeof(dev.decoder) - 1);
                    }
                    if (doc.containsKey("raw_offset_mm")) {
                        dev.raw_offset_mm = doc["raw_offset_mm"] | 0;
                    }

                    JsonObject keys = doc["keys"];
                    if (keys && keys.containsKey("app_skey")) {
                        const char* app_key_str = keys["app_skey"] | "";
                        if (strlen(app_key_str) >= 32) {
                            parse_hex_bytes(keys["nwk_skey"] | "", dev.nwk_skey, 16);
                            parse_hex_bytes(app_key_str, dev.app_skey, 16);
                        }
                    }

                    JsonObject geo = doc["tank_geometry"];
                    if (geo) {
                        const char* type_str = geo["type"] | "none";
                        if (strcmp(type_str, "rectangle") == 0) dev.geometry.type = TANK_TYPE_RECTANGLE;
                        else if (strcmp(type_str, "cylinder_vertical") == 0) dev.geometry.type = TANK_TYPE_CYLINDER_VERTICAL;
                        else if (strcmp(type_str, "cylinder_horizontal") == 0) dev.geometry.type = TANK_TYPE_CYLINDER_HORIZONTAL;
                        else if (strcmp(type_str, "cone_frustum") == 0) dev.geometry.type = TANK_TYPE_CONE_FRUSTUM;
                        else if (strcmp(type_str, "lut") == 0) dev.geometry.type = TANK_TYPE_LUT;
                        else dev.geometry.type = TANK_TYPE_NONE;

                        dev.geometry.max_depth_cm = geo["max_depth_cm"] | 0.0f;
                        dev.geometry.width_cm = geo["width_cm"] | 0.0f;
                        dev.geometry.length_cm = geo["length_cm"] | 0.0f;
                        dev.geometry.diameter_cm = geo["diameter_cm"] | 0.0f;
                        dev.geometry.dish_height_cm = geo["dish_height_cm"] | 0.0f;
                        dev.geometry.bottom_diameter_cm = geo["bottom_diameter_cm"] | 0.0f;
                        dev.geometry.top_diameter_cm = geo["top_diameter_cm"] | 0.0f;
                        dev.geometry.height_cm = geo["height_cm"] | 0.0f;
                        dev.geometry.cone_compensation = geo["cone_compensation"] | 1.0f;

                        JsonArray points = geo["points"];
                        if (points) {
                            dev.geometry.lut_count = 0;
                            for (JsonArray pt : points) {
                                if (dev.geometry.lut_count >= MAX_LUT_POINTS) break;
                                dev.geometry.lut[dev.geometry.lut_count].depth_cm = pt[0] | 0.0f;
                                dev.geometry.lut[dev.geometry.lut_count].liters = pt[1] | 0.0f;
                                dev.geometry.lut_count++;
                            }
                        }
                    }

                    JsonObject disp = doc["display_config"];
                    if (disp) {
                        strncpy(dev.display_cfg.unit_system, disp["unit_system"] | "metric", sizeof(dev.display_cfg.unit_system) - 1);
                        strncpy(dev.display_cfg.volume_unit, disp["volume_unit"] | "liters", sizeof(dev.display_cfg.volume_unit) - 1);
                    }

                    int8_t dec_idx = lorasensors_get_or_load_decoder(sd, dev.decoder);
                    g_device_decoder_map[target_idx] = dec_idx;

                    if (dec_idx >= 0 && dec_idx < static_cast<int8_t>(g_decoder_count)) {
                        dev.fport_cfg = g_decoders[dec_idx].fport_cfg;
                    }

                    memset(&dev.reaction, 0, sizeof(lora_device_reaction_t));
                    if (dec_idx >= 0 && dec_idx < static_cast<int8_t>(g_decoder_count)) {
                        const lora_decoder_t &dec = g_decoders[dec_idx];
                        if (dec.reaction_count > 0) {
                            const lora_reaction_t &r = dec.reactions[0];
                            dev.reaction.trigger_fport = r.trigger_fport;
                            dev.reaction.trigger_prefix = r.trigger_prefix;
                            dev.reaction.trigger_encrypted = r.trigger_encrypted;
                            dev.reaction.target_fport = r.target_fport;
                            dev.reaction.max_retries = r.max_retries;

                            const lora_command_t *cmd = lorasensors_find_command(dec.name, r.action_command);
                            if (cmd) {
                                uint8_t payload_buf[16];
                                size_t p_len = lorasensors_build_command_payload(cmd, cmd->default_val, payload_buf, sizeof(payload_buf));
                                if (p_len > 0) {
                                    dev.reaction.target_fport = cmd->fport;
                                    dev.reaction.payload_len = static_cast<uint8_t>(p_len);
                                    memcpy(dev.reaction.payload_bytes, payload_buf, p_len);
                                    dev.reaction.valid = true;
                                }
                            }
                        }
                    }

                    bool has_keys = (dev.app_skey[0] || dev.app_skey[1] || dev.app_skey[15]);
                    LogSerial.printf("RP2040: Loaded Device [%d]: '%s' (DevAddr: 0x%08X, Decoder: '%s' -> RAM Index: %d, HasKeys: %s)\r\n",
                                  target_idx, dev.device_name, dev.dev_addr, dev.decoder,
                                  g_device_decoder_map[target_idx], has_keys ? "YES" : "NO");
                    
                    /* Load offsets from device JSON */
                    dev.offset_count = 0;
                    JsonObject offsets = doc["offsets"];
                    if (offsets) {
                        size_t off_idx = 0;
                        for (JsonPair offset_pair : offsets) {
                            if (off_idx >= MAX_DEVICE_OFFSETS) break;
                            const char* off_key = offset_pair.key().c_str();
                            JsonObject off_cfg = offset_pair.value().as<JsonObject>();
                            if (off_cfg) {
                                float val = 0.0f;
                                if (off_cfg.containsKey("value")) val = off_cfg["value"].as<float>();
                                const char* u = off_cfg["unit"] | "";

                                strncpy(g_device_offsets[target_idx][off_idx].key, off_key, sizeof(g_device_offsets[target_idx][off_idx].key) - 1);
                                g_device_offsets[target_idx][off_idx].value = val;
                                strncpy(g_device_offsets[target_idx][off_idx].unit, u, sizeof(g_device_offsets[target_idx][off_idx].unit) - 1);
                                g_device_offsets[target_idx][off_idx].valid = true;

                                /* Mirror into packed device struct for ESP32 UART streaming (0x3F) */
                                lora_offset_config_t &dev_off = dev.offsets[dev.offset_count++];
                                memset(&dev_off, 0, sizeof(lora_offset_config_t));
                                strncpy(dev_off.key, off_key, sizeof(dev_off.key) - 1);
                                dev_off.value = val;
                                strncpy(dev_off.unit, u, sizeof(dev_off.unit) - 1);
                                dev_off.valid = true;

                                if (strcmp(off_key, "depth") == 0) {
                                    dev.raw_offset_mm = static_cast<int32_t>(units_convert(val, u, "mm"));
                                }
                                off_idx++;
                            }
                        }
                    }

                    /* Load thresholds from device JSON */
                    dev.threshold_count = 0;
                    JsonArray thresholds = doc["thresholds"];
                    if (thresholds) {
                        for (JsonObject t_item : thresholds) {
                            if (dev.threshold_count >= MAX_THRESHOLDS_PER_DEVICE) break;

                            /* Direction is mandatory ("falling" = below threshold, "rising" = above);
                             * it is never derived from the rule id */
                            const char* dir = t_item["direction"] | "";
                            uint8_t op;
                            if (strcasecmp(dir, "falling") == 0) {
                                op = 0; /* THRESHOLD_OP_LESS_THAN (<) */
                            } else if (strcasecmp(dir, "rising") == 0) {
                                op = 1; /* THRESHOLD_OP_GREATER_THAN (>) */
                            } else {
                                LogSerial.printf("[LORA] Dev 0x%08lX: threshold '%s' skipped, \"direction\" must be \"falling\" or \"rising\" (got \"%s\")\r\n",
                                                 (unsigned long)dev.dev_addr, (const char*)(t_item["id"] | "?"), dir);
                                continue;
                            }

                            lora_threshold_config_t &th = dev.thresholds[dev.threshold_count++];
                            memset(&th, 0, sizeof(lora_threshold_config_t));

                            strncpy(th.rule_id, t_item["id"] | "", sizeof(th.rule_id) - 1);
                            strncpy(th.key, t_item["key"] | "", sizeof(th.key) - 1);

                            /* Parse severity enum */
                            const char* sev = t_item["severity"] | "danger";
                            if (strcasecmp(sev, "warning") == 0) th.severity = 1;
                            else if (strcasecmp(sev, "info") == 0) th.severity = 2;
                            else th.severity = 0; /* ALARM_SEVERITY_DANGER */

                            /* Parse buzzer mode enum */
                            const char* bm = t_item["buzzer_mode"] | "intermittent";
                            if (strcasecmp(bm, "continuous") == 0) th.buzzer_mode = 1;
                            else if (strcasecmp(bm, "off") == 0) th.buzzer_mode = 2;
                            else th.buzzer_mode = 0; /* ALARM_BUZZER_INTERMITTENT */

                            th.damping = t_item["damping"] | 0;

                            /* Retain exact value and hysteresis format as defined in JSON (no premature scaling!) */
                            if (t_item.containsKey("value_percent")) {
                                th.is_percent = true;
                                th.threshold_val = t_item["value_percent"] | 0.0f;
                                th.hysteresis_val = t_item["hysteresis_percent"] | 2.0f;
                            } else {
                                th.is_percent = false;
                                th.threshold_val = t_item.containsKey("value_threshold") 
                                                   ? (t_item["value_threshold"] | 0.0f) 
                                                   : (t_item["value"] | 0.0f);
                                th.hysteresis_val = fabsf(t_item.containsKey("hysteresis") 
                                                          ? (t_item["hysteresis"] | 0.5f) 
                                                          : (t_item["hysteresis_percent"] | 0.5f));
                            }

                            /* Dynamically resolve engineering unit from virtual decoder or decoder mappings */
                            _resolve_threshold_unit(doc, dec_idx, th.key, th.is_percent, th.unit, sizeof(th.unit));

                            th.op = op;

                            th.valid = true;

                            DBG_LORA_PRINTF("Loaded Threshold: Dev=0x%08X '%s' Key='%s' Val=%.2f %s (is_pct=%d) Op=%s Damp=%u Buzz=%u",
                                           dev.dev_addr, th.rule_id, th.key, th.threshold_val, th.unit,
                                           th.is_percent ? 1 : 0, th.op == 0 ? "<" : ">", th.damping, th.buzzer_mode);
                        }
                    }

                    /* Load limits from device JSON */
                    dev.limit_count = 0;
                    JsonObject limits = doc["limits"];
                    if (limits) {
                        size_t lim_idx = 0;
                        for (JsonPair lim_pair : limits) {
                            if (lim_idx >= MAX_DEVICE_LIMITS) break;
                            const char* lim_key = lim_pair.key().c_str();
                            JsonObject lim_cfg = lim_pair.value().as<JsonObject>();
                            if (lim_cfg) {
                                device_limit_t &l = g_device_limits[target_idx][lim_idx];
                                strncpy(l.key, lim_key, sizeof(l.key) - 1);
                                l.val_min = lim_cfg["val_min"] | 0.0f;
                                l.val_max = lim_cfg["val_max"] | 0.0f;
                                l.cap_min = lim_cfg["cap_min"] | 0;
                                l.cap_max = lim_cfg["cap_max"] | 0;
                                l.valid = true;

                                /* Mirror into packed device struct for ESP32 UART streaming (0x3F) */
                                lora_limit_config_t &dev_lim = dev.limits[dev.limit_count++];
                                memset(&dev_lim, 0, sizeof(lora_limit_config_t));
                                strncpy(dev_lim.key, lim_key, sizeof(dev_lim.key) - 1);
                                dev_lim.val_min = l.val_min;
                                dev_lim.val_max = l.val_max;
                                dev_lim.cap_min = l.cap_min;
                                dev_lim.cap_max = l.cap_max;
                                dev_lim.valid = true;

                                lim_idx++;
                            }
                        }
                    }

                    /* Load virtual decoder configurations */
                    JsonObject virtual_decoders = doc["virtual_decoder"];
                    if (virtual_decoders) {
                        for (JsonPair vd_pair : virtual_decoders) {
                            const char* output_key = vd_pair.key().c_str();
                            JsonObject vd_config = vd_pair.value().as<JsonObject>();
                            if (!vd_config) continue;

                            virtual_decoder_config_t vd;
                            memset(&vd, 0, sizeof(vd));
                            strncpy(vd.output_key, output_key, sizeof(vd.output_key) - 1);
                            vd.dev_addr = dev.dev_addr; /* Virtual decoders are always scoped to their device */

                            const char* calc_type = vd_config["calculation"] | "";
                            const char* vd_type = vd_config["type"] | "";

                            // Support both single source_key and array source_keys
                            if (vd_config.containsKey("source_key")) {
                                strncpy(vd.source_keys[0], vd_config["source_key"] | "", sizeof(vd.source_keys[0]) - 1);
                                vd.source_key_count = 1;
                            } else if (vd_config.containsKey("source_keys")) {
                                JsonArray source_keys = vd_config["source_keys"];
                                if (source_keys) {
                                    vd.source_key_count = 0;
                                    for (JsonVariant sk : source_keys) {
                                        if (vd.source_key_count >= MAX_SOURCE_KEYS) break;
                                        const char* sk_str = sk.as<const char*>();
                                        if (sk_str && strlen(sk_str) > 0) {
                                            strncpy(vd.source_keys[vd.source_key_count], sk_str, sizeof(vd.source_keys[vd.source_key_count]) - 1);
                                            vd.source_key_count++;
                                        }
                                    }
                                }
                            }

                            vd.corr_factor = vd_config["corr_factor"] | 1.0f;
                            vd.interval_sec = vd_config["interval_sec"] | 0;

                            // Parse calculation & geometry types
                            if (strcmp(calc_type, "derivative") == 0) {
                                vd.calculation = CALC_DERIVATIVE;
                                strncpy(vd.unit, vd_config["unit"] | "", sizeof(vd.unit) - 1);
                            } else if (strcmp(calc_type, "dewpoint") == 0) {
                                vd.calculation = CALC_DEWPOINT;
                                strncpy(vd.unit, vd_config["unit"] | "°C", sizeof(vd.unit) - 1);
                            } else if (strcmp(calc_type, "sum") == 0) {
                                vd.calculation = CALC_SUM;
                                strncpy(vd.unit, vd_config["unit"] | "", sizeof(vd.unit) - 1);
                            } else if (strcmp(calc_type, "average") == 0) {
                                vd.calculation = CALC_AVERAGE;
                                strncpy(vd.unit, vd_config["unit"] | "", sizeof(vd.unit) - 1);
                            } else if (strcmp(calc_type, "subtract") == 0) {
                                vd.calculation = CALC_SUBTRACT;
                                strncpy(vd.unit, vd_config["unit"] | "", sizeof(vd.unit) - 1);
                            } else if (strcmp(calc_type, "multiply") == 0) {
                                vd.calculation = CALC_MULTIPLY;
                                strncpy(vd.unit, vd_config["unit"] | "", sizeof(vd.unit) - 1);
                            } else if (strcmp(calc_type, "divide") == 0) {
                                vd.calculation = CALC_DIVIDE;
                                strncpy(vd.unit, vd_config["unit"] | "", sizeof(vd.unit) - 1);
                            } else if (strcmp(vd_type, "rectangle") == 0 || strcmp(vd_type, "cylinder_vertical") == 0 ||
                                       strcmp(vd_type, "cylinder_horizontal") == 0 || strcmp(vd_type, "cone_frustum") == 0) {
                                vd.calculation = CALC_VOLUME;

                                // Unit hierarchy: 1. vd.unit -> 2. display_config.volume_unit -> 3. "m3"
                                if (vd_config.containsKey("unit") && strlen(vd_config["unit"] | "") > 0) {
                                    strncpy(vd.unit, vd_config["unit"], sizeof(vd.unit) - 1);
                                } else if (doc.containsKey("display_config") && doc["display_config"].containsKey("volume_unit")) {
                                    strncpy(vd.unit, doc["display_config"]["volume_unit"] | "m3", sizeof(vd.unit) - 1);
                                } else {
                                    strncpy(vd.unit, "m3", sizeof(vd.unit) - 1);
                                }

                                JsonObject geo = vd_config;
                                auto parse_dim = [&](const char* dim_name) -> float {
                                    if (!geo.containsKey(dim_name)) return 0.0f;
                                    if (geo[dim_name].is<JsonObject>()) {
                                        return units_convert(geo[dim_name]["value"] | 0.0f, geo[dim_name]["unit"] | "cm", "cm");
                                    }
                                    return geo[dim_name] | 0.0f;
                                };

                                if (strcmp(vd_type, "rectangle") == 0) {
                                    vd.geometry.type = GEO_RECTANGLE;
                                    vd.geometry.width_cm = parse_dim("width");
                                    vd.geometry.length_cm = parse_dim("length");
                                    vd.geometry.max_depth_cm = parse_dim("max_depth");
                                } else if (strcmp(vd_type, "cylinder_vertical") == 0) {
                                    vd.geometry.type = GEO_CYLINDER_VERTICAL;
                                    vd.geometry.diameter_cm = parse_dim("diameter");
                                    vd.geometry.max_depth_cm = parse_dim("max_depth");
                                } else if (strcmp(vd_type, "cylinder_horizontal") == 0) {
                                    vd.geometry.type = GEO_CYLINDER_HORIZONTAL;
                                    vd.geometry.diameter_cm = parse_dim("diameter");
                                    vd.geometry.length_cm = parse_dim("length");
                                    vd.geometry.max_depth_cm = parse_dim("max_depth");
                                    vd.geometry.dish_height_cm = parse_dim("dish_height");
                                } else if (strcmp(vd_type, "cone_frustum") == 0) {
                                    vd.geometry.type = GEO_CONE_FRUSTUM;
                                    vd.geometry.bottom_diameter_cm = parse_dim("bottom_diameter");
                                    vd.geometry.top_diameter_cm = parse_dim("top_diameter");
                                    vd.geometry.height_cm = parse_dim("height");
                                    vd.geometry.max_depth_cm = parse_dim("max_depth");
                                    vd.geometry.cone_compensation = geo["cone_compensation"] | 1.0f;
                                }
                                /* Parse volume capping mode: 0=Pass-Through, 1=Clamp, 2=Drop */
                                vd.geometry.cap_max = vd_config["cap_max"] | 0;
                            }

                            if (vd.calculation != CALC_NONE && vd.source_key_count > 0) {
                                virtual_decoder_register(&vd);
                                DBG_LORA_PRINTF("Registered virtual decoder '%s' (sources: %u, type: %d) for device '%s' (0x%08lX)",
                                                vd.output_key, vd.source_key_count, vd.calculation, dev.device_name,
                                                (unsigned long)dev.dev_addr);
                            }
                        }
                    }

                    if (is_new_device) {
                        g_device_count++;
                    }
                }
            } else {
                LogSerial.printf("RP2040 Error: Failed to parse /devices/%s (%s)\r\n", filename, err.c_str());
            }
        }
        entry.close();
    }
    dev_dir.close();

    LogSerial.printf("RP2040 Engine: Total active devices loaded: %u | Decoders cached in RAM: %u\r\n",
                  (unsigned)g_device_count, (unsigned)g_decoder_count);
    return sys_glob_ok;
}

const lora_global_config_t& lorasensors_get_globals(void) {
    return g_global_cfg;
}

const lora_device_config_t* lorasensors_find_device(uint32_t dev_addr) {
    for (size_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].enabled && g_devices[i].dev_addr == dev_addr) {
            return &g_devices[i];
        }
    }
    return NULL;
}

size_t lorasensors_get_device_count(void) {
    return g_device_count;
}

static void process_and_dispatch_rule(PacketSerial &ps, const lora_device_config_t *dev, 
                                       uint32_t fcnt, const decoder_rule_t *rule, 
                                       const uint8_t *raw_bytes, size_t available_bytes)
{
    uint8_t val_type = 0;
    uint8_t ctx_enum = 0;

    // Resolve context enum for telemetry packet
    if (strcmp(rule->context, "diagnostic") == 0) ctx_enum = 1;
    else if (strcmp(rule->context, "history") == 0) ctx_enum = 2;
    else ctx_enum = 0; // measurement

    // Handle raw byte arrays (e.g. Serial numbers, DevEUI)
    if (strncmp(rule->datatype, "bytes_", 6) == 0) {
        size_t n_bytes = atoi(&rule->datatype[6]);
        if (n_bytes == 0 || n_bytes > available_bytes) return;

        char hex_str[64] = {0};
        size_t out_pos = 0;
        for (size_t b = 0; b < n_bytes && (out_pos + 2) < sizeof(hex_str); b++) {
            out_pos += snprintf(&hex_str[out_pos], sizeof(hex_str) - out_pos, "%02X", raw_bytes[b]);
        }

        DBG_LORA_PRINTF(" -> [%s] %s: %s (Raw Hex)", rule->context, rule->key, hex_str);
        val_type = 3;
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, hex_str, strlen(hex_str) + 1);
        return;
    }

    double raw_num = 0.0;
    bool valid = false;

    if (strcmp(rule->datatype, "uint8") == 0 && available_bytes >= 1) {
        raw_num = raw_bytes[0];
        valid = true;
    } else if (strcmp(rule->datatype, "int8") == 0 && available_bytes >= 1) {
        raw_num = static_cast<int8_t>(raw_bytes[0]);
        valid = true;
    } else if (strcmp(rule->datatype, "uint16_le") == 0 && available_bytes >= 2) {
        uint16_t val = raw_bytes[0] | (raw_bytes[1] << 8);
        if (val != 0xFFFF && val != 0xFFFD) {
            raw_num = val;
            valid = true;
        } else {
            DBG_LORA_PRINTF(" -> [%s] %s: Sensor Error / Out of Range!", rule->context, rule->key);
            return;
        }
    } else if (strcmp(rule->datatype, "int16_le") == 0 && available_bytes >= 2) {
        raw_num = static_cast<int16_t>(raw_bytes[0] | (raw_bytes[1] << 8));
        valid = true;
    } else if (strcmp(rule->datatype, "uint32_le") == 0 && available_bytes >= 4) {
        uint32_t val = raw_bytes[0] | (raw_bytes[1] << 8) | (raw_bytes[2] << 16) | (raw_bytes[3] << 24);
        raw_num = val;
        valid = true;
    } else if (strcmp(rule->datatype, "int32_le") == 0 && available_bytes >= 4) {
        int32_t val = raw_bytes[0] | (raw_bytes[1] << 8) | (raw_bytes[2] << 16) | (raw_bytes[3] << 24);
        raw_num = val;
        valid = true;
    } else if (strcmp(rule->datatype, "float_le") == 0 && available_bytes >= 4) {
        float val;
        memcpy(&val, raw_bytes, sizeof(float));
        raw_num = val;
        valid = true;
    }

    if (!valid) return;

    // ── STEP 1: Driver Hardware Bounds (Stage 1 Gate) ───────────────────
    if (rule->has_bounds) {
        if (raw_num < rule->val_min || raw_num > rule->val_max) {
            DBG_LORA_PRINTF(" -> [%s] %s: Raw %.2f outside hardware bounds [%.2f..%.2f] - DROPPED",
                            rule->context, rule->key, raw_num, rule->val_min, rule->val_max);
            return;
        }
    }

    // ── STEP 2: Convert value to target_unit (rounded to 6 decimal places) ─
    double converted_val = raw_num;
    if (strlen(rule->unit) > 0 && strlen(rule->target_unit) > 0 && strcmp(rule->unit, rule->target_unit) != 0) {
        converted_val = round_6dp(units_convert(static_cast<float>(raw_num), rule->unit, rule->target_unit));
    }

    // ── STEP 3: Apply corr_factor (scale) ────────────────────────────────
    double scaled_val = round_6dp(converted_val * rule->scale);

    // ── STEP 4: Generic Offset Addition (in target_unit) ─────────────────
    int dev_idx = dev - g_devices;
    if (dev_idx >= 0 && dev_idx < static_cast<int>(g_device_count)) {
        for (size_t o = 0; o < MAX_DEVICE_OFFSETS; o++) {
            if (g_device_offsets[dev_idx][o].valid) {
                bool key_match = (strcmp(g_device_offsets[dev_idx][o].key, rule->key) == 0);
                if (!key_match && strcmp(g_device_offsets[dev_idx][o].key, "depth") == 0 && strcmp(rule->key, "history_depth") == 0) {
                    key_match = true;
                }
                if (key_match) {
                    float converted_offset = units_convert(g_device_offsets[dev_idx][o].value, 
                                                           g_device_offsets[dev_idx][o].unit, 
                                                           rule->target_unit);
                    scaled_val = round_6dp(scaled_val + converted_offset);
                    DBG_LORA_PRINTF(" -> [%s] %s: Raw=%.2f, Offset=%.2f %s (from %.2f %s), Scaled=%.2f %s",
                                    rule->context, rule->key, raw_num, 
                                    converted_offset, rule->target_unit,
                                    g_device_offsets[dev_idx][o].value, g_device_offsets[dev_idx][o].unit,
                                    scaled_val, rule->target_unit);
                    break;
                }
            }
        }
    }

    // ── STEP 5: Device Limits & Capping (Stage 2 Gate) ───────────────────
    if (dev_idx >= 0 && dev_idx < static_cast<int>(g_device_count)) {
        for (size_t l = 0; l < MAX_DEVICE_LIMITS; l++) {
            const device_limit_t &lim = g_device_limits[dev_idx][l];
            if (lim.valid && strcmp(lim.key, rule->key) == 0) {
                // Minimum bound check
                if (scaled_val < lim.val_min) {
                    if (lim.cap_min == 0) { // Drop
                        DBG_LORA_PRINTF(" -> [%s] %s: Value %.2f < val_min %.2f (cap_min=0) - DROPPED",
                                        rule->context, rule->key, scaled_val, lim.val_min);
                        return;
                    } else if (lim.cap_min == 1) { // Clamp
                        DBG_LORA_PRINTF(" -> [%s] %s: Value %.2f clamped to val_min %.2f",
                                        rule->context, rule->key, scaled_val, lim.val_min);
                        scaled_val = lim.val_min;
                    }
                    // cap_min == 2: Pass-Through (no action)
                }

                // Maximum bound check
                if (scaled_val > lim.val_max) {
                    if (lim.cap_max == 0) { // Drop
                        DBG_LORA_PRINTF(" -> [%s] %s: Value %.2f > val_max %.2f (cap_max=0) - DROPPED",
                                        rule->context, rule->key, scaled_val, lim.val_max);
                        return;
                    } else if (lim.cap_max == 1) { // Clamp
                        DBG_LORA_PRINTF(" -> [%s] %s: Value %.2f clamped to val_max %.2f",
                                        rule->context, rule->key, scaled_val, lim.val_max);
                        scaled_val = lim.val_max;
                    }
                    // cap_max == 2: Pass-Through (no action)
                }
                break;
            }
        }
    }

    // 3. Dispatch telemetry to ESP32 according to target datatype
    float numeric_for_storage = static_cast<float>(scaled_val);

    if (strcmp(rule->target_type, "float") == 0) {
        float f_val = static_cast<float>(scaled_val);
        char f_str[16];
        dtostrf(f_val, 4, 2, f_str);
        DBG_LORA_PRINTF(" -> [%s] %s: %s %s", rule->context, rule->key, f_str, rule->target_unit);
        val_type = 0;
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, &f_val, sizeof(float));

    } else if (strcmp(rule->target_type, "bool") == 0) {
        uint8_t b_val = (scaled_val != 0.0) ? 1 : 0;
        numeric_for_storage = static_cast<float>(b_val);
        DBG_LORA_PRINTF(" -> [%s] %s: %s", rule->context, rule->key, b_val ? "true" : "false");
        val_type = 2;
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, &b_val, sizeof(uint8_t));

    } else if (strcmp(rule->target_type, "hex") == 0 || strcmp(rule->target_type, "string") == 0) {
        char hex_buf[32];
        snprintf(hex_buf, sizeof(hex_buf), "0x%X", static_cast<unsigned int>(raw_num));
        DBG_LORA_PRINTF(" -> [%s] %s: %s", rule->context, rule->key, hex_buf);
        val_type = 3;
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, hex_buf, strlen(hex_buf) + 1);
        return;

    } else {
        int32_t i_val = static_cast<int32_t>(scaled_val);
        numeric_for_storage = static_cast<float>(i_val);
        DBG_LORA_PRINTF(" -> [%s] %s: %ld %s", rule->context, rule->key, (long)i_val, rule->target_unit);
        val_type = 1;
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, &i_val, sizeof(int32_t));
    }

    // 4. Archive numeric measurement and diagnostic keys (e.g. battery_soc).
    //    data_storage only keeps keys listed in the device's "archive" block.
    bool is_measurement = (strcmp(rule->context, "measurement") == 0);
    if (is_measurement || strcmp(rule->context, "diagnostic") == 0) {
        char dev_addr_str[16];
        snprintf(dev_addr_str, sizeof(dev_addr_str), "%08X", dev->dev_addr);

        data_storage_add_point(dev_addr_str, dev->decoder, rule->key,
                               (uint32_t)time(NULL), numeric_for_storage, rule->target_unit);
    }

    // 5. In the measurement trigger block, pass rule->target_unit to virtual decoders:
    if (is_measurement) {
        virtual_decoder_process_and_send(ps, dev->device_name, dev->dev_addr, fcnt,
                                         rule->key, numeric_for_storage, (uint32_t)time(NULL), rule->target_unit);
    }
}

void lorasensors_process_raw_frame(PacketSerial &ps, const uint8_t *buffer, size_t size) {
    if (size < 9) return;

    uint32_t dev_addr = 0;
    memcpy(&dev_addr, &buffer[1], 4);
    int16_t rssi = 0;
    memcpy(&rssi, &buffer[5], 2);
    int8_t snr = static_cast<int8_t>(buffer[7]);
    uint8_t phy_len = buffer[8];

    if (size < 9 + phy_len || phy_len < 13) return;
    const uint8_t *phy_payload = &buffer[9];

    uint32_t fcnt = phy_payload[6] | (phy_payload[7] << 8);
    uint8_t fport = phy_payload[8];

    const uint8_t *frm_payload = &phy_payload[9];
    size_t frm_len = phy_len - 9 - 4;

    int8_t dev_idx = -1;
    const lora_device_config_t *dev = NULL;
    for (size_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].enabled && g_devices[i].dev_addr == dev_addr) {
            dev = &g_devices[i];
            dev_idx = static_cast<int8_t>(i);
            break;
        }
    }

    if (!dev) {
        DBG_LORA_PRINTF("Frame from unknown DevAddr 0x%08X ignored.", dev_addr);
        return;
    }

    int8_t dec_idx = (dev_idx >= 0) ? g_device_decoder_map[dev_idx] : -1;
    if (dec_idx < 0 || dec_idx >= static_cast<int8_t>(g_decoder_count) || !g_decoders[dec_idx].valid) {
        DBG_LORA_PRINTF("Error: Cannot decode frame for '%s' (DevAddr: 0x%08X)! Decoder '%s' missing!",
                        dev->device_name, dev_addr, dev->decoder);
        return;
    }

    const lora_decoder_t &dec = g_decoders[dec_idx];

    DBG_LORA_PRINTF("Frame Header -> DevAddr: 0x%08X | FCnt: %lu | FPort: %u | RSSI: %d dBm | SNR: %d dB",
                    dev_addr, fcnt, fport, rssi, snr);

    if (dev->reaction.valid && fport == dev->reaction.trigger_fport) {
        if (frm_len > 0 && frm_payload[0] == dev->reaction.trigger_prefix) {
            DBG_LORA_PRINTF(" -> [REACTION TRIGGER] Device '%s' matched FPort %u", dev->device_name, dev->reaction.trigger_fport);
        }
    }

    if (strcmp(dec.format, "PACKED_BINARY") == 0) {
        for (uint8_t r = 0; r < dec.rule_count; r++) {
            const decoder_rule_t &rule = dec.rules[r];
            if (rule.offset < frm_len) {
                process_and_dispatch_rule(ps, dev, fcnt, &rule, 
                                           &frm_payload[rule.offset], frm_len - rule.offset);
            }
        }
        return;
    }

    size_t i = 0;
    if (dec.fport_cfg.valid && dec.fport_cfg.fport == fport && dec.fport_cfg.payload_offset < frm_len) {
        i = dec.fport_cfg.payload_offset;
    }

    while (i + 1 < frm_len) {
        uint8_t ch_id   = frm_payload[i++];
        uint8_t ch_type = frm_payload[i++];

        const decoder_rule_t *matched_rule = NULL;
        for (uint8_t r = 0; r < dec.rule_count; r++) {
            if ((dec.rules[r].fport == 0 || dec.rules[r].fport == fport) &&
                dec.rules[r].channel == ch_id && dec.rules[r].type == ch_type) {
                matched_rule = &dec.rules[r];
                break;
            }
        }

        if (matched_rule) {
            size_t avail = frm_len - i;
            process_and_dispatch_rule(ps, dev, fcnt, matched_rule, &frm_payload[i], avail);

            if (strncmp(matched_rule->datatype, "bytes_", 6) == 0) {
                i += atoi(&matched_rule->datatype[6]);
            } else if (strcmp(matched_rule->datatype, "uint8") == 0 || strcmp(matched_rule->datatype, "int8") == 0) {
                i += 1;
            } else if (strcmp(matched_rule->datatype, "uint16_le") == 0 || strcmp(matched_rule->datatype, "int16_le") == 0) {
                i += 2;
            } else if (strcmp(matched_rule->datatype, "uint32_le") == 0 || strcmp(matched_rule->datatype, "int32_le") == 0 || strcmp(matched_rule->datatype, "float_le") == 0) {
                i += 4;
            } else if (strcmp(matched_rule->datatype, "ipso_history") == 0 && i + 5 < frm_len) {
                i += 6;
            } else {
                break;
            }
        } else if (ch_id == 0xFF) {
            if (ch_type == 0x09 || ch_type == 0x0A) i += 2;
            else if (ch_type == 0x16) i += 8;
            else i++;
        } else {
            break;
        }
    }
}

bool lorasensors_save_offset_to_sd(SdFat32 &sd, uint32_t dev_addr, const char *key,
                                   float value, const char *unit) {
    if (!key || strlen(key) == 0) return false;

    /* 1. Locate and parse target device JSON file directly */
    File32 dev_dir = sd.open("/devices/");
    if (!dev_dir || !dev_dir.isDir()) {
        LogSerial.println("RP2040 Error: /devices/ directory missing on SD!");
        return false;
    }

    char target_path[128] = {0};
    DynamicJsonDocument doc(4096);
    bool found_and_parsed = false;

    File32 entry;
    while (entry.openNext(&dev_dir, O_READ)) {
        char filename[64];
        entry.getName(filename, sizeof(filename));

        if (!entry.isDir() && strstr(filename, ".json") != NULL) {
            doc.clear();
            DeserializationError err = deserializeJson(doc, entry);
            if (!err) {
                uint32_t file_dev_addr = parse_hex_u32(doc["dev_addr"] | "");
                if (file_dev_addr == dev_addr) {
                    snprintf(target_path, sizeof(target_path), "/devices/%s", filename);
                    found_and_parsed = true;
                    entry.close();
                    break;
                }
            }
        }
        entry.close();
    }
    dev_dir.close();

    if (!found_and_parsed) {
        LogSerial.printf("RP2040 Error: No JSON config found for DevAddr 0x%08lX\r\n", (unsigned long)dev_addr);
        return false;
    }

    /* 2. Locate or create target offset entry in offsets object */
    JsonObject offsets = doc["offsets"];
    if (!offsets) {
        offsets = doc.createNestedObject("offsets");
    }

    JsonObject target_off = offsets[key];
    if (!target_off) {
        target_off = offsets.createNestedObject(key);
    }

    target_off["value"] = value;
    if (unit && unit[0] != '\0') {
        target_off["unit"] = unit;
    }

    /* 3. Atomically write back updated JSON to SD card */
    File32 file = sd.open(target_path, O_WRITE | O_CREAT | O_TRUNC);
    if (!file) {
        LogSerial.printf("RP2040 Error: Cannot open %s for writing\r\n", target_path);
        return false;
    }

    serializeJsonPretty(doc, file);
    file.close();
    LogSerial.printf("RP2040: Successfully patched offset '%s' = %.2f %s in %s\r\n",
                  key, value, unit ? unit : "", target_path);

    /* 4. Synchronize in-memory RAM structures immediately */
    for (size_t d = 0; d < g_device_count; d++) {
        if (g_devices[d].dev_addr == dev_addr) {
            /* Update local processing offset table */
            bool updated_local = false;
            for (size_t o = 0; o < MAX_DEVICE_OFFSETS; o++) {
                if (g_device_offsets[d][o].valid && strcmp(g_device_offsets[d][o].key, key) == 0) {
                    g_device_offsets[d][o].value = value;
                    if (unit && unit[0] != '\0') {
                        strncpy(g_device_offsets[d][o].unit, unit, sizeof(g_device_offsets[d][o].unit) - 1);
                    }
                    updated_local = true;
                    break;
                }
            }
            if (!updated_local) {
                for (size_t o = 0; o < MAX_DEVICE_OFFSETS; o++) {
                    if (!g_device_offsets[d][o].valid) {
                        strncpy(g_device_offsets[d][o].key, key, sizeof(g_device_offsets[d][o].key) - 1);
                        g_device_offsets[d][o].value = value;
                        strncpy(g_device_offsets[d][o].unit, unit ? unit : "", sizeof(g_device_offsets[d][o].unit) - 1);
                        g_device_offsets[d][o].valid = true;
                        break;
                    }
                }
            }

            /* Update streamed device struct */
            for (uint8_t o = 0; o < g_devices[d].offset_count; o++) {
                if (strcmp(g_devices[d].offsets[o].key, key) == 0) {
                    g_devices[d].offsets[o].value = value;
                    if (unit && unit[0] != '\0') {
                        strncpy(g_devices[d].offsets[o].unit, unit, sizeof(g_devices[d].offsets[o].unit) - 1);
                    }
                    break;
                }
            }

            if (strcmp(key, "depth") == 0) {
                const char *u = (unit && unit[0] != '\0') ? unit : "mm";
                g_devices[d].raw_offset_mm = static_cast<int32_t>(units_convert(value, u, "mm"));
            }
            break;
        }
    }

    return true;
}

bool lorasensors_save_threshold_to_sd(SdFat32 &sd, uint32_t dev_addr, const char *rule_id,
                                      bool is_percent, float threshold_val, float hysteresis_val,
                                      uint8_t damping, uint8_t severity, uint8_t buzzer_mode) {
    if (!rule_id || strlen(rule_id) == 0) return false;

    /* 1. Locate and parse target device JSON file directly */
    File32 dev_dir = sd.open("/devices/");
    if (!dev_dir || !dev_dir.isDir()) {
        LogSerial.println("RP2040 Error: /devices/ directory missing on SD!");
        return false;
    }

    char target_path[128] = {0};
    DynamicJsonDocument doc(4096);
    bool found_and_parsed = false;

    File32 entry;
    while (entry.openNext(&dev_dir, O_READ)) {
        char filename[64];
        entry.getName(filename, sizeof(filename));

        if (!entry.isDir() && strstr(filename, ".json") != NULL) {
            doc.clear();
            DeserializationError err = deserializeJson(doc, entry);
            if (!err) {
                uint32_t file_dev_addr = parse_hex_u32(doc["dev_addr"] | "");
                if (file_dev_addr == dev_addr) {
                    snprintf(target_path, sizeof(target_path), "/devices/%s", filename);
                    found_and_parsed = true;
                    entry.close();
                    break;
                }
            } else {
                LogSerial.printf("RP2040 Warning: Skipping /devices/%s: JSON parse failed (%s)\r\n", 
                              filename, err.c_str());
            }
        }
        entry.close();
    }
    dev_dir.close();

    if (!found_and_parsed) {
        LogSerial.printf("RP2040 Error: No JSON config found for DevAddr 0x%08lX\r\n", (unsigned long)dev_addr);
        return false;
    }

    /* 2. Locate and update target threshold entry in thresholds array */
    JsonArray thresholds = doc["thresholds"];
    if (!thresholds) {
        LogSerial.printf("RP2040 Error: %s has no 'thresholds' array\r\n", target_path);
        return false;
    }

    bool rule_found = false;
    for (JsonObject t_obj : thresholds) {
        const char *id_str = t_obj["id"] | "";
        if (strcmp(id_str, rule_id) == 0) {
            rule_found = true;

            /* Map severity enum */
            if (severity == 1) t_obj["severity"] = "warning";
            else if (severity == 2) t_obj["severity"] = "info";
            else t_obj["severity"] = "danger";

            /* Map buzzer mode enum */
            if (buzzer_mode == 1) t_obj["buzzer_mode"] = "continuous";
            else if (buzzer_mode == 2) t_obj["buzzer_mode"] = "off";
            else t_obj["buzzer_mode"] = "intermittent";

            t_obj["damping"] = damping;

            /* Retain exact JSON field format without loss */
            if (is_percent) {
                t_obj["value_percent"] = threshold_val;
                t_obj["hysteresis_percent"] = hysteresis_val;
                t_obj.remove("value_threshold");
                t_obj.remove("value");
                t_obj.remove("hysteresis");
            } else {
                /* Preserve original naming: 'value' if used previously, else 'value_threshold' */
                if (t_obj.containsKey("value")) {
                    t_obj["value"] = threshold_val;
                } else {
                    t_obj["value_threshold"] = threshold_val;
                }
                t_obj["hysteresis"] = hysteresis_val;
                t_obj.remove("value_percent");
                t_obj.remove("hysteresis_percent");
            }
            break;
        }
    }

    if (!rule_found) {
        LogSerial.printf("RP2040 Error: Rule '%s' not found in %s\r\n", rule_id, target_path);
        return false;
    }

    /* 3. Atomically write back updated JSON to SD card */
    File32 file = sd.open(target_path, O_WRITE | O_CREAT | O_TRUNC);
    if (!file) {
        LogSerial.printf("RP2040 Error: Cannot open %s for writing\r\n", target_path);
        return false;
    }

    serializeJsonPretty(doc, file);
    file.close();
    LogSerial.printf("RP2040: Successfully patched threshold '%s' in %s\r\n", rule_id, target_path);

    /* 4. Synchronize in-memory RAM structure immediately */
    for (size_t d = 0; d < g_device_count; d++) {
        if (g_devices[d].dev_addr == dev_addr) {
            for (uint8_t t = 0; t < g_devices[d].threshold_count; t++) {
                lora_threshold_config_t &th = g_devices[d].thresholds[t];
                if (strcmp(th.rule_id, rule_id) == 0) {
                    th.is_percent = is_percent;
                    th.threshold_val = threshold_val;
                    th.hysteresis_val = hysteresis_val;
                    th.damping = damping;
                    th.severity = severity;
                    th.buzzer_mode = buzzer_mode;

                    if (is_percent) {
                        strncpy(th.unit, "%", sizeof(th.unit) - 1);
                    }
                    /* th.op stays as loaded from "direction" (not editable in the GUI, never derived from the id) */
                    break;
                }
            }
            break;
        }
    }

    return true;
}