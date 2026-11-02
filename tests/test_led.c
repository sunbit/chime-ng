#include "led.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned gpio=0xa0,mode=4,dir;
static uint32_t pad=0xc61;
static bool clock_on,force_low;
uint32_t *mock_led_reg(uint32_t address){assert(address==OCP_SHARED_BASE+OCP_SHARED_O_GPIO_PAD_CONFIG_11);return &pad;}
void PRCMPeripheralClkEnable(unsigned long p,unsigned long clock){assert(p==PRCM_GPIOA1&&clock==PRCM_RUN_MODE_CLK);clock_on=true;}
tBoolean PRCMPeripheralStatusGet(unsigned long p){assert(p==PRCM_GPIOA1);return clock_on;}
void PinTypeGPIO(unsigned long p,unsigned long m,tBoolean od){assert(p==PIN_02&&m==PIN_MODE_0&&!od&&clock_on);mode=m;pad=0x20;}
void PinDirModeSet(unsigned long p,unsigned long d){assert(p==PIN_02&&d==PIN_DIR_MODE_OUT&&dir==GPIO_DIR_MODE_OUT);pad=(pad&~0xc00)|d;}
unsigned long PinDirModeGet(unsigned long p){assert(p==PIN_02);return pad&0xc00;}
unsigned long PinModeGet(unsigned long p){assert(p==PIN_02);return mode;}
void PinConfigGet(unsigned long p,unsigned long *strength,unsigned long *type){assert(p==PIN_02);*strength=0x20;*type=0;}
void GPIODirModeSet(unsigned long p,unsigned char mask,unsigned long d){assert(p==GPIOA1_BASE&&mask==8&&d==GPIO_DIR_MODE_OUT);dir=d;}
unsigned long GPIODirModeGet(unsigned long p,unsigned char pin){assert(p==GPIOA1_BASE&&pin==3);return dir;}
void GPIOPinWrite(unsigned long p,unsigned char mask,unsigned char v){assert(p==GPIOA1_BASE&&mask==8&&(v==0||v==8)&&clock_on&&dir==GPIO_DIR_MODE_OUT);gpio=(gpio&~mask)|v;}
long GPIOPinRead(unsigned long p,unsigned char mask){assert(p==GPIOA1_BASE&&mask==8);return dir==GPIO_DIR_MODE_OUT?gpio&mask:force_low?0:gpio&mask;}
static void advance(unsigned n){while(n--)board_led_tick();}
int main(void) {
    assert(!board_led_on());advance(4000);assert(gpio==0xa0&&!clock_on);
    board_start_indicator();assert(board_led_on()&&gpio==0xa8);
    advance(2999);assert(board_led_on());advance(1);assert(!board_led_on()&&gpio==0xa0);
    /* A manual ON just before the old deadline must persist past that deadline. */
    board_start_indicator();advance(2999);board_led_set(true);advance(6000);
    assert(board_led_on()&&gpio==0xa8);
    assert(strstr(board_led_diagnostic(),"requested=1 latch=1 dir=1 mode=0 ready=1"));
    assert(strstr(board_led_diagnostic(),"pad=820 oe=800"));
    board_led_set(false);advance(4000);assert(!board_led_on()&&gpio==0xa0);
    board_start_indicator();board_led_set(false);advance(4000);assert(!board_led_on()&&gpio==0xa0);
    /* A stuck-low physical input does NOT change output-latch readback. */
    board_led_set(true);force_low=true;
    assert(strstr(board_led_diagnostic(),"requested=1 latch=1 dir=1"));
    /* Show a disabled pad even when GPIODIR and output latch still say ON. */
    pad=0xc20;
    assert(strstr(board_led_diagnostic(),"requested=1 latch=1 dir=1"));
    assert(strstr(board_led_diagnostic(),"pad=c20 oe=c00"));
    board_led_set(true);
    assert(strstr(board_led_diagnostic(),"before=c20 pad=820 oe=800"));
    mode=4;dir=0;
    assert(strstr(board_led_diagnostic(),"requested=1 latch=0 dir=0 mode=4"));
    board_led_set(true);assert(mode==0&&dir==GPIO_DIR_MODE_OUT);
    force_low=false;assert(strstr(board_led_diagnostic(),"requested=1 latch=1"));
    board_led_set(false);assert(strlen(board_led_diagnostic())<LED_DIAGNOSTIC_SIZE&&gpio==0xa0);
    puts("LED: real driver, timed boot, manual override at expiry, explicit pad output enable, disabled-pad detection, latch semantics and unrelated GPIO preservation passed");
}
