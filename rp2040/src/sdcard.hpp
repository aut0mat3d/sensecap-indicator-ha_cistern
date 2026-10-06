/**
 * @file sdcard.hpp
 * @brief MicroSD Storage Engine, MBR Partitioning & Non-blocking State Machines.
 * @copyright © 2024-2026, Seeed Studio / SenseCAP Cistern Extension
 */

#ifndef SDCARD_HPP
#define SDCARD_HPP

#include "indicator_rp2040.hpp"
#include <SdFat.h>
#include <Adafruit_TinyUSB.h>

/* ── Public API ────────────────────────────────────────────────────────── */

/**
 * @brief Initializes SPI1 bus, GPIO 7 Card-Detect Interrupt, and SdFat driver.
 */
void sd_card_init(void);

/**
 * @brief Returns true if an SD card hardware event or MSC status change is pending.
 */
bool sd_card_has_event_pending(void);

void sd_card_clear_event_pending(void);

bool sd_card_needs_config_reload(void);

void sd_card_clear_config_reload(void);

void sd_card_trigger_status_push(void);

/**
 * @brief Provides reference to the initialized SdFat32 filesystem instance.
 */
SdFat32& sd_card_get_fs(void);

/**
 * @brief Non-blocking worker task to be called in loop1().
 * Handles Card-Detect ISR pushes and non-blocking format state machine steps.
 */
void sd_card_update(PacketSerial &packetSerial);

/**
 * @brief Runtime check for physical SD presence, FAT32 volume label, and MBR Partition 2 integrity.
 */
void sd_card_get_info(uint8_t &is_present, uint8_t &is_usb_mounted, int8_t &state, uint32_t &capacity_mb, char* volume_label);

/**
 * @brief Triggers the non-blocking format state machine for Dual-Partition setup.
 */
bool sd_card_format_start(uint8_t format_type, const char* new_label);

/**
 * @brief Toggles TinyUSB Mass Storage Class (MSC) exposing Partition 1 to PC.
 */
void sd_card_msc_toggle(bool enable);

/**
 * @brief Sets the expected target Volume Label received from ESP32 eFuse MAC.
 */
void sd_card_set_expected_label(const char* label);

/**
 * @brief Returns true if USB MSC mode is currently active.
 */
bool sd_card_is_msc_active(void);

bool save_time_config_to_sd(SdFat32 &sd, const char* posix_tz, const char* format_str);

#endif /* SDCARD_HPP */