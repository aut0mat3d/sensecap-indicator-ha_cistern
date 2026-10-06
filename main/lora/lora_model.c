/**
 * @file lora_model.c
 * @brief ESP32-S3 LoRa Receiver, Frame Processing & TX Queue Engine using Seeed HAL.
 */

#include "lora_model.h"
#include "radio.h"
#include <sys/time.h>
#include <time.h>
#include "sntp_service.h"
#include "sx126x-board.h"
#include "debug_log.h"
#include <string.h>
#include <stdlib.h>
#include "rp2040.h"
#include "mbedtls/aes.h"
#include "mbedtls/cmac.h"

static const char *TAG = "lora_model";
static int16_t s_last_noise_floor = -100;
bool s_time_synced_via_ntp = false;

/* ── Packed Mirror Structs matching RP2040 Layout ──────────────────────── */

typedef struct __attribute__((packed)) {
    bool     enabled;         /* Enable P2P Mesh features */
    char     device_id[16];   /* Node ID ("AUTO" or custom string) */
    uint16_t network_id;      /* Mesh Network ID (e.g. 0x4A6F) */
    uint8_t  aes_key[16];     /* 128-bit Mesh AES Encryption Key */
    uint8_t  broadcast_ttl;   /* Default TTL hop count */
} lora_mesh_config_t;

typedef struct __attribute__((packed)) {
    char posix_timezone[64];  /* e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */
    char time_format[8];      /* "24h" or "12h" */
    char ntp_server[32];      /* e.g. "pool.ntp.org" */
    bool p2p_sync_enabled;    /* Enable P2P time mesh */
} lora_time_config_t;

typedef struct __attribute__((packed)) {
    uint32_t           rf_frequency_hz;
    uint8_t            rf_spreading_factor;
    uint16_t           rf_bandwidth_khz;
    uint8_t            rf_coding_rate;
    uint8_t            rf_preamble_len;
    uint8_t            sync_word;
    lora_mesh_config_t mesh;
    lora_time_config_t time_cfg; /* Integrated Time Configuration */
} lora_global_config_t;

typedef struct __attribute__((packed)) {
    uint8_t trigger_fport;      /* Trigger FPort match (e.g. 13) */
    uint8_t trigger_prefix;     /* Trigger byte prefix (e.g. 0x55) */
    bool    trigger_encrypted;  /* False if payload on trigger FPort is unencrypted */
    uint8_t target_fport;       /* Target FPort for downlink (e.g. 85) */
    uint8_t payload_len;        /* Length of payload_bytes */
    uint8_t payload_bytes[16];  /* Raw command bytes (e.g. FF 84 00) */
    uint8_t max_retries;        /* Maximum retries, e.g. 3 */
    bool    valid;              /* Whether reaction rule is active */
} lora_device_reaction_t;

typedef struct __attribute__((packed)) {
    uint8_t fport;           /* Target FPort (e.g. 13) */
    uint8_t encryption_mode; /* 0=full, 1=none, 2=partial */
    uint8_t cipher_offset;   /* Offset where AES CTR starts (e.g. 1) */
    uint8_t key_type;        /* 0=app_skey, 1=nwk_skey */
    uint8_t payload_offset;  /* Offset where parser starts reading TLV (e.g. 1) */
    bool    valid;           /* Rule active */
} lora_fport_config_t;

typedef struct __attribute__((packed)) {
    char                   device_name[32];
    char                   medium[16];
    bool                   enabled;
    uint32_t               dev_addr;
    char                   dev_sn[32];
    char                   dev_eui[32];
    uint8_t                nwk_skey[16];
    uint8_t                app_skey[16];
    char                   decoder[32];
    int32_t                raw_offset_mm;
    lora_device_reaction_t reaction;
    uint32_t               fcnt_down;
    lora_fport_config_t    fport_cfg;
} lora_device_config_t;

#define MAX_LORA_DEVICES 16
#define MAX_TX_QUEUE_SIZE 8
#define DUP_CACHE_SIZE 16

typedef struct {
    uint32_t dev_addr;
    uint16_t fcnt;
} lora_dup_entry_t;

static lora_global_config_t g_rf_cfg;
static lora_device_config_t g_devices[MAX_LORA_DEVICES];
static uint8_t              g_device_count = 0;
static bool                 g_radio_active = false;

static lora_dup_entry_t     s_dup_cache[DUP_CACHE_SIZE];
static uint8_t              s_dup_idx = 0;

/* ── Anti-Looping Duplicate Cache Helpers ───────────────────────────────── */

static bool is_duplicate_frame(uint32_t dev_addr, uint16_t fcnt) {
    for (uint8_t i = 0; i < DUP_CACHE_SIZE; i++) {
        if (s_dup_cache[i].dev_addr == dev_addr && s_dup_cache[i].fcnt == fcnt) {
            return true;
        }
    }
    return false;
}

static void add_duplicate_frame(uint32_t dev_addr, uint16_t fcnt) {
    s_dup_cache[s_dup_idx].dev_addr = dev_addr;
    s_dup_cache[s_dup_idx].fcnt = fcnt;
    s_dup_idx = (s_dup_idx + 1) % DUP_CACHE_SIZE;
}

/* ── TX Queue & Radio State ─────────────────────────────────────────────── */

static lora_tx_frame_t g_tx_queue[MAX_TX_QUEUE_SIZE];
static bool            g_tx_in_progress = false;

