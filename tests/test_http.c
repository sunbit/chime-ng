#include "app.h"
#include "setup_secret.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
Config config;bool configured=false,ap_mode=true;char ap_name[32]="ChimeSetup-445566";
uint8_t device_mac[6]={0x02,1,2,3,4,5};volatile bool wifi_ip_ready=true,wifi_connected=true;
volatile uint32_t wifi_ip=0xc0a80401;
static int bind_error,listen_error,accept_error;static bool codec_ready=true;
static uint32_t ticks;static bool rebooting,playing,uploading;static unsigned play_count,save_count,ota_count;static size_t ota_received,ota_expected;
static char inbound[SOUND_MAX_BYTES+4096],outbound[40000];static size_t in_size,in_pos,out_size;static bool accepting;
uint32_t millis(void){return ticks++;}
bool app_busy(void){return rebooting||uploading||sound_busy();}
void app_reboot_later(void){rebooting=true;}
bool audio_playing(void){return playing;}
bool audio_ready(void){return codec_ready;}
const char *audio_error(void){return codec_ready?"none":"codec";}
static char failure_diagnostic[AUDIO_DIAGNOSTIC_SIZE];
const char *audio_diagnostic(void){return codec_ready?"none":failure_diagnostic;}
void log_line(const char *s){}
int sl_NetAppStop(uint32_t mask){assert(mask==SL_NET_APP_HTTP_SERVER_ID);return 0;}
bool audio_play(void){if(app_busy()||playing)return false;playing=true;play_count++;return true;}
bool app_save_config(Config *c){config_seal(c);if(!config_valid(c))return false;config=*c;configured=true;save_count++;return true;}
bool app_set_volume(unsigned v){if(v>100||playing)return false;config.volume=v;return true;}
const char *mqtt_status(void){return "offline";}
static bool led_on;
static unsigned led_commands;
void board_led_set(bool on){led_on=on;led_commands++;}
bool board_led_on(void){return led_on;}
static char led_diagnostic[LED_DIAGNOSTIC_SIZE],led_command[MQTT_LED_COMMAND_SIZE];
const char *board_led_diagnostic(void){return led_diagnostic;}
const char *mqtt_led_command(void){return led_command;}
bool ota_busy(void){return uploading;}
bool ota_confirmed(void){return true;}
bool ota_begin(uint32_t n){if(n<1088||n>IMAGE_MAX+64)return false;uploading=true;ota_received=0;ota_expected=n;return true;}
bool ota_write(const void *p,size_t n){ota_received+=n;return ota_received<=ota_expected;}
bool ota_finish(void){if(ota_received!=ota_expected)return false;uploading=false;ota_count++;return true;}
void ota_abort(void){uploading=false;}
static bool sound_uploading,reject_sound_finish,boot_confirmed=true;
static unsigned sound_count,sound_started,sound_aborts;
static size_t sound_received,sound_expected;
bool sound_busy(void){return sound_uploading;}
bool sound_begin(uint32_t n){if(!boot_confirmed)return false;sound_uploading=true;sound_expected=n;sound_received=0;sound_started++;return true;}
bool sound_write(const void *data,size_t n){sound_received+=n;return sound_received<=sound_expected;}
bool sound_finish(void){if(reject_sound_finish||sound_received!=sound_expected)return false;sound_uploading=false;sound_count++;return true;}
void sound_abort(void){sound_uploading=false;sound_aborts++;}

