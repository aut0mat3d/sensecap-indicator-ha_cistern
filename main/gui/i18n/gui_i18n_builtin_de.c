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

    /* End-of-table sentinel */
    {NULL, NULL}
};