static void lora_model_task(void *pvParameters) {
    while (1) {
        lora_model_update();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void lora_model_init(void) {
    memset(g_tx_queue, 0, sizeof(g_tx_queue));
    memset(s_dup_cache, 0, sizeof(s_dup_cache));
    xTaskCreate(lora_model_task, "lora_model_task", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "LoRa background task initialized.");
}

/* ── Seeed Radio Callback Declarations ──────────────────────────────────── */

static void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr);
static void OnRxTimeout(void);
static void OnRxError(void);
static void OnTxDone(void);
static void OnTxTimeout(void);
static void OnCadDone(bool channelActivityDetected);

static RadioEvents_t RadioEvents = {
    .TxDone    = OnTxDone,
    .TxTimeout = OnTxTimeout,
    .RxDone    = OnRxDone,
    .RxTimeout = OnRxTimeout,
    .RxError   = OnRxError,
    .CadDone   = OnCadDone,
};

/* ── SX1262 Radio Initialization ────────────────────────────────────────── */
static void lora_model_apply_rf_config(void) {
    static bool radio_hw_initialized = false;
    ESP_LOGI(TAG, "Initializing SX1262 via Seeed HAL...");

    /* 1. Low-Level SPI Bus, Mutex & PCA9535 Expander Init */
    bsp_sx126x_init();

    /* 2. Guard against missing or invalid Radio driver pointers */
    if (Radio.Init == NULL || Radio.SetChannel == NULL || Radio.SetRxConfig == NULL) {
        ESP_LOGE(TAG, "SX1262 Radio HAL pointers invalid! Skipping RF config.");
        return;
    }

    ESP_LOGI(TAG, "RF Config: Freq=%lu Hz, SF=%u, BW=%u kHz, Sync=0x%02X, MeshNetID=0x%04X, TTL=%u",
             g_rf_cfg.rf_frequency_hz, g_rf_cfg.rf_spreading_factor,
             g_rf_cfg.rf_bandwidth_khz, g_rf_cfg.sync_word,
             g_rf_cfg.mesh.network_id, g_rf_cfg.mesh.broadcast_ttl);

    /* 3. Initialize Radio HW & Callbacks ONLY ONCE */
    if (!radio_hw_initialized) {
        Radio.Init(&RadioEvents);
        
        /* Activate TCXO power supply via DIO3 (1.8V, 5ms delay) */
        SX126xSetDio3AsTcxoCtrl(TCXO_CTRL_1_8V, 500);

        /* Route DIO2 to control internal RF Switch (RX/TX antenna relay) */
        SX126xSetDio2AsRfSwitchCtrl(true);

        radio_hw_initialized = true;
    }

    /* 4. Set Channel Frequency */
    Radio.SetChannel(g_rf_cfg.rf_frequency_hz);

    /* 5. Bandwidth & Coding Rate Normalization */
    uint8_t bw_idx = 0;
    if (g_rf_cfg.rf_bandwidth_khz >= 500) bw_idx = 2;
    else if (g_rf_cfg.rf_bandwidth_khz >= 250) bw_idx = 1;

    /* Normalize Coding Rate: 5 (4/5) -> 1 (LORA_CR_4_5) */
    uint8_t cr = g_rf_cfg.rf_coding_rate;
    if (cr > 4) {
        cr = cr - 4;
    }

    /* 6. Configure RX parameters for Continuous P2P Listening */
    Radio.SetRxConfig(
        MODEM_LORA,
        bw_idx,
        g_rf_cfg.rf_spreading_factor,
        cr,                       /* Normalized coding rate */
        0,                        /* Bandwidth AFC */
        g_rf_cfg.rf_preamble_len,
        0,                        /* SymbTimeout */
        false,                    /* FixLen */
        0,                        /* PayloadLen */
        true,                     /* CrcOn */
        0,                        /* FreqHopOn */
        0,                        /* HopPeriod */
        false,                    /* IqInverted = false (Uplinks from End-Devices use normal IQ) */
        true                      /* RxContinuous */
    );

    /* 7. Apply SyncWord AFTER SetRxConfig */
    if (g_rf_cfg.sync_word == 0x34) {
        Radio.SetPublicNetwork(true);  /* Sets REG_LR_SYNCWORD to 0x3444 */
    } else {
        Radio.SetPublicNetwork(false); /* Sets REG_LR_SYNCWORD to 0x1424 */
    }

    /* 8. Enter Continuous RX Mode with Boosted LNA Gain */
    Radio.RxBoosted(0);
    g_radio_active = true;
    ESP_LOGI(TAG, "SX1262 RF Switch & TCXO active! Listening in Boosted LNA LoRa P2P mode.");
}

/* ── Hardware Payload Cryptography & MIC Calculation ────────────────────── */

/**
 * @brief LoRaWAN 1.0.x Payload Encryption/Decryption using ESP32 Hardware AES (AES-128 CTR Mode)
 */
static void lorawan_cipher_payload(const uint8_t *key, uint32_t dev_addr, uint8_t dir, uint32_t fcnt, 
                                   const uint8_t *input, size_t size, uint8_t *output) {
    if (!key || !input || !output || size == 0) return;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);

    uint8_t block_a[16] = {0};
    uint8_t s_block[16] = {0};

    /* Construct LoRaWAN Block A_i Header */
    block_a[0] = 0x01;
    block_a[5] = dir; /* 0x00 = Uplink, 0x01 = Downlink */
    memcpy(&block_a[6], &dev_addr, 4);
    memcpy(&block_a[10], &fcnt, 4);

    size_t num_blocks = (size + 15) / 16;
    for (size_t b = 0; b < num_blocks; b++) {
        block_a[15] = (uint8_t)(b + 1); /* Block Sequence Counter */
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block_a, s_block);

        size_t block_bytes = (size - (b * 16) >= 16) ? 16 : (size - (b * 16));
        for (size_t i = 0; i < block_bytes; i++) {
            output[b * 16 + i] = input[b * 16 + i] ^ s_block[i];
        }
    }

    mbedtls_aes_free(&aes);
}

