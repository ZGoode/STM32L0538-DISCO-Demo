/**
 * @file    epaper.h
 * @brief   Display-independent e-paper API for the STM32L0538-DISCO.
 *
 * Two display variants are supported (GDE021A1, 172x72, and GDEY0213B74,
 * 250x122). The variant is selected by the EPAPER_DISPLAY define at the top
 * of epaper.c; nothing in this header depends on it. Query the size at
 * runtime with epaper_width() / epaper_height().
 *
 * Coordinates: (0,0) is the top-left corner of the logical landscape screen.
 * Drawing goes into a RAM framebuffer; nothing appears on the panel until
 * one of the update functions is called.
 */
#ifndef EPAPER_H
#define EPAPER_H

/* Includes -----------------------------------------------------------------*/
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported constants -------------------------------------------------------*/

#define EPAPER_WHITE 0
#define EPAPER_BLACK 1

/*
 * Two additional shades, usable with every drawing call. They appear as real
 * gray only through epaper_update_gray(); a plain epaper_update() renders
 * them as the nearest black/white (DARK -> black, LIGHT -> white).
 * Both panels offer 4 levels at best.
 */
#define EPAPER_GRAY_LIGHT 2
#define EPAPER_GRAY_DARK  3

/*
 * Drive-length color for epaper_update_gray16(): EPAPER_FRAMES(n), n = 0..63,
 * selects a per-pixel drive state, roughly "frames driven from white toward
 * black". Outside epaper_update_gray16() the value maps to the nearest of the
 * four native shades.
 */
#define EPAPER_FRAMES_BASE 0x40
#define EPAPER_FRAMES(n)   ((uint8_t)(EPAPER_FRAMES_BASE + (n)))

/* Number of selectable drive levels of the fast-refresh paths. */
#define EPAPER_FAST_LEVELS    7U
#define EPAPER_EXTREME_LEVELS EPAPER_FAST_LEVELS

/* Exported functions prototypes --------------------------------------------*/

/* ---- Initialization and power ------------------------------------------- */

/**
 * @brief Initialize the display GPIOs and controller.
 *
 * SPI1 must already be initialized (MX_SPI1_Init).
 * @return false if the display never became ready (BUSY timeout).
 */
bool epaper_init(void);

/**
 * @brief Remove the display supply and park its pins, for use before STOP mode.
 *
 * The image stays on the glass while the display is unpowered.
 */
void epaper_power_off(void);

/**
 * @brief Restore the display supply and re-initialize the panel.
 *
 * The framebuffer content is kept, so epaper_update() can be called
 * immediately. Re-initializes SPI1 through MX_SPI1_Init().
 * @return false on BUSY timeout.
 */
bool epaper_power_on(void);

/* ---- Display information ------------------------------------------------ */

/** @brief Name and resolution of the configured display, e.g. "GDE021A1 (172x72)". */
const char *epaper_name(void);

/** @brief Value of EPAPER_DISPLAY: 0 = GDE021A1, 1 = GDEY0213B74. */
uint8_t  epaper_display_id(void);

/** @brief Logical screen width in pixels. */
uint16_t epaper_width(void);

/** @brief Logical screen height in pixels. */
uint16_t epaper_height(void);

/* ---- Framebuffer drawing (RAM only) ------------------------------------- */

void epaper_clear(uint8_t color);
void epaper_set_pixel(uint16_t x, uint16_t y, uint8_t color);   /* out-of-range pixels are ignored */
void epaper_hline(uint16_t x, uint16_t y, uint16_t len, uint8_t color);
void epaper_vline(uint16_t x, uint16_t y, uint16_t len, uint8_t color);
void epaper_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color);
void epaper_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color);
void epaper_circle(uint16_t cx, uint16_t cy, uint16_t r, uint8_t color);
void epaper_fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint8_t color);

/* ---- Display updates ---------------------------------------------------- */

/**
 * @brief Send the framebuffer to the panel and refresh (black/white).
 *
 * Blocks for roughly 1-3 s. If the panel is in a gray or fast-refresh
 * configuration, it is first re-initialized for the normal waveform.
 * @return false on BUSY timeout (typical symptom of the wrong display
 *         selection, or of wiring/power problems).
 */
bool epaper_update(void);

/**
 * @brief 4-level gray update: black, EPAPER_GRAY_DARK, EPAPER_GRAY_LIGHT, white.
 *
 * @p draw must paint the COMPLETE frame using epaper_*() / gfx_*() calls. It is
 * called twice (once per bit of the 2-bit shade), so it must not depend on
 * state that changes between the calls. This keeps the framebuffer at 1 bit
 * per pixel: a 2 bpp 250x122 buffer (7.6 KB) does not fit in the
 * STM32L053's 8 KB of RAM.
 *
 * Takes roughly 2-3 s and flashes like a full refresh. The next epaper_update()
 * switches the panel back to its normal black/white waveform. Afterwards the
 * framebuffer holds the black/white version of the frame.
 * @return false on BUSY timeout or if @p draw is NULL.
 */
