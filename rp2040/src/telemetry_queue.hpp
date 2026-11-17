/**
 * @file telemetry_queue.hpp
 * @brief Thread-safe FIFO telemetry transmission queue with pacing for RP2040.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "pico/mutex.h"
#include "indicator_rp2040.hpp"

#define TELEMETRY_QUEUE_CAPACITY 32

typedef struct {
    uint32_t dev_addr;
    uint32_t fcnt;
    char     key[24];
    char     unit[12];
    float    value;
} telemetry_item_t;

class TelemetryQueue {
private:
    telemetry_item_t items[TELEMETRY_QUEUE_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    mutex_t mtx;
    unsigned long last_tx_ms;
    const unsigned long pacing_ms = 8; /* 8 ms minimum spacing between UART bursts */

public:
    void init() {
        mutex_init(&mtx);
        head = 0;
        tail = 0;
        count = 0;
        last_tx_ms = 0;
        memset(items, 0, sizeof(items));
    }

    bool enqueue(uint32_t dev_addr, uint32_t fcnt, const char* key, const char* unit, float value) {
        if (!key) return false;
        mutex_enter_blocking(&mtx);
        if (count >= TELEMETRY_QUEUE_CAPACITY) {
            /* Drop oldest entry to ensure freshest telemetry values flow through */
            head = (head + 1) % TELEMETRY_QUEUE_CAPACITY;
            count--;
        }

        telemetry_item_t* item = &items[tail];
        item->dev_addr = dev_addr;
        item->fcnt = fcnt;
        strncpy(item->key, key, sizeof(item->key) - 1);
        item->key[sizeof(item->key) - 1] = '\0';
        if (unit) {
            strncpy(item->unit, unit, sizeof(item->unit) - 1);
            item->unit[sizeof(item->unit) - 1] = '\0';
        } else {
            item->unit[0] = '\0';
        }
        item->value = value;

        tail = (tail + 1) % TELEMETRY_QUEUE_CAPACITY;
        count++;
        mutex_exit(&mtx);
        return true;
    }

    void process(PacketSerial& ps) {
        if (count == 0) return;

        unsigned long now = millis();
        if (now - last_tx_ms < pacing_ms) return;

        telemetry_item_t item;
        mutex_enter_blocking(&mtx);
        if (count == 0) {
            mutex_exit(&mtx);
            return;
        }
        item = items[head];
        head = (head + 1) % TELEMETRY_QUEUE_CAPACITY;
        count--;
        mutex_exit(&mtx);

        uint8_t tx_buf[64];
        size_t idx = 0;

        uint8_t pkt_id = (item.dev_addr < 0x01000000) ? 
                         static_cast<uint8_t>(PKT_TYPE_LOCAL_TELEMETRY) : 
                         static_cast<uint8_t>(PKT_TYPE_LORA_TELEMETRY);
        tx_buf[idx++] = pkt_id;

        memcpy(&tx_buf[idx], &item.dev_addr, 4); idx += 4;
        memcpy(&tx_buf[idx], &item.fcnt, 4);     idx += 4;
        tx_buf[idx++] = 0; /* float type (LORA_VAL_TYPE_FLOAT) */
        tx_buf[idx++] = 0; /* measurement context */

        size_t key_len = strlen(item.key) + 1;
        if (idx + key_len <= sizeof(tx_buf)) {
            memcpy(&tx_buf[idx], item.key, key_len);
            idx += key_len;
        }

        size_t unit_len = strlen(item.unit) + 1;
        if (idx + unit_len <= sizeof(tx_buf)) {
            memcpy(&tx_buf[idx], item.unit, unit_len);
            idx += unit_len;
        }

        if (idx + sizeof(float) <= sizeof(tx_buf)) {
            memcpy(&tx_buf[idx], &item.value, sizeof(float));
            idx += sizeof(float);
        }

        uint8_t checksum = 0;
        for (size_t j = 0; j < idx; j++) checksum ^= tx_buf[j];
        tx_buf[idx++] = checksum;

        ps.send(tx_buf, idx);
        last_tx_ms = millis();
    }

    size_t pending() {
        return count;
    }
};

extern TelemetryQueue g_telemetry_tx_queue;

inline void telemetry_tx_init() {
    g_telemetry_tx_queue.init();
}

inline bool telemetry_tx_enqueue(uint32_t dev_addr, uint32_t fcnt, const char* key, const char* unit, float value) {
    return g_telemetry_tx_queue.enqueue(dev_addr, fcnt, key, unit, value);
}

inline void telemetry_tx_process_queue(PacketSerial& ps) {
    g_telemetry_tx_queue.process(ps);
}