/**
 * @brief P2P Mesh Payload Encryption/Decryption using AES-128 CTR Mode with Mesh Key
 */
static void lora_mesh_cipher_payload(const uint8_t *key, uint32_t dev_addr, uint16_t fcnt,
                                     const uint8_t *input, size_t size, uint8_t *output) {
    if (!key || !input || !output || size == 0) return;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);

    uint8_t block_a[16] = {0};
    uint8_t s_block[16] = {0};

    block_a[0] = 0x4D; /* 'M' for Mesh */
    memcpy(&block_a[1], &dev_addr, 4);
    memcpy(&block_a[5], &fcnt, 2);

    size_t num_blocks = (size + 15) / 16;
    for (size_t b = 0; b < num_blocks; b++) {
        block_a[15] = (uint8_t)(b + 1);
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block_a, s_block);

        size_t block_bytes = (size - (b * 16) >= 16) ? 16 : (size - (b * 16));
        for (size_t i = 0; i < block_bytes; i++) {
            output[b * 16 + i] = input[b * 16 + i] ^ s_block[i];
        }
    }

    mbedtls_aes_free(&aes);
}

/**
 * @brief LoRaWAN 1.0.x MIC Calculation using AES-128 CMAC over B0 + Message.
 */
static void lorawan_calculate_mic(const uint8_t *nwk_skey, uint32_t dev_addr, uint8_t dir, uint32_t fcnt,
                                  const uint8_t *msg, size_t msg_len, uint8_t *out_mic) {
    if (!nwk_skey || !msg || !out_mic || msg_len == 0) return;

    uint8_t b0[16] = {0};
    b0[0] = 0x49;
    b0[5] = dir; /* 0x00 = Uplink, 0x01 = Downlink */
    memcpy(&b0[6], &dev_addr, 4);
    memcpy(&b0[10], &fcnt, 4);
    b0[15] = (uint8_t)msg_len;

    /* Prepare CMAC buffer = B0 (16B) + Msg (msg_len B) */
    size_t cmac_input_len = 16 + msg_len;
    uint8_t *cmac_buf = (uint8_t*)malloc(cmac_input_len);
    if (!cmac_buf) return;

    memcpy(cmac_buf, b0, 16);
    memcpy(&cmac_buf[16], msg, msg_len);

    const mbedtls_cipher_info_t *cipher_info = mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB);
    uint8_t cmac_out[16] = {0};

    mbedtls_cipher_cmac(cipher_info, nwk_skey, 128, cmac_buf, cmac_input_len, cmac_out);
    memcpy(out_mic, cmac_out, 4); /* First 4 bytes form LoRaWAN MIC */

    free(cmac_buf);
}

/* ── Public Queue API Implementation ────────────────────────────────────── */

void lora_model_purge_downlink_for_devaddr(uint32_t dev_addr) {
    for (size_t i = 0; i < MAX_TX_QUEUE_SIZE; i++) {
        if (g_tx_queue[i].valid && g_tx_queue[i].dev_addr == dev_addr &&
            g_tx_queue[i].msg_type == TX_TYPE_LORAWAN_DOWNLINK) {
            
            if (g_tx_queue[i].is_auto_reaction) {
                g_tx_queue[i].valid = false;
                ESP_LOGI(TAG, "PURGED auto-reaction downlink for DevAddr 0x%08lX", (unsigned long)dev_addr);
            } else {
                g_tx_queue[i].target_timestamp_ms = 0;
                ESP_LOGI(TAG, "DEFERRED manual command for DevAddr 0x%08lX to next RX1 window", (unsigned long)dev_addr);
            }
        }
    }
}

bool lora_model_enqueue_downlink(uint32_t dev_addr, uint8_t fport, uint32_t fcnt_down,
                                 const uint8_t *raw_cmd, size_t cmd_len,
                                 uint32_t target_ts_ms,
                                 const uint8_t *app_skey, const uint8_t *nwk_skey) {
    if (!raw_cmd || cmd_len == 0 || !app_skey || !nwk_skey) return false;

    int slot = -1;
    for (size_t i = 0; i < MAX_TX_QUEUE_SIZE; i++) {
        if (!g_tx_queue[i].valid) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0) {
        ESP_LOGE(TAG, "TX Queue Full! Cannot enqueue LoRaWAN downlink.");
        return false;
    }

    lora_tx_frame_t *frame = &g_tx_queue[slot];
    memset(frame, 0, sizeof(lora_tx_frame_t));

    /* 1. MHDR (Unconfirmed Data Down) */
    frame->raw_payload[0] = 0x60;

    /* 2. FHDR: DevAddr (4B) + FCtrl (1B) + FCnt (2B) */
    memcpy(&frame->raw_payload[1], &dev_addr, 4);
    frame->raw_payload[5] = 0x00; /* FCtrl */
    uint16_t fcnt16 = (uint16_t)(fcnt_down & 0xFFFF);
    memcpy(&frame->raw_payload[6], &fcnt16, 2);

    /* 3. FPort */
    frame->raw_payload[8] = fport;

    /* 4. Encrypt FRMPayload using AppSKey in Downlink direction (dir = 0x01) */
    lorawan_cipher_payload(app_skey, dev_addr, 0x01, fcnt_down, raw_cmd, cmd_len, &frame->raw_payload[9]);

    size_t msg_len = 9 + cmd_len;

    /* 5. Calculate MIC using NwkSKey in Downlink direction (dir = 0x01) */
    uint8_t mic[4];
    lorawan_calculate_mic(nwk_skey, dev_addr, 0x01, fcnt_down, frame->raw_payload, msg_len, mic);
    memcpy(&frame->raw_payload[msg_len], mic, 4);

    frame->payload_len = (uint8_t)(msg_len + 4);
    frame->target_timestamp_ms = target_ts_ms;
    frame->iq_inverted = true; /* Class A Downlinks use inverted IQ */
    frame->tx_power_dbm = 14;
    frame->msg_type = TX_TYPE_LORAWAN_DOWNLINK;
    frame->dev_addr = dev_addr;
    frame->trigger_fcnt = fcnt16;
    frame->is_auto_reaction = true;
    frame->valid = true;

    ESP_LOGI(TAG, "Enqueued LoRaWAN Downlink for DevAddr 0x%08lX (Target: %lu ms, %u bytes)",
             (unsigned long)dev_addr, (unsigned long)target_ts_ms, frame->payload_len);
    return true;
}

