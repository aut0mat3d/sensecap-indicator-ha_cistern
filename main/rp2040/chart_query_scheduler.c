/**
 * @file chart_query_scheduler.c
 * @brief Implementation of the serialized chart query FIFO queue and subscription manager.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "chart_query_scheduler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "rp2040.h"
#include <string.h>
#include <time.h>

static const char *TAG = "chart_scheduler";
static bool s_initialized = false;

typedef enum {
    SCHED_STATE_IDLE = 0,
    SCHED_STATE_AWAIT_RESPONSE
} sched_state_t;

typedef struct {
    uint8_t  req_id;
    int8_t   sub_id;
    uint32_t dev_addr;
    char     key[32];
    uint32_t start_ts;
    uint32_t end_ts;
    uint32_t step_sec;
    uint16_t point_count;
    float    val_min;
    float    val_max;
    bool     valid;
} query_queue_item_t;

/* Subscription database (allocated once in BSS) */
static chart_subscription_t s_subs[CHART_SCHEDULER_MAX_SUBSCRIPTIONS];

/* FIFO Request Queue */
static query_queue_item_t   s_queue[CHART_SCHEDULER_QUEUE_SIZE];
static uint8_t              s_q_head = 0;
static uint8_t              s_q_tail = 0;
static uint8_t              s_q_count = 0;

static sched_state_t        s_state = SCHED_STATE_IDLE;
static uint32_t             s_request_sent_ms = 0;
static uint8_t              s_next_req_id = 1;

static SemaphoreHandle_t    s_sched_mutex = NULL;

/* ── Static Internal Helpers ────────────────────────────────────────────── */

static bool _enqueue_query_internal(int8_t sub_id, uint32_t start_ts, uint32_t end_ts) {
    if (sub_id < 0 || sub_id >= CHART_SCHEDULER_MAX_SUBSCRIPTIONS) return false;
    chart_subscription_t *sub = &s_subs[sub_id];
    if (sub->sub_id < 0) return false;

    if (s_q_count >= CHART_SCHEDULER_QUEUE_SIZE) {
        ESP_LOGW(TAG, "FIFO Queue full! Dropping request for key='%s' (sub_id=%d)", sub->key, sub_id);
        return false;
    }

    /* Assign rotating 8-bit request transaction ID */
    uint8_t req_id = s_next_req_id++;
    if (s_next_req_id == 0) s_next_req_id = 1;

    query_queue_item_t *item = &s_queue[s_q_tail];
    item->req_id = req_id;
    item->sub_id = sub_id;
    item->dev_addr = sub->dev_addr;
    strncpy(item->key, sub->key, sizeof(item->key) - 1);
    item->key[sizeof(item->key) - 1] = '\0';
    item->start_ts = start_ts;
    item->end_ts = end_ts;
    item->step_sec = sub->interval_sec;
    item->point_count = sub->point_count;
    item->val_min = sub->val_min;
    item->val_max = sub->val_max;
    item->valid = true;

    s_q_tail = (s_q_tail + 1) % CHART_SCHEDULER_QUEUE_SIZE;
    s_q_count++;

    return true;
}

static void _trigger_immediate_subscription_query(int8_t sub_id) {
    if (sub_id < 0 || sub_id >= CHART_SCHEDULER_MAX_SUBSCRIPTIONS) return;
    chart_subscription_t *sub = &s_subs[sub_id];
    if (sub->sub_id < 0 || sub->interval_sec == 0) return;

    time_t now = time(NULL);
    if (now < 1700000000UL) return;

    uint32_t end_ts = (uint32_t)now;
    if (sub->interval_sec < 3600) {
        /* Align sub-hourly buckets to completed interval */
        end_ts = (uint32_t)(now - (now % sub->interval_sec));
    }
    uint32_t start_ts = end_ts - (sub->point_count * sub->interval_sec);

    if (_enqueue_query_internal(sub_id, start_ts, end_ts)) {
        sub->last_queried_bucket = end_ts;
    }
}

