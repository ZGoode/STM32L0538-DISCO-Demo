/**
 * @file    epaper.c
 * @brief   E-paper driver for the STM32L0538-DISCO, supporting both display
 *          variants found on different board revisions.
 *
 * The display-specific parts (init sequence, waveform, RAM format, refresh
 * command, timing) are isolated in the "#if EPAPER_DISPLAY" sections below.
 * The public API in epaper.h is identical for both variants.
 *
 * Provenance: the GDE021A1 sequence and waveform follow ST's gde021a1.c
 * (STM32Cube BSP, ST license). The GDEY0213B74 sequence follows the
 * SSD1680-style command set (ST's gdem0213b74.c init and the Good Display
 * demo); see the notes in the GDEY0213B74 section.
 *
 * 4-gray: both panels reach 4 levels per refresh at best; neither controller
 * has a 16-level or multi-color mode. epaper_update_gray() provides the 4
 * native levels. epaper_update_gray16() stacks several refreshes to obtain
 * more drive levels (GDEY0213B74 only).
 */

/* Includes -----------------------------------------------------------------*/
#include "epaper.h"
#include "main.h"      /* HAL */
#include "spi.h"       /* extern SPI_HandleTypeDef hspi1 */
#include <string.h>

/* Private defines ----------------------------------------------------------*/

/* Display selection: set exactly one of the following, rebuild and flash.
 *
 *   0 = GDE021A1    (172 x 72)
 *   1 = GDEY0213B74 (250 x 122)
 */
#define EPAPER_DISPLAY 1

/* Orientation (0 or 1). If the image appears mirrored on the glass, flip
 * these; they only affect where pixels land in the framebuffer. */
#define EPAPER_FLIP_X 1
#define EPAPER_FLIP_Y 0

/* Maximum time to wait for BUSY to clear before giving up. */
#define EPAPER_BUSY_TIMEOUT_MS 15000U

/* Timeout of a single SPI transfer. */
#define EPAPER_SPI_TIMEOUT_MS 100U

/* Build-time confirmation of the selected display. */
#if EPAPER_DISPLAY == 0
  #pragma message ("E-PAPER BUILD: EPAPER_DISPLAY = 0 -> GDE021A1")
#elif EPAPER_DISPLAY == 1
  #pragma message ("E-PAPER BUILD: EPAPER_DISPLAY = 1 -> GDEY0213B74")
#else
  #error "EPAPER_DISPLAY must be 0 (GDE021A1) or 1 (GDEY0213B74)"
#endif

/* Board wiring (identical for both displays, from ST's STM32L0538-DISCO BSP).
 *
 * SPI1: SCK = PB3, MOSI = PB5 (transmit only, one line).
 *
 * Port, pin, mode, pull, speed and idle level of the control lines are set by
 * CubeMX (MX_GPIO_Init); the EPD_xxx_Pin and EPD_xxx_GPIO_Port macros come
 * from main.h, generated from the GPIO labels set in the .ioc.
 */
#define EPAPER_SPI          hspi1
#define EPAPER_CS_PORT      EPD_CS_GPIO_Port
#define EPAPER_CS_PIN       EPD_CS_Pin
#define EPAPER_DC_PORT      EPD_DC_GPIO_Port
#define EPAPER_DC_PIN       EPD_DC_Pin
#define EPAPER_RST_PORT     EPD_RST_GPIO_Port
#define EPAPER_RST_PIN      EPD_RST_Pin
#define EPAPER_BUSY_PORT    EPD_BUSY_GPIO_Port
#define EPAPER_BUSY_PIN     EPD_BUSY_Pin     /* high = busy */
#define EPAPER_PWR_PORT     EPD_PWR_GPIO_Port
#define EPAPER_PWR_PIN      EPD_PWR_Pin      /* low = display powered */

/* Private variables --------------------------------------------------------*/

/* Framebuffer layout (both displays): 1 bit per pixel, 1 = white, 0 = black.
 * Stored as rows of the panel's native RAM: one row per logical X, each row
 * ROW_BYTES long covering logical Y, MSB first. */
#if EPAPER_DISPLAY == 0
  #define EPD_NAME   "GDE021A1 (172x72)"
  #define EPD_W      172U
  #define EPD_H      72U
#else
  #define EPD_NAME   "GDEY0213B74 (250x122)"
  #define EPD_W      250U
  #define EPD_H      122U
#endif

#define ROW_BYTES  ((EPD_H + 7U) / 8U)
static uint8_t fb[EPD_W * ROW_BYTES];      /* 1548 B (disp 0) / 4000 B (disp 1) */

/* 4-gray support. The framebuffer stays 1 bpp: a gray frame is rendered twice,
 * once per bit of the 2-bit shade code
 *     0 = black, 1 = dark gray, 2 = light gray, 3 = white
 * and each pass is pushed to the panel before the next is drawn (a second
 * 250x122 plane would not fit in the STM32L053's 8 KB RAM). set_pixel()/clear()
 * store the bit of the plane being rendered. In normal B/W operation that is
 * the high bit, so DARK -> black and LIGHT -> white. */
typedef enum { PLANE_HI, PLANE_LO } plane_t;
static plane_t cur_plane = PLANE_HI;
static bool    gray_mode;                  /* panel holds the 4-gray waveform */

/* Multi-pass gray (epaper_update_gray16()). While g16_active, a
 * colour is a drive length in frames (EPAPER_FRAMES(n)); the shade code handed
 * to the plane logic is 3 - (base-4 digit of n belonging to the current pass),
 * i.e. the same "LUT index = 3 - shade code" mapping as the 4-gray path. */
static bool    g16_active;
static uint8_t g16_shift;                  /* 2 * digit position of this pass */
static uint8_t g16_tmax;                   /* darkest drive length in use     */

