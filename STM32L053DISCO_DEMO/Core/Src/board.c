/**
 * @file    board.c
 * @brief   STM32L0538-DISCO board support: user LEDs, user button and
 *          ST-LINK virtual COM port logging.
 */

/* Includes -----------------------------------------------------------------*/
#include "board.h"
#include "main.h"
#include "usart.h"
#include <stdarg.h>
#include <stdio.h>

/* Private defines ----------------------------------------------------------*/

/* Pin names come from the GPIO labels set in the .ioc (see main.h). */
#define BOARD_LOG_UART      huart1
#define LED_GREEN_PORT      LED_GREEN_GPIO_Port
#define LED_GREEN_PIN       LED_GREEN_Pin
#define LED_RED_PORT        LED_RED_GPIO_Port
#define LED_RED_PIN         LED_RED_Pin
#define BUTTON_PORT         BUTTON_GPIO_Port
#define BUTTON_PIN          BUTTON_Pin
#define BUTTON_ACTIVE_LEVEL GPIO_PIN_SET      /* B1 pulls PA0 high when pressed */

/* Log transmit timeout. */
#define BOARD_LOG_TIMEOUT_MS 50U

/* Private variables --------------------------------------------------------*/
static volatile bool s_wake;

/* Public functions ---------------------------------------------------------*/

void board_led_green(bool on) { HAL_GPIO_WritePin(LED_GREEN_PORT, LED_GREEN_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET); }
void board_led_red(bool on)   { HAL_GPIO_WritePin(LED_RED_PORT,   LED_RED_PIN,   on ? GPIO_PIN_SET : GPIO_PIN_RESET); }

bool board_button_pressed(void)
{
  return HAL_GPIO_ReadPin(BUTTON_PORT, BUTTON_PIN) == BUTTON_ACTIVE_LEVEL;
}

/*
 * Wake-up from STOP on B1.
 *
 * This module owns the EXTI0_1 interrupt. Do not enable EXTI0_1 / PA0
 * interrupts in CubeMX, otherwise a second handler is generated into
 * stm32l0xx_it.c and the link fails with a duplicate symbol.
 */
void EXTI0_1_IRQHandler(void)
{
  if (__HAL_GPIO_EXTI_GET_IT(BUTTON_PIN) != 0U)
  {
    __HAL_GPIO_EXTI_CLEAR_IT(BUTTON_PIN);
    s_wake = true;
  }
}

void board_arm_wake(void)
{
  GPIO_InitTypeDef g = {0};
  g.Pin = BUTTON_PIN; g.Mode = GPIO_MODE_IT_RISING; g.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(BUTTON_PORT, &g);
  __HAL_GPIO_EXTI_CLEAR_IT(BUTTON_PIN);
  s_wake = false;
  HAL_NVIC_SetPriority(EXTI0_1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_1_IRQn);
}

void board_disarm_wake(void)
{
  GPIO_InitTypeDef g = {0};
  HAL_NVIC_DisableIRQ(EXTI0_1_IRQn);
  g.Pin = BUTTON_PIN; g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(BUTTON_PORT, &g);
}

bool board_wake_flag(void) { return s_wake; }

void board_log(const char *fmt, ...)
{
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof(buf) - 3) n = (int)sizeof(buf) - 3;
  buf[n++] = '\r';
  buf[n++] = '\n';
  HAL_UART_Transmit(&BOARD_LOG_UART, (uint8_t *)buf, (uint16_t)n, BOARD_LOG_TIMEOUT_MS);
}
