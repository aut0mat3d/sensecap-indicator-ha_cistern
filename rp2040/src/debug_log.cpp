/**
 * @file debug_log.cpp
 * @brief Non-blocking USB CDC console output for RP2040 (see debug_log.hpp).
 */
#include "debug_log.hpp"

NonBlockingLog LogSerial;

size_t NonBlockingLog::write(uint8_t c) {
    return write(&c, 1);
}

size_t NonBlockingLog::write(const uint8_t *buffer, size_t size) {
    if (!buffer || size == 0) return 0;

    int avail = Serial.availableForWrite();
    if (avail <= 0) {
        _dropped += size;
        return size; /* Report success: callers must never retry or block */
    }

    /* Announce lost output once the host drains the FIFO again */
    if (_dropped > 0 && avail >= 64) {
        char note[48];
        int n = snprintf(note, sizeof(note), "\r\n[LOG] %lu bytes dropped\r\n", (unsigned long)_dropped);
        if (n > 0 && n < avail) {
            Serial.write((const uint8_t *)note, (size_t)n);
            avail -= n;
            _dropped = 0;
        }
    }

    size_t chunk = (size <= (size_t)avail) ? size : (size_t)avail;
    if (chunk > 0) {
        Serial.write(buffer, chunk); /* Fits into the FIFO: returns without spinning */
    }
    if (chunk < size) {
        _dropped += (uint32_t)(size - chunk);
    }
    return size;
}

int NonBlockingLog::availableForWrite() {
    return Serial.availableForWrite();
}
