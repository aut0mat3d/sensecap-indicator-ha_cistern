/**
 * @file chart_query_scheduler.h
 * @brief Serialized FIFO query scheduler and subscription manager for historical chart data.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef CHART_QUERY_SCHEDULER_H
#define CHART_QUERY_SCHEDULER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "rp2040.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHART_SCHEDULER_MAX_SUBSCRIPTIONS  64
#define CHART_SCHEDULER_QUEUE_SIZE        16
#define CHART_QUERY_TIMEOUT_MS            600
#define CHART_SUB_MAX_POINTS              32

/**
 * @brief Structure representing a historical telemetry subscription for a widget.
 */
typedef struct {
    int8_t             sub_id;          /* Subscription ID (0..63, or -1 if unused) */
    uint32_t           dev_addr;        /* Device address */
    char               key[32];         /* Telemetry key (e.g. "depth") */
    uint8_t            zone_idx;        /* Zone index (0=daily, 1=hourly, 2=sub-hourly) */
    uint32_t           interval_sec;    /* Bucket step size in seconds (e.g. 300, 7200, 86400) */
    uint16_t           point_count;     /* Number of points requested (e.g. 11, 12, 7) */
    float              val_min;         /* Scale minimum value */
    float              val_max;         /* Scale maximum value */
    bool               is_active;       /* Active state (true = page currently visible) */

    uint32_t           last_queried_bucket; /* Timestamp of the last queried bucket to prevent duplicate requests */
    
    /* Handshake channel for LVGL rendering */
    bool               new_data_ready;  /* True when new archive data was received from RP2040 */
    uint16_t           data_count;      /* Number of valid points in data_buffer */
    chart_data_point_t data_buffer[CHART_SUB_MAX_POINTS]; /* RAM point buffer */
} chart_subscription_t;

/**
 * @brief Initializes the scheduler, mutexes, and zero-initializes all subscription slots.
 */
void chart_query_scheduler_init(void);

/**
 * @brief Registers a new chart subscription.
 *
 * @param dev_addr Device address.
 * @param key Telemetry key (e.g., "depth").
 * @param zone_idx Target zone index.
 * @param interval_sec Bucket interval in seconds (e.g., 300, 7200, 86400).
 * @param point_count Number of data points.
 * @param val_min Minimum range value.
 * @param val_max Maximum range value.
 * @return Subscription ID (0..63) on success, -1 if table is full.
 */
int8_t chart_query_scheduler_subscribe(uint32_t dev_addr, const char *key, uint8_t zone_idx,
                                      uint32_t interval_sec, uint16_t point_count,
                                      float val_min, float val_max);

/**
 * @brief Sets active state of an individual subscription.
 * Triggering an immediate query if transitioning from inactive to active.
 *
 * @param sub_id Subscription ID.
 * @param active True if visible, false if hidden.
 * @return True if subscription exists.
 */
bool chart_query_scheduler_set_active(int8_t sub_id, bool active);

/**
 * @brief Sets active state for all subscriptions matching a specific device address and key.
 *
 * @param dev_addr Device network address.
 * @param key Telemetry key.
 * @param active True if visible, false if hidden.
 */
void chart_query_scheduler_set_binding_active(uint32_t dev_addr, const char *key, bool active);

/**
 * @brief Checks if new archive data is available for a subscription.
 *
 * @param sub_id Subscription ID.
 * @return True if new data arrived and has not been consumed yet.
 */
bool chart_query_scheduler_is_data_ready(int8_t sub_id);

/**
 * @brief Non-blocking data consumption by LVGL widget (clears new_data_ready handshake flag).
 *
 * @param sub_id Subscription ID.
 * @param out_pts Destination buffer for points.
 * @param max_pts Size of destination buffer.
 * @param out_count Number of points copied.
 * @return True if data was consumed, false if no new data was ready.
 */
bool chart_query_scheduler_consume_data(int8_t sub_id, chart_data_point_t *out_pts,
                                        uint16_t max_pts, uint16_t *out_count);

/**
 * @brief Must be called by UART parser when PKT_TYPE_RES_CHART_DATA (0x3C) is received.
 *
 * @param req_id Transaction request ID.
 * @param status Status flag from RP2040 (0 = success).
 * @param step Step size in seconds.
 * @param count Number of received points.
 * @param pts Pointer to array of data points.
 */
void chart_query_scheduler_on_response(uint8_t req_id, uint8_t status, uint32_t step,
                                       uint16_t count, const chart_data_point_t *pts);

/**
 * @brief Periodic worker called every second (handles wall-clock grid triggers, FIFO dispatch, and timeouts).
 */
void chart_query_scheduler_update(void);

#ifdef __cplusplus
}
#endif

#endif /* CHART_QUERY_SCHEDULER_H */