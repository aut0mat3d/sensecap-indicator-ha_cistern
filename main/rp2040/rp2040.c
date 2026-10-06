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
#include <stdlib.h>
#include <string.h>

static const char* TAG = "esp32_rp2040";

#define ESP32_RP2040_TXD				  (19)
#define ESP32_RP2040_RXD				  (20)
#define ESP32_RP2040_RTS				  (UART_PIN_NO_CHANGE)
#define ESP32_RP2040_CTS				  (UART_PIN_NO_CHANGE)

#define ESP32_COMM_PORT_NUM				  (2)
#define ESP32_COMM_BAUD_RATE			  (460800)
#define ESP32_RP2040_COMM_TASK_STACK_SIZE (1024 * 8)
#define BUF_SIZE						  (2048)

static uint8_t buf[BUF_SIZE];
static uint8_t data[BUF_SIZE];

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
        case PKT_TYPE_SENSOR_ATTACHED:
            if(len < 5) return false;
            uint8_t name_len = p_data[3];
            size_t unit_len_offset = 4U + name_len;
            if(unit_len_offset >= len - 1) return false;
            uint8_t unit_len = p_data[unit_len_offset];
            return unit_len_offset + 1U + unit_len == len - 1;

        case PKT_TYPE_SENSOR_DETACHED:
            return len == 3;

        case PKT_TYPE_SENSOR_VALUE:
            return len == 7;

        case PKT_TYPE_LORA_TELEMETRY:
            return len >= 12; /* Dynamic Telemetry-Packet (variable Length) */

        default:
            return false;
    }
}

static bool _packet_handle(uint8_t* p_data, size_t len) {
    if(len < 1) return false;

    switch(p_data[0]) {
        case PKT_TYPE_SENSOR_ATTACHED:
        case PKT_TYPE_SENSOR_DETACHED:
        case PKT_TYPE_SENSOR_VALUE:
        case PKT_TYPE_LORA_TELEMETRY:
            if(!_dynamic_sensor_packet_is_valid(p_data, len)) {
                ESP_LOGW(TAG, "drop malformed dynamic sensor packet type=0x%x len=%d", p_data[0], (int)len);
                return false;
            }

            if (p_data[0] == PKT_TYPE_LORA_TELEMETRY) {
				uint32_t dev_addr = 0;
				memcpy(&dev_addr, &p_data[1], 4);

				uint32_t fcnt = 0;
				memcpy(&fcnt, &p_data[5], 4);

				uint8_t val_type = p_data[9];  /* 0=Float, 1=Int32, 2=Bool, 3=String */
				uint8_t context  = p_data[10]; /* 0=Measurement, 1=Diagnostic, 2=History */

				size_t idx = 11;
				const char *key = (const char *)&p_data[idx];
				idx += strlen(key) + 1;
				if (idx >= len) return false;

				const char *unit = (const char *)&p_data[idx];
				idx += strlen(unit) + 1;
				if (idx > len) return false;

				float val = 0.0f;
				if (val_type == 0 && (idx + 4) <= len) {
					memcpy(&val, &p_data[idx], 4);
				} else if (val_type == 1 && (idx + 4) <= len) {
					int32_t i_val = 0;
					memcpy(&i_val, &p_data[idx], 4);
					val = (float)i_val;
				} else if (val_type == 2 && idx < len) {
					val = (float)p_data[idx];
				}

				/* Post rich LoRa Telemetry Event to UI */
				struct view_data_lora_telemetry telem_evt = {0};
				telem_evt.dev_addr = dev_addr;
				telem_evt.fcnt = fcnt;
				telem_evt.val_type = val_type;
				telem_evt.context = context;
				strncpy(telem_evt.key, key, sizeof(telem_evt.key) - 1);
				strncpy(telem_evt.unit, unit, sizeof(telem_evt.unit) - 1);
				telem_evt.val = val;

				esp_event_post_to(view_event_handle, VIEW_EVENT_BASE,
								VIEW_EVENT_LORA_TELEMETRY, &telem_evt,
								sizeof(telem_evt), portMAX_DELAY);
				return true;
			}
            return true;
		case PKT_TYPE_PUSH_LORA_CFG: /* 0x35 */
            lora_model_on_rp2040_cfg_received(p_data, len);
            return true;
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
				_sensor_data_parse_handle((uint8_t*)p_data, len);
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