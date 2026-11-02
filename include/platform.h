#ifndef CHIME_PLATFORM_H
#define CHIME_PLATFORM_H
#include "core.h"
#include "led.h"
#include "simplelink.h"
#include "inc/hw_types.h"
#include "inc/hw_memmap.h"
#include "inc/hw_ints.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/prcm.h"
#include "driverlib/pin.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/uart.h"
#include "driverlib/utils.h"
#include "driverlib/systick.h"
#include "driverlib/wdt.h"

extern volatile bool wifi_ip_ready, wifi_connected;
extern volatile uint32_t wifi_ip;
extern uint8_t device_mac[6];
void board_init(void);
uint32_t millis(void);
void delay_ms(uint32_t ms);
void log_line(const char *s);
void watchdog_feed(void);
void reboot(void) __attribute__((noreturn));
void network_pump(void);
int fs_open_read(const char *name);
int fs_open_write(const char *name,uint32_t capacity,bool mirror);
int fs_read(int fd,uint32_t offset,void *p,size_t n);
int fs_write(int fd,uint32_t offset,const void *p,size_t n);
int fs_close(int fd);
int fs_size(const char *name);
bool fs_save(const char *name,const void *p,size_t n);
bool boot_read(BootRecord *b);
bool boot_save(BootRecord *b);
const char *slot_path(unsigned slot);
int socket_nonblocking(int fd);
int socket_send_all(int fd,const void *p,size_t n,uint32_t timeout_ms);
bool ip_parse(const char *s,uint32_t *ip);
#endif
