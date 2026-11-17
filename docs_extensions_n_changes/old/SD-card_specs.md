# MicroSD Card Configuration & Schema Specification

This document defines the filesystem structure, configuration schemas, payload decoder mappings, data conversion rules, and communication parameters for the universal open-source LoRaWAN sensor platform.

---

## 1. Directory Structure

The MicroSD card root directory must contain the main configuration folder `lora_p2p_configs/` structured as follows:

```text
lora_p2p_configs/
├── globals.json              # Global RF & P2P Mesh configuration
├── decoder/                  # Schema definition directory
│   ├── EM500-SWL.json        # Example payload decoder schema
│   └── <DECODER_NAME>.json   # Additional decoder schemas
└── devices/                  # Registered sensor instances
    ├── Wasser_Hochbehaelter.json
    └── <DEVICE_NAME>.json    # Additional device configurations
```

---

## 2. `globals.json` Specification

The `globals.json` file configures the SX1262 LoRa transceiver RF frontend and sets parameters for encrypted inter-device (P2P/Mesh) communication and packet retransmissions.

### JSON Schema & Example

```json
{
  "rf_frequency_hz": 868300000,
  "rf_spreading_factor": 10,
  "rf_bandwidth_khz": 125,
  "rf_coding_rate": 5,
  "rf_preamble_len": 8,
  "sync_word": "0x34",
  "p2p_mesh": {
    "enabled": true,
    "device_id": "AUTO",
    "network_id": "0xAABB",
    "aes_key": "00112233445566778899AABBCCDDEEFF",
    "retransmit_interval_ms": 1500,
    "broadcast_ttl": 2
  },
  "time_config": {
    "posix_timezone": "CET-1CEST,M3.5.0,M10.5.0/3",
    "ntp_server": "pool.ntp.org",
    "time_format": "24h",
    "p2p_time_sync_enabled": true
  }
}
```

### Field Definitions

| Field | Type | Description |
| :--- | :--- | :--- |
| `rf_frequency_hz` | `uint32` | Center frequency in Hz (e.g., `868300000` for 868.3 MHz). |
| `rf_spreading_factor` | `uint8` | Spreading factor (`7` to `12`). |
| `rf_bandwidth_khz` | `uint16` | Bandwidth in kHz (`125`, `250`, or `500`). |
| `rf_coding_rate` | `uint8` | Coding rate denominator (`5` = 4/5, `8` = 4/8). |
| `rf_preamble_len` | `uint8` | Preamble length in symbols (default: `8`). |
| `sync_word` | `string` | Radio sync word in hex (`"0x34"` for LoRaWAN public, `"0x12"` for private P2P). |
| `p2p_mesh.enabled` | `boolean` | Enables encrypted P2P broadcast/retransmit between SenseCap units. |
| `p2p_mesh.device_id` | `string` | Unique identifier for this unit in the local P2P network. |
| `p2p_mesh.network_id` | `string` | Local network ID in hex (`"0x4A6F"`) to filter foreign nodes. |
| `p2p_mesh.aes_key` | `string` | 128-bit AES encryption key (32 hex characters) for P2P payloads. |
| `p2p_mesh.retransmit_count` | `uint8` | Max retransmission attempts for unacknowledged P2P frames. |
| `p2p_mesh.retransmit_interval_ms`| `uint16` | Time-to-wait in milliseconds between P2P retransmission attempts. |
| `p2p_mesh.broadcast_ttl` | `uint8` | Time-to-live / max hop count for multi-hop P2P frame forwarding. |

---

## 3. `devices/*.json` Specification

Each `.json` file inside the `devices/` directory registers a specific LoRaWAN end-device node. The file name (excluding `.json`) serves as an internal identifier.

### 3.1 JSON Schema & Example (`Wasser_Hochbehaelter.json`)

