/**
 * @file lv_qnx.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_qnx.h"
#if LV_USE_QNX
#include <stdbool.h>
#include "../../core/lv_refr.h"
#include "../../stdlib/lv_string.h"
#include "../../core/lv_global.h"
#include "../../display/lv_display_private.h"
#include "../../lv_init.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <screen/screen.h>
#include "../zenbox/zen_warm.h"
#include <sys/keycodes.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    screen_window_t     window;
    screen_buffer_t     buffers[LV_QNX_BUF_COUNT];
    void           *    mapped[LV_QNX_BUF_COUNT];
    void           *    shadow;
    int                 bufsize;
    int                 stride;
    lv_area_t           frame_dirty;
    lv_area_t           prev_dirty;
    bool                dirty_valid;
    zen_warm_t          warm;
    int                 bufidx;
    bool                managed;
    lv_indev_t     *    pointer;
    lv_indev_t     *    keyboard;
} lv_qnx_window_t;

typedef struct {
    int                 pos[2];
    int                 buttons;
    uint32_t            last_move_ms;   /* tick of the last MTOUCH_MOVE */
    bool                buttons_from_mtouch; /* set the buttons from touch, not a mouse */
    uint32_t            quiet_until_ms; /* ignore contact until this tick */
} lv_qnx_pointer_t;

/* A finger is "still down" while MOVE events keep arriving. This is how long
 * the stream may stop before the contact is treated as lifted. */
#define LV_QNX_TOUCH_IDLE_MS  120

/* After an explicit RELEASE, how long to disregard the trailing MOVE repeats
 * the panel emits for an already-lifted finger. Human gestures are far apart
 * compared to this, so it costs a new contact nothing. */
#define LV_QNX_TOUCH_QUIET_MS 100

typedef struct {
    int                 key;
    int                 flags;
} lv_qnx_keyboard_t;


/**********************
 *  STATIC PROTOTYPES
 **********************/
static uint32_t get_ticks(void);
static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * color_p);
static bool window_create(lv_display_t * disp);
static bool init_display_from_window(lv_display_t * disp);
static void get_pointer(lv_indev_t * indev, lv_indev_data_t * data);
static void get_key(lv_indev_t * indev, lv_indev_data_t * data);
static bool handle_pointer_event(lv_display_t * disp, screen_event_t event);
static bool handle_mtouch_event(lv_display_t * disp, screen_event_t event, int type);
static bool handle_keyboard_event(lv_display_t * disp, screen_event_t event);
static void release_disp_cb(lv_event_t * e);
static void refresh_cb(lv_timer_t * timer);

/***********************
 *   GLOBAL PROTOTYPES
 ***********************/

static screen_context_t context;

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_display_t * lv_qnx_window_create(int32_t hor_res, int32_t ver_res)
{
    static bool inited = false;

    if(!inited) {
        if(screen_create_context(&context,
                                 SCREEN_APPLICATION_CONTEXT) != 0) {
            LV_LOG_ERROR("screen_create_context: %s", strerror(errno));
            return NULL;
        }

        lv_tick_set_cb(get_ticks);
        inited = true;
    }

    lv_qnx_window_t * dsc = lv_malloc_zeroed(sizeof(lv_qnx_window_t));
    LV_ASSERT_MALLOC(dsc);
    if(dsc == NULL) return NULL;

    lv_display_t * disp = lv_display_create(hor_res, ver_res);
    if(disp == NULL) {
        lv_free(dsc);
        return NULL;
    }
    lv_display_add_event_cb(disp, release_disp_cb, LV_EVENT_DELETE, disp);
    lv_display_set_driver_data(disp, dsc);

    /*Colour temperature, from the environment so an appliance can be started
     *warm with no code change. Unset means neutral, which costs nothing.*/
    {
        const char * kelvin_env = getenv("LV_QNX_KELVIN");
        zen_warm_build(&dsc->warm,
                       kelvin_env != NULL ? atoi(kelvin_env) : ZEN_WARM_NEUTRAL);
    }
    if(!window_create(disp)) {
        lv_free(dsc);
        return NULL;
    }

    lv_display_set_flush_cb(disp, flush_cb);

    if(!init_display_from_window(disp)) {
        screen_destroy_window(dsc->window);
        lv_free(dsc);
        return NULL;
    }

    /*Replace the default refresh timer handler, so that we can run it on
     *demand instead of constantly.*/
    lv_timer_t * refr_timer = lv_display_get_refr_timer(disp);
    lv_timer_set_cb(refr_timer, refresh_cb);

    return disp;
}

