/**
 * @file lorasensors.cpp
 * @brief Parsing engine for LoRa P2P global settings, multi-device JSON configs,
 *        and case-insensitive RAM-cached IPSO decoders.
 */

#include "lorasensors.hpp"
#include "debug_log.hpp"

/* ── Private Driver State (RAM Caches) ──────────────────────────────────── */

static lora_global_config_t g_global_cfg = {
    .rf_frequency_hz = 868300000,
    .rf_spreading_factor = 10,
    .rf_bandwidth_khz = 125,
    .rf_coding_rate = 5,
    .rf_preamble_len = 8,
    .sync_word = 0x12
};

static lora_device_config_t g_devices[MAX_LORA_DEVICES];
static int8_t               g_device_decoder_map[MAX_LORA_DEVICES];
static size_t               g_device_count = 0;

static lora_decoder_t       g_decoders[MAX_LORA_DECODERS];
static size_t               g_decoder_count = 0;

/* ── Private Parsing Helpers ────────────────────────────────────────────── */

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

/* ── Command Lookup & Payload Builder Implementation ──────────────────── */

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
        /* Static command from raw_hex */
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
        /* Dynamic command: prefix + encoded value */
        size_t prefix_len = strlen(cmd->prefix);
        if (prefix_len == 0 || prefix_len % 2 != 0) return 0;
        size_t prefix_bytes = prefix_len / 2;

        size_t val_bytes = 0;
        if (strcmp(cmd->datatype, "uint8") == 0) val_bytes = 1;
        else if (strcmp(cmd->datatype, "uint16_le") == 0) val_bytes = 2;
        else if (strcmp(cmd->datatype, "uint32_le") == 0) val_bytes = 4;
        else val_bytes = 2; /* Default uint16_le */

        if (prefix_bytes + val_bytes > max_len) return 0;

        /* Write prefix bytes */
        for (size_t i = 0; i < prefix_bytes; i++) {
            char buf[3] = { cmd->prefix[i * 2], cmd->prefix[i * 2 + 1], '\0' };
            out_bytes[i] = static_cast<uint8_t>(strtoul(buf, NULL, 16));
        }

        /* Write value bytes (little endian) */
        for (size_t b = 0; b < val_bytes; b++) {
            out_bytes[prefix_bytes + b] = static_cast<uint8_t>((val >> (b * 8)) & 0xFF);
        }

        return prefix_bytes + val_bytes;
    }
}

/* ── Case-Insensitive SD Decoder Resolver & RAM Cacher ─────────────────── */

