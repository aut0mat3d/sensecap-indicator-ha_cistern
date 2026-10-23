/**
 * @file cistern_view.h
 * @brief Modular Cistern View screen using the 10x8 grid widget architecture.
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef CISTERN_VIEW_H
#define CISTERN_VIEW_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the cistern view inside a parent screen or tile.
 * @param parent Container (tileview page or screen).
 */
int cistern_view_init(void);

/**
 * @brief Destroy view objects and unregister event handlers.
 */
void cistern_view_destroy(void);

#ifdef __cplusplus
}
#endif

#endif /* CISTERN_VIEW_H */