screen_window_t lv_qnx_window_get_native(lv_display_t * disp)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    return dsc != NULL ? dsc->window : NULL;
}

void lv_qnx_window_set_kelvin(lv_display_t * disp, int kelvin)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    zen_warm_build(&dsc->warm, kelvin);

    /* The filter is applied during the copy out, so only pixels that get
     * re-copied get the new temperature. Without a full invalidate the screen
     * would warm one dirty rectangle at a time as widgets happened to redraw. */
    lv_obj_invalidate(lv_screen_active());
}

void lv_qnx_window_set_title(lv_display_t * disp, const char * title)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    if(!dsc->managed) {
        /*Can't set title if there is no window manager*/
        return;
    }

    screen_event_t event;
    screen_create_event(&event);

    char title_buf[64];
    lv_snprintf(title_buf, sizeof(title_buf), "Title=%s", title);

    int type = SCREEN_EVENT_MANAGER;
    screen_set_event_property_iv(event, SCREEN_PROPERTY_TYPE, &type);
    screen_set_event_property_cv(event, SCREEN_PROPERTY_USER_DATA,
                                 sizeof(title_buf), title_buf);
    screen_set_event_property_pv(event, SCREEN_PROPERTY_WINDOW,
                                 (void **)&dsc->window);
    screen_set_event_property_pv(event, SCREEN_PROPERTY_CONTEXT,
                                 (void **)&context);

    screen_inject_event(NULL, event);
}

bool lv_qnx_add_pointer_device(lv_display_t * disp)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    if(dsc->pointer != NULL) {
        /*Only one pointer device per display*/
        return false;
    }

    lv_qnx_pointer_t * ptr_dsc = lv_malloc_zeroed(sizeof(lv_qnx_pointer_t));
    LV_ASSERT_MALLOC(ptr_dsc);
    if(ptr_dsc == NULL) {
        return false;
    }

    dsc->pointer = lv_indev_create();
    if(dsc->pointer == NULL) {
        lv_free(ptr_dsc);
        return false;
    }

    lv_indev_set_type(dsc->pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(dsc->pointer, get_pointer);
    lv_indev_set_driver_data(dsc->pointer, ptr_dsc);
    lv_indev_set_mode(dsc->pointer, LV_INDEV_MODE_EVENT);
    return true;
}

bool lv_qnx_add_keyboard_device(lv_display_t * disp)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    if(dsc->keyboard != NULL) {
        /*Only one keyboard device per display*/
        return false;
    }

    lv_qnx_keyboard_t * kbd_dsc = lv_malloc_zeroed(sizeof(lv_qnx_keyboard_t));
    LV_ASSERT_MALLOC(kbd_dsc);
    if(dsc == NULL) {
        return false;
    }

    dsc->keyboard = lv_indev_create();
    if(dsc->keyboard == NULL) {
        lv_free(kbd_dsc);
        return false;
    }

    lv_indev_set_type(dsc->keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(dsc->keyboard, get_key);
    lv_indev_set_driver_data(dsc->keyboard, kbd_dsc);
    lv_indev_set_mode(dsc->keyboard, LV_INDEV_MODE_EVENT);
    return true;
}

