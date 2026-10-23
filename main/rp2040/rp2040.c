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
#include "debug_log.h"
#include "sntp_service.h"
#include "i18n.h"
#include "gui/i18n/gui_i18n.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

static SemaphoreHandle_t s_file_req_sem = NULL;
static char *s_file_rx_buf = NULL;
static size_t s_file_rx_max = 0;
static size_t s_file_rx_len = 0;
static bool s_file_rx_success = false;

static chart_data_callback_t s_chart_data_cb = NULL;

void rp2040_register_chart_data_callback(chart_data_callback_t cb) {
    s_chart_data_cb = cb;
}

int rp2040_read_file_from_sd(const char *path, char *out_buf, size_t max_len, uint32_t timeout_ms) {
    if (!path || !out_buf || max_len == 0) return -1;

    if (!s_file_req_sem) {
        s_file_req_sem = xSemaphoreCreateBinary();
    }

    s_file_rx_buf = out_buf;
    s_file_rx_max = max_len;
    s_file_rx_len = 0;
    s_file_rx_success = false;

    /* Send read file request to RP2040 */
    uint8_t payload[130] = {0};
    payload[0] = PKT_TYPE_CMD_READ_FILE;
    size_t path_len = strlen(path);
    if (path_len > 120) path_len = 120;
    memcpy(&payload[1], path, path_len);

    cobs_encode_result ret;
    uint8_t cobs_buf[150];
    ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, path_len + 1);
    if (ret.status != COBS_ENCODE_OK) {
        s_file_rx_buf = NULL;
        s_file_rx_max = 0;
        return -1;
    }
    cobs_buf[ret.out_len] = 0x00;
    uart_write_bytes(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);

    /* Wait for response from RP2040 */
    if (xSemaphoreTake(s_file_req_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        if (s_file_rx_success) {
            return s_file_rx_len;
        }
    }

    /* Safety fallback on timeout: immediately invalidate pointers to prevent use-after-free */
    s_file_rx_buf = NULL;
    s_file_rx_max = 0;
    return -1;
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
		return uart_write_bytes(ESP32_COMM_PORT_NUM, buf, ret.out_len + 1);
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

/* Helper task to reload language files without blocking the UART receiver task */
static void i18n_reload_task(void *arg) {
    char *lang = (char *)arg;
    if (lang) {
        gui_i18n_reload_from_sd(lang);
        free(lang);
    }
    vTaskDelete(NULL);
}

static void trigger_async_i18n_reload(const char *lang) {
    if (!lang || strlen(lang) == 0) return;
    char *lang_copy = strdup(lang);
    if (lang_copy) {
        xTaskCreate(i18n_reload_task, "i18n_load", 4096, lang_copy, 3, NULL);
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

                esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
                                  VIEW_EVENT_LORA_TELEMETRY, &telem_evt,
                                  sizeof(telem_evt), portMAX_DELAY);
                return true;
            }
            return true;
		case PKT_TYPE_PUSH_LORA_CFG: { /* 0x35 */
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

				esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
								  VIEW_EVENT_SD_INFO_RET, &info,
								  sizeof(info), portMAX_DELAY);
				return true;
			}
			break;
		}
		case PKT_TYPE_RES_FILE_DATA: {
            if (len >= 4) {
                uint8_t status = p_data[1];
                uint16_t file_len = 0;
                memcpy(&file_len, &p_data[2], sizeof(file_len));

                if (status == 1 && s_file_rx_buf && s_file_rx_max > 0) {
                    size_t to_copy = (file_len < s_file_rx_max - 1) ? file_len : (s_file_rx_max - 1);
                    memcpy(s_file_rx_buf, &p_data[4], to_copy);
                    s_file_rx_buf[to_copy] = '\0';
                    s_file_rx_len = to_copy;
                    s_file_rx_success = true;
                } else {
                    s_file_rx_success = false;
                }

                /* Clear buffer pointer immediately so late packets can never corrupt heap */
                s_file_rx_buf = NULL;
                s_file_rx_max = 0;

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

                ESP_LOGI(TAG, "UART Rx: RES_CHART_DATA Zone %u: status=%u, count=%u, step=%lu s",
                         req_id, status, count, (unsigned long)actual_interval);

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
				esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
								  VIEW_EVENT_SD_FORMAT_RET, &success,
								  sizeof(success), portMAX_DELAY);
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

    ESP_ERROR_CHECK(
        uart_driver_install(ESP32_COMM_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(ESP32_COMM_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(ESP32_COMM_PORT_NUM, ESP32_RP2040_TXD, ESP32_RP2040_RXD,
                                 ESP32_RP2040_RTS, ESP32_RP2040_CTS));

    _cmd_send(PKT_TYPE_CMD_POWER_ON, NULL, 0);
	/* Announce ESP32 to RP2040 immediately: triggers SD check & config push */
    vTaskDelay(pdMS_TO_TICKS(100));
    rp2040_send_esp_mac();

    cobs_decode_result ret;
    static uint8_t frag[BUF_SIZE] = {0}; /* STATIC: Nimmt 2048 Bytes vom Stack weg! */
    size_t frag_len = 0;

    while(1)
	{
		int len = uart_read_bytes(ESP32_COMM_PORT_NUM, buf, (BUF_SIZE - 1), pdMS_TO_TICKS(20));
#if SENSOR_COMM_DEBUG
		ESP_LOGI(TAG, "len:%d", len);
#endif
		if(len > 0)
		{
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
							_packet_handle((uint8_t*)data, ret.out_len);
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
	xTaskCreate(esp32_rp2040_comm_task, "esp32_rp2040_comm_task", ESP32_RP2040_COMM_TASK_STACK_SIZE,
				NULL, 5, NULL);
	ESP_ERROR_CHECK(esp_event_handler_instance_register_with(view_event_handle, VIEW_EVENT_BASE,
															 VIEW_EVENT_SHUTDOWN,
															 _commu_event_handler, NULL, NULL));
}

int rp2040_send_esp_mac(void) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    char expected_label[12] = {0};
    snprintf(expected_label, sizeof(expected_label), "SCAP_%02X%02X%02X", mac[3], mac[4], mac[5]);

    uint8_t payload[18] = {0};
    memcpy(&payload[0], mac, 6);
    memcpy(&payload[6], expected_label, 11);

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
        return uart_write_bytes(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    }

    return -1;
}

int rp2040_request_chart_data(uint8_t req_id, uint32_t dev_addr, const char *key,
                              uint32_t start_ts, uint32_t end_ts, uint32_t interval_sec,
                              uint16_t max_points, float alarm_min, float alarm_max) {
    uint8_t payload[44] = {0};
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

    uint8_t cobs_buf[60];
    cobs_encode_result ret = cobs_encode(cobs_buf, sizeof(cobs_buf), payload, sizeof(payload));
    if (ret.status == COBS_ENCODE_OK) {
        cobs_buf[ret.out_len] = 0x00;
        int written = uart_write_bytes(ESP32_COMM_PORT_NUM, cobs_buf, ret.out_len + 1);
    /*    ESP_LOGI(TAG, "UART Tx: Req Chart Zone %u (key=%s, start=%lu, end=%lu, step=%lu s) -> %d bytes sent",
                 req_id, key ? key : "?", (unsigned long)start_ts, (unsigned long)end_ts, (unsigned long)interval_sec, written);
      */
        return written;
    }
    ESP_LOGE(TAG, "UART Tx: COBS encode failed for chart request");
    return -1;
}