/**
 * @file rp2040.c
 * @date  29 November 2023

 * @author Spencer Yan
 *
 * @copyright © 2023, Seeed Studio
 */
#include "rp2040.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "cobs.h"
#include "driver/uart.h"
#include "view_data.h"
#include "lora/lora_model.h"
#include "alarm/alarm_model.h"
#include "debug_log.h"
#include "sntp_service.h"
#include "i18n.h"
#include "gui/i18n/gui_i18n.h"
#include "media_standards.h"
#include "gui_theme.h"
#include "mqtt_bridge.h"
#include "chart_query_scheduler.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char* TAG = "esp32_rp2040";

#define ESP32_RP2040_TXD				  (19)
#define ESP32_RP2040_RXD				  (20)
#define ESP32_RP2040_RTS				  (UART_PIN_NO_CHANGE)
#define ESP32_RP2040_CTS				  (UART_PIN_NO_CHANGE)

#define ESP32_COMM_PORT_NUM				  (2)
#define ESP32_COMM_BAUD_RATE			  (921600)
#define ESP32_RP2040_COMM_TASK_STACK_SIZE (1024 * 8)
#define BUF_SIZE						  (2048)

static uint8_t buf[BUF_SIZE];
static uint8_t data[BUF_SIZE];

/* Chunked file read from the SD card (CMD_READ_FILE / RES_FILE_DATA, stop-and-wait per chunk).
 * s_file_req_mutex serializes callers; the rx_* fields are handed between caller and UART task. */
static SemaphoreHandle_t s_file_req_sem = NULL;
static SemaphoreHandle_t s_file_req_mutex = NULL;
static char *s_file_rx_buf = NULL;
static size_t s_file_rx_max = 0;
static uint32_t s_file_rx_expected_offset = 0;
static uint16_t s_file_rx_chunk_len = 0;
static uint32_t s_file_rx_total = 0;
static bool s_file_rx_success = false;

static chart_data_callback_t s_chart_data_cb = NULL;
static SemaphoreHandle_t s_uart_tx_mutex = NULL;

static SemaphoreHandle_t s_thresh_req_sem = NULL;
static bool s_thresh_req_success = false;

static SemaphoreHandle_t s_offset_req_sem = NULL;
static bool s_offset_req_success = false;

/* UART driver event queue: used to detect RX overflows (FIFO / ring buffer full) */
#define RP2040_UART_EVT_QUEUE_LEN        16
/* RX FIFO interrupt threshold in bytes (hardware FIFO: 128 bytes) */
#define RP2040_UART_RX_FULL_THRESHOLD    64
static QueueHandle_t s_uart_evt_queue = NULL;

/* The UART task never blocks indefinitely on the event loop: telemetry is dropped (and
 * counted) when the loop is congested for longer than this, other events wait longer */
#define RP2040_EVT_POST_TIMEOUT_TELEM_MS  50
#define RP2040_EVT_POST_TIMEOUT_CTRL_MS   500

/* Alarm journal upload: entries per chunk, ack timeout, attempts before giving up */
#define RP2040_ALARM_SAVE_ENTRIES_PER_CHUNK  4
#define RP2040_ALARM_SAVE_ACK_TIMEOUT_MS     1500
#define RP2040_ALARM_SAVE_MAX_ATTEMPTS       3
_Static_assert(4 + RP2040_ALARM_SAVE_ENTRIES_PER_CHUNK * sizeof(alarm_entry_t) <= 800,
               "alarm save chunk exceeds the 800 B packet buffer of _alarm_send_save_chunk");

typedef struct {
    bool                          active;
    alarm_entry_t                *entries;       /* owned snapshot, oldest first */
    uint16_t                      count;
    uint8_t                       total_chunks;
    uint8_t                       next_chunk;    /* next chunk to send */
    bool                          awaiting_ack;  /* ack outstanding for chunk next_chunk - 1 */
    uint32_t                      sent_ms;
    uint8_t                       attempt;
    rp2040_alarm_upload_done_cb_t done_cb;
} alarm_upload_t;

static alarm_upload_t    s_upl = {0};
static SemaphoreHandle_t s_upl_mutex = NULL;
/* Last ack received (written and read by the UART task only) */
static bool    s_upl_ack_pending = false;
static uint8_t s_upl_ack_chunk = 0;
static bool    s_upl_ack_ok = false;

static int _alarm_send_save_chunk(uint8_t chunk_idx, uint8_t total_chunks, uint8_t count, const void *entries);
static void _alarm_upload_update(void);

#if RP2040_LINK_DIAG
typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_packets;
    uint32_t rx_decode_errors;   /* COBS decode failures */
    uint32_t rx_oversized;       /* fragments larger than the frame buffer */
    uint32_t rx_overflows;       /* UART FIFO / ring buffer overflows */
    uint32_t rx_line_errors;     /* frame / parity errors */
    uint32_t evt_drops;          /* events not posted (event loop congested) */
    uint32_t max_evt_wait_us;
    uint32_t max_handle_us;
    uint8_t  max_handle_type;
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t max_tx_us;          /* longest blocking uart write incl. mutex wait */
} link_diag_t;

static link_diag_t s_diag = {0};
static int64_t     s_diag_last_us = 0;

static void _link_diag_periodic(void) {
    int64_t now = esp_timer_get_time();
    if (s_diag_last_us == 0) s_diag_last_us = now;
    if (now - s_diag_last_us < (int64_t)RP2040_LINK_DIAG_PERIOD_MS * 1000) return;
    s_diag_last_us = now;

    link_diag_t d = s_diag;
    memset(&s_diag, 0, sizeof(s_diag));
    if (d.rx_packets == 0 && d.tx_packets == 0 && d.rx_overflows == 0 && d.rx_decode_errors == 0) return;

    ESP_LOGI(TAG, "[LINK] %us: RX %lu pkt / %lu B, decode err %lu, oversized %lu, overflow %lu, line err %lu | "
                  "slowest handler 0x%02X %lu us | evt drops %lu, max post wait %lu us | "
                  "TX %lu pkt / %lu B, max write %lu us",
             (unsigned)(RP2040_LINK_DIAG_PERIOD_MS / 1000),
             (unsigned long)d.rx_packets, (unsigned long)d.rx_bytes, (unsigned long)d.rx_decode_errors,
             (unsigned long)d.rx_oversized, (unsigned long)d.rx_overflows, (unsigned long)d.rx_line_errors,
             d.max_handle_type, (unsigned long)d.max_handle_us,
             (unsigned long)d.evt_drops, (unsigned long)d.max_evt_wait_us,
             (unsigned long)d.tx_packets, (unsigned long)d.tx_bytes, (unsigned long)d.max_tx_us);
}
#define LINK_DIAG(stmt) do { stmt; } while (0)
#else
#define LINK_DIAG(stmt) do { } while (0)
#endif

