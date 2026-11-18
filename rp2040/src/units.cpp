/**
 * @file units.cpp
 * @brief Unit conversion for the RP2040: complete builtin table in flash, optional additions /
 *        corrections from /config/units.json on the SD card.
 *
 * Conversion model (all quantities, including temperature):
 *   base = value * factor + offset          (offset is 0 for everything except temperatures)
 *   result = (base - offset_to) / factor_to
 * Base units: m, m³, m², °C, Pa, W/m², N, m³/s, %, kg, s, m/s, J, W, Hz, rad.
 *
 * Lookup: SD entries first, then the builtin table; exact match of key, symbol or alias first,
 * then a case-insensitive match for names of at least 3 characters (so "mW" never matches "MW").
 * An empty unit never matches anything.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "units.h"
#include "debug_log.hpp"
#include "sdcard.hpp"
#include <ArduinoJson.h>
#include <SdFat.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <math.h>

// ============================================================================
// Builtin table (const, read from flash, no RAM)
// ============================================================================

typedef struct {
    const char* key;
    const char* symbol;
    const char* aliases;   /* '|' separated, may be NULL */
    float       factor;
    float       offset;
} unit_def_t;

typedef struct {
    const char*       name;
    const char*       si_base;
    const unit_def_t* units;
    uint8_t           count;
} quantity_def_t;

#define UNIT_COUNT(a) static_cast<uint8_t>(sizeof(a) / sizeof((a)[0]))