```json
{
  "device_name": "Hochbehälter",
  "medium": "water",
  "enabled": true,
  "dev_addr": "AABBCCDD",
  "dev_sn": "0000000000000000",
  "dev_eui": "0011223344556677",
  "keys": {
    "nwk_skey": "00112233445566778899AABBCCDDEEFF",
    "app_skey": "00112233445566778899AABBCCDDEEFF",
    "d2d_key":  "00112233445566778899AABBCCDDEEFF"
  },
  "decoder": "EM500-SWL",
  "raw_offset_mm": 50,
  "tank_geometry": {
    "type": "rectangle",
    "width_cm": 150.0,
    "length_cm": 145.0,
    "max_depth_cm": 140.0
  },
  "display_config": {
    "unit_system": "metric",
    "volume_unit": "liters"
  }
}
```

### 3.2 Field Definitions

| Field | Type | Required? | Description |
| :--- | :--- | :--- | :--- |
| `device_name` | `string` | **Yes** | Human-readable name used for UI display and MQTT discovery (max 31 chars). |
| `medium` | `string` | Optional | Application context/medium (`"water"`, `"oil"`, `"diesel"`, `"pellets"`, `"grain"`, `"gas"`, etc.). |
| `enabled` | `boolean` | **Yes** | `true` to activate parsing and processing for this device. |
| `dev_addr` | `string` | **Yes** | 4-byte LoRaWAN Device Address in hex (8 characters). |
| `dev_sn` | `string` | Optional | Sensor serial number (used for asset tracking). |
| `dev_eui` | `string` | Optional | 8-byte LoRaWAN DevEUI in hex (16 characters). |
| `keys.nwk_skey` | `string` | **Yes** | 16-byte Network Session Key in hex (32 characters). |
| `keys.app_skey` | `string` | **Yes** | 16-byte Application Session Key in hex (32 characters). |
| `keys.d2d_key` | `string` | Optional | 16-byte D2D encryption key in hex (32 characters). |
| `decoder` | `string` | **Yes** | Name of the decoder schema file inside `decoder/` (without `.json`). |
| `raw_offset_mm` | `int32` | Optional | Zero-level physical offset in millimeters (applied during depth parsing). |
| `tank_geometry` | `object` | Optional | Tank shape and dimensions for volume and percentage calculations. |
| `display_config` | `object` | Optional | Preferred units and GUI display preferences. |

---

### 3.3 Tank Geometry Configuration (`tank_geometry`)

When present, `tank_geometry` enables real-time volumetric calculations.

#### Fill Percentage Rule
Fill percentage is calculated purely as measured depth relative to `max_depth_cm` and is strictly capped at `100.0%`:
$$\text{Fill}_{\%} = \min\left(100.0, \max\left(0.0, \frac{h_{\text{measured}}}{h_{\text{max}}} \cdot 100.0\right)\right)$$

`max_depth_cm` is a **mandatory** field for all geometry types.

#### Supported Geometry Types (`type`)

- **`rectangle`** (Cuboid / Rectangular Cistern)
  - `width_cm` (`float`, Required): Inner width in cm.
  - `length_cm` (`float`, Required): Inner length in cm.
  - `max_depth_cm` (`float`, Required): Maximum usable depth in cm.

- **`cylinder_vertical`** (Standing Cylindrical Tank)
  - `diameter_cm` (`float`, Required): Inner diameter in cm.
  - `max_depth_cm` (`float`, Required): Maximum usable depth in cm.

- **`cylinder_horizontal`** (Horizontal Cylindrical Underground Tank)
  - `diameter_cm` (`float`, Required): Inner tube diameter in cm.
  - `length_cm` (`float`, Required): Cylindrical body length in cm.
  - `dish_height_cm` (`float`, Optional): Side dish / dome protrusion in cm (default: `0.0`).
  - `max_depth_cm` (`float`, Required): Maximum fill height in cm.

- **`cone_frustum`** (Silo / Hopper / Bulk Storage)
  - `bottom_diameter_cm` (`float`, Required): Outlet diameter in cm.
  - `top_diameter_cm` (`float`, Required): Top rim diameter in cm.
  - `height_cm` (`float`, Required): Cone height in cm.
  - `max_depth_cm` (`float`, Required): Maximum fill height in cm.
  - `cone_compensation` (`float`, Optional): Factor for bulk material repose angle (default: `1.0`).

