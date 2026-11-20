/* board_utils.h - LEDs + logs console pour NUCLEO-N657X0-Q
 *
 * Dans CubeMX, nommez (User Label) les 3 broches des LEDs :
 *   PG10 -> LED_RED    PG0 -> LED_GREEN    PG8 -> LED_BLUE
 * (mode GPIO_Output). CubeMX génère alors LED_xxx_Pin et LED_xxx_GPIO_Port
 * dans main.h.
 *
 * ATTENTION : les LEDs de la NUCLEO-N657X0-Q sont ACTIVES À L'ÉTAT BAS
 * (broche à 0 = LED allumée). Vérifiez dans le schéma MB1940 / UM3417.
 */
#ifndef BOARD_UTILS_H
#define BOARD_UTILS_H

#include <stdio.h>
#include "main.h"

#define LED_ON(led)   HAL_GPIO_WritePin(led##_GPIO_Port, led##_Pin, GPIO_PIN_RESET)
#define LED_OFF(led)  HAL_GPIO_WritePin(led##_GPIO_Port, led##_Pin, GPIO_PIN_SET)

static inline void LEDS_AllOff(void) { LED_OFF(LED_RED); LED_OFF(LED_GREEN); LED_OFF(LED_BLUE); }
static inline void LEDS_AllOn(void)  { LED_ON(LED_RED);  LED_ON(LED_GREEN);  LED_ON(LED_BLUE);  }

/* Log horodaté sur la console (UART ST-LINK VCP, via printf) */
#define LOG(fmt, ...) \
    printf("[%8lu ms] " fmt "\r\n", (unsigned long)HAL_GetTick(), ##__VA_ARGS__)

#endif /* BOARD_UTILS_H */
