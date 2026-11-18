# SenseCAP Cistern GUI Header API

The `gui_header` is a globally managed, persistent 64 px header designed to prevent layout shifting
and callback leaks during page transitions. It offers a consistent layout with a main title and five
slots for navigation, status indicators, page indicators and actions.

## Standard Layout & Slot Index

```
┌──────┬──────┬──────────────── title ────────────────┬────────┬──────┬──────┐
│ LEFT │LEFT_2│                                       │RIGHT_3 │RIGHT_2│RIGHT_1│
│ gear │ batt │                                       │ action │ LoRa │ WiFi │
└──────┴──────┴───────────────────────────────────────┴────────┴──────┴──────┘
```

The slots are accessed via the `header_slot_t` enum:

1. **`HEADER_SLOT_LEFT` (0):** leftmost slot. Settings gear by default, or a "Back" button.
2. **`HEADER_SLOT_RIGHT_1` (1):** outermost right slot. Reserved for **Wi-Fi status**; replaced by
   the alarm warning symbol while alarms are active.
3. **`HEADER_SLOT_RIGHT_2` (2):** second slot from the right. Reserved for **LoRa status**.
4. **`HEADER_SLOT_RIGHT_3` (3):** third slot from the right (80 px). **Custom actions**
   (e.g. "Mute", "Acknowledge").
5. **`HEADER_SLOT_LEFT_2` (4):** right of the left slot. **Page specific indicator** hosted as a
   custom widget, e.g. the `battery_icon` of the start page.

## Typical Page Lifecycle

Every page **must** initialize the header when it is shown. The header manages its own background
state (Wi-Fi / LoRa polling, alarm indicator), so a page only clears previous states, sets its title
and optionally maps custom actions.

### 1. Initializing a Normal Page

```c
#include "gui_header.h"

/* Always call clear first to purge old callbacks and hide custom slots! */
gui_header_clear();

/* Set the title for the new page */
gui_header_set_title("Zisterne Hochbehälter");
```

**What `gui_header_clear()` does automatically:**
* Clears the title and removes the standard click callbacks from all slots.
* Hides `HEADER_SLOT_RIGHT_3` (custom action) and `HEADER_SLOT_LEFT_2` (page indicator).
* Restores the default `LV_SYMBOL_SETTINGS` icon in `HEADER_SLOT_LEFT`.
* Restores the live Wi-Fi (or alarm) and LoRa status icons in `HEADER_SLOT_RIGHT_1` and `HEADER_SLOT_RIGHT_2`.

### 2. Creating a Custom Action Page (e.g., Alarm View)

```c
gui_header_clear();
gui_header_set_title(gui_i18n_get("alarm.title"));

/* Override the left slot with a Back button */
gui_header_set_slot(HEADER_SLOT_LEFT, LV_SYMBOL_LEFT, _on_back_clicked, NULL);

/* Override the LoRa icon slot with a custom "Mute" action */
gui_header_set_slot(HEADER_SLOT_RIGHT_2, "Mute (2h)", _on_mute_clicked, NULL);

/* Use the third slot for an "Acknowledge" action */
gui_header_set_slot(HEADER_SLOT_RIGHT_3, LV_SYMBOL_OK, _on_acknowledge_clicked, NULL);

/* Image instead of text */
gui_header_set_slot_image(HEADER_SLOT_RIGHT_3, &my_icon, _on_action_clicked, NULL);
```

*As soon as the next page calls `gui_header_clear()`, these overrides are removed and the native
Wi-Fi / LoRa icons return.*

### 3. Hosting a Custom Widget in a Slot (e.g., Battery Indicator)

A slot can serve as host container for a widget the page creates and owns:

```c
lv_obj_t *slot = gui_header_get_slot_container(HEADER_SLOT_LEFT_2);  /* shows the slot */
if (slot) {
    s_battery.parent = slot;
    battery_icon_init(&s_battery);
}

/* Page without indicator, or page left: */
battery_icon_destroy(&s_battery);
gui_header_hide_slot(HEADER_SLOT_LEFT_2);
```

* `gui_header_get_slot_container()` hides the slot's own label / image and makes the slot visible.
  Call it with the LVGL lock held or from LVGL context.
* `gui_header_clear()` only **hides** the slot; the page deletes its widget itself.

### 4. Creating Full-Screen Pages (e.g., LoRa Diagnostic View)

Some screens need the entire 480 × 480 canvas:

```c
gui_header_set_visible(false);
```

When the user returns to a normal page, call `gui_header_set_visible(true)` before configuring the header.

## Status Icons

`gui_header_update_wifi(is_connected, rssi)` and `gui_header_update_lora(status)` update the default
icons of `HEADER_SLOT_RIGHT_1` / `HEADER_SLOT_RIGHT_2` (`LORA_STATUS_UNINIT / WAITING / ACTIVE / TIMEOUT`).
They are called by the Wi-Fi and LoRa models; pages do not need to call them.

## Global Alarm Indicator

The header shows active alarms without code on the active page.

`gui_header_set_alarm_state(bool)` is thread-safe and does **not** need the LVGL lock: it only stores
the request, an LVGL timer applies the latest request within ~100 ms. While alarms are active:

1. The header background blinks (600 ms) in the theme danger color (`/gui/theme.json`), the title
   switches to a high-contrast text color.
2. The Wi-Fi icon in `HEADER_SLOT_RIGHT_1` is replaced by `LV_SYMBOL_WARNING` in the theme warning color.
3. A tap on the warning symbol opens the alarm view.
4. `gui_header_set_alarm_state(false)` stops the blinking and restores the Wi-Fi icon and its click behavior.
