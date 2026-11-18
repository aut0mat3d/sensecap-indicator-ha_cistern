/**
 * @file mqtt_bridge.h
 * @brief MQTT publisher with Home Assistant MQTT discovery.
 *
 * - Broker settings: /config/mqtt.json on the SD card (no NVS). Without the file, or with
 *   "enabled": false, no connection is made.
 * - What is published is chosen per device in its device JSON ("mqtt" block, parsed by the
 *   RP2040 and streamed as packet 0x46): only listed keys, optionally their alarm state, and the
 *   reception quality ("rssi" / "snr") of the device at this SenseCAP.
 * - Identity: this SenseCAP is "sensecap_<last 3 MAC bytes>"; LoRa sensors are identified by
 *   dev_eui (dev_addr if no dev_eui is configured), so a sensor stays the same HA device no
 *   matter which SenseCAP forwards it. Onboard / Grove sensors (dev_addr <= 0x7F) belong to the
 *   SenseCAP device.
 * - Discovery: one retained config per entity under <discovery_prefix>/sensor/<node_id>/...;
 *   republished on HA birth ("online" on <discovery_prefix>/status) and after every config
 *   reload; configs of this node that are no longer selected are removed.
 *
 * Topics and payloads: docs_extensions_n_changes/overview/OVERVIEW_DATAFLOW.md §7.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#ifndef MQTT_BRIDGE_H
#define MQTT_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_BRIDGE_CONFIG_PATH   "/config/mqtt.json"

/**
 * @brief Create the bridge task and register the event handlers. The connection is made once
 *        the configuration has been read from the SD card and the network is up.
 */
int mqtt_bridge_init(void);

/**
 * @brief The RP2040 (re)loads its configuration (packet 0x35): drop the MQTT key selection,
 *        re-read /config/mqtt.json once the device stream is complete.
 *        Called from the UART receive task; does not block.
 */
void mqtt_bridge_on_rp2040_cfg_reload(void);

/**
 * @brief MQTT key selection of one device (packet 0x46). Called from the UART receive task;
 *        does not block.
 */
void mqtt_bridge_on_rp2040_mqtt_cfg(const uint8_t *buf, size_t len);

/** Print configuration (without password), connection state and entity counters to the console */
void mqtt_bridge_print_status(void);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_BRIDGE_H */
