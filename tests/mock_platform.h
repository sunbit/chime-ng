#ifndef CHIME_TEST_PLATFORM_H
#define CHIME_TEST_PLATFORM_H
#define CHIME_PLATFORM_H
#include "core.h"
typedef uint8_t _u8;typedef int8_t _i8;typedef uint32_t _u32;
typedef struct {uint16_t sin_family,sin_port;struct {uint32_t s_addr;} sin_addr;} SlSockAddrIn_t;
typedef SlSockAddrIn_t SlSockAddr_t;
typedef uint16_t SlSocklen_t;
#define SL_INADDR_ANY 0
#define SL_AF_INET 2
#define SL_SOCK_STREAM 1
#define SL_SEC_SOCKET 100
#define SL_IPPROTO_TCP 6
#define SL_SOL_SOCKET 1
#define SL_SO_SECMETHOD 25
#define SL_SO_SECURE_FILES_CA_FILE_NAME 32
#define SL_EAGAIN -11
#define SL_EALREADY -114
extern volatile bool wifi_ip_ready,wifi_connected;
extern volatile uint32_t wifi_ip;
#define SL_NET_APP_HTTP_SERVER_ID 1
int sl_NetAppStop(uint32_t applications);
void log_line(const char *message);
extern uint8_t device_mac[6];
uint32_t millis(void);
void watchdog_feed(void);
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
int sl_Socket(int domain,int type,int protocol);
int sl_Close(int fd);
int sl_Connect(int fd,const SlSockAddr_t *a,int length);
int sl_SetSockOpt(int fd,int level,int option,const void *value,int length);
int sl_Recv(int fd,void *p,int n,int flags);
int sl_Bind(int fd,const SlSockAddr_t *address,int length);
int sl_Listen(int fd,int backlog);
int sl_Accept(int fd,SlSockAddr_t *address,SlSocklen_t *length);
int sl_NetAppDnsGetHostByName(_i8 *name,int length,_u32 *ip,int family);
uint16_t sl_Htons(uint16_t n);
uint32_t sl_Htonl(uint32_t n);
#endif
