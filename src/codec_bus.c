#include "platform.h"
#include "codec_bus.h"
#include "driverlib/i2c.h"
#include "inc/hw_i2c.h"
#include "inc/hw_ocp_shared.h"
#include <stdio.h>
#include <string.h>

static char diagnostic[AUDIO_DIAGNOSTIC_SIZE]="none";
static uint8_t codec_page;
static uint32_t i2c_error,api_flags,bus_before;
static bool sdk_calls;
static unsigned attempt,probe_gpio=255;
static const char *path="ring400";
static char attempts[100];
const char *codec_bus_diagnostic(void) { return diagnostic; }

static uint32_t raw_flags(void) {
    return sdk_calls?I2CMasterIntStatusEx(I2CA0_BASE,false):MAP_I2CMasterIntStatusEx(I2CA0_BASE,false);
}
static void control(unsigned cmd) {
    if(sdk_calls)I2CMasterControl(I2CA0_BASE,cmd);else MAP_I2CMasterControl(I2CA0_BASE,cmd);
}
static void address(bool read) {
    if(sdk_calls)I2CMasterSlaveAddrSet(I2CA0_BASE,0x18,read);else MAP_I2CMasterSlaveAddrSet(I2CA0_BASE,0x18,read);
}
static void put(uint8_t value) {
    if(sdk_calls)I2CMasterDataPut(I2CA0_BASE,value);else MAP_I2CMasterDataPut(I2CA0_BASE,value);
}
static bool command(unsigned cmd) {
    /* Match Ring's completion protocol; retain a bounded wait so the web and
     * OTA remain available even if the peripheral or bus does not respond. */
    if(sdk_calls){I2CMasterIntClearEx(I2CA0_BASE,raw_flags());I2CMasterTimeoutSet(I2CA0_BASE,0x7d);}
    else {MAP_I2CMasterIntClearEx(I2CA0_BASE,raw_flags());MAP_I2CMasterTimeoutSet(I2CA0_BASE,0x7d);}
    bus_before=HWREG(I2CA0_BASE+I2C_O_MBMON)&3;
    control(cmd);
    uint32_t start=millis();
    for(;;) {
        api_flags=raw_flags();
        if(api_flags&(I2C_MASTER_INT_DATA|I2C_MASTER_INT_TIMEOUT))break;
        /* TI's 0x7d counter allows 20 ms at 100 kHz; allow its hardware
         * timeout to expire before our software deadline at either speed. */
        if((uint32_t)(millis()-start)>50){i2c_error=0x100;return false;}
    }
    i2c_error=sdk_calls?I2CMasterErr(I2CA0_BASE):MAP_I2CMasterErr(I2CA0_BASE);
    if(api_flags&I2C_MASTER_INT_TIMEOUT)i2c_error|=I2C_MASTER_ERR_CLK_TOUT;
    return i2c_error==0;
}

static bool failed(uint8_t reg,const char *step) {
    /* Read hardware directly, before STOP can change the evidence. api vs ris
     * also tells us whether the ROM status call agrees with the raw register. */
    uint32_t mcs=HWREG(I2CA0_BASE+I2C_O_MCS),ris=HWREG(I2CA0_BASE+I2C_O_MRIS);
    uint32_t bus=HWREG(I2CA0_BASE+I2C_O_MBMON)&3;
    uint32_t ctl=HWREG(I2CA0_BASE+I2C_O_MCR),tpr=HWREG(I2CA0_BASE+I2C_O_MTPR);
    uint32_t timeout=HWREG(I2CA0_BASE+I2C_O_MCLKOCNT),sa=HWREG(I2CA0_BASE+I2C_O_MSA);
    unsigned p3=MAP_PinModeGet(PIN_03),p4=MAP_PinModeGet(PIN_04);
    unsigned reset=!!MAP_GPIOPinRead(GPIOA3_BASE,0x10);
    unsigned ready=!!MAP_PRCMPeripheralStatusGet(PRCM_I2CA0);
    uint32_t pad3=HWREG(OCP_SHARED_BASE+OCP_SHARED_O_GPIO_PAD_CONFIG_12);
    uint32_t pad4=HWREG(OCP_SHARED_BASE+OCP_SHARED_O_GPIO_PAD_CONFIG_13);
    control(I2C_MASTER_CMD_BURST_SEND_ERROR_STOP);
    snprintf(diagnostic,sizeof diagnostic,
        "i2c-0x18 page=%u reg=0x%02x step=%s error=0x%lx "
        "mcs=%lx ris=%lx api=%lx ctl=%lx tpr=%lx to=%lx sa=%lx bus=%lu/%lu pins=%u/%u rst=%u clk=%u "
        "path=%s pad=%lx/%lx",
        codec_page,reg,step,(unsigned long)i2c_error,
        (unsigned long)mcs,(unsigned long)ris,(unsigned long)api_flags,
        (unsigned long)ctl,(unsigned long)tpr,(unsigned long)timeout,(unsigned long)sa,
        (unsigned long)bus_before,(unsigned long)bus,p3,p4,reset,ready,path,(unsigned long)pad3,(unsigned long)pad4);
    log_line(diagnostic);
    return false;
}

