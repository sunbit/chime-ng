#include "app.h"
#include <stdio.h>
#include <string.h>

/* MQTT 5.0, clean sessions, QoS 2 states, QoS 0 commands/events.
 * One outbound QoS transaction at a time. Disconnect/reconnect on a deadline:
 * never resend a non-idempotent play event after an uncertain connection. */
enum { DOWN,CONNECTING,CONNACK,ONLINE };
static int sock=-1,connection;
static uint8_t rx[2048],tx[2048];
static size_t used;
static uint32_t deadline,retry_at,last_io,ping_at,uptime_at;
static uint16_t sequence,pending;
static uint8_t ack_type;
static unsigned phase;
static bool available,sub_wait,closing;
static int last_playing=-1,last_volume=-1,last_available=-1,last_led=-1;
static char last_led_diagnostic[LED_DIAGNOSTIC_SIZE];
static uint32_t led_command_count;
static char led_command[MQTT_LED_COMMAND_SIZE]="seq=0 result=none";
static char last_led_command[MQTT_LED_COMMAND_SIZE];
static char last_error[16],last_web[48],last_diagnostic[AUDIO_DIAGNOSTIC_SIZE],root[64];
static uint32_t last_ip;
static const char *const command_paths[]={"chime/play/set","chime/volume/set","chime/led/set"};
enum { INITIAL_PHASES=15, COMMAND_COUNT=sizeof command_paths/sizeof command_paths[0] };
static const char *status="offline";
static SlSockAddrIn_t address;