static uint8_t g16_shade(uint8_t color)
{
  uint8_t f;
  if (color >= EPAPER_FRAMES_BASE)          f = (uint8_t)(color - EPAPER_FRAMES_BASE);
  else if (color == EPAPER_BLACK)           f = g16_tmax;
  else if (color == EPAPER_GRAY_DARK)       f = 7U;
  else if (color == EPAPER_GRAY_LIGHT)      f = 2U;
  else                                      f = 0U;      /* white */
  if (f > g16_tmax) f = g16_tmax;
  return (uint8_t)(3U - ((f >> g16_shift) & 3U));
}

static uint8_t shade_of(uint8_t color)
{
  if (g16_active) return g16_shade(color);
  if (color >= EPAPER_FRAMES_BASE)          /* outside gray16: nearest native shade */
  {
    uint8_t f = (uint8_t)(color - EPAPER_FRAMES_BASE);
    return (f == 0U) ? 3U : (f < 5U) ? 2U : (f < 15U) ? 1U : 0U;
  }
  switch (color)
  {
    case EPAPER_BLACK:      return 0U;
    case EPAPER_GRAY_DARK:  return 1U;
    case EPAPER_GRAY_LIGHT: return 2U;
    default:                return 3U;     /* EPAPER_WHITE and any unknown color */
  }
}

static uint8_t plane_bit(uint8_t color)    /* 1 = white in the plane being drawn */
{
  uint8_t s = shade_of(color);
  return (cur_plane == PLANE_LO) ? (uint8_t)(s & 1U) : (uint8_t)(s >> 1);
}

/* Private functions: low-level I/O (shared) -------------------------------*/

#define CS_LOW()    HAL_GPIO_WritePin(EPAPER_CS_PORT,  EPAPER_CS_PIN,  GPIO_PIN_RESET)
#define CS_HIGH()   HAL_GPIO_WritePin(EPAPER_CS_PORT,  EPAPER_CS_PIN,  GPIO_PIN_SET)
#define DC_LOW()    HAL_GPIO_WritePin(EPAPER_DC_PORT,  EPAPER_DC_PIN,  GPIO_PIN_RESET)
#define DC_HIGH()   HAL_GPIO_WritePin(EPAPER_DC_PORT,  EPAPER_DC_PIN,  GPIO_PIN_SET)
#define RST_LOW()   HAL_GPIO_WritePin(EPAPER_RST_PORT, EPAPER_RST_PIN, GPIO_PIN_RESET)
#define RST_HIGH()  HAL_GPIO_WritePin(EPAPER_RST_PORT, EPAPER_RST_PIN, GPIO_PIN_SET)
#define IS_BUSY()   (HAL_GPIO_ReadPin(EPAPER_BUSY_PORT, EPAPER_BUSY_PIN) != GPIO_PIN_RESET)

/* The pin modes (CS/DC/RST/PWR push-pull outputs, BUSY pulled-down input) are
 * configured once by MX_GPIO_Init(). This re-asserts the idle levels (PWR low =
 * display powered, CS and RST high); it is needed again after
 * epaper_power_off() has driven the pins low. */
static void epd_gpio_init(void)
{
  HAL_GPIO_WritePin(EPAPER_PWR_PORT, EPAPER_PWR_PIN, GPIO_PIN_RESET);
  CS_HIGH();
  RST_HIGH();
  HAL_Delay(10);
}

/* One command byte / one data byte; CS is toggled per byte, as in ST's BSP. */
static void epd_cmd(uint8_t c)
{
  CS_LOW(); DC_LOW();
  HAL_SPI_Transmit(&EPAPER_SPI, &c, 1, EPAPER_SPI_TIMEOUT_MS);
  CS_HIGH();
}

static void epd_data(uint8_t d)
{
  CS_LOW(); DC_HIGH();
  HAL_SPI_Transmit(&EPAPER_SPI, &d, 1, EPAPER_SPI_TIMEOUT_MS);
  CS_HIGH();
}

/* Wait until BUSY goes low. false on timeout. */
static bool epd_wait_busy(uint32_t timeout_ms)
{
  uint32_t t0 = HAL_GetTick();
  while (IS_BUSY())
  {
    if ((HAL_GetTick() - t0) > timeout_ms) return false;
  }
  return true;
}

/* ========================================================================== */
/*  DISPLAY 0: GDE021A1  (172 x 72, panel RAM is 2 bits/pixel)                */
/*  Follows ST gde021a1.c. RAM: 18 bytes (= 72 px) per row, 172 rows.         */
/* ========================================================================== */
#if EPAPER_DISPLAY == 0

static const uint8_t gde021a1_lut[90] = {
  0x82,0x00,0x00,0x00,0xAA,0x00,0x00,0x00,
  0xAA,0xAA,0x00,0x00,0xAA,0xAA,0xAA,0x00,
  0x55,0xAA,0xAA,0x00,0x55,0x55,0x55,0x55,
  0xAA,0xAA,0xAA,0xAA,0x55,0x55,0x55,0x55,
  0xAA,0xAA,0xAA,0xAA,0x15,0x15,0x15,0x15,
  0x05,0x05,0x05,0x05,0x01,0x01,0x01,0x01,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x41,0x45,0xF1,0xFF,0x5F,0x55,0x01,0x00,
  0x00,0x00
};

