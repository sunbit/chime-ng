#ifndef CHIME_MOCK_CODEC_H
#define CHIME_MOCK_CODEC_H
#define CHIME_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "inc/hw_i2c.h"
#include "inc/hw_ocp_shared.h"
#include "driverlib/i2c.h"
#include "driverlib/pin.h"
#include "driverlib/gpio.h"
#include "driverlib/prcm.h"
#undef HWREG
uint32_t *mock_reg(uint32_t address);
#define HWREG(address) (*mock_reg(address))
#define MAP_I2CMasterIntClearEx I2CMasterIntClearEx
#define MAP_I2CMasterIntStatusEx I2CMasterIntStatusEx
#define MAP_I2CMasterTimeoutSet I2CMasterTimeoutSet
#define MAP_I2CMasterControl I2CMasterControl
#define MAP_I2CMasterErr I2CMasterErr
#define MAP_I2CMasterSlaveAddrSet I2CMasterSlaveAddrSet
#define MAP_I2CMasterDataPut I2CMasterDataPut
#define MAP_I2CMasterDataGet I2CMasterDataGet
#define MAP_PRCMPeripheralClkEnable PRCMPeripheralClkEnable
#define MAP_PRCMPeripheralReset PRCMPeripheralReset
#define MAP_GPIOPinWrite GPIOPinWrite
#define MAP_PinTypeGPIO PinTypeGPIO
#define MAP_GPIODirModeSet GPIODirModeSet
#define MAP_PinTypeI2C mock_rom_pin_i2c
#define MAP_I2CMasterInitExpClk mock_rom_i2c_init
#define MAP_PinModeGet PinModeGet
#define MAP_GPIOPinRead GPIOPinRead
#define MAP_PRCMPeripheralStatusGet PRCMPeripheralStatusGet
uint32_t millis(void);
void log_line(const char *message);
void delay_ms(uint32_t ms);
void mock_rom_pin_i2c(unsigned long pin,unsigned long mode);
void mock_rom_i2c_init(uint32_t base,uint32_t clock,bool fast);
#endif