bool lora_model_enqueue_claim(uint32_t dev_addr, uint32_t target_ts_ms) {
    int slot = -1;
    for (size_t i = 0; i < MAX_TX_QUEUE_SIZE; i++) {
        if (!g_tx_queue[i].valid) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0) {
        ESP_LOGE(TAG, "TX Queue Full! Cannot enqueue CLAIM broadcast.");
        return false;
    }

    lora_tx_frame_t *frame = &g_tx_queue[slot];
    memset(frame, 0, sizeof(lora_tx_frame_t));

    /* P2P CLAIM Header: SyncWord (0x34) + Type 0x01 + DevAddr (4B) */
    frame->raw_payload[0] = (uint8_t)(g_rf_cfg.sync_word);
    frame->raw_payload[1] = 0x01; /* MsgType: P2P_CLAIM */
    memcpy(&frame->raw_payload[2], &dev_addr, 4);

    frame->payload_len = 6;
    frame->target_timestamp_ms = target_ts_ms;
    frame->iq_inverted = false; /* P2P uses normal IQ */
    frame->tx_power_dbm = 14;
    frame->msg_type = TX_TYPE_P2P_CLAIM;
    frame->dev_addr = dev_addr;
    frame->valid = true;

    ESP_LOGI(TAG, "Enqueued P2P CLAIM for DevAddr 0x%08lX (Target: %lu ms)",
             (unsigned long)dev_addr, (unsigned long)target_ts_ms);
    return true;
}

bool lora_model_enqueue_retransmit(uint32_t dev_addr, uint16_t fcnt, uint8_t fport,
                                   uint8_t ttl, const uint8_t *raw_tlv, size_t tlv_len,
                                   uint32_t target_ts_ms) {
    if (!raw_tlv || tlv_len == 0 || ttl == 0 || !g_rf_cfg.mesh.enabled) return false;

    int slot = -1;
    for (size_t i = 0; i < MAX_TX_QUEUE_SIZE; i++) {
        if (!g_tx_queue[i].valid) {
            slot = (int)i;
            break;
        }
    }

    if (slot < 0) {
        ESP_LOGE(TAG, "TX Queue Full! Cannot enqueue P2P Retransmit broadcast.");
        return false;
    }

    lora_tx_frame_t *frame = &g_tx_queue[slot];
    memset(frame, 0, sizeof(lora_tx_frame_t));

    /* P2P RETRANSMIT Header (14 Bytes):
       Byte 0: SyncWord (e.g. 0x34)
       Byte 1: Type (0x02 = TX_TYPE_P2P_RETRANSMIT)
       Bytes 2..5: DevAddr (4B)
       Byte 6: TTL (4 bits)
       Bytes 7..8: FCnt (2B)
       Byte 9: FPort (1B)
       Bytes 10..13: UTC Timestamp (4B)
    */
    frame->raw_payload[0] = (uint8_t)(g_rf_cfg.sync_word);
    frame->raw_payload[1] = 0x02; /* TX_TYPE_P2P_RETRANSMIT */
    memcpy(&frame->raw_payload[2], &dev_addr, 4);
    frame->raw_payload[6] = ttl & 0x0F;
    memcpy(&frame->raw_payload[7], &fcnt, 2);
    frame->raw_payload[9] = fport;

    /* Insert current UTC timestamp (0 if RTC is not yet synced) */
    uint32_t current_utc = (uint32_t)time(NULL);
    if (current_utc < 1700000000) current_utc = 0;
    memcpy(&frame->raw_payload[10], &current_utc, 4);

    /* Encrypt raw TLV payload starting at byte 14 using Mesh AES key */
    lora_mesh_cipher_payload(g_rf_cfg.mesh.aes_key, dev_addr, fcnt, raw_tlv, tlv_len, &frame->raw_payload[14]);

    frame->payload_len = (uint8_t)(14 + tlv_len);
    frame->target_timestamp_ms = target_ts_ms;
    frame->iq_inverted = false; /* P2P Mesh uses normal IQ */
    frame->tx_power_dbm = 14;
    frame->msg_type = TX_TYPE_P2P_RETRANSMIT;
    frame->dev_addr = dev_addr;
    frame->trigger_fcnt = fcnt;
    frame->is_auto_reaction = false;
    frame->valid = true;

    ESP_LOGI(TAG, "Enqueued P2P RETRANSMIT for DevAddr 0x%08lX (TTL: %u, UTC: %lu, Target: %lu ms, %u bytes)",
             (unsigned long)dev_addr, ttl, (unsigned long)current_utc, (unsigned long)target_ts_ms, frame->payload_len);
    return true;
}

