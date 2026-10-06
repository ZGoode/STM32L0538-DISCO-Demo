/**
 * @file    touch.h
 * @brief   Linear touch sensor of the STM32L0538-DISCO, exposed as three zones.
 *
 * The sensor is read through three TSC groups, each with one electrode
 * (channel) and one sampling capacitor pin:
 *
 *   G1: PA2 / PA3     G2: PA6 / PA7     G3: PB0 / PB1
 *
 * Zones are numbered left to right (0..2) and map to the groups
 * G3, G2, G1 (see TOUCH_SEGMENT_ORDER in touch.c). A touch lowers the raw
 * acquisition count; the touch delta is (baseline - raw).
 */
#ifndef TOUCH_H
#define TOUCH_H

/* Includes -----------------------------------------------------------------*/
#include <stdbool.h>
#include <stdint.h>

/* Exported types -----------------------------------------------------------*/
typedef struct {
  uint16_t raw[3];     /* Latest acquisition per zone (0 = group did not complete) */
  uint16_t base[3];    /* Untouched baseline measured by touch_init() */
  int8_t   zone;       /* Debounced touched zone: -1 = none, else 0..2 */
} touch_state_t;

/* Exported functions prototypes --------------------------------------------*/

/**
 * @brief Measure the untouched baseline of each zone.
 *
 * Keep fingers off the sensor while this runs.
 * @return false if the TSC produced no usable counts (wrong IO roles or
 *         peripheral not enabled).
 */
bool touch_init(void);

/**
 * @brief Run one acquisition and update the debounced zone (takes a few ms).
 * @return false on a TSC error.
 */
bool touch_scan(void);

/** @brief Return the current touch state. */
const touch_state_t *touch_state(void);

#endif /* TOUCH_H */
