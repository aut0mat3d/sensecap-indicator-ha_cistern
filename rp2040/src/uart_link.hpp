/**
 * @file uart_link.hpp
 * @brief Central, paced UART transmit queue to the ESP32 plus link diagnostics.
 *
 * Every packet to the ESP32 goes through uart_link_send(): it is copied into a ring buffer
 * (callable from both cores) and only core 1 writes it to Serial1 in uart_link_process().
 * Packets are paced: after a packet of n bytes the next one waits at least
 * max(UART_LINK_MIN_GAP_MS, n / UART_LINK_BYTES_PER_MS) milliseconds. The ESP32 thus gets
 * time to decode and dispatch every packet, and bursts (config streaming, alarm history,
 * chart data, LoRa frames with many keys) are spread out instead of hitting the ESP32 at once.
 *
 * Producers that send many or large packets check uart_link_can_send() first and continue
 * in a later loop iteration (never block, never delay()).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#ifndef UART_LINK_HPP
#define UART_LINK_HPP

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/** Ring buffer size for queued packets (payload + 2 byte length header each) */
#define UART_LINK_TX_RING_BYTES   6144U
/** Largest single packet (before COBS); must stay below the 2048 B receive buffers */
#define UART_LINK_MAX_PACKET      1536U
/** Minimum pause between two packets */
#define UART_LINK_MIN_GAP_MS      2U
/** Pacing budget: ~32 KB/s, about a third of the 921600 Bd line rate */
#define UART_LINK_BYTES_PER_MS    32U

/** Link diagnostics (counters + periodic summary on LogSerial). 0 = compiled out */
#ifndef UART_LINK_DIAG
#define UART_LINK_DIAG            1
#endif
/** Interval of the diagnostic summary (only printed when there was traffic) */
#define UART_LINK_DIAG_PERIOD_MS  30000U

void uart_link_init(void);

/**
 * @brief Queue one packet (raw payload, COBS encoding happens on transmit). Thread-safe.
 * @return false if the packet is too large or the queue has no room (packet dropped, counted).
 */
bool uart_link_send(const uint8_t* data, size_t len);

/**
 * @brief true if a packet of len bytes fits into the queue right now.
 */
bool uart_link_can_send(size_t len);

/**
 * @brief Transmit at most one queued packet (via the global packetSerial) if its pacing slot
 *        has come. Core 1 only. Also prints the periodic diagnostic summary (UART_LINK_DIAG).
 */
void uart_link_process(void);

/* ── Diagnostic hooks (no-ops when UART_LINK_DIAG is 0) ─────────────────── */

/** Record one received packet and the time its handler took */
void uart_link_diag_rx(uint8_t type, size_t len, uint32_t handler_us);
/** Record a PacketSerial receive buffer overflow */
void uart_link_diag_rx_overflow(void);

#endif /* UART_LINK_HPP */
