#ifndef SETTINGS_VIEW_H
#define SETTINGS_VIEW_H

#include "lvgl.h"
#include "view_data.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LORA_UI_UNINIT = 0,
    LORA_UI_WAITING,
    LORA_UI_ACTIVE,
    LORA_UI_TIMEOUT
} lora_ui_state_t;

/* header_slot_t and old header_set_* prototypes REMOVED here.
 * The entire header is now handled exclusively by gui/gui_header.h */

int settings_view_init(void);
void settings_view_show(void);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_VIEW_H */