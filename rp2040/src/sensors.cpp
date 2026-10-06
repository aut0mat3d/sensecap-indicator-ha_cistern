/**
 * @file sensors.cpp
 * @brief Fully non-blocking sensor drivers & dynamic I2C scanner.
 * @copyright © 2024-2026, Seeed Studio / SenseCAP Cistern Extension
 */
/**
 * based on:
 * @file sensors.ino
 * @author Spencer
 * @brief Sensor instances
 * @version 0.1
 * @date 2024-02-29
 * 
 * @copyright Copyright (c) 2024
 * 
 */

#include "indicator_rp2040.hpp"
#include "AHT20.h"
#include <SensirionI2CSgp40.h>
#include <SensirionI2cScd4x.h>
#include <VOCGasIndexAlgorithm.h>
#include <math.h>
#include <string.h>

static AHT20             AHT;
static SensirionI2CSgp40 sgp40;
static SensirionI2cScd4x scd4x;

static uint8_t sht41_address = 0x00;
static uint8_t light_sensor_addr = 0x00;

void sensor_power_on(void)
{
    pinMode(18, OUTPUT);
    digitalWrite(18, HIGH);
}

void sensor_power_off(void)
{
    pinMode(18, OUTPUT);
    digitalWrite(18, LOW);
}

static uint8_t sensor_packet_checksum(const uint8_t* data, size_t len)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < len; i++) {
        checksum ^= data[i];
    }
    return checksum;
}

void sensor_data_send(PacketSerial& packetSerial, pkt_type type, float data)
{
    uint8_t data_buf[32];
    data_buf[0] = static_cast<uint8_t>(type);
    memcpy(&data_buf[1], &data, sizeof(float));
    int packetLength = 1 + sizeof(float);
    packetSerial.send(data_buf, packetLength);
}

void sensor_attached_send(PacketSerial& packetSerial, uint8_t id, uint8_t category, const char* name, const char* unit)
{
    uint8_t name_len = static_cast<uint8_t>(strlen(name));
    uint8_t unit_len = static_cast<uint8_t>(strlen(unit));
    if (name_len > 15) name_len = 15;
    if (unit_len > 7) unit_len = 7;

    uint8_t data_buf[32] = {0};
    size_t offset = 0;
    data_buf[offset++] = static_cast<uint8_t>(PKT_TYPE_SENSOR_ATTACHED);
    data_buf[offset++] = id;
    data_buf[offset++] = category;
    data_buf[offset++] = name_len;
    memcpy(&data_buf[offset], name, name_len);
    offset += name_len;
    data_buf[offset++] = unit_len;
    memcpy(&data_buf[offset], unit, unit_len);
    offset += unit_len;
    data_buf[offset++] = sensor_packet_checksum(data_buf, offset);

    packetSerial.send(data_buf, offset);
}

void sensor_detached_send(PacketSerial& packetSerial, uint8_t id)
{
    uint8_t data_buf[3] = {
        static_cast<uint8_t>(PKT_TYPE_SENSOR_DETACHED),
        id,
        0
    };
    data_buf[2] = sensor_packet_checksum(data_buf, 2);
    packetSerial.send(data_buf, sizeof(data_buf));
}

void sensor_value_send(PacketSerial& packetSerial, uint8_t id, float data)
{
    uint8_t data_buf[7] = {0};
    data_buf[0] = static_cast<uint8_t>(PKT_TYPE_SENSOR_VALUE);
    data_buf[1] = id;
    memcpy(&data_buf[2], &data, sizeof(float));
    data_buf[6] = sensor_packet_checksum(data_buf, 6);
    packetSerial.send(data_buf, sizeof(data_buf));
}

void lora_telemetry_send(PacketSerial& packetSerial, uint32_t dev_addr, uint8_t id, float data)
{
    uint8_t data_buf[11] = {0};
    data_buf[0] = static_cast<uint8_t>(PKT_TYPE_LORA_TELEMETRY);
    memcpy(&data_buf[1], &dev_addr, sizeof(uint32_t));
    data_buf[5] = id;
    memcpy(&data_buf[6], &data, sizeof(float));
    data_buf[10] = sensor_packet_checksum(data_buf, 10);
    packetSerial.send(data_buf, sizeof(data_buf));
}

/**
 * @brief Dynamic I2C Bus Auto-Scanner (Zero delay, non-blocking probing).
 */
