# Text Widget Module

The `text_widget` module provides a flexible multi-row text and telemetry card for displaying label-value pairs. It supports vertical auto-scrolling on overflow, automatic word-wrapping, dynamic data binding, and per-pair styling with data quality color indicators.

## Overview

The text widget displays multiple rows of information in a scrollable card layout:
- **Label-Value Pairs**: Configurable rows with label and value text
- **Auto-Scrolling**: Vertical scrolling automatically enabled on content overflow
- **Word-Wrapping**: Long text automatically wraps within card width
- **Data Binding**: Pairs can be bound to telemetry keys for automatic updates
- **Quality Indicators**: Value text color changes based on data quality (normal, estimated, alarm)
- **Custom Colors**: Per-pair label and value colors
- **Dynamic Title**: Optional card header

## Quick Start

### 1. Factory Method (Recommended for Page Manager)

```c
#include "text_widget.h"

/* Create via factory function */
widget_t* widget = text_widget_create(
    tile,                    // Parent LVGL object (tile or view container)
    "sensor_card",           // Unique widget ID
    "depth",                // Optional default binding key
    230,                    // X position in pixels
    72,                     // Y position in pixels
    230,                    // Width in pixels
    256                     // Height in pixels
);

/* Add pairs after creation */
text_widget_add_pair((text_widget_t*)widget->user_data, 
    "depth", "Füllstand:", "--", "m³", lv_color_hex(0x52AAE5));
text_widget_add_pair((text_widget_t*)widget->user_data,
    "flowrate", "Änderungsrate:", "--", "m³/h", lv_color_hex(0x2ECC71));
```

### 2. Direct Initialization (Backward Compatible)

```c
#include "text_widget.h"

/* Declare and configure */
text_widget_t my_widget;
my_widget.parent = tile;
my_widget.x = 230;
my_widget.y = 72;
my_widget.width = 230;
my_widget.height = 256;
my_widget.bg_color = lv_color_hex(0x282828);

/* Set title */
my_widget.title = "Sensor Status";
my_widget.title_color = lv_color_hex(0x9E9E9E);

/* Configure pairs */
text_widget_pair_t pairs[3];
pairs[0].label = "Füllstand:";
pairs[0].value = "3.42 m³";
pairs[0].unit = "m³";
pairs[0].value_color = lv_color_hex(0x52AAE5);

pairs[1].label = "Änderungsrate:";
pairs[1].value = "+0.12 m³/h";
pairs[1].unit = "m³/h";
pairs[1].value_color = lv_color_hex(0x2ECC71);

pairs[2].label = "Aktualisierung:";
pairs[2].value = "Vor 2 Min.";
pairs[2].value_color = lv_color_hex(0xFFFFFF);

memcpy(my_widget.pairs, pairs, sizeof(pairs));
my_widget.pair_count = 3;

/* Initialize */
if (text_widget_init(&my_widget) != 0) {
    // Handle error
}

/* Update a value */
text_widget_update_value(&my_widget, 0, "4.20 m³");

/* Cleanup when done */
text_widget_destroy(&my_widget);
```

## Configuration Structure

The `text_widget_t` structure contains all configurable parameters:

```c
typedef struct {
    /* Position & Size */
    lv_obj_t* parent;           // Parent LVGL container
    int16_t x;                  // X position (pixels)
    int16_t y;                  // Y position (pixels)
    uint16_t width;             // Width (pixels)
    uint16_t height;            // Height (pixels)
    
    /* Display */
    const char* title;         // Optional card title/header
    text_widget_pair_t pairs[TEXT_WIDGET_MAX_PAIRS];  // Array of label-value pairs
    uint8_t pair_count;        // Number of configured pairs
    
    /* Styling */
    const lv_font_t* font;     // Default font for text
    lv_color_t bg_color;       // Card background color (default: 0x282828)
    lv_color_t text_color;     // Default text color (default: 0xFFFFFF)
    lv_color_t title_color;    // Title text color (default: 0x9E9E9E)
    
    /* Callback */
    lv_event_cb_t click_cb;    // Click handler for config subpage
    void* user_data;           // User data for callback
    
    /* Internal - Do not modify directly */
    lv_obj_t* container;       // Scrollable card container
    lv_obj_t* lbl_title;       // Title label object
    lv_obj_t* pair_conts[TEXT_WIDGET_MAX_PAIRS]; // Row containers
    lv_obj_t* labels[TEXT_WIDGET_MAX_PAIRS];    // Label objects
    lv_obj_t* values[TEXT_WIDGET_MAX_PAIRS];    // Value objects
} text_widget_t;
```

### Pair Configuration