int lv_qnx_event_loop(lv_display_t * disp)
{
    lv_refr_now(disp);

    /*Run the event loop*/
    screen_event_t  event;
    if(screen_create_event(&event) != 0) {
        LV_LOG_ERROR("screen_create_event: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    uint64_t timeout_ns = 0;
    for(;;) {
        /*Wait for an event, timing out after 16ms if animations are running*/
        if(screen_get_event(context, event, timeout_ns) != 0) {
            LV_LOG_ERROR("screen_get_event: %s", strerror(errno));
            return EXIT_FAILURE;
        }

        /*Get the event's type*/
        int type;
        if(screen_get_event_property_iv(event, SCREEN_PROPERTY_TYPE, &type)
           != 0) {
            LV_LOG_ERROR("screen_get_event_property_iv(TYPE): %s", strerror(errno));
            return EXIT_FAILURE;
        }

        if(type == SCREEN_EVENT_POINTER) {
            if(!handle_pointer_event(disp, event)) {
                return EXIT_FAILURE;
            }
        }
        else if(type == SCREEN_EVENT_MTOUCH_TOUCH ||
                type == SCREEN_EVENT_MTOUCH_MOVE ||
                type == SCREEN_EVENT_MTOUCH_RELEASE) {
            if(!handle_mtouch_event(disp, event, type)) {
                return EXIT_FAILURE;
            }
        }
        else if(type == SCREEN_EVENT_KEYBOARD) {
            if(!handle_keyboard_event(disp, event)) {
                return EXIT_FAILURE;
            }
        }
        else if(type == SCREEN_EVENT_MANAGER) {
            /*Only sub-type supported is closing the window*/
            break;
        }

        /*Calculate the next timeout*/
        uint32_t timeout_ms = lv_timer_handler();
        if(timeout_ms == LV_NO_TIMER_READY) {
            timeout_ns = -1ULL;
        }
        else {
            timeout_ns = (uint64_t)timeout_ms * 1000000UL;
        }
    }

    return EXIT_SUCCESS;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

static uint32_t get_ticks(void)
{
    uint64_t const ns = clock_gettime_mon_ns();
    return (uint32_t)(ns / 1000000UL);
}

static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);

    LV_UNUSED(px_map);

    /* DIRECT mode calls this once per dirty region; accumulate them and copy
     * once, when the frame is complete. */
    if(dsc->dirty_valid) {
        lv_area_join(&dsc->frame_dirty, &dsc->frame_dirty, area);
    }
    else {
        dsc->frame_dirty = *area;
        dsc->dirty_valid = true;
    }

    if(!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }

    {
        /* Copy this frame's dirty region *and* the previous frame's. The two
         * window buffers alternate, so the one being written now last saw the
         * frame before last and is stale wherever that frame changed. Copying
         * the union repairs it without tracking per-buffer history.
         *
         * This is the difference between a slider drag costing 3.9 ms of CPU
         * and costing almost nothing: the region is a few tens of KB against a
         * 7.91 MB frame, and on an audio box that CPU is not spare. */
        lv_area_t box = dsc->frame_dirty;
        int32_t y, x1, w_bytes;
        const uint8_t * src;
        uint8_t * dst;

        if(dsc->prev_dirty.x2 >= dsc->prev_dirty.x1) {
            lv_area_join(&box, &box, &dsc->prev_dirty);
        }

        /* Clamp: a joined area can exceed the display if a previous region was
         * recorded against a different size. */
        if(box.x1 < 0) box.x1 = 0;
        if(box.y1 < 0) box.y1 = 0;
        if(box.x2 >= lv_display_get_horizontal_resolution(disp))
            box.x2 = lv_display_get_horizontal_resolution(disp) - 1;
        if(box.y2 >= lv_display_get_vertical_resolution(disp))
            box.y2 = lv_display_get_vertical_resolution(disp) - 1;

        x1 = box.x1;
        w_bytes = (box.x2 - x1 + 1) * (int32_t)sizeof(uint32_t);
        src = (const uint8_t *)dsc->shadow;
        dst = (uint8_t *)dsc->mapped[dsc->bufidx];

        for(y = box.y1; y <= box.y2; y++) {
            size_t off = (size_t)y * (size_t)dsc->stride
                       + (size_t)x1 * sizeof(uint32_t);
            /* The warm filter rides the copy that patch 0001 already
             * put here for bandwidth reasons, so it costs one pass, not two.
             * At 6500 K it is inactive and this stays the plain memcpy it
             * was - which is why the unfiltered figures in this README still
             * stand unchanged. */
            if(dsc->warm.active) {
                zen_warm_row_argb8888(&dsc->warm, dst + off, src + off,
                                      (size_t)(box.x2 - x1 + 1));
            }
            else {
                lv_memcpy(dst + off, src + off, (size_t)w_bytes);
            }
        }

        dsc->prev_dirty = dsc->frame_dirty;
        dsc->dirty_valid = false;
    }

    if(screen_post_window(dsc->window, dsc->buffers[dsc->bufidx], 0, NULL, 0)
       != 0) {
        LV_LOG_ERROR("screen_post_window: %s", strerror(errno));
    }

#if (LV_QNX_BUF_COUNT > 1)
    dsc->bufidx = 1 - dsc->bufidx;
#endif

    lv_display_flush_ready(disp);
}

static bool window_create(lv_display_t * disp)
{
    /*Create a window*/
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    if(screen_create_window(&dsc->window, context) != 0) {
        LV_LOG_ERROR("screen_create_window: %s", strerror(errno));
        return false;
    }

    /*Optionally place the window on a specific display.
     *
     *Screen puts a new window on the default display, which on a multi-head
     *board is not necessarily the wanted one - a Pi 5 with a panel on its
     *second HDMI port puts the UI on the developer's monitor instead. The
     *display index comes from the environment so this needs no API change and
     *is inert when unset.*/
    const char * display_env = getenv("LV_QNX_DISPLAY");
    if(display_env != NULL) {
        int display_count = 0;
        int wanted = atoi(display_env);

        if(screen_get_context_property_iv(context, SCREEN_PROPERTY_DISPLAY_COUNT,
                                          &display_count) != 0) {
            LV_LOG_ERROR("screen_get_context_property_iv(DISPLAY_COUNT): %s", strerror(errno));
        }
        else if(wanted < 0 || wanted >= display_count) {
            LV_LOG_WARN("LV_QNX_DISPLAY=%d out of range, %d display(s) present",
                        wanted, display_count);
        }
        else {
            screen_display_t * displays =
                lv_malloc_zeroed((size_t)display_count * sizeof(screen_display_t));
            if(displays != NULL) {
                if(screen_get_context_property_pv(context, SCREEN_PROPERTY_DISPLAYS,
                                                  (void **)displays) != 0) {
                    LV_LOG_ERROR("screen_get_context_property_pv(DISPLAYS): %s", strerror(errno));
                }
                else if(screen_set_window_property_pv(dsc->window, SCREEN_PROPERTY_DISPLAY,
                                                      (void **)&displays[wanted]) != 0) {
                    LV_LOG_ERROR("screen_set_window_property_pv(DISPLAY): %s", strerror(errno));
                }
                else {
                    LV_LOG_INFO("window placed on display index %d of %d",
                                wanted, display_count);
                }
                lv_free(displays);
            }
        }
    }

    /*Set window properties*/
    int rect[] = { 0, 0, disp->hor_res, disp->ver_res };
    if(screen_set_window_property_iv(dsc->window, SCREEN_PROPERTY_POSITION,
                                     &rect[0]) != 0) {
        LV_LOG_ERROR("screen_window_set_property_iv(POSITION): %s", strerror(errno));
        return false;
    }

    if(screen_set_window_property_iv(dsc->window, SCREEN_PROPERTY_SIZE,
                                     &rect[2]) != 0) {
        LV_LOG_ERROR("screen_window_set_property_iv(SIZE): %s", strerror(errno));
        return false;
    }

    if(screen_set_window_property_iv(dsc->window, SCREEN_PROPERTY_SOURCE_SIZE,
                                     &rect[2]) != 0) {
        LV_LOG_ERROR("screen_window_set_property_iv(SOURCE_SIZE): %s", strerror(errno));
        return NULL;
    }

    int usage = SCREEN_USAGE_WRITE;
    if(screen_set_window_property_iv(dsc->window, SCREEN_PROPERTY_USAGE,
                                     &usage) != 0) {
        LV_LOG_ERROR("screen_window_set_property_iv(USAGE): %s", strerror(errno));
        return NULL;
    }

    int format = SCREEN_FORMAT_RGBA8888;
    if(screen_set_window_property_iv(dsc->window, SCREEN_PROPERTY_FORMAT,
                                     &format) != 0) {
        LV_LOG_ERROR("screen_window_set_property_iv(USAGE): %s", strerror(errno));
        return NULL;
    }

    /*Initialize window buffers*/
    if(screen_create_window_buffers(dsc->window, LV_QNX_BUF_COUNT) != 0) {
        LV_LOG_ERROR("screen_create_window_buffers: %s", strerror(errno));
        return false;
    }

    if(screen_get_window_property_pv(dsc->window, SCREEN_PROPERTY_BUFFERS,
                                     (void **)&dsc->buffers) != 0) {
        LV_LOG_ERROR("screen_get_window_property_pv(BUFFERS): %s", strerror(errno));
        return false;
    }

    /*Connect to the window manager. Can legitimately fail if one is not running*/
    if(screen_manage_window(dsc->window, "Frame=Y") == 0) {
        dsc->managed = true;
    }
    else {
        dsc->managed = false;
    }

    int visible = 1;
    if(screen_set_window_property_iv(dsc->window, SCREEN_PROPERTY_VISIBLE,
                                     &visible) != 0) {
        LV_LOG_ERROR("screen_set_window_property_iv(VISIBLE): %s", strerror(errno));
        return false;
    }

    return true;
}

static bool init_display_from_window(lv_display_t * disp)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);

    int bufsize;
    if(screen_get_buffer_property_iv(dsc->buffers[0], SCREEN_PROPERTY_SIZE,
                                     &bufsize) == -1) {
        LV_LOG_ERROR("screen_get_buffer_property_iv(SIZE): %s", strerror(errno));
        return false;
    }

    void * ptr1 = NULL;
    if(screen_get_buffer_property_pv(dsc->buffers[0], SCREEN_PROPERTY_POINTER,
                                     &ptr1) == -1) {
        LV_LOG_ERROR("screen_get_buffer_property_pv(POINTER): %s", strerror(errno));
        return false;
    }

    void * ptr2 = NULL;