bool codec_reg_write(uint8_t reg,uint8_t value) {
    address(false);put(reg);
    if(!command(I2C_MASTER_CMD_BURST_SEND_START))return failed(reg,"register");
    put(value);
    /* Ring 0x2001b128 uses START (3), CONT (1), then a separate STOP (4). */
    if(!command(I2C_MASTER_CMD_BURST_SEND_CONT))return failed(reg,"data");
    if(!command(I2C_MASTER_CMD_BURST_SEND_STOP))return failed(reg,"stop");
    if(!reg)codec_page=value;
    return true;
}

bool codec_reg_read(uint8_t reg,uint8_t *value) {
    address(false);put(reg);
    if(!command(I2C_MASTER_CMD_BURST_SEND_START))return failed(reg,"read-register");
    address(true);
    if(!command(I2C_MASTER_CMD_SINGLE_RECEIVE))return failed(reg,"read-data");
    *value=sdk_calls?I2CMasterDataGet(I2CA0_BASE):MAP_I2CMasterDataGet(I2CA0_BASE);
    return true;
}

bool codec_reg_verify(uint8_t reg,uint8_t expected) {
    uint8_t value;
    if(!codec_reg_read(reg,&value))return false;
    if(value==expected)return true;
    snprintf(diagnostic,sizeof diagnostic,"i2c-readback page=%u reg=0x%02x expected=0x%02x got=0x%02x path=%s",
        codec_page,reg,expected,value,path);log_line(diagnostic);i2c_error=0x101;return false;
}

static void ring_board_gpio(void) {
    /* Directions from 0x20005af0, checked against disassembly (including
     * GPIO16, whose argument is lost in the decompiler). GPIO15=1 comes from
     * 0x200150d4. GPIO11 is owned and configured by led.c before codec init:
     * leave both its latch and pad configuration intact, including pad OE. */
    static const struct {unsigned pin,port,mask,direction;} pins[]={
        {PIN_61,GPIOA0_BASE,0x40,GPIO_DIR_MODE_OUT},
        {PIN_01,GPIOA1_BASE,0x04,GPIO_DIR_MODE_OUT},
        {PIN_05,GPIOA1_BASE,0x40,GPIO_DIR_MODE_IN},
        {PIN_06,GPIOA1_BASE,0x80,GPIO_DIR_MODE_OUT},
        {PIN_07,GPIOA2_BASE,0x01,GPIO_DIR_MODE_OUT},
        {PIN_08,GPIOA2_BASE,0x02,GPIO_DIR_MODE_OUT},
        {PIN_15,GPIOA2_BASE,0x40,GPIO_DIR_MODE_IN},
        {PIN_18,GPIOA3_BASE,0x10,GPIO_DIR_MODE_OUT},
        {PIN_53,GPIOA3_BASE,0x40,GPIO_DIR_MODE_OUT}};
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA0,PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA1,PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA2,PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralClkEnable(PRCM_GPIOA3,PRCM_RUN_MODE_CLK);
    /* Load explicit levels before enabling outputs to avoid a low glitch. */
    MAP_GPIOPinWrite(GPIOA1_BASE,0x80,0x80);
    for(unsigned i=0;i<sizeof pins/sizeof pins[0];i++){
        MAP_PinTypeGPIO(pins[i].pin,PIN_MODE_0,false);
        MAP_GPIODirModeSet(pins[i].port,pins[i].mask,pins[i].direction);
    }
}

