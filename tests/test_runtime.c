#include "app.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

Config config;bool configured=true,ap_mode=false;
volatile bool wifi_ip_ready=true,wifi_connected=true;
volatile uint32_t wifi_ip=0xc0000201;
static const char *web_state="listening:80";
const char *http_status(void){return web_state;}
uint8_t device_mac[6]={0x02,0x11,0x22,0x33,0x44,0x55};
static uint32_t ticks;
uint32_t millis(void){return ticks++;}
void watchdog_feed(void){}
void network_pump(void){}
static bool busy_sound,hardware_ready=true;
static char failure_diagnostic[AUDIO_DIAGNOSTIC_SIZE];
const char *audio_diagnostic(void){return hardware_ready?"none":failure_diagnostic;}
static unsigned plays;
static bool led_on;
static unsigned led_commands;
void board_led_set(bool on){led_on=on;led_commands++;}
bool board_led_on(void){return led_on;}
const char *board_led_diagnostic(void){return led_on?"gpio11 requested=1 latch=1":"gpio11 requested=0 latch=0";}
bool audio_playing(void){return busy_sound;}
bool audio_ready(void){return hardware_ready;}
const char *audio_error(void){return hardware_ready?"none":"codec";}
bool audio_play(void){if(busy_sound||!hardware_ready)return false;busy_sound=true;plays++;return true;}
bool app_busy(void){return ota_busy();}
bool app_time_valid(void){return true;}
bool app_set_volume(unsigned v){if(v>100||busy_sound)return false;config.volume=v;return true;}

