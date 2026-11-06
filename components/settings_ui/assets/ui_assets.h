/* Declarations for the images compiled into the firmware.
 *
 * The assets' .c files define the array and the descriptor; this header is for
 * whoever uses them. Keeping it separate avoids declaring the externs by hand
 * in ui.c, which is the classic way to end up with a descriptor that no longer
 * matches its array. */
#pragma once
#include "lvgl.h"

/* About screen: 320x240 RGB565, full screen. */
extern const lv_image_dsc_t author_selfie;