void sensor_i2c_auto_detect(PacketSerial& packetSerial)
{
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
            if (addr == 0x38) {
                sensor_attached_send(packetSerial, PKT_SENSOR_ID_AHT20_TEMP, PKT_SENSOR_CAT_TEMP, "AHT20", "C");
                sensor_attached_send(packetSerial, PKT_SENSOR_ID_AHT20_HUMIDITY, PKT_SENSOR_CAT_HUMIDITY, "AHT20", "%RH");
            } else if (addr == 0x44 || addr == 0x45 || addr == 0x46) {
                sensor_attached_send(packetSerial, PKT_SENSOR_ID_GROVE_BASE, PKT_SENSOR_CAT_TEMP, "SHT41", "C");
                sensor_attached_send(packetSerial, PKT_SENSOR_ID_GROVE_BASE + 1, PKT_SENSOR_CAT_HUMIDITY, "SHT41", "%RH");
            } else if (addr == 0x59) {
                sensor_attached_send(packetSerial, PKT_SENSOR_ID_SGP40_VOC, PKT_SENSOR_CAT_VOC, "SGP40", "idx");
            } else if (addr == 0x62) {
                sensor_attached_send(packetSerial, PKT_SENSOR_ID_SCD41_CO2, PKT_SENSOR_CAT_CO2, "SCD41", "ppm");
            } else if (addr == 0x23 || addr == 0x29 || addr == 0x39 || addr == 0x4A) {
                light_sensor_addr = addr;
                sensor_attached_send(packetSerial, PKT_SENSOR_ID_LIGHT_LUX, PKT_SENSOR_CAT_LIGHT, "Light", "lux");
            }
        }
    }
}

/* ── Non-blocking SHT41 Driver ─────────────────────────────────────────── */

static uint8_t sht41_crc8(const uint8_t* data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

void sensor_aht_init(void) { AHT.begin(); }

bool sensor_aht_get(AHTData& data)
{
    data.humidity = AHT.getHumidity();
    data.temperature = AHT.getTemperature();
    return !(isnan(data.humidity) || isnan(data.temperature));
}

bool sensor_sht41_init(void)
{
    const uint8_t addresses[] = {0x44, 0x45, 0x46};
    sht41_address = 0x00;
    for (uint8_t i = 0; i < sizeof(addresses); i++) {
        Wire.beginTransmission(addresses[i]);
        Wire.write(0x89);
        if (Wire.endTransmission() == 0) {
            sht41_address = addresses[i];
            return true;
        }
    }
    return false;
}

/**
 * @brief Non-blocking SHT41 measurement trigger.
 */
bool sensor_sht41_trigger(void)
{
    if (sht41_address == 0x00) return false;
    Wire.beginTransmission(sht41_address);
    Wire.write(0xFD);
    return (Wire.endTransmission() == 0);
}

/**
 * @brief Read SHT41 data after non-blocking conversion wait.
 */
bool sensor_sht41_read(AHTData& data)
{
    if (sht41_address == 0x00) return false;
    if (Wire.requestFrom(sht41_address, (uint8_t)6) != 6) return false;

    uint8_t raw[6];
    for (uint8_t i = 0; i < 6; i++) raw[i] = Wire.read();
    if (sht41_crc8(&raw[0], 2) != raw[2] || sht41_crc8(&raw[3], 2) != raw[5]) return false;

    uint16_t raw_temp = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t raw_humi = ((uint16_t)raw[3] << 8) | raw[4];
    data.temperature = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    data.humidity    = -6.0f + 125.0f * ((float)raw_humi / 65535.0f);
    return true;
}

/* ── SGP40 & SCD41 Non-blocking Drivers ────────────────────────────────── */

static VOCGasIndexAlgorithm voc_algorithm;

void sensor_sgp40_init(void) { sgp40.begin(Wire); }

bool sensor_sgp40_get(SPG40Data& data, const uint16_t compensationRh, const uint16_t compensationT)
{
    data.srawVoc = 0;
    uint16_t error = sgp40.measureRawSignal(compensationRh, compensationT, data.srawVoc);
    if (error != 0) return false;
    data.vocIndex = voc_algorithm.process(data.srawVoc);
    return true;
}

void sensor_scd4x_init(void)
{
    scd4x.begin(Wire, 0x62);
    scd4x.stopPeriodicMeasurement();
    scd4x.startPeriodicMeasurement();
}

bool sensor_scd4x_get(SCD4XData& data)
{
    uint16_t error = scd4x.readMeasurement(data.co2, data.temperature, data.humidity);
    return (error == 0 && data.co2 != 0);
}

/* ── Light Sensor Non-blocking Driver ─────────────────────────────────── */

bool sensor_light_init(void)
{
    if (light_sensor_addr == 0x00) return false;
    if (light_sensor_addr == 0x23) {
        Wire.beginTransmission(light_sensor_addr);
        Wire.write(0x10);
        return (Wire.endTransmission() == 0);
    }
    return true;
}

bool sensor_light_get(LightData& data)
{
    data.valid = false;
    if (light_sensor_addr == 0x23) {
        if (Wire.requestFrom(light_sensor_addr, (uint8_t)2) == 2) {
            uint16_t raw = ((uint16_t)Wire.read() << 8) | Wire.read();
            data.lux = raw / 1.2f;
            data.valid = true;
        }
    }
    return data.valid;
}