#include "platform.h"
#include <string.h>
#include <stdio.h>
#include "inc/hw_wdt.h"
static volatile uint32_t ticks;
volatile bool wifi_ip_ready,wifi_connected;
volatile uint32_t wifi_ip;
uint8_t device_mac[6];
extern void (*vectors[])(void);
void SysTick_Handler(void) {
    ticks++;
    /* One-shot indication also expires while network initialization waits. */
    board_led_tick();
}
uint32_t millis(void) { return ticks; }
void board_init(void) {
    MAP_IntVTableBaseSet((unsigned long)vectors);
    PRCMCC3200MCUInit();
    MAP_PRCMPeripheralClkEnable(PRCM_UARTA0,PRCM_RUN_MODE_CLK);
    MAP_PinTypeUART(PIN_55,PIN_MODE_3);MAP_PinTypeUART(PIN_57,PIN_MODE_3);
    MAP_UARTConfigSetExpClk(UARTA0_BASE,80000000,115200,UART_CONFIG_WLEN_8|UART_CONFIG_STOP_ONE|UART_CONFIG_PAR_NONE);
    MAP_SysTickPeriodSet(80000);MAP_SysTickIntEnable();MAP_SysTickEnable();
    MAP_IntMasterEnable();
    MAP_PRCMPeripheralClkEnable(PRCM_WDT,PRCM_RUN_MODE_CLK);
    MAP_WatchdogUnlock(WDT_BASE);MAP_WatchdogReloadSet(WDT_BASE,80000000u*20u);
    HWREG(WDT_BASE + WDT_O_CTL) |= WDT_CTL_RESEN;
    MAP_WatchdogEnable(WDT_BASE);
    MAP_WatchdogIntClear(WDT_BASE);MAP_WatchdogLock(WDT_BASE);
}
void watchdog_feed(void) { MAP_WatchdogUnlock(WDT_BASE);MAP_WatchdogIntClear(WDT_BASE);MAP_WatchdogLock(WDT_BASE); }
void log_line(const char *s) { for(;*s;s++)MAP_UARTCharPut(UARTA0_BASE,*s);MAP_UARTCharPut(UARTA0_BASE,'\r');MAP_UARTCharPut(UARTA0_BASE,'\n'); }
void delay_ms(uint32_t ms) { uint32_t start=millis();while((uint32_t)(millis()-start)<ms){} }
void reboot(void) { sl_Stop(200);MAP_PRCMMCUReset(true);for(;;){} }
void network_pump(void) { _SlNonOsMainLoopTask(); }
void SimpleLinkWlanEventHandler(SlWlanEvent_t *e) {
    if(e->Event==SL_WLAN_CONNECT_EVENT)wifi_connected=true;
    if(e->Event==SL_WLAN_DISCONNECT_EVENT) { wifi_connected=false;wifi_ip_ready=false;wifi_ip=0; }
}
void SimpleLinkNetAppEventHandler(SlNetAppEvent_t *e) {
    if(e->Event==SL_NETAPP_IPV4_IPACQUIRED_EVENT) {
        wifi_ip=e->EventData.ipAcquiredV4.ip;wifi_ip_ready=true;
        char ip[16],line[64];ip_format(wifi_ip,ip);
        snprintf(line,sizeof line,"Wi-Fi IP: %s; web: http://%s",ip,ip);log_line(line);
    }
}
void SimpleLinkHttpServerCallback(SlHttpServerEvent_t *e,SlHttpServerResponse_t *r) { (void)e;(void)r; }
void SimpleLinkSockEventHandler(SlSockEvent_t *e) { (void)e; }
int fs_open_read(const char *name) { _i32 fd=-1;_u32 token=0;int rc=sl_FsOpen((const _u8*)name,FS_MODE_OPEN_READ,&token,&fd);return rc<0?rc:(int)fd; }
int fs_open_write(const char *name,uint32_t cap,bool mirror) {
    _i32 fd=-1;_u32 token=0;SlFsFileInfo_t info;
    int rc=sl_FsGetInfo((const _u8*)name,0,&info);
    if(rc>=0 && info.AllocatedLen<cap)return -1;
    _u32 mode=rc<0?FS_MODE_OPEN_CREATE(cap,mirror?_FS_FILE_OPEN_FLAG_COMMIT:0):FS_MODE_OPEN_WRITE;
    rc=sl_FsOpen((const _u8*)name,mode,&token,&fd);return rc<0?rc:(int)fd;
}
int fs_read(int fd,uint32_t off,void *p,size_t n) { return sl_FsRead(fd,off,p,n); }
int fs_write(int fd,uint32_t off,const void *p,size_t n) { return sl_FsWrite(fd,off,(_u8*)p,n); }
int fs_close(int fd) { return sl_FsClose(fd,NULL,NULL,0); }
int fs_size(const char *name) { SlFsFileInfo_t i;return sl_FsGetInfo((const _u8*)name,0,&i)<0?-1:(int)i.FileLen; }
bool fs_save(const char *name,const void *p,size_t n) {
    int fd=fs_open_write(name,n,true);if(fd<0)return false;
    int wrote=fs_write(fd,0,p,n),closed=fs_close(fd);return wrote==(int)n && closed>=0;
}
bool boot_read(BootRecord *b) {
    int fd=fs_open_read(BOOT_PATH);if(fd<0)return false;
    int n=fs_read(fd,0,b,sizeof *b);fs_close(fd);return n==sizeof *b&&boot_record_valid(b);
}
bool boot_save(BootRecord *b) { boot_record_seal(b);return fs_save(BOOT_PATH,b,sizeof *b); }
const char *slot_path(unsigned s) { return s==0?"/chime/slot0.bin":"/chime/slot1.bin"; }
int socket_nonblocking(int fd) { SlSockNonblocking_t v={1};return sl_SetSockOpt(fd,SL_SOL_SOCKET,SL_SO_NONBLOCKING,&v,sizeof v); }
__attribute__((weak)) void platform_idle_hook(void) {}
int socket_send_all(int fd,const void *data,size_t n,uint32_t timeout) {
    const uint8_t *p=data;size_t sent=0;uint32_t start=millis();
    while(sent<n) {
        size_t chunk=n-sent;if(chunk>1024)chunk=1024;
        int rc=sl_Send(fd,p+sent,(int)chunk,0);
        if(rc>0)sent+=(size_t)rc;
        else if(rc!=SL_EAGAIN)return -1;
        if((uint32_t)(millis()-start)>timeout)return -1;
        network_pump();platform_idle_hook();
    }
    return (int)sent;
}
bool ip_parse(const char *s,uint32_t *ip) {
    uint32_t v=0;
    for(unsigned i=0;i<4;i++) {
        char part[4];unsigned n=0;
        while(*s && *s!='.') { if(n>=3)return false;part[n++]=*s++; }
        part[n]=0;uint32_t oct;
        if(!parse_uint(part,255,&oct))return false;v=(v<<8)|oct;
        if(i<3) { if(*s++!='.')return false; } else if(*s)return false;
    }
    *ip=v;return true;
}
