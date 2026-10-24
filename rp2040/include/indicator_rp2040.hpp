/**
 * @file indicator_rp2040.hpp
 * @brief Umbrella header for RP2040 co-processor firmware.
 * @copyright © 2024-2026, Seeed Studio / SenseCAP Cistern Extension
 */
/**
 * based on:
 * @file utility.h
 * @date  29 February 2024

 * @author Spencer Yan
 *
 * @note Description of the file
 *
 * @copyright © 2024, Seeed Studio
 */

#pragma once

#include <Arduino.h>
#include <PacketSerial.h>
#include <SPI.h>
#include <SdFat.h>
#include <Wire.h>

/* RP2040 Hardware SPI1 Pin Definitions for MicroSD Card */
#define SD_PIN_DET  7
#define SD_PIN_SCK  10
#define SD_PIN_MOSI 11
#define SD_PIN_MISO 12
#define SD_PIN_CS   13

typedef enum {
    SD_STATE_UNCHECKED       = -1, /* Still unchecked / initialization pending */
    SD_STATE_MISMATCH        =  0, /* Validation failed: Wrong volume label or wrong device */
    SD_STATE_VALID_DEVICE    =  1, /* Belongs to this device, but no valid partitions */
    SD_STATE_VALID_FAT32     =  2, /* Belongs to device, FAT32 partition ready */
    SD_STATE_VALID_FAT32_LFS =  3  /* Belongs to device, FAT32 AND LittleFS partitions ready */
} sd_card_state_t;

/* Packet protocol types for UART communication with ESP32-S3 */
enum pkt_type {
    PKT_TYPE_NONE                 = 0,
    PKT_TYPE_CMD_GET_SYS_INFO     = 0x20,
    PKT_TYPE_RES_SYS_INFO         = 0x21,
    PKT_TYPE_CMD_SET_ESP_MAC      = 0x22,  /* Payload: 6-byte raw MAC + 11-byte expected label */

    PKT_TYPE_CMD_COLLECT_INTERVAL = 0xA0,  /* uint32_t ms */
    PKT_TYPE_CMD_BEEP_ON          = 0xA1,  /* Legacy single beep click */
    PKT_TYPE_CMD_BEEP_OFF         = 0xA2,
    PKT_TYPE_CMD_SHUTDOWN         = 0xA3,
    PKT_TYPE_CMD_POWER_ON         = 0xA4,
    PKT_TYPE_CMD_RESCAN_GROVE     = 0xA5,

    /* Cistern Alarm System Packets */
    PKT_TYPE_CMD_ALARM_SET        = 0xA6,  /* Payload: uint16_t tON_ms, uint16_t tOFF_ms */
    PKT_TYPE_CMD_ALARM_MUTE       = 0xA7,  /* Payload: uint32_t mute_sec (0=indefinite) */
    PKT_TYPE_CMD_ALARM_REARM      = 0xA8,  /* Payload: none */

    /* SD Card Management Packets */
    PKT_TYPE_CMD_SD_GET_INFO      = 0x30,  /* Request card size, presence & validation state */
    PKT_TYPE_RES_SD_INFO          = 0x31,  /* Response: present, usb_mounted, state, total_mb, label */
    PKT_TYPE_CMD_SD_FORMAT        = 0x32,  /* Request format (0=LittleFS, 1=Full SD) */
    PKT_TYPE_RES_SD_FORMAT        = 0x33,  /* Response: success (uint8_t) */
    PKT_TYPE_CMD_USB_MSC_TOGGLE   = 0x34,  /* Toggle USB Mass Storage Mode */

    PKT_TYPE_CMD_LORA_RAW_FRAME   = 0x36,  /* Payload: DevAddr(4B) + RSSI(2B) + SNR(1B) + Size(1B) + Payload(NB) */
    PKT_TYPE_CMD_SET_TIME_CFG     = 0x37, /* Payload: posix_tz[64] + time_format[8] */
    PKT_TYPE_CMD_READ_FILE        = 0x38, /* Payload: path string (null-terminated) */
    PKT_TYPE_RES_FILE_DATA        = 0x39, /* Payload: status(uint8_t: 0=Error/Not found, 1=Data) + length(uint16_t) + data[] */
    PKT_TYPE_CMD_SYNC_TIME        = 0x3A, /* Payload: utc_ts(uint32_t, 4B) + flags(uint8_t, 1B) */
    PKT_TYPE_CMD_GET_CHART_DATA   = 0x3B, /* Request: zone query from ESP32 */
    PKT_TYPE_RES_CHART_DATA       = 0x3C, /* Response: packed array of chart_data_point_t */
    PKT_TYPE_CMD_GET_TIME         = 0x3D, /* Request last saved UTC timestamp from LittleFS */
    PKT_TYPE_RES_TIME             = 0x3E, /* Response: uint32_t last_utc */
    

    /* Dynamic Sensor Registry Packets */
    PKT_TYPE_LORA_TELEMETRY  = 0xBB,  /* Payload: DevAddr(4B) + SensorID(1B) + FloatVal(4B) + Checksum(1B) */
    PKT_TYPE_LOCAL_TELEMETRY = 0xBC,  /* Onboard and Grove I2C sensors */

};


/* Sensor Data Structures */
typedef struct {
    float humidity;
    float temperature;
} AHTData;

typedef struct {
    uint16_t srawVoc;
    int32_t  vocIndex;
} SPG40Data;

typedef struct {
    uint16_t co2;
    float    humidity;
    float    temperature;
} SCD4XData;

typedef struct {
    float lux;
    bool  valid;
} LightData;

/* Non-blocking Sensor Auto-Detection & Drivers */
void sensor_power_on(void);
void sensor_power_off(void);

void sensor_aht_init(void);
bool sensor_aht_get(AHTData& data);

bool sensor_sht41_init(void);
bool sensor_sht41_trigger(void);
bool sensor_sht41_read(AHTData& data);

void sensor_sgp40_init(void);
bool sensor_sgp40_get(SPG40Data& data, const uint16_t compensationRh, const uint16_t compensationT);

void sensor_scd4x_init(void);
bool sensor_scd4x_get(SCD4XData& data);

bool sensor_light_init(void);
bool sensor_light_get(LightData& data);

/* Non-blocking Cistern Alarm Engine */
void alarm_init(void);/* Extended LoRa Telemetry Helper with explicit DevAddr origin */
void lora_telemetry_send(PacketSerial& packetSerial, uint32_t dev_addr, uint8_t sensor_id, float data);
void alarm_set(uint16_t tON_ms, uint16_t tOFF_ms);
void alarm_mute(uint32_t mute_sec);
void alarm_rearm(void);
void beep_click(uint16_t duration_ms = 50);
void buzzer_update(void);

/* SD Card Management Functions (Core 1) */
void sd_card_init(void);
void sd_card_get_info(uint8_t &is_present, uint8_t &is_usb_mounted, int8_t &state, uint32_t &capacity_mb, char* volume_label);
bool sd_card_format(uint8_t format_type, const char* new_label);

/* Non-blocking Delay Helper */
class NonBlockingDelay {
   private:
    unsigned long previousMillis;
    unsigned long interval;

   public:
    NonBlockingDelay(unsigned long interval) : interval(interval), previousMillis(0) {}

    bool check()
    {
        unsigned long currentMillis = millis();
        if (currentMillis - previousMillis >= interval) {
            previousMillis = currentMillis;
            return true;
        }
        return false;
    }

    void setInterval(unsigned long newInterval) { interval = newInterval; }
};