- **`lut`** (Look-Up Table / Irregular Tank)
  - `max_depth_cm` (`float`, Required): Maximum fill height in cm.
  - `points` (`array` of `[depth_cm, liters]`, Required): Calibration points for linear interpolation with a Maximum of 16 points.

- **`none` / Omitted**: Skip volume processing; output raw depth only.

---

### 3.4 Display Configuration (`display_config`)

- **`unit_system`**: `"metric"` (cm, m³, liters) or `"imperial"` (inches, gallons).
- **`volume_unit`**: `"liters"`, `"m3"`, `"gallons"`, `"percent_only"`.

---

### 3.5 Device Configuration Examples

#### Example A: Horizontal Diesel Tank (`cylinder_horizontal`)

```json
{
  "device_name": "Dieseltank Hof",
  "medium": "diesel",
  "enabled": true,
  "dev_addr": "11223344",
  "dev_sn": "SN-DIESEL-001",
  "dev_eui": "0011223344556677",
  "keys": {
    "nwk_skey": "00112233445566778899AABBCCDDEEFF",
    "app_skey": "00112233445566778899AABBCCDDEEFF"
  },
  "decoder": "EM500-SWL",
  "raw_offset_mm": 0,
  "tank_geometry": {
    "type": "cylinder_horizontal",
    "diameter_cm": 160.0,
    "length_cm": 250.0,
    "dish_height_cm": 20.0,
    "max_depth_cm": 155.0
  },
  "display_config": {
    "unit_system": "metric",
    "volume_unit": "liters"
  }
}
```

#### Example B: Grain Silo Hopper (`cone_frustum`)

```json
{
  "device_name": "Pellets Silo",
  "medium": "pellets",
  "enabled": true,
  "dev_addr": "AABBCCDD",
  "dev_sn": "SN-SILO-002",
  "dev_eui": "0011223344556677",
  "keys": {
    "nwk_skey": "00112233445566778899AABBCCDDEEFF",
    "app_skey": "00112233445566778899AABBCCDDEEFF"
  },
  "decoder": "GENERIC-RADAR",
  "raw_offset_mm": 120,
  "tank_geometry": {
    "type": "cone_frustum",
    "bottom_diameter_cm": 30.0,
    "top_diameter_cm": 200.0,
    "height_cm": 280.0,
    "max_depth_cm": 260.0,
    "cone_compensation": 0.92
  },
  "display_config": {
    "unit_system": "metric",
    "volume_unit": "m3"
  }
}
```

#### Example C: Imperial Farm Water Tank (`cylinder_vertical`)

```json
{
  "device_name": "Pasture Water Tank",
  "medium": "water",
  "enabled": true,
  "dev_addr": "99887766",
  "dev_sn": "US-TANK-003",
  "dev_eui": "0011223344556677",
  "keys": {
    "nwk_skey": "00112233445566778899AABBCCDDEEFF",
    "app_skey": "00112233445566778899AABBCCDDEEFF"
  },
  "decoder": "EM500-SWL",
  "raw_offset_mm": 0,
  "tank_geometry": {
    "type": "cylinder_vertical",
    "diameter_cm": 240.0,
    "max_depth_cm": 180.0
  },
  "display_config": {
    "unit_system": "imperial",
    "volume_unit": "gallons"
  }
}
```

#### Example D: Natural Water cavern (`lut`)

```json
{
  "device_name": "Water Cavern",
  "medium": "water",
  "enabled": true,
  "dev_addr": "11223344",
  "decoder": "GENERIC-RADAR",
  "tank_geometry": {
    "type": "lut",
    "max_depth_cm": 200.0,
    "points": [
      [0.0, 0.0],
      [20.0, 150.0],
      [50.0, 500.0],
      [100.0, 1400.0],
      [150.0, 2500.0],
      [200.0, 3200.0]
    ]
  },
  "display_config": {
    "unit_system": "metric",
    "volume_unit": "liters"
  }
}
```