static void start_controller(bool fast) {
    codec_page=0;
    /* Same relative ordering as Ring 0x20020778: pins, DAC high, controller
     * reset/init, DAC low/high. Ring waits UtilsDelay(30000); 2 ms covers it. */
    if(sdk_calls){PinTypeI2C(PIN_03,PIN_MODE_5);PinTypeI2C(PIN_04,PIN_MODE_5);}
    else {MAP_PinTypeI2C(PIN_03,PIN_MODE_5);MAP_PinTypeI2C(PIN_04,PIN_MODE_5);}
    MAP_GPIOPinWrite(GPIOA3_BASE,0x10,0x10);delay_ms(2);
    MAP_PRCMPeripheralClkEnable(PRCM_I2CA0,PRCM_RUN_MODE_CLK);
    MAP_PRCMPeripheralReset(PRCM_I2CA0);
    if(sdk_calls)I2CMasterInitExpClk(I2CA0_BASE,80000000,fast);
    else MAP_I2CMasterInitExpClk(I2CA0_BASE,80000000,fast);
    MAP_GPIOPinWrite(GPIOA3_BASE,0x10,0);delay_ms(2);
    MAP_GPIOPinWrite(GPIOA3_BASE,0x10,0x10);delay_ms(2);
}

static void passive_gpio_probe(void) {
    /* Release both pins, then read them through GPIO. No bit-banging, driven
     * high levels, or extra transactions. Restore the selected I2C pinmux. */
    I2CMasterDisable(I2CA0_BASE);MAP_PRCMPeripheralReset(PRCM_I2CA0);
    MAP_GPIODirModeSet(GPIOA1_BASE,0x30,GPIO_DIR_MODE_IN);
    PinTypeGPIO(PIN_03,PIN_MODE_0,false);PinTypeGPIO(PIN_04,PIN_MODE_0,false);
    PinConfigSet(PIN_03,PIN_STRENGTH_2MA,PIN_TYPE_STD_PU);
    PinConfigSet(PIN_04,PIN_STRENGTH_2MA,PIN_TYPE_STD_PU);
    delay_ms(2);probe_gpio=(MAP_GPIOPinRead(GPIOA1_BASE,0x30)>>4)&3;
    PinTypeI2C(PIN_03,PIN_MODE_5);PinTypeI2C(PIN_04,PIN_MODE_5);
}

bool codec_bus_init(void) {
    attempts[0]=0;probe_gpio=255;
    ring_board_gpio();
    for(attempt=0;attempt<3;attempt++){
        sdk_calls=attempt>0;path=attempt==0?"ring400":attempt==1?"sdk400":"sdk100";
        start_controller(attempt<2);
        bool ok=codec_reg_write(0,0)&&codec_reg_verify(0,0);
        size_t used=strlen(attempts);
        snprintf(attempts+used,sizeof attempts-used,"%s%s:%lx",used?",":"",path,(unsigned long)(ok?0:i2c_error));
        if(ok)return true;
    }
    passive_gpio_probe();
    size_t used=strlen(diagnostic);
    snprintf(diagnostic+used,sizeof diagnostic-used," gpio=%u tries=%s",probe_gpio,attempts);
    log_line(diagnostic);return false;
}

void codec_bus_ready(void) {
    snprintf(diagnostic,sizeof diagnostic,"i2c-ok path=%s tries=%s clock-reg=07 gpio15=%u",
        path,attempts,!!MAP_GPIOPinRead(GPIOA1_BASE,0x80));
    log_line(diagnostic);
}
