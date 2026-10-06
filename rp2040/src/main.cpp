/**
 * @file main.cpp
 * @brief Fully non-blocking execution entry point for RP2040.
 * Core 0: Real-time Sensors, Alarm Engine & Non-blocking I2C
 * Core 1: UART Link & MicroSD Storage Engine
 * @copyright © 2024-2026, Seeed Studio / SenseCAP Cistern Extension
 */
/**
 * based on:
 * @file rp2040.ino
 * @date  01 March 2024
 * @author Spencer Yan
 *
 * @note :
 * Product Wiki:
 * https://wiki.seeedstudio.com/Sensor/SenseCAP/SenseCAP_Indicator/Get_started_with_SenseCAP_Indicator/
 * System Diagram:
 * https://files.seeedstudio.com/wiki/SenseCAP/SenseCAP_Indicator/SenseCAP_Indicator_6.png
 * Grove Ports:
 * https://files.seeedstudio.com/wiki/SenseCAP/SenseCAP_Indicator/grove.png
 *
 * @copyright © 2024, Seeed Studio
 */

#include "indicator_rp2040.hpp"
#include "sdcard.hpp"
#include "lorasensors.hpp"

/* Force Earle-Philhower core to allocate a dedicated 8KB stack for Core 1 */
bool core1_separate_stack = true;

PacketSerial packetSerial;

typedef enum {
    SYS_STATE_BOOT = 0,
    SYS_STATE_SD_CHECK,
    SYS_STATE_LOAD_CFG,
    SYS_STATE_RUNNING,
    SYS_STATE_RELOAD_CFG
} system_state_t;

static system_state_t g_sys_state = SYS_STATE_BOOT;
static lora_global_config_t g_active_rf_cfg;

static bool has_sht41 = false;
static bool has_light = false;
static uint8_t g_esp32_mac[6] = {0};

/* ── Core 1 UART Packet Handler ────────────────────────────────────────── */

void onPacketReceived(const uint8_t* buffer, size_t size)
{
    if (size < 1) return;

    switch (buffer[0]) {
        case PKT_TYPE_CMD_SHUTDOWN:
            Serial.printf("RP2040: Received PKT_TYPE_CMD_SHUTDOWN - turning off sensor power\r\n");
            sensor_power_off();
            break;

        case PKT_TYPE_CMD_POWER_ON:
            sensor_power_on();
            sensor_aht_init();
            has_sht41 = sensor_sht41_init();
            sensor_sgp40_init();
            sensor_scd4x_init();
            has_light = sensor_light_init();
            break;

        case PKT_TYPE_CMD_BEEP_ON:
            beep_click(50);
            break;

        case PKT_TYPE_CMD_BEEP_OFF:
            alarm_set(0, 0);
            break;

        case PKT_TYPE_CMD_ALARM_SET: {
            if (size >= 5) {
                uint16_t tON_ms = buffer[1] | (buffer[2] << 8);
                uint16_t tOFF_ms = buffer[3] | (buffer[4] << 8);
                alarm_set(tON_ms, tOFF_ms);
            }
            break;
        }

        case PKT_TYPE_CMD_ALARM_MUTE: {
            if (size >= 5) {
                uint32_t mute_sec;
                memcpy(&mute_sec, &buffer[1], sizeof(mute_sec));
                alarm_mute(mute_sec);
            } else {
                alarm_mute(0);
            }
            break;
        }

        case PKT_TYPE_CMD_ALARM_REARM:
            alarm_rearm();
            break;

        case PKT_TYPE_CMD_RESCAN_GROVE:
            sensor_i2c_auto_detect(packetSerial);
            break;

        case PKT_TYPE_CMD_SET_ESP_MAC: {
            if (size >= 18) {
                memcpy(g_esp32_mac, &buffer[1], 6);
                char expected_label[12] = {0};
                memcpy(expected_label, &buffer[7], 11);
                sd_card_set_expected_label(expected_label);
                Serial.printf("Stored expected volume label from ESP32: %s\r\n", expected_label);
                
                /* SAFE STATUS PUSH: Trigger status evaluation without re-initializing SPI/USB */
                sd_card_trigger_status_push();
            }
            break;
        }

        case PKT_TYPE_CMD_SD_GET_INFO: {
            uint8_t is_present = 0, is_usb_mounted = 0;
            int8_t state = SD_STATE_UNCHECKED;
            uint32_t capacity_mb = 0;
            char volume_label[12] = {0};

            sd_card_get_info(is_present, is_usb_mounted, state, capacity_mb, volume_label);

            uint8_t res_buf[24] = {0};
            res_buf[0] = static_cast<uint8_t>(PKT_TYPE_RES_SD_INFO);
            res_buf[1] = is_present;
            res_buf[2] = is_usb_mounted;
            res_buf[3] = static_cast<uint8_t>(state);
            memcpy(&res_buf[4], &capacity_mb, sizeof(capacity_mb));
            memcpy(&res_buf[8], volume_label, 11);

            packetSerial.send(res_buf, 19);
            break;
        }

        case PKT_TYPE_CMD_USB_MSC_TOGGLE: {
            uint8_t enable = (size >= 2) ? buffer[1] : 0;
            Serial.printf("USB MSC Toggle request: %d\r\n", enable);
            sd_card_msc_toggle(enable != 0);
            break;
        }

        case PKT_TYPE_CMD_SD_FORMAT: {
            uint8_t format_type = (size >= 2) ? buffer[1] : 0;
            char label_buf[12] = "SCAP_UNK";
            if (size >= 3) {
                size_t label_len = size - 2;
                if (label_len > 11) label_len = 11;
                memcpy(label_buf, &buffer[2], label_len);
                label_buf[label_len] = '\0';
            }

            Serial.printf("RP2040 Rx: Received PKT_TYPE_CMD_SD_FORMAT (type=%d, label=%s)\r\n", format_type, label_buf);
            sd_card_format_start(format_type, label_buf);
            break;
        }

        case PKT_TYPE_CMD_LORA_RAW_FRAME: {
            lorasensors_process_raw_frame(packetSerial, buffer, size);
            break;
        }

        case PKT_TYPE_CMD_SET_TIME_CFG: {
            if (size >= 73) {
                char tz_buf[64] = {0};
                char fmt_buf[8] = {0};
                memcpy(tz_buf, &buffer[1], 64);
                memcpy(fmt_buf, &buffer[65], 8);

                Serial.printf("RP2040: Saving new time config to SD (TZ: %s, Format: %s)\r\n", tz_buf, fmt_buf);
                save_time_config_to_sd(sd_card_get_fs(), tz_buf, fmt_buf);
            }
            break;
        }

        default:
            break;
    }
}