bool mqtt_ready(void) { return available&&!closing&&audio_ready()&&ota_confirmed(); }
const char *mqtt_status(void) { return status; }
const char *mqtt_led_command(void) { return led_command; }
static void down(const char *reason) {
    if(sock>=0)sl_Close(sock);
    sock=-1;connection=DOWN;available=false;pending=0;ack_type=0;used=0;sub_wait=false;
    retry_at=millis()+5000;status=reason;
}
static uint16_t next_id(void) { if(!++sequence)++sequence;return sequence; }
static void put16(uint8_t *p,unsigned n) { p[0]=n>>8;p[1]=n; }
static size_t put_string(uint8_t *p,const char *s) { size_t n=strlen(s);put16(p,n);memcpy(p+2,s,n);return n+2; }
static bool send_packet(uint8_t first,const uint8_t *body,size_t n) {
    uint8_t header[5];header[0]=first;size_t h=mqtt_varint(header+1,n)+1;
    if(socket_send_all(sock,header,h,100)<0||socket_send_all(sock,body,n,100)<0){down("send-error");return false;}
    last_io=millis();return true;
}
static bool publish(const char *suffix,const char *value,bool event) {
    char topic[112];snprintf(topic,sizeof topic,"%s/%s",root,suffix);
    size_t n=put_string(tx,topic);
    uint16_t id=0;if(!event){id=next_id();put16(tx+n,id);n+=2;}
    tx[n++]=0;size_t len=strlen(value);if(n+len>sizeof tx)return false;
    memcpy(tx+n,value,len);n+=len;
    if(!send_packet(event?0x30:0x35,tx,n))return false;
    if(!event){pending=id;ack_type=5;deadline=millis()+5000;}
    return true;
}
static bool subscribe_commands(void) {
    uint16_t id=next_id();size_t n=0;put16(tx,id);n=2;tx[n++]=0;
    for(unsigned i=0;i<COMMAND_COUNT;i++) {
        char topic[112];snprintf(topic,sizeof topic,"%s/%s",root,command_paths[i]);n+=put_string(tx+n,topic);
        /* QoS 0, Retain As Published=1, Retain Handling=2 (no stored commands). */
        tx[n++]=0x28;
    }
    if(!send_packet(0x82,tx,n))return false;
    pending=id;ack_type=9;deadline=millis()+5000;sub_wait=true;return true;
}
static bool connect_packet(void) {
    size_t n=put_string(tx,"MQTT");tx[n++]=5;
    /* Clean Start + Will retained QoS2 + optional username/password. */
    tx[n++]=0x36|(config.mqtt_user[0]?0x80:0)|(config.mqtt_password[0]?0x40:0);
    put16(tx+n,30);n+=2;
    /* Maximum Packet Size = RX buffer size, no topic aliases requested. */
    tx[n++]=5;tx[n++]=0x27;tx[n++]=0;tx[n++]=0;tx[n++]=8;tx[n++]=0;
    char client[40];snprintf(client,sizeof client,"chime-%02x%02x%02x%02x%02x%02x",
        device_mac[0],device_mac[1],device_mac[2],device_mac[3],device_mac[4],device_mac[5]);
    n+=put_string(tx+n,client);tx[n++]=0; /* Will property length. */
    char topic[96];snprintf(topic,sizeof topic,"%s/$state",root);
    n+=put_string(tx+n,topic);n+=put_string(tx+n,"lost");
    if(config.mqtt_user[0])n+=put_string(tx+n,config.mqtt_user);
    if(config.mqtt_password[0])n+=put_string(tx+n,config.mqtt_password);
    if(!send_packet(0x10,tx,n))return false;
    connection=CONNACK;deadline=millis()+5000;status="connecting";return true;
}
static void start_connect(void) {
    if(config.mqtt_tls&&(!app_time_valid()||fs_size(CA_PATH)<=0)){status="tls-needs-time-or-ca";retry_at=millis()+5000;return;}
    uint32_t ip;
    if(!ip_parse(config.broker,&ip)) {
        _u32 resolved;
        if(sl_NetAppDnsGetHostByName((_i8*)config.broker,strlen(config.broker),&resolved,SL_AF_INET)<0){down("dns-error");return;}
        ip=resolved;
    }
    snprintf(root,sizeof root,"homie/5/%s",config.homie_id);
    sock=sl_Socket(SL_AF_INET,SL_SOCK_STREAM,config.mqtt_tls?SL_SEC_SOCKET:SL_IPPROTO_TCP);
    if(sock<0){down("socket-error");return;}
    if(config.mqtt_tls) {
        _u8 method=3; /* TLS 1.2; never allow an unverified connection (-453). */
        if(sl_SetSockOpt(sock,SL_SOL_SOCKET,SL_SO_SECMETHOD,&method,sizeof method)<0||
           sl_SetSockOpt(sock,SL_SOL_SOCKET,SL_SO_SECURE_FILES_CA_FILE_NAME,CA_PATH,strlen(CA_PATH))<0||
           sl_SetSockOpt(sock,SL_SOL_SOCKET,35,config.broker,strlen(config.broker))<0) {
            down("tls-options-error");return;
        }
    }
    if(socket_nonblocking(sock)<0){down("socket-error");return;}
    memset(&address,0,sizeof address);address.sin_family=SL_AF_INET;
    address.sin_port=sl_Htons(config.mqtt_port);address.sin_addr.s_addr=sl_Htonl(ip);
    connection=CONNECTING;deadline=millis()+12000;status="connecting";
}
static void led_command_result(const MqttPublish *message,const char *result) {
    /* Only the LED topic is recorded. Bounded hex cannot inject JSON or leak
     * credentials from unrelated topics. Keep the latest result across MQTT
     * reconnects, and publish it through the normal serialized QoS2 path. */
    char hex[17];size_t bytes=message->payload_len;if(bytes>8)bytes=8;
    for(size_t i=0;i<bytes;i++)snprintf(hex+i*2,3,"%02x",message->payload[i]);
    hex[bytes*2]=0;
    snprintf(led_command,sizeof led_command,
        "seq=%lu result=%s qos=%u retain=%u dup=%u len=%u hex=%s",
        (unsigned long)++led_command_count,result,(unsigned)message->qos,
        (unsigned)!!message->retained,(unsigned)!!message->duplicate,
        (unsigned)message->payload_len,bytes?hex:"-");
}
static void on_publish(const uint8_t *p,size_t n) {
    MqttPublish message;if(!mqtt_parse_publish(p,n,&message)){down("invalid-publish");return;}
    char topic[112];snprintf(topic,sizeof topic,"%s/chime/led/set",root);
    if(message.topic_len==strlen(topic)&&!memcmp(message.topic,topic,message.topic_len)) {
        const char *result;
        if(message.qos)result="rejected-qos";
        else if(!mqtt_ready())result="rejected-not-ready";
        else if(app_busy())result="rejected-busy";
        else if(message.retained)result="rejected-retained";
        else if(message.duplicate)result="rejected-duplicate";
        else if(message.payload_len==4&&!memcmp(message.payload,"true",4)) {
            board_led_set(true);result="applied-true";
        } else if(message.payload_len==5&&!memcmp(message.payload,"false",5)) {
            board_led_set(false);result="applied-false";
        } else result="rejected-payload";
        led_command_result(&message,result);
        if(message.qos)down("unexpected-command-qos");
        return;
    }
    if(message.qos){down("unexpected-command-qos");return;}
    if(!mqtt_ready()||app_busy()||message.retained||message.duplicate)return;
    snprintf(topic,sizeof topic,"%s/chime/play/set",root);
    if(play_command_valid(&message,topic,true,audio_playing())) {
        if(audio_play())publish("chime/play","original",true);
        return;
    }
    snprintf(topic,sizeof topic,"%s/chime/volume/set",root);
    if(message.topic_len==strlen(topic)&&!memcmp(message.topic,topic,message.topic_len)&&message.payload_len>0&&message.payload_len<4) {
        char value[4];memcpy(value,message.payload,message.payload_len);value[message.payload_len]=0;uint32_t v;
        if(!memchr(message.payload,0,message.payload_len)&&parse_uint(value,100,&v))app_set_volume(v);
    }
}
static void packet(const uint8_t *p,size_t n,size_t h) {
    unsigned type=p[0]>>4;const uint8_t *b=p+h;size_t len=n-h;
    if(connection==CONNACK) {
        if(type!=2||p[0]!=0x20||len<3||b[0]!=0||b[1]!=0){down("connack-rejected");return;}
        connection=ONLINE;phase=0;pending=0;available=false;closing=false;ping_at=0;
        last_playing=last_volume=last_available=last_led=-1;last_led_diagnostic[0]=last_led_command[0]=0;
        last_error[0]=last_web[0]=last_diagnostic[0]=0;last_ip=0;uptime_at=0;status="init";return;
    }
    if(type==3){on_publish(p,n);return;}
    if(type==13&&len==0){ping_at=0;return;}
    if(type==14){down("broker-disconnected");return;}
    if(type==5||type==7||type==9) {
        if(len<2||p[0]!=(type<<4)){down("invalid-ack");return;}
        unsigned id=(unsigned)b[0]*256+b[1];
        if(!pending||id!=pending){down("unexpected-ack");return;}
        if(type==5&&(ack_type==5||ack_type==7)) {
            if(len>=3&&b[2]>=0x80){down("publish-rejected");return;}
            uint8_t rel[2];put16(rel,id);
            if(send_packet(0x62,rel,2)){ack_type=7;deadline=millis()+5000;}
        } else if(type==7&&ack_type==7) {
            if(len>=3&&b[2]>=0x80){down("pubcomp-error");return;}
            pending=0;ack_type=0;if(phase<INITIAL_PHASES)phase++;
        } else if(type==9&&ack_type==9) {
            /* Broker has no reason to send properties here; support their length. */
            size_t pos=2,props=0,mul=1;bool done=false;
            for(unsigned i=0;i<4&&pos<len;i++){unsigned v=b[pos++];props+=(v&127)*mul;if(!(v&128)){done=true;break;}mul*=128;}
            if(!done||props>len-pos||len-pos-props!=COMMAND_COUNT){down("subscribe-rejected");return;}
            for(unsigned i=0;i<COMMAND_COUNT;i++)if(b[pos+props+i]!=0){down("subscribe-rejected");return;}
            pending=0;ack_type=0;sub_wait=false;phase++;
        } else down("unexpected-ack-type");
    } else down("unexpected-packet");
}
static void receive_packets(void) {
    int n=sl_Recv(sock,rx+used,sizeof rx-used,0);
    if(n==0||(n<0&&n!=SL_EAGAIN)){down("connection-lost");return;}
    if(n>0)used+=n;
    while(used&&connection>=CONNACK) {
        size_t h=0;int size=mqtt_frame_size(rx,used,sizeof rx,&h);
        if(size<0){down("packet-too-large");return;}
        if(!size)break;
        packet(rx,size,h);if(connection==DOWN)return;
        used-=size;memmove(rx,rx+size,used);
    }
    if(used==sizeof rx)down("invalid-frame");
}
void mqtt_poll(void) {
    if(!configured||ap_mode||!wifi_ip_ready){if(sock>=0)down("offline");return;}
    uint32_t now=millis();
    if(connection==DOWN){if(!audio_playing()&&!app_busy()&&(int32_t)(now-retry_at)>=0)start_connect();return;}
    if(connection==CONNECTING) {
        int rc=sl_Connect(sock,(SlSockAddr_t*)&address,sizeof address);
        if(rc==0)connect_packet();
        else if(rc!=SL_EALREADY||((int32_t)(now-deadline)>=0))down("connect-failed");
        return;
    }
    receive_packets();if(connection==DOWN)return;
    if((connection==CONNACK||pending)&&(int32_t)(now-deadline)>=0){down("ack-timeout");return;}
    if(ping_at&&(uint32_t)(now-ping_at)>10000){down("ping-timeout");return;}
    if(!ping_at&&(uint32_t)(now-last_io)>15000){if(send_packet(0xc0,NULL,0))ping_at=now;}
    if(connection!=ONLINE||pending||closing||ota_busy())return;
    char value[24];
    switch(phase) {
    case 0:publish("$state","init",false);break;
    case 1:{char description[1400];if(homie_description(description,sizeof description,config.name))publish("$description",description,false);else down("description-error");break;}
    case 2:subscribe_commands();break;
    case 3:publish("system/firmware",FW_VERSION,false);break;
    case 4:snprintf(value,sizeof value,"%u",config.volume);last_volume=config.volume;publish("chime/volume",value,false);break;
    case 5:last_playing=audio_playing();publish("chime/playing",last_playing?"true":"false",false);break;
    case 6:strncpy(last_error,audio_error(),sizeof last_error-1);publish("chime/error",last_error,false);break;
    case 7:snprintf(value,sizeof value,"%lu",(unsigned long)(now/1000));uptime_at=now;publish("system/uptime",value,false);break;
    case 8:last_ip=wifi_ip;ip_format(last_ip,value);publish("system/ip",value,false);break;
    case 9:snprintf(last_web,sizeof last_web,"%s",http_status());publish("system/web",last_web,false);break;
    case 10:snprintf(last_diagnostic,sizeof last_diagnostic,"%s",audio_diagnostic());publish("chime/diagnostic",last_diagnostic,false);break;
    case 11:last_led=board_led_on();publish("chime/led",last_led?"true":"false",false);break;
    case 12:snprintf(last_led_diagnostic,sizeof last_led_diagnostic,"%s",board_led_diagnostic());publish("chime/led-diagnostic",last_led_diagnostic,false);break;
    case 13:snprintf(last_led_command,sizeof last_led_command,"%s",mqtt_led_command());publish("chime/led-command",last_led_command,false);break;
    case 14:last_available=audio_ready()&&ota_confirmed();publish("$state",last_available?"ready":"init",false);break;
    default:
        { bool capable=audio_ready()&&ota_confirmed();
        available=capable&&last_available==1;status=available?"ready":"init";
        if(last_available!=(int)capable){last_available=capable;publish("$state",capable?"ready":"init",false);}
        else if(last_playing!=(int)audio_playing()){last_playing=audio_playing();publish("chime/playing",last_playing?"true":"false",false);}
        else if(last_volume!=config.volume){last_volume=config.volume;snprintf(value,sizeof value,"%u",config.volume);publish("chime/volume",value,false);}
        else if(last_led!=(int)board_led_on()){last_led=board_led_on();publish("chime/led",last_led?"true":"false",false);}
        else if(strcmp(last_led_diagnostic,board_led_diagnostic())){snprintf(last_led_diagnostic,sizeof last_led_diagnostic,"%s",board_led_diagnostic());publish("chime/led-diagnostic",last_led_diagnostic,false);}
        else if(strcmp(last_led_command,mqtt_led_command())){snprintf(last_led_command,sizeof last_led_command,"%s",mqtt_led_command());publish("chime/led-command",last_led_command,false);}
        else if(strcmp(last_error,audio_error())){strncpy(last_error,audio_error(),sizeof last_error-1);publish("chime/error",last_error,false);}
        else if(last_ip!=wifi_ip){last_ip=wifi_ip;ip_format(last_ip,value);publish("system/ip",value,false);}
        else if(strcmp(last_web,http_status())){snprintf(last_web,sizeof last_web,"%s",http_status());publish("system/web",last_web,false);}
        else if(strcmp(last_diagnostic,audio_diagnostic())){snprintf(last_diagnostic,sizeof last_diagnostic,"%s",audio_diagnostic());publish("chime/diagnostic",last_diagnostic,false);}
        else if((uint32_t)(now-uptime_at)>30000){uptime_at=now;snprintf(value,sizeof value,"%lu",(unsigned long)(now/1000));publish("system/uptime",value,false);}
        }
    }
}
void mqtt_shutdown(void) {
    if(sock<0)return;
    available=false;closing=true;
    uint32_t start=millis();
    while(pending&&sock>=0&&(uint32_t)(millis()-start)<1500){network_pump();receive_packets();}
    if(sock>=0&&connection==ONLINE&&!pending&&publish("$state","disconnected",false)) {
        start=millis();while(pending&&sock>=0&&(uint32_t)(millis()-start)<1500){network_pump();receive_packets();}
        if(!pending&&sock>=0)send_packet(0xe0,NULL,0);
    }
    down("disconnected");
}