/* ── Public API Implementation ─────────────────────────────────────────── */

void chart_query_scheduler_init(void) {
    if (s_initialized) {
        return; /* Prevent wiping subscriptions on second call from model_init */
    }

    if (!s_sched_mutex) {
        s_sched_mutex = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);

    for (size_t i = 0; i < CHART_SCHEDULER_MAX_SUBSCRIPTIONS; i++) {
        s_subs[i].sub_id = -1;
        s_subs[i].is_active = false;
        s_subs[i].new_data_ready = false;
        s_subs[i].data_count = 0;
        s_subs[i].last_queried_bucket = 0;
    }

    memset(s_queue, 0, sizeof(s_queue));
    s_q_head = 0;
    s_q_tail = 0;
    s_q_count = 0;
    s_state = SCHED_STATE_IDLE;
    s_next_req_id = 1;
    s_initialized = true;

    xSemaphoreGive(s_sched_mutex);

    ESP_LOGI(TAG, "Chart query FIFO scheduler initialized (capacity: %d subscriptions, %d queue slots).",
             CHART_SCHEDULER_MAX_SUBSCRIPTIONS, CHART_SCHEDULER_QUEUE_SIZE);
}

int8_t chart_query_scheduler_subscribe(uint32_t dev_addr, const char *key, uint8_t zone_idx,
                                      uint32_t interval_sec, uint16_t point_count,
                                      float val_min, float val_max) {
    if (!key) return -1;

    /* Defensive auto-initialization if called before model init */
    if (!s_sched_mutex) {
        chart_query_scheduler_init();
    }

    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);

    /* Find existing matching subscription to avoid duplicates */
    for (size_t i = 0; i < CHART_SCHEDULER_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].sub_id >= 0 &&
            s_subs[i].dev_addr == dev_addr &&
            s_subs[i].zone_idx == zone_idx &&
            strcmp(s_subs[i].key, key) == 0) {
            
            /* Update dynamic parameters */
            s_subs[i].interval_sec = interval_sec;
            s_subs[i].point_count = (point_count <= CHART_SUB_MAX_POINTS) ? point_count : CHART_SUB_MAX_POINTS;
            s_subs[i].val_min = val_min;
            s_subs[i].val_max = val_max;

            xSemaphoreGive(s_sched_mutex);
            return (int8_t)i;
        }
    }

    /* Find empty slot */
    int8_t allocated_id = -1;
    for (size_t i = 0; i < CHART_SCHEDULER_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].sub_id < 0) {
            allocated_id = (int8_t)i;
            break;
        }
    }

    if (allocated_id < 0) {
        xSemaphoreGive(s_sched_mutex);
        ESP_LOGE(TAG, "No free subscription slot available! (Max: %d)", CHART_SCHEDULER_MAX_SUBSCRIPTIONS);
        return -1;
    }

    chart_subscription_t *sub = &s_subs[allocated_id];
    sub->sub_id = allocated_id;
    sub->dev_addr = dev_addr;
    strncpy(sub->key, key, sizeof(sub->key) - 1);
    sub->key[sizeof(sub->key) - 1] = '\0';
    sub->zone_idx = zone_idx;
    sub->interval_sec = interval_sec;
    sub->point_count = (point_count <= CHART_SUB_MAX_POINTS) ? point_count : CHART_SUB_MAX_POINTS;
    sub->val_min = val_min;
    sub->val_max = val_max;
    sub->is_active = false;
    sub->new_data_ready = false;
    sub->data_count = 0;
    sub->last_queried_bucket = 0;

    xSemaphoreGive(s_sched_mutex);

    ESP_LOGI(TAG, "Subscribed [%d]: DevAddr=0x%08lX, Key='%s', Zone=%u, Step=%lu s, Points=%u",
             allocated_id, (unsigned long)dev_addr, key, zone_idx, (unsigned long)interval_sec, sub->point_count);

    return allocated_id;
}