#if (LV_QNX_BUF_COUNT > 1)
    if(screen_get_buffer_property_pv(dsc->buffers[1], SCREEN_PROPERTY_POINTER,
                                     &ptr2) == -1) {
        LV_LOG_ERROR("screen_get_buffer_property_pv(POINTER): %s", strerror(errno));
        return false;
    }
#endif

    int stride = 0;
    if(screen_get_buffer_property_iv(dsc->buffers[0], SCREEN_PROPERTY_STRIDE,
                                     &stride) == -1) {
        LV_LOG_ERROR("screen_get_buffer_property_iv(STRIDE): %s", strerror(errno));
        return false;
    }
    dsc->stride = stride;

    dsc->mapped[0] = ptr1;
#if (LV_QNX_BUF_COUNT > 1)
    dsc->mapped[1] = ptr2;
#endif
    dsc->bufsize = bufsize;

    /* Render into ordinary heap memory rather than straight into the window
     * buffer, and copy the finished frame across once per refresh.
     *
     * Screen's buffer is write-combining. Measured on a Pi 5 at 1080x1920:
     * sequential writes reach 5.7 GB/s, but reads manage 256 MB/s and
     * read-modify-write just 17.9 MB/s - against 1.3 GB/s for the same loop on
     * the heap, a 74x penalty. Almost everything LVGL draws is a blend, and a
     * blend is a read-modify-write, so drawing directly into the window buffer
     * put every anti-aliased edge and rounded corner on the slowest path the
     * hardware has. A full-screen repaint took 62 ms, making a Pi 5 slower than
     * a Pi Zero writing to a plain framebuffer.
     *
     * Copying the frame out afterwards is the one access pattern this memory is
     * good at: 7.91 MB of sequential writes costs about 1.4 ms.
     *
     * DIRECT rather than FULL so LVGL redraws only what changed; the copy stays
     * whole-frame, which keeps both window buffers coherent without tracking
     * dirty regions across the flip. */
    /* malloc, not lv_malloc: with LV_STDLIB_BUILTIN the LVGL heap is LV_MEM_SIZE
     * (512 KB here) and a full-screen frame is 7.91 MB, so the allocation would
     * simply fail. A framebuffer is a one-off allocation with none of the churn
     * LVGL's pool exists to manage. */
    dsc->prev_dirty.x1 = 0;
    dsc->prev_dirty.y1 = 0;
    dsc->prev_dirty.x2 = -1;
    dsc->prev_dirty.y2 = -1;

    dsc->shadow = malloc((size_t)bufsize);
    if(dsc->shadow == NULL) {
        LV_LOG_ERROR("failed to allocate %d byte shadow buffer", bufsize);
        return false;
    }

    lv_display_set_buffers(disp, dsc->shadow, NULL, bufsize,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    return true;
}