---

## 4. `decoder/*.json` Specification

Decoder files define how the RP2040 parser extracts, scales, converts, and tags incoming LoRaWAN FRMPayload frames, as well as downstream commands and automated reaction triggers.

### 4.1 Supported Payload Formats

1. **`IPSO_TLV`**: Dynamic tag-length-value parsing based on `channel` and `type` bytes (e.g., Milesight sensors).
2. **`PACKED_BINARY`**: Fixed struct parsing based on explicit byte `offset` and `length` fields.

---

### 4.2 Standardized Read Data Types (`datatype`)

The `datatype` attribute instructs the RP2040 binary parser how to read raw bytes from the packet:

| `datatype` | Length | Interpretation / Byte Order |
| :--- | :--- | :--- |
| `uint8` | 1 Byte | Unsigned 8-bit Integer |
| `int8` | 1 Byte | Signed 8-bit Integer |
| `uint16_le` | 2 Bytes | Unsigned 16-bit Integer (Little Endian) |
| `int16_le` | 2 Bytes | Signed 16-bit Integer (Little Endian) |
| `uint32_le` | 4 Bytes | Unsigned 32-bit Integer (Little Endian) |
| `int32_le` | 4 Bytes | Signed 32-bit Integer (Little Endian) |
| `float_le` | 4 Bytes | IEEE-754 Single-Precision Floating Point (Little Endian) |
| `bytes_N` | N Bytes | Raw byte array of length N (e.g., `bytes_8` for 8-byte serial) |
| `ipso_history` | Variable | IPSO historical data block (4-byte timestamp + 2-byte value) |

---

### 4.3 Commands Section (`commands`)

The `commands` object defines executable downlink commands for the target sensor:

* **Static Commands**: Defined via `"raw_hex"` (e.g., `"FF8400"`).
* **Dynamic Commands**: Defined via `"prefix"` and a dynamic parameter `"datatype"` (e.g., `"prefix": "FF03"`, `"datatype": "uint16_le"`).

---

### 4.4 Reactions Section (`reactions`)

The `reactions` array defines automated, event-driven downlink triggers:

| Parameter | Type | Description |
| :--- | :--- | :--- |
| `trigger.fport` | `uint8` | Incoming FPort that triggers the reaction (e.g., `13`). |
| `trigger.raw_prefix` | `string` | Hex byte prefix matching the trigger payload (e.g., `"0x55"`). |
| `trigger.encrypted` | `boolean` | `false` if the trigger FPort sends raw unencrypted bytes (disables AES decryption). Default: `true`. |
| `action.command` | `string` | Name of the target command defined in `commands` (e.g., `"disable_d2d"`). |
| `action.target_fport` | `uint8` | FPort used for sending the automated downlink (e.g., `85`). |
| `action.timing_mode` | `string` | Transmission timing mode (e.g., `"rx1_exact"` for t = 1000 ms). |
| `action.max_retries` | `uint8` | Maximum retry attempts if command is unacknowledged. |

---

### 4.5 Target Output Types (`target_type`)

The `target_type` specifies the output representation serialized over UART `0xBB` to the ESP32 and Home Assistant:

* **`float`**: 32-bit IEEE-754 floating-point number (used for scaled telemetry measurements).
* **`int`**: 32-bit signed integer (used for counters, timestamps, discrete status codes).
* **`bool`**: 8-bit boolean (`0` = false/off, `1` = true/on).
* **`hex` / `string`**: ASCII or formatted hex string (used for serial numbers, versions, text).

---

### 4.6 JSON Schema Example (`EM500-SWL.json`)

