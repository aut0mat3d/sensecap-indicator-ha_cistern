# Units & Configuration System

**Version:** 1.0.0  
**Last Updated:** 2026-10-09  
**Status:** Draft

---

## 📋 Table of Contents

1. [Overview](#1-overview)
2. [Physical Quantities & Units](#2-physical-quantities--units)
3. [Configuration Hierarchy](#3-configuration-hierarchy)
4. [Unit Conversion Flow](#4-unit-conversion-flow)
5. [Validation Rules](#5-validation-rules)
6. [Decoder Configuration](#6-decoder-configuration)
7. [Device Configuration](#7-device-configuration)
8. [Tank Geometry & Volume Calculation](#8-tank-geometry--volume-calculation)
9. [Display Configuration](#9-display-configuration)
10. [Examples](#10-examples)
11. [API Reference](#11-api-reference)

---

## 1. Overview

The SenseCAP Cistern Extension uses a **flexible unit system** that allows:

- **Sensor-agnostic parsing**: Any sensor can provide data in any unit
- **Automatic unit conversion**: Between compatible units (length→length, volume→volume)
- **Type safety**: Prevents invalid conversions (e.g., length→volume)
- **Multi-system support**: SI (metric) and Imperial units
- **Central configuration**: All units defined in `units.json`

### Core Principles

1. **Decoders** extract raw sensor data with its `unit`
2. **Devices** define tank geometry for volume calculation
3. **Display** can optionally specify `display_unit` for GUI output
4. **Conversion** only happens between compatible quantity types
5. **Fallback**: If conversion impossible, use original unit + log warning

### Supported Quantity Types

| Type | Description | SI Base | Example Units |
|------|-------------|---------|---------------|
| `length` | Linear dimensions | m | cm, mm, inch, ft, m |
| `volume` | Volume/capacity | m³ | liter, m³, gallon_us, barrel |
| `area` | Surface area | m² | cm², m², acre, ft² |
| `temperature` | Temperature | K | °C, °F, K |
| `pressure` | Pressure | Pa | hPa, bar, psi, mmHg |
| `solar_radiation` | Solar/light intensity | W/m² | lux, UV_index, fc |
| `force` | Force | N | kN, lbf, kgf |
| `flow_rate` | Volumetric flow | m³/s | L/min, gpm, m³/h |
| `level` | Fill level | % | %, ratio |
| `mass` | Mass | kg | g, lb, oz, t |
| `time` | Time duration | s | ms, min, h, d |
| `speed` | Speed | m/s | km/h, mph, knot |
| `energy` | Energy | J | kWh, cal, kcal |
| `power` | Power | W | kW, MW, hp |
| `frequency` | Frequency | Hz | kHz, MHz, GHz |
| `angle` | Angle | rad | deg, gon |

---

## 2. Physical Quantities & Units

All physical quantities are organized in **quantity types** with their respective **units**. Each quantity type has:

- A **description** for documentation
- An **SI base unit** for internal calculations
- A set of **units** with their properties:
  - `factor`: Conversion factor to SI base
  - `offset`: Temperature offset (only for temperature units)
  - `symbol`: Display symbol (e.g., "m³", "°C")
  - `name`: Human-readable name

### Example: Length Units

```json
"length": {
  "description": "Linear dimensions and distances",
  "si_base": "m",
  "units": {
    "mm": { "factor": 0.001, "symbol": "mm", "name": "Millimeter" },
    "cm": { "factor": 0.01, "symbol": "cm", "name": "Centimeter" },
    "m":  { "factor": 1.0, "symbol": "m", "name": "Meter" },
    "ft": { "factor": 0.3048, "symbol": "ft", "name": "Foot" }
  }
}
```

### Special Case: Temperature

Temperature units require **offset handling** because of different zero points:

```json
"temperature": {
  "si_base": "K",
  "units": {
    "C": { "factor": 1.0, "offset": 0, "symbol": "°C", "name": "Celsius" },
    "K": { "factor": 1.0, "offset": 0, "symbol": "K", "name": "Kelvin" },
    "F": { "factor": 0.555556, "offset": -459.67, "symbol": "°F", "name": "Fahrenheit" }
  }
}
```

**Conversion Formula:**
```
kelvin = value * factor + offset
converted = (kelvin - target_offset) / target_factor
```

---

## 3. Configuration Hierarchy

```
┌─────────────────────────────────────────────────────────────┐
│                     CONFIGURATION LAYERS                         │
├─────────────────────────────────────────────────────────────┤
│  Layer 1: units.json                    (Builtin + SD card)      │
│  Layer 2: decoder/*.json                (Sensor abstraction)     │
│  Layer 3: devices/*.json                (Device config)         │
│  Layer 4: GUI display config            (User preference)       │
└─────────────────────────────────────────────────────────────┘
```

### File Locations

| Purpose | Builtin Location | SD Card Location |
|---------|------------------|-------------------|
| Units Database | `units.json` | `/config/units.json` (full override) |
| Language Units | N/A | `/config/lang/{lang}/units.json` (names only) |
| Decoders | `main/decoder/` | `/lora_p2p_configs/decoder/` |
| Devices | `main/devices/` | `/lora_p2p_configs/devices/` |
| Display Config | N/A | `/config/globals.json` |

**Language Loading Priority:**
1. Builtin `units.json` (English, compiled-in)  
2. `/config/units.json` (full override)  
3. `/config/lang/{language}/units.json` (language-specific names only)  

**Note:** Language-specific units.json files only need to contain the `name` fields they want to override. All other fields (factor, symbol, offset, etc.) are inherited from the main units database.

---

## 4. Unit Conversion Flow

```
Sensor Raw Data ──────┬─────────────────────┐
                     │                     │
                     ▼                     ▼
         ┌───────────────────┐   ┌─────────────────┐
         │   Decoder JSON     │   │  Decoder Logic   │
         │                   │   │                 │
         │  "unit": "cm"     │───▶ "target_unit":│
         │  "target_unit":   │   │   "cm" or "m"   │
         │   "cm" or "m"     │   │                 │
         └───────────────────┘   └────────┬────────┘
                                              │
                                              ▼
                    ┌────────────────────────────────────┐
                    │     RESOLVED UNIT (for internal use) │
                    │     = target_unit if specified       │
                    │     = unit if target_unit is empty   │
                    └────────────────────────────────────┘
                                              │
        ┌─────────────────────┼─────────────────────┐
        ▼                     ▼                     ▼
   ┌───────────┐      ┌───────────┐          ┌───────────┐
   │  Volume   │      │  Tank     │          │   Raw     │
   │  Calc     │      │  Geometry │          │  Display  │
   │ (m³ calc) │      │  (cm→m³)  │          │  (no conv)│
   └───────────┘      └───────────┘          └───────────┘
        │
        ▼
┌─────────────────────────────────────────────────────────┐
│                    ESP32 GUI Layer                            │
│                                                             │
│  Input: [value=150, unit="cm", display_unit="m"]            │
│         │                                                      │
│         ▼                                                      │
│  Check: cm ∈ length, m ∈ length → ✅ Compatible              │
│         │                                                      │
│         ▼                                                      │
│  Convert: 150 * (0.01 / 1.0) = 1.5 m                          │
│         │                                                      │
│         ▼                                                      │
│  Output: 1.5 m                                               │
│                                                             │
└─────────────────────────────────────────────────────────┘
```

### Key Rules

1. **Decoder Level**: `target_unit` is **optional**
   - If not specified → use `unit` (no conversion)
   - If specified → convert to `target_unit`
   - `target_type` is **MANDATORY**

2. **GUI Level**: `display_unit` is **optional**
   - If not specified → use received unit
   - If specified → attempt conversion
   - If conversion fails → use original unit + log warning

3. **Type Safety**: Only convert within same quantity type
   - ✅ cm → m (both length)
   - ❌ cm → m³ (length vs. volume) → **keep original**

---

## 5. Validation Rules

### Rule 1: Quantity Type Compatibility

**Valid:**
```c
units_convert(150.0, "cm", "m")      // ✅ length → length
units_convert(25.0, "C", "F")       // ✅ temperature → temperature
units_convert(1000.0, "liter", "m3") // ✅ volume → volume
```

**Invalid (keeps original value):**
```c
units_convert(150.0, "cm", "m3")    // ❌ length → volume → returns 150.0
units_convert(25.0, "C", "m")       // ❌ temperature → length → returns 25.0
```

### Rule 2: Unknown Units

If a unit is not found in the database:
- Return original value
- Log warning: `"Unknown unit: xyz"`
- Continue processing

### Rule 3: Temperature Offset

Temperature conversions require special handling:

```c
// Convert 25°C to °F
float celsius = 25.0;
float fahrenheit = units_convert(celsius, "C", "F");
// Result: 77.0°F

// Internally:
// 1. 25°C → Kelvin: 25 * 1.0 + 0 = 298.15 K
// 2. Kelvin → °F: (298.15 - (-459.67)) / 0.555556 = 77.0°F
```

---

## 6. Decoder Configuration

### Schema: `decoder/*.json`

```json
{
  "format": "IPSO_TLV",
  "fport_config": [...],
  "commands": {...},
  "reactions": [...],
  "mappings": [
    {
      "fport": 0,
      "channel": "0x03",
      "type": "0x77",
      "key": "depth",
      "unit": "cm",              // ⭐ MANDATORY: Input unit from sensor
      "target_unit": "cm",      // ⭐ OPTIONAL: Convert to this unit (if empty, use "unit")
      "datatype": "uint16_le",  // ⭐ MANDATORY: How to parse raw bytes
      "target_type": "float",    // ⭐ MANDATORY: Output type (float/int/bool/hex)
      "context": "measurement",  // ⭐ MANDATORY: Classification
      "scale": 1.0
    }
  ]
}
```

### Field Definitions

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `unit` | string | ✅ Yes | Unit of raw sensor data (e.g., "cm", "Pa", "C") |
| `target_unit` | string | ❌ No | Convert to this unit before internal processing. If empty/omitted, use `unit` |
| `datatype` | string | ✅ Yes | How to parse: `uint8`, `int16_le`, `float_le`, `bytes_N`, etc. |
| `target_type` | string | ✅ Yes | Output type: `float`, `int`, `bool`, `hex`, `string` |
| `context` | string | ✅ Yes | Classification: `measurement`, `history`, `diagnostic`, `status` |
| `scale` | float | ❌ No | Multiplicative scale factor (default: 1.0) |

### Unit Resolution in Decoders

```c
// Pseudocode for decoder processing
const char* resolved_unit = units_resolve_target(unit, target_unit);
// If target_unit is NULL or empty → resolved_unit = unit
// Otherwise → resolved_unit = target_unit
```

---

## 7. Device Configuration

### Schema: `devices/*.json`

```json
{
  "device_name": "Hochbehälter",
  "medium": "water",
  "enabled": true,
  "decoder": "EM500-SWL",
  "tank_geometry": {
    "type": "rectangle",
    "width_cm": 150.0,
    "length_cm": 145.0,
    "max_depth_cm": 140.0
  },
  "display_config": {
    "unit_system": "metric",
    "volume_unit": "m3"
  }
}
```

### Tank Geometry Types

| Type | Required Fields | Description |
|------|----------------|-------------|
| `rectangle` | width_cm, length_cm, max_depth_cm | Cuboid tank |
| `cylinder_vertical` | diameter_cm, max_depth_cm | Standing cylinder |
| `cylinder_horizontal` | diameter_cm, length_cm, dish_height_cm, max_depth_cm | Horizontal cylinder |
| `cone_frustum` | bottom_diameter_cm, top_diameter_cm, height_cm, max_depth_cm | Silo/hopper |
| `lut` | max_depth_cm, points[] | Lookup table for irregular tanks |
| `none` | max_depth_cm | Raw depth only, no volume calculation |

**Important:** All geometry dimensions use **centimeters (cm)** as input for consistency.

### Volume Calculation

Volume is calculated internally in **cubic meters (m³)** and can be displayed in any volume unit:

```
Input: depth = 100 cm (from decoder)
Tank: rectangle, width = 150 cm, length = 145 cm

Calculation:
1. depth_m = 100 / 100 = 1.0 m
2. width_m = 150 / 100 = 1.5 m
3. length_m = 145 / 100 = 1.45 m
4. volume_m3 = 1.0 * 1.5 * 1.45 = 2.175 m³

Display:
- volume_unit = "m3" → 2.175 m³
- volume_unit = "liter" → 2175 L
- volume_unit = "gallon_us" → 574.5 gal
```

---

## 8. Tank Geometry & Volume Calculation

### Volume Calculation Principle

**All tank geometry calculations:**
1. Accept **centimeter (cm)** as input for dimensions
2. Convert internally to **meter (m)** for volume calculation
3. Output volume in **cubic meters (m³)** as SI base
4. Can be converted to any volume unit via `units_convert()`

### Example: Rectangular Tank

```json
"tank_geometry": {
  "type": "rectangle",
  "width_cm": 150.0,    // 1.5 m
  "length_cm": 145.0,   // 1.45 m
  "max_depth_cm": 140.0 // 1.4 m
}
```

**Volume Formula:**
```
volume_m3 = (width_cm / 100) * (length_cm / 100) * (depth_cm / 100)
```

**Fill Percentage:**
```
percent = min(100.0, max(0.0, (depth_cm / max_depth_cm) * 100.0))
```

### Future Enhancement: Unit-Aware Geometry

For supporting imperial units in tank geometry (optional future feature):

```json
"tank_geometry": {
  "type": "rectangle",
  "width": { "value": 150.0, "unit": "cm" },
  "length": { "value": 145.0, "unit": "cm" },
  "max_depth": { "value": 140.0, "unit": "cm" }
}
```

---

## 9. Display Configuration

### In `devices/*.json`

```json
"display_config": {
  "unit_system": "metric",    // "metric" or "imperial"
  "volume_unit": "m3"        // Display unit for volume
}
```

### Preset Unit Systems

**Metric Presets:**
- length: m
- volume: m³
- temperature: °C
- pressure: hPa

**Imperial Presets:**
- length: ft
- volume: gallon_us
- temperature: °F
- pressure: psi

### GUI-Side Unit Override

The GUI can specify a `display_unit` for each displayed value:

```c
// In GUI config or per-widget
const char* display_unit = "m";  // Show in meters
float display_value = units_convert(raw_value, raw_unit, display_unit);
```

If conversion fails (incompatible types), the **original value and unit are used** with a warning logged.

---

## 10. Examples

### Example 1: Basic Length Conversion

**Sensor:** Depth sensor outputting centimeters  
**Decoder:** EM500-SWL with `unit: "cm"`, `target_unit: "cm"`  
**Device:** Rectangle tank (150cm × 145cm × 140cm max)  
**GUI:** Display in meters

```c
// RP2040: Decoder processing
float depth_cm = 125.0;  // Raw from sensor
const char* resolved_unit = units_resolve_target("cm", "cm");  // "cm"
// No conversion needed

// RP2040: Volume calculation
float volume_m3 = calculate_volume(125.0, tank_geometry);  // 2.71875 m³

// ESP32: GUI display
float display_value = units_convert(2.71875, "m3", "liter");  // 2718.75 L
```

### Example 2: Temperature Conversion

**Sensor:** Temperature in Celsius  
**Decoder:** `unit: "C"`, `target_unit: "C"`  
**GUI:** Display in Fahrenheit

```c
float temp_c = 25.0;
float temp_f = units_convert(temp_c, "C", "F");  // 77.0°F
```

### Example 3: Invalid Conversion (Fallback)

**Sensor:** Depth in cm  
**GUI:** Display in m³ (invalid!)

```c
float depth_cm = 150.0;
float result = units_convert(depth_cm, "cm", "m3");
// ⚠️ Logs: "Cannot convert from cm (length) to m3 (volume): incompatible quantity types"
// ✅ Returns: 150.0 (original value)
// ✅ GUI displays: "150 cm" (not "150 m3")
```

### Example 4: Decoder with Unit Conversion

**Decoder:** EM500-SWL modified
```json
{
  "key": "depth",
  "unit": "mm",           // Sensor outputs millimeters
  "target_unit": "cm",    // Convert to centimeters
  "datatype": "uint16_le",
  "target_type": "float",
  "context": "measurement"
}
```

**Processing:**
```c
// Sensor outputs: 1500 mm
// Decoder: units_convert(1500, "mm", "cm") = 150.0 cm
// Volume calculation: uses 150 cm
```

### Example 5: Complete Water Cistern Setup

**File:** `lora_p2p_configs/devices/Wasser_Hochbehaelter.json`

```json
{
  "device_name": "Hochbehälter",
  "medium": "water",
  "enabled": true,
  "dev_addr": "AABBCCDD",
  "decoder": "EM500-SWL",
  "tank_geometry": {
    "type": "rectangle",
    "width_cm": 150.0,
    "length_cm": 145.0,
    "max_depth_cm": 140.0
  },
  "display_config": {
    "unit_system": "metric",
    "volume_unit": "m3"
  }
}
```

**File:** `lora_p2p_configs/decoder/EM500-SWL.json` (relevant part)

```json
{
  "mappings": [
    {
      "fport": 0,
      "channel": "0x03",
      "type": "0x77",
      "key": "depth",
      "unit": "cm",
      "target_unit": "",  // Empty = use "cm"
      "datatype": "uint16_le",
      "target_type": "float",
      "context": "measurement",
      "scale": 1.0
    }
  ]
}
```

**Data Flow:**
```
LoRa Payload → EM500-SWL Decoder → depth=125, unit="cm"
                              ↓
                    Tank Geometry Calc → volume=2.71875 m³
                              ↓
                    GUI Display → 2.719 m³  (volume_unit="m3")
```

---

## 11. API Reference

### C API (ESP32)

```c
// Initialization
bool units_init(const char* units_json_path);
void units_shutdown(void);

// Type checking
bool units_is_compatible(const char* quantity_type, const char* unit);
const char* units_get_quantity_type(const char* unit);

// Conversion
float units_convert(float value, const char* from_unit, const char* to_unit);

// Unit properties
float units_get_factor(const char* unit);
float units_get_offset(const char* unit);
const char* units_get_si_base(const char* quantity_type);
const char* units_get_symbol(const char* unit);
const char* units_get_name(const char* unit);

// Validation
bool units_exists(const char* unit);

// Helper
const char* units_resolve_target(const char* unit, const char* target_unit);
```

### C++ API (RP2040)

Same API as C version (extern "C" compatible).

### Usage Examples

**ESP32:**
```c
#include "units.h"

void setup() {
    units_init(NULL);  // Use builtin
}

void display_temperature(float temp, const char* unit) {
    float temp_f = units_convert(temp, unit, "F");
    printf("Temperature: %.1f °F\n", temp_f);
}
```

**RP2040:**
```cpp
#include "units.h"

void process_sensor_data(float value, const char* unit) {
    // Convert to meters for tank calculation
    float value_m = units_convert(value, unit, "m");
    // Use value_m for volume calculation
}
```

---

## 📝 Appendix

### Unit Database File

The `units.json` file is located at:
- **Builtin:** `/units.json` (compiled into firmware)
- **SD Card:** `/config/units.json` (optional override)

### Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0.0 | 2026-10-09 | Initial release |

### References

- SI Units: https://www.bipm.org/en/si-base-units
- Unit Conversion: https://en.wikipedia.org/wiki/Conversion_of_units
- Temperature Conversion: https://en.wikipedia.org/wiki/Conversion_between_Celsius_and_Fahrenheit

---

**© 2026, SenseCAP Cistern Extension**  
**License:** Apache 2.0 (same as main project)
