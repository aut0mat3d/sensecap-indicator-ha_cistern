#ifndef LORA_MODEL_H
#define LORA_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "gui_header.h"
#include "rp2040.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_LORA_DEVICES          16
#define MAX_LORA_PEERS            8
#define MAX_TX_QUEUE_SIZE         8
#define DUP_CACHE_SIZE            16
#define MAX_THRESHOLDS_PER_DEVICE 8
#define MAX_DEVICE_OFFSETS        4
#define MAX_DEVICE_LIMITS         8
#define MAX_LUT_POINTS            16

/* ── Packed Mirror Structs matching RP2040 Layout ──────────────────────── */

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
    char unit_system[16];
    char volume_unit[16];
} tank_display_config_t;

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
    tank_geometry_t        geometry;     /* Synchronize layout with RP2040 lorasensors.hpp */
    tank_display_config_t  display_cfg;  /* Synchronize layout with RP2040 lorasensors.hpp */
    uint8_t                 threshold_count;
    lora_threshold_config_t thresholds[MAX_THRESHOLDS_PER_DEVICE];
    uint8_t                 offset_count;
    lora_offset_config_t    offsets[MAX_DEVICE_OFFSETS];
    uint8_t                 limit_count;
    lora_limit_config_t     limits[MAX_DEVICE_LIMITS];
    uint32_t                report_interval_sec; /* Configured sensor transmit interval (device JSON, 60..3600 s); keep layout identical to rp2040/src/lorasensors.hpp */
} lora_device_config_t;

typedef struct {
    uint32_t dev_addr;
    uint16_t fcnt;
} lora_dup_entry_t;

typedef enum {
    LORA_DEV_ST_WAITING = 0,  /* Booted, awaiting first frame */
    LORA_DEV_ST_ONLINE  = 1,  /* Received within nominal interval * 2.025 */
    LORA_DEV_ST_TIMEOUT = 2   /* Watchdog expired (> interval * 2.025) */
} lora_dev_state_t;

/**
 * @brief Dynamic runtime status of a configured LoRaWAN sensor.
 */
typedef struct {
    uint32_t         dev_addr;
    char             name[32];
    uint32_t         interval_sec;     /* Nominal roundtrip interval from JSON */
    uint32_t         timeout_sec;      /* interval_sec * 2.025 (5% jitter + 2 cycles) */
    uint32_t         last_seen_ts;     /* Unix timestamp (for UI formatting) */
    uint32_t         last_seen_ms;     /* Monotonic system tick ms (for drift-free watchdog) */
    int16_t          last_rssi;
    int8_t           last_snr;
    uint16_t         last_fcnt;
    uint32_t         rx_count;
    bool             is_p2p_relayed;   /* True if last reception arrived via P2P Mesh */
    lora_dev_state_t state;
} lora_device_status_t;

/**
 * @brief Status of a remote P2P Mesh neighbor / relay peer.
 */
typedef struct {
    uint32_t last_dev_relayed; /* DevAddr of sensor relayed by this peer */
    uint32_t last_seen_ts;     /* Unix timestamp */
    uint32_t last_seen_ms;     /* Monotonic system tick ms */
    int16_t  last_rssi;
    int8_t   last_snr;
    uint8_t  last_ttl;         /* Hop distance indicator */
    uint32_t packet_count;     /* Total valid mesh packets relayed */
    bool     valid;
} lora_p2p_peer_t;

/**
 * @brief Get total number of configured LoRa devices.
 */
size_t lora_model_get_device_status_count(void);

/**
 * @brief Get device status by index.
 * @return Pointer to status struct, or NULL if index out of bounds.
 */
const lora_device_status_t* lora_model_get_device_status(size_t index);

/**
 * @brief Get device configuration by DevAddr.
 * @param dev_addr Target device address.
 * @return Pointer to internal device config, or NULL if not found.
 */
lora_device_config_t* lora_model_get_device_by_addr(uint32_t dev_addr);

/**
 * @brief Resolve a threshold to physical units (single source of truth for percent thresholds).
 *
 * Absolute thresholds are returned unchanged. Percent thresholds are resolved against the
 * reference range of the threshold's key: limits[key] (val_min..val_max), else
 * 0..geometry.max_depth_cm, else 0..100.
 *
 * @param dev      Device owning the threshold.
 * @param th       Threshold to resolve.
 * @param out_val  Physical threshold value.
 * @param out_hyst Physical hysteresis (always >= 0).
 */
void lora_model_resolve_threshold(const lora_device_config_t *dev, const lora_threshold_config_t *th,
                                  float *out_val, float *out_hyst);

/**
 * @brief Get total active P2P mesh peers.
 */
size_t lora_model_get_peer_count(void);

/**
 * @brief Get peer entry by index.
 * @return Pointer to peer struct, or NULL if index out of bounds.
 */
