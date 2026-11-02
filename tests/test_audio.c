/* Exercise the production audio state machine against a DMA/codec/storage
 * model. Tests deliberately omit interrupts, coalesce completions, stop clocks,
 * fail reads and replay. They cannot validate physical pins or audible sound. */
#include "app.h"
#include "inc/hw_mcasp.h"
#include "inc/hw_apps_config.h"
#include "inc/hw_udma.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Config config;
static uint32_t ticks,step,mask=0xffffffff,table_shadow[256];
static uint32_t mcasp[0x1100/4],udma[0x600/4];
#define gbl mcasp[MCASP_O_GBLCTL/4]
#define pdir mcasp[MCASP_O_PDIR/4]
static uint32_t *control_table;
static unsigned attributes,irq_mask,tx_status,dma_error,page,active_side;
static bool dma_enabled,irq_enabled,amp_enabled,start_stuck,busy,codec_fail;
static bool prefilled,prefill_stuck;
static unsigned prefill_reads,resets;
static void (*handler)(void);
static uint8_t *wav;
static size_t wav_size,words_played;
static int opened,closes,read_calls,fail_read=-1;
static struct {unsigned mode,left;const int16_t *data;unsigned transferred;} dma[2];
static void transfer(unsigned count,bool interrupt,bool check_pcm);
static void service_prefill(void) {
    if(gbl!=0x700||prefilled)return;
    tx_status|=I2S_STS_XDATA;
    if(prefill_stuck||prefill_reads--)return;
    assert(dma_enabled&&mcasp[MCASP_0_WFIFOCTL/4]==0x10801);
    assert(!mcasp[MCASP_O_XEVTCTL/4]&&dma[0].mode==UDMA_MODE_PINGPONG);
    prefilled=true;transfer(8,false,true);
    mcasp[MCASP_0_WFIFOSTS/4]=7;tx_status&=~I2S_STS_XDATA;
}

