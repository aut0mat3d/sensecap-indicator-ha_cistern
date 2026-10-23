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

typedef struct __attribute__((packed)) {
    uint32_t timestamp;
    float    value;
    uint8_t  flags;
} chart_data_point_t;

typedef void (*chart_data_callback_t)(uint8_t req_id, uint8_t status, uint32_t actual_interval, 
                                      const chart_data_point_t *points, uint16_t count);

enum pkt_type_cmd
{
	PKT_TYPE_NONE                 = 0,
	PKT_TYPE_CMD_COLLECT_INTERVAL = 0xA0, /* uint32_t ms */
	PKT_TYPE_CMD_BEEP_ON          = 0xA1, /* uint32_t ms duration */
	PKT_TYPE_CMD_BEEP_OFF         = 0xA2,
	PKT_TYPE_CMD_SHUTDOWN         = 0xA3,
	PKT_TYPE_CMD_POWER_ON         = 0xA4,
	PKT_TYPE_CMD_RESCAN_GROVE     = 0xA5,

	/* Cistern Alarm System */
	PKT_TYPE_CMD_ALARM_SET        = 0xA6, /* tON_ms, tOFF_ms */
	PKT_TYPE_CMD_ALARM_MUTE       = 0xA7, /* mute_sec */
	PKT_TYPE_CMD_ALARM_REARM      = 0xA8,
	PKT_TYPE_CMD_ALARM_ACK_ALL    = 0xA9, /* Acknowledge all alarms */
	PKT_TYPE_CMD_ALARM_GET_LIST   = 0xAA, /* Request alarm list */
	PKT_TYPE_RES_ALARM_LIST       = 0xAB, /* Response with alarm data */
	PKT_TYPE_CMD_ALARM_ADD        = 0xAC, /* Add new alarm to RP2040 */
	PKT_TYPE_CMD_ALARM_CONFIG_GET = 0xAD, /* Get alarm config from LittleFS */
	PKT_TYPE_RES_ALARM_CONFIG     = 0xAE, /* Response with alarm config */

	PKT_TYPE_CMD_SET_ESP_MAC     = 0x22, /* Payload: 6-byte raw MAC + 11-byte expected label */

	/* SD Card Management Packets */
	PKT_TYPE_CMD_SD_GET_INFO     = 0x30, /* Request card size, presence & label check */
    PKT_TYPE_RES_SD_INFO         = 0x31, /* Response: present, usb_mounted, state, total_mb, label */
    PKT_TYPE_CMD_SD_FORMAT       = 0x32, /* Payload: format_type (0=LFS, 1=Full SD) + volume_label[12] */
    PKT_TYPE_RES_SD_FORMAT       = 0x33, /* Response: success (uint8_t) */
    PKT_TYPE_CMD_USB_MSC_TOGGLE  = 0x34, /* Toggle USB Mass Storage Mode */

	PKT_TYPE_PUSH_LORA_CFG       = 0x35, /* Payload: valid_flag + globals + devices */
    PKT_TYPE_CMD_LORA_RAW_FRAME  = 0x36, /* Payload: DevAddr(4B) + RSSI(2B) + SNR(1B) + Size(1B) + RawData(NB) */

	PKT_TYPE_CMD_SET_TIME_CFG 	 = 0x37, /* Payload: posix_tz[64] + time_format[8] */
    PKT_TYPE_CMD_READ_FILE       = 0x38, /* Payload: path string (null-terminated) */
    PKT_TYPE_RES_FILE_DATA       = 0x39, /* Payload: status(uint8_t: 0=Error/Not found, 1=Data) + length(uint16_t) + data[] */

    PKT_TYPE_CMD_SYNC_TIME       = 0x3A,
    PKT_TYPE_CMD_GET_CHART_DATA  = 0x3B, /* Request: zone query from ESP32 */
    PKT_TYPE_RES_CHART_DATA      = 0x3C, /* Response: packed array of chart_data_point_t */
    PKT_TYPE_CMD_GET_TIME        = 0x3D, /* Request last saved UTC timestamp from LittleFS */
    PKT_TYPE_RES_TIME            = 0x3E, /* Response: uint32_t last_utc */
};

enum pkt_type_data
{
    /* Dynamic telemetry packets */
    PKT_TYPE_LORA_TELEMETRY  = 0xBB, /* External LoRa / LoRaWAN nodes */
    PKT_TYPE_LOCAL_TELEMETRY = 0xBC  /* Onboard and Grove I2C sensors */
};

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
                              uint16_t max_points, float alarm_min, float alarm_max);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* RP2040_H */