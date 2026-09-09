/**
 * @file    lv_qnx.h
 * @brief   LVGL driver for the QNX Screen compositing window manager
 */

#ifndef LV_QNX_H
#define LV_QNX_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/

#include "../../display/lv_display.h"
#include "../../indev/lv_indev.h"

#if LV_USE_QNX

#include <stdbool.h>
#include <screen/screen.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Create a window to use as a display for LVGL.
 * @param   hor_res     The horizontal resolution (size) of the window
 * @param   ver_res     The vertical resolution (size) of the window
 * @return  A pointer to a new display object if successful, NULL otherwise
 */
lv_display_t * lv_qnx_window_create(int32_t hor_res, int32_t ver_res);

/**
 * Set the title of the window identified by the given display.
 * @param   disp    The display object for the window
 * @param   title   The new title to set
 */
void lv_qnx_window_set_title(lv_display_t * disp, const char * title);

/**
 * Set the display's colour temperature, in kelvin.
 *
 * A warm, low-blue mode for after dark - the f.lux / Redshift effect, applied
 * as a per-channel gain while the finished frame is copied into the window buffer.
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
void lv_qnx_window_set_kelvin(lv_display_t * disp, int kelvin);

/**
 * Create a pointer input device for the display.
 * Only one pointer object is currently supported.
 * @param   disp    The display object associated with the device
 * @return  true if successful, false otherwise
 */
/**
 * Get the underlying QNX Screen window.
 *
 * Zenbox addition. Needed to composite this window against another one on the
 * same display - z-order and blending are Screen properties, and the driver
 * owns the only handle. Returns NULL if `disp` is not a QNX display.
 */
screen_window_t lv_qnx_window_get_native(lv_display_t * disp);

/**
 * The Screen context this driver created and polls.
 *
 * Zenbox addition, and the reason it exists is worth stating: anything else in
 * this process that needs a Screen window MUST create it in this context rather
 * than one of its own.
 *
 * Screen queues events per context. This driver's event loop polls only the
 * context it made, so a window living anywhere else is a queue with no reader -
 * and on the Pi 4 DSI panel a second context did not merely go unread, it cost
 * the first one events. Measured 2026-09-08: with a Vulkan window in a context
 * of its own the panel received 8 MTOUCH TOUCH events for about 12 taps and the
 * TOUCH and RELEASE counts did not match; with that window gone, 45 and 45.
 * Neither frame pacing nor draining the second queue from its own thread
 * recovered it.
 *
 * Returns NULL if `disp` is not a QNX display.
 */
screen_context_t lv_qnx_context_get_native(lv_display_t * disp);

bool lv_qnx_add_pointer_device(lv_display_t * disp);

/**
 * Create a keyboard input device for the display.
 * Only one keyboard object is currently supported.
 * @param   disp    The display object associated with the device
 * @return  true if successful, false otherwise
 */
bool lv_qnx_add_keyboard_device(lv_display_t * disp);

/**
 * Runs the event loop for the display.
 * The function only returns in response to a close event.
 * @param   disp    The display for the event loop
 * @return  Exit code
 */
int lv_qnx_event_loop(lv_display_t * disp);

/**********************
 *      MACROS
 **********************/

#endif /* LV_DRV_QNX */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV_QNX_H */
