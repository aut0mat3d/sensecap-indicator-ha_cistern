#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include "esp_log.h"

/* Debug domain switches (einkommentieren für verbose output) */
#define DEBUG_LORA
#define DEBUG_UART
#define DEBUG_P2P

#ifdef DEBUG_LORA
    #define DBG_LORA_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
    #define DBG_LORA_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
    #define DBG_LORA_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#else
    #define DBG_LORA_LOGI(tag, fmt, ...) ((void)0)
    #define DBG_LORA_LOGW(tag, fmt, ...) ((void)0)
    #define DBG_LORA_LOGE(tag, fmt, ...) ((void)0)
#endif

#ifdef DEBUG_UART
    #define DBG_UART_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#else
    #define DBG_UART_LOGI(tag, fmt, ...) ((void)0)
#endif

#ifdef DEBUG_P2P
    #define DBG_P2P_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#else
    #define DBG_P2P_LOGI(tag, fmt, ...) ((void)0)
#endif

#endif /* DEBUG_LOG_H */