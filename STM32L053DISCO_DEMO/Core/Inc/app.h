/**
 * @file    app.h
 * @brief   STM32L0538-DISCO showcase application (menu on the e-paper display).
 */
#ifndef APP_H
#define APP_H

/* Exported functions prototypes --------------------------------------------*/

/** @brief Initialize board, display and touch sensor and show the menu. Call once at start-up. */
void app_init(void);

/** @brief Process input and redraw as needed. Call repeatedly from the main loop. */
void app_run(void);

#endif /* APP_H */
