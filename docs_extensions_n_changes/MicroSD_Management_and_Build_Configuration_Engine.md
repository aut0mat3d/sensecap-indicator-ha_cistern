# MicroSD Card Configuration & Schema Specification

This document defines the filesystem structure, configuration schemas, payload decoder mappings, data conversion rules, and communication parameters for the universal open-source LoRaWAN sensor platform.

---

## 1. Directory Structure

The MicroSD card root directory must contain the main configuration folder `lora_p2p_configs/` structured as follows:

```text
lora_p2p_configs/
├── globals.json              # Global RF & P2P Mesh configuration
├── config/                   # Additional system configs
│   ├── units.json            # Unit conversion definitions
│   └── ...
├── decoder/                  # Schema definition directory
│   ├── EM500-SWL.json        # Example payload decoder schema
│   └── <DECODER_NAME>.json   # Additional decoder schemas
└── devices/                  # Registered sensor instances
    ├── Water_Reservoir.json
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
| `p2p_mesh.enabled` | `boolean` | Enables encrypted P2P broadcast/retransmit between units. |
| `p2p_mesh.device_id` | `string` | Unique identifier for this unit in the local P2P network. |
| `p2p_mesh.network_id` | `string` | Local network ID in hex (`"0x4A6F"`) to filter foreign nodes. |
| `p2p_mesh.aes_key` | `string` | 128-bit AES encryption key (32 hex characters) for P2P payloads. |
| `p2p_mesh.retransmit_count` | `uint8` | Max retransmission attempts for unacknowledged P2P frames. |
| `p2p_mesh.retransmit_interval_ms`| `uint16` | Time-to-wait in milliseconds between P2P retransmission attempts. |
| `p2p_mesh.broadcast_ttl` | `uint8` | Time-to-live / max hop count for multi-hop P2P frame forwarding. |

---

## 3. `devices/*.json` Specification

Each `.json` file inside the `devices/` directory registers a specific LoRaWAN end-device node. The file name (excluding `.json`) serves as an internal identifier.

### 3.1 JSON Schema & Example

```json
{
  "device_name": "Water Reservoir",
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
  "decoder": "SENSOR-DECODER-1",
  "offsets": {
    "depth": { "value": 50, "unit": "mm" }
  },
  "tank_geometry": {
    "type": "rectangle",
    "width_cm": 150.0,
    "length_cm": 145.0,
    "max_depth_cm": 140.0
  },
  "limits": {
    "depth": {
      "val_min": 0,
      "val_max": 120,
      "cap_min": 1,
      "cap_max": 0
    }
  },
  "display_config": {
    "unit_system": "metric",
    "volume_unit": "m3"
  }
}
```

### 3.2 Field Definitions

| Field | Type | Required? | Description |
| :--- | :--- | :--- | :--- |
| `device_name` | `string` | **Yes** | Human-readable name used for UI display (max 31 chars). |
| `medium` | `string` | Optional | Application context/medium (`"water"`, `"oil"`, `"diesel"`, `"pellets"`, `"grain"`, etc.). |
| `enabled` | `boolean` | **Yes** | `true` to activate parsing and processing for this device. |
| `dev_addr` | `string` | **Yes** | 4-byte LoRaWAN Device Address in hex (8 characters). |
| `dev_sn` | `string` | Optional | Sensor serial number (used for asset tracking). |
| `dev_eui` | `string` | Optional | 8-byte LoRaWAN DevEUI in hex (16 characters). |
| `keys.nwk_skey` | `string` | **Yes** | 16-byte Network Session Key in hex (32 characters). |
| `keys.app_skey` | `string` | **Yes** | 16-byte Application Session Key in hex (32 characters). |
| `keys.d2d_key` | `string` | Optional | 16-byte D2D encryption key in hex (32 characters). |
| `decoder` | `string` | **Yes** | Name of the decoder schema file inside `decoder/` (without `.json`). |
| `offsets` | `object` | Optional | Zero-level physical offsets applied *before* bounds evaluation. Keys match telemetry points (e.g., `depth`). Each object contains `value` and `unit`. |
| `limits` | `object` | Optional | Bound controls applied *after* offsets (see Section 3.3). |
| `tank_geometry` | `object` | Optional | Tank shape and dimensions for basic volume calculations (if not using `virtual_decoder`). |
| `display_config` | `object` | Optional | Preferred units and GUI display preferences. |

### 3.3 Data Quality Constraints (`limits`)

The `limits` object enforces plausibility and bounding limits on incoming telemetry values **after** offsets are applied. 

Each telemetry key (e.g., `depth`) can define a limit object with the following properties:

- `val_min` (`float`, Required if limiting): Minimum allowed value.
- `val_max` (`float`, Required if limiting): Maximum allowed value.
- `cap_min` (`uint8`, Required if limiting): Behavior when value < `val_min`:
  - `0` = **Drop**: Discard the value (ignore data point).
  - `1` = **Clamp**: Restrict value to `val_min`.
  - `2` = **Pass-Through**: Allow value as-is (ignore boundary).
- `cap_max` (`uint8`, Required if limiting): Behavior when value > `val_max`:
  - `0` = **Drop**: Discard the value (ignore data point).
  - `1` = **Clamp**: Restrict value to `val_max`.
  - `2` = **Pass-Through**: Allow value as-is (ignore boundary).

*Note:* `cap_min` / `cap_max` rules take priority over the global driver-level `val_min` / `val_max` bounds defined in the `decoder/*.json` mapping.

---

## 4. `decoder/*.json` Specification

Decoder files define how the RP2040 parser extracts, scales, converts, and tags incoming LoRaWAN FRMPayload frames. They represent the hardware-level "Driver" for the sensor.

### 4.1 Supported Payload Formats

1. **`IPSO_TLV`**: Dynamic tag-length-value parsing based on `channel` and `type` bytes.
2. **`PACKED_BINARY`**: Fixed struct parsing based on explicit byte `offset` and `length` fields.

### 4.2 Hardware Bounds in Mappings (`val_min`, `val_max`)

In addition to data types, mappings can define absolute physical boundaries (`val_min` and `val_max`) the sensor hardware is capable of measuring. 
**If a raw sensor value falls outside this range, the frame is immediately dropped** before any offsets or device-level limits are processed.

```json
{
  "fport": 0,
  "channel": "0x03",
  "type": "0x77",
  "key": "depth",
  "val_min": -1.0,
  "val_max": 500.0,
  "unit": "cm",
  "datatype": "uint16_le",
  "target_type": "float",
  "context": "measurement"
}
```

---

## 5. Virtual Decoder Configuration (`virtual_decoder`)

The `virtual_decoder` section enables computed values derived from raw sensor data or other virtual decoder outputs (e.g., calculating volume, flow rates, or combining metrics).

### 5.1 JSON Schema

```json
{
  "virtual_decoder": {
    "<output_key>": {
      "source_key": "<source>" | "source_keys": ["<source1>", "<source2>", ...],
      "calculation": "<calc_type>",
      "type": "<geometry_type>",
      "unit": "<unit>",
      "cap_max": <uint8>,
      "interval_sec": <uint32>
    }
  }
}
```

### 5.2 Volume Calculation & Overfill Limiting

When `calculation` is `"volume"`, the `type` field specifies the geometry.

Crucially, virtual decoders can implement their own capping logic independent of the visual representation. For example, a tank might overflow at 120cm. You want the display to show 124cm (actual depth), but the calculated stored volume must never exceed the tank's maximum cubic capacity.

- `max_depth` (`object`, Required): Maximum fill height (e.g., `{ "value": 120, "unit": "cm" }`).
- `cap_max` (`uint8`, Optional): Behavior when source value exceeds `max_depth`.
  - `0` (or omitted) = **Pass-Through**: Calculate volume linearly beyond `max_depth`.
  - `1` = **Clamp**: Cap input depth to `max_depth` (restricting volume to nominal tank capacity).
  - `2` = **Drop**: Stop calculation if input depth exceeds `max_depth`.

#### Example

```json
"virtual_decoder": {
  "volume": {
    "source_key": "depth",
    "type": "rectangle",
    "width": { "value": 200, "unit": "cm" },
    "length": { "value": 200, "unit": "cm" },
    "max_depth": { "value": 120, "unit": "cm" },
    "cap_max": 1,
    "unit": "m3"
  }
}
```

---

## 6. Thresholds Configuration (`thresholds`)

Thresholds define the conditions under which alarms are triggered and evaluated autonomously.

### 6.1 JSON Schema & Example

```json
"thresholds": [
  {
    "id": "warn_high",
    "key": "depth",
    "severity": "warning",
    "value_percent": 110.0,
    "direction": "rising",
    "damping": 0,
    "hysteresis_percent": 3.0,
    "buzzer_mode": "off"
  },
  {
    "id": "alarm_frate",
    "key": "flowrate",
    "severity": "danger",
    "value_threshold": -2.0,
    "direction": "falling",
    "damping": 2,
    "hysteresis": 0.4,
    "buzzer_mode": "intermittent"
  }
]
```

### 6.2 Field Definitions

| Field | Type | Description |
| :--- | :--- | :--- |
| `id` | `string` | Unique identifier for the threshold rule. |
| `key` | `string` | Telemetry key to evaluate (e.g., `"depth"`, `"battery_soc"`). |
| `severity` | `string` | `"danger"`, `"warning"`, or `"info"`. |
| `direction` | `string` | **Evaluation Direction**:<br>`"falling"` = Triggers when value < threshold (e.g., dry run, low battery).<br>`"rising"` = Triggers when value > threshold (e.g., overflow). |
| `value_threshold` / `value_percent` | `float` | The trigger value. Use `value_percent` to compute a dynamic threshold based on tank `max_depth_cm`. |
| `hysteresis` / `hysteresis_percent` | `float` | Deadband required to clear the alarm. |
| `damping` | `uint8` | Number of consecutive matching readings required to trigger the alarm (debounce). |
| `buzzer_mode` | `string` | Acoustic response: `"continuous"`, `"intermittent"`, or `"off"`. |
