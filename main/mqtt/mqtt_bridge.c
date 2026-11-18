/**
 * @file mqtt_bridge.c
 * @brief MQTT publisher with Home Assistant MQTT discovery (see mqtt_bridge.h).
 *
 * Threading: one bridge task owns the MQTT client and the entity table. The UART receive task
 * (packets 0x35 / 0x46), the view event loop (telemetry, Wi-Fi) and the esp-mqtt task only copy
 * their data into a staging table (spinlock) or into the bridge queue (no wait), so none of
 * them can be stalled by the network.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "mqtt_bridge.h"
#include "mqtt_ha_classes.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "alarm/alarm_evaluator.h"
#include "gui/i18n/gui_i18n.h"
#include "lora/lora_model.h"
#include "view_data.h"

static const char *TAG = "mqtt_bridge";

/* ── Limits & timing ──────────────────────────────────────────────────── */

#define MQTT_SEL_MAX_DEVICES      16      /* = MAX_LORA_DEVICES (RP2040 device table) */
#define MQTT_SEL_MAX_KEYS         10      /* = MAX_MQTT_KEYS (rp2040/src/lorasensors.hpp) */
#define MQTT_PKT_ENTRY_SIZE       21      /* key[16] + flags(1) + expire_after_sec(4) */
#define MQTT_KEY_FLAG_VALUE       0x01
#define MQTT_KEY_FLAG_ALARM       0x02

#define MQTT_MAX_ENTITIES         192
#define MQTT_MAX_COLLECTED        192
#define MQTT_TOPIC_LEN            128
#define MQTT_CFG_FILE_MAX         2048
#define MQTT_CA_FILE_MAX          4096

#define CFG_SETTLE_MS             2500    /* quiet time after the last 0x35/0x46 before applying */
#define SYNC_COLLECT_MS           3000    /* time to receive our retained discovery configs */
#define ALARM_POLL_MS             1000
#define QUEUE_LEN                 32
#define TASK_STACK                6144
#define TASK_PRIO                 3

#define LOCAL_DEV_ADDR_MAX        0x7F    /* onboard / Grove I2C sensors use their I2C address */

/* ── Configuration (/config/mqtt.json) ────────────────────────────────── */

typedef struct {
    bool     enabled;
    char     broker_uri[128];
    char     username[64];
    char     password[64];
    char     client_id[48];
    char     ca_file[64];
    char     base_topic[48];
    char     discovery_prefix[48];
    uint16_t keepalive_sec;
    /* No reconnect needed for these: */
    char     node_name[48];
    bool     discovery;
    bool     retain_states;
    uint16_t local_interval_sec;
} mqtt_bridge_cfg_t;

/* ── MQTT selection staging (written by the UART task) ────────────────── */

typedef struct {
    char     key[16];
    uint8_t  flags;
    uint32_t expire_after_sec;
} sel_key_t;

typedef struct {
    uint32_t  dev_addr;
    uint8_t   count;
    sel_key_t keys[MQTT_SEL_MAX_KEYS];
} sel_dev_t;

EXT_RAM_BSS_ATTR static sel_dev_t s_sel[MQTT_SEL_MAX_DEVICES];
static uint8_t       s_sel_count;
static portMUX_TYPE  s_sel_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_cfg_dirty;
static volatile uint32_t s_cfg_dirty_ms;

/* ── Entities (bridge task only) ──────────────────────────────────────── */

typedef enum {
    ENT_VALUE = 0,
    ENT_ALARM,
    ENT_RSSI,
    ENT_SNR,
} entity_kind_t;

typedef struct {
    uint32_t dev_addr;
    char     key[16];
    uint8_t  kind;               /* entity_kind_t */
    bool     known;              /* value type (and unit) known: discovery can be published */
    bool     is_numeric;
    bool     cfg_published;      /* discovery sent on the current connection */
    char     unit[12];           /* unit as sent by the RP2040 */
    uint32_t expire_after_sec;
    int8_t   alarm_sent;         /* last published alarm severity, -2 = nothing published yet */
    bool     has_pending;        /* throttled local value waiting for its slot */
    char     pending[40];
    uint32_t last_pub_ms;
    uint32_t last_fcnt;          /* RSSI / SNR: frame of the last publication */
    bool     fcnt_valid;
} mqtt_entity_t;

static mqtt_entity_t *s_ent;          /* PSRAM, MQTT_MAX_ENTITIES */
static size_t         s_ent_count;
static char         (*s_collected)[MQTT_TOPIC_LEN]; /* PSRAM, MQTT_MAX_COLLECTED */
static size_t         s_collected_count;
static bool           s_collected_overflow;

/* ── Queue ────────────────────────────────────────────────────────────── */

typedef enum {
    Q_TELEMETRY = 0,
    Q_CONNECTED,
    Q_DISCONNECTED,
    Q_MESSAGE,
    Q_NETWORK,
} q_type_t;

typedef struct {
    uint8_t type;
    union {
        struct {
            uint32_t dev_addr;
            uint32_t fcnt;
            uint8_t  val_type;
            uint8_t  context;
            char     key[24];
            char     unit[12];
            union {
                float   f;
                int32_t i;
                bool    b;
                char    s[32];
            } v;
        } telem;
        struct {
            char    topic[MQTT_TOPIC_LEN];
            char    data[16];
            uint8_t data_len;
            bool    topic_truncated;
        } msg;
        bool network_up;
    };
} q_item_t;

static QueueHandle_t s_queue;

/* ── State ────────────────────────────────────────────────────────────── */

static mqtt_bridge_cfg_t        s_cfg;
static bool                     s_cfg_loaded;
static char                     s_node_id[24];
static char                     s_status_topic[MQTT_TOPIC_LEN];
static char                    *s_ca_pem;          /* PSRAM, kept while the client exists */
static esp_mqtt_client_handle_t s_client;
static volatile bool            s_connected;
static volatile bool            s_network_up;
static bool                     s_syncing;
static uint32_t                 s_sync_end_ms;
static uint32_t                 s_last_alarm_poll_ms;

