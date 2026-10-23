# Horizontal Bar Graph Module

The `horiz_bargraph` module provides a configurable horizontal chart widget for displaying historical time-series data. It supports multiple chart types (bar, line, point), per-data-point quality indicators, and X-axis time labels. The module uses LVGL 9's draw task interception to render individual bar/line colors based on data quality (normal, estimated, alarm).

## Overview

The horizontal bar graph displays a series of historical data points with:
- **Multiple chart types**: Bar, line, or point scatter
- **Per-point quality indicators**: Visual styling for estimated or alarm states
- **X-axis labels**: Time or category labels (e.g., "-8h", "-1h", "Now")
- **Y-axis scaling**: Configurable minimum and maximum values
- **Click support**: Optional callback for configuration subpages
- **LVGL 9 draw task interception**: Individual bar coloring without full redraw

## Quick Start

### 1. Factory Method (Recommended for Page Manager)

```c
#include "horiz_bargraph.h"

/* Create via factory function */
widget_t* graph_widget = horiz_bargraph_create(
    tile,                    // Parent LVGL object (tile or view container)
    "history_chart",        // Unique widget ID
    "05975dc9.EM500-SWL.depth", // Data binding key
    20,                     // X position in pixels
    338,                    // Y position in pixels
    440,                    // Width in pixels
    128                     // Height in pixels
);

/* Update with new data */
/* The widget_t can be cast back to horiz_bargraph_t if needed */
```

### 2. Direct Initialization (Backward Compatible)

```c
#include "horiz_bargraph.h"

/* Declare and configure */
horiz_bargraph_t my_graph;
my_graph.parent = tile;
my_graph.x = 20;
my_graph.y = 338;
my_graph.width = 440;
my_graph.height = 128;
my_graph.min_value = 0.0f;
my_graph.max_value = 100.0f;
strcpy(my_graph.unit, "%");

/* Set chart type */
my_graph.chart_type = HORIZ_BARGRAPH_TYPE_BAR;

/* Set X-axis labels */
const char* labels[] = {"-8h", "-4h", "-2h", "Now"};
my_graph.axis_label_count = 4;
for (int i = 0; i < 4; i++) {
    my_graph.axis_labels[i] = labels[i];
}

/* Initialize */
if (horiz_bargraph_init(&my_graph) != 0) {
    // Handle error
}

/* Update with data */
float values[] = {20.0f, 45.0f, 67.0f, 85.0f};
horiz_bargraph_update_data(&my_graph, values, 4);

/* Cleanup when done */
horiz_bargraph_destroy(&my_graph);
```

## Configuration Structure

The `horiz_bargraph_t` structure contains all configurable parameters:

```c
typedef struct {
    /* Position & Size */
    lv_obj_t* parent;           // Parent LVGL container
    int16_t x;                  // X position (pixels)
    int16_t y;                  // Y position (pixels)
    uint16_t width;             // Width (pixels)
    uint16_t height;            // Height (pixels)
    
    /* Display */
    const char* title;         // Optional title text (top-left)
    char unit[16];             // Engineering unit (e.g., "m³", "cm", "%")
    horiz_bargraph_type_t chart_type; // BAR, LINE, or POINT
    
    /* Data Series */
    float data[HORIZ_BARGRAPH_MAX_POINTS];           // Array of data values
    bool  is_estimated[HORIZ_BARGRAPH_MAX_POINTS];   // Per-point: true if estimated
    bool  is_alarm[HORIZ_BARGRAPH_MAX_POINTS];       // Per-point: true if alarm
    uint8_t data_count;                              // Number of valid data points
    float min_value;                                 // Minimum Y-axis value
    float max_value;                                 // Maximum Y-axis value
    
    /* X-Axis Labels */
    const char* axis_labels[HORIZ_BARGRAPH_MAX_LABELS]; // X-axis label strings
    uint8_t axis_label_count;                           // Number of labels
    
    /* Styling */
    const lv_font_t* font;      // Font for labels (default: ui_font_font0)
    lv_color_t bg_color;        // Card background color (default: 0x282828)
    lv_color_t bar_color;       // Default bar/line color (default: 0x3498db)
    lv_color_t text_color;      // Axis label text color (default: 0x9E9E9E)
    lv_color_t alarm_color;     // Alarm bar color (default: 0xE74C3C)
    
    /* Callback */
    lv_event_cb_t click_cb;     // Click handler for config subpage
    void* user_data;            // User data for callback
    
    /* Internal - Do not modify directly */
    lv_obj_t* container;        // Root card container
    lv_obj_t* lbl_title;        // Title label object
    lv_obj_t* chart;            // LVGL chart widget
    lv_chart_series_t* series;  // Primary data series
    lv_obj_t* axis_labels_obj[HORIZ_BARGRAPH_MAX_LABELS]; // X-axis label objects
} horiz_bargraph_t;
```

