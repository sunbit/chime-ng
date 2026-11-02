#include "app.h"
#include "driverlib/i2c.h"
#include "driverlib/i2s.h"
#include "driverlib/udma.h"
#include "inc/hw_mcasp.h"
#include "inc/hw_apps_config.h"
#include "inc/hw_udma.h"
#include <string.h>
#include <stdio.h>

/* Pin routes and DAC register sequence recovered from this board's Ring 8.0.3.
 * See docs/hardware.md for the verified setup and remaining limitations. */
#define QUEUE 8
#define SAMPLES 512
#define CHANNEL 5
#define DMA_STALL_MS 2000u
static uint32_t dma_table[256] __attribute__((aligned(1024)));
static int16_t pcm[QUEUE][SAMPLES*2],mono[SAMPLES];
static volatile uint8_t state[QUEUE]; /* 0 free, 1 ready, 2 DMA-owned */
static volatile int owner[2]={-1,-1};
static volatile unsigned consume;
static unsigned produce;
static volatile bool playing,finished,eof;
static bool codec_ok,wav_ok;
static const char *error="none";
static char diagnostic[AUDIO_DIAGNOSTIC_SIZE];
static volatile unsigned next_side,completed,irq_count,poll_count,tx_flags;
static uint32_t started_at,progress_at,last_progress,play_limit,diagnostic_at;
static const char *end_phase;
static const char *start_phase="idle";
static unsigned prefill;
static struct {
    unsigned long mode[2],left[2],gbl,fifo,mask,dma_error;
    unsigned long fctl,evt,ser[2],dir,dstat,map,req;
    unsigned enabled,table_ok;
} registers;
const char *audio_diagnostic(void) {
    return !codec_ok||!diagnostic[0]?codec_bus_diagnostic():diagnostic;
}
static int wav_fd=-1;
static uint32_t wav_start,wav_length,position;

static unsigned descriptor(unsigned side) { return CHANNEL|(side?UDMA_ALT_SELECT:UDMA_PRI_SELECT); }
/* Capture before disabling DMA/I2S, so a timeout retains the failing state. */
static void capture(void) {
    tx_flags|=I2SIntStatus(I2S_BASE)&0x1ff;
    for(unsigned s=0;s<2;s++) {
        registers.mode[s]=MAP_uDMAChannelModeGet(descriptor(s));
        registers.left[s]=MAP_uDMAChannelSizeGet(descriptor(s));
    }
    registers.enabled=MAP_uDMAChannelIsEnabled(CHANNEL);
    registers.gbl=HWREG(I2S_BASE+MCASP_O_GBLCTL);
    registers.fifo=MAP_I2STxFIFOStatusGet(I2S_BASE);
    registers.mask=HWREG(APPS_CONFIG_BASE+APPS_CONFIG_O_DMA_DONE_INT_MASK);
    registers.dma_error=MAP_uDMAErrorStatusGet();
    registers.fctl=HWREG(I2S_BASE+MCASP_0_WFIFOCTL);
    registers.evt=HWREG(I2S_BASE+MCASP_O_XEVTCTL);
    registers.ser[0]=HWREG(I2S_BASE+MCASP_O_XRSRCTL0);
    registers.ser[1]=HWREG(I2S_BASE+MCASP_O_XRSRCTL1);
    registers.dir=HWREG(I2S_BASE+MCASP_O_PDIR);
    registers.dstat=HWREG(UDMA_BASE+UDMA_O_STAT);
    registers.map=(HWREG(UDMA_BASE+UDMA_O_CHMAP0)>>(CHANNEL*4))&15;
    registers.req=(HWREG(UDMA_BASE+UDMA_O_REQMASKSET)>>CHANNEL)&1;
    registers.table_ok=HWREG(UDMA_BASE+UDMA_O_CTLBASE)==(uint32_t)(uintptr_t)dma_table;
}
static void snapshot(const char *phase) {
    snprintf(diagnostic,sizeof diagnostic,
        "audio=%s irq=%u done=%u poll=%u pos=%lu/%lu dma=%lu/%lu left=%lu/%lu en=%u tx=%x gbl=%lx fifo=%lu mask=%lx err=%lu stage=%s pre=%u fctl=%lx evt=%lx ser=%lx/%lx dir=%lx ds=%lx map=%lx req=%lx tbl=%u",
        phase,irq_count,completed,poll_count,(unsigned long)position,(unsigned long)wav_length,
        registers.mode[0],registers.mode[1],registers.left[0],registers.left[1],
        registers.enabled,tx_flags,registers.gbl,registers.fifo,registers.mask,registers.dma_error,
        start_phase,prefill,registers.fctl,registers.evt,registers.ser[0],registers.ser[1],
        registers.dir,registers.dstat,registers.map,registers.req,registers.table_ok);
}
static void finish(const char *reason) {
    if(finished)return;
    if(reason)error=reason;
    end_phase=reason?reason:"done";capture();
    I2SIntDisable(I2S_BASE,I2S_INT_XDMA);
    MAP_uDMAChannelDisable(CHANNEL);
    finished=true;
}