const lora_p2p_peer_t* lora_model_get_peer(size_t index);

/**
 * @brief Update the nominal reporting interval for a specific device.
 * @param dev_addr Device address.
 * @param interval_sec Nominal interval in seconds.
 */
void lora_model_set_device_interval(uint32_t dev_addr, uint32_t interval_sec);

/**
 * @brief UART Packet Type for sending raw LoRa frames from ESP32 to RP2040.
 */
#define PKT_TYPE_LORA_RAW_FRAME 0x32

typedef enum {
    LORA_MODE_DISABLED = 0,
    LORA_MODE_P2P,
    LORA_MODE_MESH
} lora_mode_t;

typedef enum {
    TX_TYPE_LORAWAN_DOWNLINK = 0,
    TX_TYPE_P2P_CLAIM        = 1,
    TX_TYPE_P2P_RETRANSMIT   = 2
} lora_tx_type_t;

/**
 * @brief Structure for queued LoRa transmissions.
 */
typedef struct {
    uint8_t        raw_payload[256];
    uint8_t        payload_len;
    uint32_t       target_timestamp_ms;
    bool           iq_inverted;
    uint8_t        tx_power_dbm;
    lora_tx_type_t msg_type;        /* LORAWAN_DOWNLINK, P2P_CLAIM, P2P_RETRANSMIT */
    uint32_t       dev_addr;
    uint16_t       trigger_fcnt;
    bool           is_auto_reaction;
    bool           valid;
} lora_tx_frame_t;

/**
 * @brief Initializes the LoRa background FreeRTOS task.
 */
void lora_model_init(void);

/**
 * @brief Handles incoming UART packet 0x35 (PKT_TYPE_PUSH_LORA_CFG) from RP2040.
 * 
 * @param buf Raw UART payload buffer.
 * @param len Length of payload buffer in bytes.
 */
void lora_model_on_rp2040_cfg_received(const uint8_t *buf, size_t len);

void lora_model_on_rp2040_device_received(const uint8_t *buf, size_t len);

/**
 * @brief Driver worker called periodically to check IRQ registers, TX queue, and noise floor.
 */
void lora_model_update(void);

/**
 * @brief Prepares and enqueues a LoRaWAN Class A Downlink frame (encrypted with AppSKey + MIC).
 * 
 * @param dev_addr Target device network address.
 * @param fport Target FPort (e.g. 85).
 * @param fcnt_down Downlink frame counter.
 * @param raw_cmd Raw command payload bytes (e.g. FF 84 00).
 * @param cmd_len Length of raw command bytes.
 * @param target_ts_ms Target time on air timestamp in milliseconds.
 * @param app_skey 128-bit Application Session Key.
 * @param nwk_skey 128-bit Network Session Key.
 * @return true if successfully enqueued.
 */
bool lora_model_enqueue_downlink(uint32_t dev_addr, uint8_t fport, uint32_t fcnt_down,
                                 const uint8_t *raw_cmd, size_t cmd_len,
                                 uint32_t target_ts_ms,
                                 const uint8_t *app_skey, const uint8_t *nwk_skey);

/**
 * @brief Prepares and enqueues a P2P CLAIM broadcast packet.
 * 
 * @param dev_addr Target device address claimed by this display.
 * @param target_ts_ms Target time on air timestamp in milliseconds.
 * @return true if successfully enqueued.
 */
bool lora_model_enqueue_claim(uint32_t dev_addr, uint32_t target_ts_ms);

/**
 * @brief Prepares and enqueues a P2P RETRANSMIT broadcast packet (encrypted raw TLV payload).
 * 
 * @param dev_addr Target sensor device address.
 * @param fcnt Uplink frame counter.
 * @param fport Uplink FPort.
 * @param ttl Time-to-Live hop count.
 * @param raw_tlv Decrypted TLV bytes.
 * @param tlv_len Length of TLV payload.
 * @param target_ts_ms Target timestamp in milliseconds.
 * @return true if successfully enqueued.
 */
bool lora_model_enqueue_retransmit(uint32_t dev_addr, uint16_t fcnt, uint8_t fport,
                                   uint8_t ttl, const uint8_t *raw_tlv, size_t tlv_len,
                                   uint32_t target_ts_ms);

/**
 * @brief Purges any pending LoRaWAN Downlink for a specific DevAddr from local TX queue.
 * 
 * @param dev_addr Target DevAddr to purge.
 */
void lora_model_purge_downlink_for_devaddr(uint32_t dev_addr);

/**
 * @brief Prepares and enqueues a Standalone P2P Time Broadcast frame.
 * 
 * @param utc_timestamp Current UTC Unix timestamp.
 * @return true if successfully enqueued.
 */
bool lora_model_enqueue_time_broadcast(uint32_t utc_timestamp);

#ifdef __cplusplus
}
#endif

#endif /* LORA_MODEL_H */