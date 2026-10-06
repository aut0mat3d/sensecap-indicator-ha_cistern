#ifndef LORA_MODEL_H
#define LORA_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UART Packet Type for sending raw LoRa frames from ESP32 to RP2040.
 */
#define PKT_TYPE_LORA_RAW_FRAME 0x32

typedef enum {
    LORA_MODE_DISABLED = 0,
    LORA_MODE_P2P,
    LORA_MODE_MESH
} lora_mode_t;

typedef enum {
    TX_TYPE_LORAWAN_DOWNLINK = 0,
    TX_TYPE_P2P_CLAIM        = 1,
    TX_TYPE_P2P_RETRANSMIT   = 2
} lora_tx_type_t;

/**
 * @brief Structure for queued LoRa transmissions.
 */
typedef struct {
    uint8_t        raw_payload[256];
    uint8_t        payload_len;
    uint32_t       target_timestamp_ms;
    bool           iq_inverted;
    uint8_t        tx_power_dbm;
    lora_tx_type_t msg_type;        /* LORAWAN_DOWNLINK, P2P_CLAIM, P2P_RETRANSMIT */
    uint32_t       dev_addr;
    uint16_t       trigger_fcnt;
    bool           is_auto_reaction;
    bool           valid;
} lora_tx_frame_t;

/**
 * @brief Initializes the LoRa background FreeRTOS task.
 */
void lora_model_init(void);

/**
 * @brief Handles incoming UART packet 0x35 (PKT_TYPE_PUSH_LORA_CFG) from RP2040.
 * 
 * @param buf Raw UART payload buffer.
 * @param len Length of payload buffer in bytes.
 */
void lora_model_on_rp2040_cfg_received(const uint8_t *buf, size_t len);

/**
 * @brief Driver worker called periodically to check IRQ registers, TX queue, and noise floor.
 */
void lora_model_update(void);

/**
 * @brief Prepares and enqueues a LoRaWAN Class A Downlink frame (encrypted with AppSKey + MIC).
 * 
 * @param dev_addr Target device network address.
 * @param fport Target FPort (e.g. 85).
 * @param fcnt_down Downlink frame counter.
 * @param raw_cmd Raw command payload bytes (e.g. FF 84 00).
 * @param cmd_len Length of raw command bytes.
 * @param target_ts_ms Target time on air timestamp in milliseconds.
 * @param app_skey 128-bit Application Session Key.
 * @param nwk_skey 128-bit Network Session Key.
 * @return true if successfully enqueued.
 */
bool lora_model_enqueue_downlink(uint32_t dev_addr, uint8_t fport, uint32_t fcnt_down,
                                 const uint8_t *raw_cmd, size_t cmd_len,
                                 uint32_t target_ts_ms,
                                 const uint8_t *app_skey, const uint8_t *nwk_skey);

/**
 * @brief Prepares and enqueues a P2P CLAIM broadcast packet.
 * 
 * @param dev_addr Target device address claimed by this display.
 * @param target_ts_ms Target time on air timestamp in milliseconds.
 * @return true if successfully enqueued.
 */
bool lora_model_enqueue_claim(uint32_t dev_addr, uint32_t target_ts_ms);

/**
 * @brief Prepares and enqueues a P2P RETRANSMIT broadcast packet (encrypted raw TLV payload).
 * 
 * @param dev_addr Target sensor device address.
 * @param fcnt Uplink frame counter.
 * @param fport Uplink FPort.
 * @param ttl Time-to-Live hop count.
 * @param raw_tlv Decrypted TLV bytes.
 * @param tlv_len Length of TLV payload.
 * @param target_ts_ms Target timestamp in milliseconds.
 * @return true if successfully enqueued.
 */
bool lora_model_enqueue_retransmit(uint32_t dev_addr, uint16_t fcnt, uint8_t fport,
                                   uint8_t ttl, const uint8_t *raw_tlv, size_t tlv_len,
                                   uint32_t target_ts_ms);

/**
 * @brief Purges any pending LoRaWAN Downlink for a specific DevAddr from local TX queue.
 * 
 * @param dev_addr Target DevAddr to purge.
 */
void lora_model_purge_downlink_for_devaddr(uint32_t dev_addr);

/**
 * @brief Prepares and enqueues a Standalone P2P Time Broadcast frame.
 * 
 * @param utc_timestamp Current UTC Unix timestamp.
 * @return true if successfully enqueued.
 */
bool lora_model_enqueue_time_broadcast(uint32_t utc_timestamp);

#ifdef __cplusplus
}
#endif

#endif /* LORA_MODEL_H */