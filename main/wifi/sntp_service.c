#include "sntp_service.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "rp2040.h"
#include "lora/lora_model.h"

static const char *TAG = "time_service";
static bool s_sntp_active = false;
static esp_timer_handle_t s_grace_timer = NULL;
static esp_timer_handle_t s_sync_61s_timer = NULL;

extern bool s_time_synced_via_ntp;

/* Apply valid time from NTP or LoRa P2P */
void time_service_apply_sync(uint32_t utc_sec, uint8_t flag) {
    if (utc_sec < 1700000000UL) return;

    struct timeval tv = { .tv_sec = (time_t)utc_sec, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    /* Stop 60s grace timer if still active */
    if (s_grace_timer) {
        esp_timer_stop(s_grace_timer);
    }

    /* Push immediately to RP2040 */
    rp2040_send_sync_time(utc_sec, flag);
    ESP_LOGI(TAG, "Time synced: UTC=%lu (Flag: 0x%02X)", (unsigned long)utc_sec, flag);
}

/* Callback when RP2040 returns last time from LittleFS */
void time_service_on_recovered_time(uint32_t recovered_utc) {
    time_t current = time(NULL);
    if (current >= 1700000000UL && s_time_synced_via_ntp) {
        return; /* Real NTP time already took precedence */
    }

    /* Add the 60 seconds we spent waiting for NTP */
    uint32_t dummy_time = recovered_utc + 60;
    struct timeval tv = { .tv_sec = (time_t)dummy_time, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    /* Inform RP2040 of recovered estimated time */
    rp2040_send_sync_time(dummy_time, 0x02 /* TIME_FLAG_ESTIMATED */);
    ESP_LOGW(TAG, "OFFLINE RECOVERY: Restored time from SD LittleFS: %lu (+60s -> %lu)", 
             (unsigned long)recovered_utc, (unsigned long)dummy_time);
}

/* 60-Second Grace Period Timeout Callback */
static void grace_period_timeout_cb(void *arg) {
    time_t current = time(NULL);
    if (current >= 1700000000UL) return;

    ESP_LOGW(TAG, "60s grace period expired without NTP/LoRa. Requesting last time from LittleFS...");
    rp2040_get_last_time_req();
}

/* Periodic 61-Second Sync Timer (keeps RP2040 in sync and persists time) */
static void sync_61s_timer_cb(void *arg) {
    time_t now = time(NULL);
    if (now >= 1700000000UL) {
        uint8_t flag = s_time_synced_via_ntp ? 0x01 : 0x02;
        rp2040_send_sync_time((uint32_t)now, flag);
    }
}

static void time_sync_notification_cb(struct timeval *tv) {
    time_t now = tv->tv_sec;
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    s_time_synced_via_ntp = true;
    ESP_LOGI(TAG, "NTP Time Sync SUCCESS! Current local time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    time_service_apply_sync((uint32_t)now, 0x01 /* TIME_FLAG_ACCURATE */);
    lora_model_enqueue_time_broadcast((uint32_t)now);
}

void sntp_service_start(const char *ntp_server) {
    const char *server = (ntp_server && strlen(ntp_server) > 0) ? ntp_server : "pool.ntp.org";
    ESP_LOGI(TAG, "Starting SNTP service (Server: %s)", server);

    if (s_sntp_active) {
        esp_netif_sntp_deinit();
        s_sntp_active = false;
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    config.sync_cb = time_sync_notification_cb;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_sntp_active = true;
    }

    /* Start 60-second grace timer for offline recovery */
    if (!s_grace_timer) {
        esp_timer_create_args_t grace_args = {
            .callback = &grace_period_timeout_cb,
            .name = "time_grace"
        };
        esp_timer_create(&grace_args, &s_grace_timer);
        esp_timer_start_once(s_grace_timer, 60ULL * 1000ULL * 1000ULL);
    }

    /* Start 61-second periodic sync & persistence timer */
    if (!s_sync_61s_timer) {
        esp_timer_create_args_t sync_args = {
            .callback = &sync_61s_timer_cb,
            .name = "time_61s_sync"
        };
        esp_timer_create(&sync_args, &s_sync_61s_timer);
        esp_timer_start_periodic(s_sync_61s_timer, 61ULL * 1000ULL * 1000ULL);
    }
}

void sntp_service_stop(void) {
    if (s_sntp_active) {
        esp_netif_sntp_deinit();
        s_sntp_active = false;
    }
}