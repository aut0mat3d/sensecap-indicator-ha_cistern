/**
 * @file mqtt_ha_classes.c
 * @brief Home Assistant sensor classification from key name and unit.
 *
 * HA rejects a device_class whose unit it does not know, so a class is only set for units listed
 * here; any other numeric value gets state_class "measurement" without device_class.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */
#include "mqtt_ha_classes.h"
#include <ctype.h>
#include <string.h>

/* Unit spellings of the RP2040 / units.json mapped to the HA spelling */
static const struct {
    const char *from;
    const char *to;
} k_unit_alias[] = {
    {"m3", "m³"}, {"l", "L"}, {"ml", "mL"}, {"l/min", "L/min"}, {"l/h", "L/h"}, {"l/s", "L/s"},
    {"m3/h", "m³/h"}, {"C", "°C"}, {"degC", "°C"}, {"F", "°F"}, {"lux", "lx"}, {"mbar", "mbar"},
};

/* Unit -> device_class (fixed, independent of the key) */
static const struct {
    const char *unit;
    const char *device_class;
    const char *state_class;
} k_unit_class[] = {
    {"°C", "temperature", "measurement"},  {"°F", "temperature", "measurement"},
    {"K", "temperature", "measurement"},   {"lx", "illuminance", "measurement"},
    {"hPa", "pressure", "measurement"},    {"Pa", "pressure", "measurement"},
    {"kPa", "pressure", "measurement"},    {"bar", "pressure", "measurement"},
    {"mbar", "pressure", "measurement"},   {"mm", "distance", "measurement"},
    {"cm", "distance", "measurement"},     {"m", "distance", "measurement"},
    {"km", "distance", "measurement"},     {"L", "volume_storage", "measurement"},
    {"mL", "volume_storage", "measurement"}, {"m³", "volume_storage", "measurement"},
    {"m³/h", "volume_flow_rate", "measurement"}, {"L/min", "volume_flow_rate", "measurement"},
    {"L/h", "volume_flow_rate", "measurement"},  {"L/s", "volume_flow_rate", "measurement"},
    {"V", "voltage", "measurement"},       {"mV", "voltage", "measurement"},
    {"A", "current", "measurement"},       {"mA", "current", "measurement"},
    {"W", "power", "measurement"},         {"kW", "power", "measurement"},
    {"Wh", "energy", "total_increasing"},  {"kWh", "energy", "total_increasing"},
    {"dBm", "signal_strength", "measurement"}, {"dB", "signal_strength", "measurement"},
    {"s", "duration", "measurement"},      {"min", "duration", "measurement"},
    {"h", "duration", "measurement"},
};

static bool key_contains(const char *key, const char *part) {
    char low[32];
    size_t i = 0;
    for (; key[i] && i < sizeof(low) - 1; i++) low[i] = (char)tolower((unsigned char)key[i]);
    low[i] = '\0';
    return strstr(low, part) != NULL;
}

void mqtt_ha_classify(const char *key, const char *unit, bool is_numeric, mqtt_ha_class_t *out) {
    memset(out, 0, sizeof(*out));
    if (!is_numeric) return;

    out->state_class = "measurement";
    if (!unit || unit[0] == '\0') return;

    const char *u = unit;
    for (size_t i = 0; i < sizeof(k_unit_alias) / sizeof(k_unit_alias[0]); i++) {
        if (strcmp(unit, k_unit_alias[i].from) == 0) {
            u = k_unit_alias[i].to;
            break;
        }
    }
    strncpy(out->unit, u, sizeof(out->unit) - 1);

    if (strcmp(u, "%") == 0) {
        if (key_contains(key, "hum")) out->device_class = "humidity";
        else if (key_contains(key, "batt") || key_contains(key, "soc")) out->device_class = "battery";
        else if (key_contains(key, "moist")) out->device_class = "moisture";
        return;
    }
    if (strcmp(u, "ppm") == 0) {
        if (key_contains(key, "co2")) out->device_class = "carbon_dioxide";
        else if (key_contains(key, "voc")) out->device_class = "volatile_organic_compounds_parts";
        return;
    }
    for (size_t i = 0; i < sizeof(k_unit_class) / sizeof(k_unit_class[0]); i++) {
        if (strcmp(u, k_unit_class[i].unit) == 0) {
            out->device_class = k_unit_class[i].device_class;
            out->state_class = k_unit_class[i].state_class;
            return;
        }
    }
}