static bool amp(bool enable) { return codec_reg_write(0,1)&&codec_reg_write(0x20,enable?0x86:0x06); }
static void configure_i2s(void) {
    /* Reset only I2S, never the DMA controller/table shared with SimpleLink.
     * GBLCTL=0 alone does not reset configuration (including DMA event gating). */
    PRCMPeripheralReset(PRCM_I2S);
    I2SIntDisable(I2S_BASE,0xc0ff00ff);
    I2STxFIFODisable(I2S_BASE);
    MAP_PRCMI2SClockFreqSet(22050*32);
    HWREG(I2S_BASE+0xb0)=0xa0;HWREG(I2S_BASE+0xb4)=0x8000;
    HWREG(I2S_BASE+0xa8)=0x18074;HWREG(I2S_BASE+0x68)=0x18070;
    HWREG(I2S_BASE+0xac)=0x113;HWREG(I2S_BASE+0x6c)=0x113;
    HWREG(I2S_BASE+0xa4)=0xffff;HWREG(I2S_BASE+0x64)=0xffff;
    HWREG(I2S_BASE+0xb8)=3;HWREG(I2S_BASE+0x78)=3;
    MAP_I2SSerializerConfig(I2S_BASE,1,I2S_SER_MODE_TX,I2S_INACT_LOW_LEVEL);
    HWREG(I2S_BASE+MCASP_O_XEVTCTL)=0; /* XDATDMA=0 enables TX DMA events. */
    /* FIFO must stay disabled until the DMA descriptors and channel are ready. */
}
/* Ring's TX reset-release order, with the McASP prefill check between XSER
 * and XSM (TI SPRU041J 3.1.1 steps 6-8). Do not clock an empty transmitter.
 * Each wait is bounded so a missing DMA request leaves web/OTA usable. */
static bool start_i2s(void) {
    HWREG(I2S_BASE+MCASP_O_PDIR)|=0x14000000;
    static const uint32_t stages[]={0x200,0x100,0x400,0x800,0x1000};
    static const char *const names[]={"hclk","bclk","serializer","state","sync"};
    for(unsigned i=0;i<sizeof stages/sizeof stages[0];i++) {
        start_phase=names[i];
        uint32_t bits=HWREG(I2S_BASE+MCASP_O_GBLCTL)|stages[i],start=millis();
        HWREG(I2S_BASE+MCASP_O_GBLCTL)=bits;
        while(HWREG(I2S_BASE+MCASP_O_GBLCTL)!=bits)
            if((uint32_t)(millis()-start)>=10)return false;
        if(stages[i]==0x400) {
            start_phase="prefill";start=millis();
            for(;;) {
                unsigned left=MAP_uDMAChannelSizeGet(descriptor(0));
                prefill=SAMPLES*2-left;
                unsigned status=I2SIntStatus(I2S_BASE);tx_flags|=status&0x1ff;
                if(prefill&&!(status&I2S_STS_XDATA))break;
                if(MAP_uDMAErrorStatusGet()||(uint32_t)(millis()-start)>=10)return false;
            }
        }
    }
    start_phase="running";
    return true;
}
static bool arm_dma(unsigned side) {
    if(state[consume]!=1)return false;
    unsigned slot=consume;consume=(consume+1)%QUEUE;state[slot]=2;owner[side]=(int)slot;
    MAP_uDMAChannelTransferSet(descriptor(side),
        UDMA_MODE_PINGPONG,pcm[slot],(void*)I2S_TX_DMA_PORT,SAMPLES*2);
    return true;
}
/* Called in the ISR, or with INT_I2S masked by the main loop. The polling
 * path also reaps descriptors if a DMA-done interrupt was lost. */