static bool panel_init(void)
{
  /* ST's sequence: display enabled, RESET held high (no reset pulse). */
  RST_HIGH();
  HAL_Delay(10);

  epd_cmd(0x10); epd_data(0x00);                 /* deep sleep off        */
  epd_cmd(0x11); epd_data(0x03);                 /* data entry mode       */
  epd_cmd(0x44); epd_data(0x00); epd_data(0x11); /* RAM "X": 18 x 4 px    */
  epd_cmd(0x45); epd_data(0x00); epd_data(0xAB); /* RAM "Y": 172 rows     */
  epd_cmd(0x4E); epd_data(0x00);
  epd_cmd(0x4F); epd_data(0x00);
  epd_cmd(0xF0); epd_data(0x1F);                 /* booster feedback      */
  epd_cmd(0x21); epd_data(0x03);                 /* RAM bypass / GS trans */
  epd_cmd(0x2C); epd_data(0xA0);                 /* VCOM                  */
  epd_cmd(0x3C); epd_data(0x64);                 /* border waveform       */
  epd_cmd(0x32);                                 /* LUT                   */
  for (uint8_t i = 0; i < sizeof(gde021a1_lut); i++) epd_data(gde021a1_lut[i]);
  return true;
}

static void panel_write_ram(void)
{
  epd_cmd(0x44); epd_data(0x00); epd_data(0x11);
  epd_cmd(0x45); epd_data(0x00); epd_data(0xAB);
  epd_cmd(0x4E); epd_data(0x00);
  epd_cmd(0x4F); epd_data(0x00);

  epd_cmd(0x24);                                 /* write RAM */
  for (uint16_t x = 0; x < EPD_W; x++)
  {
    const uint8_t *row = &fb[x * ROW_BYTES];
    for (uint8_t i = 0; i < ROW_BYTES; i++)
    {
      uint8_t b = row[i];
      for (uint8_t half = 0; half < 2; half++)   /* 8 px -> 2 RAM bytes */
      {
        uint8_t out = 0;
        for (uint8_t p = 0; p < 4; p++)          /* 2 bits/px, first px in MSBs */
        {
          if (b & 0x80) out |= (uint8_t)(0x3U << (6 - 2 * p));  /* white=11 */
          b <<= 1;                                              /* black=00 */
        }
        epd_data(out);
      }
    }
  }
}

static bool panel_refresh(void)
{
  epd_cmd(0x22); epd_data(0xC4);                 /* update sequence       */
  epd_cmd(0x20);                                 /* master activation     */
  bool ok = epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS);

  RST_HIGH();
  HAL_Delay(10);

  /* ST closes the charge pump after an update (then waits 400 ms). */
  epd_cmd(0x22); epd_data(0x03);
  epd_cmd(0x20);
  HAL_Delay(400);
  return ok;
}

static void panel_sleep(void)
{
  /* Nothing to do: the charge pump is already closed after every update.
   * The image stays on the glass with the display unpowered. */
}

/* --- 4-gray ---------------------------------------------------------------
 * Panel RAM already takes 2 bits per pixel: 00 black, 01 dark gray, 10 light
 * gray, 11 white (the codes ST's BSP uses for its EPD_COLOR_* values). The LUT
 * above is ST's, so gray needs no waveform change: the RAM simply gets both
 * shade bits instead of one bit replicated. */
static uint8_t fb_lo[sizeof(fb)];              /* low shade bit (1548 B) */

static bool panel_gray_begin(void)
{
  return true;                                 /* nothing to switch */
}

static void panel_gray_plane_done(plane_t plane)
{
  if (plane == PLANE_LO)                       /* keep until the high bit is drawn */
  {
    memcpy(fb_lo, fb, sizeof(fb));
    return;
  }

  /* High bit is in fb, low bit in fb_lo: same window/order as panel_write_ram() */
  epd_cmd(0x44); epd_data(0x00); epd_data(0x11);
  epd_cmd(0x45); epd_data(0x00); epd_data(0xAB);
  epd_cmd(0x4E); epd_data(0x00);
  epd_cmd(0x4F); epd_data(0x00);

  epd_cmd(0x24);                               /* write RAM */
  for (uint16_t n = 0; n < sizeof(fb); n++)
  {
    uint8_t h = fb[n], l = fb_lo[n];
    for (uint8_t half = 0; half < 2; half++)   /* 8 px -> 2 RAM bytes */
    {
      uint8_t out = 0;
      for (uint8_t p = 0; p < 4; p++)          /* 2 bits/px, first px in MSBs */
      {
        uint8_t px = (uint8_t)((((h >> 7) & 1U) << 1) | ((l >> 7) & 1U));
        out |= (uint8_t)(px << (6 - 2 * p));
        h <<= 1;
        l <<= 1;
      }
      epd_data(out);
    }
  }
}

static bool panel_gray_refresh(void)
{
  return panel_refresh();                      /* same update as B/W */
}

#endif /* EPAPER_DISPLAY == 0 */

/* ========================================================================== */
/*  DISPLAY 1: GDEY0213B74  (250 x 122, panel RAM is 1 bit/pixel)             */
/*  SSD1680-style command set, built-in waveform (no LUT upload).             */
/*  RAM: 16 bytes (=128 px, 122 used) per row, 250 rows.                      */
/*                                                                            */
/*  ST's rev-B03 BSP code (gdem0213b74.c, BSP_EPD_*) calls this panel         */
/*  "GDEM0213B74", reuses the GDE021A1 2-bit image packing, and uses window   */
/*  and clear counts that do not match the controller's RAM map. This         */
/*  section therefore uses the controller's native 1 bpp format instead.      */
/* ========================================================================== */
#if EPAPER_DISPLAY == 1

