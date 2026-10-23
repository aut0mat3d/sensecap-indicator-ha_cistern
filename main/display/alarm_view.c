/**
 * @file alarm_view.c
 * @brief Alarm view UI implementation for LVGL
 *
 * @copyright © 2026, SenseCAP Cistern Extension
 */

#include "alarm_view.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lv_port.h"
#include "nav.h"
#include "gui_header.h"
#include "alarm_model.h"

LV_FONT_DECLARE(ui_font_font0);

static const char *TAG = "alarm-view";

// ============================================================================
// UI Widgets & State
// ============================================================================

// Navigation tile index for alarm view (must match nav.h)
#define NAV_TILE_ALARM  4

// Current sort and filter state
static alarm_sort_mode_t s_current_sort_mode = ALARM_SORT_BY_TIME_DESC;
static alarm_filter_t s_current_filter = ALARM_FILTER_ALL;
static bool s_group_by_source = false;

// UI Widgets
static lv_obj_t *s_tile = NULL;
static lv_obj_t *s_list_alarms = NULL;
static lv_obj_t *s_btn_mute = NULL;
static lv_obj_t *s_btn_acknowledge = NULL;
static lv_obj_t *s_lbl_sort_type = NULL;
static lv_obj_t *s_lbl_sort_time = NULL;
static lv_obj_t *s_lbl_sort_content = NULL;

// ============================================================================
// Unicode Icons for Alarm States and Severity
// ============================================================================

// Severity icons (LVGL symbols)
#define ICON_DANGER   LV_SYMBOL_WARNING
#define ICON_WARNING  LV_SYMBOL_WARNING
#define ICON_INFO     LV_SYMBOL_BULLET

// State indicators
#define ICON_INCOMING     "→"
#define ICON_GOING        "✓"
#define ICON_MUTED        "M"
#define ICON_RETRIGGERED  "↻"
#define ICON_ACKNOWLEDGED "✓"

// ============================================================================
// Color Helpers
// ============================================================================

/**
 * @brief Get LVGL color for severity
 */
static lv_color_t _get_severity_color(alarm_severity_t severity) {
    const alarm_ui_config_t* config = alarm_get_ui_config();
    
    switch (severity) {
        case ALARM_SEVERITY_DANGER: return lv_color_hex(config->colors.color_danger);
        case ALARM_SEVERITY_WARNING: return lv_color_hex(config->colors.color_warning);
        case ALARM_SEVERITY_INFO: return lv_color_hex(config->colors.color_info);
        default: return lv_color_hex(0xFFFFFF); // White
    }
}

/**
 * @brief Get LVGL color for state
 */
static lv_color_t _get_state_color(alarm_state_t state, alarm_severity_t severity) {
    const alarm_ui_config_t* config = alarm_get_ui_config();
    
    switch (state) {
        case ALARM_STATE_INCOMING: return lv_color_hex(config->colors.color_incoming);
        case ALARM_STATE_GOING: return lv_color_hex(config->colors.color_going);
        case ALARM_STATE_MUTED: return lv_color_hex(config->colors.color_muted);
        case ALARM_STATE_RETRIGGERED: return lv_color_hex(config->colors.color_retriggered);
        case ALARM_STATE_ACKNOWLEDGED: return lv_color_hex(config->colors.color_acknowledged);
        default: return _get_severity_color(severity);
    }
}

/**
 * @brief Get icon for severity
 */
static const char* _get_severity_icon(alarm_severity_t severity) {
    switch (severity) {
        case ALARM_SEVERITY_DANGER: return ICON_DANGER;
        case ALARM_SEVERITY_WARNING: return ICON_WARNING;
        case ALARM_SEVERITY_INFO: return ICON_INFO;
        default: return "";
    }
}

/**
 * @brief Get state text
 */
static const char* _get_state_text(alarm_state_t state) {
    const alarm_ui_config_t* config = alarm_get_ui_config();
    
    switch (state) {
        case ALARM_STATE_INCOMING: return config->texts.incoming;
        case ALARM_STATE_GOING: return config->texts.going;
        case ALARM_STATE_MUTED: return config->texts.muted;
        case ALARM_STATE_RETRIGGERED: return config->texts.retriggered;
        case ALARM_STATE_ACKNOWLEDGED: return config->texts.acknowledged;
        default: return "";
    }
}

