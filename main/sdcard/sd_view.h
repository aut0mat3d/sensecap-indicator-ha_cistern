#ifndef SD_VIEW_H
#define SD_VIEW_H

#include <stdbool.h>

/**
 * @brief Initialize the SD card management view component.
 * Registers event listeners for SD format responses.
 * 
 * @return 0 on success, negative value on error.
 */
int sd_view_init(void);

/**
 * @brief Display the SD Card settings modal window.
 */
void sd_view_show(void);

#endif /* SD_VIEW_H */