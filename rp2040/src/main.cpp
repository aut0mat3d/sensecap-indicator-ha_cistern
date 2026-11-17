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
#include "data_storage.hpp"
#include "sensor_detect.hpp"
#include "virtual_decoder.hpp"
#include "debug_log.hpp"
#include "pico/mutex.h"
#include <sys/time.h>

/* Mutex protecting shared UART (packetSerial), virtual decoder & data storage across cores */
static mutex_t g_comm_mutex;

/* Force Earle-Philhower core to allocate a dedicated 8KB stack for Core 1 */
bool core1_separate_stack = true;

PacketSerial packetSerial;
TelemetryQueue g_telemetry_tx_queue;

typedef enum {
    SYS_STATE_BOOT = 0,
    SYS_STATE_SD_CHECK,
    SYS_STATE_LOAD_CFG,
    SYS_STATE_STREAM_DEV_CFG,
    SYS_STATE_RUNNING,
    SYS_STATE_RELOAD_CFG
} system_state_t;

static system_state_t g_sys_state = SYS_STATE_BOOT;
static lora_global_config_t g_active_rf_cfg;

static bool has_sht41 = false;
static bool has_light = false;
static uint8_t g_esp32_mac[6] = {0};


/**
 * @brief Strictly validate the semantic integrity of an alarm entry.
 * @return true if all fields are within plausible specification, false otherwise.
 */
static bool is_valid_alarm_entry(const alarm_entry_t *entry) {
    if (!entry) return false;

    /* 1. Timestamp check: must be 0 (uptime fallback) or valid modern epoch (2017 - 2049) */
    if (entry->timestamp != 0 && (entry->timestamp < 1500000000UL || entry->timestamp > 2500000000UL)) {
        return false;
    }

    /* 2. Severity check: 0=DANGER, 1=WARNING, 2=INFO */
    if (entry->severity > 2) {
        return false;
    }

    /* 3. State check: 0=INCOMING, 1=GOING, 2=MUTED, 3=RETRIGGERED, 4=ACKNOWLEDGED */
    if (entry->state > 4) {
        return false;
    }

    /* 4. String validation: source must be null-terminated and not empty */
    bool source_null = false;
    for (size_t i = 0; i < ALARM_SOURCE_MAX_LEN; i++) {
        if (entry->source[i] == '\0') {
            source_null = true;
            break;
        }
    }
    if (!source_null || entry->source[0] == '\0') {
        return false;
    }

    /* 5. String validation: message must be null-terminated and not empty */
    bool msg_null = false;
    for (size_t i = 0; i < ALARM_MESSAGE_MAX_LEN; i++) {
        if (entry->message[i] == '\0') {
            msg_null = true;
            break;
        }
    }
    if (!msg_null || entry->message[0] == '\0') {
        return false;
    }

    return true;
}

/* ── Core 1 UART Packet Handler ────────────────────────────────────────── */

