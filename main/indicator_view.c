#include "indicator_enabler.h"

#include "nav.h"
#include "view_data.h"
#if CONFIG_GUI_CONFIG_EXTERN
#include "sd_view.h"
#endif

extern int indicator_display_view_init(void);

int indicator_view_init(void) {
	nav_init();

#ifdef INDICATOR_DISPLAY_H
	indicator_display_view_init();
#endif

#ifdef SENSOR_H
	view_sensor_init();
#endif

#ifdef WIFI_H
	indicator_wifi_view_init();
#endif

#ifdef HA_H
	indicator_ha_view_init();
#endif

#ifdef SETTINGS_H
	settings_view_init();
#endif

#if CONFIG_LORA_ENABLE
	lora_diag_view_init();
#endif

#if CONFIG_GUI_CONFIG_EXTERN
    /* Initialize SD card management view component */
    sd_view_init();
#endif

	return 0;
}