/* Counters for the console */
static volatile uint32_t s_cnt_published;
static volatile uint32_t s_cnt_dropped;
static volatile uint32_t s_cnt_discovery;
static volatile uint32_t s_cnt_removed;

static uint32_t now_ms(void) {
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static bool is_local_dev(uint32_t dev_addr) {
    return dev_addr <= LOCAL_DEV_ADDR_MAX;
}

/* Replace every character outside [a-z0-9_-] by '_' (lowercase), for topic levels and ids */
static void sanitize_id(char *s) {
    for (; *s; s++) {
        char c = (char)tolower((unsigned char)*s);
        *s = (isalnum((unsigned char)c) || c == '_' || c == '-') ? c : '_';
    }
}

/* Topic level / HA object of a device: "lora_<dev_eui or dev_addr>" or "i2c_<addr>" */
static void dev_object_id(uint32_t dev_addr, char *out, size_t len) {
    if (is_local_dev(dev_addr)) {
        snprintf(out, len, "i2c_%02lx", (unsigned long)dev_addr);
        return;
    }
    const lora_device_config_t *dev = lora_model_get_device_by_addr(dev_addr);
    if (dev && dev->dev_eui[0]) {
        snprintf(out, len, "lora_%s", dev->dev_eui);
    } else {
        snprintf(out, len, "lora_%08lx", (unsigned long)dev_addr);
    }
    sanitize_id(out);
}

static void dev_name(uint32_t dev_addr, char *out, size_t len) {
    const lora_device_config_t *dev = lora_model_get_device_by_addr(dev_addr);
    if (dev && dev->device_name[0]) {
        strlcpy(out, dev->device_name, len);
    } else {
        snprintf(out, len, "0x%08lX", (unsigned long)dev_addr);
    }
}

/* Entity label: i18n "telemetry.<key>" without the trailing colon, else the key with spaces */
static void key_label(const char *key, char *out, size_t len) {
    char i18n_key[48];
    snprintf(i18n_key, sizeof(i18n_key), "telemetry.%s", key);
    const char *txt = gui_i18n_get(i18n_key);
    if (txt && strcmp(txt, i18n_key) != 0) {
        strlcpy(out, txt, len);
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == ':' || out[n - 1] == ' ')) out[--n] = '\0';
        if (n > 0) return;
    }
    strlcpy(out, key, len);
    for (char *p = out; *p; p++) {
        if (*p == '_') *p = ' ';
    }
    if (out[0]) out[0] = (char)toupper((unsigned char)out[0]);
}

static void entity_state_topic(const mqtt_entity_t *e, char *out, size_t len) {
    char obj[48];
    dev_object_id(e->dev_addr, obj, sizeof(obj));
    snprintf(out, len, "%s/%s/%s/%s%s", s_cfg.base_topic, s_node_id, obj, e->key,
             (e->kind == ENT_ALARM) ? "/alarm" : "");
}

static void entity_object_id(const mqtt_entity_t *e, char *out, size_t len) {
    char obj[48];
    dev_object_id(e->dev_addr, obj, sizeof(obj));
    snprintf(out, len, "%s_%s%s", obj, e->key, (e->kind == ENT_ALARM) ? "_alarm" : "");
    sanitize_id(out);
}

static void entity_discovery_topic(const mqtt_entity_t *e, char *out, size_t len) {
    char obj[96];
    entity_object_id(e, obj, sizeof(obj));
    snprintf(out, len, "%s/sensor/%s/%s/config", s_cfg.discovery_prefix, s_node_id, obj);
}

/* Unique id: sensor entities of LoRa devices are independent of the forwarding SenseCAP,
 * reception quality and onboard sensors belong to this SenseCAP. */
static void entity_unique_id(const mqtt_entity_t *e, char *out, size_t len) {
    char obj[96];
    entity_object_id(e, obj, sizeof(obj));
    bool node_bound = is_local_dev(e->dev_addr) || e->kind == ENT_RSSI || e->kind == ENT_SNR;
    if (node_bound) {
        snprintf(out, len, "%s_%s", s_node_id, obj);
    } else {
        strlcpy(out, obj, len);
    }
}

static void format_float(float v, char *out, size_t len) {
    if (isnan(v) || isinf(v)) {
        snprintf(out, len, "unknown");
        return;
    }
    snprintf(out, len, "%.3f", (double)v);
    char *dot = strchr(out, '.');
    if (!dot) return;
    char *end = out + strlen(out) - 1;
    while (end > dot && *end == '0') *end-- = '\0';
    if (end == dot) *end = '\0';
    if (strcmp(out, "-0") == 0) strlcpy(out, "0", len);
}

static const char *severity_state(int sev) {
    switch (sev) {
        case 0:  return "danger";
        case 1:  return "warning";
        case 2:  return "info";
        default: return "ok";
    }
}

