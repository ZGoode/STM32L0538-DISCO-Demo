/**
 * @file    gfx.c
 * @brief   Text rendering on top of the display-independent epaper API.
 */

/* Includes -----------------------------------------------------------------*/
#include "gfx.h"
#include "epaper.h"
#include "font5x7.h"
#include <string.h>

/* Private functions --------------------------------------------------------*/

static void gfx_char(uint16_t x, uint16_t y, char c, uint8_t scale, uint8_t color)
{
  if (c < FONT5X7_FIRST || c > FONT5X7_LAST) c = '?';
  const uint8_t *g = &font5x7[(c - FONT5X7_FIRST) * 5];

  /* Glyph columns are 8 bits tall (row 7 carries descenders: g j p q y). */
  for (uint8_t col = 0; col < 5; col++)
    for (uint8_t row = 0; row < 8; row++)
      if (g[col] & (1U << row))
        epaper_fill_rect(x + col * scale, y + row * scale, scale, scale, color);
}

/* Public functions ---------------------------------------------------------*/

uint16_t gfx_text_width(const char *s, uint8_t scale)
{
  return (uint16_t)(strlen(s) * GFX_CHAR_W * scale);
}

void gfx_text(uint16_t x, uint16_t y, const char *s, uint8_t scale, uint8_t color)
{
  for (; *s; s++, x += GFX_CHAR_W * scale)
    gfx_char(x, y, *s, scale, color);
}

void gfx_text_centered(uint16_t y, const char *s, uint8_t scale, uint8_t color)
{
  uint16_t w = gfx_text_width(s, scale);
  uint16_t x = (w < epaper_width()) ? (uint16_t)((epaper_width() - w) / 2U) : 0U;
  gfx_text(x, y, s, scale, color);
}