static bool panel_init(void)
{
  RST_LOW();  HAL_Delay(10);
  RST_HIGH(); HAL_Delay(10);
  if (!epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS)) return false;

  epd_cmd(0x12);                                 /* SWRESET */
  if (!epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS)) return false;

  epd_cmd(0x01); epd_data(0xF9); epd_data(0x00); epd_data(0x00); /* 250 gates */
  epd_cmd(0x3C); epd_data(0x05);                 /* border waveform       */
  epd_cmd(0x21); epd_data(0x00); epd_data(0x80); /* update ctrl 1         */
  epd_cmd(0x18); epd_data(0x80);                 /* internal temp sensor  */
  epd_cmd(0x11); epd_data(0x03);                 /* data entry: X+, Y+    */
  epd_cmd(0x44); epd_data(0x00); epd_data(0x0F); /* RAM X: 16 bytes       */
  epd_cmd(0x45); epd_data(0x00); epd_data(0x00); /* RAM Y: 0..249         */
                 epd_data(0xF9); epd_data(0x00);
  epd_cmd(0x4E); epd_data(0x00);
  epd_cmd(0x4F); epd_data(0x00); epd_data(0x00);
  return epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS);
}

static void panel_write_ram(void)
{
  epd_cmd(0x44); epd_data(0x00); epd_data(0x0F);
  epd_cmd(0x45); epd_data(0x00); epd_data(0x00); epd_data(0xF9); epd_data(0x00);

  /* Write the frame to both RAMs so the "red"/previous-frame plane never
   * contains stale data, whatever update-control option the controller uses. */
  for (uint8_t ram = 0; ram < 2; ram++)
  {
    epd_cmd(0x4E); epd_data(0x00);
    epd_cmd(0x4F); epd_data(0x00); epd_data(0x00);
    epd_cmd(ram == 0 ? 0x24 : 0x26);
    for (uint16_t i = 0; i < sizeof(fb); i++) epd_data(fb[i]);
  }
}

static bool panel_refresh(void)
{
  epd_cmd(0x22); epd_data(0xF7);                 /* full update sequence  */
  epd_cmd(0x20);                                 /* master activation     */
  return epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS);
}

static void panel_sleep(void)
{
  epd_cmd(0x10); epd_data(0x01);                 /* deep sleep mode 1     */
  HAL_Delay(10);
}

/* --- 4-gray ---------------------------------------------------------------
 * Good Display lists this panel as "black/white, 4 grayscale" (SSD1680Z).
 *
 * How the SSD1680 does it: for every pixel the two RAM planes form a 2-bit
 * value that selects the waveform, LUT index = 2 * RED_RAM + BW_RAM (datasheet
 * tables 6-4 / 6-5). The built-in OTP waveform makes LUT2 = LUT0 and
 * LUT3 = LUT1, which is why it only gives black/white; a waveform written
 * with command 0x32 can give all four LUTs different drive, i.e. 4 shades.
 * Found on the real panel: with this waveform the shades come out in the
 * opposite order (LUT0 = white ... LUT3 = black), so the planes are sent
 * inverted (LUT index = 3 - shade code). The B/W path is not affected: it uses
 * the panel's own OTP waveform.
 *
 * Waveform (153 bytes, one byte per LUT per group = 4 phases x 2 bits, with
 * 00 = VSS, 01 = VSH1, 10 = VSL): group 1 is a "shake" (VSH1 then VSL, twice)
 * common to all four LUTs that leaves every pixel black. Group 2 then drives
 * toward white for 0 / 2 / 7 / 27 frames (LUT0..LUT3) -> black, dark gray,
 * light gray, white. Group 0 pre-drives the opposite polarity by the same
 * amount for DC balance.
 *
 * SOURCE / CAVEAT: the byte values are the SSD1680 4-gray waveform used by
 * Adafruit's ThinkInk 2.9" (EAAMFGN) driver, which follows Good Display's
 * 4-gray demo. It was NOT tuned for this exact panel or for temperature, so
 * gray levels / ghosting / speed may need adjusting on real hardware. */
static const uint8_t ssd1680_gray4_lut[153] = {
  0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* VS LUT0 */
  0x20, 0x60, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* VS LUT1 */
  0x28, 0x60, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* VS LUT2 */
  0x2A, 0x60, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* VS LUT3 */
  0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* VS LUT4 (VCOM) */
  0x00, 0x02, 0x00, 0x05, 0x14, 0x00, 0x00,                                /* group 0: TP A,B SR TP C,D SR RP */
  0x1E, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x01,                                /* group 1 */
  0x00, 0x02, 0x00, 0x05, 0x14, 0x00, 0x00,                                /* group 2 */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                                /* group 3..11 unused */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x24, 0x22, 0x22, 0x22, 0x23, 0x32, 0x00, 0x00, 0x00                     /* FR (6), XON (3) */
};

static bool panel_gray_begin(void)
{
  if (!panel_init()) return false;             /* same reset + setup as B/W */

  /* Load the panel's own waveform setting once (temperature + LUT, no
   * display) so its calibrated gate / source / VCOM levels are in place,
   * then replace just the LUT with the 4-gray one. VCOM stays at the panel's
   * own OTP setting. */
  epd_cmd(0x22); epd_data(0xB1);
  epd_cmd(0x20);
  if (!epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS)) return false;

  epd_cmd(0x3F); epd_data(0x22);               /* option for LUT end: normal */
  epd_cmd(0x32);                               /* LUT */
  for (uint8_t i = 0; i < sizeof(ssd1680_gray4_lut); i++) epd_data(ssd1680_gray4_lut[i]);

  gray_mode = true;                            /* B/W update must re-init first */
  return true;
}

static void panel_gray_plane_done(plane_t plane)
{
  epd_cmd(0x44); epd_data(0x00); epd_data(0x0F);
  epd_cmd(0x45); epd_data(0x00); epd_data(0x00); epd_data(0xF9); epd_data(0x00);
  epd_cmd(0x4E); epd_data(0x00);
  epd_cmd(0x4F); epd_data(0x00); epd_data(0x00);
  epd_cmd(plane == PLANE_LO ? 0x24 : 0x26);    /* low bit -> BW RAM, high bit -> RED RAM */
  for (uint16_t i = 0; i < sizeof(fb); i++) epd_data((uint8_t)~fb[i]);   /* inverted, see note above */
}