/* ── Rx Configuration Restore Helper ───────────────────────────────────── */

static void lora_model_restore_rx(void) {
    uint8_t bw_idx = 0;
    if (g_rf_cfg.rf_bandwidth_khz >= 500) bw_idx = 2;
    else if (g_rf_cfg.rf_bandwidth_khz >= 250) bw_idx = 1;

    uint8_t cr = g_rf_cfg.rf_coding_rate;
    if (cr > 4) cr = cr - 4;

    /* Re-apply RX configuration with iqInverted = false for normal Uplink reception */
    Radio.SetRxConfig(
        MODEM_LORA,
        bw_idx,
        g_rf_cfg.rf_spreading_factor,
        cr,
        0,
        g_rf_cfg.rf_preamble_len,
        0,
        false,
        0,
        true,
        0,
        0,
        false, /* IqInverted = false (Normal IQ) */
        true   /* RxContinuous */
    );

    Radio.RxBoosted(0);
}

/* ── Standalone Time Broadcast Enqueue Helper ─────────────────────────── */

bool lora_model_enqueue_time_broadcast(uint32_t utc_timestamp) {
    if (!g_rf_cfg.mesh.enabled || utc_timestamp < 1700000000) return false;

    int slot = -1;
    for (size_t i = 0; i < MAX_TX_QUEUE_SIZE; i++) {
        if (!g_tx_queue[i].valid) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) return false;

    lora_tx_frame_t *frame = &g_tx_queue[slot];
    memset(frame, 0, sizeof(lora_tx_frame_t));

    frame->raw_payload[0] = (uint8_t)(g_rf_cfg.sync_word);
    frame->raw_payload[1] = 0x03; /* TX_TYPE_P2P_TIME */
    memcpy(&frame->raw_payload[2], &g_rf_cfg.mesh.network_id, 2);
    memcpy(&frame->raw_payload[4], &utc_timestamp, 4);

    frame->payload_len = 8;
    frame->target_timestamp_ms = xTaskGetTickCount() * portTICK_PERIOD_MS + 100 + (Radio.Random() % 500);
    frame->iq_inverted = false;
    frame->tx_power_dbm = 14;
    frame->msg_type = TX_TYPE_P2P_RETRANSMIT;
    frame->valid = true;

    ESP_LOGI(TAG, "Enqueued Standalone P2P Time Broadcast (UTC: %lu)", (unsigned long)utc_timestamp);
    return true;
}

/* ── Seeed Radio Event Handlers ─────────────────────────────────────────── */

