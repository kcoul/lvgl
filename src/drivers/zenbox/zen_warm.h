/*
 * Warm (low-blue) display filter - the f.lux / Redshift effect, applied per
 * pixel on the way to the framebuffer.
 *
 * Shared by the QNX Screen and Linux fbdev display clients because there is
 * exactly one thing here worth getting right - the Kelvin-to-gain table - and
 * two copies of it would drift. Deliberately free of any LVGL or platform
 * dependency: it is plain C over a pixel buffer, so the same table can be
 * lifted into a vc4 CTM tool if the display-controller route in
 * docs/qnx-blue-light-filter.md ever opens up.
 *
 * Why this exists at the application layer at all, rather than in the display
 * controller where f.lux and Redshift put it, is the subject of that document.
 * The short version: the QNX drm-rpi5 CRTCs register no colour management, and
 * the obvious fallback - a translucent amber overlay - is an *additive* blend,
 * which on an AMOLED lights every black pixel. This is a multiply, so black
 * stays black.
 */

#ifndef ZEN_WARM_H
#define ZEN_WARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZEN_WARM_NEUTRAL 6500   /* identity; the panel's native white */
#define ZEN_WARM_MIN     1000

typedef struct {
    /* Per-channel tables rather than a multiply. Same cost to apply, and they
     * can hold any curve - a per-panel calibration, or a gamma-correct version
     * of this one - without the copy loops needing to know. */
    uint8_t  lut8[3][256];      /* 8 bits per channel: R, G, B */
    uint16_t lut565_r[32];      /* pre-shifted into place for RGB565, so the */
    uint16_t lut565_g[64];      /* apply step is three loads and two ORs */
    uint16_t lut565_b[32];
    bool     active;            /* false at 6500 K: callers take a memcpy path */
    int      kelvin;
} zen_warm_t;

/**
 * Build the lookup tables for a colour temperature.
 * Clamped to ZEN_WARM_MIN..ZEN_WARM_NEUTRAL. Sets w->active false for neutral,
 * so an unfiltered display can keep its plain-memcpy path and go on measuring
 * exactly as it did before this existed.
 */
void zen_warm_build(zen_warm_t * w, int kelvin);

/**
 * Apply to one row. Both are safe with dst == src (in-place).
 *
 * argb8888 covers XRGB8888 too: the top byte is copied through untouched.
 * Neither checks w->active - the caller does, and skips to memcpy.
 */
void zen_warm_row_argb8888(const zen_warm_t * w, void * dst, const void * src, size_t px);
void zen_warm_row_rgb565(const zen_warm_t * w, void * dst, const void * src, size_t px);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZEN_WARM_H */
