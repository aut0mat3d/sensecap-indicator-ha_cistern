/**
 * @file units.c
 * @brief Unit conversion utilities implementation for ESP32
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "units.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char* TAG = "units";

// ============================================================================
// Built-in units database (fallback)
// ============================================================================

static const char* s_builtin_units_json =
    "{"
    "  \"quantity_types\": {"
    "    \"length\": {"
    "      \"description\": \"Linear dimensions\","
    "      \"si_base\": \"m\","
    "      \"units\": {"
    "        \"mm\": { \"factor\": 0.001, \"symbol\": \"mm\", \"name\": \"Millimeter\" },"
    "        \"cm\": { \"factor\": 0.01, \"symbol\": \"cm\", \"name\": \"Centimeter\" },"
    "        \"m\":  { \"factor\": 1.0, \"symbol\": \"m\", \"name\": \"Meter\" },"
    "        \"km\": { \"factor\": 1000.0, \"symbol\": \"km\", \"name\": \"Kilometer\" },"
    "        \"inch\": { \"factor\": 0.0254, \"symbol\": \"in\", \"name\": \"Inch\" },"
    "        \"ft\": { \"factor\": 0.3048, \"symbol\": \"ft\", \"name\": \"Foot\" },"
    "        \"yd\": { \"factor\": 0.9144, \"symbol\": \"yd\", \"name\": \"Yard\" },"
    "        \"mile\": { \"factor\": 1609.34, \"symbol\": \"mi\", \"name\": \"Mile\" }"
    "      }"
    "    },"
    "    \"volume\": {"
    "      \"description\": \"Volume\","
    "      \"si_base\": \"m3\","
    "      \"units\": {"
    "        \"ml\": { \"factor\": 0.000001, \"symbol\": \"mL\", \"name\": \"Milliliter\" },"
    "        \"liter\": { \"factor\": 0.001, \"symbol\": \"L\", \"name\": \"Liter\" },"
    "        \"m3\": { \"factor\": 1.0, \"symbol\": \"m³\", \"name\": \"Cubic Meter\" },"
    "        \"gallon_us\": { \"factor\": 0.00378541, \"symbol\": \"gal\", \"name\": \"US Gallon\" },"
    "        \"gallon_uk\": { \"factor\": 0.00454609, \"symbol\": \"gal\", \"name\": \"UK Gallon\" },"
    "        \"barrel\": { \"factor\": 0.158987, \"symbol\": \"bbl\", \"name\": \"Oil Barrel\" }"
    "      }"
    "    },"
    "    \"area\": {"
    "      \"description\": \"Area\","
    "      \"si_base\": \"m2\","
    "      \"units\": {"
    "        \"mm2\": { \"factor\": 0.000001, \"symbol\": \"mm²\", \"name\": \"Square Millimeter\" },"
    "        \"cm2\": { \"factor\": 0.0001, \"symbol\": \"cm²\", \"name\": \"Square Centimeter\" },"
    "        \"m2\": { \"factor\": 1.0, \"symbol\": \"m²\", \"name\": \"Square Meter\" },"
    "        \"km2\": { \"factor\": 1000000.0, \"symbol\": \"km²\", \"name\": \"Square Kilometer\" },"
    "        \"ha\": { \"factor\": 10000.0, \"symbol\": \"ha\", \"name\": \"Hectare\" },"
    "        \"acre\": { \"factor\": 4046.86, \"symbol\": \"acre\", \"name\": \"Acre\" },"
    "        \"ft2\": { \"factor\": 0.092903, \"symbol\": \"ft²\", \"name\": \"Square Foot\" },"
    "        \"in2\": { \"factor\": 0.00064516, \"symbol\": \"in²\", \"name\": \"Square Inch\" }"
    "      }"
    "    },"
    "    \"temperature\": {"
    "      \"description\": \"Temperature\","
    "      \"si_base\": \"K\","
    "      \"units\": {"
    "        \"C\": { \"factor\": 1.0, \"offset\": 0, \"symbol\": \"°C\", \"name\": \"Celsius\" },"
    "        \"K\": { \"factor\": 1.0, \"offset\": 0, \"symbol\": \"K\", \"name\": \"Kelvin\" },"
    "        \"F\": { \"factor\": 0.555556, \"offset\": -459.67, \"symbol\": \"°F\", \"name\": \"Fahrenheit\" }"
    "      }"
    "    },"
    "    \"pressure\": {"
    "      \"description\": \"Pressure\","
    "      \"si_base\": \"Pa\","
    "      \"units\": {"
    "        \"Pa\": { \"factor\": 1.0, \"symbol\": \"Pa\", \"name\": \"Pascal\" },"
    "        \"hPa\": { \"factor\": 100.0, \"symbol\": \"hPa\", \"name\": \"Hectopascal\" },"
    "        \"kPa\": { \"factor\": 1000.0, \"symbol\": \"kPa\", \"name\": \"Kilopascal\" },"
    "        \"bar\": { \"factor\": 100000.0, \"symbol\": \"bar\", \"name\": \"Bar\" },"
    "        \"psi\": { \"factor\": 6894.76, \"symbol\": \"psi\", \"name\": \"PSI\" },"
    "        \"mmHg\": { \"factor\": 133.322, \"symbol\": \"mmHg\", \"name\": \"mm Mercury\" },"
    "        \"inHg\": { \"factor\": 3386.39, \"symbol\": \"\\\"Hg\", \"name\": \"Inch Mercury\" }"
    "      }"
    "    },"
    "    \"solar_radiation\": {"
    "      \"description\": \"Solar and light\","
    "      \"si_base\": \"W/m2\","
    "      \"units\": {"
    "        \"W/m2\": { \"factor\": 1.0, \"symbol\": \"W/m²\", \"name\": \"Watts per Square Meter\" },"
    "        \"lux\": { \"factor\": 0.0079, \"symbol\": \"lx\", \"name\": \"Lux\", \"note\": \"Approximate\" },"
    "        \"UV_index\": { \"factor\": 1.0, \"symbol\": \"\", \"name\": \"UV Index\", \"type\": \"dimensionless\" },"
    "        \"fc\": { \"factor\": 10.7639, \"symbol\": \"fc\", \"name\": \"Foot-candles\" }"
    "      }"
    "    },"
    "    \"force\": {"
    "      \"description\": \"Force\","
    "      \"si_base\": \"N\","
    "      \"units\": {"
    "        \"N\": { \"factor\": 1.0, \"symbol\": \"N\", \"name\": \"Newton\" },"
    "        \"kN\": { \"factor\": 1000.0, \"symbol\": \"kN\", \"name\": \"Kilonewton\" },"
    "        \"lbf\": { \"factor\": 4.44822, \"symbol\": \"lbf\", \"name\": \"Pound-force\" },"
    "        \"kgf\": { \"factor\": 9.80665, \"symbol\": \"kgf\", \"name\": \"Kilogram-force\" }"
    "      }"
    "    },"
    "    \"flow_rate\": {"
    "      \"description\": \"Flow rate\","
    "      \"si_base\": \"m3/s\","
    "      \"units\": {"
    "        \"m3/s\": { \"factor\": 1.0, \"symbol\": \"m³/s\", \"name\": \"Cubic Meters per Second\" },"
    "        \"m3/h\": { \"factor\": 0.000277778, \"symbol\": \"m³/h\", \"name\": \"Cubic Meters per Hour\" },"
    "        \"L/min\": { \"factor\": 1.66667e-5, \"symbol\": \"L/min\", \"name\": \"Liters per Minute\" },"
    "        \"L/s\": { \"factor\": 0.001, \"symbol\": \"L/s\", \"name\": \"Liters per Second\" },"
    "        \"gpm\": { \"factor\": 6.30902e-5, \"symbol\": \"gpm\", \"name\": \"Gallons per Minute\" }"
    "      }"
    "    },"
    "    \"level\": {"
    "      \"description\": \"Fill level\","
    "      \"si_base\": \"%\","
    "      \"units\": {"
    "        \"%\": { \"factor\": 1.0, \"symbol\": \"%\", \"name\": \"Percent\" },"
    "        \"ratio\": { \"factor\": 1.0, \"symbol\": \"\", \"name\": \"Ratio (0-1)\" }"
    "      }"
    "    },"
    "    \"mass\": {"
    "      \"description\": \"Mass\","
    "      \"si_base\": \"kg\","
    "      \"units\": {"
    "        \"g\": { \"factor\": 0.001, \"symbol\": \"g\", \"name\": \"Gram\" },"
    "        \"kg\": { \"factor\": 1.0, \"symbol\": \"kg\", \"name\": \"Kilogram\" },"
    "        \"t\": { \"factor\": 1000.0, \"symbol\": \"t\", \"name\": \"Tonne\" },"
    "        \"lb\": { \"factor\": 0.453592, \"symbol\": \"lb\", \"name\": \"Pound\" },"
    "        \"oz\": { \"factor\": 0.0283495, \"symbol\": \"oz\", \"name\": \"Ounce\" }"
    "      }"
    "    },"
    "    \"time\": {"
    "      \"description\": \"Time\","
    "      \"si_base\": \"s\","
    "      \"units\": {"
    "        \"ms\": { \"factor\": 0.001, \"symbol\": \"ms\", \"name\": \"Millisecond\" },"
    "        \"s\": { \"factor\": 1.0, \"symbol\": \"s\", \"name\": \"Second\" },"
    "        \"min\": { \"factor\": 60.0, \"symbol\": \"min\", \"name\": \"Minute\" },"
    "        \"h\": { \"factor\": 3600.0, \"symbol\": \"h\", \"name\": \"Hour\" },"
    "        \"d\": { \"factor\": 86400.0, \"symbol\": \"d\", \"name\": \"Day\" }"
    "      }"
    "    }"
    "  }"
    "}";

// ============================================================================
// Static State
// ============================================================================

static cJSON* s_units_root = NULL;
static bool s_units_initialized = false;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Get quantity_types object from JSON
 */