Each label-value pair has its own configuration:

```c
typedef struct {
    char        binding_key[32];   // Key for data binding (e.g. "depth")
    const char* label;             // Label text (e.g. "Füllstand:")
    const char* value;             // Initial value text
    const char* unit;              // Unit string (e.g. "m³")
    lv_color_t  label_color;       // Color for label text
    lv_color_t  value_color;       // Color for value text
    const lv_font_t* font;         // Font override for value (optional)
} text_widget_pair_t;
```

## API Reference

### Initialization

| Function | Description |
|----------|-------------|
| `widget_t* text_widget_create(lv_obj_t* parent, const char* id, const char* binding_key, int16_t x, int16_t y, uint16_t w, uint16_t h)` | Factory function for widget_base integration. Returns `widget_t*` or NULL on failure. |
| `int text_widget_init(text_widget_t* widget)` | Initialize a pre-allocated structure. Returns 0 on success, -1 on error. |

### Pair Management

| Function | Description |
|----------|-------------|
| `int text_widget_add_pair(text_widget_t* widget, const char* binding_key, const char* label, const char* initial_value, const char* unit, lv_color_t value_color)` | Add a new label-value pair dynamically. Returns 0 on success, -1 if max pairs reached. |

### Updates

| Function | Description |
|----------|-------------|
| `void text_widget_update_value(text_widget_t* widget, uint8_t pair_index, const char* value)` | Update value text by index (simple string) |
| `void text_widget_update_binding(text_widget_t* widget, const char* key, float value, const char* unit, bool is_estimated, bool is_alarm)` | Update value by binding key with quality flags |
| `void text_widget_update_label(text_widget_t* widget, uint8_t pair_index, const char* label)` | Update label text by index |
| `void text_widget_set_title(text_widget_t* widget, const char* title)` | Set or update card title (NULL to hide) |

### Color Configuration

| Function | Description |
|----------|-------------|
| `void text_widget_set_value_color(text_widget_t* widget, uint8_t pair_index, lv_color_t color)` | Set value text color for a specific pair |
| `void text_widget_set_label_color(text_widget_t* widget, uint8_t pair_index, lv_color_t color)` | Set label text color for a specific pair |

### Cleanup

| Function | Description |
|----------|-------------|
| `void text_widget_destroy(text_widget_t* widget)` | Clean up all LVGL objects and free resources |

## Data Binding

The text widget supports **automatic data binding** via binding keys. When using `text_widget_update_binding()`, the widget searches for pairs matching the specified key and updates them:

```c
/* Configure widget with bound pairs */
text_widget_add_pair(&my_widget, "depth", "Füllstand:", "--", "m³", lv_color_hex(0x52AAE5));
text_widget_add_pair(&my_widget, "flowrate", "Flow Rate:", "--", "m³/h", lv_color_hex(0x2ECC71));

/* Later, update via binding key */
text_widget_update_binding(&my_widget, "depth", 3.42f, "m³", false, false);
text_widget_update_binding(&my_widget, "flowrate", 0.12f, "m³/h", false, true);
```

**Note:** The widget can also be updated via the widget_base interface when created with a binding_key.

## Data Quality Visualization

The text widget supports **automatic color rendering based on data quality** when using `text_widget_update_binding()`:

| State | Value Text Color | When to Use |
|-------|------------------|-------------|
| **Normal** | Uses pair's `value_color` | Valid, real-time data |
| **Estimated** | Light grey (0xBDC3C7) | Interpolated or replacement value |
| **Alarm** | Red (0xE74C3C) | Threshold violation detected |

The color is automatically applied when using `text_widget_update_binding()` with the quality flags.

### Manual Color Control

You can also manually set colors for each pair:

```c
/* Set custom colors for a pair */
text_widget_set_value_color(&my_widget, 1, lv_color_hex(0xE74C3C));  // Red for alarm
text_widget_set_label_color(&my_widget, 1, lv_color_hex(0xFFFFFF));   // White for label
```

## Complete Example

