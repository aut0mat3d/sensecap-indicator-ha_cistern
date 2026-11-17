/**
 * @file lorasensors.hpp
 * @brief MicroSD Config Parser & LoRa P2P Device Table Manager for RP2040.
 */

#ifndef LORASENSORS_HPP
#define LORASENSORS_HPP

#include <Arduino.h>
#include <SdFat.h>
#include <ArduinoJson.h>
#include <PacketSerial.h>
#include <stdint.h>
#include "indicator_rp2040.hpp"

#define MAX_LORA_DEVICES                16
#define MAX_LORA_DECODERS               8
#define MAX_RULES_PER_DECODER           16
#define MAX_LORA_COMMANDS_PER_DECODER   8
#define MAX_REACTIONS_PER_DECODER       4
#define MAX_LUT_POINTS                  16
#define MAX_THRESHOLDS_PER_DEVICE       8
#define MAX_DEVICE_OFFSETS              4
#define MAX_DEVICE_LIMITS               8

typedef enum {
    TANK_TYPE_NONE = 0,
    TANK_TYPE_RECTANGLE,
    TANK_TYPE_CYLINDER_VERTICAL,
    TANK_TYPE_CYLINDER_HORIZONTAL,
    TANK_TYPE_CONE_FRUSTUM,
    TANK_TYPE_LUT
} tank_type_t;

typedef struct {
    float depth_cm;
    float liters;
} lut_point_t;

typedef struct __attribute__((packed)) {
    uint8_t     type;
    float       max_depth_cm;
    float       width_cm;
    float       length_cm;
    float       diameter_cm;
    float       dish_height_cm;
    float       bottom_diameter_cm;
    float       top_diameter_cm;
    float       height_cm;
    float       cone_compensation;
    lut_point_t lut[MAX_LUT_POINTS];
    uint8_t     lut_count;
} tank_geometry_t;

typedef struct __attribute__((packed)) {
    char     rule_id[16];          /* "alarm_low", "alarm_frate" */
    char     key[16];              /* "depth", "flowrate", "battery_soc" */
    char     unit[16];             /* Resolved unit: "%", "cm", "m³/h", "°C", etc. */
    bool     is_percent;           /* true = value_percent in JSON, false = absolute */
    uint8_t  severity;             /* 0=danger, 1=warning, 2=info */
    uint8_t  op;                   /* 0=less_than (<), 1=greater_than (>) */
    float    threshold_val;        /* Original value from JSON (e.g. 15.0 or -2.0) */
    float    hysteresis_val;       /* Original hysteresis from JSON (e.g. 3.0 or 0.4) */
    uint8_t  damping;              /* Damping cycles (e.g. 0 or 2) */
    uint8_t  buzzer_mode;          /* 0=intermittent, 1=continuous, 2=off */
    bool     valid;
} lora_threshold_config_t;

typedef struct __attribute__((packed)) {
    char unit_system[16]; /* "metric" or "imperial" */
    char volume_unit[16]; /* "liters", "m3", "gallons" */
} tank_display_config_t;

typedef struct __attribute__((packed)) {
    bool     enabled;         /* Enable P2P Mesh features */
    char     device_id[16];   /* Node ID ("AUTO" or custom string) */
    uint16_t network_id;      /* Mesh Network ID (e.g. 0x4A6F) */
    uint8_t  aes_key[16];     /* 128-bit Mesh AES Encryption Key */
    uint8_t  broadcast_ttl;   /* Default TTL hop count */
} lora_mesh_config_t;

typedef struct __attribute__((packed)) {
    char language[8];         /* e.g. "de", "en" */
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
    lora_time_config_t time_cfg; /* Integrated System Time & Language Config */
} lora_global_config_t;

typedef struct __attribute__((packed)) {
    uint8_t trigger_fport;
    uint8_t trigger_prefix;
    bool    trigger_encrypted;
    uint8_t target_fport;
    uint8_t payload_len;
    uint8_t payload_bytes[16];
    uint8_t max_retries;
    bool    valid;
} lora_device_reaction_t;

typedef struct __attribute__((packed)) {
    uint8_t fport;
    uint8_t encryption_mode;
    uint8_t cipher_offset;
    uint8_t key_type;
    uint8_t payload_offset;
    bool    valid;
} lora_fport_config_t;

typedef struct __attribute__((packed)) {
    char  key[16];
    float value;
    char  unit[8];
    bool  valid;
} lora_offset_config_t;

typedef struct __attribute__((packed)) {
    char    key[16];
    float   val_min;
    float   val_max;
    uint8_t cap_min;
    uint8_t cap_max;
    bool    valid;
} lora_limit_config_t;

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
    lora_fport_config_t    fport_cfg;
    tank_geometry_t        geometry;
    tank_display_config_t  display_cfg;
    uint8_t                 threshold_count;
    lora_threshold_config_t thresholds[MAX_THRESHOLDS_PER_DEVICE];
    uint8_t                 offset_count;
    lora_offset_config_t    offsets[MAX_DEVICE_OFFSETS];
    uint8_t                 limit_count;
    lora_limit_config_t     limits[MAX_DEVICE_LIMITS];
    uint32_t                report_interval_sec; /* Configured sensor transmit interval (device JSON, 60..3600 s); keep layout identical to main/lora/lora_model.h */
} lora_device_config_t;