/**
 * @brief Post a view event from the UART task without blocking it indefinitely.
 * @return true if posted, false if the event loop stayed congested (event dropped).
 */
static bool _post_event(int32_t id, const void *data, size_t size, uint32_t timeout_ms) {
#if RP2040_LINK_DIAG
    int64_t t0 = esp_timer_get_time();
#endif
    esp_err_t err = esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, id, data, size,
                                      pdMS_TO_TICKS(timeout_ms));
#if RP2040_LINK_DIAG
    uint32_t waited = (uint32_t)(esp_timer_get_time() - t0);
    if (waited > s_diag.max_evt_wait_us) s_diag.max_evt_wait_us = waited;
    if (err != ESP_OK) s_diag.evt_drops++;
#endif
    return err == ESP_OK;
}


/**
 * @brief Thread-safe drop-in replacement for uart_write_bytes.
 */
static int _uart_write_safe(uart_port_t uart_num, const void *src, size_t size) {
    if (!src || size == 0) return -1;
    if (!s_uart_tx_mutex) {
        return uart_write_bytes(uart_num, src, size);
    }

#if RP2040_LINK_DIAG
    int64_t t0 = esp_timer_get_time();
#endif
    xSemaphoreTake(s_uart_tx_mutex, portMAX_DELAY);
    int written = uart_write_bytes(uart_num, src, size);
#if RP2040_LINK_DIAG
    uint32_t took = (uint32_t)(esp_timer_get_time() - t0);
    s_diag.tx_packets++;
    s_diag.tx_bytes += (written > 0) ? (uint32_t)written : 0;
    if (took > s_diag.max_tx_us) s_diag.max_tx_us = took;
#endif
    xSemaphoreGive(s_uart_tx_mutex);

    return written;
}

void rp2040_register_chart_data_callback(chart_data_callback_t cb) {
    s_chart_data_cb = cb;
}

int rp2040_read_file_from_sd(const char *path, char *out_buf, size_t max_len, uint32_t timeout_ms) {
    if (!path || !out_buf || max_len == 0 || !s_file_req_mutex || !s_file_req_sem) return -1;

    size_t path_len = strlen(path);
    if (path_len > 120) path_len = 120;

    xSemaphoreTake(s_file_req_mutex, portMAX_DELAY);

    uint32_t offset = 0;
    uint32_t total = 0;
    bool ok = false;

    /* One chunk per request: the RP2040 answers with at most 512 bytes plus the total size */
    for (;;) {
        s_file_rx_buf = out_buf;
        s_file_rx_max = max_len;
        s_file_rx_expected_offset = offset;
        s_file_rx_chunk_len = 0;
        s_file_rx_success = false;
        xSemaphoreTake(s_file_req_sem, 0); /* drop a stale token */

        uint8_t payload[130] = {0};
        payload[0] = PKT_TYPE_CMD_READ_FILE;
        memcpy(&payload[1], &offset, sizeof(offset));
        memcpy(&payload[5], path, path_len);

        uint8_t cobs_buf[160];
        cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, path_len + 5);
        if (ret.status != COBS_ENCODE_OK) break;
        cobs_buf[ret.out_len] = 0x00;
        _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);

        if (xSemaphoreTake(s_file_req_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE || !s_file_rx_success) {
            break;
        }
        total = s_file_rx_total;
        offset += s_file_rx_chunk_len;

        if (s_file_rx_chunk_len == 0 || offset >= total) {
            ok = true;
            break;
        }
        if (offset >= max_len - 1) {
            ESP_LOGW(TAG, "File %s truncated to %u of %lu bytes (buffer too small)",
                     path, (unsigned)offset, (unsigned long)total);
            ok = true;
            break;
        }
    }

    /* Invalidate the buffer pointer so late packets can never write into it */
    s_file_rx_buf = NULL;
    s_file_rx_max = 0;

    int result = -1;
    if (ok) {
        size_t end = (offset < max_len - 1) ? offset : (max_len - 1);
        out_buf[end] = '\0';
        result = (int)end;
    }
    xSemaphoreGive(s_file_req_mutex);
    return result;
}

/**
 * @brief sensor data parse
 * @attention should be implemented in indicator_sensor_model.c
 */
__attribute__((weak)) int _sensor_data_parse_handle(uint8_t* p_data, size_t len) {
	ESP_LOGI(TAG, "_sensor_data_parse_handle, please import the code from indicator_sensor_model");
	return -1;
}

/**
 * @brief send cmd to RP2040
 *
 * @param cmd
 * @param p_data
 * @param len
 * @return int
 */
static int _cmd_send(uint8_t cmd, void* p_data, uint8_t len) {
	uint8_t buf[40] = {0};
	uint8_t data[32] = {0};

	if(len > 31)
	{
		return -1;
	}

	uint8_t index = 1;

	data[0] = cmd;

	if(len > 0 && p_data != NULL)
	{
		memcpy(&data[1], p_data, len);
		index += len;
	}
	cobs_encode_result ret = cobs_encode(buf, sizeof(buf), data, index);
#if SENSOR_COMM_DEBUG
	ESP_LOGI(TAG, "encode status:%d, len:%d", ret.status, ret.out_len);
	for(int i = 0; i < ret.out_len; i++)
	{
		printf("0x%x ", buf[i]);
	}
	printf("\r\n");
#endif

	if(ret.status == COBS_ENCODE_OK)
	{
		buf[ret.out_len] = 0x00;
		return _uart_write_safe(ESP32_COMM_PORT_NUM, buf, ret.out_len + 1);
	}
	return -1;
}

static bool _verify_xor_checksum(const uint8_t* p_data, size_t len) {
	if(len < 2)
	{
		return false;
	}

	uint8_t checksum = 0;
	for(size_t i = 0; i < len - 1; i++)
	{
		checksum ^= p_data[i];
	}
	return checksum == p_data[len - 1];
}

static bool _dynamic_sensor_packet_is_valid(const uint8_t* p_data, size_t len) {
    if(len < 2 || !_verify_xor_checksum(p_data, len)) {
        return false;
    }

    switch(p_data[0]) {
        case PKT_TYPE_LORA_TELEMETRY:
        case PKT_TYPE_LOCAL_TELEMETRY:
            return len >= 12; /* Dynamic Telemetry-Packet (variable Length) */

        default:
            return false;
    }
}

/* Reload of the GUI files from SD (language files, media colors, theme) in a helper task, so the
 * UART receiver never blocks. Requests are merged: the files are read once the requests have
 * been quiet for GUI_RELOAD_SETTLE_MS; a request during the reload starts one more pass. */
