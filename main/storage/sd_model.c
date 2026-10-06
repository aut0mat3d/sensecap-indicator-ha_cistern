#include "sd_model.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "view_data.h"
#include "rp2040.h"

typedef enum {
    SD_STATE_IDLE,
    SD_STATE_FORMATTING
} sd_state_t;

static sd_state_t s_sd_state = SD_STATE_IDLE;
static uint8_t s_format_timeout_counter = 0;

static bool s_sd_usb_msc_active = false;

bool sd_model_is_usb_msc_active(void) {
    return s_sd_usb_msc_active;
}

static const char *TAG = "sd-model-bridge";
static esp_timer_handle_t s_sd_poll_timer = NULL;

static void sd_poll_timer_cb(void* arg) {
    if (s_sd_state == SD_STATE_FORMATTING) {
        s_format_timeout_counter++;
        if (s_format_timeout_counter >= 3) { /* 9s Timeout */
            s_sd_state = SD_STATE_IDLE;
            s_format_timeout_counter = 0;
            
            bool success = false;
            ESP_LOGE(TAG, "Format request timed out!");
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SD_FORMAT_RET, &success, sizeof(success), portMAX_DELAY);
        }
        return;
    }
	// idle
    rp2040_sd_get_info_req();
}

/*
 * MicroSD hardware is physically connected to RP2040 (SPI1).
 * This module acts purely as a software bridge for LVGL events.
 */

static void sd_model_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t id, void *event_data)
{
	if(id == VIEW_EVENT_SD_INFO_REQ)
	{
		if (s_sd_state == SD_STATE_FORMATTING) return;

		ESP_LOGI(TAG, "Forwarding SD info request to RP2040 via UART Link");
		rp2040_sd_get_info_req();
	}
	else if(id == VIEW_EVENT_SD_FORMAT_REQ)
	{
		uint8_t format_type = *(uint8_t *)event_data;
		
		uint8_t mac[6] = {0};
		esp_efuse_mac_get_default(mac);
		char label[12] = {0};
		snprintf(label, sizeof(label), "SCAP_%02X%02X%02X", mac[3], mac[4], mac[5]);

		s_sd_state = SD_STATE_FORMATTING;
		s_format_timeout_counter = 0;

		ESP_LOGI(TAG, "Forwarding SD format request (type=%d, label=%s) to RP2040", format_type, label);
		rp2040_sd_format_req(format_type, label);
	}
	else if(id == VIEW_EVENT_SD_FORMAT_RET)
	{
		bool success = *(bool *)event_data;
		ESP_LOGI(TAG, "Format response received from RP2040: %s", success ? "SUCCESS" : "FAILED");

		s_sd_state = SD_STATE_IDLE;
		s_format_timeout_counter = 0;

		/* Request status refresh after format completion */
		rp2040_sd_get_info_req();
	}
	else if(id == VIEW_EVENT_SD_USB_MOUNT_REQ)
	{
		bool enable = *(bool *)event_data;
		ESP_LOGI(TAG, "Forwarding USB MSC toggle (%d) to RP2040 via UART Link", enable);
		rp2040_sd_usb_msc_toggle(enable ? 1 : 0);
	}
}

int sd_model_init(void) {
    /* Send ESP32 MAC & Expected Label right after UART boot */
    rp2040_send_esp_mac();

    /* Register Event Handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SD_INFO_REQ,
        sd_model_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SD_FORMAT_REQ,
        sd_model_event_handler, NULL, NULL));

	/* Register Event Handler for Format Result */
    ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SD_FORMAT_RET,
        sd_model_event_handler, NULL, NULL));
	
	ESP_ERROR_CHECK(esp_event_handler_instance_register_with(
        view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_SD_USB_MOUNT_REQ,
        sd_model_event_handler, NULL, NULL));	

    /* Start 5-second periodic fallback validator timer */
    const esp_timer_create_args_t timer_args = {
        .callback = &sd_poll_timer_cb,
        .name = "sd_poll_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_sd_poll_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_sd_poll_timer, 5000000)); /* 5000 ms */

    ESP_LOGI(TAG, "SD UART bridge & periodic validator initialized");
    return 0;
}