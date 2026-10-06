/**
 * @file lorasensors.hpp
 * @brief MicroSD Config Parser & LoRa P2P Device Table Manager for RP2040.
 * @copyright © 2024-2026, Seeed Studio / SenseCAP Cistern Extension
 */

#ifndef LORASENSORS_HPP
#define LORASENSORS_HPP

#include <Arduino.h>
#include <SdFat.h>
#include <ArduinoJson.h>
#include <PacketSerial.h>
#include <stdint.h>
#include "indicator_rp2040.hpp"

#define MAX_LORA_DEVICES 16
#define MAX_LORA_DECODERS 8
#define MAX_RULES_PER_DECODER 16
#define MAX_LORA_COMMANDS_PER_DECODER 8
#define MAX_REACTIONS_PER_DECODER 4

/* ── Data Structures ────────────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    bool     enabled;         /* Enable P2P Mesh features */
    char     device_id[16];   /* Node ID ("AUTO" or custom string) */
    uint16_t network_id;      /* Mesh Network ID (e.g. 0x4A6F) */
    uint8_t  aes_key[16];     /* 128-bit Mesh AES Encryption Key */
    uint8_t  broadcast_ttl;   /* Default TTL hop count */
} lora_mesh_config_t;

typedef struct __attribute__((packed)) {
    char posix_timezone[64];  /* e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */
    char time_format[8];      /* "24h" or "12h" */
    char ntp_server[32];      /* e.g. "pool.ntp.org" */
    bool p2p_sync_enabled;    /* Enable P2P time mesh */
} lora_time_config_t;

typedef struct __attribute__((packed)) {
    uint32_t           rf_frequency_hz;
    uint8_t            rf_spreading_factor;
    uint16_t           rf_bandwidth_khz;
    uint8_t            rf_coding_rate;
    uint8_t            rf_preamble_len;
    uint8_t            sync_word;
    lora_mesh_config_t mesh;
    lora_time_config_t time_cfg; /* Integrated Time Configuration */
} lora_global_config_t;

typedef struct __attribute__((packed)) {
    uint8_t trigger_fport;      /* Trigger FPort match (e.g. 13) */
    uint8_t trigger_prefix;     /* Trigger byte prefix (e.g. 0x55) */
    bool    trigger_encrypted;  /* False if payload on trigger FPort is unencrypted */
    uint8_t target_fport;       /* Target FPort for downlink (e.g. 85) */
    uint8_t payload_len;        /* Length of payload_bytes */
    uint8_t payload_bytes[16];  /* Raw command bytes (e.g. FF 84 00) */
    uint8_t max_retries;        /* Maximum retries, e.g. 3 */
    bool    valid;              /* Whether reaction rule is active */
} lora_device_reaction_t;

typedef struct __attribute__((packed)) {
    uint8_t fport;           /* Target FPort (e.g. 13) */
    uint8_t encryption_mode; /* 0=full, 1=none, 2=partial */
    uint8_t cipher_offset;   /* Offset where AES CTR starts (e.g. 1) */
    uint8_t key_type;        /* 0=app_skey, 1=nwk_skey */
    uint8_t payload_offset;  /* Offset where parser starts reading TLV (e.g. 1) */
    bool    valid;           /* Rule active */
} lora_fport_config_t;

typedef struct __attribute__((packed)) {
    char                   device_name[32];
    char                   medium[16];
    bool                   enabled;
    uint32_t               dev_addr;
    char                   dev_sn[32];
    char                   dev_eui[32];
    uint8_t                nwk_skey[16];
    uint8_t                app_skey[16];
    char                   decoder[32];
    int32_t                raw_offset_mm;
    lora_device_reaction_t reaction;
    uint32_t               fcnt_down;
    lora_fport_config_t    fport_cfg; /* Sync per-fport rule to ESP32 */
} lora_device_config_t;

/* ── RAM Decoder Cache Structures ───────────────────────────────────────── */

typedef struct {
    uint8_t  fport;             /* 0=Wildcard, >0 = FPort Match*/
    uint8_t  channel;
    uint8_t  type;
    uint16_t offset;            /* For PACKED_BINARY format */
    uint8_t  length;            /* Byte length for PACKED_BINARY / bytes_N */
    char     key[24];
    char     unit[12];          /* Raw sensor unit */
    char     target_unit[12];   /* Output unit for UI/HA */
    char     datatype[16];      /* Raw payload read instruction */
    char     target_type[12];   /* Output type: float, int, bool, hex, string */
    char     context[16];       /* measurement, diagnostic, history */
    float    scale;
} decoder_rule_t;