#define GUI_RELOAD_SETTLE_MS 1000
static portMUX_TYPE      s_gui_reload_mux = portMUX_INITIALIZER_UNLOCKED;
static char              s_gui_reload_lang[16];
static volatile uint32_t s_gui_reload_req_ms;
static volatile bool     s_gui_reload_pending;
static volatile bool     s_gui_reload_running;

static void i18n_reload_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        uint32_t quiet = now - s_gui_reload_req_ms;
        if (quiet < GUI_RELOAD_SETTLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(GUI_RELOAD_SETTLE_MS - quiet));
            continue;
        }

        char lang[sizeof(s_gui_reload_lang)];
        portENTER_CRITICAL(&s_gui_reload_mux);
        bool pending = s_gui_reload_pending;
        s_gui_reload_pending = false;
        if (!pending) s_gui_reload_running = false;
        memcpy(lang, s_gui_reload_lang, sizeof(lang));
        portEXIT_CRITICAL(&s_gui_reload_mux);
        if (!pending) break;

        gui_i18n_reload_from_sd(lang);
        media_standards_load_from_sd();
        gui_theme_load_from_sd();
        gui_theme_check_media_contrast();
    }
    vTaskDelete(NULL);
}

static void trigger_async_i18n_reload(const char *lang) {
    if (!lang || strlen(lang) == 0) return;
    bool start_task;
    portENTER_CRITICAL(&s_gui_reload_mux);
    strlcpy(s_gui_reload_lang, lang, sizeof(s_gui_reload_lang));
    s_gui_reload_req_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_gui_reload_pending = true;
    start_task = !s_gui_reload_running;
    if (start_task) s_gui_reload_running = true;
    portEXIT_CRITICAL(&s_gui_reload_mux);

    if (start_task && xTaskCreate(i18n_reload_task, "i18n_load", 4096, NULL, 3, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_gui_reload_mux);
        s_gui_reload_running = false;
        portEXIT_CRITICAL(&s_gui_reload_mux);
        ESP_LOGE(TAG, "GUI file reload task not created");
    }
}