static mqtt_entity_t *find_entity(uint32_t dev_addr, const char *key, uint8_t kind) {
    for (size_t i = 0; i < s_ent_count; i++) {
        mqtt_entity_t *e = &s_ent[i];
        if (e->dev_addr == dev_addr && e->kind == kind && strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

/* ── Publishing (bridge task) ─────────────────────────────────────────── */

static void publish(const char *topic, const char *payload, int qos, bool retain) {
    if (!s_client || !s_connected) return;
    int len = payload ? (int)strlen(payload) : 0;
    /* enqueue: never blocks on the socket, esp-mqtt sends from its own task */
    int id = esp_mqtt_client_enqueue(s_client, topic, payload ? payload : "", len, qos, retain ? 1 : 0, true);
    if (id < 0) {
        s_cnt_dropped++;
        ESP_LOGW(TAG, "Publish to %s dropped (outbox full)", topic);
    } else {
        s_cnt_published++;
    }
}

static cJSON *node_device_json(void) {
    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_node_id));
    if (s_cfg.node_name[0]) {
        cJSON_AddStringToObject(dev, "name", s_cfg.node_name);
    } else {
        char name[40];
        snprintf(name, sizeof(name), "SenseCAP %s", s_node_id + strlen("sensecap_"));
        cJSON_AddStringToObject(dev, "name", name);
    }
    cJSON_AddStringToObject(dev, "manufacturer", "Seeed Studio");
    cJSON_AddStringToObject(dev, "model", "SenseCAP Indicator");
    cJSON_AddStringToObject(dev, "sw_version", esp_app_get_description()->version);
    return dev;
}

static cJSON *lora_device_json(uint32_t dev_addr) {
    char obj[48], name[40];
    dev_object_id(dev_addr, obj, sizeof(obj));
    dev_name(dev_addr, name, sizeof(name));
    const lora_device_config_t *cfg = lora_model_get_device_by_addr(dev_addr);

    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(obj));
    cJSON_AddStringToObject(dev, "name", name);
    if (cfg && cfg->decoder[0]) cJSON_AddStringToObject(dev, "model", cfg->decoder);
    if (cfg && cfg->dev_sn[0]) cJSON_AddStringToObject(dev, "serial_number", cfg->dev_sn);
    cJSON_AddStringToObject(dev, "via_device", s_node_id);
    return dev;
}

static void publish_discovery(mqtt_entity_t *e) {
    if (!s_cfg.discovery || !e->known) return;

    char topic[MQTT_TOPIC_LEN], state_topic[MQTT_TOPIC_LEN], uid[128], label[40], name[80];
    entity_discovery_topic(e, topic, sizeof(topic));
    entity_state_topic(e, state_topic, sizeof(state_topic));
    entity_unique_id(e, uid, sizeof(uid));
    key_label(e->key, label, sizeof(label));

    cJSON *root = cJSON_CreateObject();
    switch (e->kind) {
        case ENT_ALARM:
            snprintf(name, sizeof(name), "%s %s", label, gui_i18n_get("mqtt.alarm"));
            break;
        case ENT_RSSI:
        case ENT_SNR: {
            char dn[40];
            dev_name(e->dev_addr, dn, sizeof(dn));
            snprintf(name, sizeof(name), "%s %s", dn, (e->kind == ENT_RSSI) ? "RSSI" : "SNR");
            break;
        }
        default:
            strlcpy(name, label, sizeof(name));
            break;
    }
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "unique_id", uid);
    cJSON_AddStringToObject(root, "state_topic", state_topic);
    cJSON_AddStringToObject(root, "availability_topic", s_status_topic);

    if (e->kind == ENT_ALARM) {
        cJSON_AddStringToObject(root, "device_class", "enum");
        cJSON *opts = cJSON_AddArrayToObject(root, "options");
        cJSON_AddItemToArray(opts, cJSON_CreateString("ok"));
        cJSON_AddItemToArray(opts, cJSON_CreateString("info"));
        cJSON_AddItemToArray(opts, cJSON_CreateString("warning"));
        cJSON_AddItemToArray(opts, cJSON_CreateString("danger"));
        cJSON_AddStringToObject(root, "icon", "mdi:alarm-light-outline");
    } else {
        mqtt_ha_class_t cls;
        const char *unit = (e->kind == ENT_RSSI) ? "dBm" : (e->kind == ENT_SNR) ? "dB" : e->unit;
        mqtt_ha_classify(e->key, unit, e->is_numeric, &cls);
        if (cls.device_class) cJSON_AddStringToObject(root, "device_class", cls.device_class);
        if (cls.state_class) cJSON_AddStringToObject(root, "state_class", cls.state_class);
        if (cls.unit[0]) cJSON_AddStringToObject(root, "unit_of_measurement", cls.unit);
        if (e->kind != ENT_VALUE) cJSON_AddStringToObject(root, "entity_category", "diagnostic");
        if (e->expire_after_sec > 0 && !is_local_dev(e->dev_addr)) {
            cJSON_AddNumberToObject(root, "expire_after", e->expire_after_sec);
        }
    }

    bool on_node = is_local_dev(e->dev_addr) || e->kind == ENT_RSSI || e->kind == ENT_SNR;
    cJSON_AddItemToObject(root, "device", on_node ? node_device_json() : lora_device_json(e->dev_addr));

    cJSON *origin = cJSON_AddObjectToObject(root, "origin");
    cJSON_AddStringToObject(origin, "name", "SenseCAP Cistern");
    cJSON_AddStringToObject(origin, "sw_version", esp_app_get_description()->version);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        ESP_LOGE(TAG, "No memory for discovery payload of %s", uid);
        return;
    }
    publish(topic, payload, 1, true);
    cJSON_free(payload);
    e->cfg_published = true;
    s_cnt_discovery++;
}

static void publish_state(mqtt_entity_t *e, const char *payload) {
    char topic[MQTT_TOPIC_LEN];
    entity_state_topic(e, topic, sizeof(topic));
    publish(topic, payload, 0, s_cfg.retain_states);
    e->last_pub_ms = now_ms();
    e->has_pending = false;
}

/* ── Entity table ─────────────────────────────────────────────────────── */