/* Storage mock detects writes to the active slot and injects write/close errors. */
static uint8_t storage[2][IMAGE_MAX+64];static size_t lengths[2];
static BootRecord record;
static int fail_write,fail_close,fail_commit;static unsigned writes;
const char *slot_path(unsigned s){return s?"slot1":"slot0";}
int fs_open_read(const char *path){return !strcmp(path,"slot0")?0:!strcmp(path,"slot1")?1:-1;}
int fs_open_write(const char *path,uint32_t cap,bool mirror){int fd=fs_open_read(path);assert(fd>=0&&cap==IMAGE_MAX+64&&!mirror);assert(fd!=record.active);lengths[fd]=0;return fd;}
int fs_read(int fd,uint32_t off,void *p,size_t n){if(fd<0||fd>1||off+n>lengths[fd])return -1;memcpy(p,storage[fd]+off,n);return n;}
int fs_write(int fd,uint32_t off,const void *p,size_t n){assert(fd!=record.active);if(fail_write)return -1;assert(off+n<=sizeof storage[0]);memcpy(storage[fd]+off,p,n);lengths[fd]=off+n;writes++;return n;}
int fs_close(int fd){return fail_close?-1:0;}
int fs_size(const char *p){int fd=fs_open_read(p);return fd<0?-1:lengths[fd];}
bool boot_read(BootRecord *b){*b=record;return boot_record_valid(b);}
bool boot_save(BootRecord *b){if(fail_commit)return false;boot_record_seal(b);record=*b;return true;}
static uint8_t image[64+2048];
static void prepare_image(void){ImageHeader h={0};memcpy(h.magic,IMAGE_MAGIC,8);h.board=BOARD_ID;h.format=1;h.load_address=APP_BASE;h.size=2048;h.build=1;for(unsigned i=64;i<sizeof image;i++)image[i]=i;uint32_t v[2]={RAM_END,APP_BASE+1025};memcpy(image+64,v,8);sha256(image+64,2048,h.sha);memcpy(image,&h,64);}
static void storage_reset(void){ota_abort();memset(&record,0,sizeof record);record.active=0;record.pending=NO_SLOT;boot_record_seal(&record);memset(storage[0],0xa5,sizeof storage[0]);lengths[0]=sizeof storage[0];lengths[1]=0;fail_write=fail_close=fail_commit=0;busy_sound=false;assert(ota_confirm());writes=0;}
static void upload(unsigned chunk){assert(ota_begin(sizeof image));for(unsigned off=0;off<sizeof image;){unsigned n=sizeof image-off;if(n>chunk)n=chunk;assert(ota_write(image+off,n));off+=n;}}
static void test_ota(void){
    prepare_image();storage_reset();upload(7);assert(ota_finish());assert(record.active==0&&record.pending==1&&!record.tried);assert(!memcmp(storage[1],image,sizeof image));for(unsigned i=0;i<sizeof storage[0];i++)assert(storage[0][i]==0xa5);
    assert(boot_select(&record)==1);assert(record.tried);assert(boot_select(&record)==0);assert(record.pending==NO_SLOT); /* Crash -> rollback. */
    storage_reset();upload(1024);assert(ota_finish());assert(boot_select(&record)==1);assert(ota_confirm());assert(record.active==1&&record.pending==NO_SLOT); /* Healthy trial -> commit. */
    storage_reset();assert(ota_begin(sizeof image));assert(ota_write(image,64));assert(!ota_finish());ota_abort();assert(record.pending==NO_SLOT&&record.active==0);
    storage_reset();upload(256);storage[1][100]^=1;assert(!ota_finish());ota_abort();assert(record.pending==NO_SLOT);
    storage_reset();image[0]='X';assert(ota_begin(sizeof image));assert(!ota_write(image,64));ota_abort();assert(writes==0);prepare_image();
    storage_reset();assert(!ota_begin(1));assert(!ota_begin(IMAGE_MAX+65));assert(!ota_begin(64+1023));
    storage_reset();assert(ota_begin(sizeof image));assert(!ota_write(image,sizeof image+1));ota_abort();
    storage_reset();fail_write=1;assert(ota_begin(sizeof image));assert(!ota_write(image,64));ota_abort();assert(record.pending==NO_SLOT);
    storage_reset();upload(1024);fail_close=1;assert(!ota_finish());ota_abort();assert(record.pending==NO_SLOT);
    storage_reset();upload(1024);fail_commit=1;assert(!ota_finish());ota_abort();assert(record.pending==NO_SLOT);
    storage_reset();record.pending=1;boot_record_seal(&record);assert(!ota_begin(sizeof image));
    storage_reset();busy_sound=true;assert(!ota_begin(sizeof image));busy_sound=false;
    /* Header hash can be valid while vectors are unsafe: reject before activation. */
    storage_reset();uint32_t invalid=0x20000000;memcpy(image+64,&invalid,4);sha256(image+64,2048,((ImageHeader*)image)->sha);upload(73);assert(!ota_finish());ota_abort();prepare_image();
    puts("OTA: valid upload, fragmented header, active-slot preservation, rollback, confirmation, truncation, corruption, board mismatch, bounds and storage failures passed");
}

/* In-memory MQTT broker: execute the actual client, parse every outgoing packet,
 * and complete the real QoS2 exchange. No network or device access. */
