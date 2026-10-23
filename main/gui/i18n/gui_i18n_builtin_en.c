/**
 * @file gui_i18n_builtin_en.c
 * @brief Flash-resident base English dictionary.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "gui_i18n.h"

const i18n_entry_t g_i18n_builtin_en[] = {
    /* Common UI Actions */
    {"common.back",         "Back"},
    {"common.cancel",       "Cancel"},
    {"common.save",         "Save"},
    {"common.delete",       "Delete"},
    {"common.ok",           "OK"},
    {"common.close",        "Close"},
    {"common.quit",         "Quit"},
    {"common.mute",         "Mute"},
    {"common.settings",     "Settings"},
    {"common.refresh",      "Refresh"},

    /* Status & Qualities */
    {"status.ok",           "Normal"},
    {"status.warning",      "Warning"},
    {"status.danger",       "Danger"},
    {"status.info",         "Info"},
    {"status.incoming",     "Incoming"},
    {"status.going",        "Going"},
    {"status.muted",        "Muted"},
    {"status.retriggered",  "Retriggered"},
    {"status.acknowledged", "Acknowledged"},
    {"status.estimated",    "Estimated"},
    {"status.offline",      "Offline"},

    /* Generic Telemetry Keys */
    {"telemetry.level",     "Level:"},
    {"telemetry.rate",      "Rate of Change:"},
    {"telemetry.updated",   "Last Update:"},
    {"telemetry.volume",    "Volume:"},
    {"telemetry.depth",     "Depth:"},
    {"telemetry.flow",      "Flow Rate:"},
    {"telemetry.temp",      "Temperature:"},
    {"telemetry.humidity",  "Humidity:"},
    {"telemetry.battery",   "Battery:"},
    {"telemetry.pressure",  "Pressure:"},

    /* End-of-table sentinel */
    {NULL, NULL}
};