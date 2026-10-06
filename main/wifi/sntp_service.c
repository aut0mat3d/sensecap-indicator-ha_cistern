#include "sntp_service.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "lora/lora_model.h"

static const char *TAG = "sntp_service";
static bool s_sntp_active = false;

extern bool s_time_synced_via_ntp; /* In lora_model.c definiert */

/* Native IDF-Callback bei erfolgreichem NTP-Sync */
static void time_sync_notification_cb(struct timeval *tv) {
    time_t now = tv->tv_sec;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    s_time_synced_via_ntp = true;
    ESP_LOGI(TAG, "NTP Time Sync SUCCESS! Current local time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    /* Uhrzeit sofort an Offline-Displays im P2P-Mesh senden */
    lora_model_enqueue_time_broadcast((uint32_t)now);
}

void sntp_service_start(const char *ntp_server) {
    const char *server = (ntp_server && strlen(ntp_server) > 0) ? ntp_server : "pool.ntp.org";
    ESP_LOGI(TAG, "Starting SNTP service (Server: %s)", server);

    /* Falls bereits aktiv, vorher sauber beenden für Re-Konfiguration */
    if (s_sntp_active) {
        esp_netif_sntp_deinit();
        s_sntp_active = false;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    config.sync_cb = time_sync_notification_cb;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_sntp_active = true;
        ESP_LOGI(TAG, "SNTP service initialized successfully.");
    } else {
        ESP_LOGE(TAG, "Failed to initialize SNTP service: %s", esp_err_to_name(err));
    }
}

void sntp_service_stop(void) {
    if (s_sntp_active) {
        esp_netif_sntp_deinit();
        s_sntp_active = false;
        ESP_LOGI(TAG, "SNTP service stopped.");
    }
}