static void service_dma(bool from_irq) {
    unsigned status=I2SIntStatus(I2S_BASE);
    I2SIntClear(I2S_BASE,status);
    if(!playing||finished)return;
    tx_flags|=status&0x1ff;
    for(unsigned i=0;i<2;i++) {
        unsigned side=next_side;
        if(owner[side]<0||MAP_uDMAChannelModeGet(descriptor(side))!=UDMA_MODE_STOP)break;
        state[owner[side]]=0;owner[side]=-1;completed++;next_side^=1;
        if(!from_irq)poll_count++;
        if(!arm_dma(side)&&!eof){finish("underrun");return;}
    }
    if(owner[0]<0&&owner[1]<0){finish(NULL);return;}
    if(MAP_uDMAErrorStatusGet()){finish("dma-error");return;}
    if(status&(I2S_STS_XDMAERR|I2S_STS_XSYNCERR|I2S_STS_XUNDRN)) {
        finish("i2s-error");return;
    }
    /* Hardware can disable the channel when both descriptors reach STOP. */
    MAP_uDMAChannelEnable(CHANNEL);
}
static void audio_irq(void) {
    irq_count++;service_dma(true);
}
bool audio_init(void) {
    codec_ok=codec_bus_init()&&codec_reg_write(1,1);delay_ms(10);
    static const uint8_t init[][2]={
        {0,0},{4,7},{6,0x3c},{7,0},{8,0},{5,0x92},{0x0b,0x83},{0x0c,0x8a},
        {0x0d,0},{0x0e,0x80},{0,0},{0x3c,5},{0,1},{0x21,0x4e},{0x24,0},
        {0x1f,4},{0x28,6},{0,1},{0x23,0x40},{0,0},{0x3f,0xb6},{0x40,4},
        {0x41,0},{0,1},{0x1f,0x14},{0x22,0x70},{0x2a,4},{0x26,0x80},
        {0,0},{0x52,0x68},{0x53,0x68}};
    for(unsigned i=0;codec_ok&&i<sizeof init/sizeof init[0];i++)codec_ok=codec_reg_write(init[i][0],init[i][1]);
    codec_ok=codec_ok&&codec_reg_write(0,0)&&codec_reg_verify(4,7)&&amp(false);
    if(codec_ok)codec_bus_ready();
    if(!codec_ok)error="codec";
    int fd=fs_open_read(sound_path()),size=fs_size(sound_path());uint8_t head[1024];
    int n=fd<0?-1:fs_read(fd,0,head,sizeof head);if(fd>=0)fs_close(fd);
    wav_ok=n>0&&size>0&&wav_header(head,n,size,&wav_start,&wav_length);
    if(!wav_ok && codec_ok)error="wav";
    MAP_PRCMPeripheralClkEnable(PRCM_I2S,PRCM_RUN_MODE_CLK);
    MAP_PinTypeI2S(PIN_50,PIN_MODE_6);MAP_PinTypeI2S(PIN_64,PIN_MODE_7);
    MAP_PinTypeI2S(PIN_62,PIN_MODE_13);MAP_PinTypeI2S(PIN_45,PIN_MODE_12);
    MAP_PRCMPeripheralClkEnable(PRCM_UDMA,PRCM_RUN_MODE_CLK);
    MAP_uDMAControlBaseSet(dma_table);MAP_uDMAEnable();MAP_uDMAChannelAssign(UDMA_CH5_I2S_TX);
    configure_i2s();
    /* Use the inspectable TI C interrupt implementation, including APPS_CONFIG
     * DMA-done mask/ACK and patched IntEnable, rather than the ROM wrappers. */
    I2SIntDisable(I2S_BASE,0xc0ff00ff);I2SIntRegister(I2S_BASE,audio_irq);
    MAP_IntDisable(INT_I2S);
    return audio_ready();
}
bool audio_ready(void) { return codec_ok&&wav_ok; }
void audio_use_sound(uint32_t offset,uint32_t length) {
    wav_start=offset;wav_length=length;wav_ok=true;
    if(codec_ok)error="none";
}
bool audio_playing(void) { return playing; }
const char *audio_error(void) { return error; }
static void fill_queue(void) {
    while(!finished && !eof && state[produce]==0) {
        unsigned n=wav_length-position;if(n>sizeof mono)n=sizeof mono;
        if(fs_read(wav_fd,wav_start+position,mono,n)!=(int)n) {
            error="storage";eof=true;finished=true;return;
        }
        position+=n;
        for(unsigned i=0;i<SAMPLES;i++) {
            int16_t sample=i<n/2?(int32_t)mono[i]*config.volume/100:0;
            pcm[produce][2*i]=sample;pcm[produce][2*i+1]=sample;
        }
        __atomic_thread_fence(__ATOMIC_RELEASE);state[produce]=1;produce=(produce+1)%QUEUE;
        if(position>=wav_length)eof=true;
    }
}
bool audio_play(void) {
    if(playing||!audio_ready()||app_busy())return false;
    wav_fd=fs_open_read(sound_path());if(wav_fd<0){error="storage";return false;}
    MAP_IntDisable(INT_I2S);MAP_uDMAChannelDisable(CHANNEL);
    MAP_uDMAChannelAttributeDisable(CHANNEL,UDMA_ATTR_ALL);
    MAP_uDMAChannelAttributeEnable(CHANNEL,UDMA_ATTR_USEBURST);
    MAP_uDMAErrorStatusClear();
    for(unsigned s=0;s<2;s++) {
        /* Clear stale STOP/count/mode on replay without disturbing SPI's table. */
        dma_table[descriptor(s)*4+2]=0;
        MAP_uDMAChannelControlSet(descriptor(s),UDMA_SIZE_16|UDMA_SRC_INC_16|UDMA_DST_INC_NONE|UDMA_ARB_8);
    }
    position=0;produce=consume=0;owner[0]=owner[1]=-1;
    next_side=completed=irq_count=poll_count=tx_flags=0;prefill=0;start_phase="config";
    diagnostic[0]=0;end_phase=NULL;
    memset((void*)state,0,sizeof state);eof=finished=false;error="none";
    fill_queue();if(finished){fs_close(wav_fd);wav_fd=-1;return false;}
    configure_i2s();
    if(!amp(true)){fs_close(wav_fd);wav_fd=-1;codec_ok=false;error="codec";return false;}
    arm_dma(0);arm_dma(1);playing=true;
    started_at=progress_at=diagnostic_at=millis();last_progress=0;
    play_limit=((wav_length/2+SAMPLES-1)/SAMPLES*SAMPLES*1000u)/22050u+DMA_STALL_MS;
    I2SIntClear(I2S_BASE,I2SIntStatus(I2S_BASE));
    I2SIntEnable(I2S_BASE,I2S_INT_XDMA);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    MAP_uDMAChannelEnable(CHANNEL);
    /* Ring 0x200207cc arms/enables DMA before 0x20019a18 enables the FIFO. */
    I2STxFIFOEnable(I2S_BASE,8,1);
    if(!start_i2s()) {finish("i2s-start");audio_poll();return false;}
    capture();snapshot("playing");MAP_IntEnable(INT_I2S);
    return true;
}
void audio_poll(void) {
    if(!playing)return;
    MAP_IntDisable(INT_I2S);service_dma(false);
    uint32_t now=millis(),progress=completed*SAMPLES*2;
    for(unsigned s=0;s<2;s++)if(owner[s]>=0)
        progress+=SAMPLES*2-MAP_uDMAChannelSizeGet(descriptor(s));
    if(progress!=last_progress){last_progress=progress;progress_at=now;}
    if(!finished&&(uint32_t)(now-progress_at)>=DMA_STALL_MS)finish("dma-timeout");
    if(!finished&&(uint32_t)(now-started_at)>=play_limit)finish("audio-timeout");
    if(!finished&&(uint32_t)(now-diagnostic_at)>=1000){capture();snapshot("playing");diagnostic_at=now;}
    if(!finished)MAP_IntEnable(INT_I2S);
    if(!finished)fill_queue();
    if(finished) {
        MAP_IntDisable(INT_I2S);
        if(!end_phase){end_phase=error;capture();}
        snapshot(end_phase);
        I2SIntDisable(I2S_BASE,I2S_INT_XDMA);MAP_uDMAChannelDisable(CHANNEL);
        delay_ms(2);PRCMPeripheralReset(PRCM_I2S);
        I2STxFIFODisable(I2S_BASE);
        I2SIntClear(I2S_BASE,I2SIntStatus(I2S_BASE));
        if(!amp(false)){codec_ok=false;error="codec";}
        if(wav_fd>=0)fs_close(wav_fd);wav_fd=-1;
        playing=false;finished=false;
    }
}
