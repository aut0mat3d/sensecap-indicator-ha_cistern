/**
 * @file uart_link.cpp
 * @brief Central, paced UART transmit queue to the ESP32 plus link diagnostics.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "uart_link.hpp"
#include "indicator_rp2040.hpp"
#include "debug_log.hpp"
#include "pico/mutex.h"
#include <string.h>

extern PacketSerial packetSerial;

/* Statically initialized: usable by both cores before setup()/setup1() run */
auto_init_mutex(s_link_mtx);

static uint8_t  s_ring[UART_LINK_TX_RING_BYTES];
static size_t   s_head = 0;      /* read position */
static size_t   s_tail = 0;      /* write position */
static size_t   s_used = 0;      /* bytes in use (headers + payloads) */
static uint32_t s_next_tx_us = 0;

/* Transmit staging buffer: one packet copied out of the ring (core 1 only) */
static uint8_t  s_tx_buf[UART_LINK_MAX_PACKET];

#if UART_LINK_DIAG
typedef struct {
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_drops;
    size_t   queue_hwm;
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t rx_overflows;
    uint32_t max_handler_us;
    uint8_t  max_handler_type;
} link_diag_t;

static link_diag_t s_diag;
static uint32_t    s_diag_last_ms = 0;
#endif

static void ring_write(const uint8_t* src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        s_ring[s_tail] = src[i];
        s_tail = (s_tail + 1) % UART_LINK_TX_RING_BYTES;
    }
}

static void ring_read(uint8_t* dst, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dst[i] = s_ring[s_head];
        s_head = (s_head + 1) % UART_LINK_TX_RING_BYTES;
    }
}

void uart_link_init(void) {
    /* Ring state is zero-initialized statically; packets queued by core 0 before this call are kept */
#if UART_LINK_DIAG
    mutex_enter_blocking(&s_link_mtx);
    s_diag_last_ms = millis();
    mutex_exit(&s_link_mtx);
#endif
}

bool uart_link_can_send(size_t len) {
    if (len == 0 || len > UART_LINK_MAX_PACKET) return false;
    mutex_enter_blocking(&s_link_mtx);
    bool fits = (UART_LINK_TX_RING_BYTES - s_used) >= (len + 2);
    mutex_exit(&s_link_mtx);
    return fits;
}

bool uart_link_send(const uint8_t* data, size_t len) {
    if (!data || len == 0 || len > UART_LINK_MAX_PACKET) {
        LogSerial.printf("[LINK] Packet rejected: type=0x%02X len=%u (max %u)\r\n",
                         (data && len) ? data[0] : 0, (unsigned)len, (unsigned)UART_LINK_MAX_PACKET);
        return false;
    }

    mutex_enter_blocking(&s_link_mtx);
    if ((UART_LINK_TX_RING_BYTES - s_used) < (len + 2)) {
#if UART_LINK_DIAG
        s_diag.tx_drops++;
#endif
        mutex_exit(&s_link_mtx);
        return false;
    }

    uint8_t hdr[2] = { (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    ring_write(hdr, 2);
    ring_write(data, len);
    s_used += len + 2;
#if UART_LINK_DIAG
    if (s_used > s_diag.queue_hwm) s_diag.queue_hwm = s_used;
#endif
    mutex_exit(&s_link_mtx);
    return true;
}

#if UART_LINK_DIAG
static void diag_print_periodic(void) {
    uint32_t now_ms = millis();
    if (now_ms - s_diag_last_ms < UART_LINK_DIAG_PERIOD_MS) return;
    s_diag_last_ms = now_ms;

    mutex_enter_blocking(&s_link_mtx);
    link_diag_t d = s_diag;
    size_t used = s_used;
    memset(&s_diag, 0, sizeof(s_diag));
    s_diag.queue_hwm = used;
    mutex_exit(&s_link_mtx);

    if (d.tx_packets == 0 && d.rx_packets == 0 && d.tx_drops == 0 && d.rx_overflows == 0) return;

    LogSerial.printf("[LINK] %lus: TX %lu pkt / %lu B, drops %lu, queue max %u/%u B | "
                     "RX %lu pkt / %lu B, overflows %lu, slowest handler 0x%02X %lu us\r\n",
                     (unsigned long)(UART_LINK_DIAG_PERIOD_MS / 1000),
                     (unsigned long)d.tx_packets, (unsigned long)d.tx_bytes, (unsigned long)d.tx_drops,
                     (unsigned)d.queue_hwm, (unsigned)UART_LINK_TX_RING_BYTES,
                     (unsigned long)d.rx_packets, (unsigned long)d.rx_bytes, (unsigned long)d.rx_overflows,
                     d.max_handler_type, (unsigned long)d.max_handler_us);
}
#endif

void uart_link_process(void) {
#if UART_LINK_DIAG
    diag_print_periodic();
#endif

    uint32_t now_us = micros();
    if ((int32_t)(now_us - s_next_tx_us) < 0) return;

    mutex_enter_blocking(&s_link_mtx);
    if (s_used < 2) {
        mutex_exit(&s_link_mtx);
        return;
    }
    uint8_t hdr[2];
    ring_read(hdr, 2);
    size_t len = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
    ring_read(s_tx_buf, len);
    s_used -= len + 2;
    mutex_exit(&s_link_mtx);

    packetSerial.send(s_tx_buf, len);

    /* Pacing slot for the next packet: proportional to the size of this one */
    uint32_t gap_us = (uint32_t)((len * 1000U) / UART_LINK_BYTES_PER_MS);
    if (gap_us < UART_LINK_MIN_GAP_MS * 1000U) gap_us = UART_LINK_MIN_GAP_MS * 1000U;
    s_next_tx_us = micros() + gap_us;

#if UART_LINK_DIAG
    mutex_enter_blocking(&s_link_mtx);
    s_diag.tx_packets++;
    s_diag.tx_bytes += len;
    mutex_exit(&s_link_mtx);
#endif
}

void uart_link_diag_rx(uint8_t type, size_t len, uint32_t handler_us) {
#if UART_LINK_DIAG
    mutex_enter_blocking(&s_link_mtx);
    s_diag.rx_packets++;
    s_diag.rx_bytes += len;
    if (handler_us > s_diag.max_handler_us) {
        s_diag.max_handler_us = handler_us;
        s_diag.max_handler_type = type;
    }
    mutex_exit(&s_link_mtx);
#else
    (void)type; (void)len; (void)handler_us;
#endif
}

void uart_link_diag_rx_overflow(void) {
#if UART_LINK_DIAG
    mutex_enter_blocking(&s_link_mtx);
    s_diag.rx_overflows++;
    mutex_exit(&s_link_mtx);
#endif
}