static mqtt_entity_t *add_entity(uint32_t dev_addr, const char *key, uint8_t kind, uint32_t expire,
                                 const mqtt_entity_t *old, size_t old_count) {
    if (s_ent_count >= MQTT_MAX_ENTITIES) {
        ESP_LOGW(TAG, "Entity table full (%d), %s not published", MQTT_MAX_ENTITIES, key);
        return NULL;
    }
    mqtt_entity_t *e = &s_ent[s_ent_count++];
    memset(e, 0, sizeof(*e));
    e->dev_addr = dev_addr;
    strlcpy(e->key, key, sizeof(e->key));
    e->kind = kind;
    e->expire_after_sec = expire;
    e->alarm_sent = -2;

    /* Keep what was learned from earlier values (type, unit) across config reloads */
    for (size_t i = 0; i < old_count; i++) {
        const mqtt_entity_t *o = &old[i];
        if (o->dev_addr == dev_addr && o->kind == kind && strcmp(o->key, key) == 0) {
            e->known = o->known;
            e->is_numeric = o->is_numeric;
            strlcpy(e->unit, o->unit, sizeof(e->unit));
            e->last_fcnt = o->last_fcnt;
            e->fcnt_valid = o->fcnt_valid;
            break;
        }
    }
    if (kind == ENT_ALARM || kind == ENT_RSSI || kind == ENT_SNR) {
        e->known = true;
        e->is_numeric = (kind != ENT_ALARM);
    }
    return e;
}

/* Rebuild the entity table from the staged selection (bridge task) */
static void rebuild_entities(void) {
    EXT_RAM_BSS_ATTR static sel_dev_t sel[MQTT_SEL_MAX_DEVICES];
    uint8_t count;
    portENTER_CRITICAL(&s_sel_mux);
    count = s_sel_count;
    memcpy(sel, s_sel, sizeof(sel_dev_t) * count);
    portEXIT_CRITICAL(&s_sel_mux);

    size_t old_count = s_ent_count;
    mqtt_entity_t *old = NULL;
    if (old_count > 0) {
        old = heap_caps_malloc(old_count * sizeof(mqtt_entity_t), MALLOC_CAP_SPIRAM);
        if (old) memcpy(old, s_ent, old_count * sizeof(mqtt_entity_t));
        else old_count = 0;
    }

    s_ent_count = 0;
    for (uint8_t d = 0; d < count; d++) {
        const sel_dev_t *sd = &sel[d];
        if (!lora_model_get_device_by_addr(sd->dev_addr)) {
            ESP_LOGW(TAG, "MQTT selection for unknown device 0x%08lX ignored", (unsigned long)sd->dev_addr);
            continue;
        }
        for (uint8_t k = 0; k < sd->count; k++) {
            const sel_key_t *sk = &sd->keys[k];
            bool radio = (strcmp(sk->key, "rssi") == 0 || strcmp(sk->key, "snr") == 0);
            if (radio) {
                if (is_local_dev(sd->dev_addr)) continue;
                add_entity(sd->dev_addr, sk->key, (sk->key[0] == 'r') ? ENT_RSSI : ENT_SNR,
                           sk->expire_after_sec, old, old_count);
                continue;
            }
            if (sk->flags & MQTT_KEY_FLAG_VALUE) {
                add_entity(sd->dev_addr, sk->key, ENT_VALUE, sk->expire_after_sec, old, old_count);
            }
            if (sk->flags & MQTT_KEY_FLAG_ALARM) {
                add_entity(sd->dev_addr, sk->key, ENT_ALARM, 0, old, old_count);
            }
        }
    }
    free(old);
    ESP_LOGI(TAG, "%u entities selected on %u devices", (unsigned)s_ent_count, (unsigned)count);
}

/* ── Discovery sync: remove configs of this node that are no longer selected ── */

static void start_sync(void) {
    if (!s_connected) return;
    for (size_t i = 0; i < s_ent_count; i++) s_ent[i].cfg_published = false;
    if (!s_cfg.discovery) {
        s_syncing = false;
        return;
    }
    s_collected_count = 0;
    s_collected_overflow = false;
    char filter[MQTT_TOPIC_LEN];
    snprintf(filter, sizeof(filter), "%s/+/%s/+/config", s_cfg.discovery_prefix, s_node_id);
    esp_mqtt_client_subscribe(s_client, filter, 0);
    s_syncing = true;
    s_sync_end_ms = now_ms() + SYNC_COLLECT_MS;
    ESP_LOGI(TAG, "Discovery sync: collecting retained configs of %s", s_node_id);
}

static void finish_sync(void) {
    char filter[MQTT_TOPIC_LEN];
    snprintf(filter, sizeof(filter), "%s/+/%s/+/config", s_cfg.discovery_prefix, s_node_id);
    esp_mqtt_client_unsubscribe(s_client, filter);
    s_syncing = false;

    /* Remove configs that no selected entity produces any more */
    size_t removed = 0;
    if (!s_collected_overflow) {
        char (*wanted_topics)[MQTT_TOPIC_LEN] = NULL;
        if (s_ent_count > 0) {
            wanted_topics = heap_caps_malloc(s_ent_count * MQTT_TOPIC_LEN, MALLOC_CAP_SPIRAM);
            if (!wanted_topics) {
                ESP_LOGE(TAG, "No memory for the discovery sync, stale entities are not removed");
                s_collected_count = 0;
            }
        }
        for (size_t i = 0; wanted_topics && i < s_ent_count; i++) {
            entity_discovery_topic(&s_ent[i], wanted_topics[i], MQTT_TOPIC_LEN);
        }
        for (size_t c = 0; c < s_collected_count; c++) {
            bool wanted = false;
            for (size_t i = 0; i < s_ent_count && !wanted; i++) {
                wanted = (strcmp(wanted_topics[i], s_collected[c]) == 0);
            }
            if (!wanted) {
                publish(s_collected[c], "", 1, true);
                removed++;
            }
        }
        free(wanted_topics);
    } else {
        ESP_LOGW(TAG, "More than %d retained configs, stale entities are not removed", MQTT_MAX_COLLECTED);
    }
    s_cnt_removed += removed;

    for (size_t i = 0; i < s_ent_count; i++) publish_discovery(&s_ent[i]);
    ESP_LOGI(TAG, "Discovery sync done: %u found, %u removed, %lu published", (unsigned)s_collected_count,
             (unsigned)removed, (unsigned long)s_cnt_discovery);
}

