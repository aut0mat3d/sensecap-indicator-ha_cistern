/**
 * @file gui_i18n.h
 * @brief Cascading Internationalization (i18n) Engine for SenseCAP Indicator GUI.
 *        Supports Flash-resident base languages (EN/DE) with dynamic SD-Card JSON overrides.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef GUI_I18N_H
#define GUI_I18N_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GUI_LANG_EN = 0,
    GUI_LANG_DE,
    GUI_LANG_CUSTOM
} gui_lang_t;

typedef struct {
    const char *key;
    const char *val;
} i18n_entry_t;

/**
 * @brief Initialize the i18n engine and internal mutexes.
 */
void gui_i18n_init(void);

/**
 * @brief Reload language dictionary from SD-Card for the given ISO language code.
 *        Requests /gui/lang/<lang_code>/common.json and alarm_config.json via RP2040 UART.
 * @param lang_code Language code string (e.g. "de", "en", "cz").
 * @return Total number of keys loaded from SD-Card.
 */
int gui_i18n_reload_from_sd(const char *lang_code);

/**
 * @brief Set the active runtime display language.
 * @param lang Language enum (GUI_LANG_EN, GUI_LANG_DE, or GUI_LANG_CUSTOM).
 */
void gui_i18n_set_language(gui_lang_t lang);

/**
 * @brief Get the currently active language.
 * @return Active language enum.
 */
gui_lang_t gui_i18n_get_language(void);

/**
 * @brief Resolve a localized text string by key using the cascade:
 *        1. SD-Card RAM Overrides -> 2. Target Builtin (e.g. DE) -> 3. Fallback Builtin (EN) -> 4. Key itself.
 * @param key Dot-notated dictionary key (e.g. "common.back", "telemetry.level").
 * @return Pointer to localized string (never returns NULL).
 */
const char* gui_i18n_get(const char *key);

/**
 * @brief Parse and register JSON language overrides into RAM.
 *        Flattens JSON trees into dot-notated keys (e.g. {"common": {"back": "Zurück"}} -> "common.back").
 * @param json_str Null-terminated JSON buffer loaded from SD-Card.
 * @return Number of imported keys, or -1 on parse failure.
 */
int gui_i18n_load_sd_overrides(const char *json_str);

/**
 * @brief Clear all dynamic SD-Card overrides from RAM.
 */
void gui_i18n_clear_sd_overrides(void);

#ifdef __cplusplus
}
#endif

#endif /* GUI_I18N_H */