## Chart Types

The module supports three chart rendering types:

| Type | Description | Appearance |
|------|-------------|-----------|
| `HORIZ_BARGRAPH_TYPE_BAR` | Bar chart (default) | Individual bars for each data point |
| `HORIZ_BARGRAPH_TYPE_LINE` | Line chart | Connected line between points |
| `HORIZ_BARGRAPH_TYPE_POINT` | Point scatter | Individual points without connecting lines |

Set the chart type with:
```c
horiz_bargraph_set_type(&my_graph, HORIZ_BARGRAPH_TYPE_LINE);
```

## API Reference

### Initialization

| Function | Description |
|----------|-------------|
| `widget_t* horiz_bargraph_create(lv_obj_t* parent, const char* id, const char* binding_key, int16_t x, int16_t y, uint16_t w, uint16_t h)` | Factory function for widget_base integration. Returns `widget_t*` or NULL on failure. |
| `int horiz_bargraph_init(horiz_bargraph_t* graph)` | Initialize a pre-allocated structure. Returns 0 on success, -1 on error. |

### Data Updates

| Function | Description |
|----------|-------------|
| `void horiz_bargraph_update_data(horiz_bargraph_t* graph, const float* new_data, uint8_t count)` | Update with simple scalar array (no quality flags) |
| `void horiz_bargraph_update_series(horiz_bargraph_t* graph, const float* values, const bool* is_estimated, const bool* is_alarm, uint8_t count)` | Update with per-point quality flags |
| `void horiz_bargraph_update_point(horiz_bargraph_t* graph, uint8_t index, float value, bool is_estimated, bool is_alarm)` | Update a single data point |

### Configuration

| Function | Description |
|----------|-------------|
| `void horiz_bargraph_set_axis_labels(horiz_bargraph_t* graph, const char** labels, uint8_t count)` | Set X-axis time/category labels |
| `void horiz_bargraph_set_range(horiz_bargraph_t* graph, float min_val, float max_val)` | Set Y-axis scale range |
| `void horiz_bargraph_set_type(horiz_bargraph_t* graph, horiz_bargraph_type_t chart_type)` | Set chart rendering type |
| `void horiz_bargraph_set_media_color(horiz_bargraph_t* graph, lv_color_t color)` | Set the primary series color |

### Cleanup

| Function | Description |
|----------|-------------|
| `void horiz_bargraph_destroy(horiz_bargraph_t* graph)` | Clean up all LVGL objects and free resources |

## Data Quality Visualization

The horizontal bar graph supports **per-point data quality indicators** through LVGL 9's draw task interception. This allows individual bars to be colored differently without triggering a full chart redraw.

### Visual States for Each Data Point

| State | Bar Appearance | When to Use |
|-------|---------------|-------------|
| **Normal** | 100% opacity, uses `bar_color` | Valid, real-time data |
| **Estimated** | 50% opacity, uses `bar_color` | Interpolated or replacement value |
| **Alarm** | 100% opacity, uses `alarm_color` (red) | Threshold violation detected |

### Usage Example with Quality Flags

```c
/* Data with quality information */
float values[] = {20.0f, 45.0f, 67.0f, 85.0f};
bool estimated[] = {false, true, false, false};  // 2nd point is estimated
bool alarm[] = {false, false, false, true};      // 4th point is alarm

horiz_bargraph_update_series(&my_graph, values, estimated, alarm, 4);
```

**Result:** The chart will show:
- Bar 0: Solid blue (normal)
- Bar 1: Semi-transparent blue (estimated)
- Bar 2: Solid blue (normal)
- Bar 3: Solid red (alarm)

## X-Axis Labels

The module supports up to **6 X-axis labels** for time or category divisions:

```c
/* Set time-based labels */
const char* labels[] = {"-8h", "-4h", "-2h", "Now"};
horiz_bargraph_set_axis_labels(&my_graph, labels, 4);

/* Or category-based labels */
const char* categories[] = {"Mon", "Tue", "Wed", "Thu", "Fri"};
horiz_bargraph_set_axis_labels(&my_graph, categories, 5);
```

Labels are automatically distributed across the chart width. The first label aligns to the left, the last to the right, and intermediate labels are spaced proportionally.

## Complete Example

