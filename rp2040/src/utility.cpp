/**
 * @file utility.cpp
 * @brief Beep and hardware utility helpers.
 * @copyright © 2024-2026, Seeed Studio / SenseCAP Cistern Extension
 */

#include "indicator_rp2040.hpp"

static const int BUZZER_PIN = 19;

/* Non-blocking Alarm State Variables */
static uint16_t s_alarm_tON = 0;
static uint16_t s_alarm_tOFF = 0;
static uint32_t s_alarm_cycle_start_ms = 0;

static uint32_t s_click_until_ms = 0;
static uint32_t s_mute_until_ms = 0;
static bool     s_muted_indefinitely = false;

void alarm_init(void)
{
    pinMode(BUZZER_PIN, OUTPUT);
    analogWrite(BUZZER_PIN, 0);
}

/**
 * @brief Set or clear alarm sound pattern.
 * @param tON_ms Tone duration in ms. (0,0 = STOP, n,0 = STEADY ON)
 * @param tOFF_ms Silence duration in ms.
 */
void alarm_set(uint16_t tON_ms, uint16_t tOFF_ms)
{
    s_alarm_tON = tON_ms;
    s_alarm_tOFF = tOFF_ms;
    s_alarm_cycle_start_ms = millis();

    /* Setting a new alarm automatically clears previous mutes */
    if (tON_ms > 0) {
        s_muted_indefinitely = false;
        s_mute_until_ms = 0;
    }
}

/**
 * @brief Mute ongoing alarm indefinitely or for a specific duration.
 * @param mute_sec 0 = mute until rearmed or new alarm, >0 = snooze for N seconds.
 */
void alarm_mute(uint32_t mute_sec)
{
    if (mute_sec == 0) {
        s_muted_indefinitely = true;
        s_mute_until_ms = 0;
    } else {
        s_muted_indefinitely = false;
        s_mute_until_ms = millis() + (mute_sec * 1000);
    }
    analogWrite(BUZZER_PIN, 0);
}

/**
 * @brief Re-arm alarm system (clears all active mutes).
 */
void alarm_rearm(void)
{
    s_muted_indefinitely = false;
    s_mute_until_ms = 0;
}

/**
 * @brief Non-blocking button click/confirmation beep.
 */
void beep_click(uint16_t duration_ms)
{
    s_click_until_ms = millis() + duration_ms;
}

/* Legacy wrapper functions for backward compatibility */
void beep_init(void) { alarm_init(); }
void beep_off(void) { alarm_set(0, 0); analogWrite(BUZZER_PIN, 0); }
void beep_on(void)  { beep_click(50); }

/**
 * @brief Zero-blocking Buzzer Engine. Must be called in loop().
 */
void buzzer_update(void)
{
    uint32_t now = millis();

    /* 1. Mute evaluation */
    if (s_muted_indefinitely) {
        analogWrite(BUZZER_PIN, 0);
        return;
    }
    if (s_mute_until_ms > 0) {
        if (now < s_mute_until_ms) {
            analogWrite(BUZZER_PIN, 0);
            return;
        } else {
            /* Mute duration expired -> Auto-rearm! */
            s_mute_until_ms = 0;
        }
    }

    /* 2. Alarm pattern evaluation */
    if (s_alarm_tON > 0) {
        if (s_alarm_tOFF == 0) {
            /* Steady ON Alarm */
            analogWrite(BUZZER_PIN, 127);
        } else {
            /* Pulsing Alarm Pattern */
            uint32_t cycle_length = s_alarm_tON + s_alarm_tOFF;
            uint32_t phase = (now - s_alarm_cycle_start_ms) % cycle_length;

            if (phase < s_alarm_tON) {
                analogWrite(BUZZER_PIN, 127);
            } else {
                analogWrite(BUZZER_PIN, 0);
            }
        }
        return;
    }

    /* 3. Non-blocking UI click evaluation */
    if (now < s_click_until_ms) {
        analogWrite(BUZZER_PIN, 127);
    } else {
        analogWrite(BUZZER_PIN, 0);
    }
}