```json
{
  "format": "IPSO_TLV",
  "fport_config": [
    {
      "fport": 13,
      "encryption": "partial",
      "cipher_offset": 1,
      "key": "app_skey",
      "payload_offset": 1
    }
  ],
  "commands": {
    "reboot": {
      "fport": 85,
      "raw_hex": "FF10FF"
    },
    "read_status": {
      "fport": 85,
      "raw_hex": "FF28FF"
    },
    "set_report_interval": {
      "fport": 85,
      "prefix": "FF03",
      "datatype": "uint16_le",
      "unit": "seconds",
      "default_val": 120
    }
  },
  "reactions": [
    {
      "trigger": {
        "fport": 228,
        "raw_prefix": "0x55",
        "encrypted": false
      },
      "action": {
        "command": "read_status",
        "timing_mode": "rx1_exact",
        "max_retries": 3
      }
    }
  ],
  "mappings": [
    {
      "fport": 0,
      "channel": "0x01",
      "type": "0x75",
      "key": "battery_soc",
      "unit": "%",
      "datatype": "uint8",
      "target_type": "float",
      "context": "diagnostic",
      "scale": 1.0
    },
    {
      "fport": 0,
      "channel": "0x03",
      "type": "0x77",
      "key": "depth",
      "unit": "cm",
      "target_unit": "cm",
      "datatype": "uint16_le",
      "target_type": "float",
      "context": "measurement",
      "scale": 1.0
    },
    {
      "fport": 0,
      "channel": "0x20",
      "type": "0xCE",
      "key": "history_depth",
      "unit": "cm",
      "datatype": "ipso_history",
      "context": "history"
    },
    {
      "fport": 0,
      "channel": "0xFF",
      "type": "0x01",
      "key": "ipso_version",
      "unit": "",
      "datatype": "uint8",
      "target_type": "int",
      "context": "diagnostic"
    },
    {
      "fport": 0,
      "channel": "0xFF",
      "type": "0x09",
      "key": "hardware_version",
      "unit": "",
      "datatype": "uint16_le",
      "target_type": "hex",
      "context": "diagnostic"
    },
    {
      "fport": 0,
      "channel": "0xFF",
      "type": "0x0A",
      "key": "firmware_version",
      "unit": "",
      "datatype": "uint16_le",
      "target_type": "hex",
      "context": "diagnostic"
    },
    {
      "fport": 0,
      "channel": "0xFF",
      "type": "0x0B",
      "key": "device_status",
      "unit": "",
      "datatype": "uint8",
      "target_type": "int",
      "context": "diagnostic"
    },
    {
      "fport": 0,
      "channel": "0xFF",
      "type": "0x16",
      "key": "serial_number",
      "unit": "",
      "datatype": "bytes_8",
      "target_type": "hex",
      "context": "diagnostic"
    }
  ]
}
```

---

## 5. Virtual Decoder Configuration (`virtual_decoder`)

The `virtual_decoder` section enables computed values derived from raw sensor data or other virtual decoder outputs. This allows for:
- Volume calculations from depth measurements
- Flow rate calculations from volume changes
- Arithmetic operations (sum, average, subtract, multiply, divide)
- Custom calculations like dew point from temperature and humidity
- Cascading calculations where outputs feed into other virtual decoders

### 5.1 JSON Schema

```json
{
  "virtual_decoder": {
    "<output_key>": {
      "source_key": "<source>" | "source_keys": ["<source1>", "<source2>", ...],
      "calculation": "<calc_type>",
      "type": "<geometry_type>",
      "unit": "<unit>",
      "corr_factor": <float>,
      "interval_sec": <uint32>
    },
    "...": { ... }
  }
}
```

### 5.2 Field Definitions

| Field | Type | Required? | Description |
| :--- | :--- | :--- | :--- |
| `<output_key>` | `string` | **Yes** | Unique identifier for this computed value (used as key in telemetry). |
| `source_key` | `string` | Either | Single source key for calculations that use one input. |
| `source_keys` | `array` of `string` | Either | Multiple source keys for calculations that use multiple inputs (e.g., arithmetic operations). Max 4 sources. |
| `calculation` | `string` | **Yes** | Type of calculation to perform. See Calculation Types below. |
| `type` | `string` | *Depends* | Geometry type for volume calculations (required for `CALC_VOLUME`). |
| `unit` | `string` | **Yes** | Output unit for the computed value (e.g., "m3", "m3/h", "°C"). |
| `corr_factor` | `float` | Optional | Correction factor applied to the result (default: `1.0`). |
| `interval_sec` | `uint32` | Optional | Fixed interval in seconds for periodic calculations. `0` = calculate on new data (default: `0`). |

