# MicroSD Card Configuration & Schema Specification

This document defines the filesystem structure, configuration schemas, payload decoder mappings, data conversion rules, and communication parameters for the universal open-source LoRaWAN sensor platform.

---

## 1. Directory Structure

The MicroSD card root directory must contain the main configuration folder `lora_p2p_configs/` structured as follows[cite: 7]:

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

Each `.json` file inside the `devices/` directory registers a specific LoRaWAN end-device node[cite: 9]. The file name (excluding `.json`) serves as an internal identifier[cite: 9].

### JSON Schema & Example (`Wasser_Hochbehaelter.json`)

```json
{
  "device_name": "Hochbehälter",
  "medium": "water",
  "enabled": true,
  "dev_addr": "AABBCCDD",
  "dev_sn": "9999999999999999",
  "dev_eui": "AABBCCDDEEFFGGHH",
  "keys": {
    "nwk_skey": "00112233445566778899AABBCCDDEEFF",
    "app_skey": "00112233445566778899AABBCCDDEEFF",
    "d2d_key":  "00112233445566778899AABBCCDDEEFF"
  },
  "decoder": "EM500-SWL",
  "raw_offset_mm": 50
}
```

### Field Definitions

| Field | Type | Description |
| :--- | :--- | :--- |
| `device_name` | `string` | Human-readable name used for UI display and MQTT discovery[cite: 9]. |
| `medium` | `string` | Application context/medium (e.g., `"water"`, `"air"`, `"gas"`)[cite: 9]. |
| `enabled` | `boolean` | `true` to activate parsing and processing for this device[cite: 9]. |
| `dev_addr` | `string` | 4-byte LoRaWAN Device Address in hex (8 characters)[cite: 9]. |
| `dev_sn` | `string` | Sensor serial number (used for asset tracking)[cite: 9]. |
| `dev_eui` | `string` | 8-byte LoRaWAN DevEUI in hex (16 characters)[cite: 9]. |
| `keys.nwk_skey` | `string` | 16-byte Network Session Key in hex (32 characters)[cite: 9]. |
| `keys.app_skey` | `string` | 16-byte Application Session Key in hex (32 characters)[cite: 9]. |
| `keys.d2d_key` | `string` | Optional 16-byte D2D encryption key in hex (32 characters). |
| `decoder` | `string` | Name of the decoder schema file inside `decoder/` (without `.json`)[cite: 9]. |
| `raw_offset_mm` | `int32` | Zero-level physical offset in millimeters (applied during depth parsing)[cite: 9]. |

---

## 4. `decoder/*.json` Specification

Decoder files define how the RP2040 parser extracts, scales, converts, and tags incoming LoRaWAN FRMPayload frames, as well as downstream commands and automated reaction triggers[cite: 8].

### 4.1 Supported Payload Formats

1. **`IPSO_TLV`**: Dynamic tag-length-value parsing based on `channel` and `type` bytes (e.g., Milesight sensors)[cite: 8].
2. **`PACKED_BINARY`**: Fixed struct parsing based on explicit byte `offset` and `length` fields.

---

### 4.2 Standardized Read Data Types (`datatype`)

The `datatype` attribute instructs the RP2040 binary parser how to read raw bytes from the packet[cite: 8]:

| `datatype` | Length | Interpretation / Byte Order |
| :--- | :--- | :--- |
| `uint8` | 1 Byte | Unsigned 8-bit Integer[cite: 8] |
| `int8` | 1 Byte | Signed 8-bit Integer |
| `uint16_le` | 2 Bytes | Unsigned 16-bit Integer (Little Endian)[cite: 8] |
| `int16_le` | 2 Bytes | Signed 16-bit Integer (Little Endian) |
| `uint32_le` | 4 Bytes | Unsigned 32-bit Integer (Little Endian) |
| `int32_le` | 4 Bytes | Signed 32-bit Integer (Little Endian) |
| `float_le` | 4 Bytes | IEEE-754 Single-Precision Floating Point (Little Endian) |
| `bytes_N` | N Bytes | Raw byte array of length N (e.g., `bytes_8` for 8-byte serial)[cite: 8] |
| `ipso_history` | Variable | IPSO historical data block (4-byte timestamp + 2-byte value)[cite: 8] |

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

The `target_type` specifies the output representation serialized over UART `0xBB` to the ESP32 and Home Assistant[cite: 8]:

* **`float`**: 32-bit IEEE-754 floating-point number (used for scaled telemetry measurements)[cite: 8].
* **`int`**: 32-bit signed integer (used for counters, timestamps, discrete status codes)[cite: 8].
* **`bool`**: 8-bit boolean (`0` = false/off, `1` = true/on).
* **`hex` / `string`**: ASCII or formatted hex string (used for serial numbers, versions, text)[cite: 8].

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