```c
#include "horiz_bargraph.h"
#include "lv_port.h"

/* Create a historical depth chart */
horiz_bargraph_t depth_graph;

void setup_history_chart(lv_obj_t* tile) {
    memset(&depth_graph, 0, sizeof(depth_graph));
    
    depth_graph.parent = tile;
    depth_graph.x = 20;
    depth_graph.y = 338;
    depth_graph.width = 440;
    depth_graph.height = 128;
    depth_graph.min_value = 0.0f;
    depth_graph.max_value = 140.0f;  // 140 cm max depth
    depth_graph.chart_type = HORIZ_BARGRAPH_TYPE_BAR;
    strcpy(depth_graph.unit, "cm");
    
    /* Set X-axis time labels */
    const char* labels[] = {"-8t", "-32h", "-1h", "Jetzt"};
    depth_graph.axis_label_count = 4;
    for (int i = 0; i < 4; i++) {
        depth_graph.axis_labels[i] = labels[i];
    }
    
    /* Set colors */
    depth_graph.bar_color = lv_color_hex(0x3498db);  // Blue
    depth_graph.alarm_color = lv_color_hex(0xE74C3C); // Red
    
    /* Optional: Click handler for configuration */
    depth_graph.click_cb = on_history_config_clicked;
    
    horiz_bargraph_init(&depth_graph);
}

void update_history_data(float* history_values, bool* estimated_flags, uint8_t count) {
    /* Update with quality information */
    horiz_bargraph_update_series(&depth_graph, history_values, estimated_flags, NULL, count);
}

void update_single_point(uint8_t index, float value, bool is_estimated, bool in_alarm) {
    /* Update individual point with quality flags */
    horiz_bargraph_update_point(&depth_graph, index, value, is_estimated, in_alarm);
}

void on_history_config_clicked(lv_event_t *e) {
    /* Open configuration subpage */
    config_view_show(&depth_graph);
}
```

## Integration with data_storage

To load historical data from LittleFS for the horizontal bar graph:

```c
#include "data_storage.h"

void load_and_display_history(horiz_bargraph_t* graph, const char* channel_key) {
    uint32_t now = (uint32_t)time(NULL);
    uint32_t start = now - 86400;  // Last 24 hours
    uint32_t interval = 3600;     // 1 hour intervals
    uint32_t max_points = 24;
    
    uint32_t actual_interval;
    size_t count;
    
    /* Query data from LittleFS */
    data_point_t* points = data_storage_query(
        channel_key,
        start,
        now,
        interval,
        max_points,
        &actual_interval,
        &count
    );
    
    if (points && count > 0) {
        float values[max_points];
        bool estimated[max_points];
        
        for (size_t i = 0; i < count; i++) {
            values[i] = points[i].value;
            estimated[i] = points[i].is_estimated;
        }
        
        horiz_bargraph_update_series(graph, values, estimated, NULL, count);
        free(points);
    }
}
```

## Default Values

If not specified, the module uses these defaults:

| Parameter | Default Value |
|-----------|---------------|
| width | 440 pixels |
| height | 128 pixels |
| font | ui_font_font0 |
| bg_color | 0x282828 (dark grey) |
| bar_color | 0x3498db (blue) |
| text_color | 0x9E9E9E (light grey) |
| alarm_color | 0xE74C3C (red) |
| chart_type | HORIZ_BARGRAPH_TYPE_BAR |
| min_value | 0.0f |
| max_value | 100.0f |

## Limits

| Limit | Value |
|-------|-------|
| Maximum data points | 40 (HORIZ_BARGRAPH_MAX_POINTS) |
| Maximum X-axis labels | 6 (HORIZ_BARGRAPH_MAX_LABELS) |

## Thread Safety

All public API functions are **thread-safe** and use `lv_port_sem_take()` / `lv_port_sem_give()` internally to protect LVGL operations. Do not call LVGL functions directly on the graph's internal objects.

## Integration with widget_base

The module supports the `widget_base.h` interface for dynamic page management:

```c
/* The factory function returns a widget_t* that can be stored in a page's widget list */
widget_t* graph = horiz_bargraph_create(parent, "id", "binding", x, y, w, h);

/* Update via widget interface (updates the rightmost point) */
if (graph->update_val) {
    graph->update_val(graph, new_value, "cm");
}

/* Destroy via widget interface */
if (graph->destroy) {
    graph->destroy(graph);
}
```

The widget's `user_data` field contains the `horiz_bargraph_t*` for direct access if needed.

## Use Cases

- **Historical Data Display**: Show time-series sensor readings (depth, temperature, pressure)
- **Statistics Overview**: Display daily/weekly/monthly aggregated values
- **Threshold Monitoring**: Visualize when values exceeded alarm thresholds
- **Data Quality Tracking**: Highlight estimated vs. real measurements
- **Multi-Sensor Comparison**: Compare values across different time periods

## Notes

- The chart uses LVGL's built-in `lv_chart` widget internally
- LVGL 9's draw task interception enables efficient per-bar coloring
- For best results with many data points, consider using `HORIZ_BARGRAPH_TYPE_LINE`
- X-axis labels support any text; typical use is time-based (e.g., "-24h", "-12h", "Now")
- All color values support LVGL's color formats (16-bit, 32-bit, true color)
