# Vertical Graph Module

The `vert_graph` module provides a configurable vertical bar widget for displaying sensor data such as tank levels, percentages, or any normalized value range. It integrates with the LVGL graphics library and supports dynamic scaling, alarm thresholds, and data quality visualization.

## Overview

The vertical graph displays a single value as a vertical bar with:
- **Percentage display** (centered label showing current fill percentage)
- **Scale labels** (minimum value at bottom, maximum value at top with units)
- **Alarm threshold marker** (visual indicator for configured alarm levels)
- **Data quality indicators** (visual styling for estimated or alarm states)
- **Click support** (optional callback for configuration subpages)

## Quick Start

### 1. Factory Method (Recommended for Page Manager)

The simplest way to create a vertical graph is using the factory function, which integrates with the `widget_base` system:

```c
#include "vert_graph.h"

/* Create via factory function */
widget_t* graph_widget = vert_graph_create(
    tile,                    // Parent LVGL object (tile or view container)
    "tank_level",           // Unique widget ID
    "depth",               // Data binding key
    20,                     // X position in pixels
    72,                     // Y position in pixels
    190,                    // Width in pixels
    256                     // Height in pixels
);

/* Later, update the value */
/* The widget_t can be cast back to vert_graph_t if needed */
```

### 2. Direct Initialization (Backward Compatible)

For direct control or when pre-allocating the structure:

```c
#include "vert_graph.h"

/* Declare and configure */
vert_graph_t my_graph;
my_graph.parent = tile;
my_graph.x = 20;
my_graph.y = 72;
my_graph.width = 190;
my_graph.height = 256;
my_graph.min_value = 0.0f;
my_graph.max_value = 5.0f;    // e.g., 5 m³
my_graph.current_value = 3.42f;
strcpy(my_graph.unit, "m³");

/* Initialize */
if (vert_graph_init(&my_graph) != 0) {
    // Handle error
}

/* Update value */
vert_graph_update(&my_graph, 4.2f);

/* Cleanup when done */
vert_graph_destroy(&my_graph);
```

## Configuration Structure

The `vert_graph_t` structure contains all configurable parameters:

```c
typedef struct {
    /* Position & Size */
    lv_obj_t* parent;           // Parent LVGL container
    int16_t x;                  // X position (pixels)
    int16_t y;                  // Y position (pixels)
    uint16_t width;            // Width (pixels)
    uint16_t height;           // Height (pixels)
    
    /* Display */
    const char* title;         // Optional title text
    char unit[16];             // Unit string (e.g., "m³", "cm", "%")
    float min_value;           // Minimum scale value
    float max_value;           // Maximum scale value
    float current_value;       // Initial/current displayed value
    
    /* Styling */
    const lv_font_t* font;     // Font for labels (default: ui_font_font0)
    lv_color_t bg_color;       // Card background color (default: 0x282828)
    lv_color_t bar_color;      // Bar indicator color (default: 0x3498db)
    lv_color_t text_color;     // Value text color (default: white)
    lv_color_t alarm_color;    // Alarm marker/threshold color (default: 0xE74C3C)
    
    /* Alarm Levels */
    float alarm_level_min;     // Min alarm threshold (0.0f = disabled)
    float alarm_level_max;     // Max alarm threshold (0.0f = disabled)
    float hysteresis_min;      // Hysteresis for min alarm
    float hysteresis_max;      // Hysteresis for max alarm
    
    /* Data Quality */
    bool is_estimated;         // true = value is interpolated/estimated
    bool is_alarm;             // true = active alarm condition
    
    /* Callback */
    lv_event_cb_t click_cb;    // Click handler for config subpage
    void* user_data;           // User data for callback
    
    /* Internal - Do not modify directly */
    lv_obj_t* container;       // Root card container
    lv_obj_t* bar;             // LVGL bar widget
    lv_obj_t* lbl_percent;     // Center percentage label
    lv_obj_t* lbl_max;         // Top scale label
    lv_obj_t* lbl_min;         // Bottom scale label
    lv_obj_t* alarm_marker;    // Alarm threshold marker
    uint16_t bar_height;       // Cached bar height
} vert_graph_t;
```

## API Reference

### Initialization

| Function | Description |
|----------|-------------|
| `widget_t* vert_graph_create(lv_obj_t* parent, const char* id, const char* binding_key, int16_t x, int16_t y, uint16_t w, uint16_t h)` | Factory function for widget_base integration. Returns `widget_t*` or NULL on failure. |
| `int vert_graph_init(vert_graph_t* graph)` | Initialize a pre-allocated structure. Returns 0 on success, -1 on error. |

### Data Updates

| Function | Description |
|----------|-------------|
| `void vert_graph_update(vert_graph_t* graph, float value)` | Update with new value (keeps existing unit, status flags) |
| `void vert_graph_update_value(vert_graph_t* graph, float value, const char* unit, bool is_estimated, bool is_alarm)` | Full update with value, unit, and quality flags |
| `void vert_graph_set_status(vert_graph_t* graph, bool is_estimated, bool is_alarm)` | Update only the quality status flags |

### Configuration

