/**
 * @file units.cpp
 * @brief Unit conversion utilities implementation for RP2040
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "units.h"
#include "sdcard.hpp"
#include <ArduinoJson.h>
#include <SdFat.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

// ============================================================================
// Built-in units database (complete fallback including flow_rate & speed)
// ============================================================================

static const char* s_builtin_units_json = R"({
  "quantity_types": {
    "length": {
      "si_base": "m",
      "units": {
        "mm": { "factor": 0.001, "symbol": "mm" },
        "cm": { "factor": 0.01, "symbol": "cm" },
        "m":  { "factor": 1.0, "symbol": "m" },
        "km": { "factor": 1000.0, "symbol": "km" },
        "inch": { "factor": 0.0254, "symbol": "in" },
        "ft": { "factor": 0.3048, "symbol": "ft" }
      }
    },
    "volume": {
      "si_base": "m3",
      "units": {
        "ml": { "factor": 0.000001, "symbol": "mL" },
        "liter": { "factor": 0.001, "symbol": "L" },
        "m3": { "factor": 1.0, "symbol": "m³" },
        "gallon_us": { "factor": 0.00378541, "symbol": "gal" }
      }
    },
    "flow_rate": {
      "si_base": "m3/s",
      "units": {
        "m3/s": { "factor": 1.0, "symbol": "m³/s" },
        "m3/h": { "factor": 0.000277778, "symbol": "m³/h" },
        "L/min": { "factor": 1.66667e-5, "symbol": "L/min" },
        "L/s": { "factor": 0.001, "symbol": "L/s" },
        "L/h": { "factor": 2.77778e-7, "symbol": "L/h" },
        "gpm": { "factor": 6.30902e-5, "symbol": "gpm" }
      }
    },
    "speed": {
      "si_base": "m/s",
      "units": {
        "m/s": { "factor": 1.0, "symbol": "m/s" },
        "km/h": { "factor": 0.277778, "symbol": "km/h" },
        "cm/h": { "factor": 2.77778e-6, "symbol": "cm/h" },
        "mm/h": { "factor": 2.77778e-7, "symbol": "mm/h" }
      }
    },
    "temperature": {
      "si_base": "K",
      "units": {
        "C": { "factor": 1.0, "offset": 0, "symbol": "°C" },
        "K": { "factor": 1.0, "offset": 0, "symbol": "K" },
        "F": { "factor": 0.555556, "offset": -459.67, "symbol": "°F" }
      }
    },
    "pressure": {
      "si_base": "Pa",
      "units": {
        "Pa": { "factor": 1.0, "symbol": "Pa" },
        "hPa": { "factor": 100.0, "symbol": "hPa" },
        "bar": { "factor": 100000.0, "symbol": "bar" },
        "psi": { "factor": 6894.76, "symbol": "psi" }
      }
    },
    "time": {
      "si_base": "s",
      "units": {
        "s": { "factor": 1.0, "symbol": "s" },
        "min": { "factor": 60.0, "symbol": "min" },
        "h": { "factor": 3600.0, "symbol": "h" }
      }
    }
  }
})";

static DynamicJsonDocument* s_units_doc = nullptr;
static bool s_units_initialized = false;

// ============================================================================
// Helper Functions & Flexible Matching
// ============================================================================

static DynamicJsonDocument* _get_units_doc(void) {
    if (!s_units_initialized) {
        s_units_doc = new DynamicJsonDocument(16384);
        DeserializationError error = deserializeJson(*s_units_doc, s_builtin_units_json);
        if (error) {
            Serial.println("[UNITS] Error parsing builtin units database");
            delete s_units_doc;
            s_units_doc = nullptr;
            return nullptr;
        }
        s_units_initialized = true;
    }
    return s_units_doc;
}

static bool _is_units_initialized(void) {
    return _get_units_doc() != nullptr;
}

static JsonObject _get_quantity_types(void) {
    DynamicJsonDocument* doc = _get_units_doc();
    return (*doc)["quantity_types"];
}

static bool _unit_matches(const char* key, JsonObject unit_obj, const char* target) {
    if (!key || !target) return false;
    if (strcmp(key, target) == 0 || strcasecmp(key, target) == 0) return true;

    if (unit_obj.containsKey("symbol")) {
        const char* sym = unit_obj["symbol"];
        if (sym && (strcmp(sym, target) == 0 || strcasecmp(sym, target) == 0)) return true;
    }

    // Common aliases across UI and JSON configs
    if ((strcmp(target, "m³") == 0 || strcmp(target, "m3") == 0) && strcmp(key, "m3") == 0) return true;
    if ((strcmp(target, "m²") == 0 || strcmp(target, "m2") == 0) && strcmp(key, "m2") == 0) return true;
    if ((strcmp(target, "L") == 0 || strcmp(target, "l") == 0 || strcasecmp(target, "liters") == 0) && strcmp(key, "liter") == 0) return true;
    if ((strcmp(target, "m³/h") == 0 || strcmp(target, "m3/h") == 0) && strcmp(key, "m3/h") == 0) return true;
    if ((strcmp(target, "°C") == 0 || strcasecmp(target, "C") == 0) && strcmp(key, "C") == 0) return true;
    if ((strcmp(target, "°F") == 0 || strcasecmp(target, "F") == 0) && strcmp(key, "F") == 0) return true;
    if (strcasecmp(target, "gal") == 0 && strcmp(key, "gallon_us") == 0) return true;

    return false;
}

static JsonObject _find_unit_entry(const char* unit, char* out_qty_type) {
    if (!unit || !_is_units_initialized()) return JsonObject();
    JsonObject qty_types = _get_quantity_types();

    for (JsonPair pair : qty_types) {
        JsonObject qty_obj = pair.value().as<JsonObject>();
        JsonObject units_obj = qty_obj["units"];
        for (JsonPair u_pair : units_obj) {
            JsonObject u_info = u_pair.value().as<JsonObject>();
            if (_unit_matches(u_pair.key().c_str(), u_info, unit)) {
                if (out_qty_type) {
                    strncpy(out_qty_type, pair.key().c_str(), 31);
                    out_qty_type[31] = '\0';
                }
                return u_info;
            }
        }
    }
    return JsonObject();
}

// ============================================================================
// Public API
// ============================================================================

bool units_init(const char* units_json_path) {
    if (units_json_path && strlen(units_json_path) > 0) {
        SdFat32& sd = sd_card_get_fs();
        if (sd.exists(units_json_path)) {
            File32 file = sd.open(units_json_path, O_READ);
            if (file) {
                if (s_units_doc) delete s_units_doc;
                s_units_doc = new DynamicJsonDocument(16384);
                DeserializationError err = deserializeJson(*s_units_doc, file);
                file.close();
                if (!err) {
                    s_units_initialized = true;
                    Serial.printf("[UNITS] Loaded %s from MicroSD.\r\n", units_json_path);
                    return true;
                }
                Serial.printf("[UNITS] JSON Error in %s (%s). Using fallback.\r\n", units_json_path, err.c_str());
                delete s_units_doc;
                s_units_doc = nullptr;
            }
        }
    }
    return _is_units_initialized();
}

void units_shutdown(void) {
    if (s_units_doc) {
        delete s_units_doc;
        s_units_doc = nullptr;
    }
    s_units_initialized = false;
}

const char* units_get_quantity_type(const char* unit) {
    static char qty_type_buf[32];
    JsonObject entry = _find_unit_entry(unit, qty_type_buf);
    return entry.isNull() ? nullptr : qty_type_buf;
}

bool units_is_compatible(const char* quantity_type, const char* unit) {
    const char* detected_type = units_get_quantity_type(unit);
    return (detected_type && quantity_type && strcmp(detected_type, quantity_type) == 0);
}

float units_get_factor(const char* unit) {
    JsonObject entry = _find_unit_entry(unit, nullptr);
    if (!entry.isNull() && entry.containsKey("factor")) {
        return entry["factor"].as<float>();
    }
    return 1.0f;
}

float units_get_offset(const char* unit) {
    JsonObject entry = _find_unit_entry(unit, nullptr);
    if (!entry.isNull() && entry.containsKey("offset")) {
        return entry["offset"].as<float>();
    }
    return 0.0f;
}

float units_convert(float value, const char* from_unit, const char* to_unit) {
    if (!from_unit || !to_unit || strcmp(from_unit, to_unit) == 0) {
        return value;
    }

    char from_type[32] = {0};
    char to_type[32] = {0};
    JsonObject from_entry = _find_unit_entry(from_unit, from_type);
    JsonObject to_entry = _find_unit_entry(to_unit, to_type);

    if (from_entry.isNull() || to_entry.isNull() || strcmp(from_type, to_type) != 0) {
        return value; // Incompatible or unknown units
    }

    // Temperature with offset handling
    if (strcmp(from_type, "temperature") == 0) {
        float from_factor = from_entry["factor"] | 1.0f;
        float from_offset = from_entry["offset"] | 0.0f;
        float to_factor   = to_entry["factor"] | 1.0f;
        float to_offset   = to_entry["offset"] | 0.0f;

        float kelvin = value * from_factor + from_offset;
        return (kelvin - to_offset) / to_factor;
    }

    float from_factor = from_entry["factor"] | 1.0f;
    float to_factor   = to_entry["factor"] | 1.0f;
    if (to_factor == 0.0f) return value;

    return value * (from_factor / to_factor);
}

bool units_exists(const char* unit) {
    return units_get_quantity_type(unit) != nullptr;
}