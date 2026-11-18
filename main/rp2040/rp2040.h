/**
 * @file rp2040.h
 * @date  12 December 2023

 * @author Spencer Yan / Cistern Extension
 *
 * @note Description of the file
 *
 * @copyright © 2023, Seeed Studio
 */

#ifndef RP2040_H
#define RP2040_H

#ifdef __cplusplus
extern "C" {
#endif

#include "view_data.h"

/* In main/rp2040/rp2040.h */

typedef struct __attribute__((packed)) {
    bool     enabled;         /* Enable P2P Mesh features */
    char     device_id[16];   /* Node ID ("AUTO" or custom string) */
    uint16_t network_id;      /* Mesh Network ID (e.g. 0x4A6F) */
    uint8_t  aes_key[16];     /* 128-bit Mesh AES Encryption Key */
    uint8_t  broadcast_ttl;   /* Default TTL hop count */
} lora_mesh_config_t;

typedef struct __attribute__((packed)) {
    char language[8];         /* Language code e.g. "de", "en" */
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
    lora_time_config_t time_cfg; /* Global System Time & Timezone Configuration */
} lora_global_config_t;

#define DATA_FLAG_REAL          0x00
#define DATA_FLAG_ESTIMATED     0x01
#define DATA_FLAG_AVERAGED      0x02
#define DATA_FLAG_ALARM_WARN    0x04
#define DATA_FLAG_ALARM_DANGER  0x08
#define DATA_FLAG_NO_DATA       0x10

/* Severity of the chart query alarm levels (CMD_GET_CHART_DATA byte 44, same bits on the RP2040):
 * a set bit marks the level as warning (DATA_FLAG_ALARM_WARN), a cleared bit as danger */
#define CHART_ALARM_MIN_IS_WARNING  0x01
#define CHART_ALARM_MAX_IS_WARNING  0x02

/* Chart query mode (CMD_GET_CHART_DATA byte 45, same values on the RP2040):
 *  GRID: one value per bucket of interval_sec (fixed grid, gaps as DATA_FLAG_NO_DATA)
 *  LTTB: window [start, end[ reduced to at most max_points representative points with their real
 *        timestamps (Largest-Triangle-Three-Buckets on time buckets; one NO_DATA marker per gap) */
#define CHART_QUERY_MODE_GRID  0
#define CHART_QUERY_MODE_LTTB  1
/* LTTB time buckets for max_points: points plus one gap marker per gap always fit into max_points */
#define CHART_LTTB_BUCKETS(max_points)  ((((max_points) * 2) / 3) < 2 ? 2 : (((max_points) * 2) / 3))

typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    float    value;
    uint8_t  flags;
} chart_data_point_t;

typedef void (*chart_data_callback_t)(uint8_t req_id, uint8_t status, uint32_t actual_interval, 
                                      const chart_data_point_t *points, uint16_t count);

enum pkt_type_cmd
{
    PKT_TYPE_NONE                 = 0x00,

    /* ── 0x20..0x2F: System & Identity ─────────────────────────────────── */
    PKT_TYPE_CMD_GET_SYS_INFO     = 0x20,
    PKT_TYPE_RES_SYS_INFO         = 0x21,
    PKT_TYPE_CMD_SET_ESP_MAC      = 0x22,  /* Payload: 6-byte raw MAC + 11-byte expected label */