static bool panel_gray_refresh(void)
{
  epd_cmd(0x22); epd_data(0xC7);               /* display, mode 1, keep the LUT above */
  epd_cmd(0x20);
  return epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS);
}

/* --- Multi-pass gray (epaper_update_gray16) ----------------------------------
 * Same waveform idea as ssd1680_gray4_lut above, but rebuilt per pass so that
 * LUT k (k = 0..3 = the pixel's 2-bit code for this pass) drives VSH1 (toward
 * black) for k * w frames, w = 1 / 4 / 16 depending on the pass's digit weight.
 * Adding the passes gives every drive length 0..63 frames.
 *
 *   pass 0     : pre-drive toward white (VSL, same length as its own drive, for
 *                DC balance) -> the same 2x VSH1/VSL "shake" as the 4-gray
 *                waveform, which leaves every pixel white -> VSH1 drive.
 *   later passes: VSH1 drive only, starting from whatever the pixel holds.
 *
 * The frame-rate nibble of the drive group is the same in every pass (0x2), so
 * a "frame" always has the same length. Everything else (voltages, VCOM, XON,
 * temperature) is the panel's own, as in the 4-gray path.
 *
 * Measured on the panel: the drive does NOT add up linearly across passes.
 * The last (weight 1) pass darkens far more per frame than the first
 * (weight 16), so state n is not monotonic in brightness; app.c sorts the
 * states by measured brightness (g16_ramp) instead of trusting n. */
static uint8_t g16_vs_bits(uint8_t steps, uint8_t v)   /* v: 1 = VSH1, 2 = VSL */
{
  uint8_t b = 0;
  for (uint8_t i = 0; i < 3U; i++)              /* phases A, B, C */
    if (steps > i) b |= (uint8_t)(v << (6U - 2U * i));
  return b;
}

static void panel_gray16_lut(uint8_t pass, uint8_t passes, uint8_t tmax)
{
  const uint8_t shift = (uint8_t)(2U * (passes - 1U - pass));
  const uint8_t w     = (uint8_t)(1U << shift);
  const uint8_t top   = (uint8_t)(tmax >> shift);
  const uint8_t dmax  = (top > 3U) ? 3U : top;
  const bool    first = (pass == 0U);

  epd_cmd(0x32);

  for (uint8_t k = 0; k < 5U; k++)              /* VS LUT0..LUT3, then LUT4 (VCOM) */
    for (uint8_t g = 0; g < 12U; g++)
    {
      uint8_t b = 0;
      if (k < 4U)
      {
        if (first && g == 0U)                                b = g16_vs_bits(k, 2U);
        else if (first && g == 1U)                           b = 0x60U;
        else if ((first && g == 2U) || (!first && g == 0U))  b = g16_vs_bits(k, 1U);
      }
      else if (first && g == 1U)                             b = 0x90U;
      epd_data(b);
    }

  for (uint8_t g = 0; g < 12U; g++)             /* TP A, TP B, SR, TP C, TP D, SR, RP */
  {
    uint8_t ta = 0, tb = 0, tc = 0, rp = 0;
    if (first && g == 1U)
    {
      ta = 30U; tb = 30U; rp = 1U;              /* shake, as in the 4-gray waveform */
    }
    else if ((first && (g == 0U || g == 2U)) || (!first && g == 0U))
    {
      ta = (dmax > 0U) ? w : 0U;
      tb = (dmax > 1U) ? w : 0U;
      tc = (dmax > 2U) ? w : 0U;
    }
    epd_data(ta); epd_data(tb); epd_data(0);
    epd_data(tc); epd_data(0);  epd_data(0); epd_data(rp);
  }

  epd_data(first ? 0x24U : 0x22U);              /* FR: frame rates, 6 bytes */
  epd_data(0x22); epd_data(0x22); epd_data(0x22); epd_data(0x23); epd_data(0x32);
  epd_data(0x00); epd_data(0x00); epd_data(0x00);   /* XON */
}

#endif /* EPAPER_DISPLAY == 1 */

/* ========================================================================== */
/*  Common API (identical for both displays)                                  */
/* ========================================================================== */
const char *epaper_name(void)       { return EPD_NAME; }
uint8_t     epaper_display_id(void) { return EPAPER_DISPLAY; }
uint16_t    epaper_width(void)      { return EPD_W; }
uint16_t    epaper_height(void)     { return EPD_H; }

static bool hw_init(void)
{
  epd_gpio_init();          /* also switches the display's supply on */
  gray_mode = false;        /* panel_init() loads the B/W configuration */
  return panel_init();
}

bool epaper_init(void)
{
  memset(fb, 0xFF, sizeof(fb));
  return hw_init();
}

/* Cut the display's supply (the image stays on the glass) and park the pins
 * so nothing back-feeds the unpowered panel. Use before STOP mode. */
void epaper_power_off(void)
{
  panel_sleep();
  HAL_SPI_DeInit(&EPAPER_SPI);                       /* SCK/MOSI -> analog */
  CS_LOW(); DC_LOW(); RST_LOW();
  HAL_GPIO_WritePin(EPAPER_PWR_PORT, EPAPER_PWR_PIN, GPIO_PIN_SET);
}

/* Power the display back up and re-initialise it. The framebuffer contents
 * are kept, so you can epaper_update() right away. */
bool epaper_power_on(void)
{
  MX_SPI1_Init();                                    /* CubeMX-generated  */
  return hw_init();
}

void epaper_clear(uint8_t color)
{
  memset(fb, plane_bit(color) ? 0xFF : 0x00, sizeof(fb));
}