static cJSON* _get_units_root(void) {
    if (!s_units_initialized) {
        // Parse builtin JSON
        s_units_root = cJSON_Parse(s_builtin_units_json);
        if (!s_units_root) {
            ESP_LOGE(TAG, "Failed to parse builtin units.json");
            return NULL;
        }
        s_units_initialized = true;
    }
    return s_units_root;
}

/**
 * @brief Get quantity_types object
 */
static cJSON* _get_quantity_types(void) {
    cJSON* root = _get_units_root();
    if (!root) return NULL;
    return cJSON_GetObjectItemCaseSensitive(root, "quantity_types");
}

/**
 * @brief Get unit object from quantity type
 */
static cJSON* _get_unit_object(const char* quantity_type, const char* unit) {
    cJSON* qty_types = _get_quantity_types();
    if (!qty_types) return NULL;
    
    cJSON* qty_type = cJSON_GetObjectItemCaseSensitive(qty_types, quantity_type);
    if (!qty_type) return NULL;
    
    cJSON* units = cJSON_GetObjectItemCaseSensitive(qty_type, "units");
    if (!units) return NULL;
    
    return cJSON_GetObjectItemCaseSensitive(units, unit);
}

// ============================================================================
// Public API
// ============================================================================

bool units_init(const char* units_json_path) {
    // For now, use builtin. File loading from SD can be added later.
    s_units_root = cJSON_Parse(s_builtin_units_json);
    if (!s_units_root) {
        ESP_LOGE(TAG, "Failed to initialize units system");
        return false;
    }
    s_units_initialized = true;
    ESP_LOGI(TAG, "Units system initialized with builtin database");
    return true;
}