void onPacketReceived(const uint8_t* buffer, size_t size)
{
    if (size < 1) return;

    /* Lock communication, virtual decoder and shared memory during packet processing */
    mutex_enter_blocking(&g_comm_mutex);

    switch (buffer[0]) {
        case PKT_TYPE_CMD_SHUTDOWN:
            LogSerial.printf("RP2040: Received PKT_TYPE_CMD_SHUTDOWN - turning off sensor power\r\n");
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
        
        case PKT_TYPE_CMD_SYNC_TIME: {
            if (size >= 6) {
                uint32_t ts = 0;
                memcpy(&ts, &buffer[1], sizeof(ts));
                uint8_t flags = buffer[5];

                if (ts >= 1700000000UL) {
                    struct timeval tv = { .tv_sec = (time_t)ts, .tv_usec = 0 };
                    settimeofday(&tv, NULL);
                    data_storage_set_time_valid(true, flags);
                    data_storage_save_time(ts); // <--- Auf LittleFS sichern!

                    LogSerial.printf("RP2040: Time synchronized & saved! UTC: %lu, Source: %s\r\n",
                                  (unsigned long)ts,
                                  (flags & 0x01) ? "NTP/LoRa (Accurate)" : "RTC/Uptime (Estimated)");
                } else {
                    LogSerial.printf("RP2040: Ignored invalid time sync (%lu < 1700000000)\r\n", (unsigned long)ts);
                }
            }
            break;
        }

        case PKT_TYPE_CMD_GET_TIME: {
            uint32_t last_ts = data_storage_get_saved_time();
            uint8_t res_buf[5];
            res_buf[0] = static_cast<uint8_t>(PKT_TYPE_RES_TIME);
            memcpy(&res_buf[1], &last_ts, sizeof(last_ts));
            packetSerial.send(res_buf, sizeof(res_buf));
            LogSerial.printf("RP2040: Sent stored LittleFS time (%lu) to ESP32\r\n", (unsigned long)last_ts);
            break;
        }

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
                LogSerial.printf("RP2040: Alarm set -> tON=%u ms, tOFF=%u ms\r\n", tON_ms, tOFF_ms);
            }
            break;
        }

        case PKT_TYPE_CMD_ALARM_MUTE: {
            if (size >= 5) {
                uint32_t mute_sec;
                memcpy(&mute_sec, &buffer[1], sizeof(mute_sec));
                alarm_mute(mute_sec);
                LogSerial.printf("RP2040: Alarm muted for %lu s\r\n", (unsigned long)mute_sec);
            } else {
                alarm_mute(0);
                LogSerial.printf("RP2040: Alarm muted indefinitely\r\n");
            }
            break;
        }

        case PKT_TYPE_CMD_ALARM_REARM:
            alarm_rearm();
            LogSerial.printf("RP2040: Alarm re-armed\r\n");
            break;

        case PKT_TYPE_CMD_ALARM_SAVE: {
            /* Payload structure from ESP32:
             * [1]: chunk_idx (uint8_t)
             * [2]: total_chunks (uint8_t)
             * [3]: count (uint8_t)
             * [4..]: raw alarm entries (count * sizeof(alarm_entry_t))
             */
            if (size < 4) break;

            uint8_t chunk_idx    = buffer[1];
            uint8_t total_chunks = buffer[2];
            uint8_t count        = buffer[3];
            size_t payload_len   = size - 4;

            static bool     s_save_active = false;
            static uint8_t  s_save_expected_chunk = 0;
            static uint8_t  s_save_total_chunks = 0;
            static uint16_t s_staged_entries = 0;

            /* Check 1: Verify exact byte payload length matches entry size */
            size_t expected_len = (size_t)count * sizeof(alarm_entry_t);
            if (payload_len != expected_len) {
                LogSerial.printf("RP2040: Alarm save chunk %u REJECTED (payload len %u != expected %u)\r\n",
                              chunk_idx, (unsigned)payload_len, (unsigned)expected_len);
                if (s_save_active) {
                    sd_card_get_fs().remove("/alarm_history.tmp");
                    s_save_active = false;
                }
                break;
            }

            /* Check 2: Semantic validation of every entry inside this chunk */
            const alarm_entry_t* incoming_entries = reinterpret_cast<const alarm_entry_t*>(&buffer[4]);
            bool chunk_valid = true;
            for (uint8_t i = 0; i < count; i++) {
                if (!is_valid_alarm_entry(&incoming_entries[i])) {
                    LogSerial.printf("RP2040: Alarm save chunk %u entry %u REJECTED (corrupt fields)\r\n", chunk_idx, i);
                    chunk_valid = false;
                    break;
                }
            }
            if (!chunk_valid) {
                if (s_save_active) {
                    sd_card_get_fs().remove("/alarm_history.tmp");
                    s_save_active = false;
                }
                break;
            }

            /* Check 3: Strictly enforce chunk sequence */
            if (chunk_idx == 0) {
                s_save_active = true;
                s_save_expected_chunk = 1;
                s_save_total_chunks = total_chunks;
                s_staged_entries = count;

                File32 f = sd_card_get_fs().open("/alarm_history.tmp", O_WRITE | O_CREAT | O_TRUNC);
                if (f) {
                    uint16_t entry_sz = sizeof(alarm_entry_t);
                    uint16_t dummy_total = 0;
                    f.write(reinterpret_cast<const uint8_t*>(&entry_sz), 2);
                    f.write(reinterpret_cast<const uint8_t*>(&dummy_total), 2);
                    if (payload_len > 0) {
                        f.write(&buffer[4], payload_len);
                    }
                    f.close();
                } else {
                    s_save_active = false;
                    LogSerial.printf("RP2040: Failed to create /alarm_history.tmp\r\n");
                    break;
                }
            } else {
                if (!s_save_active || chunk_idx != s_save_expected_chunk || total_chunks != s_save_total_chunks) {
                    LogSerial.printf("RP2040: Alarm save aborted: sequence mismatch (expected %u of %u, got %u of %u)\r\n",
                                  s_save_expected_chunk, s_save_total_chunks, chunk_idx, total_chunks);
                    if (s_save_active) {
                        sd_card_get_fs().remove("/alarm_history.tmp");
                        s_save_active = false;
                    }
                    break;
                }

                File32 f = sd_card_get_fs().open("/alarm_history.tmp", O_WRITE | O_APPEND);
                if (f) {
                    if (payload_len > 0) {
                        f.write(&buffer[4], payload_len);
                    }
                    f.close();
                    s_staged_entries += count;
                    s_save_expected_chunk++;
                } else {
                    sd_card_get_fs().remove("/alarm_history.tmp");
                    s_save_active = false;
                    break;
                }
            }

            /* Final chunk: finalize total count and atomically commit file to SD */
            if (chunk_idx + 1 >= total_chunks && s_save_active) {
                File32 f = sd_card_get_fs().open("/alarm_history.tmp", O_RDWR);
                if (f) {
                    f.seek(2);
                    f.write(reinterpret_cast<const uint8_t*>(&s_staged_entries), 2);
                    f.close();

                    sd_card_get_fs().remove("/alarm_history.bin");
                    sd_card_get_fs().rename("/alarm_history.tmp", "/alarm_history.bin");
                    LogSerial.printf("RP2040: Atomically saved verified alarm history (%u entries, %u B each)\r\n",
                                  s_staged_entries, (unsigned)sizeof(alarm_entry_t));
                }
                s_save_active = false;
            }
            break;
        }

        case PKT_TYPE_CMD_ALARM_LOAD: {
            File32 f = sd_card_get_fs().open("/alarm_history.bin", O_READ);
            if (!f || f.size() < 4) {
                if (f) f.close();
                uint8_t res_buf[4] = { static_cast<uint8_t>(PKT_TYPE_RES_ALARM_LOAD), 0, 1, 0 };
                packetSerial.send(res_buf, sizeof(res_buf));
                LogSerial.printf("RP2040: No alarm history on SD, dispatched empty response\r\n");
                break;
            }

            uint16_t file_entry_size = 0;
            uint16_t file_total_entries = 0;
            f.read(reinterpret_cast<uint8_t*>(&file_entry_size), 2);
            f.read(reinterpret_cast<uint8_t*>(&file_total_entries), 2);

            /* Check A: Purge completely incompatible legacy/corrupted files (e.g. old 62-byte fragments) */
            if (file_entry_size != sizeof(alarm_entry_t)) {
                f.close();
                sd_card_get_fs().remove("/alarm_history.bin");
                LogSerial.printf("RP2040: Purged incompatible alarm history file (entry_size %u != %u)\r\n",
                              file_entry_size, (unsigned)sizeof(alarm_entry_t));
                uint8_t res_buf[4] = { static_cast<uint8_t>(PKT_TYPE_RES_ALARM_LOAD), 0, 1, 0 };
                packetSerial.send(res_buf, sizeof(res_buf));
                break;
            }

            /* Check B: Empty history */
            if (file_total_entries == 0) {
                f.close();
                uint8_t res_buf[4] = { static_cast<uint8_t>(PKT_TYPE_RES_ALARM_LOAD), 0, 1, 0 };
                packetSerial.send(res_buf, sizeof(res_buf));
                LogSerial.printf("RP2040: Alarm history empty (0 entries), dispatched empty response\r\n");
                break;
            }

            /* Check C: Clamp to actual readable file size to prevent read overruns */
            uint32_t file_data_size = f.size() - 4;
            uint16_t max_readable = (uint16_t)(file_data_size / sizeof(alarm_entry_t));
            if (file_total_entries > max_readable) {
                file_total_entries = max_readable;
            }

            /* Pass 1: Scan and validate individual entries */
            uint16_t valid_count = 0;
            uint16_t corrupt_count = 0;
            alarm_entry_t test_entry;

            for (uint16_t i = 0; i < file_total_entries; i++) {
                if (f.read(&test_entry, sizeof(alarm_entry_t)) == sizeof(alarm_entry_t)) {
                    if (is_valid_alarm_entry(&test_entry)) {
                        valid_count++;
                    } else {
                        corrupt_count++;
                        LogSerial.printf("RP2040: Corrupt alarm entry detected at index %u\r\n", i);
                    }
                } else {
                    corrupt_count++;
                }
            }

            /* Self-healing: If corrupt entries were found, rewrite file keeping ONLY valid entries */
            if (corrupt_count > 0) {
                LogSerial.printf("RP2040: Self-healing history: pruning %u corrupt entries, keeping %u valid\r\n",
                              corrupt_count, valid_count);

                File32 f_heal = sd_card_get_fs().open("/alarm_history.tmp", O_WRITE | O_CREAT | O_TRUNC);
                if (f_heal) {
                    uint16_t entry_sz = sizeof(alarm_entry_t);
                    f_heal.write(reinterpret_cast<const uint8_t*>(&entry_sz), 2);
                    f_heal.write(reinterpret_cast<const uint8_t*>(&valid_count), 2);

                    f.seek(4);
                    for (uint16_t i = 0; i < file_total_entries; i++) {
                        if (f.read(&test_entry, sizeof(alarm_entry_t)) == sizeof(alarm_entry_t)) {
                            if (is_valid_alarm_entry(&test_entry)) {
                                f_heal.write(reinterpret_cast<const uint8_t*>(&test_entry), sizeof(alarm_entry_t));
                            }
                        }
                    }
                    f_heal.close();
                    f.close();

                    sd_card_get_fs().remove("/alarm_history.bin");
                    sd_card_get_fs().rename("/alarm_history.tmp", "/alarm_history.bin");

                    /* Reopen healed file for transmission */
                    f = sd_card_get_fs().open("/alarm_history.bin", O_READ);
                    if (f) f.seek(4);
                } else {
                    f.seek(4);
                }
            } else {
                f.seek(4);
            }

            /* Pass 2: Stream only verified valid entries to ESP32 */
            if (!f || valid_count == 0) {
                if (f) f.close();
                uint8_t res_buf[4] = { static_cast<uint8_t>(PKT_TYPE_RES_ALARM_LOAD), 0, 1, 0 };
                packetSerial.send(res_buf, sizeof(res_buf));
                break;
            }

            LogSerial.printf("RP2040: Streaming %u verified alarms to ESP32...\r\n", valid_count);

            const uint8_t entries_per_chunk = 1;
            uint8_t total_chunks = valid_count;
            static uint8_t res_buf[256];
            uint8_t chunk_idx = 0;

            for (uint16_t i = 0; i < valid_count && f.available() >= (int)sizeof(alarm_entry_t); i++) {
                alarm_entry_t entry;
                if (f.read(&entry, sizeof(alarm_entry_t)) == sizeof(alarm_entry_t)) {
                    res_buf[0] = static_cast<uint8_t>(PKT_TYPE_RES_ALARM_LOAD);
                    res_buf[1] = chunk_idx++;
                    res_buf[2] = total_chunks;
                    res_buf[3] = 1;
                    memcpy(&res_buf[4], &entry, sizeof(alarm_entry_t));

                    packetSerial.send(res_buf, 4 + sizeof(alarm_entry_t));

                    if (chunk_idx < total_chunks) {
                        delay(15); /* Controlled 15 ms pacing between chunks */
                    }
                }
            }
            f.close();
            LogSerial.printf("RP2040: Alarm history streaming complete.\r\n");
            break;
        }


        case PKT_TYPE_CMD_SET_ESP_MAC: {
            if (size >= 18) {
                memcpy(g_esp32_mac, &buffer[1], 6);
                char expected_label[12] = {0};
                memcpy(expected_label, &buffer[7], 11);
                sd_card_set_expected_label(expected_label);
                LogSerial.printf("Stored expected volume label from ESP32: %s\r\n", expected_label);
                
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

        case PKT_TYPE_CMD_READ_FILE: {
            if (size < 2) break;

            char file_path[128] = {0};
            size_t path_len = size - 1;
            if (path_len > sizeof(file_path) - 1) path_len = sizeof(file_path) - 1;
            memcpy(file_path, &buffer[1], path_len);
            file_path[path_len] = '\0';

            LogSerial.printf("RP2040: File request from ESP32: %s\r\n", file_path);

            uint8_t res_buf[1024];
            res_buf[0] = static_cast<uint8_t>(PKT_TYPE_RES_FILE_DATA);

            File32 f = sd_card_get_fs().open(file_path, O_READ);
            if (!f) {
                LogSerial.printf("RP2040: File not found or read error: %s\r\n", file_path);
                res_buf[1] = 0; // Status: Failed / Not found
                uint16_t len = 0;
                memcpy(&res_buf[2], &len, sizeof(len));
                packetSerial.send(res_buf, 4);
                break;
            }

            uint16_t bytes_read = f.read(&res_buf[4], sizeof(res_buf) - 4);
            f.close();

            res_buf[1] = 1; // Status: Success
            memcpy(&res_buf[2], &bytes_read, sizeof(bytes_read));

            packetSerial.send(res_buf, 4 + bytes_read);
            LogSerial.printf("RP2040: Sent %u bytes for %s\r\n", bytes_read, file_path);
            break;
        }

        case PKT_TYPE_CMD_GET_CHART_DATA: {
            /* Payload-Struktur:
             * [1]: req_id (uint8_t)
             * [2..5]: dev_addr (uint32_t)
             * [6..21]: key (char[16], null-terminated)
             * [22..25]: start_ts (uint32_t)
             * [26..29]: end_ts (uint32_t)
             * [30..33]: interval_sec (uint32_t)
             * [34..35]: max_points (uint16_t)
             * [36..39]: alarm_min (float)
             * [40..43]: alarm_max (float)
             */
            if (size < 44) {
                LogSerial.printf("[RP2040] CMD_GET_CHART_DATA error: packet too short (%u bytes)\r\n", (unsigned)size);
                break;
            }

            uint8_t  req_id       = buffer[1];
            uint32_t dev_addr     = 0;
            memcpy(&dev_addr, &buffer[2], 4);
            char key[16] = {0};
            memcpy(key, &buffer[6], 16);
            key[15] = '\0';

            uint32_t start_ts, end_ts, interval_sec;
            uint16_t max_points;
            float alarm_min, alarm_max;

            memcpy(&start_ts, &buffer[22], 4);
            memcpy(&end_ts, &buffer[26], 4);
            memcpy(&interval_sec, &buffer[30], 4);
            memcpy(&max_points, &buffer[34], 2);
            memcpy(&alarm_min, &buffer[36], 4);
            memcpy(&alarm_max, &buffer[40], 4);

            /* Device suchen, um Decoder-Namen für den Pfad zu ermitteln */
            const lora_device_config_t *dev = lorasensors_find_device(dev_addr);
            const char *decoder = dev ? dev->decoder : "EM500-SWL";

            char channel_key[64];
            snprintf(channel_key, sizeof(channel_key), "%08lX.%s.%s", (unsigned long)dev_addr, decoder, key);

            /*
            LogSerial.printf("[RP2040] Chart Query: Zone %u, Key: %s, Time: %lu..%lu, Step: %lu s, MaxPts: %u\r\n",
                          req_id, channel_key, (unsigned long)start_ts, (unsigned long)end_ts, 
                          (unsigned long)interval_sec, (unsigned)max_points);
*/
            uint32_t actual_interval = interval_sec;
            size_t pt_count = 0;
            chart_data_point_t *pts = data_storage_query_chart(
                channel_key, start_ts, end_ts, interval_sec, max_points, alarm_min, alarm_max, &actual_interval, &pt_count);
/*
            LogSerial.printf("[RP2040] Chart Result: Zone %u returned %u points (actual step: %lu s)\r\n",
                          req_id, (unsigned)pt_count, (unsigned long)actual_interval);
*/            
            /* Antwort aufbauen:
             * [0]: PKT_TYPE_RES_CHART_DATA (0x3C)
             * [1]: req_id (Zone ID)
             * [2]: status (0 = OK, 1 = Empty / Not Found)
             * [3..6]: actual_interval (uint32_t)
             * [7..8]: count (uint16_t)
             * [9..]: chart_data_point_t array (count * 9 Bytes)
             */
            static uint8_t res_buf[512];
            res_buf[0] = static_cast<uint8_t>(PKT_TYPE_RES_CHART_DATA);
            res_buf[1] = req_id;
            res_buf[2] = (pts && pt_count > 0) ? 0 : 1;
            memcpy(&res_buf[3], &actual_interval, 4);

            uint16_t safe_count = (pt_count > 40) ? 40 : (uint16_t)pt_count;
            memcpy(&res_buf[7], &safe_count, 2);

            size_t payload_size = 9;
            if (pts && safe_count > 0) {
                size_t bytes_to_copy = safe_count * sizeof(chart_data_point_t);
                if (payload_size + bytes_to_copy <= sizeof(res_buf)) {
                    memcpy(&res_buf[payload_size], pts, bytes_to_copy);
                    payload_size += bytes_to_copy;
                }
            }

            packetSerial.send(res_buf, payload_size);
/*            LogSerial.printf("[RP2040] Sent RES_CHART_DATA: %u bytes (status: %u)\r\n", 
                          (unsigned)payload_size, res_buf[2]);
*/
            if (pts) free(pts);
            break;
        }

        case PKT_TYPE_CMD_USB_MSC_TOGGLE: {
            uint8_t enable = (size >= 2) ? buffer[1] : 0;
            LogSerial.printf("USB MSC Toggle request: %d\r\n", enable);
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

            LogSerial.printf("RP2040 Rx: Received PKT_TYPE_CMD_SD_FORMAT (type=%d, label=%s)\r\n", format_type, label_buf);
            bool format_success = sd_card_format_start(format_type, label_buf);
            
            // Send response back to ESP32-S3
            uint8_t res_buf[2] = {static_cast<uint8_t>(PKT_TYPE_RES_SD_FORMAT), format_success};
            packetSerial.send(res_buf, sizeof(res_buf));
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

                /* Apply runtime timezone change */
                setenv("TZ", tz_buf, 1);
                tzset();

                LogSerial.printf("RP2040: Applied & saved new time config (TZ: %s, Format: %s)\r\n", tz_buf, fmt_buf);
                save_time_config_to_sd(sd_card_get_fs(), tz_buf, fmt_buf);
            }
            break;
        }

        case PKT_TYPE_CMD_SET_THRESHOLD: {
            /* Payload structure from ESP32:
             * [1..4]:   dev_addr (uint32_t)
             * [5..20]:  rule_id (char[16], null-terminated)
             * [21]:     is_percent (uint8_t / bool)
             * [22..25]: threshold_val (float)
             * [26..29]: hysteresis_val (float)
             * [30]:     damping (uint8_t)
             * [31]:     severity (uint8_t)
             * [32]:     buzzer_mode (uint8_t)
             */
            if (size < 33) {
                LogSerial.printf("RP2040: CMD_SET_THRESHOLD payload too short (%u bytes)\r\n", (unsigned)size);
                uint8_t res_buf[2] = { static_cast<uint8_t>(PKT_TYPE_RES_SET_THRESHOLD), 0 };
                packetSerial.send(res_buf, sizeof(res_buf));
                break;
            }

            uint32_t dev_addr = 0;
            memcpy(&dev_addr, &buffer[1], 4);

            char rule_id[17] = {0};
            memcpy(rule_id, &buffer[5], 16);
            rule_id[16] = '\0';

            bool is_percent = (buffer[21] != 0);

            float threshold_val = 0.0f;
            float hysteresis_val = 0.0f;
            memcpy(&threshold_val, &buffer[22], sizeof(float));
            memcpy(&hysteresis_val, &buffer[26], sizeof(float));

            uint8_t damping = buffer[30];
            uint8_t severity = buffer[31];
            uint8_t buzzer_mode = buffer[32];

            LogSerial.printf("RP2040: Rx CMD_SET_THRESHOLD: Dev=0x%08lX '%s' Val=%.2f Hyst=%.2f Pct=%d Damp=%u Sev=%u Buzz=%u\r\n",
                          (unsigned long)dev_addr, rule_id, threshold_val, hysteresis_val,
                          is_percent ? 1 : 0, damping, severity, buzzer_mode);

            bool success = lorasensors_save_threshold_to_sd(
                sd_card_get_fs(), dev_addr, rule_id,
                is_percent, threshold_val, hysteresis_val,
                damping, severity, buzzer_mode
            );

            /* Respond back to ESP32: [0x41, success] */
            uint8_t res_buf[2] = { static_cast<uint8_t>(PKT_TYPE_RES_SET_THRESHOLD), static_cast<uint8_t>(success ? 1 : 0) };
            packetSerial.send(res_buf, sizeof(res_buf));
            break;
        }

        case PKT_TYPE_CMD_SET_OFFSET: {
            /* Payload structure from ESP32:
             * [1..4]:   dev_addr (uint32_t)
             * [5..20]:  key (char[16], null-terminated)
             * [21..24]: value (float)
             * [25..32]: unit (char[8], null-terminated)
             */
            if (size < 33) {
                LogSerial.printf("RP2040: CMD_SET_OFFSET payload too short (%u bytes)\r\n", (unsigned)size);
                uint8_t res_buf[2] = { static_cast<uint8_t>(PKT_TYPE_RES_SET_OFFSET), 0 };
                packetSerial.send(res_buf, sizeof(res_buf));
                break;
            }

            uint32_t dev_addr = 0;
            memcpy(&dev_addr, &buffer[1], 4);

            char key[17] = {0};
            memcpy(key, &buffer[5], 16);
            key[16] = '\0';

            float value = 0.0f;
            memcpy(&value, &buffer[21], sizeof(float));

            char unit[9] = {0};
            memcpy(unit, &buffer[25], 8);
            unit[8] = '\0';

            LogSerial.printf("RP2040: Rx CMD_SET_OFFSET: Dev=0x%08lX Key='%s' Val=%.2f Unit='%s'\r\n",
                          (unsigned long)dev_addr, key, value, unit);

            bool success = lorasensors_save_offset_to_sd(
                sd_card_get_fs(), dev_addr, key, value, unit
            );

            /* Respond back to ESP32: [0x43, success] */
            uint8_t res_buf[2] = { static_cast<uint8_t>(PKT_TYPE_RES_SET_OFFSET), static_cast<uint8_t>(success ? 1 : 0) };
            packetSerial.send(res_buf, sizeof(res_buf));
            break;
        }

        default:
            break;
    }

    mutex_exit(&g_comm_mutex);
}