| Function | Description |
|----------|-------------|
| `void vert_graph_set_range(vert_graph_t* graph, float min_val, float max_val)` | Dynamically set scale range and refresh labels |
| `void vert_graph_set_media_color(vert_graph_t* graph, lv_color_t color)` | Set the bar accent color |
| `void vert_graph_set_alarm_levels(vert_graph_t* graph, float min_alarm, float max_alarm, float hysteresis_min, float hysteresis_max)` | Configure alarm thresholds |

### Cleanup

| Function | Description |
|----------|-------------|
| `void vert_graph_destroy(vert_graph_t* graph)` | Clean up all LVGL objects and free resources |

## Visual States

The vertical graph supports three distinct visual states based on data quality:

### Normal State
- **Bar**: 100% opacity, uses `bar_color`
- **Border**: None
- **Appearance**: Solid bar indicating valid, real-time data

### Estimated State (`is_estimated = true`)
- **Bar**: 50% opacity, uses `bar_color`
- **Border**: 2px light grey (0xBDC3C7) outline
- **Appearance**: Semi-transparent bar with outline, indicating interpolated/replacement value

### Alarm State (`is_alarm = true`)
- **Bar**: 100% opacity, uses `alarm_color` (default: red 0xE74C3C)
- **Border**: 2px white outline
- **Appearance**: Red bar with white border, indicating active threshold violation

## Alarm Threshold Marker

When alarm levels are configured, a white horizontal marker line appears on the bar at the threshold position:

```c
/* Configure alarm at 20% of range */
vert_graph_set_alarm_levels(&my_graph, 
    0.0f,   // min_alarm (disabled)
    1.0f,   // max_alarm = 1.0 in a 0-5 range = 20%
    0.0f,   // hysteresis_min
    0.0f    // hysteresis_max
);
```

The marker is automatically hidden when:
- Both `alarm_level_min` and `alarm_level_max` are ≤ `min_value`
- The alarm target is outside the configured range

## Complete Example

```c
#include "vert_graph.h"
#include "lv_port.h"

/* Create a tank level indicator */
vert_graph_t tank_graph;

void setup_tank_graph(lv_obj_t* tile) {
    memset(&tank_graph, 0, sizeof(tank_graph));
    
    tank_graph.parent = tile;
    tank_graph.x = 20;
    tank_graph.y = 72;
    tank_graph.width = 190;
    tank_graph.height = 256;
    tank_graph.min_value = 0.0f;
    tank_graph.max_value = 5.0f;    // 5 m³ capacity
    tank_graph.current_value = 3.42f;
    tank_graph.bar_color = lv_color_hex(0x3498db);  // Blue
    tank_graph.alarm_color = lv_color_hex(0xE74C3C); // Red
    strcpy(tank_graph.unit, "m³");
    
    /* Set alarm at 1 m³ (20%) */
    tank_graph.alarm_level_min = 1.0f;
    tank_graph.hysteresis_min = 0.1f;
    
    /* Optional: Click handler for configuration */
    tank_graph.click_cb = on_tank_config_clicked;
    
    vert_graph_init(&tank_graph);
}

void update_tank_level(float new_level) {
    /* Simple update */
    vert_graph_update(&tank_graph, new_level);
    
    /* Or with quality flags: */
    // bool estimated = check_if_estimated(new_level);
    // bool in_alarm = (new_level < 1.0f);
    // vert_graph_update_value(&tank_graph, new_level, "m³", estimated, in_alarm);
}

void on_tank_config_clicked(lv_event_t *e) {
    /* Open configuration subpage */
    config_view_show(&tank_graph);
}
```

## Default Values

If not specified, the module uses these defaults:

| Parameter | Default Value |
|-----------|---------------|
| width | 190 pixels |
| height | 256 pixels |
| font | ui_font_font0 |
| bg_color | 0x282828 (dark grey) |
| bar_color | 0x3498db (blue) |
| text_color | 0xFFFFFF (white) |
| alarm_color | 0xE74C3C (red) |
| max_value | 100.0f |

## Thread Safety

All public API functions are **thread-safe** and use `lv_port_sem_take()` / `lv_port_sem_give()` internally to protect LVGL operations. Do not call LVGL functions directly on the graph's internal objects.

## Integration with widget_base

The module supports the `widget_base.h` interface for dynamic page management:

```c
/* The factory function returns a widget_t* that can be stored in a page's widget list */
widget_t* graph = vert_graph_create(parent, "id", "binding", x, y, w, h);

/* Update via widget interface */
if (graph->update_val) {
    graph->update_val(graph, new_value, "m³");
}

/* Destroy via widget interface */
if (graph->destroy) {
    graph->destroy(graph);
}
```

The widget's `user_data` field contains the `vert_graph_t*` for direct access if needed.

## Use Cases

- **Tank Level Monitoring**: Display fill level with alarm thresholds
- **Battery Status**: Show battery percentage with low-battery warning
- **Sensor Readings**: Any normalized value (0-100% or custom range)
- **Temperature**: Display with min/max warnings
- **Pressure**: Visual gauge with configurable ranges

## Notes

- The bar uses LVGL's built-in `lv_bar` widget internally
- Percentage labels show one decimal place (e.g., "68.4 %")
- Scale labels show two decimal places with unit (e.g., "5.00 m³")
- The alarm marker is a 3px high horizontal line inside the bar
- All color values support LVGL's color formats (16-bit, 32-bit, true color)
