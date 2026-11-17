#ifndef LV_PORT_H
#define LV_PORT_H

#include "lvgl.h"

/** Render diagnostics: frames rendered and render time (avg / max) per period in the log.
 *  Passive measurement via LVGL display events, no influence on timing. 0 = compiled out */
#ifndef LV_PORT_RENDER_DIAG
#define LV_PORT_RENDER_DIAG            1
#endif
#define LV_PORT_RENDER_DIAG_PERIOD_MS  30000

/** Skip rendering the active screen while an opaque full-screen modal on lv_layer_top() covers it.
 *  LVGL always renders the active screen first and the top layer on top of it, so without this
 *  every frame of a modal also re-rendered the complete page underneath. 0 = disabled */
#ifndef LV_PORT_HIDE_COVERED_SCREEN
#define LV_PORT_HIDE_COVERED_SCREEN    1
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize related work for lvgl.
 *
 */
void lv_port_init(void);

/**
 * @brief Take the semaphore.
 * @note  It should be called before manipulate lvgl gui.
 *
 */
void lv_port_sem_take(void);

/**
 * @brief Give the semaphore.
 * @note  It should be called after manipulate lvgl gui.
 *
 */
void lv_port_sem_give(void);

#ifdef __cplusplus
}
#endif

#endif