/**
 * @brief Get severity text
 */
static const char* _get_severity_text(alarm_severity_t severity) {
    const alarm_ui_config_t* config = alarm_get_ui_config();
    
    switch (severity) {
        case ALARM_SEVERITY_DANGER: return config->texts.danger;
        case ALARM_SEVERITY_WARNING: return config->texts.warning;
        case ALARM_SEVERITY_INFO: return config->texts.info;
        default: return "";
    }
}

// ============================================================================
// Date/Time Formatting
// ============================================================================

/**
 * @brief Format timestamp as YYMMDD-HHMM
 */
static void _format_timestamp(uint32_t timestamp, char* buf, size_t buf_size) {
    struct tm timeinfo;
    time_t rawtime = (time_t)timestamp;
    
    // Convert to local time
    localtime_r(&rawtime, &timeinfo);
    
    // Format as YYMMDD-HHMM
    snprintf(buf, buf_size, "%02d%02d%02d-%02d%02d",
             timeinfo.tm_year % 100, // Year (last 2 digits)
             timeinfo.tm_mon + 1,   // Month (1-12)
             timeinfo.tm_mday,      // Day
             timeinfo.tm_hour,      // Hour
             timeinfo.tm_min);      // Minute
}

// ============================================================================
// Callback Functions
// ============================================================================

/**
 * @brief Callback for back button (slot 2)
 */
static void _on_back_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Back button clicked");

        // Restore Header for Main view
        gui_header_clear();
        gui_header_set_title("Zisterne Hochbehälter");

        // Go back to main view
        nav_go_tile(NAV_TILE_CISTERN_DEMO);
    }
}

/**
 * @brief Callback for mute button (slot 3)
 */
static void _on_mute_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Mute button clicked");
        alarm_view_mute_2h();
        alarm_view_update_list();
    }
}

/**
 * @brief Callback for acknowledge button (slot 4)
 */
static void _on_acknowledge_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Acknowledge all button clicked");
        alarm_view_acknowledge_all();
        alarm_view_update_list();
    }
}

/**
 * @brief Callback for type header (cycle filter)
 */
static void _on_type_header_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Type header clicked");
        // Cycle through filter modes
        s_current_filter = (s_current_filter + 1) % (ALARM_FILTER_INFO + 1);
        alarm_view_update_list();
    }
}

/**
 * @brief Callback for time header (reverse sort order)
 */
static void _on_time_header_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Time header clicked");
        // Toggle between ascending and descending
        if (s_current_sort_mode == ALARM_SORT_BY_TIME_DESC) {
            s_current_sort_mode = ALARM_SORT_BY_TIME_ASC;
        } else if (s_current_sort_mode == ALARM_SORT_BY_TIME_ASC) {
            s_current_sort_mode = ALARM_SORT_BY_TIME_DESC;
        } else {
            s_current_sort_mode = ALARM_SORT_BY_TIME_DESC; // Default to newest first
        }
        alarm_view_update_list();
    }
}

/**
 * @brief Callback for content header (toggle grouping)
 */
static void _on_content_header_clicked(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ESP_LOGI(TAG, "Content header clicked");
        s_group_by_source = !s_group_by_source;
        alarm_view_update_list();
    }
}

// ============================================================================
// Alarm List Management
// ============================================================================

/**
 * @brief Create a list item for an alarm entry
 * @param alarm Alarm entry to display
 * @return LVGL object for the list item
 */
