/**
 * @file input_dialog.h
 * @brief Universal modal touch input dialog for numeric values (float/int) and text/passwords.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef INPUT_DIALOG_H
#define INPUT_DIALOG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    INPUT_DIALOG_MODE_FLOAT = 0,
    INPUT_DIALOG_MODE_INT,
    INPUT_DIALOG_MODE_TEXT,
    INPUT_DIALOG_MODE_PASSWORD
} input_dialog_mode_t;

/**
 * @brief Completion callback invoked when user confirms or cancels the dialog.
 *
 * @param confirmed True if confirmed with OK, false if cancelled.
 * @param f_val Resulting float value (for FLOAT/INT modes).
 * @param i_val Resulting integer value (for INT mode).
 * @param str_val Resulting text string (for all modes).
 * @param user_data Caller context pointer.
 */
typedef void (*input_dialog_cb_t)(bool confirmed, float f_val, int32_t i_val, const char *str_val, void *user_data);

typedef struct {
    const char          *title;        /* Dialog title (e.g. "Trockenlaufschutz") */
    input_dialog_mode_t  mode;         /* FLOAT, INT, TEXT, PASSWORD */
    float                initial_f;    /* Initial float value */
    int32_t              initial_i;    /* Initial integer value */
    const char          *initial_str;  /* Initial text (or NULL) */
    const char          *unit;         /* Unit badge string (e.g. "%", "cm", "m³/h") */
    float                min_val;      /* Minimum value limit (numeric) */
    float                max_val;      /* Maximum value limit (numeric) */
    uint16_t             max_len;      /* Maximum character length (default: 32) */
    input_dialog_cb_t    callback;     /* Result callback */
    void                *user_data;    /* Custom context passed to callback */
} input_dialog_config_t;

/**
 * @brief Open the modal input dialog on lv_layer_top().
 *
 * @param cfg Pointer to dialog configuration descriptor.
 * @return 0 on success, negative error code on failure.
 */
int input_dialog_open(const input_dialog_config_t *cfg);

/**
 * @brief Dismiss and destroy any currently active input dialog.
 */
void input_dialog_dismiss(void);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_DIALOG_H */