static uint8_t incoming[16384],wire[16384];static size_t incoming_n,wire_n;
static bool reject_publish,drop_ack,reject_led_sub;
static unsigned pubs,subs,connects,disconnects;
static char topics[128][128],values[128][1500];static uint8_t pubflags[128];
static void enqueue(const void *p,size_t n){assert(incoming_n+n<sizeof incoming);memcpy(incoming+incoming_n,p,n);incoming_n+=n;}
static void broker(const uint8_t *p,size_t n,size_t h){
    unsigned type=p[0]>>4;const uint8_t *b=p+h;
    if(type==1){assert(b[6]==5);assert((b[7]&0x3e)==0x36);assert(b[8]==0&&b[9]==30);assert(memmem(b,n-h,"homie/5/test-chime/$state",24));assert(memmem(b,n-h,"lost",4));connects++;uint8_t ack[]={0x20,3,0,0,0};enqueue(ack,sizeof ack);}
    else if(type==3){MqttPublish m;assert(mqtt_parse_publish(p,n,&m));assert(pubs<128);memcpy(topics[pubs],m.topic,m.topic_len);topics[pubs][m.topic_len]=0;memcpy(values[pubs],m.payload,m.payload_len);values[pubs][m.payload_len]=0;pubflags[pubs++]=p[0];if(m.qos){assert(m.qos==2&&m.retained);if(!drop_ack){uint8_t ack[]={0x50,3,m.packet_id>>8,m.packet_id,reject_publish?0x80:0};enqueue(ack,sizeof ack);}}else assert(!m.retained);}
    else if(type==6){assert(p[0]==0x62);if(!drop_ack){uint8_t ack[]={0x70,2,b[0],b[1]};enqueue(ack,sizeof ack);}}
    else if(type==8){assert(p[0]==0x82);size_t pos=3;const char *paths[]={"homie/5/test-chime/chime/play/set","homie/5/test-chime/chime/volume/set","homie/5/test-chime/chime/led/set"};for(unsigned i=0;i<3;i++){size_t len=b[pos]*256+b[pos+1];assert(len==strlen(paths[i])&&!memcmp(b+pos+2,paths[i],len));pos+=2+len;assert(b[pos++]==0x28);}assert(pos==n-h);subs++;uint8_t ack[]={0x90,6,b[0],b[1],0,0,0,reject_led_sub?0x80:0};enqueue(ack,sizeof ack);}
    else if(type==12){uint8_t ack[]={0xd0,0};enqueue(ack,2);}
    else if(type==14)disconnects++;
    else assert(!"unexpected outgoing packet");
}
int socket_send_all(int fd,const void *p,size_t n,uint32_t timeout){assert(wire_n+n<sizeof wire);if(n)memcpy(wire+wire_n,p,n);wire_n+=n;size_t h;int len;while((len=mqtt_frame_size(wire,wire_n,sizeof wire,&h))>0){broker(wire,len,h);wire_n-=len;memmove(wire,wire+len,wire_n);}assert(len>=0);return n;}
int sl_Recv(int fd,void *p,int n,int flags){if(!incoming_n)return SL_EAGAIN;/* Deliberately split broker responses across reads. */if(n>3)n=3;if((size_t)n>incoming_n)n=incoming_n;memcpy(p,incoming,n);incoming_n-=n;memmove(incoming,incoming+n,incoming_n);return n;}
int socket_nonblocking(int fd){return 0;}
int sl_Socket(int a,int b,int c){return 7;}
int sl_Close(int fd){incoming_n=wire_n=0;return 0;}
int sl_Connect(int fd,const SlSockAddr_t *a,int n){return 0;}
int sl_SetSockOpt(int fd,int l,int o,const void *v,int n){return 0;}
int sl_NetAppDnsGetHostByName(_i8 *n,int len,_u32 *ip,int f){*ip=0x7f000001;return 0;}
uint16_t sl_Htons(uint16_t n){return (n>>8)|(n<<8);}
uint32_t sl_Htonl(uint32_t n){return __builtin_bswap32(n);}
bool ip_parse(const char *s,uint32_t *ip){*ip=0x7f000001;return true;}
static void polls(unsigned n){while(n--)mqtt_poll();}
static void command_bytes(const char *topic,const void *value,size_t size,unsigned flags,bool properties){
    uint8_t p[1024],var[4];unsigned qos=(flags>>1)&3;
    size_t n=2+strlen(topic)+1+size+(qos?2:0)+(properties?2:0),v=mqtt_varint(var,n);
    assert(n+v+1<=sizeof p);p[0]=0x30|flags;memcpy(p+1,var,v);size_t i=1+v;
    p[i++]=strlen(topic)>>8;p[i++]=strlen(topic);memcpy(p+i,topic,strlen(topic));i+=strlen(topic);
    if(qos){p[i++]=0;p[i++]=77;}
    p[i++]=properties?2:0;if(properties){p[i++]=1;p[i++]=1;} /* Payload Format Indicator. */
    if(size)memcpy(p+i,value,size);i+=size;enqueue(p,i);polls(200);
}
static void command(const char *topic,const char *value,unsigned flags){command_bytes(topic,value,strlen(value),flags,false);}