static lv_obj_t* _create_alarm_list_item(const alarm_entry_t* alarm) {
    if (!alarm) return NULL;

    // Create container for the alarm item
    lv_obj_t* item = lv_obj_create(s_list_alarms);
    lv_obj_set_size(item, LV_PCT(100), 56); // Fixed height for alarm items
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(item, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x282828), LV_PART_MAIN);
    lv_obj_set_style_border_width(item, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(item, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_pad_all(item, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_column(item, 6, LV_PART_MAIN);

    // Determine colors based on state and severity
    lv_color_t severity_color = _get_severity_color(alarm->severity);
    lv_color_t state_color = _get_state_color(alarm->state, alarm->severity);
    const char* severity_icon = _get_severity_icon(alarm->severity);

    // Create severity icon
    lv_obj_t* lbl_icon = lv_label_create(item);
    lv_label_set_text(lbl_icon, severity_icon);
    lv_obj_set_style_text_font(lbl_icon, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_icon, severity_color, LV_PART_MAIN);
    lv_obj_set_pos(lbl_icon, 5, 5);
    lv_obj_set_width(lbl_icon, 30);

    // Format timestamp
    char time_buf[16];
    _format_timestamp(alarm->timestamp, time_buf, sizeof(time_buf));

    // Create timestamp label
    lv_obj_t* lbl_time = lv_label_create(item);
    lv_label_set_text(lbl_time, time_buf);
    lv_obj_set_style_text_font(lbl_time, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(lbl_time, 40, 5);
    lv_obj_set_width(lbl_time, 90);

    // Create source and message label (may wrap to two lines)
    lv_obj_t* lbl_source_msg = lv_label_create(item);
    char source_msg[256];
    
    // Check if we need to wrap to two lines
    size_t total_len = strlen(alarm->source) + strlen(alarm->message) + 2; // +2 for ": "
    if (total_len > 35) {
        // Two-line format
        snprintf(source_msg, sizeof(source_msg), "%s:\n%s", alarm->source, alarm->message);
    } else {
        // Single line format
        snprintf(source_msg, sizeof(source_msg), "%s: %s", alarm->source, alarm->message);
    }
    
    lv_label_set_text(lbl_source_msg, source_msg);
    lv_obj_set_style_text_font(lbl_source_msg, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_source_msg, lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    lv_obj_set_pos(lbl_source_msg, 135, 5);
    lv_obj_set_width(lbl_source_msg, 250);
    lv_label_set_long_mode(lbl_source_msg, LV_LABEL_LONG_WRAP);

    // Create state label
    lv_obj_t* lbl_state = lv_label_create(item);
    const char* state_text = _get_state_text(alarm->state);
    lv_label_set_text(lbl_state, state_text);
    lv_obj_set_style_text_font(lbl_state, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_state, state_color, LV_PART_MAIN);
    lv_obj_set_pos(lbl_state, 395, 5);
    lv_obj_set_width(lbl_state, 80);

    return item;
}

/**
 * @brief Create header row for alarm list
 */
static void _create_alarm_list_header(void) {
    // Create header container
    lv_obj_t* header = lv_obj_create(s_list_alarms);
    lv_obj_set_size(header, LV_PCT(100), 38);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(header, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(header, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_column(header, 8, LV_PART_MAIN);

    // Type header
    s_lbl_sort_type = lv_label_create(header);
    lv_label_set_text(s_lbl_sort_type, "Type");
    lv_obj_set_style_text_font(s_lbl_sort_type, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_sort_type, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_sort_type, 5, 5);
    lv_obj_set_width(s_lbl_sort_type, 30);
    lv_obj_add_event_cb(s_lbl_sort_type, _on_type_header_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_lbl_sort_type, LV_OBJ_FLAG_CLICKABLE);

    // Time header
    s_lbl_sort_time = lv_label_create(header);
    lv_label_set_text(s_lbl_sort_time, "Time ↓");
    lv_obj_set_style_text_font(s_lbl_sort_time, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_sort_time, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_sort_time, 40, 5);
    lv_obj_set_width(s_lbl_sort_time, 90);
    lv_obj_add_event_cb(s_lbl_sort_time, _on_time_header_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_lbl_sort_time, LV_OBJ_FLAG_CLICKABLE);

    // Content header
    s_lbl_sort_content = lv_label_create(header);
    lv_label_set_text(s_lbl_sort_content, "Content");
    lv_obj_set_style_text_font(s_lbl_sort_content, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_sort_content, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(s_lbl_sort_content, 135, 5);
    lv_obj_set_width(s_lbl_sort_content, 250);
    lv_obj_add_event_cb(s_lbl_sort_content, _on_content_header_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_lbl_sort_content, LV_OBJ_FLAG_CLICKABLE);

    // State header
    lv_obj_t* lbl_state_header = lv_label_create(header);
    lv_label_set_text(lbl_state_header, "State");
    lv_obj_set_style_text_font(lbl_state_header, &ui_font_font0, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_state_header, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(lbl_state_header, 395, 5);
    lv_obj_set_width(lbl_state_header, 80);
}

/**
 * @brief Update the filter/type header text
 */
static void _update_header_texts(void) {
    if (!s_lbl_sort_type || !s_lbl_sort_time || !s_lbl_sort_content) return;

    // Update type header based on current filter
    const char* filter_text = "Type";
    switch (s_current_filter) {
        case ALARM_FILTER_ALL: filter_text = "All"; break;
        case ALARM_FILTER_DANGER: filter_text = "Danger"; break;
        case ALARM_FILTER_WARNING: filter_text = "Warning"; break;
        case ALARM_FILTER_INFO: filter_text = "Info"; break;
    }
    lv_label_set_text(s_lbl_sort_type, filter_text);

    // Update time header based on current sort mode
    const char* time_text = "Time";
    if (s_current_sort_mode == ALARM_SORT_BY_TIME_ASC) {
        time_text = "Time ↑";
    } else if (s_current_sort_mode == ALARM_SORT_BY_TIME_DESC) {
        time_text = "Time ↓";
    }
    lv_label_set_text(s_lbl_sort_time, time_text);

    // Update content header based on grouping
    const char* content_text = s_group_by_source ? "Grouped" : "Ungrouped";
    lv_label_set_text(s_lbl_sort_content, content_text);
}

/**
 * @brief Update mute button text based on mute state
 */
static void _update_mute_button(void) {
    if (alarm_is_muted()) {
        gui_header_set_slot(HEADER_SLOT_RIGHT_2, "Muted", NULL, NULL);
    } else {
        gui_header_set_slot(HEADER_SLOT_RIGHT_2, "Mute (2h)", _on_mute_clicked, NULL);
    }
}

// ============================================================================
// Public API Implementation
// ============================================================================

void alarm_view_set_sort_mode(alarm_sort_mode_t sort_mode) {
    s_current_sort_mode = sort_mode;
    alarm_view_update_list();
}

void alarm_view_set_filter_mode(alarm_filter_t filter) {
    s_current_filter = filter;
    alarm_view_update_list();
}

void alarm_view_toggle_sort_order(void) {
    if (s_current_sort_mode == ALARM_SORT_BY_TIME_DESC) {
        s_current_sort_mode = ALARM_SORT_BY_TIME_ASC;
    } else {
        s_current_sort_mode = ALARM_SORT_BY_TIME_DESC;
    }
    alarm_view_update_list();
}

void alarm_view_toggle_grouping(void) {
    s_group_by_source = !s_group_by_source;
    alarm_view_update_list();
}

void alarm_view_acknowledge_all(void) {
    size_t count = 0;
    alarm_entry_t* alarms = alarm_get_ringbuffer(&count);
    
    if (alarms) {
        for (size_t i = 0; i < count; i++) {
            if (alarms[i].state == ALARM_STATE_INCOMING || 
                alarms[i].state == ALARM_STATE_RETRIGGERED) {
                alarm_set_state(&alarms[i], ALARM_STATE_ACKNOWLEDGED);
                alarm_acknowledge(&alarms[i]);
            }
        }
        free(alarms);
    }
    
    ESP_LOGI(TAG, "All alarms acknowledged");
    alarm_buzzer_stop();
}

void alarm_view_mute_2h(void) {
    alarm_mute(2 * 3600); // 2 hours in seconds
    ESP_LOGI(TAG, "Alarms muted for 2 hours");
    alarm_buzzer_stop();
}

void alarm_view_handle_hw_button(void) {
    // Acknowledge all alarms on hardware button press
    alarm_view_acknowledge_all();
    alarm_view_update_list();
}

void alarm_view_update_list(void) {
    lv_port_sem_take();

    // Update header texts
    _update_header_texts();
    _update_mute_button();

    // Clear existing alarm items (keep the list header)
    uint32_t child_count = lv_obj_get_child_count(s_list_alarms);
    int32_t i = (int32_t)child_count;
    while (i > 1) { // Keep the header (first child at index 0)
        i--;
        lv_obj_t* child = lv_obj_get_child(s_list_alarms, i);
        if (child) {
            lv_obj_delete(child);
        }
    }

    // Get alarm list with current sort and filter
    size_t count = 0;
    alarm_entry_t* alarms = alarm_get_list(s_current_sort_mode, s_current_filter, &count);
    
    if (!alarms || count == 0) {
        // Show "No alarms" message
        lv_obj_t* no_alarms = lv_label_create(s_list_alarms);
        lv_label_set_text(no_alarms, "No alarms found");
        lv_obj_set_style_text_font(no_alarms, &ui_font_font0, LV_PART_MAIN);
        lv_obj_set_style_text_color(no_alarms, lv_color_hex(0x9E9E9E), LV_PART_MAIN);
        lv_obj_center(no_alarms);
        
        if (alarms) free(alarms);
        lv_port_sem_give();
        return;
    }

    // Create alarm items
    for (size_t i = 0; i < count; i++) {
        _create_alarm_list_item(&alarms[i]);
    }
    
    free(alarms);
    lv_port_sem_give();
    
    ESP_LOGI(TAG, "Updated alarm list with %d alarms", count);
}

void alarm_view_show(void) {
    lv_port_sem_take();

    /* 1. Header beim Anzeigen der Alarmseite aktivieren */
    gui_header_clear();
    gui_header_set_title("Alarme");
    gui_header_set_slot(HEADER_SLOT_LEFT, LV_SYMBOL_LEFT " Back", _on_back_clicked, NULL);
    gui_header_set_slot(HEADER_SLOT_RIGHT_2, "Mute (2h)", _on_mute_clicked, NULL);
    gui_header_set_slot(HEADER_SLOT_RIGHT_1, "✓ Quit", _on_acknowledge_clicked, NULL);
    gui_header_set_visible(true);

    nav_go_tile(NAV_TILE_ALARM);
    lv_port_sem_give();
    
    // Update the list when shown
    alarm_view_update_list();
}

int alarm_view_init(void) {
    lv_port_sem_take();

    // Get the alarm tile from navigation system
    s_tile = nav_get_tile(NAV_TILE_ALARM);
    if (!s_tile) {
        ESP_LOGE(TAG, "Failed to get alarm tile from navigation system");
        lv_port_sem_give();
        return -1;
    }

    /* 1. 
    gui_header_clear();
    gui_header_set_title("Alarme");
    gui_header_set_slot(HEADER_SLOT_LEFT, LV_SYMBOL_LEFT " Back", _on_back_clicked, NULL);
    gui_header_set_slot(HEADER_SLOT_RIGHT_2, "Mute (2h)", _on_mute_clicked, NULL);
    gui_header_set_slot(HEADER_SLOT_RIGHT_1, "✓ Quit", _on_acknowledge_clicked, NULL);
    gui_header_set_visible(true); */

    /* 2. Alarm List Container (Scrollable) */
    s_list_alarms = lv_obj_create(s_tile);
    lv_obj_set_size(s_list_alarms, LV_PCT(100), 416); // 480 - 64 (header)
    lv_obj_set_pos(s_list_alarms, 0, 64);
    lv_obj_set_style_radius(s_list_alarms, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_list_alarms, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_list_alarms, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_list_alarms, 0, LV_PART_MAIN);
    lv_obj_add_flag(s_list_alarms, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_list_alarms, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_dir(s_list_alarms, LV_DIR_VER);

    // Create header row for alarm list
    _create_alarm_list_header();

    // Initial state
    s_current_sort_mode = ALARM_SORT_BY_TIME_DESC;
    s_current_filter = ALARM_FILTER_ALL;
    s_group_by_source = false;

    lv_port_sem_give();

    ESP_LOGI(TAG, "Alarm View initialized successfully.");
    return 0;
}