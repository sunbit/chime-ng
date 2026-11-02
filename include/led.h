#ifndef CHIME_LED_H
#define CHIME_LED_H
#include <stdbool.h>
#define LED_DIAGNOSTIC_SIZE 128
void board_start_indicator(void);
void board_led_tick(void); /* Existing 1 ms SysTick only. */
void board_led_set(bool on);
bool board_led_on(void); /* Requested output, not an optical measurement. */
const char *board_led_diagnostic(void);
#endif