static const unit_def_t k_length[] = {
    {"mm", "mm", nullptr, 0.001f, 0.0f},        {"cm", "cm", nullptr, 0.01f, 0.0f},
    {"m", "m", nullptr, 1.0f, 0.0f},            {"km", "km", nullptr, 1000.0f, 0.0f},
    {"inch", "in", nullptr, 0.0254f, 0.0f},     {"ft", "ft", nullptr, 0.3048f, 0.0f},
    {"yd", "yd", nullptr, 0.9144f, 0.0f},       {"mile", "mi", nullptr, 1609.344f, 0.0f},
};
static const unit_def_t k_volume[] = {
    {"ml", "mL", nullptr, 1e-6f, 0.0f},         {"liter", "L", "l|liters|litre", 0.001f, 0.0f},
    {"m3", "m³", nullptr, 1.0f, 0.0f},          {"gallon_us", "gal", nullptr, 0.003785411784f, 0.0f},
    {"gallon_uk", "gal (UK)", nullptr, 0.00454609f, 0.0f}, {"barrel", "bbl", nullptr, 0.158987295f, 0.0f},
};
static const unit_def_t k_area[] = {
    {"mm2", "mm²", nullptr, 1e-6f, 0.0f},       {"cm2", "cm²", nullptr, 1e-4f, 0.0f},
    {"m2", "m²", nullptr, 1.0f, 0.0f},          {"km2", "km²", nullptr, 1e6f, 0.0f},
    {"ha", "ha", nullptr, 1e4f, 0.0f},          {"acre", "acre", nullptr, 4046.8564f, 0.0f},
    {"ft2", "ft²", nullptr, 0.09290304f, 0.0f}, {"in2", "in²", nullptr, 0.00064516f, 0.0f},
};
static const unit_def_t k_temperature[] = {
    {"C", "°C", "degC", 1.0f, 0.0f},
    {"K", "K", nullptr, 1.0f, -273.15f},
    {"F", "°F", "degF", 5.0f / 9.0f, -160.0f / 9.0f},
};
static const unit_def_t k_pressure[] = {
    {"Pa", "Pa", nullptr, 1.0f, 0.0f},          {"hPa", "hPa", "mbar", 100.0f, 0.0f},
    {"kPa", "kPa", nullptr, 1000.0f, 0.0f},     {"bar", "bar", nullptr, 100000.0f, 0.0f},
    {"psi", "psi", nullptr, 6894.757f, 0.0f},   {"mmHg", "mmHg", nullptr, 133.322f, 0.0f},
    {"inHg", "inHg", "\"Hg", 3386.389f, 0.0f},
};
static const unit_def_t k_solar_radiation[] = {
    {"W/m2", "W/m²", nullptr, 1.0f, 0.0f},
    {"lux", "lx", nullptr, 0.0079f, 0.0f},      /* approximate (sunlight) */
    {"fc", "fc", nullptr, 0.085f, 0.0f},        /* 10.764 lx, approximate */
};
static const unit_def_t k_force[] = {
    {"N", "N", nullptr, 1.0f, 0.0f},            {"kN", "kN", nullptr, 1000.0f, 0.0f},
    {"lbf", "lbf", nullptr, 4.448222f, 0.0f},   {"kgf", "kgf", nullptr, 9.80665f, 0.0f},
};
static const unit_def_t k_flow_rate[] = {
    {"m3/s", "m³/s", nullptr, 1.0f, 0.0f},      {"m3/h", "m³/h", nullptr, 1.0f / 3600.0f, 0.0f},
    {"L/min", "L/min", "l/min", 1.0f / 60000.0f, 0.0f}, {"L/h", "L/h", "l/h", 1.0f / 3600000.0f, 0.0f},
    {"L/s", "L/s", "l/s", 0.001f, 0.0f},        {"gpm", "gpm", nullptr, 6.30902e-5f, 0.0f},
};
static const unit_def_t k_level[] = {
    {"%", "%", nullptr, 1.0f, 0.0f},
    {"ratio", "ratio", nullptr, 100.0f, 0.0f},  /* 1.0 = 100 % */
};
static const unit_def_t k_mass[] = {
    {"g", "g", nullptr, 0.001f, 0.0f},          {"kg", "kg", nullptr, 1.0f, 0.0f},
    {"t", "t", nullptr, 1000.0f, 0.0f},         {"lb", "lb", nullptr, 0.45359237f, 0.0f},
    {"oz", "oz", nullptr, 0.028349523f, 0.0f},
};
static const unit_def_t k_time[] = {
    {"ms", "ms", nullptr, 0.001f, 0.0f},        {"s", "s", "sec|seconds", 1.0f, 0.0f},
    {"min", "min", "minutes", 60.0f, 0.0f},     {"h", "h", "hours", 3600.0f, 0.0f},
    {"d", "d", "days", 86400.0f, 0.0f},
};
static const unit_def_t k_speed[] = {
    {"m/s", "m/s", nullptr, 1.0f, 0.0f},        {"km/h", "km/h", nullptr, 1.0f / 3.6f, 0.0f},
    {"mph", "mph", nullptr, 0.44704f, 0.0f},    {"knot", "kn", nullptr, 0.514444f, 0.0f},
};
static const unit_def_t k_energy[] = {
    {"J", "J", nullptr, 1.0f, 0.0f},            {"Wh", "Wh", nullptr, 3600.0f, 0.0f},
    {"kWh", "kWh", nullptr, 3600000.0f, 0.0f},  {"cal", "cal", nullptr, 4.184f, 0.0f},
    {"kcal", "kcal", nullptr, 4184.0f, 0.0f},
};
static const unit_def_t k_power[] = {
    {"mW", "mW", nullptr, 0.001f, 0.0f},        {"W", "W", nullptr, 1.0f, 0.0f},
    {"kW", "kW", nullptr, 1000.0f, 0.0f},       {"MW", "MW", nullptr, 1e6f, 0.0f},
    {"hp", "hp", nullptr, 745.69987f, 0.0f},
};
static const unit_def_t k_frequency[] = {
    {"Hz", "Hz", nullptr, 1.0f, 0.0f},          {"kHz", "kHz", nullptr, 1e3f, 0.0f},
    {"MHz", "MHz", nullptr, 1e6f, 0.0f},        {"GHz", "GHz", nullptr, 1e9f, 0.0f},
};
static const unit_def_t k_angle[] = {
    {"rad", "rad", nullptr, 1.0f, 0.0f},        {"deg", "°", nullptr, 0.017453293f, 0.0f},
    {"gon", "gon", nullptr, 0.015707963f, 0.0f},
};