void epaper_set_pixel(uint16_t x, uint16_t y, uint8_t color)
{
  if (x >= EPD_W || y >= EPD_H) return;
#if EPAPER_FLIP_X
  x = (uint16_t)(EPD_W - 1U - x);
#endif
#if EPAPER_FLIP_Y
  y = (uint16_t)(EPD_H - 1U - y);
#endif
  uint16_t idx  = (uint16_t)(x * ROW_BYTES + (y >> 3));
  uint8_t  mask = (uint8_t)(0x80U >> (y & 7U));
  if (plane_bit(color)) fb[idx] |= mask;
  else                  fb[idx] &= (uint8_t)~mask;
}

void epaper_hline(uint16_t x, uint16_t y, uint16_t len, uint8_t color)
{
  for (uint16_t i = 0; i < len; i++) epaper_set_pixel(x + i, y, color);
}

void epaper_vline(uint16_t x, uint16_t y, uint16_t len, uint8_t color)
{
  for (uint16_t i = 0; i < len; i++) epaper_set_pixel(x, y + i, color);
}

void epaper_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color)
{
  if (w == 0 || h == 0) return;
  epaper_hline(x, y, w, color);
  epaper_hline(x, y + h - 1, w, color);
  epaper_vline(x, y, h, color);
  epaper_vline(x + w - 1, y, h, color);
}

void epaper_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint8_t color)
{
  for (uint16_t j = 0; j < h; j++) epaper_hline(x, y + j, w, color);
}

/* Midpoint circle. Coordinates are computed in a wider signed type and only
 * cast to uint16_t at the point of drawing: a pixel that lands off the
 * left/top edge wraps to a large unsigned value, which epaper_set_pixel()
 * already bounds-checks and silently drops, so no separate clipping is
 * needed here. */
void epaper_circle(uint16_t cx, uint16_t cy, uint16_t r, uint8_t color)
{
  int32_t x = (int32_t)r, y = 0, err = 1 - (int32_t)r;
  while (x >= y)
  {
    epaper_set_pixel((uint16_t)((int32_t)cx + x), (uint16_t)((int32_t)cy + y), color);
    epaper_set_pixel((uint16_t)((int32_t)cx - x), (uint16_t)((int32_t)cy + y), color);
    epaper_set_pixel((uint16_t)((int32_t)cx + x), (uint16_t)((int32_t)cy - y), color);
    epaper_set_pixel((uint16_t)((int32_t)cx - x), (uint16_t)((int32_t)cy - y), color);
    epaper_set_pixel((uint16_t)((int32_t)cx + y), (uint16_t)((int32_t)cy + x), color);
    epaper_set_pixel((uint16_t)((int32_t)cx - y), (uint16_t)((int32_t)cy + x), color);
    epaper_set_pixel((uint16_t)((int32_t)cx + y), (uint16_t)((int32_t)cy - x), color);
    epaper_set_pixel((uint16_t)((int32_t)cx - y), (uint16_t)((int32_t)cy - x), color);
    y++;
    if (err < 0) { err += 2 * y + 1; }
    else         { x--; err += 2 * (y - x) + 1; }
  }
}

/* Brute-force filled disc (radius is always small here, tens of pixels at
 * most), avoiding any float/sqrt dependency. */
void epaper_fill_circle(uint16_t cx, uint16_t cy, uint16_t r, uint8_t color)
{
  int32_t r2 = (int32_t)r * (int32_t)r;
  for (int32_t dy = -(int32_t)r; dy <= (int32_t)r; dy++)
    for (int32_t dx = -(int32_t)r; dx <= (int32_t)r; dx++)
      if (dx * dx + dy * dy <= r2)
        epaper_set_pixel((uint16_t)((int32_t)cx + dx), (uint16_t)((int32_t)cy + dy), color);
}

bool epaper_update(void)
{
  if (gray_mode)            /* leave 4-gray: back to the built-in B/W waveform */
  {
    gray_mode = false;
    if (!panel_init()) return false;
  }
  panel_write_ram();
  return panel_refresh();
}

bool epaper_update_gray(void (*draw)(void))
{
  if (draw == NULL) return false;
  if (!panel_gray_begin()) return false;

  cur_plane = PLANE_LO;                        /* pass 1: low shade bit */
  epaper_clear(EPAPER_WHITE);
  draw();
  panel_gray_plane_done(PLANE_LO);

  cur_plane = PLANE_HI;                        /* pass 2: high shade bit (= the B/W version) */
  epaper_clear(EPAPER_WHITE);
  draw();
  panel_gray_plane_done(PLANE_HI);

  return panel_gray_refresh();
}

/* One full refresh per base-4 digit of the drive length; see epaper.h and
 * panel_gray16_lut(). Each pass re-sends both planes (from draw()) and a fresh
 * LUT, then refreshes. The RAM handling is the same as in the 4-gray path. */
bool epaper_update_gray16(void (*draw)(void), uint8_t tmax)
{
#if EPAPER_DISPLAY == 1
  if (draw == NULL) return false;
  if (tmax > 63U) tmax = 63U;
  const uint8_t passes = (tmax > 15U) ? 3U : (tmax > 3U) ? 2U : 1U;

  if (!panel_gray_begin()) return false;

  g16_tmax   = tmax;
  g16_active = true;
  bool ok = true;
  for (uint8_t p = 0; p < passes && ok; p++)
  {
    g16_shift = (uint8_t)(2U * (passes - 1U - p));   /* most significant digit first */

    cur_plane = PLANE_LO;
    epaper_clear(EPAPER_WHITE);
    draw();
    panel_gray_plane_done(PLANE_LO);

    cur_plane = PLANE_HI;
    epaper_clear(EPAPER_WHITE);
    draw();
    panel_gray_plane_done(PLANE_HI);

    panel_gray16_lut(p, passes, tmax);
    ok = panel_gray_refresh();
  }
  g16_active = false;
  cur_plane  = PLANE_HI;
  return ok;
#else
  (void)tmax;                                  /* GDE021A1: no multi-pass, 4 native shades */
  return epaper_update_gray(draw);
#endif
}