void units_shutdown(void) {
    if (s_units_root) {
        cJSON_Delete(s_units_root);
        s_units_root = NULL;
    }
    s_units_initialized = false;
    ESP_LOGI(TAG, "Units system shutdown");
}

bool units_is_compatible(const char* quantity_type, const char* unit) {
    if (!quantity_type || !unit) return false;
    
    cJSON* qty_types = _get_quantity_types();
    if (!qty_types) return false;
    
    cJSON* qty_type = cJSON_GetObjectItemCaseSensitive(qty_types, quantity_type);
    if (!qty_type) return false;
    
    cJSON* units = cJSON_GetObjectItemCaseSensitive(qty_type, "units");
    if (!units) return false;
    
    return cJSON_GetObjectItemCaseSensitive(units, unit) != NULL;
}

const char* units_get_quantity_type(const char* unit) {
    if (!unit) return NULL;
    
    cJSON* qty_types = _get_quantity_types();
    if (!qty_types) return NULL;
    
    // Iterate through all quantity types
    cJSON* qty_type = NULL;
    cJSON_ArrayForEach(qty_type, qty_types) {
        cJSON* units = cJSON_GetObjectItemCaseSensitive(qty_type, "units");
        if (units && cJSON_GetObjectItemCaseSensitive(units, unit)) {
            return qty_type->string;
        }
    }
    return NULL;
}

