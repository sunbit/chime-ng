#include "codec_bus.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum {OK, NACK, CLOCK_TIMEOUT, SOFTWARE_TIMEOUT, ROM_STATUS_MISMATCH};
static uint32_t regs[32],pads[64],ticks;
static unsigned scenario,failure_at,transfers,stops,bytes,valid_stops;
static unsigned path_failmask,current_path,init_count,rom_pins,sdk_pins;
static uint8_t sent[2],gpio[4],direction[4],dac[2][128],page;
static bool read_address,fault,bringup;
static unsigned phase,gpio_sample=3;
static uint8_t startup_led;
static bool clocks[32],controller_initialized,reset_low_after_init;
static char logged[AUDIO_DIAGNOSTIC_SIZE];
static unsigned port_index(unsigned long port){assert(port>=GPIOA0_BASE&&port<=GPIOA3_BASE&&!((port-GPIOA0_BASE)%0x1000));return (port-GPIOA0_BASE)/0x1000;}
uint32_t *mock_reg(uint32_t addr){
    if(addr==OCP_SHARED_BASE+OCP_SHARED_O_GPIO_PAD_CONFIG_12)return &pads[PIN_03];
    if(addr==OCP_SHARED_BASE+OCP_SHARED_O_GPIO_PAD_CONFIG_13)return &pads[PIN_04];
    assert(addr>=I2CA0_BASE&&addr<I2CA0_BASE+sizeof regs&&!(addr&3));return &regs[(addr-I2CA0_BASE)/4];
}
uint32_t millis(void){return ticks++;}
void delay_ms(uint32_t ms){ticks+=ms;}
void log_line(const char *s){snprintf(logged,sizeof logged,"%s",s);}
unsigned long PinModeGet(unsigned long pin){assert(pin<64);return pads[pin]&15;}
void PinTypeGPIO(unsigned long pin,unsigned long mode,tBoolean od){assert(pin<64&&pin!=PIN_02&&mode==0&&!od);pads[pin]=0x20;}
void PinConfigSet(unsigned long pin,unsigned long strength,unsigned long type){assert(pin==PIN_03||pin==PIN_04);pads[pin]=(pads[pin]&~0x3f0)|strength|type;}
void PinTypeI2C(unsigned long pin,unsigned long mode){assert((pin==PIN_03||pin==PIN_04)&&mode==5);pads[pin]=0x135;sdk_pins++;}
void mock_rom_pin_i2c(unsigned long pin,unsigned long mode){PinTypeI2C(pin,mode);sdk_pins--;rom_pins++;}
void GPIODirModeSet(unsigned long port,unsigned char mask,unsigned long dir){unsigned i=port_index(port);assert(clocks[PRCM_GPIOA0+i]);assert(dir==GPIO_DIR_MODE_IN||dir==GPIO_DIR_MODE_OUT);direction[i]=(direction[i]&~mask)|(dir==GPIO_DIR_MODE_OUT?mask:0);}
void GPIOPinWrite(unsigned long port,unsigned char mask,unsigned char value){unsigned i=port_index(port);assert(clocks[PRCM_GPIOA0+i]);gpio[i]=(gpio[i]&~mask)|(value&mask);if(port==GPIOA3_BASE&&mask==0x10&&!value&&controller_initialized)reset_low_after_init=true;}
long GPIOPinRead(unsigned long port,unsigned char pins){unsigned i=port_index(port);if(port==GPIOA1_BASE&&pins==0x30){assert(!(direction[1]&0x30));assert(PinModeGet(PIN_03)==0&&PinModeGet(PIN_04)==0);return gpio_sample<<4;}return gpio[i]&pins;}
tBoolean PRCMPeripheralStatusGet(unsigned long p){assert(p==PRCM_I2CA0);return true;}
void PRCMPeripheralClkEnable(unsigned long p,unsigned long mode){assert(p<32&&mode==PRCM_RUN_MODE_CLK);clocks[p]=true;}
void PRCMPeripheralReset(unsigned long p){assert(p==PRCM_I2CA0&&clocks[p]);memset(regs,0,sizeof regs);phase=0;bytes=0;fault=false;controller_initialized=false;}
static void init_controller(uint32_t base,uint32_t clock,bool fast,unsigned path){
    assert(clock==80000000&&clocks[PRCM_I2CA0]);assert(PinModeGet(PIN_03)==5&&PinModeGet(PIN_04)==5);
    assert(gpio[3]&0x10); /* DAC high before controller initialization. */
    assert(gpio[1]&0x80); /* Ring's missing GPIO15 startup level. */
    assert((gpio[1]&0x08)==startup_led);assert(direction[1]==0x8c&&direction[2]==3&&direction[3]==0x50&&direction[0]==0x40);
    current_path=path;init_count++;controller_initialized=true;reset_low_after_init=false;
    HWREG(base+I2C_O_MCR)=0x10;HWREG(base+I2C_O_MTPR)=fast?9:39;HWREG(base+I2C_O_MBMON)=(path_failmask&(1<<path))?0:3;
}
void I2CMasterInitExpClk(uint32_t b,uint32_t c,bool fast){init_controller(b,c,fast,fast?1:2);}
void mock_rom_i2c_init(uint32_t b,uint32_t c,bool fast){assert(fast);init_controller(b,c,fast,0);}
void I2CMasterDisable(uint32_t base){HWREG(base+I2C_O_MCR)&=~0x10;}
void I2CMasterIntClearEx(uint32_t base,uint32_t flags){assert(base==I2CA0_BASE);HWREG(base+I2C_O_MRIS)&=~flags;}
uint32_t I2CMasterIntStatusEx(uint32_t base,bool masked){assert(!masked);if(scenario==ROM_STATUS_MISMATCH&&transfers==failure_at)return 0;return HWREG(base+I2C_O_MRIS);}
void I2CMasterTimeoutSet(uint32_t base,uint32_t count){HWREG(base+I2C_O_MCLKOCNT)=count;}
void I2CMasterSlaveAddrSet(uint32_t base,uint8_t addr,bool read){assert(addr==0x18);read_address=read;HWREG(base+I2C_O_MSA)=(addr<<1)|read;if(!read){bytes=0;phase=0;}}
void I2CMasterDataPut(uint32_t base,uint8_t data){assert(bytes<2);sent[bytes++]=data;HWREG(base+I2C_O_MDR)=data;}
uint32_t I2CMasterDataGet(uint32_t base){assert(base==I2CA0_BASE&&read_address&&bytes==1);return sent[0]?dac[page][sent[0]]:page;}
uint32_t I2CMasterErr(uint32_t base){return HWREG(base+I2C_O_MCS)&(I2C_MASTER_ERR_ADDR_ACK|I2C_MASTER_ERR_CLK_TOUT);}
void I2CMasterControl(uint32_t base,uint32_t cmd){
    if(cmd==I2C_MASTER_CMD_BURST_SEND_ERROR_STOP&&fault){
        stops++;phase=0;fault=false;HWREG(base+I2C_O_MCS)=I2C_MCS_IDLE;HWREG(base+I2C_O_MRIS)=0;HWREG(base+I2C_O_MBMON)=3;return;
    }
    assert(!HWREG(base+I2C_O_MRIS));
    if(bringup)assert(reset_low_after_init&&(gpio[3]&0x10));
    assert(cmd==(phase==0?I2C_MASTER_CMD_BURST_SEND_START:read_address?I2C_MASTER_CMD_SINGLE_RECEIVE:phase==1?I2C_MASTER_CMD_BURST_SEND_CONT:I2C_MASTER_CMD_BURST_SEND_STOP));
    transfers++;phase++;
    unsigned result=(bringup?(path_failmask&(1<<current_path))!=0:transfers==failure_at)?scenario:OK;
    HWREG(base+I2C_O_MRIS)=I2C_MASTER_INT_DATA;HWREG(base+I2C_O_MCS)=I2C_MCS_IDLE;
    if(result!=OK)fault=true;
    if(result==NACK)HWREG(base+I2C_O_MCS)=I2C_MCS_ERROR|I2C_MCS_ADRACK;
    if(result==CLOCK_TIMEOUT){HWREG(base+I2C_O_MRIS)=I2C_MASTER_INT_TIMEOUT;HWREG(base+I2C_O_MCS)=I2C_MCS_CLKTO;}
    if(result==SOFTWARE_TIMEOUT){HWREG(base+I2C_O_MRIS)=0;HWREG(base+I2C_O_MCS)=I2C_MCS_BUSY;HWREG(base+I2C_O_MBMON)=bringup?0:2;}
    if(cmd==I2C_MASTER_CMD_BURST_SEND_STOP&&result==OK){
        assert(bytes==2);valid_stops++;
        if(sent[0]==0){assert(sent[1]<2);page=sent[1];}
        else if(page==0&&sent[0]==1&&sent[1]==1){memset(dac,0,sizeof dac);page=0;}
        else dac[page][sent[0]]=sent[1];
        phase=0;
    }
}
static void reset(unsigned outcome,unsigned stage){
    memset(regs,0,sizeof regs);memset(pads,0,sizeof pads);memset(gpio,0,sizeof gpio);memset(direction,0,sizeof direction);memset(clocks,0,sizeof clocks);memset(dac,0,sizeof dac);
    page=phase=0;scenario=outcome;failure_at=stage;transfers=stops=bytes=ticks=valid_stops=0;logged[0]=0;
    path_failmask=current_path=init_count=rom_pins=sdk_pins=0;fault=bringup=controller_initialized=reset_low_after_init=false;
    startup_led=0;
    gpio[3]=0x10;pads[PIN_03]=pads[PIN_04]=0x135;
    /* led.c initialized GPIO11 before audio init. Codec must not reset pad OE. */
    direction[1]=8;pads[PIN_02]=0x820;
    HWREG(I2CA0_BASE+I2C_O_MCR)=0x10;HWREG(I2CA0_BASE+I2C_O_MTPR)=9;HWREG(I2CA0_BASE+I2C_O_MBMON)=3;
    HWREG(I2CA0_BASE+I2C_O_MRIS)=I2C_MASTER_INT_DATA;
}
static void test_transactions(void){
    reset(OK,0);assert(codec_reg_write(0,1)&&transfers==3&&!stops&&valid_stops==1&&bytes==2&&sent[0]==0&&sent[1]==1);
    reset(NACK,1);assert(!codec_reg_write(0x20,0x06)&&transfers==1&&stops==1&&bytes==1);
    assert(strstr(logged,"page=1 reg=0x20 step=register error=0x4 mcs=6 ris=1 api=1"));
    assert(strstr(logged,"ctl=10 tpr=9 to=7d sa=30 bus=3/3 pins=5/5 rst=1 clk=1"));
    assert(strstr(logged,"pad=135/135"));assert(HWREG(I2CA0_BASE+I2C_O_MCS)==I2C_MCS_IDLE);
    reset(NACK,2);assert(!codec_reg_write(0,0)&&transfers==2&&stops==1&&strstr(logged,"page=1 reg=0x00 step=data"));
    reset(NACK,3);assert(!codec_reg_write(0,0)&&strstr(logged,"step=stop"));
    reset(CLOCK_TIMEOUT,1);assert(!codec_reg_write(2,0)&&strstr(logged,"error=0x80 mcs=80 ris=2 api=2"));
    reset(SOFTWARE_TIMEOUT,1);ticks=UINT32_MAX-4;
    assert(!codec_reg_write(2,0)&&strstr(logged,"error=0x100 mcs=1 ris=0 api=0"));
    assert(strstr(logged,"bus=3/2")&&stops==1&&ticks<60);
    reset(ROM_STATUS_MISMATCH,1);assert(!codec_reg_write(2,0)&&strstr(logged,"error=0x100 mcs=20 ris=1 api=0"));
    assert(!strcmp(logged,codec_bus_diagnostic())&&strlen(logged)<AUDIO_DIAGNOSTIC_SIZE);
    reset(OK,0);assert(codec_reg_write(0,0));assert(codec_reg_write(4,7));assert(codec_reg_verify(4,7));assert(!codec_reg_verify(4,8)&&strstr(logged,"expected=0x08 got=0x07"));
    reset(NACK,1);uint8_t value=0xff;assert(!codec_reg_read(0,&value)&&value==0xff&&strstr(logged,"step=read-register"));
    reset(NACK,2);assert(!codec_reg_read(0,&value)&&value==0xff&&strstr(logged,"step=read-data"));
}
static void test_bringup(void){
    for(unsigned led=0;led<=8;led+=8){
    for(unsigned mask=0;mask<=3;mask=mask==0?1:mask==1?3:4){
        reset(SOFTWARE_TIMEOUT,0);bringup=true;path_failmask=mask;gpio[2]=0xaa;
        gpio[1]=startup_led=led;
        assert(codec_bus_init());assert(init_count==(mask==0?1:mask==1?2:3));
        assert((gpio[1]&0x08)==led); /* Codec retries must preserve the boot LED. */
        assert(pads[PIN_02]==0x820&&(direction[1]&8));
        assert(gpio[2]==0xaa); /* Preserve unspecified latch levels. */
        assert(rom_pins==2);assert(sdk_pins==(mask==0?0:mask==1?2:4));
        assert(codec_reg_write(4,7)&&codec_reg_verify(4,7));codec_bus_ready();
        assert(strstr(logged,mask==0?"path=ring400":mask==1?"path=sdk400":"path=sdk100"));
        assert(strstr(logged,"clock-reg=07 gpio15=1"));
        assert(HWREG(I2CA0_BASE+I2C_O_MTPR)==(mask==3?39:9));
    }
    }
    reset(SOFTWARE_TIMEOUT,0);bringup=true;path_failmask=7;gpio_sample=3;
    assert(!codec_bus_init()&&init_count==3&&ticks<250);
    assert(strstr(logged,"gpio=3 tries=ring400:100,sdk400:100,sdk100:100"));
    assert(strstr(logged,"path=sdk100 pad=135/135"));
    assert(PinModeGet(PIN_03)==5&&PinModeGet(PIN_04)==5&&!(direction[1]&0x30));
    assert(!(HWREG(I2CA0_BASE+I2C_O_MCR)&0x10)); /* Failed controller released. */
    reset(NACK,0);bringup=true;path_failmask=7;gpio_sample=0;
    assert(!codec_bus_init()&&strstr(logged,"gpio=0 tries=ring400:4,sdk400:4,sdk100:4"));
}
int main(void){
    test_transactions();test_bringup();
    puts("I2C: real codec transaction driver with Ring GPIO/reset sequence, separate STOP, readback, ROM/SDK speed fallback, bounded failures and passive GPIO probe passed");
}
