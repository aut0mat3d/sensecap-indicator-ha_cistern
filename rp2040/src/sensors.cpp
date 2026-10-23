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