/* ── Core 0 Setup & Loop (Real-time & Alarm Domain) ───────────────────── */

void setup()
{
    /* 1. MUST BE CALLED BEFORE Serial.begin(): Registers USB MSC descriptors */
    sd_card_init();

    Serial.begin(115200);

    Serial1.setRX(17);
    Serial1.setTX(16);
    Serial1.setFIFOSize(256); /* set Hardware-FIFO to max */
    Serial1.begin(460800);

    sensor_power_on();

    Wire.setSDA(20);
    Wire.setSCL(21);
    Wire.begin();

    alarm_init();

    sensor_aht_init();
    has_sht41 = sensor_sht41_init();
    sensor_sgp40_init();
    sensor_scd4x_init();
    has_light = sensor_light_init();

    sensor_i2c_auto_detect(packetSerial);
}

static NonBlockingDelay aht_delay(2000);
static NonBlockingDelay sgp40_delay(3800);
static NonBlockingDelay scd4x_delay(3800);
static NonBlockingDelay light_delay(1000);

static AHTData   data_aht = {0};
static SPG40Data data_spg = {0};
static SCD4XData data_scd = {0};
static LightData data_light = {0};

static uint16_t SPG4x_compensationRh = 0x8000;
static uint16_t SPG4x_compensationT  = 0x6666;

static enum { SHT41_IDLE, SHT41_WAITING } sht41_state = SHT41_IDLE;
static uint32_t sht41_trigger_ms = 0;

void loop()
{
    /* 1. Update Non-blocking Alarm State Machine */
    buzzer_update();

    /* 2. Sample CO2 Sensor */
    if (scd4x_delay.check() && sensor_scd4x_get(data_scd)) {
        sensor_data_send(packetSerial, PKT_TYPE_SENSOR_SCD41_CO2, static_cast<float>(data_scd.co2));
        sensor_value_send(packetSerial, PKT_SENSOR_ID_SCD41_CO2, static_cast<float>(data_scd.co2));
    }

    /* 3. Sample VOC Sensor */
    if (sgp40_delay.check() && sensor_sgp40_get(data_spg, SPG4x_compensationRh, SPG4x_compensationT)) {
        sensor_data_send(packetSerial, PKT_TYPE_SENSOR_SGP40_TVOC_INDEX, static_cast<float>(data_spg.vocIndex));
        sensor_value_send(packetSerial, PKT_SENSOR_ID_SGP40_VOC, static_cast<float>(data_spg.vocIndex));
    }

    /* 4. Non-blocking SHT41 / AHT20 Temperature & Humidity Machine */
    if (sht41_state == SHT41_IDLE) {
        if (aht_delay.check()) {
            if (has_sht41 && sensor_sht41_trigger()) {
                sht41_state = SHT41_WAITING;
                sht41_trigger_ms = millis();
            } else if (sensor_aht_get(data_aht)) {
                sensor_data_send(packetSerial, PKT_TYPE_SENSOR_SHT41_HUMIDITY, data_aht.humidity);
                sensor_value_send(packetSerial, PKT_SENSOR_ID_AHT20_HUMIDITY, data_aht.humidity);
                sensor_data_send(packetSerial, PKT_TYPE_SENSOR_SHT41_TEMP, data_aht.temperature);
                sensor_value_send(packetSerial, PKT_SENSOR_ID_AHT20_TEMP, data_aht.temperature);
            }
        }
    } else if (sht41_state == SHT41_WAITING) {
        if (millis() - sht41_trigger_ms >= 15) {
            if (sensor_sht41_read(data_aht)) {
                sensor_data_send(packetSerial, PKT_TYPE_SENSOR_SHT41_HUMIDITY, data_aht.humidity);
                sensor_value_send(packetSerial, PKT_SENSOR_ID_GROVE_BASE + 1, data_aht.humidity);
                sensor_data_send(packetSerial, PKT_TYPE_SENSOR_SHT41_TEMP, data_aht.temperature);
                sensor_value_send(packetSerial, PKT_SENSOR_ID_GROVE_BASE, data_aht.temperature);

                SPG4x_compensationRh = static_cast<uint16_t>(data_aht.humidity * 65535 / 100);
                SPG4x_compensationT  = static_cast<uint16_t>((data_aht.temperature + 45) * 65535 / 175);
            }
            sht41_state = SHT41_IDLE;
        }
    }

    /* 5. Sample Ambient Light Sensor */
    if (has_light && light_delay.check() && sensor_light_get(data_light)) {
        sensor_data_send(packetSerial, PKT_TYPE_SENSOR_LIGHT_LUX, data_light.lux);
        sensor_value_send(packetSerial, PKT_SENSOR_ID_LIGHT_LUX, data_light.lux);
    }
}

