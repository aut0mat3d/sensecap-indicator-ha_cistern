# Units & Configuration System

**Version:** 1.1.0  
**Status:** Active  

---

## 1. Overview

The SenseCAP Cistern Extension uses a **flexible unit system** that allows:

- **Sensor-agnostic parsing**: Any sensor can provide data in any unit.
- **Automatic unit conversion**: Between compatible units (length→length, volume→volume).
- **Type safety**: Prevents invalid conversions (e.g., length→volume).
- **Multi-system support**: SI (metric) and Imperial units.
- **Central configuration**: All units defined in `units.json`.

---

## 2. Configuration Hierarchy

```text
┌─────────────────────────────────────────────────────────────┐
│                     CONFIGURATION LAYERS                    │
├─────────────────────────────────────────────────────────────┤
│  Layer 1: units.json                    (Builtin + SD card) │
│  Layer 2: decoder/*.json                (Sensor abstraction)│
│  Layer 3: devices/*.json                (Device config)     │
│  Layer 4: GUI display config            (User preference)   │
└─────────────────────────────────────────────────────────────┘
```

### File Locations

| Purpose | Builtin Location | SD Card Location |
|---------|------------------|-------------------|
| Units Database | `units.json` | `/config/units.json` |
| Decoders | `main/decoder/` | `/lora_p2p_configs/decoder/` |
| Devices | `main/devices/` | `/lora_p2p_configs/devices/` |
| Display Config | N/A | `/config/globals.json` |

---

## 3. Data Processing & Bounding Flow

The system processes telemetry in strict sequential stages to guarantee data integrity before storage or UI presentation.

```text
[LoRa/Sensor Raw Value] 
       │
       ▼
[Driver Bounds Filter] (val_min, val_max in decoder/*.json)
       │    ├─► Out-of-bounds: DROP immediately (Hardware failure protection)
       │
       ▼
[Offset Addition] (offsets in devices/*.json)
       │    
       ▼
[Device Limits Filter] (val_min, val_max, cap_min, cap_max in devices/*.json)
       │    ├─► cap = 0 (or drop=1) & Out-of-bounds: DROP
       │    ├─► cap = 1 & Out-of-bounds: CLAMP (Restrict to limit)
       │    └─► cap = 2 (or omitted): PASS-THROUGH
       │
       ▼
[Virtual Decoder Cascade] (Volume, Flowrate in devices/*.json)
       │    └─► Volume cap_max applied independently (0=Pass, 1=Clamp, 2=Drop)
       │
       ▼
[Storage & Telemetry] (LittleFS Archiving, GUI Display, Alarm Evaluation)
```

### 3.1 Capping Modes (`cap_min`, `cap_max`)

Whenever limits are defined (in device `limits` or virtual decoder geometry), the capping mode dictates the handling of out-of-bounds values:

*   **`0` (Drop)**: The data point is considered invalid and discarded completely. It will not be logged, displayed, or used in cascading calculations.
*   **`1` (Clamp)**: The value is strictly clamped to the boundary limit. (e.g., A reading of -2 cm is clamped to 0.0 cm).
*   **`2` (Pass-Through)**: The boundary is ignored for data rejection purposes. The value is processed and displayed as-is (e.g., allowing visualization of overflow conditions above the nominal `max_depth`).

---

## 4. Decoder Configuration (`decoder/*.json`)

Decoders define the low-level hardware mappings for extracting bytes from the LoRaWAN payload.

```json
{
  "mappings": [
    {
      "key": "depth",
      "val_min": -1,
      "val_max": 500,
      "unit": "cm",
      "target_unit": "cm",
      "datatype": "uint16_le",
      "target_type": "float",
      "context": "measurement"
    }
  ]
}
```

*   **`val_min` / `val_max`**: Hardware capabilities of the sensor. A depth reading of 600 cm from a 500 cm sensor is a physical impossibility (e.g., sensor glitch/error code) and is dropped at this stage before any offsets are applied.

---

## 5. Device Configuration (`devices/*.json`)

Device configurations apply application-specific offsets, limits, and geometry.

```json
{
  "offsets": {
    "depth": { "value": 50, "unit": "mm" }
  },
  "limits": {
    "depth": {
      "val_min": 0,
      "val_max": 120,
      "cap_min": 1,
      "cap_max": 0
    }
  },
  "virtual_decoder": {
    "volume": {
      "source_key": "depth",
      "type": "rectangle",
      "width": { "value": 200, "unit": "cm" },
      "length": { "value": 200, "unit": "cm" },
      "max_depth": { "value": 120, "unit": "cm" },
      "cap_max": 1
    }
  }
}
```

### 5.1 Offsets

Offsets are added to the valid raw sensor value. In the example above, a raw reading of `100 cm` becomes `105 cm` (after adding 50 mm). The ESP32 GUI can display and modify these offsets, sending updates to the RP2040 via UART.

### 5.2 Device Limits vs. Virtual Decoder Limits

The separation between `limits` and `virtual_decoder` capping allows for accurate physical modeling:

*   **Displaying the real level**: In the example, `limits.depth.cap_max` is `0` (Drop) but here it was corrected to `2` (Pass-Through) or omitted if values should just be passed. If it is set to `2` (Pass-Through) and the water level rises to 125 cm (in a 120 cm tank), the UI will display the true physical measurement: `125 cm`.
*   **Capping the Volume**: The `virtual_decoder.volume` sets `cap_max` to `1` (Clamp) for `max_depth`. Even though the water level is 125 cm, the calculated volume will be capped at the tank's maximum physical capacity (calculating as if the depth were 120 cm), as the extra water is lost to an overflow.

---

## 6. Thresholds & Alarms

Thresholds define the autonomous alarm evaluation logic.

```json
"thresholds": [
  {
    "id": "warn_battery",
    "key": "battery_soc",
    "severity": "warning",
    "value_threshold": 15.0,
    "direction": "falling",
    "damping": 0,
    "hysteresis": 1.0,
    "buzzer_mode": "off"
  }
]
```

### Evaluation Direction (`direction`)

To prevent logical errors (like a low battery alarm firing when the battery is full), the `direction` parameter explicitly defines the mathematical comparison:

*   **`"falling"`**: Translates to `THRESHOLD_OP_LESS_THAN`. The alarm triggers when the measured value falls *below* the `value_threshold` (e.g., dry run, low voltage). It recovers when the value rises above `value_threshold + hysteresis`.
*   **`"rising"`**: Translates to `THRESHOLD_OP_GREATER_THAN`. The alarm triggers when the measured value rises *above* the `value_threshold` (e.g., overflow, over-temperature). It recovers when the value falls below `value_threshold - hysteresis`.