/* ========================================================================== */
/*  Experimental fast refresh: epaper_fast_* and epaper_extreme_*             */
/*                                                                            */
/*  Two public paths (see epaper.h) that share one implementation and are     */
/*  independent of the normal update path. Compared with epaper_update():     */
/*   - The SPI clock is raised: 8 MHz for epaper_fast_*() (the SSD1680 write  */
/*     clock is about 10 MHz at most) and 16 MHz for epaper_extreme_*(),      */
/*     above that specification. 16 MHz is the ceiling of the SPI clock       */
/*     divider at the 32 MHz bus clock (the smallest division is /2). The     */
/*     normal speed is restored by the matching end() function.               */
/*   - Each RAM is sent as one SPI burst instead of one HAL call per byte.    */
/*   - The analog supply and clock are switched on once and left on (update   */
/*     sequence 0x04 = "display only") instead of being cycled per frame.     */
/*   - A custom waveform (0x32) applies one drive phase of n frames: VSH1 for */
/*     black pixels, VSL for white ones, independent of the previous image.   */
/*     The polarity is as measured on this panel (see the gray16 notes).      */
/*     The levels differ in n only; the table is level_frames() below.        */
/*  Every frame writes the same data to both RAMs, so the waveform selects    */
/*  its LUT from the pixel value alone (RED == BW), as in the normal B/W      */
/*  path. epaper_extreme_*() writes only the B/W RAM (0x24) and skips the     */
/*  RED RAM (0x26): this is safe because the waveform defines the RED-driven  */
/*  LUT2/LUT3 identically to the BW-driven LUT0/LUT1, so the "RED differs     */
/*  from BW" case is never reached. Skipping the second burst of about 4000   */
/*  bytes roughly halves the RAM transfer time.                               */
/*  BUSY is awaited after every refresh; that wait (n frames of the           */
/*  waveform) determines the real update rate.                                */
/*  Every pixel is driven every frame, so run these paths only for short      */
/*  periods; the next normal full update rebalances the panel. Contrast is    */
/*  markedly worse than the normal update, and the SPI link may become        */
/*  unreliable at 16 MHz.                                                     */
/* ========================================================================== */

/* Drive length (panel frames the waveform holds each pixel) per level.
 * Level 0 = longest drive (best contrast), the last level = 1 frame. */
uint8_t epaper_fast_level_frames(uint8_t level)
{
  static const uint8_t frames[EPAPER_FAST_LEVELS] = { 15, 10, 6, 4, 3, 2, 1 };
  return frames[(level < EPAPER_FAST_LEVELS) ? level : (EPAPER_FAST_LEVELS - 1U)];
}

uint8_t epaper_extreme_level_frames(uint8_t level)
{
  return epaper_fast_level_frames(level);
}

#if EPAPER_DISPLAY == 1

#define EPAPER_FAST_SPI_PRESCALER     SPI_BAUDRATEPRESCALER_4   /* 32 MHz / 4 = 8 MHz; /2 (16 MHz) exceeds the SSD1680 spec */
#define EPAPER_EXTREME_SPI_PRESCALER  SPI_BAUDRATEPRESCALER_2   /* 32 MHz / 2 = 16 MHz */
#define EPAPER_EXTREME_SPI_HZ         16000000UL
#define EPAPER_FAST_FR                0x22   /* frame-rate byte of the waveform, same value the gray16 drive passes use */
#define EPAPER_FAST_BARS              10U
#define EPAPER_BUSY_ASSERT_SPINS      4000U  /* max polls for BUSY to assert after starting a refresh */

static bool     fastpath_spi_changed;
static bool     fastpath_power_on;           /* analog + clock left enabled by fastpath_begin() */
static uint32_t fastpath_spi_saved_br;

static void fast_spi_set_br(uint32_t br)
{
  __HAL_SPI_DISABLE(&EPAPER_SPI);                    /* BR may only change with SPE = 0; HAL_SPI_Transmit re-enables */
  MODIFY_REG(EPAPER_SPI.Instance->CR1, SPI_CR1_BR, br);
}

/* Write the framebuffer to one RAM (0x24 = BW, 0x26 = RED) as a single burst. */
static void fast_write_ram(uint8_t cmd)
{
  epd_cmd(0x4E); epd_data(0x00);
  epd_cmd(0x4F); epd_data(0x00); epd_data(0x00);
  epd_cmd(cmd);
  CS_LOW(); DC_HIGH();
  HAL_SPI_Transmit(&EPAPER_SPI, fb, (uint16_t)sizeof(fb), EPAPER_SPI_TIMEOUT_MS);
  CS_HIGH();
}

/* 153-byte waveform: group 0, phase A only, n frames. LUT0/LUT2 (BW = 0,
 * black) drive VSH1; LUT1/LUT3 (BW = 1, white) drive VSL; VCOM stays DC. */
static void fast_send_lut(uint8_t n)
{
  epd_cmd(0x32);
  for (uint8_t k = 0; k < 5U; k++)                   /* VS: LUT0..3, then LUT4 (VCOM) */
    for (uint8_t g = 0; g < 12U; g++)
    {
      uint8_t b = 0;
      if (g == 0U && k < 4U) b = ((k & 1U) == 0U) ? 0x40U : 0x80U;
      epd_data(b);
    }
  for (uint8_t g = 0; g < 12U; g++)                  /* TP A, TP B, SR, TP C, TP D, SR, RP */
  {
    epd_data((g == 0U) ? n : 0U);
    for (uint8_t i = 0; i < 6U; i++) epd_data(0);
  }
  epd_data(EPAPER_FAST_FR); epd_data(0x22); epd_data(0x22);   /* FR (6) */
  epd_data(0x22); epd_data(0x23); epd_data(0x32);
  epd_data(0x00); epd_data(0x00); epd_data(0x00);             /* XON (3) */
}