bool epaper_update_gray(void (*draw)(void));

/**
 * @brief Experimental multi-pass gray update using EPAPER_FRAMES(n) colors.
 *
 * Same draw() contract as epaper_update_gray(), but pixels are painted with
 * EPAPER_FRAMES(n) instead of the four fixed shades. The controller lets one
 * refresh select only 1 of 4 drive lengths per pixel, so n is split into
 * base-4 digits (weights 16, 4, 1) and applied over 1-3 refreshes; the first
 * refresh also resets the panel to white. draw() is therefore called
 * 2 x passes times and must paint the same, complete frame every time.
 *
 * @param tmax Largest n that draw() uses; it sizes the waveform (max 63).
 *
 * On the GDEY0213B74 the resulting brightness is not monotonic in n (the later,
 * shorter refreshes are much stronger per frame than the first), so callers
 * should order states by measured brightness (see g16_ramp in app.c).
 * The classic colors (EPAPER_BLACK, EPAPER_GRAY_DARK, EPAPER_GRAY_LIGHT,
 * EPAPER_WHITE) remain usable inside draw() and map to tmax / 7 / 2 / 0 frames.
 *
 * Takes about 3-5 s. On the GDE021A1 this falls back to epaper_update_gray().
 * @return false on BUSY timeout or if @p draw is NULL.
 */
bool epaper_update_gray16(void (*draw)(void), uint8_t tmax);

/* ---- Experimental fast refresh: epaper_fast_* --------------------------- */

/*
 * Maximum-framerate path, GDEY0213B74 only (on the GDE021A1 begin() returns
 * false). It bypasses the normal update path:
 *   - SPI at 8 MHz (the SSD1680 write specification is about 10 MHz),
 *   - one bulk SPI transfer per RAM,
 *   - analog supply kept on between frames,
 *   - a custom waveform that drives each pixel for only a few frames.
 * Image quality is deliberately poor at the fast levels.
 *
 * Usage: begin(); then per frame pattern() -> frame_start() -> poll busy() ->
 * optionally set_level(); finally end(), which restores the normal SPI speed
 * and makes the next epaper_update() fully re-initialize the panel. Call end()
 * even if begin() returned false. Level 0 is the longest drive (best
 * contrast); higher levels drive for fewer panel frames (faster).
 */
bool    epaper_fast_begin(void);
bool    epaper_fast_set_level(uint8_t level);          /* only while not busy */
uint8_t epaper_fast_level_frames(uint8_t level);       /* drive length in panel frames */
void    epaper_fast_pattern(uint8_t phase);            /* fills the framebuffer with 10 bars, inverted by phase 0/1 */
void    epaper_fast_frame_start(void);                 /* sends the framebuffer and starts the refresh; returns before it completes */
bool    epaper_fast_busy(void);                        /* true while the panel is refreshing */
void    epaper_fast_end(void);

/* ---- Experimental fast refresh: epaper_extreme_* ------------------------ */

/*
 * Second maximum-framerate path, GDEY0213B74 only (begin() returns false on
 * the GDE021A1). It has the same usage pattern as epaper_fast_*() and shares
 * its implementation: begin(); then per frame pattern() -> frame_start() ->
 * poll busy() -> optionally set_level(); finally end() (also after a failed
 * begin()). Use either path at a time, not both.
 *
 * Differences from epaper_fast_*():
 *   - Only the B/W RAM is written. The RED RAM write is skipped because the
 *     waveform defines identical LUT entries for the BW- and RED-driven
 *     cases, so the RAMs never need to differ (see epaper.c).
 *   - SPI runs at 16 MHz, above the SSD1680 write specification of about
 *     10 MHz. This is the fastest setting of the SPI clock divider at the
 *     32 MHz bus clock.
 *   - The drive lengths of the levels are the same as in epaper_fast_*().
 *
 * Contrast is expected to be worse than epaper_fast_*() at the same level,
 * and SPI link reliability at 16 MHz is not guaranteed.
 */
bool     epaper_extreme_begin(void);
bool     epaper_extreme_set_level(uint8_t level);
uint8_t  epaper_extreme_level_frames(uint8_t level);   /* drive length in panel frames */
void     epaper_extreme_pattern(uint8_t phase);
void     epaper_extreme_frame_start(void);
bool     epaper_extreme_busy(void);
void     epaper_extreme_end(void);
uint32_t epaper_extreme_spi_hz(void);                  /* SPI clock used by this path in Hz, for display (0 if unavailable) */

#ifdef __cplusplus
}
#endif
#endif /* EPAPER_H */
