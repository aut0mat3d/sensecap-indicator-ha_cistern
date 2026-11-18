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

    /* Alarm View & Table Headers */
    {"alarm.title",         "Alarms"},
    {"alarm.no_alarms",     "No alarms found"},
    {"alarm.hdr_type",      "Type"},
    {"alarm.hdr_time",      "Time"},
    {"alarm.hdr_content",   "Content"},
    {"alarm.hdr_state",     "State"},
    {"alarm.filter_all",    "All"},
    {"alarm.grouped",       "Grouped"},
    {"alarm.ungrouped",     "Ungrouped"},

    /* Alarm Configuration & Collapsible List */
    {"alarm_cfg.title",         "Alarm Configuration"},
    {"alarm_cfg.save_exit",     "<< Save"},
    {"alarm_cfg.exit_no_save",  "Cancel"},
    {"alarm_cfg.revert",        "Reset"},
    {"alarm_cfg.prop_val",      "Threshold:"},
    {"alarm_cfg.prop_hyst",     "Hysteresis:"},
    {"alarm_cfg.prop_damp",     "Damping:"},
    {"alarm_cfg.prop_prio",     "Severity:"},
    {"alarm_cfg.prop_buzz",     "Buzzer Mode:"},
    {"alarm_cfg.cycles",        "cycles"},
    {"alarm_cfg.intermittent",  "Intermittent"},
    {"alarm_cfg.continuous",    "Continuous"},
    {"alarm_cfg.off",           "Off"},

    /* Channel / Key Names */
    {"channel.depth",           "Depth"},
    {"channel.flowrate",        "Flow Rate"},
    {"channel.volume",          "Volume"},
    {"channel.level_gain",      "Rate of Change"},

    /* Threshold Rule Titles */
    {"threshold.alarm_low",     "Dry Run Protection"},
    {"threshold.alarm_frate",   "Max Flow Rate"},

    /* State Short Abbreviations */
    {"state_abbr.incoming",     "In"},
    {"state_abbr.going",        "Out"},
    {"state_abbr.muted",        "Mute"},
    {"state_abbr.retriggered",  "Retr"},
    {"state_abbr.acknowledged", "Ack"},

    /* Time units for Mute Button */
    {"common.unit_h",       "h"},
    {"common.unit_m",       "min"},
    {"common.reset",        "Reset"},

    /* Settings View */
    {"settings.wifi",       "WiFi"},
    {"settings.display",    "Display"},
    {"settings.sdcard",     "SD Card"},
    {"settings.broker",     "Broker"},
    {"settings.lora_diag",  "LoRa Diag"},
    {"settings.alarms",     "Alarms"},
    {"settings.ip_none",    "IP: not connected"},

    /* Data point detail page (key_detail_view) and trend graph presets */
    {"status.no_data",      "No data"},
    {"status.stale",        "Stale"},
    {"telemetry.battery_soc", "Battery:"},
    {"mqtt.alarm",          "Alarm"},
    {"detail.value",        "Current value:"},
    {"detail.updated",      "Last value:"},
    {"detail.status",       "Status:"},
    {"detail.interval",     "Expected interval:"},
    {"detail.device",       "Device:"},
    {"trend.24h",           "24 h"},
    {"trend.7d",            "7 days"},
    {"trend.31d",           "31 days"},

    /* End-of-table sentinel */
    {NULL, NULL}
};