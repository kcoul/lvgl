/**
 * @file lv_linux_fbdev.h
 *
 */

#ifndef LV_LINUX_FBDEV_H
#define LV_LINUX_FBDEV_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../../display/lv_display.h"

#if LV_USE_LINUX_FBDEV

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/
lv_display_t * lv_linux_fbdev_create(void);

lv_result_t lv_linux_fbdev_set_file(lv_display_t * disp, const char * file);

/**
 * Force the display to be refreshed on every change.
 * Expected to be used with LV_DISPLAY_RENDER_MODE_DIRECT or LV_DISPLAY_RENDER_MODE_FULL.
 */
/**
 * Set the display's colour temperature, in kelvin.
 *
 * A warm, low-blue mode for after dark - the f.lux / Redshift effect, applied
 * as a per-channel gain while the finished frame is copied into the framebuffer.
 * ZEN_WARM_NEUTRAL (6500) is identity and costs nothing; below it, blue and
 * then green are attenuated. See tools/lvgl-common/zen_warm.h.
 *
 * This filters only what LVGL draws. A display-controller CTM would cover
 * every client and is the right long-term home for it - see
 * zenbox-hardware/docs/qnx-blue-light-filter.md for why that route is not
 * available yet.
 *
 * @param   disp    The display object
 * @param   kelvin  Target colour temperature
 */
void lv_linux_fbdev_set_kelvin(lv_display_t * disp, int kelvin);

void lv_linux_fbdev_set_force_refresh(lv_display_t * disp, bool enabled);

/**********************
 *      MACROS
 **********************/

#endif /* LV_USE_LINUX_FBDEV */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV_LINUX_FBDEV_H */
