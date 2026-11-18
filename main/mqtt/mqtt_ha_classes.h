/**
 * @file mqtt_ha_classes.h
 * @brief Home Assistant sensor classification from key name and unit (device_class, state_class,
 *        HA unit spelling).
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#ifndef MQTT_HA_CLASSES_H
#define MQTT_HA_CLASSES_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_class;   ///< NULL = none
    const char *state_class;    ///< NULL = none (non-numeric values)
    char        unit[12];       ///< Unit in HA spelling ("" = none)
} mqtt_ha_class_t;

/**
 * @brief Classify a sensor for HA discovery.
 * @param key        Telemetry key (e.g. "SHT41_HUMIDITY", "battery_soc", "volume")
 * @param unit       Unit as sent by the RP2040 (e.g. "m3", "%", "°C")
 * @param is_numeric false for string values (no state_class, no unit)
 */
void mqtt_ha_classify(const char *key, const char *unit, bool is_numeric, mqtt_ha_class_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_HA_CLASSES_H */
