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
};

enum pkt_type_data
{
	/* Inner Sensor SCD41 */
	PKT_TYPE_SENSOR_SCD41_CO2 = 0xB2, /* float */

	/* SHT41 / AHT20 */
	PKT_TYPE_SENSOR_SHT41_TEMP     = 0xB3,
	PKT_TYPE_SENSOR_SHT41_HUMIDITY = 0xB4, /* float */
	
	/* Inner Sensor SGP40 */
	PKT_TYPE_SENSOR_SGP40_TVOC_INDEX = 0xB5, /* float */
	PKT_TYPE_SENSOR_LIGHT_LUX        = 0xB6, /* float */

	/* Dynamic sensor registry packets */
	PKT_TYPE_SENSOR_ATTACHED = 0xB8,
	PKT_TYPE_SENSOR_DETACHED = 0xB9,
	PKT_TYPE_SENSOR_VALUE    = 0xBA,
	PKT_TYPE_LORA_TELEMETRY  = 0xBB, /* Payload: DevAddr(4B) + SensorID(1B) + FloatVal(4B) + Checksum(1B) */

	/* SEN5x */
	PKT_TYPE_SENSOR_SEN5X_massConcentrationPm1p0 = 0xB6,
	PKT_TYPE_SENSOR_SEN5X_massConcentrationPm2p5 = 0xB7,
	PKT_TYPE_SENSOR_SEN5X_massConcentrationPm4p0 = 0xB8,
	PKT_TYPE_SENSOR_SEN5X_massConcentrationPm10p0 = 0xB9,
	PKT_TYPE_SENSOR_SEN5X_ambientHumidity = 0xBA,
	PKT_TYPE_SENSOR_SEN5X_ambientTemperature = 0xBB,
	PKT_TYPE_SENSOR_SEN5X_vocIndex = 0xBC,
	PKT_TYPE_SENSOR_SEN5X_noxIndex = 0xBD,

	/* SFA3X */
	PKT_TYPE_SENSOR_SFA3X_HCHO = 0xBE,
	PKT_TYPE_SENSOR_SFA3X_HUMIDITY = 0xBF,
	PKT_TYPE_SENSOR_SFA3X_TEMP = 0xC0,
};

#define PKT_SENSOR_ID_AHT20_TEMP     0
#define PKT_SENSOR_ID_AHT20_HUMIDITY 1
#define PKT_SENSOR_ID_SCD41_CO2      2
#define PKT_SENSOR_ID_SGP40_VOC      3
#define PKT_SENSOR_ID_SCD41_TEMP     4
#define PKT_SENSOR_ID_SCD41_HUMIDITY 5
#define PKT_SENSOR_ID_LIGHT_LUX      6
#define PKT_SENSOR_ID_GROVE_BASE     0x10

#define PKT_SENSOR_CAT_TEMP     0
#define PKT_SENSOR_CAT_HUMIDITY 1
#define PKT_SENSOR_CAT_CO2      2
#define PKT_SENSOR_CAT_VOC      3
#define PKT_SENSOR_CAT_LIGHT    4
#define PKT_SENSOR_CAT_PRESSURE 5
#define PKT_SENSOR_CAT_PM       6
#define PKT_SENSOR_CAT_NOX      7
#define PKT_SENSOR_CAT_UNKNOWN  255

void esp32_rp2040_init(void);

int rp2040_send_esp_mac(void);

/* UART SD Card Command Proxies */
int rp2040_sd_get_info_req(void);
int rp2040_sd_format_req(uint8_t format_type, const char *volume_label);
int rp2040_sd_usb_msc_toggle(uint8_t enable);
int rp2040_send_lora_raw_frame(uint32_t dev_addr, int16_t rssi, int8_t snr, const uint8_t *payload, uint8_t size);


#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif /* RP2040_H */