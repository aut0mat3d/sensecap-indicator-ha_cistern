/**
 * @file debug_log.hpp
 * @brief Zero-overhead domain-specific debug logging macros for RP2040.
 */

#ifndef DEBUG_LOG_HPP
#define DEBUG_LOG_HPP

#include <Arduino.h>

/* ── Non-blocking USB console ───────────────────────────────────────────────
 * Adafruit_USBD_CDC::write() spins while a terminal is "connected" (DTR set)
 * but does not read (e.g. picocom killed without dropping DTR). A blocked log
 * call inside g_comm_mutex stalls both cores and the UART link to the ESP32.
 * LogSerial writes only what fits into the USB FIFO and drops the rest; the
 * number of dropped bytes is reported once the host reads again.
 * Use LogSerial for all console output; Serial is only used for begin(). */
class NonBlockingLog : public Print {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buffer, size_t size) override;
    int availableForWrite() override;
    uint32_t droppedBytes() const { return _dropped; }

private:
    volatile uint32_t _dropped = 0;
};

extern NonBlockingLog LogSerial;

/* ── Debug Domain Flags (Uncomment to enable domain logging) ────────────── */
// #define DEBUG_SD
#define DEBUG_LORA
#define DEBUG_P2P
#define DEBUG_LITTLEFS
// #define DEBUG_UART
//#define DEBUG_I2C

/* ── Debug Print Macros ─────────────────────────────────────────────────── */

#ifdef DEBUG_SD
    #define DBG_SD_PRINTF(fmt, ...) LogSerial.printf("[DBG_SD] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_SD_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_LORA
    #define DBG_LORA_PRINTF(fmt, ...) LogSerial.printf("[DBG_LORA] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_LORA_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_P2P
    #define DBG_P2P_PRINTF(fmt, ...) LogSerial.printf("[DBG_P2P] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_P2P_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_UART
    #define DBG_UART_PRINTF(fmt, ...) LogSerial.printf("[DBG_UART] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_UART_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_LITTLEFS
    #define DBG_LFS_PRINTF(fmt, ...) LogSerial.printf("[DBG_LFS] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_LFS_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_I2C
    #define DBG_I2C_PRINTF(fmt, ...) LogSerial.printf("[DBG_I2C] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_I2C_PRINTF(fmt, ...) ((void)0)
#endif

#endif /* DEBUG_LOG_HPP */