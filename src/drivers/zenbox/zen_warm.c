#include "zen_warm.h"

/* Blackbody whitepoint gains, normalised so red is always 1.0.
 *
 * Redshift's colour-ramp table sampled at 500 K - the same table f.lux-style
 * tools use, and the same one a vc4 CTM would need.
 *
 * PROVENANCE, because it matters here: rows at or below 5000 K are transcribed
 * from Redshift. The 5000-6500 K segment is a linear interpolation to identity
 * and is NOT Redshift's own curve. That region is near-identity, so the largest
 * error it can introduce is under one part in 256 - invisible - but it is an
 * approximation and should not be cited as sourced. If it ever matters,
 * transcribe the real rows from Redshift's colorramp.c.
 *
 * 6500 K is *defined* as neutral: the panel's native white, unfiltered. We are
 * not colour-managing the panel, only attenuating from wherever it already
 * sits.
 */
static const struct { int k; float g[3]; } warm_table[] = {
    { 1000, { 1.0f, 0.18173f, 0.00000f } },
    { 1500, { 1.0f, 0.42323f, 0.00000f } },
    { 2000, { 1.0f, 0.54360f, 0.08680f } },
    { 2500, { 1.0f, 0.64373f, 0.28820f } },
    { 3000, { 1.0f, 0.71977f, 0.42860f } },
    { 3500, { 1.0f, 0.77988f, 0.54642f } },
    { 4000, { 1.0f, 0.82855f, 0.64817f } },
    { 4500, { 1.0f, 0.86861f, 0.73689f } },
    { 5000, { 1.0f, 0.90198f, 0.81466f } },
    { 6500, { 1.0f, 1.00000f, 1.00000f } },
};

#define WARM_ROWS ((int)(sizeof warm_table / sizeof warm_table[0]))

/* Quantise an n-bit channel through the gain at 8-bit precision and back.
 *
 * Not gain applied to the 5-bit value directly: at 5 bits the steps are 8
 * levels apart in 8-bit terms, and rounding a small index through a 0.35 gain
 * collapses the bottom of the range into a hard step. Widening to 8 bits first
 * costs nothing here - this runs once per temperature change - and keeps the
 * dark end as smooth as RGB565 can express.
 */
static unsigned quantise(unsigned idx, unsigned bits, float gain)
{
    unsigned max = (1u << bits) - 1u;
    float v8 = (float)idx * 255.0f / (float)max;
    float g8 = v8 * gain;
    unsigned out = (unsigned)(g8 * (float)max / 255.0f + 0.5f);
    return out > max ? max : out;
}

void zen_warm_build(zen_warm_t * w, int kelvin)
{
    float g[3];
    int i, c;

    if(kelvin < warm_table[0].k) kelvin = warm_table[0].k;
    if(kelvin > warm_table[WARM_ROWS - 1].k) kelvin = warm_table[WARM_ROWS - 1].k;
    w->kelvin = kelvin;

    /* Bracket, stopping one short of the end so i+1 is always in range. */
    for(i = 0; i < WARM_ROWS - 2 && warm_table[i + 1].k < kelvin; i++) { }

    {
        float span = (float)(warm_table[i + 1].k - warm_table[i].k);
        float f = span > 0.0f ? (float)(kelvin - warm_table[i].k) / span : 0.0f;
        for(c = 0; c < 3; c++) {
            g[c] = warm_table[i].g[c] + f * (warm_table[i + 1].g[c] - warm_table[i].g[c]);
        }
    }

    /* The gain multiplies the sRGB-encoded value, not light-linear intensity.
     * That is what Redshift does to a gamma ramp and what a CTM does to the
     * framebuffer, so this matches the thing it stands in for. A linear-light
     * version would warm noticeably harder at the same Kelvin - worth knowing
     * if a stop ever feels too mild. */
    for(c = 0; c < 3; c++) {
        for(i = 0; i < 256; i++) {
            unsigned v = (unsigned)((float)i * g[c] + 0.5f);
            w->lut8[c][i] = (uint8_t)(v > 255u ? 255u : v);
        }
    }

    for(i = 0; i < 32; i++) {
        w->lut565_r[i] = (uint16_t)(quantise((unsigned)i, 5, g[0]) << 11);
        w->lut565_b[i] = (uint16_t)(quantise((unsigned)i, 5, g[2]));
    }
    for(i = 0; i < 64; i++) {
        w->lut565_g[i] = (uint16_t)(quantise((unsigned)i, 6, g[1]) << 5);
    }

    w->active = (g[1] < 1.0f || g[2] < 1.0f);
}

void zen_warm_row_argb8888(const zen_warm_t * w, void * dst, const void * src, size_t px)
{
    const uint32_t * s = (const uint32_t *)src;
    uint32_t * d = (uint32_t *)dst;
    size_t x;

    /* Whole 32-bit words, so byte order never has to be reasoned about: both
     * QNX SCREEN_FORMAT_RGBA8888 and LVGL at colour depth 32 mean 0xAARRGGBB in
     * a native word. Alpha carries through untouched. */
    for(x = 0; x < px; x++) {
        uint32_t p = s[x];
        d[x] = (p & 0xff000000u)
               | ((uint32_t)w->lut8[0][(p >> 16) & 0xffu] << 16)
               | ((uint32_t)w->lut8[1][(p >>  8) & 0xffu] <<  8)
               | ((uint32_t)w->lut8[2][ p        & 0xffu]);
    }
}

void zen_warm_row_rgb565(const zen_warm_t * w, void * dst, const void * src, size_t px)
{
    const uint16_t * s = (const uint16_t *)src;
    uint16_t * d = (uint16_t *)dst;
    uint16_t prev_in = 0, prev_out;
    size_t x;

    if(px == 0) return;

    /* Memoise the last pixel. This is not a micro-optimisation looking for a
     * problem: the target for RGB565 is a 1 GHz ARM1176 with no NEON, where a
     * full-screen pass is expensive enough to matter against a 35 ms frame. UI
     * content is mostly flat fill - backgrounds, button bodies, margins - so
     * long runs of identical pixels are the common case, and a compare plus a
     * predicted branch is much cheaper than three table lookups. Text and
     * gradients fall through to the full path and cost slightly more than they
     * would without the check. */
    prev_out = (uint16_t)(w->lut565_r[(prev_in >> 11) & 0x1fu]
                          | w->lut565_g[(prev_in >> 5) & 0x3fu]
                          | w->lut565_b[prev_in & 0x1fu]);

    for(x = 0; x < px; x++) {
        uint16_t p = s[x];
        if(p != prev_in) {
            prev_in = p;
            prev_out = (uint16_t)(w->lut565_r[(p >> 11) & 0x1fu]
                                  | w->lut565_g[(p >> 5) & 0x3fu]
                                  | w->lut565_b[p & 0x1fu]);
        }
        d[x] = prev_out;
    }
}