static const quantity_def_t k_quantities[] = {
    {"length", "m", k_length, UNIT_COUNT(k_length)},
    {"volume", "m3", k_volume, UNIT_COUNT(k_volume)},
    {"area", "m2", k_area, UNIT_COUNT(k_area)},
    {"temperature", "C", k_temperature, UNIT_COUNT(k_temperature)},
    {"pressure", "Pa", k_pressure, UNIT_COUNT(k_pressure)},
    {"solar_radiation", "W/m2", k_solar_radiation, UNIT_COUNT(k_solar_radiation)},
    {"force", "N", k_force, UNIT_COUNT(k_force)},
    {"flow_rate", "m3/s", k_flow_rate, UNIT_COUNT(k_flow_rate)},
    {"level", "%", k_level, UNIT_COUNT(k_level)},
    {"mass", "kg", k_mass, UNIT_COUNT(k_mass)},
    {"time", "s", k_time, UNIT_COUNT(k_time)},
    {"speed", "m/s", k_speed, UNIT_COUNT(k_speed)},
    {"energy", "J", k_energy, UNIT_COUNT(k_energy)},
    {"power", "W", k_power, UNIT_COUNT(k_power)},
    {"frequency", "Hz", k_frequency, UNIT_COUNT(k_frequency)},
    {"angle", "rad", k_angle, UNIT_COUNT(k_angle)},
};
#define QUANTITY_COUNT (sizeof(k_quantities) / sizeof(k_quantities[0]))

// ============================================================================
// SD additions / corrections (RAM, only what the file defines)
// ============================================================================

#define UNITS_SD_MAX_ENTRIES   64      /* additions beyond this are ignored (logged) */
#define UNITS_SD_PARSE_DOC     8192    /* temporary parse buffer, freed after loading */

typedef struct {
    char  quantity[24];
    char  si_base[12];
    char  key[16];
    char  symbol[16];
    float factor;
    float offset;
} unit_sd_t;

static unit_sd_t* s_sd_units = nullptr;
static size_t     s_sd_count = 0;

/* Resolved unit: points into the builtin table or the SD entries */
typedef struct {
    const char* quantity;
    const char* key;
    const char* symbol;
    float       factor;
    float       offset;
} unit_ref_t;

// ============================================================================
// Matching
// ============================================================================

/* Exact pass: equal; relaxed pass: case-insensitive, only for names of >= 3 characters */
static bool name_matches(const char* name, const char* target, bool relaxed) {
    if (!name || name[0] == '\0') return false;
    if (strcmp(name, target) == 0) return true;
    return relaxed && strlen(target) >= 3 && strcasecmp(name, target) == 0;
}