static void release_disp_cb(lv_event_t * e)
{
    lv_display_t * disp = (lv_display_t *) lv_event_get_user_data(e);
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);

    if(dsc->window != NULL) {
        screen_destroy_window(dsc->window);
    }

    if(dsc->pointer != NULL) {
        lv_free(dsc->pointer);
    }

    if(dsc->keyboard != NULL) {
        lv_free(dsc->keyboard);
    }

    if(dsc->shadow != NULL) {
        free(dsc->shadow);
    }

    lv_free(dsc);
    lv_display_set_driver_data(disp, NULL);
}

static void get_pointer(lv_indev_t * indev, lv_indev_data_t * data)
{
    lv_qnx_pointer_t * dsc = lv_indev_get_driver_data(indev);

    /* Lift a contact whose MOVE stream has stopped.
     *
     * A drag on this panel is a bare stream of MOVEs with no RELEASE to end it,
     * so without this the pointer stays pressed after the finger leaves and
     * LVGL keeps the dragged widget captured for every later touch.
     *
     * LVGL polls this from its own indev timer and lv_qnx_event_loop takes its
     * timeout from lv_timer_handler(), so this runs about every 33 ms even when
     * Screen has delivered nothing - which is what makes a timeout usable here
     * rather than needing an event to hang it on. */
    if(dsc->buttons_from_mtouch && dsc->buttons != 0 &&
       (uint32_t)(get_ticks() - dsc->last_move_ms) > LV_QNX_TOUCH_IDLE_MS) {
        dsc->buttons = 0;
    }

    data->point.x = dsc->pos[0];
    data->point.y = dsc->pos[1];
    data->state = (dsc->buttons & SCREEN_LEFT_MOUSE_BUTTON) != 0
                  ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static bool handle_pointer_event(lv_display_t * disp, screen_event_t event)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    if(dsc->pointer == NULL) return true;

    lv_qnx_pointer_t * ptr_dsc = lv_indev_get_driver_data(dsc->pointer);

    if(screen_get_event_property_iv(event, SCREEN_PROPERTY_SOURCE_POSITION,
                                    ptr_dsc->pos)
       != 0) {
        LV_LOG_ERROR("screen_get_event_property_iv(SOURCE_POSITION): %s", strerror(errno));
        return false;
    }

    if(screen_get_event_property_iv(event, SCREEN_PROPERTY_BUTTONS,
                                    &ptr_dsc->buttons)
       != 0) {
        LV_LOG_ERROR("screen_get_event_property_iv(BUTTONS): %s", strerror(errno));
        return false;
    }

    /*A mouse reports its own releases, so the idle lift must not apply to it.*/
    ptr_dsc->buttons_from_mtouch = false;

    lv_indev_read(dsc->pointer);
    return true;
}

