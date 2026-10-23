/**
 * @file debug_log.hpp
 * @brief Zero-overhead domain-specific debug logging macros for RP2040.
 */

#ifndef DEBUG_LOG_HPP
#define DEBUG_LOG_HPP

#include <Arduino.h>

/* ── Debug Domain Flags (Uncomment to enable domain logging) ────────────── */
// #define DEBUG_SD
#define DEBUG_LORA
#define DEBUG_P2P
#define DEBUG_LITTLEFS
// #define DEBUG_UART
//#define DEBUG_I2C

/* ── Debug Print Macros ─────────────────────────────────────────────────── */

#ifdef DEBUG_SD
    #define DBG_SD_PRINTF(fmt, ...) Serial.printf("[DBG_SD] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_SD_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_LORA
    #define DBG_LORA_PRINTF(fmt, ...) Serial.printf("[DBG_LORA] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_LORA_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_P2P
    #define DBG_P2P_PRINTF(fmt, ...) Serial.printf("[DBG_P2P] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_P2P_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_UART
    #define DBG_UART_PRINTF(fmt, ...) Serial.printf("[DBG_UART] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_UART_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_LITTLEFS
    #define DBG_LFS_PRINTF(fmt, ...) Serial.printf("[DBG_LFS] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_LFS_PRINTF(fmt, ...) ((void)0)
#endif

#ifdef DEBUG_I2C
    #define DBG_I2C_PRINTF(fmt, ...) Serial.printf("[DBG_I2C] " fmt "\r\n", ##__VA_ARGS__)
#else
    #define DBG_I2C_PRINTF(fmt, ...) ((void)0)
#endif

#endif /* DEBUG_LOG_HPP */