float units_convert(float value, const char* from_unit, const char* to_unit) {
    if (!from_unit || !to_unit) return value;
    
    const char* from_type = units_get_quantity_type(from_unit);
    const char* to_type = units_get_quantity_type(to_unit);
    
    // If types don't match or either unit is unknown, return original value
    if (!from_type || !to_type || strcmp(from_type, to_type) != 0) {
        if (from_type && to_type && strcmp(from_type, to_type) != 0) {
            ESP_LOGW(TAG, "Cannot convert from %s (%s) to %s (%s): incompatible quantity types",
                     from_unit, from_type, to_unit, to_type);
        }
        return value; // Keep original value as per your requirement
    }
    
    // Special handling for temperature (with offset)
    if (strcmp(from_type, "temperature") == 0) {
        float from_factor = units_get_factor(from_unit);
        float from_offset = units_get_offset(from_unit);
        float to_factor = units_get_factor(to_unit);
        float to_offset = units_get_offset(to_unit);
        
        // Convert to Kelvin first
        float kelvin = value * from_factor + from_offset;
        // Convert to target unit
        return (kelvin - to_offset) / to_factor;
    }
    
    // Standard conversion: value * (from_factor / to_factor)
    float from_factor = units_get_factor(from_unit);
    float to_factor = units_get_factor(to_unit);
    
    if (to_factor == 0) return value;
    
    return value * (from_factor / to_factor);
}

float units_get_factor(const char* unit) {
    if (!unit) return 1.0f;
    
    const char* qty_type = units_get_quantity_type(unit);
    if (!qty_type) return 1.0f;
    
    cJSON* unit_obj = _get_unit_object(qty_type, unit);
    if (!unit_obj) return 1.0f;
    
    cJSON* factor = cJSON_GetObjectItemCaseSensitive(unit_obj, "factor");
    if (factor && cJSON_IsNumber(factor)) {
        return (float)factor->valuedouble;
    }
    return 1.0f;
}

float units_get_offset(const char* unit) {
    if (!unit) return 0.0f;
    
    const char* qty_type = units_get_quantity_type(unit);
    if (!qty_type) return 0.0f;
    
    cJSON* unit_obj = _get_unit_object(qty_type, unit);
    if (!unit_obj) return 0.0f;
    
    cJSON* offset = cJSON_GetObjectItemCaseSensitive(unit_obj, "offset");
    if (offset && cJSON_IsNumber(offset)) {
        return (float)offset->valuedouble;
    }
    return 0.0f;
}

const char* units_get_si_base(const char* quantity_type) {
    if (!quantity_type) return NULL;
    
    cJSON* qty_types = _get_quantity_types();
    if (!qty_types) return NULL;
    
    cJSON* qty_type = cJSON_GetObjectItemCaseSensitive(qty_types, quantity_type);
    if (!qty_type) return NULL;
    
    cJSON* si_base = cJSON_GetObjectItemCaseSensitive(qty_type, "si_base");
    if (si_base && cJSON_IsString(si_base)) {
        return si_base->valuestring;
    }
    return NULL;
}

const char* units_get_symbol(const char* unit) {
    if (!unit) return unit;
    
    const char* qty_type = units_get_quantity_type(unit);
    if (!qty_type) return unit;
    
    cJSON* unit_obj = _get_unit_object(qty_type, unit);
    if (!unit_obj) return unit;
    
    cJSON* symbol = cJSON_GetObjectItemCaseSensitive(unit_obj, "symbol");
    if (symbol && cJSON_IsString(symbol)) {
        return symbol->valuestring;
    }
    return unit;
}

const char* units_get_name(const char* unit) {
    if (!unit) return unit;
    
    const char* qty_type = units_get_quantity_type(unit);
    if (!qty_type) return unit;
    
    cJSON* unit_obj = _get_unit_object(qty_type, unit);
    if (!unit_obj) return unit;
    
    cJSON* name = cJSON_GetObjectItemCaseSensitive(unit_obj, "name");
    if (name && cJSON_IsString(name)) {
        return name->valuestring;
    }
    return unit;
}

bool units_exists(const char* unit) {
    return units_get_quantity_type(unit) != NULL;
}

const char* units_resolve_target(const char* unit, const char* target_unit) {
    if (!unit) return NULL;
    if (!target_unit || strlen(target_unit) == 0) {
        return unit;
    }
    return target_unit;
}