static bool _packet_handle(uint8_t* p_data, size_t len) {
    if(len < 1) return false;

    switch(p_data[0]) {
        case PKT_TYPE_LORA_TELEMETRY:
        case PKT_TYPE_LOCAL_TELEMETRY:
            if(!_dynamic_sensor_packet_is_valid(p_data, len)) {
                ESP_LOGW(TAG, "drop malformed dynamic sensor packet type=0x%x len=%d", p_data[0], (int)len);
                return false;
            }

            if (p_data[0] == PKT_TYPE_LORA_TELEMETRY || p_data[0] == PKT_TYPE_LOCAL_TELEMETRY) {
                uint32_t dev_addr = 0;
                memcpy(&dev_addr, &p_data[1], 4);

                uint32_t fcnt = 0;
                memcpy(&fcnt, &p_data[5], 4);

                uint8_t val_type = p_data[9];
                uint8_t context  = p_data[10];

                size_t idx = 11;
                const char *key = (const char *)&p_data[idx];
                idx += strlen(key) + 1;
                if (idx >= len) return false;

                const char *unit = (const char *)&p_data[idx];
                idx += strlen(unit) + 1;
                if (idx > len) return false;

                struct view_data_lora_telemetry telem_evt = {0};
                /* Tag origin based on UART command byte */
                telem_evt.origin = (p_data[0] == PKT_TYPE_LOCAL_TELEMETRY) ? 
                                    DATA_ORIGIN_LOCAL_I2C : DATA_ORIGIN_LORA;
                telem_evt.dev_addr = dev_addr;
                telem_evt.fcnt = fcnt;
                telem_evt.val_type = (lora_val_type_t)val_type;
                telem_evt.context = context;
                strlcpy(telem_evt.key, key, sizeof(telem_evt.key));
                strlcpy(telem_evt.unit, unit, sizeof(telem_evt.unit));

                /* Anytype Unpacking based on RP2040 Type Marker */
                if (val_type == LORA_VAL_TYPE_FLOAT && (idx + 4) <= len) {
                    memcpy(&telem_evt.val.f_val, &p_data[idx], 4);
                } else if (val_type == LORA_VAL_TYPE_INT && (idx + 4) <= len) {
                    memcpy(&telem_evt.val.i_val, &p_data[idx], 4);
                } else if (val_type == LORA_VAL_TYPE_BOOL && idx < len) {
                    telem_evt.val.b_val = (p_data[idx] != 0);
                } else if (val_type == LORA_VAL_TYPE_STRING && idx < len) {
                    strlcpy(telem_evt.val.str_val, (const char *)&p_data[idx], sizeof(telem_evt.val.str_val));
                }

                _post_event(VIEW_EVENT_LORA_TELEMETRY, &telem_evt, sizeof(telem_evt),
                            RP2040_EVT_POST_TIMEOUT_TELEM_MS);
                return true;
            }
            return true;
		case PKT_TYPE_PUSH_LORA_CFG: {
			if (len >= 2 && p_data[1] != 0) {
				size_t min_len = 2 + sizeof(lora_global_config_t);
				if (len >= min_len) {
					lora_global_config_t sys_glob_cfg;
					memcpy(&sys_glob_cfg, &p_data[2], sizeof(lora_global_config_t));

					/* 1. Apply System POSIX Timezone from SD */
					if (strlen(sys_glob_cfg.time_cfg.posix_timezone) > 0) {
						setenv("TZ", sys_glob_cfg.time_cfg.posix_timezone, 1);
						tzset();
						ESP_LOGI(TAG, "Applied POSIX Timezone from SD: %s", sys_glob_cfg.time_cfg.posix_timezone);
					}

					/* 2. Apply System Language from SD asynchronously */
					if (strlen(sys_glob_cfg.time_cfg.language) > 0) {
						trigger_async_i18n_reload(sys_glob_cfg.time_cfg.language);
						ESP_LOGI(TAG, "Triggered async Language load from SD: %s", sys_glob_cfg.time_cfg.language);
					}

					/* 3. Start System SNTP Service */
					const char *ntp_srv = (strlen(sys_glob_cfg.time_cfg.ntp_server) > 0) ? 
										sys_glob_cfg.time_cfg.ntp_server : "pool.ntp.org";
					sntp_service_start(ntp_srv);
				}
			}

		#if CONFIG_LORA_ENABLE
			/* Forward to LoRa RF Subsystem only if LoRa extension is enabled */
			lora_model_on_rp2040_cfg_received(p_data, len);
		#endif
			/* New configuration cycle: MQTT selection follows per device (0x46), mqtt.json is re-read */
			mqtt_bridge_on_rp2040_cfg_reload();
			return true;
		}
        case PKT_TYPE_PUSH_LORA_DEV: {
        #if CONFIG_LORA_ENABLE
            lora_model_on_rp2040_device_received(p_data, len);
        #endif
            return true;
        }
        case PKT_TYPE_PUSH_MQTT_DEV: {
            mqtt_bridge_on_rp2040_mqtt_cfg(p_data, len);
            return true;
        }
		case PKT_TYPE_RES_SD_INFO: {
			if(len >= 19) {
				struct sd_card_info info = {0};
				info.is_present = (p_data[1] != 0);
				info.is_usb_mounted = (p_data[2] != 0);
				info.state = (sd_card_state_t)(int8_t)p_data[3];

				uint32_t total_mb = 0;
				memcpy(&total_mb, &p_data[4], sizeof(uint32_t));
				info.total_mb = total_mb;

				memcpy(info.volume_label, &p_data[8], 11);
				info.volume_label[11] = '\0';

				/* Compute expected label from local ESP32 eFuse MAC */
				uint8_t mac[6] = {0};
				esp_efuse_mac_get_default(mac);
				snprintf(info.expected_label, sizeof(info.expected_label), 
						 "SCAP_%02X%02X%02X", mac[3], mac[4], mac[5]);

				_post_event(VIEW_EVENT_SD_INFO_RET, &info, sizeof(info), RP2040_EVT_POST_TIMEOUT_CTRL_MS);
				return true;
			}
			break;
		}
		case PKT_TYPE_RES_FILE_DATA: {
            /* [1] status, [2..3] chunk_len, [4..7] total, [8..11] offset, [12..] data */
            if (len >= 12) {
                uint8_t status = p_data[1];
                uint16_t chunk_len = 0;
                uint32_t total = 0, offset = 0;
                memcpy(&chunk_len, &p_data[2], sizeof(chunk_len));
                memcpy(&total, &p_data[4], sizeof(total));
                memcpy(&offset, &p_data[8], sizeof(offset));
                if ((size_t)chunk_len > len - 12) chunk_len = (uint16_t)(len - 12);

                /* Accept only the chunk the caller is waiting for (late answers are dropped) */
                if (!s_file_rx_buf || offset != s_file_rx_expected_offset) {
                    return true;
                }
                if (status == 1) {
                    size_t room = (s_file_rx_max > offset + 1) ? (s_file_rx_max - 1 - offset) : 0;
                    size_t copy = (chunk_len < room) ? chunk_len : room;
                    memcpy(s_file_rx_buf + offset, &p_data[12], copy);
                    s_file_rx_chunk_len = (uint16_t)copy;
                    s_file_rx_total = total;
                    s_file_rx_success = true;
                } else {
                    s_file_rx_success = false;
                }
                if (s_file_req_sem) {
                    xSemaphoreGive(s_file_req_sem);
                }
                return true;
            }
            break;
        }
        case PKT_TYPE_RES_TIME: {
            if (len >= 5) {
                uint32_t last_utc = 0;
                memcpy(&last_utc, &p_data[1], 4);
                ESP_LOGI(TAG, "UART Rx: Received stored LittleFS time from RP2040: %lu", (unsigned long)last_utc);
                if (last_utc >= 1700000000UL) {
                    extern void time_service_on_recovered_time(uint32_t recovered_utc);
                    time_service_on_recovered_time(last_utc);
                }
                return true;
            }
            break;
        }
		case PKT_TYPE_RES_CHART_DATA: {
            if (len >= 9) {
                uint8_t req_id = p_data[1];
                uint8_t status = p_data[2];
                uint32_t actual_interval = 0;
                memcpy(&actual_interval, &p_data[3], 4);
                uint16_t count = 0;
                memcpy(&count, &p_data[7], 2);

                const chart_data_point_t *pts = (const chart_data_point_t *)&p_data[9];

                /* Optional trailer (LTTB mode): extremes of the whole window after the points */
                chart_window_extremes_t extremes;
                const chart_window_extremes_t *p_extremes = NULL;
                size_t pts_end = 9 + (size_t)count * sizeof(chart_data_point_t);
                if ((size_t)len >= pts_end + sizeof(chart_window_extremes_t)) {
                    memcpy(&extremes, &p_data[pts_end], sizeof(extremes));
                    p_extremes = &extremes;
                }

                ESP_LOGI(TAG, "UART Rx: RES_CHART_DATA Zone %u: status=%u, count=%u, step=%lu s",
                         req_id, status, count, (unsigned long)actual_interval);

                /* Forward directly into the FIFO scheduler */
                chart_query_scheduler_on_response(req_id, status, actual_interval, count, pts, p_extremes);

                if (s_chart_data_cb) {
                    s_chart_data_cb(req_id, status, actual_interval, pts, count);
                }
                return true;
            } else {
                ESP_LOGW(TAG, "UART Rx: RES_CHART_DATA dropped, length too short (%d bytes)", (int)len);
            }
            break;
        }
		case PKT_TYPE_RES_SD_FORMAT: {
			if(len >= 2) {
				bool success = (p_data[1] != 0);
				_post_event(VIEW_EVENT_SD_FORMAT_RET, &success, sizeof(success), RP2040_EVT_POST_TIMEOUT_CTRL_MS);
				return true;
			}
			break;
		}
        case PKT_TYPE_RES_ALARM_LOAD: {
			if (len >= 4) {
				/* Payload: [1]=chunk_idx, [2]=total_chunks, [3]=count_in_chunk, [4..]=data */
				alarm_model_on_history_chunk_received(&p_data[1], len - 1);
				return true;
			}
			break;
		}
        case PKT_TYPE_RES_ALARM_SAVE: {
            if (len >= 3) {
                /* [1]=chunk_idx, [2]=status; consumed by _alarm_upload_update() in this task */
                s_upl_ack_chunk = p_data[1];
                s_upl_ack_ok = (p_data[2] != 0);
                s_upl_ack_pending = true;
                return true;
            }
            break;
        }
        case PKT_TYPE_RES_LAST_VALUE: {
            /* [1..4] dev_addr, [5..20] key, [21] status, [22..25] value, [26..29] ts,
             * [30..33] expected interval, [34..41] unit */
            if (len >= 42) {
                struct view_data_last_value lv = {0};
                memcpy(&lv.dev_addr, &p_data[1], 4);
                memcpy(lv.key, &p_data[5], 16);
                lv.key[16] = '\0';
                lv.valid = (p_data[21] != 0);
                memcpy(&lv.value, &p_data[22], 4);
                memcpy(&lv.timestamp, &p_data[26], 4);
                memcpy(&lv.expected_interval_sec, &p_data[30], 4);
                memcpy(lv.unit, &p_data[34], 8);
                lv.unit[8] = '\0';
                _post_event(VIEW_EVENT_ARCHIVE_LAST_VALUE, &lv, sizeof(lv), RP2040_EVT_POST_TIMEOUT_CTRL_MS);
                return true;
            }
            break;
        }
        case PKT_TYPE_RES_SET_THRESHOLD: {
            if (len >= 2) {
                s_thresh_req_success = (p_data[1] != 0);
                ESP_LOGI(TAG, "UART Rx: RES_SET_THRESHOLD status=%d", s_thresh_req_success ? 1 : 0);
                if (s_thresh_req_sem) {
                    xSemaphoreGive(s_thresh_req_sem);
                }
                return true;
            }
            break;
        }
        case PKT_TYPE_RES_SET_OFFSET: {
            if (len >= 2) {
                s_offset_req_success = (p_data[1] != 0);
                ESP_LOGI(TAG, "UART Rx: RES_SET_OFFSET status=%d", s_offset_req_success ? 1 : 0);
                if (s_offset_req_sem) {
                    xSemaphoreGive(s_offset_req_sem);
                }
                return true;
            }
            break;
        }
		default:
			if(len > 1)
			{
				return true;
			}
			break;
	}

	return false;
}