typedef struct {
    uint8_t  fport;
    uint8_t  channel;
    uint8_t  type;
    uint16_t offset;
    uint8_t  length;
    char     key[24];
    char     unit[12];
    char     target_unit[12];
    char     datatype[16];
    char     target_type[12];
    char     context[16];
    float    scale;
    float   val_min;
    float   val_max;
    bool    has_bounds;
} decoder_rule_t;

typedef struct {
    char     key[32];
    uint8_t  fport;
    char     raw_hex[64];
    char     prefix[16];
    char     datatype[16];
    char     unit[12];
    uint32_t default_val;
    bool     is_dynamic;
} lora_command_t;

typedef struct {
    uint8_t trigger_fport;
    uint8_t trigger_prefix;
    bool    trigger_encrypted;
    char    action_command[32];
    uint8_t target_fport;
    char    timing_mode[16];
    uint8_t max_retries;
} lora_reaction_t;

typedef struct {
    char                name[32];
    char                format[16];
    decoder_rule_t      rules[MAX_RULES_PER_DECODER];
    uint8_t             rule_count;
    lora_command_t      commands[MAX_LORA_COMMANDS_PER_DECODER];
    uint8_t             command_count;
    lora_reaction_t     reactions[MAX_REACTIONS_PER_DECODER];
    uint8_t             reaction_count;
    lora_fport_config_t fport_cfg;
    bool                valid;
} lora_decoder_t;

/* ── Public API ────────────────────────────────────────────────────────── */
/**
 * @brief Dispatch a raw measurement from an internal/Grove I2C sensor through 
 *        the offset engine, data storage, and virtual decoders.
 */
void lorasensors_dispatch_local(PacketSerial &ps, uint32_t dev_addr, 
                                const char* key, float raw_val, const char* unit);
                                
void lora_telemetry_send_dynamic(PacketSerial& ps, uint32_t dev_addr, uint32_t fcnt, 
                                 uint8_t val_type, uint8_t context, 
                                 const char* key, const char* unit, 
                                 const void* val_data, size_t val_len);

void lorasensors_push_globals(PacketSerial &ps, bool valid_cfg);
bool lorasensors_stream_single_device(PacketSerial &ps, size_t dev_idx);
bool lorasensors_load_all(SdFat32 &sd);
const lora_global_config_t& lorasensors_get_globals(void);
const lora_device_config_t* lorasensors_find_device(uint32_t dev_addr);
size_t lorasensors_get_device_count(void);
void lorasensors_process_raw_frame(PacketSerial &ps, const uint8_t *buffer, size_t size);
const lora_command_t* lorasensors_find_command(const char* decoder_name, const char* cmd_key);
size_t lorasensors_build_command_payload(const lora_command_t* cmd, uint32_t val, uint8_t* out_bytes, size_t max_len);


/**
 * @brief Atomically patch a specific sensor offset in a device's JSON on SD card
 *        and synchronize internal RAM structures without requiring a reboot.
 *
 * @param sd Reference to mounted Fat32 filesystem
 * @param dev_addr Target device address
 * @param key Telemetry key (e.g. "depth")
 * @param value Numeric offset value
 * @param unit Engineering unit string (e.g. "mm", "cm")
 * @return true on success, false if device not found or write failed
 */
bool lorasensors_save_offset_to_sd(SdFat32 &sd, uint32_t dev_addr, const char *key,
                                   float value, const char *unit);

/**
 * @brief Atomically patch a specific threshold rule in a device's JSON on SD card
 *        and synchronize internal RAM structures without requiring a reboot.
 *
 * @param sd Reference to mounted Fat32 filesystem
 * @param dev_addr Target device address
 * @param rule_id Unique threshold identifier (e.g. "alarm_low")
 * @param is_percent True if value should be written as value_percent
 * @param threshold_val Numeric threshold value
 * @param hysteresis_val Numeric hysteresis value
 * @param damping Damping filter cycles
 * @param severity Severity enum (0=danger, 1=warning, 2=info)
 * @param buzzer_mode Buzzer enum (0=intermittent, 1=continuous, 2=off)
 * @return true on success, false if device/rule not found or write failed
 */
bool lorasensors_save_threshold_to_sd(SdFat32 &sd, uint32_t dev_addr, const char *rule_id,
                                      bool is_percent, float threshold_val, float hysteresis_val,
                                      uint8_t damping, uint8_t severity, uint8_t buzzer_mode);

#endif /* LORASENSORS_HPP */