/* Enter the fast-refresh configuration with the given SPI clock divider. */
static bool fastpath_begin(uint32_t spi_prescaler)
{
  if (!panel_init()) return false;

  /* As in the 4-gray path: load the panel's own temperature/voltage setup
   * once (no display), then replace only the LUT. */
  epd_cmd(0x22); epd_data(0xB1);
  epd_cmd(0x20);
  if (!epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS)) return false;
  epd_cmd(0x3F); epd_data(0x22);

  gray_mode = true;                                  /* next epaper_update() re-inits to the normal B/W setup */

  fastpath_spi_saved_br = EPAPER_SPI.Instance->CR1 & SPI_CR1_BR;
  fast_spi_set_br(spi_prescaler);
  fastpath_spi_changed = true;

  fast_send_lut(epaper_fast_level_frames(0));

  epd_cmd(0x22); epd_data(0xC0);                     /* enable clock + analog, and leave them on */
  epd_cmd(0x20);
  if (!epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS)) return false;
  fastpath_power_on = true;
  return true;
}

/* Leave the fast-refresh configuration: wait for the last frame, switch the
 * analog supply off and restore the SPI clock. Safe to call after a failed
 * fastpath_begin(). */
static void fastpath_end(void)
{
  epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS);
  if (fastpath_power_on)
  {
    epd_cmd(0x22); epd_data(0x83);                   /* disable analog + clock */
    epd_cmd(0x20);
    epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS);
    fastpath_power_on = false;
  }
  if (fastpath_spi_changed)
  {
    fast_spi_set_br(fastpath_spi_saved_br);
    fastpath_spi_changed = false;
  }
  gray_mode = true;                                  /* next epaper_update() re-inits to the normal B/W setup */
}

static bool fastpath_set_level(uint8_t level)
{
  if (level >= EPAPER_FAST_LEVELS) return false;
  if (!epd_wait_busy(EPAPER_BUSY_TIMEOUT_MS)) return false;
  fast_send_lut(epaper_fast_level_frames(level));
  return true;
}

/* Fill the framebuffer with EPAPER_FAST_BARS vertical bars, inverted by phase. */
static void fastpath_bars(uint8_t phase)
{
  const uint16_t bar_bytes = (uint16_t)((EPD_W / EPAPER_FAST_BARS) * ROW_BYTES);
  for (uint8_t k = 0; k < EPAPER_FAST_BARS; k++)     /* fb: 1 = white, 0 = black */
    memset(&fb[k * bar_bytes], (((k ^ phase) & 1U) != 0U) ? 0x00 : 0xFF, bar_bytes);
}

/* Start the refresh of the RAM content ("display only": analog and clock were
 * left on by fastpath_begin()). */
static void fastpath_refresh_start(void)
{
  epd_cmd(0x22); epd_data(0x04);
  epd_cmd(0x20);
  for (uint16_t i = 0; i < EPAPER_BUSY_ASSERT_SPINS && !IS_BUSY(); i++) { }   /* give BUSY a moment to assert */
}

/* ---- epaper_fast_*: 8 MHz, both RAMs ------------------------------------ */

bool epaper_fast_begin(void)                  { return fastpath_begin(EPAPER_FAST_SPI_PRESCALER); }
bool epaper_fast_set_level(uint8_t level)     { return fastpath_set_level(level); }
void epaper_fast_pattern(uint8_t phase)       { fastpath_bars(phase); }
bool epaper_fast_busy(void)                   { return IS_BUSY(); }
void epaper_fast_end(void)                    { fastpath_end(); }

void epaper_fast_frame_start(void)
{
  fast_write_ram(0x24);
  fast_write_ram(0x26);
  fastpath_refresh_start();
}

/* ---- epaper_extreme_*: 16 MHz, B/W RAM only ----------------------------- */

bool epaper_extreme_begin(void)               { return fastpath_begin(EPAPER_EXTREME_SPI_PRESCALER); }
bool epaper_extreme_set_level(uint8_t level)  { return fastpath_set_level(level); }
void epaper_extreme_pattern(uint8_t phase)    { fastpath_bars(phase); }
bool epaper_extreme_busy(void)                { return IS_BUSY(); }
void epaper_extreme_end(void)                 { fastpath_end(); }
uint32_t epaper_extreme_spi_hz(void)          { return EPAPER_EXTREME_SPI_HZ; }

void epaper_extreme_frame_start(void)
{
  fast_write_ram(0x24);
  fastpath_refresh_start();
}

#else  /* EPAPER_DISPLAY == 0: the fast-refresh paths are not available on the GDE021A1 */

bool     epaper_fast_begin(void)                 { return false; }
bool     epaper_fast_set_level(uint8_t level)    { (void)level; return false; }
void     epaper_fast_pattern(uint8_t phase)      { (void)phase; }
void     epaper_fast_frame_start(void)           { }
bool     epaper_fast_busy(void)                  { return false; }
void     epaper_fast_end(void)                   { }

bool     epaper_extreme_begin(void)              { return false; }
bool     epaper_extreme_set_level(uint8_t level) { (void)level; return false; }
void     epaper_extreme_pattern(uint8_t phase)   { (void)phase; }
void     epaper_extreme_frame_start(void)        { }
bool     epaper_extreme_busy(void)               { return false; }
void     epaper_extreme_end(void)                { }
uint32_t epaper_extreme_spi_hz(void)             { return 0UL; }

#endif /* EPAPER_DISPLAY == 1 (fast-refresh paths) */
