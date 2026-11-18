/**
 * @file gui_i18n_builtin_de.c
 * @brief Flash-resident German dictionary.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "gui_i18n.h"

const i18n_entry_t g_i18n_builtin_de[] = {
    /* Common UI Actions */
    {"common.back",         "Zurück"},
    {"common.cancel",       "Abbrechen"},
    {"common.save",         "Speichern"},
    {"common.delete",       "Löschen"},
    {"common.ok",           "OK"},
    {"common.close",        "Schließen"},
    {"common.quit",         "Quittieren"},
    {"common.mute",         "Stumm"},
    {"common.settings",     "Einstellungen"},
    {"common.refresh",      "Aktualisieren"},

    /* Status & Qualities */
    {"status.ok",           "Normal"},
    {"status.warning",      "Warnung"},
    {"status.danger",       "Gefahr"},
    {"status.info",         "Info"},
    {"status.incoming",     "Eingehend"},
    {"status.going",        "Gehend"},
    {"status.muted",        "Stumm"},
    {"status.retriggered",  "Wiederausgelöst"},
    {"status.acknowledged", "Quittiert"},
    {"status.estimated",    "Geschätzt"},
    {"status.offline",      "Offline"},

    /* Generic Telemetry Keys */
    {"telemetry.level",     "Füllstand:"},
    {"telemetry.rate",      "Änderungsrate:"},
    {"telemetry.updated",   "Aktualisierung:"},
    {"telemetry.volume",    "Volumen:"},
    {"telemetry.depth",     "Tiefe:"},
    {"telemetry.flow",      "Durchfluss:"},
    {"telemetry.temp",      "Temperatur:"},
    {"telemetry.humidity",  "Feuchtigkeit:"},
    {"telemetry.battery",   "Batterie:"},
    {"telemetry.pressure",  "Druck:"},

    /* Alarm View & Table Headers */
    {"alarm.title",         "Alarme"},
    {"alarm.no_alarms",     "Keine Alarme vorhanden"},
    {"alarm.hdr_type",      "Typ"},
    {"alarm.hdr_time",      "Zeit"},
    {"alarm.hdr_content",   "Inhalt"},
    {"alarm.hdr_state",     "Status"},
    {"alarm.filter_all",    "Alle"},
    {"alarm.grouped",       "Gruppiert"},
    {"alarm.ungrouped",     "Ungruppiert"},

    /* Alarm Configuration & Collapsible List */
    {"alarm_cfg.title",         "Alarmeinstellungen"},
    {"alarm_cfg.save_exit",     "<< Speichern"},
    {"alarm_cfg.exit_no_save",  "Abbrechen"},
    {"alarm_cfg.revert",        "Reset"},
    {"alarm_cfg.prop_val",      "Schwellwert:"},
    {"alarm_cfg.prop_hyst",     "Hysterese:"},
    {"alarm_cfg.prop_damp",     "Dämpfung:"},
    {"alarm_cfg.prop_prio",     "Priorität:"},
    {"alarm_cfg.prop_buzz",     "Summer-Modus:"},
    {"alarm_cfg.cycles",        "Zyklen"},
    {"alarm_cfg.intermittent",  "Intermittierend"},
    {"alarm_cfg.continuous",    "Dauerton"},
    {"alarm_cfg.off",           "Aus"},

    /* Channel / Key Names */
    {"channel.depth",           "Füllstand"},
    {"channel.flowrate",        "Durchfluss"},
    {"channel.volume",          "Volumen"},
    {"channel.level_gain",      "Änderungsrate"},

    /* Threshold Rule Titles */
    {"threshold.alarm_low",     "Trockenlaufschutz"},
    {"threshold.alarm_frate",   "Max. Durchfluss"},
    
    /* State Short Abbreviations */
    {"state_abbr.incoming",     "Eing."},
    {"state_abbr.going",        "Gehd."},
    {"state_abbr.muted",        "Stum."},
    {"state_abbr.retriggered",  "Retr."},
    {"state_abbr.acknowledged", "Quit."},

    /* Time units for Mute Button */
    {"common.unit_h",       "h"},
    {"common.unit_m",       "min"},
    {"common.reset",        "Reset"},

    /* Settings View */
    {"settings.wifi",       "WLAN"},
    {"settings.display",    "Display"},
    {"settings.sdcard",     "SD-Karte"},
    {"settings.broker",     "Broker"},
    {"settings.lora_diag",  "LoRa Diag"},
    {"settings.alarms",     "Alarme"},
    {"settings.ip_none",    "IP: nicht verbunden"},

    /* Data point detail page (key_detail_view) and trend graph presets */
    {"status.no_data",      "Keine Daten"},
    {"status.stale",        "Veraltet"},
    {"telemetry.battery_soc", "Batterie:"},
    {"detail.value",        "Aktueller Wert:"},
    {"detail.updated",      "Letzter Wert:"},
    {"detail.status",       "Status:"},
    {"detail.interval",     "Erwartetes Intervall:"},
    {"detail.device",       "Gerät:"},
    {"trend.24h",           "24 Std"},
    {"trend.7d",            "7 Tage"},
    {"trend.31d",           "31 Tage"},

    /* End-of-table sentinel */
    {NULL, NULL}
};