static void OnRxDone(uint8_t *payload, uint16_t size, int16_t rssi, int8_t snr) {
    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "LoRa Packet Received! Size: %u bytes | RSSI: %d dBm | SNR: %d dB", size, rssi, snr);

    /* 1. Check for incoming P2P CLAIM broadcast from another display (Type 0x01) */
    if (size == 6 && payload[0] == (uint8_t)(g_rf_cfg.sync_word) && payload[1] == 0x01) {
        uint32_t claimed_dev_addr = 0;
        memcpy(&claimed_dev_addr, &payload[2], 4);
        ESP_LOGI(TAG, "P2P CLAIM received from another display for DevAddr: 0x%08lX", (unsigned long)claimed_dev_addr);
        
        /* Purge our own pending downlink from queue to avoid collision or double-sending */
        lora_model_purge_downlink_for_devaddr(claimed_dev_addr);
        Radio.RxBoosted(0);
        return;
    }

    /* 2. Check for incoming Standalone P2P Time Broadcast (Type 0x03) */
    if (size == 8 && payload[0] == (uint8_t)(g_rf_cfg.sync_word) && payload[1] == 0x03) {
        uint16_t net_id = 0;
        memcpy(&net_id, &payload[2], 2);

        /* Verify matching Mesh Network ID */
        if (net_id == g_rf_cfg.mesh.network_id) {
            uint32_t remote_utc = 0;
            memcpy(&remote_utc, &payload[4], 4);

            /* Synchronize local RTC if time is valid and device is not synced via NTP */
            if (remote_utc > 1700000000 && !s_time_synced_via_ntp) {
                struct timeval tv = { .tv_sec = (time_t)remote_utc, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "RTC synced via P2P Standalone Time Broadcast: %lu", (unsigned long)remote_utc);
            }
        }
        Radio.RxBoosted(0);
        return;
    }

    /* 3. Check for incoming P2P RETRANSMIT broadcast with piggybacked timestamp (Type 0x02) */
    if (size >= 14 && payload[0] == (uint8_t)(g_rf_cfg.sync_word) && payload[1] == 0x02) {
        uint32_t rx_dev_addr = 0;
        memcpy(&rx_dev_addr, &payload[2], 4);
        uint8_t ttl = payload[6] & 0x0F;
        uint16_t fcnt = payload[7] | (payload[8] << 8);
        uint8_t fport = payload[9];
        uint32_t remote_utc = 0;
        memcpy(&remote_utc, &payload[10], 4);
        size_t tlv_len = size - 14;

        ESP_LOGI(TAG, "P2P RETRANSMIT frame received for DevAddr: 0x%08lX (FCnt: %u, TTL: %u, Size: %u)",
                 (unsigned long)rx_dev_addr, fcnt, ttl, (unsigned)tlv_len);

        /* STEP 1: Process timestamp FIRST before evaluating duplicate cache */
        if (remote_utc > 1700000000 && !s_time_synced_via_ntp) {
            struct timeval tv = { .tv_sec = (time_t)remote_utc, .tv_usec = 0 };
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "RTC synced via P2P Retransmit Header: %lu", (unsigned long)remote_utc);
        }

        /* STEP 2: Anti-looping duplicate telemetry check */
        if (is_duplicate_frame(rx_dev_addr, fcnt)) {
            ESP_LOGI(TAG, "Duplicate P2P RETRANSMIT telemetry frame discarded (DevAddr: 0x%08lX, FCnt: %u)",
                     (unsigned long)rx_dev_addr, fcnt);
            Radio.RxBoosted(0);
            return;
        }
        add_duplicate_frame(rx_dev_addr, fcnt);

        /* STEP 3: Decrypt Mesh payload and construct synthetic LoRaWAN frame for RP2040 */
        uint8_t decrypted_tlv[200] = {0};
        if (tlv_len > sizeof(decrypted_tlv)) tlv_len = sizeof(decrypted_tlv);
        lora_mesh_cipher_payload(g_rf_cfg.mesh.aes_key, rx_dev_addr, fcnt, &payload[14], tlv_len, decrypted_tlv);

        uint8_t synthetic_frame[256] = {0};
        synthetic_frame[0] = 0x40; /* Unconfirmed Data Up MHDR */
        memcpy(&synthetic_frame[1], &rx_dev_addr, 4);
        synthetic_frame[5] = 0x00;
        memcpy(&synthetic_frame[6], &fcnt, 2);
        synthetic_frame[8] = fport;
        memcpy(&synthetic_frame[9], decrypted_tlv, tlv_len);
        size_t synthetic_len = 9 + tlv_len + 4; /* +4 dummy MIC bytes */

        /* Forward decrypted frame to RP2040 via UART (Cmd 0x36) */
        int ret = rp2040_send_lora_raw_frame(rx_dev_addr, rssi, snr, synthetic_frame, (uint8_t)synthetic_len);
        if (ret > 0) {
            ESP_LOGI(TAG, "Forwarded decrypted P2P RETRANSMIT frame to RP2040 (DevAddr: 0x%08lX)", (unsigned long)rx_dev_addr);
        }

        /* Propagate Mesh Frame if TTL > 1 */
        if (g_rf_cfg.mesh.enabled && ttl > 1) {
            uint32_t retransmit_delay = 1300 + (Radio.Random() % 3700);
            uint32_t retransmit_ts = now_ms + retransmit_delay;
            lora_model_enqueue_retransmit(rx_dev_addr, fcnt, fport, ttl - 1, decrypted_tlv, tlv_len, retransmit_ts);
        }

        Radio.RxBoosted(0);
        return;
    }

    /* 4. Direct LoRaWAN Sensor Uplink Processing */
    if (size >= 5) {
        uint32_t rx_dev_addr = ((uint32_t)payload[4] << 24) |
                               ((uint32_t)payload[3] << 16) |
                               ((uint32_t)payload[2] << 8)  |
                                (uint32_t)payload[1];

        const lora_device_config_t *matched = NULL;
        for (uint8_t i = 0; i < g_device_count; i++) {
            if (g_devices[i].enabled && g_devices[i].dev_addr == rx_dev_addr) {
                matched = &g_devices[i];
                break;
            }
        }

        if (matched) {
            ESP_LOGI(TAG, "MATCH FOUND: Device '%s' (Decoder: %s)",
                     matched->device_name, matched->decoder);
        } else {
            ESP_LOGW(TAG, "Ignored frame from unknown DevAddr: 0x%08lX", (unsigned long)rx_dev_addr);
        }

        /* Dispatch RF Diagnostic Event to LVGL Display View */
        struct view_data_lora_diag diag_evt = {
            .rssi = rssi,
            .snr = snr,
            .noise_floor = s_last_noise_floor,
            .dev_addr = rx_dev_addr
        };
        if (matched) {
            strncpy(diag_evt.dev_name, matched->device_name, sizeof(diag_evt.dev_name) - 1);
        } else {
            snprintf(diag_evt.dev_name, sizeof(diag_evt.dev_name), "0x%08lX", (unsigned long)rx_dev_addr);
        }
        esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_DIAG_DATA, &diag_evt, sizeof(diag_evt), portMAX_DELAY);

        /* Standard-conform LoRaWAN 1.0.x Key selection & Partial Decryption */
        uint8_t fport = 0;
        uint16_t fcnt = 0;
        size_t frm_len = 0;

        if (matched && size >= 13) {
            fcnt  = payload[6] | (payload[7] << 8);
            fport = payload[8];
            frm_len = size - 9 - 4;

            add_duplicate_frame(rx_dev_addr, fcnt);

            if (frm_len > 0) {
                const lora_fport_config_t *fp_cfg = &matched->fport_cfg;
                bool is_custom = (fp_cfg->valid && fp_cfg->fport == fport);

                uint8_t enc_mode = is_custom ? fp_cfg->encryption_mode : 0; /* Default: 0 (full) */
                uint8_t cipher_offset = is_custom ? fp_cfg->cipher_offset : 0;
                
                if (enc_mode == 1) { /* None */
                    ESP_LOGI(TAG, "Bypassed AES decryption for unencrypted FPort %u", fport);
                } else if (enc_mode == 2) { /* Partial */
                    if (cipher_offset < frm_len) {
                        const uint8_t *key_to_use = (fp_cfg->key_type == 1) ? matched->nwk_skey : matched->app_skey;
                        size_t cipher_len = frm_len - cipher_offset;

                        lorawan_cipher_payload(key_to_use, rx_dev_addr, 0x00, (uint32_t)fcnt,
                                               &payload[9 + cipher_offset], cipher_len, &payload[9 + cipher_offset]);

                        ESP_LOGI(TAG, "Partially decrypted FRMPayload (FPort %u, Offset %u, %u bytes): %02X | %02X %02X %02X %02X",
                                 fport, cipher_offset, (unsigned)cipher_len,
                                 payload[9], payload[10], payload[11], payload[12], payload[13]);
                    }
                } else { /* Full (Standard LoRaWAN) */
                    const uint8_t *key_to_use = (fport == 0) ? matched->nwk_skey : matched->app_skey;
                    
                    lorawan_cipher_payload(key_to_use, rx_dev_addr, 0x00, (uint32_t)fcnt,
                                           &payload[9], frm_len, &payload[9]);

                    ESP_LOGI(TAG, "Decrypted FRMPayload (FPort %u, %u bytes): %02X %02X %02X %02X %02X",
                             fport, (unsigned)frm_len,
                             payload[9], payload[10], payload[11], payload[12], payload[13]);
                }
            }
        }

        /* Generic JSON-driven reaction evaluation */
        if (matched && matched->reaction.valid && fport == matched->reaction.trigger_fport) {
            if (size >= 10 && payload[9] == matched->reaction.trigger_prefix) {
                ESP_LOGI(TAG, "REACTION MATCH: Device '%s' triggered FPort %u (Prefix: 0x%02X)",
                         matched->device_name, fport, matched->reaction.trigger_prefix);

                /* 1. Calculate random CLAIM delay between 30 ms and 700 ms */
                uint32_t claim_delay = 30 + (Radio.Random() % 670);
                uint32_t claim_ts = now_ms + claim_delay;

                /* 2. Enqueue P2P CLAIM broadcast */
                lora_model_enqueue_claim(rx_dev_addr, claim_ts);

                /* 3. Enqueue LoRaWAN Downlink with FCntDown = FCntUp from Sensor */
                uint32_t rx1_ts = now_ms + 980;
                lora_model_enqueue_downlink(rx_dev_addr, matched->reaction.target_fport, (uint32_t)fcnt,
                                            matched->reaction.payload_bytes, matched->reaction.payload_len,
                                            rx1_ts, matched->app_skey, matched->nwk_skey);
            }
        }

        /* Enqueue P2P Mesh Retransmit broadcast or Standalone Time Broadcast */
        if (g_rf_cfg.mesh.enabled && matched && frm_len > 0) {
            if (g_rf_cfg.mesh.broadcast_ttl > 0) {
                uint32_t retransmit_delay = 1300 + (Radio.Random() % 3700);
                uint32_t retransmit_ts = now_ms + retransmit_delay;
                lora_model_enqueue_retransmit(rx_dev_addr, fcnt, fport, g_rf_cfg.mesh.broadcast_ttl, &payload[9], frm_len, retransmit_ts);
            } else if (g_rf_cfg.time_cfg.p2p_sync_enabled) {
                /* If Retransmit is disabled (TTL=0), send a standalone time broadcast if we have a valid time */
                uint32_t current_utc = (uint32_t)time(NULL);
                if (current_utc >= 1700000000) {
                    lora_model_enqueue_time_broadcast(current_utc);
                }
            }
        }

        /* Forward Decrypted Frame to RP2040 via UART (Cmd 0x36) */
        int ret = rp2040_send_lora_raw_frame(rx_dev_addr, rssi, snr, payload, (uint8_t)size);
        if (ret > 0) {
            ESP_LOGI(TAG, "Forwarded decrypted frame (DevAddr: 0x%08lX, %u bytes) to RP2040",
                     (unsigned long)rx_dev_addr, size);
        } else {
            ESP_LOGE(TAG, "Failed to send frame to RP2040!");
        }
    }

    Radio.RxBoosted(0);
}