/**
 * @brief Drain UART driver events; on an RX overflow flush the input and resync the COBS framer.
 */
static void _uart_check_events(size_t *frag_len) {
    if (!s_uart_evt_queue) return;
    uart_event_t evt;
    while (xQueueReceive(s_uart_evt_queue, &evt, 0) == pdTRUE) {
        switch (evt.type) {
            case UART_FIFO_OVF:
            case UART_BUFFER_FULL:
                LINK_DIAG(s_diag.rx_overflows++);
                ESP_LOGW(TAG, "UART RX %s: input flushed, packets lost",
                         evt.type == UART_FIFO_OVF ? "FIFO overflow" : "ring buffer full");
                uart_flush_input(ESP32_COMM_PORT_NUM);
                xQueueReset(s_uart_evt_queue);
                *frag_len = 0;
                return;
            case UART_FRAME_ERR:
            case UART_PARITY_ERR:
                LINK_DIAG(s_diag.rx_line_errors++);
                break;
            default:
                break;
        }
    }
}

static void esp32_rp2040_comm_task(void* arg) {
    uart_config_t uart_config = {
        .baud_rate = ESP32_COMM_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int intr_alloc_flags = 0;

    /* Installed from this task: the UART ISR runs on the same core (core 1, see esp32_rp2040_init) */
    ESP_ERROR_CHECK(uart_driver_install(ESP32_COMM_PORT_NUM, BUF_SIZE * 2, 0,
                                        RP2040_UART_EVT_QUEUE_LEN, &s_uart_evt_queue, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ESP32_COMM_PORT_NUM, &uart_config));
    /* Default RX interrupt at 120 of 128 FIFO bytes leaves only ~87 us at 921600 Bd before the
     * hardware FIFO overflows; 64 bytes leave ~700 us for interrupt latency */
    ESP_ERROR_CHECK(uart_set_rx_full_threshold(ESP32_COMM_PORT_NUM, RP2040_UART_RX_FULL_THRESHOLD));
    ESP_ERROR_CHECK(uart_set_pin(ESP32_COMM_PORT_NUM, ESP32_RP2040_TXD, ESP32_RP2040_RXD,
                                 ESP32_RP2040_RTS, ESP32_RP2040_CTS));

    _cmd_send(PKT_TYPE_CMD_POWER_ON, NULL, 0);
	/* Announce ESP32 to RP2040 immediately: triggers SD check & config push */
    vTaskDelay(pdMS_TO_TICKS(100));
    rp2040_send_esp_mac(false);

    cobs_decode_result ret;
    static uint8_t frag[BUF_SIZE] = {0}; /* STATIC: keeps 2 KB off the task stack */
    size_t frag_len = 0;

    while(1)
	{
        _uart_check_events(&frag_len);

		int len = uart_read_bytes(ESP32_COMM_PORT_NUM, buf, (BUF_SIZE - 1), pdMS_TO_TICKS(20));
#if SENSOR_COMM_DEBUG
		ESP_LOGI(TAG, "len:%d", len);
#endif
        /* Drive serialized chart query FIFO queue and bucket triggers */
        chart_query_scheduler_update();
        /* Drive the acknowledged alarm journal upload (one chunk in flight at a time) */
        _alarm_upload_update();
#if RP2040_LINK_DIAG
        _link_diag_periodic();
#endif
		if(len > 0)
		{
            LINK_DIAG(s_diag.rx_bytes += (uint32_t)len);
#if SENSOR_COMM_DEBUG
			printf("recv:  ");
			for(int i = 0; i < len; i++)
			{
				printf("0x%x ", buf[i]);
			}
			printf("\r\n");
#endif
			uint8_t* p_buf_start = buf;
			uint8_t* p_buf_stop = buf + len;
			while(p_buf_start < p_buf_stop)
			{
				uint8_t* p_buf_end = p_buf_start;
				while(p_buf_end < p_buf_stop)
				{
					if(*p_buf_end == 0x00)
					{
						break;
					}
					p_buf_end++;
				}

				size_t chunk_len = p_buf_end - p_buf_start;
				if(chunk_len > 0)
				{
					if(frag_len + chunk_len > sizeof(frag))
					{
						ESP_LOGW(TAG, "drop oversized COBS fragment len=%u",
								 (unsigned)(frag_len + chunk_len));
                        LINK_DIAG(s_diag.rx_oversized++);
						frag_len = 0;
					}
					if(frag_len + chunk_len <= sizeof(frag))
					{
						memcpy(frag + frag_len, p_buf_start, chunk_len);
						frag_len += chunk_len;
					}
				}

				if(p_buf_end < p_buf_stop && *p_buf_end == 0x00)
				{
					if(frag_len > 0)
					{
						memset(data, 0, sizeof(data));
						ret = cobs_decode(data, sizeof(data), frag, frag_len);

#if SENSOR_COMM_DEBUG
						ESP_LOGI(TAG, "decode status:%d, len:%d, type:0x%x  ", ret.status,
								 ret.out_len, data[0]);
						printf("decode: ");
						for(int i = 0; i < ret.out_len; i++)
						{
							printf("0x%x ", data[i]);
						}
						printf("\r\n");
#endif
						if(ret.status == COBS_DECODE_OK)
						{
#if RP2040_LINK_DIAG
                            int64_t t0 = esp_timer_get_time();
#endif
							_packet_handle((uint8_t*)data, ret.out_len);
#if RP2040_LINK_DIAG
                            uint32_t took = (uint32_t)(esp_timer_get_time() - t0);
                            s_diag.rx_packets++;
                            if (took > s_diag.max_handle_us) {
                                s_diag.max_handle_us = took;
                                s_diag.max_handle_type = data[0];
                            }
#endif
						}
                        else
                        {
                            LINK_DIAG(s_diag.rx_decode_errors++);
                        }
					}
					frag_len = 0;
				}

				p_buf_start = (p_buf_end < p_buf_stop) ? (p_buf_end + 1) : p_buf_stop; // next message
			}
		}
	}
}

static void _sensor_shutdown(void) {
	int ret = 0;
	ret = _cmd_send(PKT_TYPE_CMD_SHUTDOWN, NULL, 0);
	if(ret <= 0)
	{
		ESP_LOGI(TAG, "sensor shutdown fail!. %d", ret);
	}
}
static void _commu_event_handler(void* handler_args, esp_event_base_t base, int32_t id,
								  void* event_data) {
	switch(id)
	{
		case VIEW_EVENT_SHUTDOWN:
			ESP_LOGI(TAG, "event: VIEW_EVENT_SHUTDOWN");
			_sensor_shutdown();
			break;
		default:
			break;
	}
}

void esp32_rp2040_init(void) {
    if (!s_uart_tx_mutex) {
        s_uart_tx_mutex = xSemaphoreCreateMutex();
    }
    if (!s_upl_mutex) {
        s_upl_mutex = xSemaphoreCreateMutex();
    }
    if (!s_file_req_mutex) {
        s_file_req_mutex = xSemaphoreCreateMutex();
    }
    if (!s_file_req_sem) {
        s_file_req_sem = xSemaphoreCreateBinary();
    }
    chart_query_scheduler_init();
    /* Core 1, priority above LVGL (6): the RGB panel's bounce-buffer ISR runs on core 0 and copies
     * 28.8 KB from PSRAM per interrupt; under render load it delayed the UART ISR long enough to
     * overflow the 128-byte hardware FIFO. The UART ISR is allocated on the installing task's core.
     * The task itself only decodes and dispatches (slow work such as the SX1262 init is deferred),
     * so running above LVGL costs the renderer very little and reception never waits for a frame. */
    xTaskCreatePinnedToCore(esp32_rp2040_comm_task, "esp32_rp2040_comm_task", ESP32_RP2040_COMM_TASK_STACK_SIZE,
                            NULL, 7, NULL, 1);
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle, VIEW_EVENT_BASE,
                                                             VIEW_EVENT_SHUTDOWN,
                                                             _commu_event_handler, NULL, NULL));
}