bool chart_query_scheduler_set_active(int8_t sub_id, bool active) {
    if (sub_id < 0 || sub_id >= CHART_SCHEDULER_MAX_SUBSCRIPTIONS || !s_sched_mutex) return false;

    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);

    if (s_subs[sub_id].sub_id < 0) {
        xSemaphoreGive(s_sched_mutex);
        return false;
    }

    bool was_active = s_subs[sub_id].is_active;
    s_subs[sub_id].is_active = active;

    /* Enqueue immediate initial load on transition from inactive to active */
    if (!was_active && active) {
        _trigger_immediate_subscription_query(sub_id);
    }

    xSemaphoreGive(s_sched_mutex);
    return true;
}

void chart_query_scheduler_set_binding_active(uint32_t dev_addr, const char *key, bool active) {
    if (!key || !s_sched_mutex) return;

    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);

    for (size_t i = 0; i < CHART_SCHEDULER_MAX_SUBSCRIPTIONS; i++) {
        if (s_subs[i].sub_id >= 0 &&
            s_subs[i].dev_addr == dev_addr &&
            strcmp(s_subs[i].key, key) == 0) {
            
            bool was_active = s_subs[i].is_active;
            s_subs[i].is_active = active;

            if (!was_active && active) {
                _trigger_immediate_subscription_query((int8_t)i);
            }
        }
    }

    xSemaphoreGive(s_sched_mutex);
}

bool chart_query_scheduler_is_data_ready(int8_t sub_id) {
    if (sub_id < 0 || sub_id >= CHART_SCHEDULER_MAX_SUBSCRIPTIONS) return false;
    return s_subs[sub_id].new_data_ready;
}

bool chart_query_scheduler_consume_data(int8_t sub_id, chart_data_point_t *out_pts,
                                        uint16_t max_pts, uint16_t *out_count) {
    if (sub_id < 0 || sub_id >= CHART_SCHEDULER_MAX_SUBSCRIPTIONS || !out_pts || !s_sched_mutex) return false;

    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);

    chart_subscription_t *sub = &s_subs[sub_id];
    if (sub->sub_id < 0 || !sub->new_data_ready) {
        xSemaphoreGive(s_sched_mutex);
        return false;
    }

    uint16_t copy_count = (sub->data_count <= max_pts) ? sub->data_count : max_pts;
    memcpy(out_pts, sub->data_buffer, copy_count * sizeof(chart_data_point_t));
    if (out_count) *out_count = copy_count;

    /* Handshake completed: clear flag */
    sub->new_data_ready = false;

    xSemaphoreGive(s_sched_mutex);
    return true;
}

void chart_query_scheduler_on_response(uint8_t req_id, uint8_t status, uint32_t step,
                                       uint16_t count, const chart_data_point_t *pts) {
    if (!s_sched_mutex) return;

    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);

    if (s_state == SCHED_STATE_AWAIT_RESPONSE) {
        query_queue_item_t *active = &s_queue[s_q_head];

        if (active->valid && active->req_id == req_id) {
            int8_t sub_id = active->sub_id;

            if (sub_id >= 0 && sub_id < CHART_SCHEDULER_MAX_SUBSCRIPTIONS && s_subs[sub_id].sub_id >= 0) {
                chart_subscription_t *sub = &s_subs[sub_id];

                uint16_t copy_pts = (count <= CHART_SUB_MAX_POINTS) ? count : CHART_SUB_MAX_POINTS;
                if (pts && copy_pts > 0) {
                    memcpy(sub->data_buffer, pts, copy_pts * sizeof(chart_data_point_t));
                    sub->data_count = copy_pts;
                } else {
                    sub->data_count = 0;
                }

                /* Signal data availability to widget layer */
                sub->new_data_ready = true;
            }

            /* Release active FIFO slot */
            active->valid = false;
            s_q_head = (s_q_head + 1) % CHART_SCHEDULER_QUEUE_SIZE;
            s_q_count--;
            s_state = SCHED_STATE_IDLE;

            xSemaphoreGive(s_sched_mutex);
            return;
        } else {
            ESP_LOGW(TAG, "Unmatched UART response: got req_id=%u, expected req_id=%u", req_id, active->req_id);
        }
    }

    xSemaphoreGive(s_sched_mutex);
}

