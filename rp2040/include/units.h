/**
 * @file units.h
 * @brief Unit conversion utilities for RP2040
 *        Used for: (1) Parsing sensor data with unit conversion
 *                 (2) Volume calculation from height values using tank_geometry
 *        Complete builtin table in flash; /config/units.json only adds or corrects units.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef UNITS_H
#define UNITS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief (Re)load the SD additions / corrections on top of the builtin table
 * @param units_json_path Path to units.json (NULL or missing file: builtin table only)
 * @return true (the builtin table is always available)
 */
bool units_init(const char* units_json_path = nullptr);

/**
 * @brief Drop the SD additions (builtin table stays available)
 */
void units_shutdown(void);

/**
 * @brief Check if a unit belongs to a specific quantity type
 * @param quantity_type Quantity type (e.g., "length", "volume", "temperature")
 * @param unit Unit symbol (e.g., "cm", "m3", "C")
 * @return true if unit belongs to the quantity type
 */
bool units_is_compatible(const char* quantity_type, const char* unit);

/**
 * @brief Get the quantity type for a unit
 * @param unit Unit symbol (e.g., "cm", "m3", "C")
 * @return Quantity type string, or NULL if not found
 */
const char* units_get_quantity_type(const char* unit);

/**
 * @brief Convert a value from one unit to another within the same quantity type
 *        (base = value * factor + offset). Impossible conversions are logged once per unit pair.
 * @param value Value to convert
 * @param from_unit Source unit (e.g., "cm")
 * @param to_unit Target unit (e.g., "m")
 * @return Converted value, or original value if conversion not possible
 */
float units_convert(float value, const char* from_unit, const char* to_unit);

/**
 * @brief Get conversion factor for a unit
 * @param unit Unit symbol
 * @return Conversion factor to SI base unit, or 1.0 if not found
 */
float units_get_factor(const char* unit);

/**
 * @brief Get the offset to the base unit (base = value * factor + offset; temperatures only)
 * @param unit Unit symbol
 * @return Offset, or 0.0 if not applicable
 */
float units_get_offset(const char* unit);

/**
 * @brief Get SI base unit for a quantity type
 * @param quantity_type Quantity type
 * @return SI base unit string, or NULL if not found
 */
const char* units_get_si_base(const char* quantity_type);

/**
 * @brief Get symbol for a unit
 * @param unit Unit symbol to look up
 * @return Display symbol, or the unit itself if not found
 */
const char* units_get_symbol(const char* unit);

/**
 * @brief Check if a unit exists in the database
 * @param unit Unit symbol to check
 * @return true if unit exists
 */
bool units_exists(const char* unit);

/**
 * @brief Resolve target_unit for a mapping: if target_unit is empty, use unit
 * @param unit Original unit from sensor
 * @param target_unit Optional target unit (may be NULL or empty)
 * @return Resolved unit to use
 */
const char* units_resolve_target(const char* unit, const char* target_unit);

#ifdef __cplusplus
}
#endif

#endif /* UNITS_H */