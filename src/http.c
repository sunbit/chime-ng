#include "app.h"
#include "setup_secret.h"
#include "web_asset.h"
#include "messages.h"
#include <stdio.h>
#include <string.h>

static int listener=-1,client=-1;
static uint32_t touched,total;
static char headers[2048],body[8193];
static size_t header_size;
static HttpRequest request;
static bool parsed,is_ota,is_sound;
static char web_status[48]="stopped";
const char *http_status(void) { return web_status; }
bool http_running(void) { return listener>=0; }
static void close_client(void) {
    if(client>=0)sl_Close(client);client=-1;
    if(is_ota&&ota_busy())ota_abort();is_ota=false;parsed=false;header_size=total=0;
    if(is_sound&&sound_busy())sound_abort();is_sound=false;
    memset(body,0,sizeof body);memset(headers,0,sizeof headers);
}
static void response(int code,const char *type,const char *content) {
    char head[512];size_t length=strlen(content);
    int n=snprintf(head,sizeof head,"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'\r\n\r\n",
        code,code==200?"OK":"Error",type,(unsigned)length);
    if(n>0&&(size_t)n<sizeof head&&socket_send_all(client,head,n,200)>=0)socket_send_all(client,content,length,1500);
    close_client();
}
static void message(int code,const char *text) { response(code,"text/plain; charset=utf-8",text); }
static bool authenticated(void) {
    char password[129];uint8_t digest[32],expected[32];
    if(!basic_password(request.authorization,password,sizeof password))return false;
    sha256(password,strlen(password),digest);memset(password,0,sizeof password);
    if(configured)memcpy(expected,config.admin_hash,32);else sha256(SETUP_PASSWORD,strlen(SETUP_PASSWORD),expected);
    return constant_equal(expected,digest,32);
}
static void status_response(void) {
    char ssid[68],broker[260],name[200],user[132],out[2560],ip[16];
    ip_format(wifi_ip,ip);
    json_escape(ssid,sizeof ssid,config.ssid);json_escape(broker,sizeof broker,config.broker);
    json_escape(name,sizeof name,config.name);json_escape(user,sizeof user,config.mqtt_user);
    snprintf(out,sizeof out,"{\"version\":\"%s\",\"configured\":%s,\"ap\":%s,\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\","
        "\"mqtt\":\"%s\",\"playing\":%s,\"audio_ready\":%s,\"error\":\"%s\",\"confirmed\":%s,"
        "\"ip\":\"%s\",\"web\":\"%s\",\"diagnostic\":\"%s\","
        "\"led\":%s,\"led_diagnostic\":\"%s\",\"led_command\":\"%s\","
        "\"ssid\":\"%s\",\"broker\":\"%s\",\"mqtt_port\":%u,\"mqtt_user\":\"%s\",\"mqtt_tls\":%u,"
        "\"ntp_server\":\"%s\",\"homie_id\":\"%s\",\"name\":\"%s\",\"volume\":%u,\"ca\":%s}",
        FW_VERSION,configured?"true":"false",ap_mode?"true":"false",device_mac[0],device_mac[1],device_mac[2],device_mac[3],device_mac[4],device_mac[5],
        mqtt_status(),audio_playing()?"true":"false",audio_ready()?"true":"false",audio_error(),ota_confirmed()?"true":"false",
        ip,web_status,audio_diagnostic(),
        board_led_on()?"true":"false",board_led_diagnostic(),mqtt_led_command(),
        ssid,broker,config.mqtt_port,user,config.mqtt_tls,config.ntp_server,config.homie_id,name,config.volume,fs_size(CA_PATH)>0?"true":"false");
    response(200,"application/json",out);
}
static bool config_form(Config *c) {
    *c=config;
    if(!form_get(body,"ssid",c->ssid,sizeof c->ssid)||!form_get(body,"broker",c->broker,sizeof c->broker)||
       !form_get(body,"mqtt_user",c->mqtt_user,sizeof c->mqtt_user)||!form_get(body,"homie_id",c->homie_id,sizeof c->homie_id)||
       !form_get(body,"name",c->name,sizeof c->name)||!form_get(body,"ntp_server",c->ntp_server,sizeof c->ntp_server))return false;
    char value[129];uint32_t v;
    if(!form_get(body,"mqtt_port",value,sizeof value)||!parse_uint(value,65535,&v)||!v)return false;c->mqtt_port=v;
    if(!form_get(body,"volume",value,sizeof value)||!parse_uint(value,100,&v))return false;c->volume=v;
    if(!form_get(body,"mqtt_tls",value,sizeof value)||!parse_uint(value,1,&v))return false;c->mqtt_tls=v;
    if(c->mqtt_tls&&!ip_parse(c->ntp_server,&v))return false;
    if(!form_get(body,"wifi_password",value,sizeof value))return false;
    if(value[0]||!configured){if(strlen(value)>=sizeof c->wifi_password)return false;strcpy(c->wifi_password,value);}
    /* Explicit switches allow removing passwords; an empty input preserves them. */
    char flag[2];
    if(form_get(body,"wifi_open",flag,sizeof flag)&&!strcmp(flag,"1"))c->wifi_password[0]=0;
    if(!form_get(body,"mqtt_password",value,sizeof value))return false;
    if(value[0]||!configured)strcpy(c->mqtt_password,value);
    if(form_get(body,"mqtt_clear",flag,sizeof flag)&&!strcmp(flag,"1"))c->mqtt_password[0]=0;
    if(!form_get(body,"admin_password",value,sizeof value))return false;
    if(value[0]||!configured){if(strlen(value)<12)return false;sha256(value,strlen(value),c->admin_hash);}
    memset(value,0,sizeof value);config_seal(c);return config_valid(c);
}
static void body_done(void) {
    if(is_sound) {
        if(sound_finish())message(200,MSG_SOUND_SAVED);
        else {sound_abort();message(422,MSG_SOUND_REJECTED);}
        return;
    }
    if(is_ota) {
        if(ota_finish()){app_reboot_later();message(200,MSG_FIRMWARE_VERIFIED);}
        else {ota_abort();message(422,MSG_FIRMWARE_REJECTED);}
        return;
    }
    if(strcmp(request.path,"/api/ca")&&memchr(body,0,total)){message(400,MSG_NULL_TEXT);return;}
    body[total]=0;
    if(!strcmp(request.path,"/api/config")) {
        Config c;
        if(!config_form(&c)){message(400,MSG_INVALID_CONFIG);return;}
        if(!app_save_config(&c)){message(500,MSG_CONFIG_SAVE_FAILED);return;}
        app_reboot_later();message(200,MSG_CONFIG_SAVED);
    } else if(!strcmp(request.path,"/api/play")) {
        if(strcmp(body,"original")){message(400,MSG_UNKNOWN_SOUND);return;}
        if(audio_play())message(200,MSG_PLAY_STARTED);else message(409,MSG_PLAY_UNAVAILABLE);
    } else if(!strcmp(request.path,"/api/led")) {
        if(!strcmp(body,"true"))board_led_set(true);
        else if(!strcmp(body,"false"))board_led_set(false);
        else {message(400,MSG_INVALID_BOOLEAN);return;}
        message(200,MSG_LED_APPLIED);
    } else if(!strcmp(request.path,"/api/volume")) {
        uint32_t v;if(!parse_uint(body,100,&v)){message(400,MSG_INVALID_VOLUME);return;}
        if(app_set_volume(v))message(200,MSG_VOLUME_SAVED);else message(409,MSG_PLAY_BUSY);
    } else if(!strcmp(request.path,"/api/ca")) {
        /* Root certificate, DER encoding. TLS itself validates the certificate. */
        if(total<32||(uint8_t)body[0]!=0x30){message(400,MSG_INVALID_CA);return;}
        int fd=fs_open_write(CA_PATH,8192,true);
        if(fd<0){message(500,MSG_CA_OPEN_FAILED);return;}
        int n=fs_write(fd,0,body,total),rc=fs_close(fd);
        message(n==(int)total&&rc>=0?200:500,n==(int)total&&rc>=0?MSG_CA_SAVED:MSG_CA_SAVE_FAILED);
    } else if(!strcmp(request.path,"/api/reboot")) {app_reboot_later();message(200,MSG_RESTARTING);}
    else message(404,MSG_UNKNOWN_ROUTE);
}
static bool prepare_request(void) {
    if(!http_parse_headers(headers,&request)){message(400,MSG_INVALID_REQUEST);return false;}
    if(!strcmp(request.method,"GET")&&!strcmp(request.path,"/")&&!request.length) {
        response(200,"text/html; charset=utf-8",web_page);return false;
    }
    if(!authenticated()){message(401,MSG_AUTH_FAILED);return false;}
    if(!strcmp(request.method,"GET")&&!strcmp(request.path,"/api/status")&&!request.length){status_response();return false;}
    bool put=!strcmp(request.method,"PUT"),post=!strcmp(request.method,"POST");
    if((!put&&!post)||!request.custom_header||!request.length_present){message(400,MSG_REQUEST_NOT_ALLOWED);return false;}
    bool reboot_request=post&&!strcmp(request.path,"/api/reboot");
    bool led_request=post&&!strcmp(request.path,"/api/led");
    if(app_busy()||(audio_playing()&&!reboot_request&&!led_request)){message(409,MSG_OPERATION_BUSY);return false;}
    is_ota=put&&!strcmp(request.path,"/api/ota");
    is_sound=put&&!strcmp(request.path,"/api/sound");
    bool cert=put&&!strcmp(request.path,"/api/ca");
    if((put&&!is_ota&&!cert&&!is_sound)||(post&&(!strcmp(request.path,"/api/ota")||!strcmp(request.path,"/api/ca")||!strcmp(request.path,"/api/sound")))){message(405,MSG_WRONG_METHOD);return false;}
    const char *type=(put?"application/octet-stream":!strcmp(request.path,"/api/config")?"application/x-www-form-urlencoded":"text/plain");
    if(strcmp(request.content_type,type)){message(415,MSG_WRONG_FORMAT);return false;}
    if(is_sound) {
        if(request.length<46||request.length>SOUND_MAX_BYTES){message(413,MSG_SOUND_SIZE);return false;}
        if(!sound_begin(request.length)){message(409,MSG_SOUND_UNAVAILABLE);return false;}
    } else if(is_ota) {
        if(!ota_begin(request.length)){message(409,MSG_OTA_NOT_READY);return false;}
    } else if(request.length>(cert?8192:2048)){message(413,MSG_FILE_TOO_LARGE);return false;}
    parsed=true;
    if(!request.length){body_done();return false;}
    return true;
}
void http_stop(void) {
    close_client();if(listener>=0)sl_Close(listener);listener=-1;
    strcpy(web_status,"stopped");
}
static bool start_error(const char *stage,int rc) {
    http_stop();snprintf(web_status,sizeof web_status,"%s:%d",stage,rc);
    log_line(web_status);return false;
}
bool http_start(void) {
    http_stop();
    /* Stop independently, so an unavailable mDNS service cannot obscure HTTP
     * diagnostics. Retry on every listener start after an NWP role change. */
    int rc=sl_NetAppStop(SL_NET_APP_HTTP_SERVER_ID);
    if(rc<0){char line[48];snprintf(line,sizeof line,"TI HTTP stop:%d",rc);log_line(line);}
    listener=sl_Socket(SL_AF_INET,SL_SOCK_STREAM,SL_IPPROTO_TCP);
    if(listener<0)return start_error("socket",listener);
    SlSockAddrIn_t a={0};a.sin_family=SL_AF_INET;a.sin_port=sl_Htons(80);a.sin_addr.s_addr=SL_INADDR_ANY;
    rc=sl_Bind(listener,(SlSockAddr_t*)&a,sizeof a);if(rc<0)return start_error("bind",rc);
    rc=sl_Listen(listener,1);if(rc<0)return start_error("listen",rc);
    rc=socket_nonblocking(listener);if(rc<0)return start_error("nonblocking",rc);
    strcpy(web_status,"listening:80");log_line("HTTP listening on port 80");
    return true;
}
void http_poll(void) {
    if(listener<0)return;
    if(client<0) {
        SlSockAddrIn_t a;SlSocklen_t len=sizeof a;
        client=sl_Accept(listener,(SlSockAddr_t*)&a,&len);
        if(client<0){if(client!=SL_EAGAIN)start_error("accept",client);return;}
        if(socket_nonblocking(client)<0){close_client();return;}touched=millis();
    }
    if((uint32_t)(millis()-touched)>10000){close_client();return;}
    uint8_t chunk[1024];int n=sl_Recv(client,chunk,sizeof chunk,0);
    if(n==0||(n<0&&n!=SL_EAGAIN)){close_client();return;}
    if(n<0)return;touched=millis();
    size_t i=0;
    while(i<(size_t)n&&!parsed) {
        unsigned c=chunk[i++];
        if(!c||header_size+1>=sizeof headers){message(431,MSG_INVALID_HEADER);return;}
        headers[header_size++]=c;headers[header_size]=0;
        if(header_size>=4&&!memcmp(headers+header_size-4,"\r\n\r\n",4))if(!prepare_request())return;
    }
    if(parsed&&i<(size_t)n) {
        size_t amount=n-i;if(amount>request.length-total){message(400,MSG_WRONG_LENGTH);return;}
        if(is_sound){if(!sound_write(chunk+i,amount)){sound_abort();message(422,MSG_SOUND_REJECTED);return;}}
        else if(is_ota){if(!ota_write(chunk+i,amount)){ota_abort();message(422,MSG_UPLOAD_REJECTED);return;}}
        else memcpy(body+total,chunk+i,amount);
        total+=amount;if(total==request.length)body_done();
    }
}