/* ── Configuration file ───────────────────────────────────────────────── */

extern int rp2040_read_file_from_sd(const char *path, char *out_buf, size_t max_len, uint32_t timeout_ms);

static void json_str(const cJSON *root, const char *name, char *out, size_t len, const char *def) {
    const cJSON *it = cJSON_GetObjectItem(root, name);
    strlcpy(out, (cJSON_IsString(it) && it->valuestring) ? it->valuestring : def, len);
}

static bool json_bool(const cJSON *root, const char *name, bool def) {
    const cJSON *it = cJSON_GetObjectItem(root, name);
    return cJSON_IsBool(it) ? cJSON_IsTrue(it) : def;
}

static int json_int(const cJSON *root, const char *name, int def) {
    const cJSON *it = cJSON_GetObjectItem(root, name);
    return cJSON_IsNumber(it) ? it->valueint : def;
}

/* Read /config/mqtt.json; *out.enabled = false if it is missing or invalid */
static void load_config(mqtt_bridge_cfg_t *out) {
    memset(out, 0, sizeof(*out));
    char *buf = heap_caps_malloc(MQTT_CFG_FILE_MAX, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    int len = rp2040_read_file_from_sd(MQTT_BRIDGE_CONFIG_PATH, buf, MQTT_CFG_FILE_MAX, 1500);
    if (len <= 0) {
        free(buf);
        ESP_LOGI(TAG, "%s not found: MQTT off", MQTT_BRIDGE_CONFIG_PATH);
        return;
    }
    cJSON *root = cJSON_ParseWithLength(buf, (size_t)len);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "%s: invalid JSON, MQTT off", MQTT_BRIDGE_CONFIG_PATH);
        return;
    }

    out->enabled = json_bool(root, "enabled", true);
    json_str(root, "broker_uri", out->broker_uri, sizeof(out->broker_uri), "");
    json_str(root, "username", out->username, sizeof(out->username), "");
    json_str(root, "password", out->password, sizeof(out->password), "");
    json_str(root, "client_id", out->client_id, sizeof(out->client_id), "");
    json_str(root, "ca_file", out->ca_file, sizeof(out->ca_file), "");
    json_str(root, "base_topic", out->base_topic, sizeof(out->base_topic), "sensecap");
    json_str(root, "discovery_prefix", out->discovery_prefix, sizeof(out->discovery_prefix), "homeassistant");
    json_str(root, "node_name", out->node_name, sizeof(out->node_name), "");
    out->keepalive_sec = (uint16_t)json_int(root, "keepalive_sec", 60);
    out->discovery = json_bool(root, "discovery", true);
    out->retain_states = json_bool(root, "retain_states", true);
    int li = json_int(root, "local_interval_sec", 60);
    out->local_interval_sec = (uint16_t)((li < 5) ? 5 : (li > 3600) ? 3600 : li);
    cJSON_Delete(root);

    if (out->enabled && strncmp(out->broker_uri, "mqtt://", 7) != 0 && strncmp(out->broker_uri, "mqtts://", 8) != 0) {
        ESP_LOGE(TAG, "%s: \"broker_uri\" must start with mqtt:// or mqtts:// (got \"%s\"), MQTT off",
                 MQTT_BRIDGE_CONFIG_PATH, out->broker_uri);
        out->enabled = false;
    }
    /* Topic levels must not contain wildcards or empty levels */
    if (strpbrk(out->base_topic, "+#") || out->base_topic[0] == '\0') strlcpy(out->base_topic, "sensecap", sizeof(out->base_topic));
    if (strpbrk(out->discovery_prefix, "+#") || out->discovery_prefix[0] == '\0') {
        strlcpy(out->discovery_prefix, "homeassistant", sizeof(out->discovery_prefix));
    }
}

/* true if a change of these settings needs a new client */
static bool connection_changed(const mqtt_bridge_cfg_t *a, const mqtt_bridge_cfg_t *b) {
    return a->enabled != b->enabled || strcmp(a->broker_uri, b->broker_uri) != 0 ||
           strcmp(a->username, b->username) != 0 || strcmp(a->password, b->password) != 0 ||
           strcmp(a->client_id, b->client_id) != 0 || strcmp(a->ca_file, b->ca_file) != 0 ||
           strcmp(a->base_topic, b->base_topic) != 0 || strcmp(a->discovery_prefix, b->discovery_prefix) != 0 ||
           a->keepalive_sec != b->keepalive_sec;
}

/* ── MQTT client ──────────────────────────────────────────────────────── */

static void queue_post(const q_item_t *item) {
    if (!s_queue || xQueueSend(s_queue, item, 0) != pdTRUE) s_cnt_dropped++;
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
    static q_item_t item; /* esp-mqtt task only */
    memset(&item, 0, sizeof(item));

    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            item.type = Q_CONNECTED;
            queue_post(&item);
            break;
        case MQTT_EVENT_DISCONNECTED:
            item.type = Q_DISCONNECTED;
            queue_post(&item);
            break;
        case MQTT_EVENT_DATA:
            /* Only the first fragment carries the topic */
            if (ev->current_data_offset != 0 || ev->topic_len <= 0) break;
            item.type = Q_MESSAGE;
            item.msg.topic_truncated = (ev->topic_len >= MQTT_TOPIC_LEN);
            memcpy(item.msg.topic, ev->topic, item.msg.topic_truncated ? MQTT_TOPIC_LEN - 1 : (size_t)ev->topic_len);
            item.msg.data_len = (uint8_t)((ev->data_len < (int)sizeof(item.msg.data) - 1) ? ev->data_len
                                                                                          : (int)sizeof(item.msg.data) - 1);
            memcpy(item.msg.data, ev->data, item.msg.data_len);
            queue_post(&item);
            break;
        case MQTT_EVENT_ERROR:
            if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
                ESP_LOGW(TAG, "Broker not reachable (esp-tls 0x%x, sock errno %d): check broker_uri / port, retry in 10 s",
                         ev->error_handle->esp_tls_last_esp_err, ev->error_handle->esp_transport_sock_errno);
            } else if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                ESP_LOGE(TAG, "Connection refused by broker (code %d): check username / password / client_id",
                         ev->error_handle->connect_return_code);
            }
            break;
        default:
            break;
    }
}