int fs_size(const char *p){return -1;}
int fs_open_write(const char *p,uint32_t n,bool m){return 3;}
int fs_write(int fd,uint32_t off,const void *p,size_t n){return n;}
int fs_close(int fd){return 0;}
bool ip_parse(const char *p,uint32_t *ip){*ip=1;return !strcmp(p,"192.168.1.1");}
int sl_Socket(int a,int b,int c){return 1;}
int sl_Close(int fd){return 0;}
int socket_nonblocking(int fd){return 0;}
int sl_Bind(int fd,const SlSockAddr_t *p,int n){return bind_error;}
int sl_Listen(int fd,int n){return listen_error;}
int sl_Accept(int fd,SlSockAddr_t *p,SlSocklen_t *n){if(accept_error)return accept_error;if(accepting){accepting=false;return 2;}return SL_EAGAIN;}
uint16_t sl_Htons(uint16_t n){return n;}
int sl_Recv(int fd,void *p,int n,int f){if(in_pos==in_size)return SL_EAGAIN;if(n>13)n=13;if((size_t)n>in_size-in_pos)n=in_size-in_pos;memcpy(p,inbound+in_pos,n);in_pos+=n;return n;}
int socket_send_all(int fd,const void *p,size_t n,uint32_t ms){assert(out_size+n+1<sizeof outbound);memcpy(outbound+out_size,p,n);out_size+=n;outbound[out_size]=0;return n;}
static void auth(char *out,const char *password){const char *a="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";char input[160];snprintf(input,sizeof input,"admin:%s",password);size_t n=strlen(input),k=0;for(size_t i=0;i<n;i+=3){unsigned v=(unsigned char)input[i]<<16;if(i+1<n)v|=(unsigned char)input[i+1]<<8;if(i+2<n)v|=(unsigned char)input[i+2];out[k++]=a[(v>>18)&63];out[k++]=a[(v>>12)&63];out[k++]=i+1<n?a[(v>>6)&63]:'=';out[k++]=i+2<n?a[v&63]:'=';}out[k]=0;}
static int send(const char *method,const char *path,const char *password,const char *type,const void *data,size_t size,bool custom){char token[240];auth(token,password);in_size=snprintf(inbound,sizeof inbound,"%s %s HTTP/1.1\r\nHost: chime\r\nAuthorization: Basic %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n%s\r\n",method,path,token,type,(unsigned)size,custom?"X-Chime-Request: 1\r\n":"");assert(in_size+size<sizeof inbound);if(size)memcpy(inbound+in_size,data,size);in_size+=size;in_pos=out_size=0;outbound[0]=0;accepting=true;for(unsigned i=0;i<50000&&!out_size;i++)http_poll();int code=0;assert(sscanf(outbound,"HTTP/1.1 %d",&code)==1);return code;}
int main(void){
 memset(failure_diagnostic,'x',sizeof failure_diagnostic-1);
 memset(led_diagnostic,'y',sizeof led_diagnostic-1);memset(led_command,'z',sizeof led_command-1);
 config.volume=25;config.mqtt_port=1883;
 bind_error=-98;assert(!http_start()&&!http_running()&&!strcmp(http_status(),"bind:-98"));
 bind_error=0;listen_error=-1;assert(!http_start()&&!strcmp(http_status(),"listen:-1"));
 listen_error=0;assert(http_start()&&http_running()&&!strcmp(http_status(),"listening:80"));
 accept_error=-9;http_poll();assert(!http_running()&&!strcmp(http_status(),"accept:-9"));
 accept_error=0;assert(http_start());codec_ready=false;
 assert(send("GET","/","wrong","text/plain",NULL,0,false)==200);
 assert(send("GET","/api/status",SETUP_PASSWORD,"text/plain",NULL,0,false)==200);
 assert(strstr(outbound,"\"ip\":\"192.168.4.1\"")&&strstr(outbound,"\"error\":\"codec\"")&&strstr(outbound,audio_diagnostic()));
 assert(strstr(outbound,led_diagnostic)&&strstr(outbound,led_command)&&strstr(outbound,"\"led\":false"));
 /* Direct LED test remains available without MQTT/audio, but requires the
  * same authentication, explicit request header and strict payload as MQTT. */
 assert(send("POST","/api/led","wrong","text/plain","true",4,true)==401);
 assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","true",4,false)==400);
 assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","true\0",5,true)==400);
 assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","true\n",5,true)==400);
 assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","TRUE",4,true)==400);
 assert(send("POST","/api/led",SETUP_PASSWORD,"application/json","true",4,true)==415);
 assert(!led_on&&!led_commands);
 assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","true",4,true)==200);assert(led_on&&led_commands==1);
 assert(send("GET","/api/status",SETUP_PASSWORD,"text/plain",NULL,0,false)==200);assert(strstr(outbound,"\"led\":true"));
 assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","false",5,true)==200);assert(!led_on&&led_commands==2);

 /* WAVs stream independently of the 8 KiB form buffer and require the same guards. */
 char clip[SOUND_MAX_BYTES]={0};
 assert(send("PUT","/api/sound","wrong","application/octet-stream",clip,sizeof clip,true)==401);
 assert(send("PUT","/api/sound",SETUP_PASSWORD,"application/octet-stream",clip,sizeof clip,false)==400);
 assert(send("PUT","/api/sound",SETUP_PASSWORD,"text/plain",clip,sizeof clip,true)==415);
 assert(send("POST","/api/sound",SETUP_PASSWORD,"text/plain",clip,46,true)==405);
 assert(send("PUT","/api/sound",SETUP_PASSWORD,"application/octet-stream",clip,45,true)==413);
 assert(sound_started==0);
 boot_confirmed=false;assert(send("PUT","/api/sound",SETUP_PASSWORD,"application/octet-stream",clip,sizeof clip,true)==409);boot_confirmed=true;
 assert(send("PUT","/api/sound",SETUP_PASSWORD,"application/octet-stream",clip,sizeof clip,true)==200);
 assert(sound_count==1&&sound_received==sizeof clip&&!sound_busy()&&!rebooting);
 reject_sound_finish=true;assert(send("PUT","/api/sound",SETUP_PASSWORD,"application/octet-stream",clip,sizeof clip,true)==422);
 assert(sound_count==1&&sound_aborts==1&&!sound_busy());reject_sound_finish=false;
 playing=true;assert(send("PUT","/api/sound",SETUP_PASSWORD,"application/octet-stream",clip,sizeof clip,true)==409);playing=false;
 uploading=true;assert(send("PUT","/api/sound",SETUP_PASSWORD,"application/octet-stream",clip,sizeof clip,true)==409);uploading=false;
 /* Time out a disconnected partial upload and release the global operation lock. */
 char token[240];auth(token,SETUP_PASSWORD);
 in_size=snprintf(inbound,sizeof inbound,"PUT /api/sound HTTP/1.1\r\nHost: chime\r\nAuthorization: Basic %s\r\nContent-Type: application/octet-stream\r\nContent-Length: 16384\r\nX-Chime-Request: 1\r\n\r\n",token);
 memcpy(inbound+in_size,clip,100);in_size+=100;in_pos=out_size=0;accepting=true;
 for(unsigned i=0;i<100;i++)http_poll();assert(sound_busy());ticks+=10001;http_poll();assert(!sound_busy()&&sound_aborts==2);
 assert(send("GET","/api/status",SETUP_PASSWORD,"text/plain",NULL,0,false)==200);
 char recovery_package[2048]={0};assert(send("PUT","/api/ota",SETUP_PASSWORD,"application/octet-stream",recovery_package,sizeof recovery_package,true)==200);assert(rebooting);rebooting=false;ota_count=0;
 codec_ready=true;
 assert(send("GET","/","wrong","text/plain",NULL,0,false)==200);assert(strstr(outbound,"<html lang=\"en\">"));
 assert(send("GET","/api/status","wrong","text/plain",NULL,0,false)==401);
 assert(send("GET","/api/status",SETUP_PASSWORD,"text/plain",NULL,0,false)==200);assert(strstr(outbound,"\"configured\":false"));assert(!strstr(outbound,"admin_hash"));assert(!strstr(outbound,SETUP_PASSWORD));
 assert(send("POST","/api/play",SETUP_PASSWORD,"text/plain","original",8,false)==400);assert(!play_count);
 assert(send("POST","/api/play",SETUP_PASSWORD,"text/plain","original\0x",10,true)==400);assert(!play_count);
 assert(send("POST","/api/play",SETUP_PASSWORD,"text/plain","original",8,true)==200);assert(play_count==1);
 assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","true",4,true)==200);assert(led_on&&playing&&play_count==1);
 assert(send("POST","/api/play",SETUP_PASSWORD,"text/plain","original",8,true)==409);
 assert(send("POST","/api/volume",SETUP_PASSWORD,"text/plain","76",2,true)==409);
 assert(send("PUT","/api/ota",SETUP_PASSWORD,"application/octet-stream",recovery_package,sizeof recovery_package,true)==409);
 assert(send("POST","/api/reboot","wrong","text/plain",NULL,0,true)==401);assert(!rebooting);
 assert(send("POST","/api/reboot",SETUP_PASSWORD,"text/plain",NULL,0,false)==400);assert(!rebooting);
 assert(send("POST","/api/reboot",SETUP_PASSWORD,"text/plain",NULL,0,true)==200);assert(rebooting);
 assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","false",5,true)==409);assert(led_on);
 playing=false;rebooting=false;
 uploading=true;assert(send("POST","/api/led",SETUP_PASSWORD,"text/plain","false",5,true)==409);assert(led_on);uploading=false;
 assert(send("POST","/api/volume",SETUP_PASSWORD,"text/plain","76",2,true)==200);assert(config.volume==76);
 assert(send("POST","/api/volume",SETUP_PASSWORD,"text/plain","101",3,true)==400);assert(config.volume==76);
 assert(send("PUT","/api/ota",SETUP_PASSWORD,"text/plain","x",1,true)==415);
 char package[2048]={0};assert(send("PUT","/api/ota",SETUP_PASSWORD,"application/octet-stream",package,sizeof package,true)==200);assert(ota_count==1&&ota_received==sizeof package&&rebooting);rebooting=false;
 const char *form="ssid=Test&broker=192.168.1.2&mqtt_user=&homie_id=test-chime&name=Chime&ntp_server=&mqtt_port=1883&volume=25&mqtt_tls=0&wifi_password=password123&mqtt_password=&admin_password=new-password-long";
 assert(send("POST","/api/config",SETUP_PASSWORD,"application/x-www-form-urlencoded",form,strlen(form),true)==200);assert(save_count==1&&configured&&rebooting);rebooting=false;
 assert(send("GET","/api/status",SETUP_PASSWORD,"text/plain",NULL,0,false)==401);
 assert(send("GET","/api/status","new-password-long","text/plain",NULL,0,false)==200);assert(strstr(outbound,"\"homie_id\":\"test-chime\""));assert(!strstr(outbound,"password123"));assert(!strstr(outbound,"new-password-long"));
 const char *update="ssid=Test&broker=192.168.1.2&mqtt_user=&homie_id=test-chime&name=Chime&ntp_server=&mqtt_port=1883&volume=25&mqtt_tls=0&wifi_password=&mqtt_password=&admin_password=";
 assert(send("POST","/api/config","new-password-long","application/x-www-form-urlencoded",update,strlen(update),true)==200);assert(!strcmp(config.wifi_password,"password123"));rebooting=false;
 assert(send("POST","/api/reboot","new-password-long","text/plain",NULL,0,true)==200);assert(rebooting);
 puts("HTTP: real C routes with fragmented transport, authentication, no secret disclosure, CSRF header, binary rejection, play/busy, volume, OTA upload, initial configuration, password preservation and reboot passed");
 puts("HTTP SOUND: authenticated streaming, format/size/method guards, binary payloads, upload failures, timeout cleanup and busy exclusion passed");
 puts("HTTP LED: authenticated on/off without MQTT/audio, live status, strict payload, simultaneous playback and OTA/reboot guards passed");
 return 0;
}
