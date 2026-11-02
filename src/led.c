#include "platform.h"
#include "led.h"
#include "inc/hw_ocp_shared.h"
#include <stdio.h>

/* Ring LED_init -> 0x20019794 selects GPIO11 / PIN_02, active high.
 * GPIODATA reads the output latch, not pad voltage, when GPIODIR is OUT.
 * Pad config 11 at 0x4402e0cc also controls output-buffer enable (bits 11:10).
 * Explicit OUT is a diagnostic alternative to the original peripheral control,
 * not an established explanation for the missing physical LED light. */
#define LED_PAD (OCP_SHARED_BASE+OCP_SHARED_O_GPIO_PAD_CONFIG_11)
static volatile unsigned remaining_ms;
static volatile bool requested;
static bool initialized;
static unsigned long pad_before;
static char diagnostic[LED_DIAGNOSTIC_SIZE];

void board_led_set(bool on) {
    /* Cancel the boot expiry before changing the output. SysTick cannot
     * subsequently undo a manual command, even at the expiry boundary. */
    remaining_ms=0;
    PRCMPeripheralClkEnable(PRCM_GPIOA1,PRCM_RUN_MODE_CLK);
    pad_before=HWREG(LED_PAD);
    PinTypeGPIO(PIN_02,PIN_MODE_0,false);
    GPIODirModeSet(GPIOA1_BASE,0x08,GPIO_DIR_MODE_OUT);
    GPIOPinWrite(GPIOA1_BASE,0x08,on?0x08:0);
    /* Keep the original 2 mA push-pull setting; override only pad OE, after
     * loading the desired output value. No other GPIO is touched. */
    PinDirModeSet(PIN_02,PIN_DIR_MODE_OUT);
    requested=on;initialized=true;
}
void board_start_indicator(void) { board_led_set(true);remaining_ms=3000; }
void board_led_tick(void) {
    if(remaining_ms&&!--remaining_ms) {
        GPIOPinWrite(GPIOA1_BASE,0x08,0);requested=false;
    }
}
bool board_led_on(void) { return requested; }
const char *board_led_diagnostic(void) {
    if(!initialized)return "gpio11 initialized=0";
    unsigned long strength,type;
    PinConfigGet(PIN_02,&strength,&type);
    snprintf(diagnostic,sizeof diagnostic,
        "gpio11 requested=%u latch=%u dir=%lu mode=%lu ready=%u drive=%lx type=%lx before=%lx pad=%lx oe=%lx",
        (unsigned)requested,!!GPIOPinRead(GPIOA1_BASE,0x08),
        GPIODirModeGet(GPIOA1_BASE,3),PinModeGet(PIN_02),
        !!PRCMPeripheralStatusGet(PRCM_GPIOA1),strength,type,
        pad_before,(unsigned long)HWREG(LED_PAD),PinDirModeGet(PIN_02));
    return diagnostic;
}