    /* ── 0x30..0x4F: Storage, Configuration & File Management ─────────── */
    PKT_TYPE_CMD_SD_GET_INFO      = 0x30,  /* Request card size, presence & label check */
    PKT_TYPE_RES_SD_INFO          = 0x31,  /* Response: present, usb_mounted, state, total_mb, label */
    PKT_TYPE_CMD_SD_FORMAT        = 0x32,  /* Payload: format_type (0=LFS, 1=Full SD) + volume_label[12] */
    PKT_TYPE_RES_SD_FORMAT        = 0x33,  /* Response: success (uint8_t) */
    PKT_TYPE_CMD_USB_MSC_TOGGLE   = 0x34,  /* Toggle USB Mass Storage Mode */
    PKT_TYPE_PUSH_LORA_CFG        = 0x35,  /* Globals & total device count */
    PKT_TYPE_CMD_LORA_RAW_FRAME   = 0x36,  /* Payload: DevAddr(4B) + RSSI(2B) + SNR(1B) + Size(1B) + RawData(NB) */
    PKT_TYPE_CMD_SET_TIME_CFG     = 0x37,  /* Payload: posix_tz[64] + time_format[8] */
    PKT_TYPE_CMD_READ_FILE        = 0x38,  /* Payload: offset(uint32_t) + path (chunked read) */
    PKT_TYPE_RES_FILE_DATA        = 0x39,  /* Payload: status(uint8_t) + chunk_len(uint16_t) + total(uint32_t) + offset(uint32_t) + data[] (max 512 B) */
    PKT_TYPE_CMD_SYNC_TIME        = 0x3A,
    PKT_TYPE_CMD_GET_CHART_DATA   = 0x3B,  /* Request: zone query from ESP32 */
    PKT_TYPE_RES_CHART_DATA       = 0x3C,  /* Response: packed array of chart_data_point_t */
    PKT_TYPE_CMD_GET_TIME         = 0x3D,  /* Request last saved UTC timestamp from LittleFS */
    PKT_TYPE_RES_TIME             = 0x3E,  /* Response: uint32_t last_utc */
    PKT_TYPE_PUSH_LORA_DEV        = 0x3F,  /* Streamed single device configuration chunk */
    PKT_TYPE_CMD_SET_THRESHOLD    = 0x40,  /* ESP32 -> RP2040: Patch threshold entry on SD */
    PKT_TYPE_RES_SET_THRESHOLD    = 0x41,  /* RP2040 -> ESP32: Status (uint8_t: 1=OK, 0=ERR) */
    PKT_TYPE_CMD_SET_OFFSET       = 0x42,  /* ESP32 -> RP2040: Patch offset entry on SD */
    PKT_TYPE_RES_SET_OFFSET       = 0x43,  /* RP2040 -> ESP32: Status (uint8_t: 1=OK, 0=ERR) */
    PKT_TYPE_CMD_GET_LAST_VALUE   = 0x44,  /* ESP32 -> RP2040: dev_addr(4) + key[16]: newest archived value */
    PKT_TYPE_RES_LAST_VALUE       = 0x45,  /* RP2040 -> ESP32: dev_addr, key[16], status, value, ts, expected interval, unit[8] */


    /* ── 0xA0..0xAF: Power, Hardware & Buzzer Control ─────────────────── */
    PKT_TYPE_CMD_COLLECT_INTERVAL = 0xA0,  /* uint32_t ms */
    PKT_TYPE_CMD_BEEP_ON          = 0xA1,  /* Legacy single beep click */
    PKT_TYPE_CMD_BEEP_OFF         = 0xA2,
    PKT_TYPE_CMD_SHUTDOWN         = 0xA3,
    PKT_TYPE_CMD_POWER_ON         = 0xA4,
    PKT_TYPE_CMD_RESCAN_GROVE     = 0xA5,
    PKT_TYPE_CMD_ALARM_SET        = 0xA6,  /* tON_ms, tOFF_ms */
    PKT_TYPE_CMD_ALARM_MUTE       = 0xA7,  /* mute_sec */
    PKT_TYPE_CMD_ALARM_REARM      = 0xA8,
    PKT_TYPE_CMD_ALARM_SAVE       = 0xA9,  /* Payload: chunk_idx(1B) + total_chunks(1B) + count(1B) + entries[] */
    PKT_TYPE_CMD_ALARM_LOAD       = 0xAA,  /* Request: trigger RP2040 to stream saved history */
    PKT_TYPE_RES_ALARM_LOAD       = 0xAB,  /* Response: chunk_idx(1B) + total_chunks(1B) + count(1B) + entries[] */
    PKT_TYPE_RES_ALARM_SAVE       = 0xAC   /* Ack per save chunk: chunk_idx(1B) + status(1B: 1=OK, 0=ERR) */
};

enum pkt_type_data
{
    /* Dynamic telemetry packets */
    PKT_TYPE_LORA_TELEMETRY  = 0xBB, /* External LoRa / LoRaWAN nodes */
    PKT_TYPE_LOCAL_TELEMETRY = 0xBC  /* Onboard and Grove I2C sensors */
};

/** UART link diagnostics (counters + periodic summary in the log). 0 = compiled out */
#ifndef RP2040_LINK_DIAG
#define RP2040_LINK_DIAG             1
#endif
#define RP2040_LINK_DIAG_PERIOD_MS   30000

void esp32_rp2040_init(void);

int rp2040_send_esp_mac(void);

int rp2040_send_sync_time(uint32_t utc_ts, uint8_t flags);
int rp2040_get_last_time_req(void);