static int8_t lorasensors_get_or_load_decoder(SdFat32 &sd, const char* decoder_name) {
    if (!decoder_name || strlen(decoder_name) == 0) return -1;

    /* 1. Check if already cached in RAM (case-insensitive) */
    for (size_t i = 0; i < g_decoder_count; i++) {
        if (strcasecmp(g_decoders[i].name, decoder_name) == 0) {
            return static_cast<int8_t>(i);
        }
    }

    if (g_decoder_count >= MAX_LORA_DECODERS) {
        Serial.println("RP2040 Error: Maximum RAM decoder limit reached!");
        return -1;
    }

    /* 2. Resolve decoder file path on SD card (with case-insensitive fallback) */
    char target_filename[64];
    snprintf(target_filename, sizeof(target_filename), "%s.json", decoder_name);

    char full_path[100];
    snprintf(full_path, sizeof(full_path), "/lora_p2p_configs/decoder/%s", target_filename);

    File32 file = sd.open(full_path, O_READ);

    /* Case-insensitive fallback scan if direct open failed */
    if (!file) {
        File32 dec_dir = sd.open("/lora_p2p_configs/decoder/");
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
        Serial.printf("RP2040 Error: Cannot decode! Decoder file '/lora_p2p_configs/decoder/%s.json' missing on SD card!\r\n",
                      decoder_name);
        return -1;
    }

    static StaticJsonDocument<3072> doc;
    doc.clear();
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (err) {
        Serial.printf("RP2040 Error: Failed to parse '/lora_p2p_configs/decoder/%s.json' (%s)\r\n",
                      decoder_name, err.c_str());
        return -1;
    }

    /* 3. Cache Decoder into RAM */
    lora_decoder_t &dec = g_decoders[g_decoder_count];
    memset(&dec, 0, sizeof(lora_decoder_t));
    strncpy(dec.name, decoder_name, sizeof(dec.name) - 1);
    strncpy(dec.format, doc["format"] | "IPSO_TLV", sizeof(dec.format) - 1);

    /* Parse FPort Config */
    JsonArray fport_cfgs = doc["fport_config"];
    if (fport_cfgs && fport_cfgs.size() > 0) {
        JsonObject cfg = fport_cfgs[0];
        dec.fport_cfg.fport = cfg["fport"] | 0;
        
        const char* enc_str = cfg["encryption"] | "full";
        if (strcmp(enc_str, "none") == 0) dec.fport_cfg.encryption_mode = 1;
        else if (strcmp(enc_str, "partial") == 0) dec.fport_cfg.encryption_mode = 2;
        else dec.fport_cfg.encryption_mode = 0; /* full */

        dec.fport_cfg.cipher_offset = cfg["cipher_offset"] | 0;
        
        const char* key_str = cfg["key"] | "app_skey";
        dec.fport_cfg.key_type = (strcmp(key_str, "nwk_skey") == 0) ? 1 : 0;
        
        dec.fport_cfg.payload_offset = cfg["payload_offset"] | 0;
        dec.fport_cfg.valid = true;
    }

    /* Parse Mappings */
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

            /* Unit Fallback: Use target_unit if specified, otherwise fall back to unit */
            const char* tgt_unit = rule["target_unit"] | r.unit;
            strncpy(r.target_unit, tgt_unit, sizeof(r.target_unit) - 1);

            strncpy(r.datatype, rule["datatype"] | "uint8", sizeof(r.datatype) - 1);
            strncpy(r.context, rule["context"] | "measurement", sizeof(r.context) - 1);
            r.scale   = rule["scale"] | 1.0f;

            /* Type Fallback Rules */
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

    /* Parse Commands */
    JsonObject commands = doc["commands"];
    if (commands) {
        for (JsonPair kv : commands) {
            if (dec.command_count >= MAX_LORA_COMMANDS_PER_DECODER) break;
            lora_command_t &cmd = dec.commands[dec.command_count++];
            memset(&cmd, 0, sizeof(lora_command_t));

            strncpy(cmd.key, kv.key().c_str(), sizeof(cmd.key) - 1);
            JsonObject cmd_obj = kv.value().as<JsonObject>();
            if (cmd_obj) {
                cmd.fport = cmd_obj["fport"] | 85; /* Default fallback FPort 85 */
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

    /* Parse Reactions */
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

void lora_telemetry_send_dynamic(PacketSerial& ps, uint32_t dev_addr, uint32_t fcnt, 
                                 uint8_t val_type, uint8_t context, 
                                 const char* key, const char* unit, 
                                 const void* val_data, size_t val_len)
{
    uint8_t tx_buf[256];
    size_t idx = 0;

    tx_buf[idx++] = static_cast<uint8_t>(PKT_TYPE_LORA_TELEMETRY); /* 0xBB */
    
    memcpy(&tx_buf[idx], &dev_addr, 4); idx += 4;
    memcpy(&tx_buf[idx], &fcnt, 4);     idx += 4;

    tx_buf[idx++] = val_type;  /* 0=Float, 1=Int32, 2=Bool, 3=String/Hex */
    tx_buf[idx++] = context;   /* 0=Measurement, 1=Diagnostic, 2=History */

    /* Null-terminated key string */
    size_t key_len = strlen(key) + 1;
    if (idx + key_len > sizeof(tx_buf)) return;
    memcpy(&tx_buf[idx], key, key_len);
    idx += key_len;

    /* Null-terminated unit string */
    size_t unit_len = strlen(unit) + 1;
    if (idx + unit_len > sizeof(tx_buf)) return;
    memcpy(&tx_buf[idx], unit, unit_len);
    idx += unit_len;

    /* Variable value payload */
    if (idx + val_len > sizeof(tx_buf)) return;
    memcpy(&tx_buf[idx], val_data, val_len);
    idx += val_len;

    /* Append XOR Checksum over payload */
    uint8_t checksum = 0;
    for (size_t i = 0; i < idx; i++) {
        checksum ^= tx_buf[i];
    }
    tx_buf[idx++] = checksum;

    ps.send(tx_buf, idx);
}

void lorasensors_push_config_to_esp32(PacketSerial &ps, bool valid_cfg) {
    static uint8_t tx_buf[2048];
    size_t idx = 0;

    tx_buf[idx++] = 0x35; /* PKT_TYPE_PUSH_LORA_CFG */
    tx_buf[idx++] = valid_cfg ? 1 : 0;

    if (valid_cfg) {
        memcpy(&tx_buf[idx], &g_global_cfg, sizeof(lora_global_config_t));
        idx += sizeof(lora_global_config_t);

        tx_buf[idx++] = static_cast<uint8_t>(g_device_count);

        for (size_t i = 0; i < g_device_count; i++) {
            if (idx + sizeof(lora_device_config_t) > sizeof(tx_buf)) break;
            memcpy(&tx_buf[idx], &g_devices[i], sizeof(lora_device_config_t));
            idx += sizeof(lora_device_config_t);
        }
        Serial.printf("RP2040: Pushed valid LoRa config & %u devices to ESP32-S3\r\n", (unsigned)g_device_count);
    } else {
        Serial.printf("RP2040: Pushed NO_CONFIG status to ESP32-S3\r\n");
    }

    ps.send(tx_buf, idx);
}

bool lorasensors_load_all(SdFat32 &sd) {
    g_device_count = 0;
    g_decoder_count = 0;
    memset(g_device_decoder_map, -1, sizeof(g_device_decoder_map));

    /* 1. Parse /lora_p2p_configs/globals.json */
    File32 glob_file = sd.open("/lora_p2p_configs/globals.json", O_READ);
    if (glob_file) {
        static StaticJsonDocument<768> doc;
        doc.clear();
        DeserializationError err = deserializeJson(doc, glob_file);
        glob_file.close();

        if (!err) {
            g_global_cfg.rf_frequency_hz = doc["rf_frequency_hz"] | 868300000;
            g_global_cfg.rf_spreading_factor = doc["rf_spreading_factor"] | 10;
            g_global_cfg.rf_bandwidth_khz = doc["rf_bandwidth_khz"] | 125;
            g_global_cfg.rf_coding_rate = doc["rf_coding_rate"] | 5;
            g_global_cfg.rf_preamble_len = doc["rf_preamble_len"] | 8;

            const char* sync_str = doc["sync_word"] | "0x34";
            g_global_cfg.sync_word = static_cast<uint8_t>(parse_hex_u32(sync_str));

            /* Parse p2p_mesh configuration block */
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

            Serial.printf("RP2040: Loaded globals.json (Freq: %lu Hz, SF: %u, Sync: 0x%02X, Mesh: %s, NetID: 0x%04X, TTL: %u)\r\n",
                          g_global_cfg.rf_frequency_hz,
                          g_global_cfg.rf_spreading_factor,
                          g_global_cfg.sync_word,
                          g_global_cfg.mesh.enabled ? "ACTIVE" : "DISABLED",
                          g_global_cfg.mesh.network_id,
                          g_global_cfg.mesh.broadcast_ttl);
        } else {
            Serial.printf("RP2040 Error: Failed to parse globals.json (%s)\r\n", err.c_str());
        }
    } else {
        Serial.println("RP2040 Warning: /lora_p2p_configs/globals.json not found, using defaults.");
    }

    /* 2. Scan /lora_p2p_configs/devices/ directory for *.json files */
    File32 dev_dir = sd.open("/lora_p2p_configs/devices/");
    if (!dev_dir || !dev_dir.isDir()) {
        Serial.println("RP2040 Warning: Directory /lora_p2p_configs/devices/ missing!");
        return (glob_file != 0);
    }

    File32 entry;
    while (entry.openNext(&dev_dir, O_READ)) {
        char filename[64];
        entry.getName(filename, sizeof(filename));

        if (!entry.isDir() && strstr(filename, ".json") != NULL) {
            static StaticJsonDocument<1024> doc;
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

                    /* Device Merging Check: See if dev_addr already exists in RAM */
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
                            Serial.println("RP2040 Warning: Maximum device limit reached during merging!");
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

                    /* Only parse keys if app_skey is present and non-empty (32 hex characters) */
                    JsonObject keys = doc["keys"];
                    if (keys && keys.containsKey("app_skey")) {
                        const char* app_key_str = keys["app_skey"] | "";
                        if (strlen(app_key_str) >= 32) {
                            parse_hex_bytes(keys["nwk_skey"] | "", dev.nwk_skey, 16);
                            parse_hex_bytes(app_key_str, dev.app_skey, 16);

                            DBG_LORA_PRINTF("  -> Device '%s' Keys Updated: AppSKey=%02X%02X%02X%02X... NwkSKey=%02X%02X%02X%02X...",
                                            dev.device_name,
                                            dev.app_skey[0], dev.app_skey[1], dev.app_skey[2], dev.app_skey[3],
                                            dev.nwk_skey[0], dev.nwk_skey[1], dev.nwk_skey[2], dev.nwk_skey[3]);
                        }
                    }

                    /* Dynamically load & cache Decoder from SD into RAM */
                    int8_t dec_idx = lorasensors_get_or_load_decoder(sd, dev.decoder);
                    g_device_decoder_map[target_idx] = dec_idx;

                    /* Mirror FPort config rule to device struct for ESP32 sync */
                    if (dec_idx >= 0 && dec_idx < static_cast<int8_t>(g_decoder_count)) {
                        dev.fport_cfg = g_decoders[dec_idx].fport_cfg;
                    }

                    /* Resolve active reaction rule for ESP32 sync if available */
                    memset(&dev.reaction, 0, sizeof(lora_device_reaction_t));
                    if (dec_idx >= 0 && dec_idx < static_cast<int8_t>(g_decoder_count)) {
                        const lora_decoder_t &dec = g_decoders[dec_idx];
                        if (dec.reaction_count > 0) {
                            const lora_reaction_t &r = dec.reactions[0]; /* Primary reaction rule */
                            dev.reaction.trigger_fport = r.trigger_fport;
                            dev.reaction.trigger_prefix = r.trigger_prefix;
                            dev.reaction.trigger_encrypted = r.trigger_encrypted;
                            dev.reaction.target_fport = r.target_fport;
                            dev.reaction.max_retries = r.max_retries;

                            /* Resolve target action command */
                            const lora_command_t *cmd = lorasensors_find_command(dec.name, r.action_command);
                            if (cmd) {
                                uint8_t payload_buf[16];
                                size_t p_len = lorasensors_build_command_payload(cmd, cmd->default_val, payload_buf, sizeof(payload_buf));
                                if (p_len > 0) {
                                    dev.reaction.target_fport = cmd->fport; /* Dynamically override target FPort */
                                    dev.reaction.payload_len = static_cast<uint8_t>(p_len);
                                    memcpy(dev.reaction.payload_bytes, payload_buf, p_len);
                                    dev.reaction.valid = true;
                                }
                            }
                        }
                    }

                    bool has_keys = (dev.app_skey[0] || dev.app_skey[1] || dev.app_skey[15]);
                    Serial.printf("RP2040: Loaded/Merged Device [%d]: '%s' (DevAddr: 0x%08X, Decoder: '%s' -> RAM Index: %d, Reaction: %s, HasKeys: %s)\r\n",
                                  target_idx, dev.device_name, dev.dev_addr, dev.decoder,
                                  g_device_decoder_map[target_idx], dev.reaction.valid ? "ACTIVE" : "NONE",
                                  has_keys ? "YES" : "NO");

                    if (is_new_device) {
                        g_device_count++;
                    }
                }
            } else {
                Serial.printf("RP2040 Error: Failed to parse %s (%s)\r\n", filename, err.c_str());
            }
        }
        entry.close();
    }
    dev_dir.close();

    Serial.printf("RP2040 Engine: Total active devices loaded: %u | Decoders cached in RAM: %u\r\n",
                  (unsigned)g_device_count, (unsigned)g_decoder_count);
    return true;
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

/* ── Zero-SD-IO Frame Processor (Pure RAM Execution) ──────────────────── */

/* Helper to evaluate a matched rule and dispatch dynamic UART 0xBB frame */
static void process_and_dispatch_rule(PacketSerial &ps, const lora_device_config_t *dev, 
                                       uint32_t fcnt, const decoder_rule_t *rule, 
                                       const uint8_t *raw_bytes, size_t available_bytes)
{
    uint8_t  val_type = 0; /* 0=Float, 1=Int32, 2=Bool, 3=String/Hex */
    uint8_t  ctx_enum = 0; /* 0=Measurement, 1=Diagnostic, 2=History */

    if (strcmp(rule->context, "diagnostic") == 0) ctx_enum = 1;
    else if (strcmp(rule->context, "history") == 0) ctx_enum = 2;

    /* Handle raw byte array / Hex conversion (bytes_N) */
    if (strncmp(rule->datatype, "bytes_", 6) == 0) {
        size_t n_bytes = atoi(&rule->datatype[6]);
        if (n_bytes == 0 || n_bytes > available_bytes) return;

        char hex_str[64] = {0};
        size_t out_pos = 0;
        for (size_t b = 0; b < n_bytes && (out_pos + 2) < sizeof(hex_str); b++) {
            out_pos += snprintf(&hex_str[out_pos], sizeof(hex_str) - out_pos, "%02X", raw_bytes[b]);
        }

        DBG_LORA_PRINTF(" -> [%s] %s: %s (Raw Hex)", rule->context, rule->key, hex_str);
        
        val_type = 3; /* String/Hex */
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, hex_str, strlen(hex_str) + 1);
        return;
    }

    /* Extract numerical values based on datatype */
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

    /* Calculate scaled final value and apply raw_offset_mm for depth if applicable */
    double scaled_val = raw_num * rule->scale;
    if (strcmp(rule->key, "depth") == 0) {
        scaled_val += (dev->raw_offset_mm / 10.0);
    }

    /* Output log and select target UART type */
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
        DBG_LORA_PRINTF(" -> [%s] %s: %s", rule->context, rule->key, b_val ? "true" : "false");
        val_type = 2; /* Bool */
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, &b_val, sizeof(uint8_t));

    } else if (strcmp(rule->target_type, "hex") == 0 || strcmp(rule->target_type, "string") == 0) {
        /* Generic Hex/String formatting for numeric hex fields (e.g. firmware 0x010A) */
        char hex_buf[32];
        snprintf(hex_buf, sizeof(hex_buf), "0x%X", static_cast<unsigned int>(raw_num));
        DBG_LORA_PRINTF(" -> [%s] %s: %s", rule->context, rule->key, hex_buf);
        val_type = 3; /* String / Hex */
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, hex_buf, strlen(hex_buf) + 1);

    } else { /* Default "int" */
        int32_t i_val = static_cast<int32_t>(scaled_val);
        DBG_LORA_PRINTF(" -> [%s] %s: %ld %s", rule->context, rule->key, (long)i_val, rule->target_unit);
        val_type = 1; /* Int32 */
        lora_telemetry_send_dynamic(ps, dev->dev_addr, fcnt, val_type, ctx_enum, 
                                    rule->key, rule->target_unit, &i_val, sizeof(int32_t));
    }
}

