#include "wifi.h"
#include "indicator_enabler.h"
#if CONFIG_GUI_CONFIG_EXTERN
#include "sd_model.h"
#endif

int indicator_model_init(void) {
#ifdef STORAGE_NVS_H
	indicator_nvs_init();
#else
	#error "Please inplement storage model"
#endif

#ifdef BTN_H
	indicator_btn_init();
#endif

#ifdef INDICATOR_DISPLAY_H
	indicator_display_init(); // lcd bl on
#endif

#ifdef RP2040_H
	esp32_rp2040_init();
	#ifdef SENSOR_H
	indicator_sensor_init();
	#endif
#endif

#if CONFIG_LORA_ENABLE
    lora_model_init();
#endif

#ifdef CMD_H
	indicator_cmd_init();
#endif

#ifdef WIFI_H
	indicator_wifi_model_init();
	#ifdef MQTT_H
	indicator_mqtt_init();
		#ifdef HA_H
	indicator_ha_model_init();
		#endif
	#endif

#endif

#ifdef INDICATOR_LORAWAN_H
	indicator_lorawan_init();
	#ifdef CMD_H
	indicator_cmd_init();
	#endif
#endif

#ifdef INDICATOR_TERMINAL_H
	indicator_terminal_init();
#endif

#if CONFIG_GUI_CONFIG_EXTERN
    /* Initialize SD card storage backend model */
    sd_model_init();
#endif
}