/* UART SD Card Command Proxies */
int rp2040_sd_get_info_req(void);
int rp2040_sd_format_req(uint8_t format_type, const char *volume_label);
int rp2040_sd_usb_msc_toggle(uint8_t enable);
int rp2040_send_lora_raw_frame(uint32_t dev_addr, int16_t rssi, int8_t snr, const uint8_t *payload, uint8_t size);

/**
 * @brief Register callback for incoming chart zone responses.
 */
void rp2040_register_chart_data_callback(chart_data_callback_t cb);

/**
 * @brief Request chart data for a specific zone from RP2040 LittleFS archives.
 */
int rp2040_request_chart_data(uint8_t req_id, uint32_t dev_addr, const char *key,
                              uint32_t start_ts, uint32_t end_ts, uint32_t interval_sec,
                              uint16_t max_points, float alarm_min, float alarm_max,
                              uint8_t alarm_flags, uint8_t query_mode);


/**
 * @brief Ask the RP2040 for the newest archived value of a device key. The answer arrives as
 *        VIEW_EVENT_ARCHIVE_LAST_VALUE (struct view_data_last_value), also when nothing is archived.
 * @return Number of UART bytes written, or -1 on error.
 */
int rp2040_request_last_value(uint32_t dev_addr, const char *key);

/* Alarm (Buzzer) */
int rp2040_alarm_set(uint16_t t_on_ms, uint16_t t_off_ms);
int rp2040_alarm_mute(uint32_t mute_sec);
int rp2040_alarm_rearm(void);

/* Forward declaration to avoid circular include with alarm_model.h */
struct alarm_entry_s;

/** Completion callback of an alarm journal upload (called from the UART task) */
typedef void (*rp2040_alarm_upload_done_cb_t)(bool success);

/**
 * @brief Start a non-blocking, acknowledged upload of the alarm journal to the RP2040 SD card.
 *
 * The journal is sent in small chunks; the next chunk follows only after the RP2040 has
 * written the previous one and acknowledged it (RES_ALARM_SAVE). Failed or unacknowledged
 * uploads restart from the first chunk (limited attempts), then done_cb(false) is called.
 *
 * @param entries  Heap buffer with the journal (oldest first), sizeof(alarm_entry_t) each.
 *                 Ownership passes to the uploader on success (released with free()).
 *                 May be NULL when count is 0 (clears the journal on SD).
 * @param count    Number of entries.
 * @param done_cb  Optional completion callback.
 * @return 0 if started, -1 if an upload is still running (caller keeps the buffer).
 */
int rp2040_alarm_upload_start(void *entries, uint16_t count, rp2040_alarm_upload_done_cb_t done_cb);

/**
 * @brief true while an alarm journal upload is in progress.
 */
bool rp2040_alarm_upload_busy(void);

/**
 * @brief Request RP2040 to stream saved alarm history from SD card.
 * @return Number of UART bytes written, or -1 on error.
 */
int rp2040_alarm_request_load(void);

/**
 * @brief Transmit a threshold configuration update to the RP2040 for SD card persistence.
 *
 * @param dev_addr Target device network address (DevAddr).
 * @param rule_id Unique threshold identifier string (e.g. "alarm_low").
 * @param is_percent True if value represents percentage, false for physical unit.
 * @param threshold_val Numeric threshold trigger value.
 * @param hysteresis_val Numeric hysteresis deadband value.
 * @param damping Required consecutive filter hits.
 * @param severity Alarm severity level (0=danger, 1=warning, 2=info).
 * @param buzzer_mode Buzzer audio pattern (0=intermittent, 1=continuous, 2=off).
 * @param timeout_ms Response wait timeout in ms (0 for fire-and-forget).
 * @return 0 on success, negative error code on failure or timeout.
 */
int rp2040_send_set_threshold(uint32_t dev_addr, const char *rule_id,
                              bool is_percent, float threshold_val, float hysteresis_val,
                              uint8_t damping, uint8_t severity, uint8_t buzzer_mode,
                              uint32_t timeout_ms);

/**
 * @brief Transmit a sensor offset update to the RP2040 for SD card persistence.
 *
 * @param dev_addr Target device network address (DevAddr).
 * @param key Telemetry key string (e.g. "depth").
 * @param value Numeric offset value.
 * @param unit Engineering unit string (e.g. "mm", "cm").
 * @param timeout_ms Response wait timeout in ms (0 for fire-and-forget).
 * @return 0 on success, negative error code on failure or timeout.
 */
int rp2040_send_set_offset(uint32_t dev_addr, const char *key,
                           float value, const char *unit, uint32_t timeout_ms);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* RP2040_H */