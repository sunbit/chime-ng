#ifndef CHIME_MOCK_LED_H
#define CHIME_MOCK_LED_H
#include "mock_platform.h"
#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "inc/hw_ocp_shared.h"
#undef HWREG
uint32_t *mock_led_reg(uint32_t address);
#define HWREG(address) (*mock_led_reg(address))
#include "driverlib/prcm.h"
#include "driverlib/pin.h"
#include "driverlib/gpio.h"
#endif