static void client_stop(void) {
    if (!s_client) return;
    if (s_connected) {
        /* A clean disconnect does not trigger the last will: report offline ourselves */
        esp_mqtt_client_publish(s_client, s_status_topic, "offline", 0, 1, 1);
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
    s_syncing = false;
    free(s_ca_pem);
    s_ca_pem = NULL;
}

static void client_start(void) {
    if (s_client || !s_cfg.enabled || !s_network_up) return;

    snprintf(s_status_topic, sizeof(s_status_topic), "%s/%s/status", s_cfg.base_topic, s_node_id);

    esp_mqtt_client_config_t mc = {0};
    mc.broker.address.uri = s_cfg.broker_uri;
    mc.credentials.client_id = s_cfg.client_id[0] ? s_cfg.client_id : s_node_id;
    if (s_cfg.username[0]) mc.credentials.username = s_cfg.username;
    if (s_cfg.password[0]) mc.credentials.authentication.password = s_cfg.password;
    mc.session.keepalive = s_cfg.keepalive_sec;
    mc.session.last_will.topic = s_status_topic;
    mc.session.last_will.msg = "offline";
    mc.session.last_will.qos = 1;
    mc.session.last_will.retain = 1;
    mc.network.reconnect_timeout_ms = 10000;
    mc.buffer.size = 1024;
    mc.buffer.out_size = 2048;
    mc.outbox.limit = 32 * 1024;   /* bounded memory while the broker is slow */
    mc.task.priority = 4;

    if (strncmp(s_cfg.broker_uri, "mqtts://", 8) == 0) {
        if (s_cfg.ca_file[0]) {
            s_ca_pem = heap_caps_calloc(1, MQTT_CA_FILE_MAX, MALLOC_CAP_SPIRAM);
            int len = s_ca_pem ? rp2040_read_file_from_sd(s_cfg.ca_file, s_ca_pem, MQTT_CA_FILE_MAX - 1, 1500) : -1;
            if (len <= 0) {
                ESP_LOGE(TAG, "CA file %s not readable, no connection", s_cfg.ca_file);
                free(s_ca_pem);
                s_ca_pem = NULL;
                return;
            }
            s_ca_pem[len] = '\0';
            mc.broker.verification.certificate = s_ca_pem;
        } else {
            mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    if (esp_mqtt_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return;
    }
    ESP_LOGI(TAG, "Connecting to %s as %s (node %s)", s_cfg.broker_uri, mc.credentials.client_id, s_node_id);
}

/* ── Bridge task handlers ─────────────────────────────────────────────── */

/* Station has an IP address (covers a VIEW_EVENT_WIFI_ST posted before the bridge registered) */
static bool network_is_up(void) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    return netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

static void apply_config(void) {
    mqtt_bridge_cfg_t cfg;
    load_config(&cfg);
    bool reconnect = !s_cfg_loaded || connection_changed(&s_cfg, &cfg);
    if (reconnect) client_stop();
    s_cfg = cfg;
    s_cfg_loaded = true;
    if (!s_network_up) s_network_up = network_is_up();
    rebuild_entities();

    if (!s_cfg.enabled) {
        ESP_LOGI(TAG, "MQTT off (no %s, \"enabled\": false or invalid settings)", MQTT_BRIDGE_CONFIG_PATH);
    } else if (reconnect) {
        ESP_LOGI(TAG, "MQTT on: %s, user %s, %u entities%s", s_cfg.broker_uri,
                 s_cfg.username[0] ? s_cfg.username : "(none)", (unsigned)s_ent_count,
                 s_network_up ? "" : ", waiting for the network");
        client_start();
    } else if (s_connected) {
        start_sync();
    }
}

static void on_connected(void) {
    s_connected = true;
    ESP_LOGI(TAG, "Connected");
    publish(s_status_topic, "online", 1, true);
    char topic[MQTT_TOPIC_LEN];
    snprintf(topic, sizeof(topic), "%s/status", s_cfg.discovery_prefix);
    esp_mqtt_client_subscribe(s_client, topic, 1);
    for (size_t i = 0; i < s_ent_count; i++) s_ent[i].alarm_sent = -2;
    start_sync();
}

static void on_message(const q_item_t *it) {
    char ha_status[MQTT_TOPIC_LEN];
    snprintf(ha_status, sizeof(ha_status), "%s/status", s_cfg.discovery_prefix);
    if (strcmp(it->msg.topic, ha_status) == 0) {
        if (strncmp(it->msg.data, "online", 6) == 0 && !s_syncing) {
            ESP_LOGI(TAG, "Home Assistant online: republishing discovery");
            for (size_t i = 0; i < s_ent_count; i++) publish_discovery(&s_ent[i]);
        }
        return;
    }
    if (!s_syncing || it->msg.topic_truncated || it->msg.data_len == 0) return;
    if (s_collected_count >= MQTT_MAX_COLLECTED) {
        s_collected_overflow = true;
        return;
    }
    strlcpy(s_collected[s_collected_count++], it->msg.topic, MQTT_TOPIC_LEN);
}

static void publish_radio(uint32_t dev_addr, uint32_t fcnt) {
    const lora_device_status_t *st = NULL;
    size_t n = lora_model_get_device_status_count();
    for (size_t i = 0; i < n; i++) {
        const lora_device_status_t *s = lora_model_get_device_status(i);
        if (s && s->dev_addr == dev_addr) {
            st = s;
            break;
        }
    }
    if (!st) return;
    for (uint8_t kind = ENT_RSSI; kind <= ENT_SNR; kind++) {
        mqtt_entity_t *e = find_entity(dev_addr, (kind == ENT_RSSI) ? "rssi" : "snr", kind);
        if (!e || (e->fcnt_valid && e->last_fcnt == fcnt)) continue;
        e->last_fcnt = fcnt;
        e->fcnt_valid = true;
        if (!e->cfg_published && !s_syncing) publish_discovery(e);
        char val[16];
        snprintf(val, sizeof(val), "%d", (kind == ENT_RSSI) ? (int)st->last_rssi : (int)st->last_snr);
        publish_state(e, val);
    }
}

static void on_telemetry(const q_item_t *it) {
    if (it->telem.context == 2) return; /* history values are not live states */

    if (!is_local_dev(it->telem.dev_addr)) publish_radio(it->telem.dev_addr, it->telem.fcnt);

    mqtt_entity_t *e = find_entity(it->telem.dev_addr, it->telem.key, ENT_VALUE);
    if (!e) return;

    char val[40];
    bool numeric = true;
    switch (it->telem.val_type) {
        case LORA_VAL_TYPE_FLOAT:  format_float(it->telem.v.f, val, sizeof(val)); break;
        case LORA_VAL_TYPE_INT:    snprintf(val, sizeof(val), "%ld", (long)it->telem.v.i); break;
        case LORA_VAL_TYPE_BOOL:   snprintf(val, sizeof(val), "%d", it->telem.v.b ? 1 : 0); break;
        default:
            strlcpy(val, it->telem.v.s, sizeof(val));
            numeric = false;
            break;
    }

    /* First value (or a changed unit / type) completes the entity: (re)publish its discovery */
    if (!e->known || e->is_numeric != numeric || strcmp(e->unit, it->telem.unit) != 0) {
        e->known = true;
        e->is_numeric = numeric;
        strlcpy(e->unit, it->telem.unit, sizeof(e->unit));
        e->cfg_published = false;
    }
    if (!e->cfg_published && !s_syncing) publish_discovery(e);

    /* Onboard sensors deliver every few seconds: at most one value per local_interval_sec */
    if (is_local_dev(e->dev_addr) && e->last_pub_ms != 0 &&
        now_ms() - e->last_pub_ms < (uint32_t)s_cfg.local_interval_sec * 1000U) {
        strlcpy(e->pending, val, sizeof(e->pending));
        e->has_pending = true;
        return;
    }
    publish_state(e, val);
}

static void periodic(void) {
    uint32_t now = now_ms();

    if (s_cfg_dirty && now - s_cfg_dirty_ms >= CFG_SETTLE_MS) {
        s_cfg_dirty = false;
        apply_config();
    }
    if (!s_connected) return;

    if (s_syncing && (int32_t)(now - s_sync_end_ms) >= 0) finish_sync();

    if (now - s_last_alarm_poll_ms >= ALARM_POLL_MS) {
        s_last_alarm_poll_ms = now;
        for (size_t i = 0; i < s_ent_count; i++) {
            mqtt_entity_t *e = &s_ent[i];
            if (e->kind == ENT_ALARM) {
                int sev = alarm_evaluator_get_key_severity(e->dev_addr, e->key, NULL);
                if (sev != e->alarm_sent) {
                    if (!e->cfg_published && !s_syncing) publish_discovery(e);
                    char topic[MQTT_TOPIC_LEN];
                    entity_state_topic(e, topic, sizeof(topic));
                    publish(topic, severity_state(sev), 1, true);
                    e->alarm_sent = (int8_t)sev;
                }
            } else if (e->has_pending &&
                       now - e->last_pub_ms >= (uint32_t)s_cfg.local_interval_sec * 1000U) {
                char val[40];
                strlcpy(val, e->pending, sizeof(val));
                publish_state(e, val);
            }
        }
    }
}

static void bridge_task(void *arg) {
    static q_item_t item; /* bridge task only */
    for (;;) {
        if (xQueueReceive(s_queue, &item, pdMS_TO_TICKS(250)) == pdTRUE) {
            switch (item.type) {
                case Q_TELEMETRY:
                    if (s_connected) on_telemetry(&item);
                    break;
                case Q_CONNECTED:
                    if (s_client) on_connected();
                    break;
                case Q_DISCONNECTED:
                    if (s_connected) ESP_LOGW(TAG, "Disconnected (esp-mqtt reconnects)");
                    s_connected = false;
                    s_syncing = false;
                    break;
                case Q_MESSAGE:
                    if (s_connected) on_message(&item);
                    break;
                case Q_NETWORK:
                    if (item.network_up != s_network_up) {
                        ESP_LOGI(TAG, "Network %s", item.network_up ? "up" : "down");
                    }
                    s_network_up = item.network_up;
                    if (s_network_up && s_cfg_loaded) client_start();
                    break;
                default:
                    break;
            }
        }
        periodic();
    }
}

/* ── Event sources ────────────────────────────────────────────────────── */

static void view_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (!data) return;
    q_item_t item;
    if (id == VIEW_EVENT_LORA_TELEMETRY) {
        if (!s_client) return; /* no broker: nothing to do */
        const struct view_data_lora_telemetry *t = (const struct view_data_lora_telemetry *)data;
        memset(&item, 0, sizeof(item));
        item.type = Q_TELEMETRY;
        item.telem.dev_addr = t->dev_addr;
        item.telem.fcnt = t->fcnt;
        item.telem.val_type = (uint8_t)t->val_type;
        item.telem.context = t->context;
        strlcpy(item.telem.key, t->key, sizeof(item.telem.key));
        strlcpy(item.telem.unit, t->unit, sizeof(item.telem.unit));
        if (t->val_type == LORA_VAL_TYPE_STRING) {
            strlcpy(item.telem.v.s, t->val.str_val, sizeof(item.telem.v.s));
        } else {
            memcpy(&item.telem.v, &t->val, sizeof(int32_t));
        }
        queue_post(&item);
    } else if (id == VIEW_EVENT_WIFI_ST) {
        const struct view_data_wifi_st *st = (const struct view_data_wifi_st *)data;
        memset(&item, 0, sizeof(item));
        item.type = Q_NETWORK;
        item.network_up = st->is_network;
        queue_post(&item);
    }
}

void mqtt_bridge_on_rp2040_cfg_reload(void) {
    portENTER_CRITICAL(&s_sel_mux);
    s_sel_count = 0;
    portEXIT_CRITICAL(&s_sel_mux);
    s_cfg_dirty_ms = now_ms();
    s_cfg_dirty = true;
}

void mqtt_bridge_on_rp2040_mqtt_cfg(const uint8_t *buf, size_t len) {
    if (len < 6) return;
    uint32_t dev_addr;
    memcpy(&dev_addr, &buf[1], 4);
    uint8_t count = buf[5];
    if (count > MQTT_SEL_MAX_KEYS) count = MQTT_SEL_MAX_KEYS;
    if (len < 6 + (size_t)count * MQTT_PKT_ENTRY_SIZE) {
        ESP_LOGE(TAG, "0x46 truncated (%u bytes for %u keys)", (unsigned)len, count);
        return;
    }

    sel_dev_t d = {0};
    d.dev_addr = dev_addr;
    const uint8_t *p = &buf[6];
    for (uint8_t i = 0; i < count; i++, p += MQTT_PKT_ENTRY_SIZE) {
        memcpy(d.keys[d.count].key, p, 15);
        d.keys[d.count].key[15] = '\0';
        d.keys[d.count].flags = p[16];
        memcpy(&d.keys[d.count].expire_after_sec, &p[17], 4);
        if (d.keys[d.count].key[0]) d.count++;
    }

    portENTER_CRITICAL(&s_sel_mux);
    int slot = -1;
    for (uint8_t i = 0; i < s_sel_count; i++) {
        if (s_sel[i].dev_addr == dev_addr) {
            slot = i;
            break;
        }
    }
    if (slot < 0 && d.count > 0 && s_sel_count < MQTT_SEL_MAX_DEVICES) slot = s_sel_count++;
    if (slot >= 0) s_sel[slot] = d;
    portEXIT_CRITICAL(&s_sel_mux);

    s_cfg_dirty_ms = now_ms();
    s_cfg_dirty = true;
}

void mqtt_bridge_print_status(void) {
    printf("\nMQTT bridge (%s)\n", MQTT_BRIDGE_CONFIG_PATH);
    if (!s_cfg_loaded) {
        printf("  configuration not loaded yet\n");
        return;
    }
    printf("  enabled          : %s\n", s_cfg.enabled ? "yes" : "no");
    printf("  broker           : %s\n", s_cfg.broker_uri);
    printf("  client id / user : %s / %s (password %s)\n", s_cfg.client_id[0] ? s_cfg.client_id : s_node_id,
           s_cfg.username[0] ? s_cfg.username : "-", s_cfg.password[0] ? "set" : "not set");
    printf("  node id          : %s\n", s_node_id);
    printf("  topics           : %s/%s/... , discovery %s (%s)\n", s_cfg.base_topic, s_node_id,
           s_cfg.discovery_prefix, s_cfg.discovery ? "on" : "off");
    printf("  network / broker : %s / %s%s\n", s_network_up ? "up" : "down",
           s_connected ? "connected" : "not connected", s_syncing ? " (discovery sync)" : "");
    printf("  published / dropped / discovery / removed : %lu / %lu / %lu / %lu\n",
           (unsigned long)s_cnt_published, (unsigned long)s_cnt_dropped, (unsigned long)s_cnt_discovery,
           (unsigned long)s_cnt_removed);
    static const char *const kinds[] = {"value", "alarm", "rssi", "snr"};
    printf("  entities (%u):\n", (unsigned)s_ent_count);
    for (size_t i = 0; i < s_ent_count; i++) {
        const mqtt_entity_t *e = &s_ent[i];
        char topic[MQTT_TOPIC_LEN];
        entity_state_topic(e, topic, sizeof(topic));
        printf("    0x%08lX %-15s %-5s %-8s %s\n", (unsigned long)e->dev_addr, e->key, kinds[e->kind],
               e->known ? (e->cfg_published ? "online" : "known") : "waiting", topic);
    }
}

int mqtt_bridge_init(void) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_node_id, sizeof(s_node_id), "sensecap_%02x%02x%02x", mac[3], mac[4], mac[5]);

    s_ent = heap_caps_calloc(MQTT_MAX_ENTITIES, sizeof(mqtt_entity_t), MALLOC_CAP_SPIRAM);
    s_collected = heap_caps_calloc(MQTT_MAX_COLLECTED, MQTT_TOPIC_LEN, MALLOC_CAP_SPIRAM);
    s_queue = xQueueCreateWithCaps(QUEUE_LEN, sizeof(q_item_t), MALLOC_CAP_SPIRAM);
    if (!s_ent || !s_collected || !s_queue) {
        ESP_LOGE(TAG, "No memory for the MQTT bridge");
        return -1;
    }
    if (xTaskCreatePinnedToCore(bridge_task, "mqtt_bridge", TASK_STACK, NULL, TASK_PRIO, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Bridge task not created");
        return -1;
    }
    esp_event_handler_instance_register_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_TELEMETRY,
                                             view_event_handler, NULL, NULL);
    esp_event_handler_instance_register_with(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_WIFI_ST,
                                             view_event_handler, NULL, NULL);
    ESP_LOGI(TAG, "MQTT bridge ready (node %s), waiting for %s", s_node_id, MQTT_BRIDGE_CONFIG_PATH);
    return 0;
}