static void test_mqtt(void){
    memset(failure_diagnostic,'x',sizeof failure_diagnostic-1);
    storage_reset();strcpy(config.homie_id,"test-chime");strcpy(config.name,"Test chime");strcpy(config.broker,"127.0.0.1");config.mqtt_port=1883;config.volume=25;
    polls(300);assert(mqtt_ready()&&connects==1&&subs==1&&pubs==14);
    assert(!strcmp(topics[0],"homie/5/test-chime/$state")&&!strcmp(values[0],"init"));assert(strstr(topics[1],"$description"));assert(strstr(values[1],"\"homie\":\"5.0\""));assert(!strcmp(values[13],"ready"));
    assert(!strcmp(topics[10],"homie/5/test-chime/chime/led")&&!strcmp(values[10],"false"));
    assert(!strcmp(topics[11],"homie/5/test-chime/chime/led-diagnostic"));
    assert(!strcmp(topics[12],"homie/5/test-chime/chime/led-command")&&!strcmp(values[12],"seq=0 result=none"));
    assert(!strcmp(topics[7],"homie/5/test-chime/system/ip")&&!strcmp(values[7],"192.0.2.1"));
    assert(!strcmp(values[8],"listening:80")&&!strcmp(values[9],"none"));
    command("homie/5/test-chime/chime/play/set","original",1);assert(plays==0);
    command("homie/5/test-chime/chime/play/set","original",8);assert(plays==0);
    command("homie/5/test-chime/chime/play/set","other",0);assert(plays==0);
    command("homie/5/test-chime/chime/play/set","original",0);assert(plays==1&&busy_sound);assert(!strcmp(values[14],"original")&&pubflags[14]==0x30);assert(!strcmp(values[15],"true"));
    command("homie/5/test-chime/chime/play/set","original",0);assert(plays==1);
    /* LED commands work during playback, without restarting or stopping audio. */
    command("homie/5/test-chime/chime/led/set","true",0);assert(led_on&&led_commands==1&&busy_sound&&plays==1);
    assert(!strcmp(mqtt_led_command(),"seq=1 result=applied-true qos=0 retain=0 dup=0 len=4 hex=74727565"));
    assert(!strcmp(topics[pubs-1],"homie/5/test-chime/chime/led-command")&&!strcmp(values[pubs-1],mqtt_led_command())&&pubflags[pubs-1]==0x35);
    assert(!strcmp(topics[pubs-3],"homie/5/test-chime/chime/led")&&!strcmp(values[pubs-3],"true")&&pubflags[pubs-3]==0x35);
    command("homie/5/test-chime/chime/led/set","false",1);
    assert(strstr(mqtt_led_command(),"result=rejected-retained qos=0 retain=1 dup=0"));
    command("homie/5/test-chime/chime/led/set","false",8);
    assert(strstr(mqtt_led_command(),"result=rejected-duplicate qos=0 retain=0 dup=1"));
    command("homie/5/test-chime/chime/led/set","TRUE",0);
    command("homie/5/test-chime/chime/led/set","1",0);
    command("homie/5/test-chime/chime/led/set","true\n",0);
    assert(strstr(mqtt_led_command(),"result=rejected-payload")&&strstr(mqtt_led_command(),"len=5 hex=747275650a"));
    command("homie/5/test-chime/chime/led/set","",0);
    assert(strstr(mqtt_led_command(),"len=0 hex=-"));
    command_bytes("homie/5/test-chime/chime/led/set","true\0",5,0,false);
    assert(strstr(mqtt_led_command(),"result=rejected-payload")&&strstr(mqtt_led_command(),"len=5 hex=7472756500"));
    char oversized[300];memset(oversized,'x',sizeof oversized);
    command_bytes("homie/5/test-chime/chime/led/set",oversized,sizeof oversized,0,false);
    assert(strstr(mqtt_led_command(),"len=300 hex=7878787878787878")&&strlen(mqtt_led_command())<MQTT_LED_COMMAND_SIZE);
    assert(led_on&&led_commands==1);
    command_bytes("homie/5/test-chime/chime/led/set","false",5,0,true);assert(!led_on&&led_commands==2&&busy_sound);
    assert(strstr(mqtt_led_command(),"result=applied-false"));
    busy_sound=false;polls(100);assert(!strcmp(values[pubs-1],"false"));
    command("homie/5/test-chime/chime/volume/set","73",0);assert(config.volume==73&&!strcmp(values[pubs-1],"73"));
    command("homie/5/test-chime/chime/volume/set","101",0);assert(config.volume==73);
    command("homie/5/test-chime/chime/volume/set","12",1);assert(config.volume==73);
    assert(ota_begin(sizeof image));command("homie/5/test-chime/chime/led/set","true",0);assert(!led_on&&led_commands==2);assert(strstr(mqtt_led_command(),"result=rejected-busy"));ota_abort();
    command("homie/5/test-chime/chime/led/set","true",0);assert(led_on&&led_commands==3);
    char saved_led_command[MQTT_LED_COMMAND_SIZE];strcpy(saved_led_command,mqtt_led_command());
    wifi_ip_ready=false;polls(1);assert(!mqtt_ready());wifi_ip_ready=true;ticks+=6000;polls(300);assert(mqtt_ready()&&connects==2&&subs==2&&plays==1);
    assert(led_on&&led_commands==3); /* Reconnect reports state without replaying the command. */
    assert(!strcmp(saved_led_command,mqtt_led_command()));
    hardware_ready=false;polls(100);assert(!mqtt_ready());
    assert(!strcmp(topics[pubs-1],"homie/5/test-chime/chime/diagnostic")&&!strcmp(values[pubs-1],audio_diagnostic()));
    wifi_ip=0xc000022a;polls(100);assert(!strcmp(topics[pubs-1],"homie/5/test-chime/system/ip")&&!strcmp(values[pubs-1],"192.0.2.42"));
    web_state="bind:-98";polls(100);assert(!strcmp(topics[pubs-1],"homie/5/test-chime/system/web")&&!strcmp(values[pubs-1],"bind:-98"));
    ticks+=31000;polls(100);assert(!strcmp(topics[pubs-1],"homie/5/test-chime/system/uptime"));
    web_state="listening:80";polls(100);command("homie/5/test-chime/chime/play/set","original",0);assert(plays==1);
    command("homie/5/test-chime/chime/led/set","false",0);assert(led_on&&led_commands==3);
    assert(strstr(mqtt_led_command(),"result=rejected-not-ready"));
    hardware_ready=true;polls(100);assert(mqtt_ready());
    command("homie/5/test-chime/chime/led/set","false",2);assert(led_on&&led_commands==3&&!mqtt_ready());
    assert(strstr(mqtt_led_command(),"result=rejected-qos qos=1"));
    ticks+=6000;polls(300);assert(mqtt_ready());
    assert(strstr(mqtt_led_command(),"result=rejected-qos qos=1"));
    mqtt_shutdown();assert(disconnects==1&&!strcmp(values[pubs-1],"disconnected"));
    ticks+=6000;reject_led_sub=true;polls(300);assert(!mqtt_ready()&&!strcmp(mqtt_status(),"subscribe-rejected"));reject_led_sub=false;
    ticks+=6000;reject_publish=true;polls(100);assert(!mqtt_ready());reject_publish=false;
    ticks+=6000;drop_ack=true;polls(50);ticks+=6000;polls(1);assert(!mqtt_ready());drop_ack=false;
    puts("MQTT: fragmented frames, LWT, init-description-subscribe-ready, QoS2 handshake, retained/DUP rejection, play, busy, volume, reconnect, unavailable audio, graceful shutdown and ACK failure passed");
    puts("MQTT LED: counted receipts, rejection reasons, bounded payload hex, MQTT5 properties and persistent diagnostics across reconnect passed");
}
int main(void){test_ota();test_mqtt();return 0;}