/* ── Core 1 Setup & Loop (Communication & Storage Domain) ─────────────── */

void setup1()
{
    packetSerial.setStream(&Serial1);
    packetSerial.setPacketHandler(&onPacketReceived);
    g_sys_state = SYS_STATE_BOOT;
}

void loop1()
{
    /* 1. Universal UART Communication Task */
    packetSerial.update();
    if (packetSerial.overflow()) {
        /* Buffer overflow handled cleanly */
    }

    /* 2. MicroSD Driver Worker (Card Detect ISR & Non-blocking Formatter) */
    sd_card_update(packetSerial);

    /* 3. System State Machine */
    switch (g_sys_state) {
        case SYS_STATE_BOOT:
            Serial.printf("RP2040 Core 1: System Boot Sequence Started...\r\n");
            g_sys_state = SYS_STATE_SD_CHECK;
            break;

        case SYS_STATE_SD_CHECK: {
            uint8_t present = 0, usb_mounted = 0;
            int8_t state = SD_STATE_UNCHECKED;
            uint32_t capacity_mb = 0;
            char label[12] = {0};

            sd_card_get_info(present, usb_mounted, state, capacity_mb, label);

            if (present && state >= SD_STATE_VALID_FAT32) {
                g_sys_state = SYS_STATE_LOAD_CFG;
            } else {
                Serial.printf("RP2040: No valid SD card found. Signaling NO_CONFIG to ESP32-S3...\r\n");
                lorasensors_push_config_to_esp32(packetSerial, false);
                g_sys_state = SYS_STATE_RUNNING;
            }
            break;
        }

        case SYS_STATE_LOAD_CFG: {
            Serial.printf("RP2040: Loading LoRa configuration files from MicroSD...\r\n");
            bool success = lorasensors_load_all(sd_card_get_fs());
            
            lorasensors_push_config_to_esp32(packetSerial, success);
            
            if (success) {
                g_active_rf_cfg = lorasensors_get_globals();
            }
            g_sys_state = SYS_STATE_RUNNING;
            break;
        }

        case SYS_STATE_RUNNING: {
            /* Hot-Reload Trigger: Checked when SD card is inserted or unmounted from PC */
            if (sd_card_is_msc_active()) {
                /* USB MSC active: Pause SD config re-loads */
            } else if (sd_card_needs_config_reload()) {
                g_sys_state = SYS_STATE_RELOAD_CFG;
            }
            break;
        }

        case SYS_STATE_RELOAD_CFG: {
         sd_card_clear_config_reload();
         Serial.printf("RP2040: SD card change or eject event! Re-evaluating configs...\r\n");

         uint8_t present = 0, usb_mounted = 0;
         int8_t state = SD_STATE_UNCHECKED;
         uint32_t capacity_mb = 0;
         char label[12] = {0};

         sd_card_get_info(present, usb_mounted, state, capacity_mb, label);

         if (present && state >= SD_STATE_VALID_FAT32) {
             bool success = lorasensors_load_all(sd_card_get_fs());
             lorasensors_push_config_to_esp32(packetSerial, success);
             if (success) {
                 g_active_rf_cfg = lorasensors_get_globals();
             }
         } else {
             /* Card was physically removed or unmounted mid-operation:
                Maintain active RAM device & decoder tables so decoding continues! */
             Serial.printf("RP2040: SD card not available (%d). Maintaining active RAM device/decoder tables.\r\n", state);
         }

         g_sys_state = SYS_STATE_RUNNING;
         break;
     }

        default:
            g_sys_state = SYS_STATE_RUNNING;
            break;
    }
}