static void OnRxTimeout(void) {
    ESP_LOGI(TAG, "SX1262 RX Timeout event");
    Radio.RxBoosted(0);
}

static void OnRxError(void) {
    ESP_LOGE(TAG, "SX1262 RX Error / CRC Mismatch event!");
    Radio.RxBoosted(0);
}

static void OnTxDone(void) {
    ESP_LOGI(TAG, "LoRa Packet TX completed successfully.");
    g_tx_in_progress = false;
    lora_model_restore_rx();
}

static void OnTxTimeout(void) {
    ESP_LOGE(TAG, "LoRa Packet TX Timeout!");
    g_tx_in_progress = false;
    lora_model_restore_rx();
}

static void OnCadDone(bool channelActivityDetected) {
    if (channelActivityDetected) {
        ESP_LOGW(TAG, "CAD detected active channel! Deferring P2P transmission.");
        /* Defer pending P2P frame by 50 ms */
        for (size_t i = 0; i < MAX_TX_QUEUE_SIZE; i++) {
            if (g_tx_queue[i].valid && g_tx_queue[i].msg_type != TX_TYPE_LORAWAN_DOWNLINK) {
                g_tx_queue[i].target_timestamp_ms += 50;
            }
        }
        g_tx_in_progress = false;
        Radio.RxBoosted(0);
    } else {
        ESP_LOGI(TAG, "CAD Clear! Channel free for transmission.");
    }
}

/* ── Public API Implementation ─────────────────────────────────────────── */

