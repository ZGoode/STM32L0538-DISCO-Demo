/**
 * @file    gfx.h
 * @brief   Text rendering on top of the display-independent epaper API.
 *
 * Uses a fixed 5x7 font (font5x7.h) scaled by an integer factor.
 */
#ifndef GFX_H
#define GFX_H

/* Includes -----------------------------------------------------------------*/
#include <stdint.h>

/* Exported constants -------------------------------------------------------*/
#define GFX_CHAR_W 6U   /* Character cell width at scale 1: 5 px glyph + 1 px gap */
#define GFX_CHAR_H 8U   /* Character cell height at scale 1: 8 px rows, including descenders */

/* Exported functions prototypes --------------------------------------------*/

/**
 * @brief Draw a string with its top-left corner at (x, y).
 * @param scale Integer magnification of the font (1 = 5x7 glyphs).
 * @param color An EPAPER_* color.
 *
 * Characters outside printable ASCII are drawn as '?'.
 */
void gfx_text(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint8_t color);

/** @brief Draw a string horizontally centered on the display at row y. */
void gfx_text_centered(uint16_t y, const char *s, uint8_t scale, uint8_t color);

/** @brief Return the rendered width of a string in pixels. */
uint16_t gfx_text_width(const char *s, uint8_t scale);

#endif /* GFX_H */