int rp2040_send_esp_mac(bool resync_only) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    char expected_label[12] = {0};
    snprintf(expected_label, sizeof(expected_label), "SCAP_%02X%02X%02X", mac[3], mac[4], mac[5]);

    uint8_t payload[18] = {0};
    memcpy(&payload[0], mac, 6);
    memcpy(&payload[6], expected_label, 11);
    /* [17] = 1: label resync only, the RP2040 pushes the configuration only if the label changed;
     * 0 = boot announce (config push requested) */
    payload[17] = resync_only ? 1 : 0;

    ESP_LOGI(TAG, "Syncing ESP32 MAC (%02X:%02X:%02X:%02X:%02X:%02X) and Label (%s) to RP2040",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], expected_label);

    return _cmd_send(PKT_TYPE_CMD_SET_ESP_MAC, payload, sizeof(payload));
}

int rp2040_send_sync_time(uint32_t utc_ts, uint8_t flags) {
    uint8_t payload[5];
    memcpy(&payload[0], &utc_ts, sizeof(uint32_t));
    payload[4] = flags;

    ESP_LOGI(TAG, "Syncing time to RP2040: UTC=%lu, Flags=0x%02X", (unsigned long)utc_ts, flags);
    return _cmd_send(PKT_TYPE_CMD_SYNC_TIME, payload, sizeof(payload));
}

int rp2040_get_last_time_req(void) {
    uint8_t cmd = PKT_TYPE_CMD_GET_TIME;
    ESP_LOGI(TAG, "Requesting stored offline time from RP2040 LittleFS...");
    return _cmd_send(cmd, NULL, 0);
}

int rp2040_sd_get_info_req(void) {
	ESP_LOGD(TAG, "Sending PKT_TYPE_CMD_SD_GET_INFO to RP2040");
	return _cmd_send(PKT_TYPE_CMD_SD_GET_INFO, NULL, 0);
}

int rp2040_sd_format_req(uint8_t format_type, const char* volume_label) {
    uint8_t payload[13] = {0};
    payload[0] = format_type;
    if (volume_label) {
        strncpy((char*)&payload[1], volume_label, 11);
    }
    ESP_LOGI(TAG, "Sending PKT_TYPE_CMD_SD_FORMAT (type=%d, label=%s) to RP2040", format_type, volume_label);
    return _cmd_send(PKT_TYPE_CMD_SD_FORMAT, payload, sizeof(payload));
}

int rp2040_sd_usb_msc_toggle(uint8_t enable) {
	ESP_LOGI(TAG, "Sending PKT_TYPE_CMD_USB_MSC_TOGGLE (%d) to RP2040", enable);
	return _cmd_send(PKT_TYPE_CMD_USB_MSC_TOGGLE, &enable, 1);
}

int rp2040_send_lora_raw_frame(uint32_t dev_addr, int16_t rssi, int8_t snr, const uint8_t *payload, uint8_t size) {
    uint8_t raw_data[200] = {0};
    uint8_t cobs_buf[220] = {0};

    raw_data[0] = PKT_TYPE_CMD_LORA_RAW_FRAME;
    uint8_t idx = 1;

    memcpy(&raw_data[idx], &dev_addr, 4); idx += 4;
    memcpy(&raw_data[idx], &rssi, 2);     idx += 2;
    memcpy(&raw_data[idx], &snr, 1);      idx += 1;
    raw_data[idx++] = size;

    uint8_t copy_size = (size > 180) ? 180 : size;
    if (payload && copy_size > 0) {
        memcpy(&raw_data[idx], payload, copy_size);
        idx += copy_size;
    }

    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), raw_data, idx);
    if (ret.status == COBS_ENCODE_OK) {
        cobs_buf[ret.out_len] = 0x00;
        return _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    }

    return -1;
}

