/**
 * @file alarm_view.h
 * @brief Alarm view UI implementation for LVGL
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#ifndef ALARM_VIEW_H
#define ALARM_VIEW_H

#include "lvgl.h"
#include "alarm_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize alarm view
 * @return 0 on success, -1 on failure
 */
int alarm_view_init(void);

/**
 * @brief Show alarm view
 */
void alarm_view_show(void);

/**
 * @brief Update alarm list display
 */
void alarm_view_update_list(void);

/**
 * @brief Set alarm sort mode
 * @param sort_mode Sort mode to use
 */
void alarm_view_set_sort_mode(alarm_sort_mode_t sort_mode);

/**
 * @brief Set alarm filter mode
 * @param filter Filter mode to use
 */
void alarm_view_set_filter_mode(alarm_filter_t filter);

/**
 * @brief Toggle sort order (ascending/descending for time)
 */
void alarm_view_toggle_sort_order(void);

/**
 * @brief Toggle grouping by source
 */
void alarm_view_toggle_grouping(void);

/**
 * @brief Acknowledge all alarms
 */
void alarm_view_acknowledge_all(void);

/**
 * @brief Mute alarms for 2 hours
 */
void alarm_view_mute_2h(void);

/**
 * @brief Handle hardware button press (acknowledge all)
 */
void alarm_view_handle_hw_button(void);

#ifdef __cplusplus
}
#endif

#endif /* ALARM_VIEW_H */