### 5.3 Calculation Types (`calculation`)

The following calculation types are supported:

| Calculation Type | Description | Required Source Keys | Output |
| :--- | :--- | :--- | :--- |
| `"derivative"` | Rate of change: (current - previous) / time_diff | 1 (single `source_key`) | Rate in unit/time (e.g., "m3/h") |
| `"volume"` | Volume calculation using geometry | 1 (single `source_key`, typically depth) | Volume in specified unit |
| `"dewpoint"` | Dew point calculation from temperature and humidity | 2 (`source_keys`: ["temp", "humidity"]) | Temperature in °C |
| `"lut"` | Look-up table interpolation | 1 (single `source_key`) | Interpolated value |
| `"sum"` | Sum of all source values | 1+ (multiple `source_keys`) | Sum in specified unit |
| `"average"` | Average of all source values | 1+ (multiple `source_keys`) | Average in specified unit |
| `"subtract"` | Subtraction: source[0] - source[1] - ... - source[n] | 2+ (multiple `source_keys`) | Result in specified unit |
| `"multiply"` | Multiplication: source[0] * source[1] * ... * source[n] | 2+ (multiple `source_keys`) | Product in specified unit |
| `"divide"` | Division: source[0] / source[1] / ... / source[n] | 2+ (multiple `source_keys`) | Ratio in specified unit |

#### Notes on Arithmetic Operations

- **`subtract`**: Computes `source[0] - source[1] - source[2] - ... - source[n]`. Result has the same unit as source[0].
- **`multiply`**: Computes `source[0] * source[1] * source[2] * ... * source[n]`. Result unit is the product of all source units (e.g., "m" * "m" = "m²").
- **`divide`**: Computes `source[0] / source[1] / source[2] / ... / source[n]`. Protected against division by zero (returns 0 if divisor is 0). Result is dimensionless ratio.
- **`sum`** and **`average`**: All sources must have compatible units (unit conversion is applied if needed).

### 5.4 Volume Calculation Geometry Types

When `calculation` is `"volume"`, the `type` field specifies the geometry:

- **`"rectangle"`**: Rectangular tank
  - `width`: Inner width in cm (required)
  - `length`: Inner length in cm (required)
  - `max_depth`: Maximum depth in cm (required)

- **`"cylinder_vertical"`**: Standing cylindrical tank
  - `diameter`: Inner diameter in cm (required)
  - `max_depth`: Maximum depth in cm (required)

- **`"cylinder_horizontal"`**: Horizontal cylindrical tank
  - `diameter`: Inner diameter in cm (required)
  - `length`: Tank length in cm (required)
  - `max_depth`: Maximum fill height in cm (required)
  - `dish_height`: Dome height in cm (optional, default: 0)

- **`"cone_frustum"`**: Conical/frustum shaped silo
  - `bottom_diameter`: Outlet diameter in cm (required)
  - `top_diameter`: Top rim diameter in cm (required)
  - `height`: Cone height in cm (required)
  - `max_depth`: Maximum fill height in cm (required)
  - `cone_compensation`: Bulk material repose angle factor (optional, default: 1.0)

For geometry dimensions, you can also use objects with `value` and `unit` for unit conversion:
```json
"width": { "value": 200, "unit": "cm" }
```

### 5.5 Dew Point Calculation

The `"dewpoint"` calculation requires exactly two source keys:
- First source: Temperature in °C
- Second source: Relative humidity in %

Output unit is always °C (dew point temperature).

### 5.6 Unit Conversion

