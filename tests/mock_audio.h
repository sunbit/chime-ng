#ifndef CHIME_MOCK_AUDIO_H
#define CHIME_MOCK_AUDIO_H
#include "mock_platform.h"
#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "driverlib/i2s.h"
#include "driverlib/udma.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin.h"
#include "driverlib/prcm.h"
#undef HWREG
uint32_t *mock_reg(uint32_t address);
#define HWREG(address) (*mock_reg(address))
#define MAP_I2SDisable I2SDisable
#define MAP_I2SSerializerConfig I2SSerializerConfig
#define MAP_I2STxFIFODisable I2STxFIFODisable
#define MAP_I2STxFIFOEnable I2STxFIFOEnable
#define MAP_I2STxFIFOStatusGet I2STxFIFOStatusGet
#define MAP_PRCMI2SClockFreqSet PRCMI2SClockFreqSet
#define MAP_PRCMPeripheralClkEnable PRCMPeripheralClkEnable
#define MAP_PinTypeI2S PinTypeI2S
#define MAP_IntEnable IntEnable
#define MAP_IntDisable IntDisable
#define MAP_uDMAControlBaseSet uDMAControlBaseSet
#define MAP_uDMAEnable uDMAEnable
#define MAP_uDMAChannelAssign uDMAChannelAssign
#define MAP_uDMAChannelAttributeDisable uDMAChannelAttributeDisable
#define MAP_uDMAChannelAttributeEnable uDMAChannelAttributeEnable
#define MAP_uDMAChannelControlSet uDMAChannelControlSet
#define MAP_uDMAChannelTransferSet uDMAChannelTransferSet
#define MAP_uDMAChannelEnable uDMAChannelEnable
#define MAP_uDMAChannelDisable uDMAChannelDisable
#define MAP_uDMAChannelModeGet uDMAChannelModeGet
#define MAP_uDMAChannelSizeGet uDMAChannelSizeGet
#define MAP_uDMAChannelIsEnabled uDMAChannelIsEnabled
#define MAP_uDMAErrorStatusGet uDMAErrorStatusGet
#define MAP_uDMAErrorStatusClear uDMAErrorStatusClear
void delay_ms(uint32_t ms);
#endif