int rp2040_request_chart_data(uint8_t req_id, uint32_t dev_addr, const char *key,
                              uint32_t start_ts, uint32_t end_ts, uint32_t interval_sec,
                              uint16_t max_points, float alarm_min, float alarm_max,
                              uint8_t alarm_flags, uint8_t query_mode) {
    uint8_t payload[46] = {0};
    payload[0] = PKT_TYPE_CMD_GET_CHART_DATA;
    payload[1] = req_id;
    memcpy(&payload[2], &dev_addr, 4);
    if (key) {
        strncpy((char*)&payload[6], key, 15);
    }
    memcpy(&payload[22], &start_ts, 4);
    memcpy(&payload[26], &end_ts, 4);
    memcpy(&payload[30], &interval_sec, 4);
    memcpy(&payload[34], &max_points, 2);
    memcpy(&payload[36], &alarm_min, 4);
    memcpy(&payload[40], &alarm_max, 4);
    payload[44] = alarm_flags; /* CHART_ALARM_*_IS_WARNING */
    payload[45] = query_mode;  /* CHART_QUERY_MODE_* */

    uint8_t cobs_buf[60];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status == COBS_ENCODE_OK) {
        cobs_buf[ret.out_len] = 0x00;
        int written = _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
        ESP_LOGI(TAG, "UART Tx: Req Chart Zone %u (key=%s, start=%lu, end=%lu, step=%lu s) -> %d bytes sent",
                 req_id, key ? key : "?", (unsigned long)start_ts, (unsigned long)end_ts, (unsigned long)interval_sec, written);
      
        return written;
    }
    ESP_LOGE(TAG, "UART Tx: COBS encode failed for chart request");
    return -1;
}

int rp2040_request_last_value(uint32_t dev_addr, const char *key) {
    if (!key) return -1;
    uint8_t payload[21] = {0};
    payload[0] = PKT_TYPE_CMD_GET_LAST_VALUE;
    memcpy(&payload[1], &dev_addr, 4);
    strncpy((char *)&payload[5], key, 15);

    uint8_t cobs_buf[32];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status != COBS_ENCODE_OK) return -1;
    cobs_buf[ret.out_len] = 0x00;
    return _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
}

int rp2040_alarm_set(uint16_t t_on_ms, uint16_t t_off_ms) {
    uint8_t payload[5];
    payload[0] = 0xA6; /* PKT_TYPE_CMD_ALARM_SET */
    payload[1] = (uint8_t)(t_on_ms & 0xFF);
    payload[2] = (uint8_t)((t_on_ms >> 8) & 0xFF);
    payload[3] = (uint8_t)(t_off_ms & 0xFF);
    payload[4] = (uint8_t)((t_off_ms >> 8) & 0xFF);

    uint8_t cobs_buf[16];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status == COBS_ENCODE_OK) {
        cobs_buf[ret.out_len] = 0x00;
        return _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    }
    return -1;
}

int rp2040_alarm_mute(uint32_t mute_sec) {
    uint8_t payload[5];
    payload[0] = 0xA7; /* PKT_TYPE_CMD_ALARM_MUTE */
    memcpy(&payload[1], &mute_sec, 4);

    uint8_t cobs_buf[16];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status == COBS_ENCODE_OK) {
        cobs_buf[ret.out_len] = 0x00;
        return _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    }
    return -1;
}

int rp2040_alarm_rearm(void) {
    uint8_t payload[1] = { 0xA8 }; /* PKT_TYPE_CMD_ALARM_REARM */
    uint8_t cobs_buf[8];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status == COBS_ENCODE_OK) {
        cobs_buf[ret.out_len] = 0x00;
        return _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    }
    return -1;
}

static int _alarm_send_save_chunk(uint8_t chunk_idx, uint8_t total_chunks, uint8_t count, const void *entries) {
    if (!entries && count > 0) return -1;

    size_t payload_len = 4 + (count * sizeof(alarm_entry_t));
    if (payload_len > 800) return -1; /* Guard against buffer overflow */

    uint8_t payload[800];
    payload[0] = PKT_TYPE_CMD_ALARM_SAVE;
    payload[1] = chunk_idx;
    payload[2] = total_chunks;
    payload[3] = count;

    if (count > 0) {
        memcpy(&payload[4], entries, count * sizeof(alarm_entry_t));
    }

    uint8_t cobs_buf[850];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, payload_len);
    if (ret.status == COBS_ENCODE_OK) {
        cobs_buf[ret.out_len] = 0x00;
        return _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    }

    ESP_LOGE(TAG, "COBS encode failed for alarm save chunk %u/%u", chunk_idx + 1, total_chunks);
    return -1;
}

/* ── Acknowledged alarm journal upload ─────────────────────────────────── */

int rp2040_alarm_upload_start(void *entries, uint16_t count, rp2040_alarm_upload_done_cb_t done_cb) {
    if (!s_upl_mutex || (count > 0 && !entries)) return -1;

    xSemaphoreTake(s_upl_mutex, portMAX_DELAY);
    if (s_upl.active) {
        xSemaphoreGive(s_upl_mutex);
        return -1;
    }
    uint16_t chunks = (count + RP2040_ALARM_SAVE_ENTRIES_PER_CHUNK - 1) / RP2040_ALARM_SAVE_ENTRIES_PER_CHUNK;
    s_upl.active = true;
    s_upl.entries = (alarm_entry_t *)entries;
    s_upl.count = count;
    s_upl.total_chunks = (chunks == 0) ? 1 : (uint8_t)chunks; /* empty journal: one chunk with 0 entries */
    s_upl.next_chunk = 0;
    s_upl.awaiting_ack = false;
    s_upl.sent_ms = 0;
    s_upl.attempt = 1;
    s_upl.done_cb = done_cb;
    xSemaphoreGive(s_upl_mutex);

    ESP_LOGI(TAG, "Alarm journal upload started: %u entries in %u chunks", count, s_upl.total_chunks);
    return 0;
}

bool rp2040_alarm_upload_busy(void) {
    return s_upl.active;
}

/**
 * @brief Advance the upload by at most one chunk. Runs in the UART task (acks arrive there too).
 */