All virtual decoder calculations support automatic unit conversion:
- Source values are converted to the target unit specified in the decoder's `unit` field
- Uses the project's built-in `units.h` conversion library
- Supports metric and imperial units (cm, m, mm, inches, feet, liters, m³, gallons, etc.)

### 5.7 Cascading Calculations

Virtual decoders can use the output of other virtual decoders as inputs, enabling complex calculation pipelines:

```json
{
  "virtual_decoder": {
    "volume": {
      "source_key": "depth",
      "type": "rectangle",
      "width": { "value": 200, "unit": "cm" },
      "length": { "value": 200, "unit": "cm" },
      "max_depth": { "value": 140, "unit": "cm" },
      "unit": "m3"
    },
    "flowrate": {
      "source_key": "volume",
      "calculation": "derivative",
      "unit": "m3/h"
    }
  }
}
```

In this example, the `flowrate` virtual decoder uses the output of the `volume` virtual decoder.

### 5.8 Example: Complete Device with Virtual Decoders

```json
{
  "device_name": "Water Tank with Flow Calculation",
  "medium": "water",
  "enabled": true,
  "dev_addr": "05975DC9",
  "decoder": "EM500-SWL",
  "raw_offset_mm": 50,
  "tank_geometry": {
    "type": "rectangle",
    "width_cm": 200.0,
    "length_cm": 200.0,
    "max_depth_cm": 140.0
  },
  "virtual_decoder": {
    "volume": {
      "source_key": "depth",
      "type": "rectangle",
      "width": { "value": 200, "unit": "cm" },
      "length": { "value": 200, "unit": "cm" },
      "max_depth": { "value": 140, "unit": "cm" },
      "unit": "m3"
    },
    "level_gain": {
      "source_key": "depth",
      "calculation": "derivative",
      "interval_sec": 0,
      "unit": "cm/h"
    },
    "flowrate": {
      "source_key": "volume",
      "calculation": "derivative",
      "interval_sec": 0,
      "unit": "m3/h"
    },
    "total_volume": {
      "source_keys": ["volume_tank1", "volume_tank2"],
      "calculation": "sum",
      "unit": "m3"
    },
    "temperature_difference": {
      "source_keys": ["temp_outdoor", "temp_indoor"],
      "calculation": "subtract",
      "unit": "°C"
    }
  },
  "display_config": {
    "unit_system": "metric",
    "volume_unit": "m3"
  }
}
```

### 5.9 Cascading with Arithmetic Operations

The power of virtual decoders comes from combining them in cascades:

```json
{
  "virtual_decoder": {
    "raw_sum": {
      "source_keys": ["value_a", "value_b", "value_c"],
      "calculation": "sum",
      "unit": "units"
    },
    "raw_average": {
      "source_keys": ["value_a", "value_b", "value_c"],
      "calculation": "average",
      "unit": "units"
    },
    "product": {
      "source_keys": ["value_a", "value_b"],
      "calculation": "multiply",
      "unit": "units²"
    },
    "ratio": {
      "source_keys": ["value_a", "value_b"],
      "calculation": "divide",
      "unit": ""
    },
    "complex_calc": {
      "source_keys": ["raw_sum", "product"],
      "calculation": "subtract",
      "unit": "combined"
    }
  }
}
```

In this configuration:
1. `raw_sum`, `raw_average`, `product`, and `ratio` are computed from raw sensor data
2. `complex_calc` then uses the outputs of `raw_sum` and `product` as its inputs

---

## 6. Updated Directory Structure

With the addition of virtual decoders, the complete SD card structure is:

```text
lora_p2p_configs/
├── globals.json              # Global RF & P2P Mesh configuration
├── config/                  # Additional system configs
│   ├── units.json           # Unit conversion definitions
│   └── ...
├── decoder/                  # Schema definition directory
│   ├── EM500-SWL.json        # Example payload decoder schema
│   └── <DECODER_NAME>.json   # Additional decoder schemas
└── devices/                  # Registered sensor instances
    ├── Wasser_Hochbehaelter.json
    └── <DEVICE_NAME>.json    # Additional device configurations
```