/**
 * Feed touchscreen input into the pointer indev.
 *
 * Screen reports a touch panel as SCREEN_EVENT_MTOUCH_*, a distinct event type
 * from SCREEN_EVENT_POINTER - a mouse and a touchscreen do not arrive through
 * the same path. Without this, touch events are read off the queue and silently
 * discarded, and the UI appears to work perfectly while ignoring every finger.
 *
 * Position and press state deliberately come from different events. A panel is
 * free to stream MTOUCH_MOVE on a contact that never reports a touch or a
 * release - the Waveshare AMOLED this was written against does exactly that,
 * moving contact 0 forever while the real finger arrives as touch and release
 * on contact 1, whose own coordinates are always reported as 0,0. Deriving
 * "pressed" from anything other than TOUCH and RELEASE therefore pins the
 * pointer to permanently down.
 */
/**
 * Feed touchscreen input into the pointer indev.
 *
 * Screen reports a touch panel as SCREEN_EVENT_MTOUCH_*, a distinct event type
 * from SCREEN_EVENT_POINTER - a mouse and a touchscreen do not arrive through
 * the same path. Without this, touch events are read off the queue and silently
 * discarded, and the UI appears to work perfectly while ignoring every finger.
 *
 * WHAT THE PANEL ACTUALLY DOES
 *
 * Captured from the Waveshare 5.5" AMOLED with ZENBOX_TOUCH_TRACE=1. One tap:
 *
 *     MT TOUCH   id=1  0,0
 *     MT MOVE    id=0  385,655
 *     MT RELEASE id=1  0,0
 *     MT MOVE    id=0  385,655     (repeated 5-7 times, after the release)
 *
 * Two contacts, with different jobs. **id=1 is the press bracket**: TOUCH and
 * RELEASE arrive exactly once each per tap and are entirely dependable - but
 * they carry 0,0 rather than a coordinate. **id=0 is the position channel**: it
 * reports where the finger is, and goes on repeating the last position for a
 * few events after the release.
 *
 * So press state comes from TOUCH/RELEASE and position comes from MOVE, and the
 * only real difficulty is that they arrive in that order: the press bracket
 * opens before any coordinate exists for it.
 *
 * Hence `pending`. A TOUCH opens a contact but does not press anything; the
 * first MOVE that follows supplies the coordinate and presses at it. Every
 * later MOVE is a drag. RELEASE ends it.
 *
 * Three earlier versions of this got it wrong in ways worth recording, because
 * each produced a symptom that pointed somewhere else entirely:
 *
 *  - Pressing on TOUCH used the *previous* contact's coordinate, so the first
 *    tap in a new region operated whichever widget you touched last. With a
 *    button beside a fader that reads as the hit regions overlapping.
 *  - Treating MOVE as "a finger is down" made the trailing post-release moves
 *    re-press the widget, so one tap produced two clicks - play, then stop.
 *  - Suppressing those trailing moves with an armed/disarmed flag fixed taps
 *    and broke drags, because a drag's own events were caught by the same
 *    guard.
 *
 * None of those is a hit-testing problem, and all three looked like one.
 */