static void _alarm_upload_update(void) {
    if (!s_upl_mutex) return;

    xSemaphoreTake(s_upl_mutex, portMAX_DELAY);
    if (!s_upl.active) {
        xSemaphoreGive(s_upl_mutex);
        return;
    }

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    bool finished = false;
    bool attempt_failed = false;

    if (s_upl.awaiting_ack) {
        uint8_t expected = (uint8_t)(s_upl.next_chunk - 1);
        if (s_upl_ack_pending) {
            s_upl_ack_pending = false;
            if (s_upl_ack_chunk == expected) {
                if (s_upl_ack_ok) {
                    s_upl.awaiting_ack = false;
                    finished = (s_upl.next_chunk >= s_upl.total_chunks);
                } else {
                    ESP_LOGW(TAG, "Alarm journal chunk %u rejected by RP2040", expected);
                    attempt_failed = true;
                }
            }
            /* Ack for another chunk: stale answer of an aborted attempt, ignore */
        } else if (now_ms - s_upl.sent_ms >= RP2040_ALARM_SAVE_ACK_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Alarm journal chunk %u not acknowledged within %u ms",
                     expected, (unsigned)RP2040_ALARM_SAVE_ACK_TIMEOUT_MS);
            attempt_failed = true;
        }
    }

    bool failed = false;
    if (attempt_failed) {
        if (s_upl.attempt < RP2040_ALARM_SAVE_MAX_ATTEMPTS) {
            s_upl.attempt++;
            s_upl.next_chunk = 0;      /* RP2040 restarts the staged file on chunk 0 */
            s_upl.awaiting_ack = false;
            ESP_LOGW(TAG, "Restarting alarm journal upload (attempt %u of %u)",
                     s_upl.attempt, (unsigned)RP2040_ALARM_SAVE_MAX_ATTEMPTS);
        } else {
            failed = true;
        }
    }

    if (!finished && !failed && !s_upl.awaiting_ack) {
        uint16_t first = (uint16_t)s_upl.next_chunk * RP2040_ALARM_SAVE_ENTRIES_PER_CHUNK;
        uint16_t remaining = (s_upl.count > first) ? (uint16_t)(s_upl.count - first) : 0;
        uint8_t n = (remaining < RP2040_ALARM_SAVE_ENTRIES_PER_CHUNK) ? (uint8_t)remaining
                                                                      : RP2040_ALARM_SAVE_ENTRIES_PER_CHUNK;
        if (s_upl.next_chunk == 0) s_upl_ack_pending = false;

        if (_alarm_send_save_chunk(s_upl.next_chunk, s_upl.total_chunks, n,
                                   n ? (const void *)&s_upl.entries[first] : NULL) > 0) {
            s_upl.next_chunk++;
            s_upl.awaiting_ack = true;
            s_upl.sent_ms = now_ms;
        } else {
            ESP_LOGE(TAG, "Alarm journal chunk %u could not be sent", s_upl.next_chunk);
            failed = true;
        }
    }

    rp2040_alarm_upload_done_cb_t cb = NULL;
    if (finished || failed) {
        free(s_upl.entries);
        s_upl.entries = NULL;
        s_upl.active = false;
        cb = s_upl.done_cb;
        if (finished) {
            ESP_LOGI(TAG, "Alarm journal upload complete (%u entries)", s_upl.count);
        } else {
            ESP_LOGE(TAG, "Alarm journal upload failed after %u attempts", s_upl.attempt);
        }
    }
    xSemaphoreGive(s_upl_mutex);

    if (cb) cb(finished);
}

int rp2040_alarm_request_load(void) {
    uint8_t payload[1] = { PKT_TYPE_CMD_ALARM_LOAD };
    uint8_t cobs_buf[8];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status == COBS_ENCODE_OK) {
        cobs_buf[ret.out_len] = 0x00;
        ESP_LOGI(TAG, "UART Tx: Requested alarm history load from RP2040 SD card");
        return _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    }
    return -1;
}

int rp2040_send_set_threshold(uint32_t dev_addr, const char *rule_id,
                              bool is_percent, float threshold_val, float hysteresis_val,
                              uint8_t damping, uint8_t severity, uint8_t buzzer_mode,
                              uint32_t timeout_ms) {
    if (!rule_id) return -1;

    if (!s_thresh_req_sem && timeout_ms > 0) {
        s_thresh_req_sem = xSemaphoreCreateBinary();
    } else if (s_thresh_req_sem) {
        /* Clear any stale token */
        xSemaphoreTake(s_thresh_req_sem, 0);
    }
    s_thresh_req_success = false;

    /* Build 33-byte command payload matching RP2040 packet parser */
    uint8_t payload[33] = {0};
    payload[0] = PKT_TYPE_CMD_SET_THRESHOLD;
    memcpy(&payload[1], &dev_addr, 4);
    strncpy((char *)&payload[5], rule_id, 16);
    payload[21] = is_percent ? 1 : 0;
    memcpy(&payload[22], &threshold_val, sizeof(float));
    memcpy(&payload[26], &hysteresis_val, sizeof(float));
    payload[30] = damping;
    payload[31] = severity;
    payload[32] = buzzer_mode;

    uint8_t cobs_buf[48];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status != COBS_ENCODE_OK) {
        ESP_LOGE(TAG, "COBS encode failed for CMD_SET_THRESHOLD");
        return -1;
    }
    cobs_buf[ret.out_len] = 0x00;

    int written = _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    if (written <= 0) {
        ESP_LOGE(TAG, "UART write failed for CMD_SET_THRESHOLD");
        return -1;
    }

    /* Wait for acknowledgement if requested */
    if (timeout_ms > 0 && s_thresh_req_sem) {
        if (xSemaphoreTake(s_thresh_req_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            return s_thresh_req_success ? 0 : -1;
        }
        ESP_LOGW(TAG, "Timeout waiting for RES_SET_THRESHOLD from RP2040");
        return -1;
    }

    return 0;
}

int rp2040_send_set_offset(uint32_t dev_addr, const char *key,
                           float value, const char *unit, uint32_t timeout_ms) {
    if (!key) return -1;

    if (!s_offset_req_sem && timeout_ms > 0) {
        s_offset_req_sem = xSemaphoreCreateBinary();
    } else if (s_offset_req_sem) {
        /* Clear any stale token */
        xSemaphoreTake(s_offset_req_sem, 0);
    }
    s_offset_req_success = false;

    /* Build 33-byte command payload matching RP2040 packet parser */
    uint8_t payload[33] = {0};
    payload[0] = PKT_TYPE_CMD_SET_OFFSET;
    memcpy(&payload[1], &dev_addr, 4);
    strncpy((char *)&payload[5], key, 16);
    memcpy(&payload[21], &value, sizeof(float));
    if (unit) {
        strncpy((char *)&payload[25], unit, 8);
    }

    uint8_t cobs_buf[48];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status != COBS_ENCODE_OK) {
        ESP_LOGE(TAG, "COBS encode failed for CMD_SET_OFFSET");
        return -1;
    }
    cobs_buf[ret.out_len] = 0x00;

    int written = _uart_write_safe(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    if (written <= 0) {
        ESP_LOGE(TAG, "UART write failed for CMD_SET_OFFSET");
        return -1;
    }

    /* Wait for acknowledgement if requested */
    if (timeout_ms > 0 && s_offset_req_sem) {
        if (xSemaphoreTake(s_offset_req_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
            return s_offset_req_success ? 0 : -1;
        }
        ESP_LOGW(TAG, "Timeout waiting for RES_SET_OFFSET from RP2040");
        return -1;
    }

    return 0;
}