uint32_t *mock_reg(uint32_t address) {
    if(address==I2S_BASE+MCASP_O_GBLCTL){
        if(start_stuck)gbl=0;
        /* Releasing the TX state machine with an empty XBUF underruns. */
        if(gbl&0x800)assert(prefilled);
    }
    if(address>=I2S_BASE&&address<I2S_BASE+sizeof mcasp)return &mcasp[(address-I2S_BASE)/4];
    if(address>=UDMA_BASE&&address<UDMA_BASE+sizeof udma)return &udma[(address-UDMA_BASE)/4];
    if(address==APPS_CONFIG_BASE+APPS_CONFIG_O_DMA_DONE_INT_MASK)return &mask;
    assert(!"unexpected register");return NULL;
}
uint32_t millis(void){uint32_t value=ticks;ticks+=step;return value;}
void delay_ms(uint32_t ms){ticks+=ms;}
bool app_busy(void){return busy;}
bool codec_bus_init(void){return !codec_fail;}
bool codec_reg_write(uint8_t reg,uint8_t value) {
    if(codec_fail)return false;
    if(!reg)page=value;
    if(page==1&&reg==0x20){assert(value==0x86||value==0x06);amp_enabled=value==0x86;}
    return true;
}
bool codec_reg_verify(uint8_t reg,uint8_t expected){assert(page==0&&reg==4&&expected==7);return !codec_fail;}
void codec_bus_ready(void){}
const char *codec_bus_diagnostic(void){return codec_fail?"codec-test-error":"i2c-ok path=ring400";}
const char *sound_path(void){return WAV_PATH;}
int fs_size(const char *name){assert(!strcmp(name,WAV_PATH));return (int)wav_size;}
int fs_open_read(const char *name){assert(!strcmp(name,WAV_PATH));assert(!opened);opened=1;return 3;}
int fs_close(int fd){assert(fd==3&&opened);opened=0;closes++;return 0;}
int fs_read(int fd,uint32_t offset,void *p,size_t n) {
    assert(fd==3&&opened);read_calls++;
    if(read_calls==fail_read)return -1;
    if(offset+n>wav_size)n=wav_size-offset;
    memcpy(p,wav+offset,n);return (int)n;
}
void PRCMPeripheralClkEnable(unsigned long p,unsigned long mode){assert(mode==PRCM_RUN_MODE_CLK);assert(p==PRCM_I2S||p==PRCM_UDMA);}
void PRCMPeripheralReset(unsigned long p){assert(p==PRCM_I2S);memset(mcasp,0,sizeof mcasp);tx_status=0;prefilled=false;resets++;}
void PRCMI2SClockFreqSet(unsigned long hz){assert(hz==22050*32);}
void PinTypeI2S(unsigned long pin,unsigned long mode){assert((pin==PIN_50&&mode==6)||(pin==PIN_64&&mode==7)||(pin==PIN_62&&mode==13)||(pin==PIN_45&&mode==12));}
void I2SDisable(unsigned long base){assert(base==I2S_BASE);gbl=0;}
void I2SSerializerConfig(unsigned long b,unsigned long line,unsigned long mode,unsigned long inactive){assert(b==I2S_BASE&&line==1&&mode==I2S_SER_MODE_TX&&inactive==I2S_INACT_LOW_LEVEL);mcasp[MCASP_O_XRSRCTL0/4]=mode|inactive;pdir|=line;}
void I2STxFIFODisable(unsigned long b){assert(b==I2S_BASE);mcasp[MCASP_0_WFIFOCTL/4]=0;}
void I2STxFIFOEnable(unsigned long b,unsigned long level,unsigned long words){
    assert(b==I2S_BASE&&level==8&&words==1);
    /* Initial FIFO request must find a channel with both valid descriptors. */
    assert(dma_enabled&&dma[0].mode==UDMA_MODE_PINGPONG&&dma[1].mode==UDMA_MODE_PINGPONG);
    assert(gbl==0);mcasp[MCASP_0_WFIFOCTL/4]=0x10801;prefill_reads=3;
}
unsigned long I2STxFIFOStatusGet(unsigned long b){assert(b==I2S_BASE);return mcasp[MCASP_0_WFIFOSTS/4];}
void IntDisable(unsigned long i){assert(i==INT_I2S);irq_enabled=false;}
void IntEnable(unsigned long i){assert(i==INT_I2S);irq_enabled=true;}
void I2SIntRegister(unsigned long b,void (*fn)(void)){assert(b==I2S_BASE);handler=fn;IntEnable(INT_I2S);}
void I2SIntEnable(unsigned long b,unsigned long flags){assert(b==I2S_BASE);irq_mask|=flags;mask&=~((flags&0xc0000000)>>20);}
void I2SIntDisable(unsigned long b,unsigned long flags){assert(b==I2S_BASE);irq_mask&=~flags;mask|=(flags&0xc0000000)>>20;}
unsigned long I2SIntStatus(unsigned long b){assert(b==I2S_BASE);service_prefill();return tx_status;}
void I2SIntClear(unsigned long b,unsigned long flags){assert(b==I2S_BASE);tx_status&=~flags;}
void uDMAEnable(void){udma[UDMA_O_STAT/4]=1;}
void uDMAControlBaseSet(void *p){assert(!((uintptr_t)p%1024));control_table=p;udma[UDMA_O_CTLBASE/4]=(uint32_t)(uintptr_t)p;}
void uDMAChannelAssign(unsigned long ch){assert(ch==UDMA_CH5_I2S_TX);udma[UDMA_O_CHMAP0/4]=2u<<20;}
void uDMAChannelAttributeDisable(unsigned long ch,unsigned long a){assert(ch==5);attributes&=~a;if(a&UDMA_ATTR_ALTSELECT)active_side=0;}
void uDMAChannelAttributeEnable(unsigned long ch,unsigned long a){assert(ch==5);attributes|=a;}
void uDMAChannelControlSet(unsigned long ch,unsigned long control) {
    assert((ch&31)==5&&control==(UDMA_SIZE_16|UDMA_SRC_INC_16|UDMA_DST_INC_NONE|UDMA_ARB_8));
    /* The production code must clear stale descriptor mode/count on replay. */
    assert(control_table[(ch&63)*4+2]==0);
    dma[(ch>>5)&1].mode=dma[(ch>>5)&1].left=0;
    control_table[(ch&63)*4+2]=(uint32_t)control;
}
void uDMAChannelTransferSet(unsigned long ch,unsigned long mode,void *src,void *dst,unsigned long size) {
    assert((ch&31)==5&&mode==UDMA_MODE_PINGPONG&&size==1024&&dst==(void*)I2S_TX_DMA_PORT);
    unsigned side=(ch>>5)&1;assert(dma[side].mode==UDMA_MODE_STOP);
    dma[side].mode=mode;dma[side].left=size;dma[side].data=src;dma[side].transferred=0;
}
void uDMAChannelEnable(unsigned long ch){assert(ch==5);dma_enabled=true;}
void uDMAChannelDisable(unsigned long ch){assert(ch==5);dma_enabled=false;}
tBoolean uDMAChannelIsEnabled(unsigned long ch){assert(ch==5);return dma_enabled;}
unsigned long uDMAChannelModeGet(unsigned long ch){assert((ch&31)==5);return dma[(ch>>5)&1].mode;}
unsigned long uDMAChannelSizeGet(unsigned long ch){assert((ch&31)==5);service_prefill();return dma[(ch>>5)&1].left;}
unsigned long uDMAErrorStatusGet(void){return dma_error;}
void uDMAErrorStatusClear(void){dma_error=0;}
static void raise_irq(void){assert(irq_enabled&&handler&&(irq_mask&I2S_INT_XDMA));handler();}
static void transfer(unsigned count,bool interrupt,bool check_pcm) {
    assert(dma_enabled&&(gbl==0x1f00||gbl==0x700)&&amp_enabled);
    unsigned side=active_side;assert(dma[side].mode==UDMA_MODE_PINGPONG&&dma[side].left>=count);
    for(unsigned i=0;i<count;i++) {
        if(check_pcm) {
            size_t sample=words_played/2;
            int16_t expected=0;
            if(sample<(wav_size-44)/2) {memcpy(&expected,wav+44+sample*2,2);expected=(int32_t)expected*config.volume/100;}
            assert(dma[side].data[dma[side].transferred]==expected);
        }
        dma[side].transferred++;words_played++;
    }
    dma[side].left-=count;
    if(!dma[side].left) {
        dma[side].mode=UDMA_MODE_STOP;active_side^=1;
        if(dma[active_side].mode==UDMA_MODE_STOP)dma_enabled=false;
        tx_status|=I2S_STS_XDMA;
        if(interrupt)raise_irq();
    }
}
static void complete(bool interrupt,bool check_pcm){transfer(dma[active_side].left,interrupt,check_pcm);}
static void begin(void) {
    words_played=0;assert(!opened&&!audio_playing());assert(audio_play());
    assert(audio_playing()&&amp_enabled&&opened&&!(mask&0x800)&&irq_enabled);
    assert(attributes==UDMA_ATTR_USEBURST&&active_side==0);
    assert(prefilled&&dma[0].left==1016&&words_played==8);
    assert(strstr(audio_diagnostic(),"stage=running pre=8")&&strstr(audio_diagnostic(),"map=2 req=0 tbl=1"));
}
static void stopped(const char *err) {
    assert(!audio_playing()&&!opened&&!amp_enabled&&!dma_enabled&&!irq_enabled);
    assert(!strcmp(audio_error(),err));assert(audio_ready());assert(gbl==0);
    assert(strlen(audio_diagnostic())<AUDIO_DIAGNOSTIC_SIZE);
    /* Shared SPI descriptors must survive audio replay/cancellation. */
    assert(control_table[12*4]==table_shadow[12*4]);
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);wav_size=ftell(file);rewind(file);
    wav=malloc(wav_size);assert(wav&&fread(wav,1,wav_size,file)==wav_size);fclose(file);
    assert(wav_size==228396);config.volume=25;assert(audio_init()&&audio_ready());
    assert(!audio_playing()&&!amp_enabled&&!opened&&!irq_enabled);
    control_table[12*4]=table_shadow[12*4]=0x12345678;
    busy=true;assert(!audio_play());busy=false;
    /* Two complete/repeat plays compare every stereo sample with the original
     * WAV including signed volume scaling and the zero-padded final block. */
    for(unsigned run=0;run<2;run++) {
        begin();assert(!audio_play());unsigned blocks=0;
        while(audio_playing()) {
            complete(true,true);ticks+=23;audio_poll();assert(++blocks<300);
        }
        stopped("none");assert(blocks==223&&words_played==223*1024);
        assert(strstr(audio_diagnostic(),"audio=done irq=223 done=223 poll=0"));
    }
    /* Lost IRQ: polling notices STOP and refills in stream order. */
    begin();for(unsigned block=0;audio_playing();block++) {
        assert(block<300);complete(false,true);ticks+=23;audio_poll();
    }
    stopped("none");assert(strstr(audio_diagnostic(),"irq=0 done=223 poll=223"));
    /* Coalesced completions with the alternate descriptor next. Both-stop
     * disables the channel; the driver must re-enable it, preserving order. */
    begin();complete(true,true);ticks+=23;audio_poll();
    complete(false,true);complete(false,true);assert(!dma_enabled);
    raise_irq();assert(dma_enabled);ticks+=46;audio_poll();
    while(audio_playing()){complete(true,true);ticks+=23;audio_poll();}
    stopped("none");assert(words_played==223*1024);
    /* Stalled clock/DMA: bounded cleanup, then a new request can start. */
    begin();audio_poll();ticks+=1999;audio_poll();assert(audio_playing());ticks++;audio_poll();
    stopped("dma-timeout");assert(strstr(audio_diagnostic(),"left=1016/1024")&&strstr(audio_diagnostic(),"irq=0 done=0"));
    /* Counter wrap must not prevent the no-progress timeout. */
    ticks=UINT32_MAX-100;begin();audio_poll();ticks+=2000;audio_poll();stopped("dma-timeout");
    /* Genuine partial DMA progress extends the no-progress timer, while the
     * overall duration still bounds an implausibly slow transfer. */
    ticks=100;begin();unsigned cycles=0;
    while(audio_playing()){transfer(8,false,false);ticks+=1500;audio_poll();assert(++cycles<10);}
    stopped("audio-timeout");
    /* Read failure after startup frees the open file and stops DMA. */
    begin();complete(true,true);fail_read=read_calls+1;audio_poll();stopped("storage");fail_read=-1;
    fail_read=read_calls+1;assert(!audio_play()&&!opened&&!audio_playing());fail_read=-1;
    /* Queue starvation in the ISR, and peripheral/controller errors. */
    begin();for(unsigned i=0;i<7;i++)complete(true,true);
    audio_poll();stopped("underrun");
    begin();dma_error=1;audio_poll();stopped("dma-error");
    begin();tx_status|=I2S_STS_XSYNCERR;audio_poll();stopped("i2s-error");
    /* I2S reset-release cannot hang inside the play request. */
    start_stuck=true;step=1;uint32_t before=ticks;assert(!audio_play());
    assert((uint32_t)(ticks-before)<100);stopped("i2s-start");start_stuck=false;step=0;
    begin();audio_poll();ticks+=2000;audio_poll();stopped("dma-timeout");
    /* Lost initial DMA request: do not release state/frame sync. The original
     * 0.1.4 signature is captured, including unchanged counts and empty FIFO. */
    prefill_stuck=true;step=1;before=ticks;assert(!audio_play());
    assert((uint32_t)(ticks-before)<100);stopped("i2s-start");
    assert(strstr(audio_diagnostic(),"stage=prefill pre=0"));
    assert(strstr(audio_diagnostic(),"left=1024/1024")&&strstr(audio_diagnostic(),"gbl=700 fifo=0"));
    assert(strstr(audio_diagnostic(),"fctl=10801 evt=0 ser=9/0 dir=14000001 ds=1 map=2 req=0 tbl=1"));
    prefill_stuck=false;step=0;
    /* Restart must reset retained I2S event gating and serializer settings. */
    mcasp[MCASP_O_XEVTCTL/4]=1;mcasp[MCASP_O_XRSRCTL1/4]=9;
    unsigned prior_resets=resets;begin();assert(resets==prior_resets+1);
    audio_poll();ticks+=2000;audio_poll();stopped("dma-timeout");
    codec_fail=true;assert(!audio_play()&&!opened&&!audio_playing());assert(!audio_ready()&&!strcmp(audio_error(),"codec"));
    assert(!strcmp(audio_diagnostic(),"codec-test-error"));
    free(wav);printf("AUDIO: real playback driver, generated WAV stereo/volume/padding, replay, missing/coalesced IRQs, DMA re-enable, stall/deadline/wrap, read/queue/codec faults and bounded I2S startup/prefill/reset passed (%d closes)\n",closes);
    return 0;
}