/**
 * Feed touchscreen input into the pointer indev.
 *
 * Screen reports a touch panel as SCREEN_EVENT_MTOUCH_*, a distinct event type
 * from SCREEN_EVENT_POINTER, so without this touch events are read off the
 * queue and silently discarded and the UI ignores every finger.
 *
 * WHAT THE PANEL ACTUALLY DOES
 *
 * Captured from the Waveshare 5.5" AMOLED with ZENBOX_TOUCH_TRACE=1. The two
 * gestures do not look alike, and that is the whole difficulty.
 *
 *   tap      MT TOUCH   id=1  0,0
 *            MT MOVE    id=0  368,663
 *            MT RELEASE id=1  0,0
 *            MT MOVE    id=0  368,663      x5, same position, after the release
 *
 *   drag     MT MOVE    id=0  888,564
 *            MT MOVE    id=0  890,474      ... a long stream, positions moving
 *            MT MOVE    id=0  864,741      trailing repeats as the finger lifts
 *                                          - no TOUCH, and no RELEASE at all
 *
 * So **contact 0 is the truth**: a MOVE stream means a finger is on the glass,
 * and silence means it is not. Contact 1's TOUCH/RELEASE is a bracket that
 * appears for a tap and is simply absent for a drag - useful when it arrives,
 * impossible to depend on.
 *
 * Hence: MOVE presses and carries position; RELEASE is honoured when it comes;
 * a gap in the MOVE stream lifts the contact when it does not. The one extra
 * rule is the quiet window after a RELEASE, which discards the trailing repeats
 * for a finger already lifted.
 *
 * Four earlier versions got this wrong, each with a symptom that pointed
 * somewhere else entirely - recorded because the next panel will do something
 * equally strange:
 *
 *  - Pressing on TOUCH used the *previous* contact's coordinate, because TOUCH
 *    carries 0,0. The first tap in a new region operated whichever widget you
 *    touched last, which reads exactly like overlapping hit regions.
 *  - Nothing but RELEASE clearing the button left the pointer stuck down when a
 *    release never came, so LVGL kept the last widget captured and every later
 *    touch anywhere drove it. Touching the button moved the fader.
 *  - Letting the trailing post-release MOVEs re-press produced two clicks from
 *    one tap: play, then immediately stop.
 *  - Requiring a TOUCH to arm the press fixed taps and broke drags outright,
 *    because a drag never sends one.
 */