/* ── Core 0 Setup & Loop (Real-time & Alarm Domain) ───────────────────── */

void setup()
{
    /* Initialize multi-core protection mutex first */
    mutex_init(&g_comm_mutex);

    /* 1. MUST BE CALLED BEFORE Serial.begin(): Registers USB MSC descriptors */
    sd_card_init();

    Serial.begin(115200);

    Serial1.setRX(17);
    Serial1.setTX(16);
    Serial1.setFIFOSize(4096); /* set Hardware-FIFO to max */
    Serial1.begin(921600);

    sensor_power_on();

    Wire.setSDA(20);
    Wire.setSCL(21);
    Wire.begin();

    // Initialize sensor detection (non-blocking I2C scan)
    sensor_detect_init();

    alarm_init();

    sensor_aht_init();
    has_sht41 = sensor_sht41_init();
    sensor_sgp40_init();
    sensor_scd4x_init();
    has_light = sensor_light_init();

    mutex_enter_blocking(&g_comm_mutex);
    mutex_exit(&g_comm_mutex);
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
    /* 0. Non-blocking I2C sensor detection */
    sensor_detect_i2c_scan_step();
    sensor_detect_print_summary_periodic();  // Print summary every 31 seconds

    /* 1. Update Non-blocking Alarm State Machine */
    buzzer_update();

    /* 2. Sample CO2 Sensor (SCD41 - DevAddr 0x46) */
    if (sensor_detect_is_builtin_present("SCD41_CO2")) {
        if (scd4x_delay.check() && sensor_scd4x_get(data_scd)) {
            mutex_enter_blocking(&g_comm_mutex);
            lorasensors_dispatch_local(packetSerial, 0x46, "SCD41_CO2", static_cast<float>(data_scd.co2), "ppm");
            lorasensors_dispatch_local(packetSerial, 0x46, "SCD41_TEMP", data_scd.temperature, "°C");
            lorasensors_dispatch_local(packetSerial, 0x46, "SCD41_HUMIDITY", data_scd.humidity, "%");
            mutex_exit(&g_comm_mutex);
        }
    }

    /* 3. Sample VOC Sensor (SGP40 - DevAddr 0x47) */
    if (sensor_detect_is_builtin_present("SGP40_VOC")) {
        if (sgp40_delay.check() && sensor_sgp40_get(data_spg, SPG4x_compensationRh, SPG4x_compensationT)) {
            mutex_enter_blocking(&g_comm_mutex);
            lorasensors_dispatch_local(packetSerial, 0x47, "SGP40_TVOC_INDEX", static_cast<float>(data_spg.vocIndex), "");
            mutex_exit(&g_comm_mutex);
        }
    }

    /* 4. Non-blocking SHT41 (DevAddr 0x45) / AHT20 Temperature & Humidity */
    if (sensor_detect_is_builtin_present("SHT41_TEMP")) {
        if (sht41_state == SHT41_IDLE) {
            if (aht_delay.check()) {
                if (has_sht41 && sensor_sht41_trigger()) {
                    sht41_state = SHT41_WAITING;
                    sht41_trigger_ms = millis();
                }
            }
        } else if (sht41_state == SHT41_WAITING) {
            if (millis() - sht41_trigger_ms >= 15) {
                if (sensor_sht41_read(data_aht)) {
                    mutex_enter_blocking(&g_comm_mutex);
                    lorasensors_dispatch_local(packetSerial, 0x45, "SHT41_TEMP", data_aht.temperature, "°C");
                    lorasensors_dispatch_local(packetSerial, 0x45, "SHT41_HUMIDITY", data_aht.humidity, "%");
                    mutex_exit(&g_comm_mutex);

                    SPG4x_compensationRh = static_cast<uint16_t>(data_aht.humidity * 65535 / 100);
                    SPG4x_compensationT  = static_cast<uint16_t>((data_aht.temperature + 45) * 65535 / 175);
                }
                sht41_state = SHT41_IDLE;
            }
        }
    }

    /* 5. Sample Ambient Light Sensor (DevAddr 0x48) */
    if (sensor_detect_is_builtin_present("LIGHT_LUX")) {
        if (has_light && light_delay.check() && sensor_light_get(data_light)) {
            mutex_enter_blocking(&g_comm_mutex);
            lorasensors_dispatch_local(packetSerial, 0x48, "LIGHT_LUX", data_light.lux, "lux");
            mutex_exit(&g_comm_mutex);
        }
    }
}