void lorasensors_process_raw_frame(PacketSerial &ps, const uint8_t *buffer, size_t size) {
    if (size < 9) return;

    /* 1. Extract UART Transport Metadata */
    uint32_t dev_addr = 0;
    memcpy(&dev_addr, &buffer[1], 4);
    int16_t rssi = 0;
    memcpy(&rssi, &buffer[5], 2);
    int8_t snr = static_cast<int8_t>(buffer[7]);
    uint8_t phy_len = buffer[8];

    if (size < 9 + phy_len || phy_len < 13) return;
    const uint8_t *phy_payload = &buffer[9];

    /* 2. Header Parsing */
    uint32_t fcnt = phy_payload[6] | (phy_payload[7] << 8);
    uint8_t fport = phy_payload[8];

    const uint8_t *frm_payload = &phy_payload[9];
    size_t frm_len = phy_len - 9 - 4;

    /* 3. Device & Decoder RAM Lookup */
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
    DBG_LORA_PRINTF("Device: '%s' | Decoder Schema: '%s' (%s) | Size: %u bytes",
                    dev->device_name, dec.name, dec.format, static_cast<unsigned>(frm_len));

    /* Raw Hex Payload Logger */
    char raw_hex[256] = {0};
    size_t hex_pos = 0;
    for (size_t b = 0; b < frm_len && (hex_pos + 3) < sizeof(raw_hex); b++) {
        hex_pos += snprintf(&raw_hex[hex_pos], sizeof(raw_hex) - hex_pos, "%02X ", frm_payload[b]);
    }
    DBG_LORA_PRINTF("Raw FRMPayload Hex: %s", raw_hex);

    /* Check for matched reaction trigger event */
    if (dev->reaction.valid && fport == dev->reaction.trigger_fport) {
        if (frm_len > 0 && frm_payload[0] == dev->reaction.trigger_prefix) {
            DBG_LORA_PRINTF(" -> [REACTION TRIGGER] Device '%s' matched FPort %u (Prefix: 0x%02X). ESP32 handling RX1 Downlink (FPort %u)",
                            dev->device_name, dev->reaction.trigger_fport, dev->reaction.trigger_prefix, dev->reaction.target_fport);
        }
    }

    /* 4A. Execute PACKED_BINARY Format Processing */
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

    /* 4B. Execute IPSO_TLV Format Processing */
    size_t i = 0;

    /* Apply payload offset if configured for this FPort (e.g. skip 0x55 prefix) */
    if (dec.fport_cfg.valid && dec.fport_cfg.fport == fport && dec.fport_cfg.payload_offset < frm_len) {
        i = dec.fport_cfg.payload_offset;
        DBG_LORA_PRINTF(" -> Skipping %u header byte(s) per FPort %u config (Header: 0x%02X)",
                        (unsigned)i, fport, frm_payload[0]);
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

            /* Advance stream index based on matched datatype */
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
        } else if (ch_id == 0xFF) { /* System information channel skips */
            if (ch_type == 0x09 || ch_type == 0x0A) i += 2;
            else if (ch_type == 0x16) i += 8;
            else i++;
        } else {
            break;
        }
    }
}