static bool handle_mtouch_event(lv_display_t * disp, screen_event_t event, int type)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    if(dsc->pointer == NULL) return true;

    lv_qnx_pointer_t * ptr_dsc = lv_indev_get_driver_data(dsc->pointer);
    const uint32_t now = get_ticks();

    {
        static int trace = -1;
        if(trace < 0) trace = getenv("ZENBOX_TOUCH_TRACE") != NULL;
        if(trace) {
            int id = -1, p[2] = { -1, -1 };
            screen_get_event_property_iv(event, SCREEN_PROPERTY_TOUCH_ID, &id);
            screen_get_event_property_iv(event, SCREEN_PROPERTY_SOURCE_POSITION, p);
            fprintf(stderr, "MT %-7s id=%d  %d,%d  buttons=%d\n",
                    type == SCREEN_EVENT_MTOUCH_TOUCH ? "TOUCH" :
                    type == SCREEN_EVENT_MTOUCH_MOVE  ? "MOVE"  : "RELEASE",
                    id, p[0], p[1], ptr_dsc->buttons);
        }
    }

    /* TOUCH carries 0,0 on this panel, so it can neither position nor press
     * anything. The MOVE that follows does both. */
    if(type == SCREEN_EVENT_MTOUCH_TOUCH) {
        return true;
    }

    if(type == SCREEN_EVENT_MTOUCH_RELEASE) {
        ptr_dsc->buttons = 0;
        ptr_dsc->quiet_until_ms = now + LV_QNX_TOUCH_QUIET_MS;
        lv_indev_read(dsc->pointer);
        return true;
    }

    /* MOVE: the position is always worth taking, so the pointer readout stays
     * honest even for events that must not press. */
    if(screen_get_event_property_iv(event, SCREEN_PROPERTY_SOURCE_POSITION,
                                    ptr_dsc->pos) != 0) {
        LV_LOG_ERROR("screen_get_event_property_iv(SOURCE_POSITION): %s", strerror(errno));
        return false;
    }
    ptr_dsc->last_move_ms = now;
    ptr_dsc->buttons_from_mtouch = true;

    /* Inside the quiet window this is the tail of a finger already lifted.
     * Report the position, press nothing. */
    if((int32_t)(now - ptr_dsc->quiet_until_ms) < 0) {
        lv_indev_read(dsc->pointer);
        return true;
    }

    /* A contact has no buttons; map it to the left mouse button so
     * get_pointer() needs no knowledge of where the event came from. */
    ptr_dsc->buttons = SCREEN_LEFT_MOUSE_BUTTON;
    lv_indev_read(dsc->pointer);
    return true;
}

static void get_key(lv_indev_t * indev, lv_indev_data_t * data)
{
    lv_qnx_keyboard_t * dsc = lv_indev_get_driver_data(indev);

    if((dsc->flags & KEY_DOWN) != 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->key = dsc->key;
    }
    else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static bool handle_keyboard_event(lv_display_t * disp, screen_event_t event)
{
    lv_qnx_window_t * dsc = lv_display_get_driver_data(disp);
    if(dsc->keyboard == NULL) return true;

    lv_qnx_keyboard_t * kbd_dsc = lv_indev_get_driver_data(dsc->keyboard);

    /*Get event data*/
    if(screen_get_event_property_iv(event, SCREEN_PROPERTY_FLAGS,
                                    &kbd_dsc->flags)
       != 0) {
        LV_LOG_ERROR("screen_get_event_property_iv(FLAGS): %s", strerror(errno));
        return false;
    }

    if(screen_get_event_property_iv(event, SCREEN_PROPERTY_SYM,
                                    &kbd_dsc->key)
       != 0) {
        LV_LOG_ERROR("screen_get_event_property_iv(SYM): %s", strerror(errno));
        return false;
    }

    /*Translate special keys*/
    switch(kbd_dsc->key) {
        case KEYCODE_UP:
            kbd_dsc->key = LV_KEY_UP;
            break;

        case KEYCODE_DOWN:
            kbd_dsc->key = LV_KEY_DOWN;
            break;

        case KEYCODE_LEFT:
            kbd_dsc->key = LV_KEY_LEFT;
            break;

        case KEYCODE_RIGHT:
            kbd_dsc->key = LV_KEY_RIGHT;
            break;

        case KEYCODE_RETURN:
            kbd_dsc->key = LV_KEY_ENTER;
            break;

        case KEYCODE_BACKSPACE:
            kbd_dsc->key = LV_KEY_BACKSPACE;
            break;

        case KEYCODE_HOME:
            kbd_dsc->key = LV_KEY_HOME;
            break;

        case KEYCODE_END:
            kbd_dsc->key = LV_KEY_END;
            break;

        case KEYCODE_DELETE:
            kbd_dsc->key = LV_KEY_DEL;
            break;

        default:
            /*Ignore other non-ASCII keys, including modifiers*/
            if(kbd_dsc->key > 0xff) return true;
    }

    lv_indev_read(dsc->keyboard);
    return true;
}

static void refresh_cb(lv_timer_t * timer)
{
    /*Refresh the window on timeout, but disable the timer. Any callback can
     *re-enable it.*/
    lv_display_t * disp = timer->user_data;
    lv_refr_now(disp);
    lv_timer_pause(timer);
}

#endif /*LV_USE_QNX*/
