/**
 * @file    touch.c
 * @brief   Linear touch sensor of the STM32L0538-DISCO (three zones, TSC).
 *
 * Acquires the three TSC groups through the CubeMX-generated handle, compares
 * each count against a baseline taken at start-up and reports the zone with
 * the largest drop, debounced over consecutive scans.
 */

/* Includes -----------------------------------------------------------------*/
#include "touch.h"
#include "main.h"
#include "tsc.h"        /* extern TSC_HandleTypeDef htsc */

/* Private defines ----------------------------------------------------------*/

/*
 * Zone i (left to right) -> TSC group index.
 *
 * The strip is wired G3 (left), G2 (middle), G1 (right). This differs from
 * ST's STM32L0538-DISCO touch example (tsl_user.h), which assigns the middle
 * electrode to G3 and the right one to G2.
 */
static const uint32_t TOUCH_SEGMENT_ORDER[3] = { TSC_GROUP3_IDX, TSC_GROUP2_IDX, TSC_GROUP1_IDX };

/*
 * A zone counts as touched when its raw count falls more than this many
 * counts below its own baseline. A flat count threshold works for all three
 * zones because the baselines are close in magnitude (about 1978..2232).
 * What differs between zones is the touch signal itself: the end electrodes
 * of the linear sensor couple more weakly by design (see AN5105). 100 counts
 * is above idle noise on every zone and still below the weakest end-electrode
 * touch delta.
 */
#define TOUCH_THRESHOLD_COUNTS 100U

/* Consecutive scans that must report the same zone before it is accepted. */
#define TOUCH_DEBOUNCE_SCANS     2U

/* Acquisitions averaged to form the untouched baseline. */
#define TOUCH_BASELINE_SAMPLES  16U

/* Private variables --------------------------------------------------------*/
static touch_state_t st = { {0}, {0}, -1 };
static int8_t  pending_zone = -1;
static uint8_t pending_cnt;

/* Private functions --------------------------------------------------------*/

/* One acquisition of all groups. Returns true if at least one completed. */
static bool acquire(uint16_t out[3])
{
  HAL_TSC_IODischarge(&htsc, ENABLE);
  HAL_Delay(1);
  if (HAL_TSC_Start(&htsc) != HAL_OK) return false;
  if (HAL_TSC_PollForAcquisition(&htsc) != HAL_OK) return false;

  bool any = false;
  for (uint8_t i = 0; i < 3; i++)
  {
    if (HAL_TSC_GroupGetStatus(&htsc, TOUCH_SEGMENT_ORDER[i]) == TSC_GROUP_COMPLETED)
    {
      out[i] = (uint16_t)HAL_TSC_GroupGetValue(&htsc, TOUCH_SEGMENT_ORDER[i]);
      any = true;
    }
    else
    {
      out[i] = 0;
    }
  }
  return any;
}

/* Public functions ---------------------------------------------------------*/

bool touch_init(void)
{
  uint32_t sum[3] = {0, 0, 0};
  uint16_t v[3];
  uint8_t  good = 0;

  for (uint8_t n = 0; n < TOUCH_BASELINE_SAMPLES; n++)
  {
    if (acquire(v))
    {
      for (uint8_t i = 0; i < 3; i++) sum[i] += v[i];
      good++;
    }
  }
  if (good == 0) return false;

  bool ok = true;
  for (uint8_t i = 0; i < 3; i++)
  {
    st.base[i] = (uint16_t)(sum[i] / good);
    if (st.base[i] == 0) ok = false;      /* that group never completed */
  }
  st.zone = -1;
  return ok;
}

bool touch_scan(void)
{
  uint16_t v[3];
  if (!acquire(v)) return false;

  int8_t   best = -1;
  uint32_t best_delta = 0;
  for (uint8_t i = 0; i < 3; i++)
  {
    st.raw[i] = v[i];
    if (st.base[i] == 0 || v[i] == 0) continue;
    if (v[i] < st.base[i] && (uint32_t)(st.base[i] - v[i]) > TOUCH_THRESHOLD_COUNTS)
    {
      uint32_t d = (uint32_t)(st.base[i] - v[i]);
      if (d > best_delta) { best_delta = d; best = (int8_t)i; }
    }
  }

  /* Debounce: the same zone must be seen on consecutive scans. */
  if (best == pending_zone)
  {
    if (pending_cnt < 255) pending_cnt++;
    if (pending_cnt >= TOUCH_DEBOUNCE_SCANS) st.zone = best;
  }
  else
  {
    pending_zone = best;
    pending_cnt = 1;
  }
  return true;
}

const touch_state_t *touch_state(void) { return &st; }
