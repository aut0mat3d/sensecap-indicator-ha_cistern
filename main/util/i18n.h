#ifndef I18N_H
#define I18N_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void i18n_init(void);
void i18n_set_language(const char *lang_code);
const char *i18n_get_current_language(void);

/**
 * @brief Lookup a localized string with a 5-step fallback cascade.
 * @param page_id Page identifier (e.g., "cistern_view", "global")
 * @param key String key (e.g., "lbl_volume", "lbl_rate")
 * @return Localized string, or key fallback if missing.
 */
const char *i18n_get(const char *page_id, const char *key);

#ifdef __cplusplus
}
#endif

#endif /* I18N_H */