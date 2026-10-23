# SenseCAP Cistern GUI Header API

The `gui_header` is a globally managed, persistent 64px header designed to prevent layout shifting and callback leaks during page transitions. It offers a consistent layout with a main title and four configurable slots for navigation, status indicators, and actions.

## Standard Layout & Slot Index

The header provides four fixed slots (accessed via the `header_slot_t` enum):

1. **`HEADER_SLOT_LEFT` (0):** The leftmost slot. Usually holds the "Back" button or a Settings gear icon.
2. **`HEADER_SLOT_RIGHT_1` (1):** The outermost right slot. Reserved natively for **WiFi Status**.
3. **`HEADER_SLOT_RIGHT_2` (2):** The second slot from the right. Reserved natively for **LoRa Status**.
4. **`HEADER_SLOT_RIGHT_3` (3):** The third slot from the right. Available for **Custom Actions** (e.g., "Mute", "Refresh").

## Typical Page Lifecycle

Every time a user navigates to a new page, that page **must** initialize the header. By default, the header manages its own background state (WiFi/LoRa polling), so you only need to clear previous states, set your title, and optionally map custom actions.

### 1. Initializing a Normal Page
To prepare the header for a standard page (like the Cistern Demo or a dynamically loaded JSON page), run:

```c
#include "gui_header.h"

/* Always call clear first to purge old callbacks and hide custom action buttons! */
gui_header_clear();

/* Set the title for the new page */
gui_header_set_title("Zisterne Hochbehälter");
```

**What `gui_header_clear()` does automatically:**
* Deletes any leftover text or click-callbacks from the previous page.
* Hides `HEADER_SLOT_RIGHT_3` (the custom action slot).
* Restores the default `LV_SYMBOL_SETTINGS` icon in `HEADER_SLOT_LEFT`.
* Restores the live WiFi and LoRa status icons in `HEADER_SLOT_RIGHT_1` and `HEADER_SLOT_RIGHT_2`.

### 2. Creating a Custom Action Page (e.g., Alarm View)
If your page requires custom actions or a back button, you can override the default slots:

```c
gui_header_clear();
gui_header_set_title("Alarme");

/* Override Left Slot with a Back Button */
gui_header_set_slot(HEADER_SLOT_LEFT, LV_SYMBOL_LEFT " Back", _on_back_clicked, NULL);

/* Override the LoRa icon slot with a custom "Mute" action */
gui_header_set_slot(HEADER_SLOT_RIGHT_2, "Mute (2h)", _on_mute_clicked, NULL);

/* Use the empty third slot for an "Acknowledge" action */
gui_header_set_slot(HEADER_SLOT_RIGHT_3, "✓ Quit", _on_acknowledge_clicked, NULL);
```
*Note: As soon as the user navigates away and the next page calls `gui_header_clear()`, these custom overrides are removed and the native WiFi/LoRa icons return instantly.*

### 3. Creating Full-Screen Pages (e.g., LoRa Diagnostic View)
Some screens need the entire 480x480 canvas. You can hide the header entirely.

```c
/* Hide the header container */
gui_header_set_visible(false);
```

When the user exits the full-screen view and navigates back to a normal page, simply call `gui_header_set_visible(true)` before configuring the header.

## Global Alarm Override

The header handles critical system alarms visually without requiring code changes on the active page. 

When the background task triggers `gui_header_set_alarm_state(true)`:
1. The header background pulses red.
2. The WiFi icon in `HEADER_SLOT_RIGHT_1` is temporarily replaced with a blinking yellow `LV_SYMBOL_WARNING` triangle.
3. Clicking the warning triangle intercepts the user's action and forces navigation to the Alarm View.
4. Calling `gui_header_set_alarm_state(false)` immediately halts the blinking and restores the original WiFi icon and its behavior.