void lora_model_on_rp2040_cfg_received(const uint8_t *buf, size_t len) {
    if (len < 2) return;

    uint8_t valid_cfg = buf[1];

    if (!valid_cfg) {
        ESP_LOGW(TAG, "RP2040 reported NO valid LoRa config available on MicroSD.");
        g_radio_active = false;
        return;
    }

    size_t expected_min = 2 + sizeof(lora_global_config_t) + 1;
    if (len < expected_min) {
        ESP_LOGE(TAG, "Truncated UART config packet (Got %u bytes, expected at least %u)", len, expected_min);
        return;
    }

    size_t idx = 2;
    memcpy(&g_rf_cfg, &buf[idx], sizeof(lora_global_config_t));
    idx += sizeof(lora_global_config_t);

    g_device_count = buf[idx++];
    if (g_device_count > MAX_LORA_DEVICES) g_device_count = MAX_LORA_DEVICES;

    memset(g_devices, 0, sizeof(g_devices));
    for (uint8_t i = 0; i < g_device_count; i++) {
        if (idx + sizeof(lora_device_config_t) > len) break;
        memcpy(&g_devices[i], &buf[idx], sizeof(lora_device_config_t));
        idx += sizeof(lora_device_config_t);
        
        ESP_LOGI(TAG, "Loaded Device [%u]: Name='%s', DevAddr=0x%08lX (Reaction: %s)",
                 i, g_devices[i].device_name, (unsigned long)g_devices[i].dev_addr,
                 g_devices[i].reaction.valid ? "ACTIVE" : "NONE");
    }

    lora_model_apply_rf_config();

    /* Apply POSIX Timezone received from SD card globals.json */
    if (strlen(g_rf_cfg.time_cfg.posix_timezone) > 0) {
        setenv("TZ", g_rf_cfg.time_cfg.posix_timezone, 1);
        tzset();
        ESP_LOGI(TAG, "Applied POSIX Timezone from SD: %s", g_rf_cfg.time_cfg.posix_timezone);
    }

    /* Start SNTP if Wi-Fi is connected */
    sntp_service_start(g_rf_cfg.time_cfg.ntp_server);
}

void lora_model_update(void) {
    if (g_radio_active) {
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        /* Direct SX1262 Hardware IRQ Register Inspection (Raw RF Diagnostic) */
        uint16_t irq = SX126xGetIrqStatus();
        if (irq != 0) {
            ESP_LOGI(TAG, "RAW SX1262 IRQ: 0x%04X | Preamble:%d | SyncOk:%d | HdrOk:%d | HdrErr:%d | CrcErr:%d | RxDone:%d",
                     irq,
                     (irq & 0x0004) ? 1 : 0,  /* Bit 2: Preamble Detected */
                     (irq & 0x0008) ? 1 : 0,  /* Bit 3: SyncWord Valid */
                     (irq & 0x0010) ? 1 : 0,  /* Bit 4: Header Valid */
                     (irq & 0x0020) ? 1 : 0,  /* Bit 5: Header Error */
                     (irq & 0x0040) ? 1 : 0,  /* Bit 6: CRC Error */
                     (irq & 0x0002) ? 1 : 0   /* Bit 1: RX Done */
            );

            /* Clear hardware IRQ flags to capture next RF event */
            SX126xClearIrqStatus(0xFFFF);

            /* Trigger driver event processing if RxDone or Error fired */
            if (irq & (0x0002 | 0x0020 | 0x0040)) {
                extern bool IrqFired;
                IrqFired = true;
                Radio.IrqProcess();
            }
        }

        /* Process TX Queue if no transmission is active */
        if (!g_tx_in_progress) {
            for (size_t i = 0; i < MAX_TX_QUEUE_SIZE; i++) {
                if (g_tx_queue[i].valid && now_ms >= g_tx_queue[i].target_timestamp_ms) {
                    lora_tx_frame_t *frame = &g_tx_queue[i];

                    ESP_LOGI(TAG, "Transmitting Frame from TX Queue! Type: %d, DevAddr: 0x%08lX, IQ Inverted: %d",
                             frame->msg_type, (unsigned long)frame->dev_addr, frame->iq_inverted);

                    g_tx_in_progress = true;

                    /* 1. Configure TX parameters */
                    uint8_t bw_idx = 0;
                    if (g_rf_cfg.rf_bandwidth_khz >= 500) bw_idx = 2;
                    else if (g_rf_cfg.rf_bandwidth_khz >= 250) bw_idx = 1;

                    uint8_t cr = g_rf_cfg.rf_coding_rate;
                    if (cr > 4) cr = cr - 4;

                    Radio.SetTxConfig(
                        MODEM_LORA,
                        frame->tx_power_dbm,
                        0,                        /* fdev */
                        bw_idx,
                        g_rf_cfg.rf_spreading_factor,
                        cr,
                        g_rf_cfg.rf_preamble_len,
                        false,                    /* fixLen */
                        true,                     /* crcOn */
                        0, 0,
                        frame->iq_inverted,       /* Downlinks use inverted IQ, P2P uses normal */
                        3000                      /* Timeout ms */
                    );

                    /* 2. Dispatch payload via SX1262 driver */
                    Radio.Send(frame->raw_payload, frame->payload_len);

                    /* Mark frame handled */
                    frame->valid = false;
                    break;
                }
            }
        }

        /* Heartbeat / Noise Floor Logging & UI Event */
        static uint32_t last_rssi_check = 0;
        if (now_ms - last_rssi_check >= 3000) {
            last_rssi_check = now_ms;
            s_last_noise_floor = Radio.Rssi(MODEM_LORA);
            //ESP_LOGI(TAG, "SX1262 Heartbeat: Active Listening... (Noise Floor RSSI: %d dBm)", s_last_noise_floor);

            struct view_data_lora_diag diag_evt = {
                .rssi = 0,
                .snr = 0,
                .noise_floor = s_last_noise_floor,
                .dev_addr = 0,
                .dev_name = ""
            };
            esp_event_post_to(view_event_handle, VIEW_EVENT_BASE, VIEW_EVENT_LORA_DIAG_DATA, &diag_evt, sizeof(diag_evt), portMAX_DELAY);
        }
    }
}