static bool aliases_match(const char* aliases, const char* target, bool relaxed) {
    if (!aliases) return false;
    char buf[48];
    strncpy(buf, aliases, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char* save = nullptr;
    for (char* tok = strtok_r(buf, "|", &save); tok; tok = strtok_r(nullptr, "|", &save)) {
        if (name_matches(tok, target, relaxed)) return true;
    }
    return false;
}

static bool find_unit_pass(const char* unit, bool relaxed, unit_ref_t* out) {
    for (size_t i = 0; i < s_sd_count; i++) {
        const unit_sd_t* u = &s_sd_units[i];
        if (name_matches(u->key, unit, relaxed) || name_matches(u->symbol, unit, relaxed)) {
            *out = {u->quantity, u->key, u->symbol[0] ? u->symbol : u->key, u->factor, u->offset};
            return true;
        }
    }
    for (size_t q = 0; q < QUANTITY_COUNT; q++) {
        const quantity_def_t* qd = &k_quantities[q];
        for (uint8_t i = 0; i < qd->count; i++) {
            const unit_def_t* u = &qd->units[i];
            if (name_matches(u->key, unit, relaxed) || name_matches(u->symbol, unit, relaxed) ||
                aliases_match(u->aliases, unit, relaxed)) {
                *out = {qd->name, u->key, u->symbol, u->factor, u->offset};
                return true;
            }
        }
    }
    return false;
}

static bool find_unit(const char* unit, unit_ref_t* out) {
    if (!unit || unit[0] == '\0') return false;
    return find_unit_pass(unit, false, out) || find_unit_pass(unit, true, out);
}

/* Builtin entry with the same key in the same quantity (for the override log) */
static const unit_def_t* find_builtin(const char* quantity, const char* key) {
    for (size_t q = 0; q < QUANTITY_COUNT; q++) {
        if (strcmp(k_quantities[q].name, quantity) != 0) continue;
        for (uint8_t i = 0; i < k_quantities[q].count; i++) {
            if (strcmp(k_quantities[q].units[i].key, key) == 0) return &k_quantities[q].units[i];
        }
    }
    return nullptr;
}

/* Failed conversions are logged once per unit pair (small hash memory) */
#define UNITS_WARN_SLOTS 16
static uint32_t s_warned[UNITS_WARN_SLOTS];
static uint8_t  s_warned_next = 0;

static void warn_once(const char* from, const char* to, const char* reason) {
    uint32_t h = 2166136261u;
    for (const char* p = from; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    h = (h ^ '>') * 16777619u;
    for (const char* p = to; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    for (uint8_t i = 0; i < UNITS_WARN_SLOTS; i++) {
        if (s_warned[i] == h) return;
    }
    s_warned[s_warned_next] = h;
    s_warned_next = (uint8_t)((s_warned_next + 1) % UNITS_WARN_SLOTS);
    LogSerial.printf("[UNITS] Cannot convert '%s' -> '%s' (%s): value used unchanged\r\n", from, to, reason);
}

// ============================================================================
// Public API
// ============================================================================

bool units_init(const char* units_json_path) {
    units_shutdown();
    memset(s_warned, 0, sizeof(s_warned));

    size_t builtin_units = 0;
    for (size_t q = 0; q < QUANTITY_COUNT; q++) builtin_units += k_quantities[q].count;

    if (!units_json_path || units_json_path[0] == '\0') return true;
    SdFat32& sd = sd_card_get_fs();
    if (!sd.exists(units_json_path)) {
        LogSerial.printf("[UNITS] Builtin table: %u units in %u quantities (no %s)\r\n",
                         (unsigned)builtin_units, (unsigned)QUANTITY_COUNT, units_json_path);
        return true;
    }
    File32 file = sd.open(units_json_path, O_READ);
    if (!file) return true;

    /* Only factor / offset / symbol (and si_base of new quantities) are needed */
    StaticJsonDocument<192> filter;
    JsonObject f_qty = filter["quantity_types"].to<JsonObject>()["*"].to<JsonObject>();
    f_qty["si_base"] = true;
    JsonObject f_unit = f_qty["units"].to<JsonObject>()["*"].to<JsonObject>();
    f_unit["factor"] = true;
    f_unit["offset"] = true;
    f_unit["symbol"] = true;

    DynamicJsonDocument* doc = new DynamicJsonDocument(UNITS_SD_PARSE_DOC);
    DeserializationError err = deserializeJson(*doc, file, DeserializationOption::Filter(filter));
    file.close();
    if (err) {
        LogSerial.printf("[UNITS] %s ignored (%s), builtin table only\r\n", units_json_path, err.c_str());
        delete doc;
        return true;
    }

    JsonObject qty_types = (*doc)["quantity_types"];
    size_t total = 0;
    for (JsonPair q : qty_types) total += q.value()["units"].as<JsonObject>().size();
    size_t capacity = (total > UNITS_SD_MAX_ENTRIES) ? UNITS_SD_MAX_ENTRIES : total;
    if (capacity > 0) s_sd_units = static_cast<unit_sd_t*>(calloc(capacity, sizeof(unit_sd_t)));

    size_t same = 0, changed = 0, added = 0, skipped = 0;
    for (JsonPair q : qty_types) {
        const char* si_base = q.value()["si_base"] | "";
        for (JsonPair u : q.value()["units"].as<JsonObject>()) {
            JsonObject uo = u.value().as<JsonObject>();
            const char* key = u.key().c_str();
            float factor = uo["factor"] | 1.0f;
            float offset = uo["offset"] | 0.0f;
            const char* symbol = uo["symbol"] | "";

            const unit_def_t* b = find_builtin(q.key().c_str(), key);
            if (b && fabsf(b->factor - factor) <= fabsf(b->factor) * 1e-4f && fabsf(b->offset - offset) < 1e-3f) {
                same++; /* identical to the builtin entry: not stored */
                continue;
            }
            if (!s_sd_units || s_sd_count >= capacity) {
                skipped++;
                continue;
            }
            unit_sd_t* e = &s_sd_units[s_sd_count++];
            strncpy(e->quantity, q.key().c_str(), sizeof(e->quantity) - 1);
            strncpy(e->si_base, si_base, sizeof(e->si_base) - 1);
            strncpy(e->key, key, sizeof(e->key) - 1);
            strncpy(e->symbol, symbol, sizeof(e->symbol) - 1);
            e->factor = factor;
            e->offset = offset;
            if (b) {
                changed++;
                LogSerial.printf("[UNITS] %s overrides %s.%s: factor %g offset %g (builtin %g / %g)\r\n",
                                 units_json_path, e->quantity, key, (double)factor, (double)offset,
                                 (double)b->factor, (double)b->offset);
            } else {
                added++;
            }
        }
    }
    delete doc;
    if (skipped > 0) {
        LogSerial.printf("[UNITS] %s: more than %u additions, %u ignored\r\n", units_json_path,
                         (unsigned)UNITS_SD_MAX_ENTRIES, (unsigned)skipped);
    }

    LogSerial.printf("[UNITS] Builtin table: %u units in %u quantities; %s: %u added, %u overridden, %u identical (%u B RAM)\r\n",
                     (unsigned)builtin_units, (unsigned)QUANTITY_COUNT, units_json_path, (unsigned)added,
                     (unsigned)changed, (unsigned)same, (unsigned)(s_sd_count * sizeof(unit_sd_t)));
    if (s_sd_count == 0) {
        free(s_sd_units);
        s_sd_units = nullptr;
    } else if (s_sd_count < capacity) {
        void* shrunk = realloc(s_sd_units, s_sd_count * sizeof(unit_sd_t));
        if (shrunk) s_sd_units = static_cast<unit_sd_t*>(shrunk);
    }
    return true;
}

void units_shutdown(void) {
    free(s_sd_units);
    s_sd_units = nullptr;
    s_sd_count = 0;
}

const char* units_get_quantity_type(const char* unit) {
    unit_ref_t u;
    return find_unit(unit, &u) ? u.quantity : nullptr;
}

bool units_is_compatible(const char* quantity_type, const char* unit) {
    const char* detected_type = units_get_quantity_type(unit);
    return (detected_type && quantity_type && strcmp(detected_type, quantity_type) == 0);
}

float units_get_factor(const char* unit) {
    unit_ref_t u;
    return find_unit(unit, &u) ? u.factor : 1.0f;
}

float units_get_offset(const char* unit) {
    unit_ref_t u;
    return find_unit(unit, &u) ? u.offset : 0.0f;
}

const char* units_get_si_base(const char* quantity_type) {
    if (!quantity_type) return nullptr;
    for (size_t i = 0; i < s_sd_count; i++) {
        if (strcmp(s_sd_units[i].quantity, quantity_type) == 0 && s_sd_units[i].si_base[0]) return s_sd_units[i].si_base;
    }
    for (size_t q = 0; q < QUANTITY_COUNT; q++) {
        if (strcmp(k_quantities[q].name, quantity_type) == 0) return k_quantities[q].si_base;
    }
    return nullptr;
}

const char* units_get_symbol(const char* unit) {
    unit_ref_t u;
    return find_unit(unit, &u) ? u.symbol : unit;
}

const char* units_resolve_target(const char* unit, const char* target_unit) {
    return (target_unit && target_unit[0]) ? target_unit : unit;
}

float units_convert(float value, const char* from_unit, const char* to_unit) {
    if (!from_unit || !to_unit || strcmp(from_unit, to_unit) == 0) {
        return value;
    }

    unit_ref_t from, to;
    bool has_from = find_unit(from_unit, &from);
    bool has_to = find_unit(to_unit, &to);
    if (!has_from || !has_to) {
        /* Both empty or both unknown labels (e.g. "units") are not a conversion request */
        if (from_unit[0] && to_unit[0]) warn_once(from_unit, to_unit, "unknown unit");
        return value;
    }
    if (strcmp(from.quantity, to.quantity) != 0) {
        warn_once(from_unit, to_unit, "different quantities");
        return value;
    }
    if (from.factor == to.factor && from.offset == to.offset) return value; /* aliases */
    if (to.factor == 0.0f) return value;

    float base = value * from.factor + from.offset;
    float result = (base - to.offset) / to.factor;
    if (fabsf(result) < 1e6f) result = roundf(result * 1000000.0f) / 1000000.0f;
    return result;
}

bool units_exists(const char* unit) {
    return units_get_quantity_type(unit) != nullptr;
}