/* ── Core 1 Setup & Loop (Communication & Storage Domain) ─────────────── */

void setup1()
{
    packetSerial.setStream(&Serial1);
    packetSerial.setPacketHandler(&onPacketReceived);
    g_sys_state = SYS_STATE_BOOT;
    
    // Initialize data storage
    data_storage_init(&sd_card_get_fs());
    
    // Initialize virtual decoder system & telemetry TX queue
    virtual_decoder_init();
    telemetry_tx_init();
}

void loop1()
{
    /* 1. Universal UART Communication Task */
    packetSerial.update();
    if (packetSerial.overflow()) {
        /* Buffer overflow handled cleanly */
    }

    /* 2. Dispatch queued telemetry with controlled 8 ms pacing */
    telemetry_tx_process_queue(packetSerial);

    /* 3. MicroSD Driver Worker (Card Detect ISR & Non-blocking Formatter) */
    mutex_enter_blocking(&g_comm_mutex);
    sd_card_update(packetSerial);
    mutex_exit(&g_comm_mutex);

    /* 4. System State Machine */
    switch (g_sys_state) {
        case SYS_STATE_BOOT:
            LogSerial.printf("RP2040 Core 1: System Boot Sequence Started...\r\n");
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
                LogSerial.printf("RP2040: No valid SD card found. Signaling NO_CONFIG to ESP32-S3...\r\n");
                mutex_enter_blocking(&g_comm_mutex);
                lorasensors_push_globals(packetSerial, false);
                mutex_exit(&g_comm_mutex);
                g_sys_state = SYS_STATE_RUNNING;
            }
            break;
        }

        case SYS_STATE_LOAD_CFG: {
            LogSerial.printf("RP2040: Loading LoRa configuration files from MicroSD...\r\n");
            mutex_enter_blocking(&g_comm_mutex);
            bool success = lorasensors_load_all(sd_card_get_fs());
            /* Push only globals (0x35) first */
            lorasensors_push_globals(packetSerial, success);
            mutex_exit(&g_comm_mutex);
            
            if (success) {
                g_active_rf_cfg = lorasensors_get_globals();
                data_storage_load_devices(&sd_card_get_fs());
                /* Transition to non-blocking streaming state */
                g_sys_state = SYS_STATE_STREAM_DEV_CFG;
            } else {
                g_sys_state = SYS_STATE_RUNNING;
            }
            break;
        }

        case SYS_STATE_STREAM_DEV_CFG: {
            static NonBlockingDelay dev_stream_timer(15); /* 15 ms pacing between device chunks */
            static size_t current_stream_idx = 0;

            if (dev_stream_timer.check()) {
                mutex_enter_blocking(&g_comm_mutex);
                bool has_more = lorasensors_stream_single_device(packetSerial, current_stream_idx);
                mutex_exit(&g_comm_mutex);

                if (has_more) {
                    current_stream_idx++;
                } else {
                    current_stream_idx = 0;
                    g_sys_state = SYS_STATE_RUNNING;
                    LogSerial.printf("RP2040: All device chunks streamed successfully.\r\n");
                }
            }
            break;
        }

        case SYS_STATE_RUNNING: {
            /* Hot-Reload Trigger: Checked when SD card is inserted or unmounted from PC */
            if (sd_card_is_msc_active()) {
                /* USB MSC active: Pause SD config re-loads */
            } else if (sd_card_needs_config_reload()) {
                g_sys_state = SYS_STATE_RELOAD_CFG;
            }
            
            /* Data storage periodic tasks */
            static uint32_t last_bucket_check_ms = 0;
            static uint32_t last_hourly_hour = 0;
            static uint32_t last_daily_day = 0;
            uint32_t now_ms = millis();
            
            /* 1. Process RAM buckets every 1000ms: flushes to roundtrip.csv on wall-clock grid */
            if (now_ms - last_bucket_check_ms >= 1000) {
                last_bucket_check_ms = now_ms;
                mutex_enter_blocking(&g_comm_mutex);
                data_storage_process_buckets();
                mutex_exit(&g_comm_mutex);
            }

            /* 2. Wall-clock based hourly and daily aggregation */
            time_t now_unix = time(NULL);
            if (now_unix >= 1700000000UL) {
                uint32_t cur_hour = (uint32_t)(now_unix / 3600);
                if (last_hourly_hour == 0) {
                    last_hourly_hour = cur_hour;
                } else if (cur_hour != last_hourly_hour) {
                    last_hourly_hour = cur_hour;
                    mutex_enter_blocking(&g_comm_mutex);
                    data_storage_aggregate_hourly();
                    mutex_exit(&g_comm_mutex);
                }

                /* Trigger daily aggregation at local midnight */
                struct tm ltm_now;
                localtime_r(&now_unix, &ltm_now);
                uint32_t cur_day = (uint32_t)ltm_now.tm_mday;

                if (last_daily_day == 0) {
                    last_daily_day = cur_day;
                } else if (cur_day != last_daily_day) {
                    last_daily_day = cur_day;
                    mutex_enter_blocking(&g_comm_mutex);
                    data_storage_aggregate_daily();
                    mutex_exit(&g_comm_mutex);
                }
            }
            
            break;
        }

        case SYS_STATE_RELOAD_CFG: {
            sd_card_clear_config_reload();
            LogSerial.printf("RP2040: SD card change or eject event! Re-evaluating configs...\r\n");

            uint8_t present = 0, usb_mounted = 0;
            int8_t state = SD_STATE_UNCHECKED;
            uint32_t capacity_mb = 0;
            char label[12] = {0};

            sd_card_get_info(present, usb_mounted, state, capacity_mb, label);

            if (present && state >= SD_STATE_VALID_FAT32) {
                mutex_enter_blocking(&g_comm_mutex);
                bool success = lorasensors_load_all(sd_card_get_fs());
                lorasensors_push_globals(packetSerial, success);
                mutex_exit(&g_comm_mutex);
                if (success) {
                    g_active_rf_cfg = lorasensors_get_globals();
                    /* Load data storage channels from SD JSONs into RAM registry */
                    data_storage_load_devices(&sd_card_get_fs());
                    /* Transition to streaming state to dispatch individual devices */
                    g_sys_state = SYS_STATE_STREAM_DEV_CFG;
                    break;
                }
            } else {
                /* Card was physically removed or unmounted mid-operation:
                   Maintain active RAM device & decoder tables so decoding continues! */
                LogSerial.printf("RP2040: SD card not available (%d). Maintaining active RAM device/decoder tables.\r\n", state);
            }

            g_sys_state = SYS_STATE_RUNNING;
            break;
        }
    }
}