```c
#include "text_widget.h"
#include "lv_port.h"

/* Create a sensor status card */
text_widget_t sensor_card;

void setup_sensor_card(lv_obj_t* tile) {
    memset(&sensor_card, 0, sizeof(sensor_card));
    
    sensor_card.parent = tile;
    sensor_card.x = 230;
    sensor_card.y = 72;
    sensor_card.width = 230;
    sensor_card.height = 256;
    sensor_card.bg_color = lv_color_hex(0x282828);
    sensor_card.title = "Tank Status";
    sensor_card.title_color = lv_color_hex(0x9E9E9E);
    
    /* Add label-value pairs */
    text_widget_add_pair(&sensor_card, "depth", "Füllstand:", "--", "m³", lv_color_hex(0x52AAE5));
    text_widget_add_pair(&sensor_card, "volume", "Volumen:", "--", "m³", lv_color_hex(0x52AAE5));
    text_widget_add_pair(&sensor_card, "flowrate", "Änderungsrate:", "--", "m³/h", lv_color_hex(0x2ECC71));
    text_widget_add_pair(&sensor_card, "updated", "Aktualisierung:", "--", "", lv_color_hex(0xFFFFFF));
    
    /* Optional: Click handler for configuration */
    sensor_card.click_cb = on_sensor_config_clicked;
    
    text_widget_init(&sensor_card);
}

void update_sensor_values(float depth, float volume, float flowrate, bool flowrate_alarm) {
    /* Update by binding keys with quality information */
    text_widget_update_binding(&sensor_card, "depth", depth, "m³", false, false);
    text_widget_update_binding(&sensor_card, "volume", volume, "m³", false, false);
    text_widget_update_binding(&sensor_card, "flowrate", flowrate, "m³/h", false, flowrate_alarm);
    
    /* Update by index (simple string) */
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "Vor %d Min.", 2);
    text_widget_update_value(&sensor_card, 3, timestamp);
}

void on_sensor_config_clicked(lv_event_t *e) {
    /* Open configuration subpage */
    config_view_show(&sensor_card);
}
```

## Integration with Live Telemetry

The text widget integrates seamlessly with the data storage system:

```c
#include "data_storage.h"

void update_from_telemetry(text_widget_t* widget, const char* channel_key, float value, const char* unit) {
    /* Check if this widget has a pair bound to this channel */
    text_widget_update_binding(widget, channel_key, value, unit, false, false);
}

/* Or use the widget_base callback */
void on_telemetry_update(const char* key, float value, const char* unit) {
    /* All widgets with matching binding_key will be updated automatically */
    /* via the widget_base update_val callback */
}
```

## Vertical Scrolling

The text widget **automatically enables vertical scrolling** when content exceeds the card height. Features:
- Scrollbar appears automatically (LV_SCROLLBAR_MODE_AUTO)
- Horizontal scrolling is disabled
- Flex column layout ensures proper vertical stacking
- Word-wrapping on long text prevents horizontal overflow

```c
/* Content will automatically scroll when it exceeds 256px height */
text_widget_t tall_widget;
tall_widget.height = 256;  // Fixed card height
// Add many pairs - scrolling will be enabled automatically
```

## Limits

| Limit | Value |
|-------|-------|
| Maximum pairs | 8 (TEXT_WIDGET_MAX_PAIRS) |

## Default Values

If not specified, the module uses these defaults:

| Parameter | Default Value |
|-----------|---------------|
| width | 230 pixels |
| height | 256 pixels |
| font | ui_font_font0 |
| bg_color | 0x282828 (dark grey) |
| text_color | 0xFFFFFF (white) |
| title_color | 0x9E9E9E (light grey) |
| label_color (per pair) | 0x9E9E9E (light grey) |
| value_color (per pair) | 0x52AAE5 (light blue) |

## Thread Safety

All public API functions are **thread-safe** and use `lv_port_sem_take()` / `lv_port_sem_give()` internally to protect LVGL operations. Do not call LVGL functions directly on the widget's internal objects.

## Integration with widget_base

The module supports the `widget_base.h` interface for dynamic page management:

```c
/* The factory function returns a widget_t* */
widget_t* widget = text_widget_create(parent, "id", "binding", x, y, w, h);

/* Update via widget interface */
if (widget->update_val) {
    widget->update_val(widget, new_value, "m³");
}

/* Destroy via widget interface */
if (widget->destroy) {
    widget->destroy(widget);
}
```

The widget's `user_data` field contains the `text_widget_t*` for direct access if needed.

## Use Cases

- **Sensor Status Display**: Show multiple sensor readings in a compact card
- **Device Information**: Display device name, serial number, firmware version
- **Configuration Panels**: Show current settings with editable values
- **Dashboard Widgets**: Compact display of key metrics
- **Telemetry Monitoring**: Real-time updates of multiple values
- **Alarm Status**: Color-coded display of alarm conditions

## Notes

- The widget uses LVGL's flex layout for automatic vertical stacking
- All text supports word-wrapping via `LV_LABEL_LONG_WRAP`
- Binding keys enable automatic updates from telemetry systems
- For best appearance, limit pair count to 4-6 in typical card heights
- All color values support LVGL's color formats (16-bit, 32-bit, true color)