typedef struct {
    char     key[32];          /* Command name, e.g. "disable_d2d" */
    uint8_t  fport;            /* Target FPort for this command (e.g. 13 or 85) */
    char     raw_hex[64];      /* Static hex string if present, e.g. "FF8400" */
    char     prefix[16];       /* Prefix for dynamic commands, e.g. "FF03" */
    char     datatype[16];     /* Datatype for dynamic value, e.g. "uint16_le" */
    char     unit[12];         /* Unit, e.g. "seconds" */
    uint32_t default_val;      /* Default value */
    bool     is_dynamic;       /* True if command uses dynamic value */
} lora_command_t;

typedef struct {
    uint8_t trigger_fport;      /* Trigger FPort match (e.g. 13) */
    uint8_t trigger_prefix;     /* Trigger byte prefix (e.g. 0x55) */
    bool    trigger_encrypted;  /* False if payload on trigger FPort is unencrypted */
    char    action_command[32]; /* Target command name (e.g. "disable_d2d") */
    uint8_t target_fport;       /* Target FPort for downlink (e.g. 85) */
    char    timing_mode[16];    /* Timing mode, e.g. "rx1_exact" */
    uint8_t max_retries;        /* Maximum retries, e.g. 3 */
} lora_reaction_t;

typedef struct {
    char                name[32];
    char                format[16];  /* "IPSO_TLV" or "PACKED_BINARY" */
    decoder_rule_t      rules[MAX_RULES_PER_DECODER];
    uint8_t             rule_count;
    lora_command_t      commands[MAX_LORA_COMMANDS_PER_DECODER];
    uint8_t             command_count;
    lora_reaction_t     reactions[MAX_REACTIONS_PER_DECODER];
    uint8_t             reaction_count;
    lora_fport_config_t fport_cfg;   /* Cached FPort config rule */
    bool                valid;
} lora_decoder_t;

/* ── Public API ────────────────────────────────────────────────────────── */

/* Dynamic UART 0xBB telemetry dispatch helper */
void lora_telemetry_send_dynamic(PacketSerial& ps, uint32_t dev_addr, uint32_t fcnt, 
                                 uint8_t val_type, uint8_t context, 
                                 const char* key, const char* unit, 
                                 const void* val_data, size_t val_len);

/**
 * @brief Pushes current LoRa config status and active device table to ESP32 over UART.
 * @param ps Active PacketSerial instance.
 * @param valid_cfg True if a valid configuration was successfully loaded from SD.
 */
void lorasensors_push_config_to_esp32(PacketSerial &ps, bool valid_cfg);

/**
 * @brief Scans /lora_p2p_configs/ on Partition 1 and loads globals.json and devices'/'*'.json.
 * @param sd Reference to the active SdFat32 filesystem instance.
 * @return true if globals.json was successfully parsed.
 */
bool lorasensors_load_all(SdFat32 &sd);

/**
 * @brief Returns current active global RF parameters.
 */
const lora_global_config_t& lorasensors_get_globals(void);

/**
 * @brief Searches the loaded device table for a matching DevAddr.
 * @param dev_addr Numeric DevAddr received in LoRa PHY payload.
 * @return Pointer to device config struct, or NULL if not found or disabled.
 */
const lora_device_config_t* lorasensors_find_device(uint32_t dev_addr);

/**
 * @brief Returns the total number of loaded devices in RAM.
 */
size_t lorasensors_get_device_count(void);

/**
 * @brief Processes incoming raw LoRa frame packet (0x36) 100% in RAM without touching SD card.
 */
void lorasensors_process_raw_frame(PacketSerial &ps, const uint8_t *buffer, size_t size);

/**
 * @brief Finds a command in a cached decoder by decoder name and command key.
 * @param decoder_name Name of decoder schema (e.g. "EM500-SWL").
 * @param cmd_key Command key name (e.g. "disable_d2d").
 * @return Pointer to lora_command_t or NULL if not found.
 */
const lora_command_t* lorasensors_find_command(const char* decoder_name, const char* cmd_key);

/**
 * @brief Builds binary payload bytes for a command (static or dynamic).
 * @param cmd Pointer to lora_command_t struct.
 * @param val Dynamic parameter value (ignored if command is static).
 * @param out_bytes Destination buffer for output payload.
 * @param max_len Maximum output buffer size.
 * @return Number of payload bytes generated, or 0 on error.
 */
size_t lorasensors_build_command_payload(const lora_command_t* cmd, uint32_t val, uint8_t* out_bytes, size_t max_len);

#endif /* LORASENSORS_HPP */