void chart_query_scheduler_update(void) {
    if (!s_sched_mutex) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    time_t now_sec = time(NULL);

    xSemaphoreTake(s_sched_mutex, portMAX_DELAY);

    /* 1. Evaluate Periodic Grid Trigger for active subscriptions (only if clock is valid) */
    if (now_sec >= 1700000000UL) {
        for (size_t i = 0; i < CHART_SCHEDULER_MAX_SUBSCRIPTIONS; i++) {
            chart_subscription_t *sub = &s_subs[i];
            if (sub->sub_id < 0 || !sub->is_active || sub->interval_sec == 0) continue;

            /* Initial query kickoff as soon as system time became valid */
            if (sub->last_queried_bucket == 0) {
                ESP_LOGI(TAG, "Initial kickoff for sub_id=%d (key='%s', zone=%u)", 
                         sub->sub_id, sub->key, sub->zone_idx);
                _trigger_immediate_subscription_query((int8_t)i);
                continue;
            }

            uint32_t interval = sub->interval_sec;
            
            /* Jitter-safe trigger: >= 18s into interval (15s RP2040 tolerance + 3s buffer) */
            if ((now_sec % interval) >= 18) {
                uint32_t target_bucket = (uint32_t)(now_sec - (now_sec % interval));

                /* Avoid duplicate enqueues if already requested for this bucket */
                if (target_bucket > sub->last_queried_bucket) {
                    uint32_t end_ts = target_bucket;
                    uint32_t start_ts = end_ts - (sub->point_count * interval);

                    ESP_LOGI(TAG, "Grid trigger sub_id=%d (key='%s', zone=%u): bucket=%lu",
                             sub->sub_id, sub->key, sub->zone_idx, (unsigned long)target_bucket);

                    if (_enqueue_query_internal((int8_t)i, start_ts, end_ts)) {
                        sub->last_queried_bucket = target_bucket;
                    }
                }
            }
        }
    }

    /* 2. Handle timeout for in-flight UART transaction */
    if (s_state == SCHED_STATE_AWAIT_RESPONSE) {
        if (now_ms - s_request_sent_ms >= CHART_QUERY_TIMEOUT_MS) {
            query_queue_item_t *active = &s_queue[s_q_head];
            ESP_LOGW(TAG, "Transaction timeout: req_id=%u key='%s' (Zone step=%lu s)",
                     active->req_id, active->key, (unsigned long)active->step_sec);

            active->valid = false;
            s_q_head = (s_q_head + 1) % CHART_SCHEDULER_QUEUE_SIZE;
            s_q_count--;
            s_state = SCHED_STATE_IDLE;
        }
    }

    /* 3. Dispatch next pending request from FIFO */
    if (s_state == SCHED_STATE_IDLE && s_q_count > 0) {
        query_queue_item_t *next = &s_queue[s_q_head];

        ESP_LOGI(TAG, "Dispatching query: req_id=%u, sub_id=%d, key='%s', step=%lu s",
                 next->req_id, next->sub_id, next->key, (unsigned long)next->step_sec);

        rp2040_request_chart_data(next->req_id, next->dev_addr, next->key,
                                  next->start_ts, next->end_ts, next->step_sec,
                                  next->point_count, next->val_min, next->val_max);

        s_request_sent_ms = now_ms;
        s_state = SCHED_STATE_AWAIT_RESPONSE;
    }

    xSemaphoreGive(s_sched_mutex);
}