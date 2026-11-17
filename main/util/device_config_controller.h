/**
 * @file device_config_controller.h
 * @brief Generic, schema-driven configuration controller connector for device thresholds
 *        and runtime parameter management.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef DEVICE_CONFIG_CONTROLLER_H
#define DEVICE_CONFIG_CONTROLLER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback invoked when a device threshold is updated and applied at runtime.
 *        Allows any active view or widget to update graphical threshold markers.
 *
 * @param dev_addr Target device address.
 * @param key Channel identifier (e.g. "depth", "flowrate", "temp").
 * @param op Comparison operator (0 = < / min, 1 = > / max).
 * @param value New physical threshold value in channel engineering units.
 * @param hyst New physical hysteresis value in channel engineering units.
 */
typedef void (*device_config_threshold_update_cb_t)(uint32_t dev_addr, const char *key, uint8_t op, float value, float hyst);

/**
 * @brief Register a listener callback for runtime threshold updates.
 *
 * @param cb Callback function pointer (pass NULL to unregister).
 */
void device_config_controller_register_threshold_cb(device_config_threshold_update_cb_t cb);

/**
 * @brief Open the interactive threshold configuration view for any registered device.
 *
 * @param dev_addr Target device address (e.g., 0x05975DC9).
 * @return 0 on success, negative error code on failure.
 */
int device_config_controller_open_thresholds(uint32_t dev_addr);

/* Backward compatibility alias */
#define cistern_threshold_cfg_open(dev_addr) device_config_controller_open_thresholds(dev_addr)

#ifdef __cplusplus
}
#endif

#endif /* DEVICE_CONFIG_CONTROLLER_H */