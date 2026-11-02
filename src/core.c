#include "core.h"
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(ImageHeader)==64,"wire header layout");
_Static_assert(sizeof(BootRecord)==16,"boot record layout");
static uint32_t le32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static uint16_t le16(const uint8_t *p) { return p[0]|(uint16_t)p[1]<<8; }
bool image_header_valid(const ImageHeader *h) {
    return !memcmp(h->magic,IMAGE_MAGIC,8) && h->board==BOARD_ID && h->format==1 &&
        h->load_address==APP_BASE && h->size>=1024 && h->size<=IMAGE_MAX && h->flags==0 && h->build>0;
}
bool image_vectors_valid(const ImageHeader *h,const uint8_t v[8]) {
    uint32_t sp=le32(v), pc=le32(v+4);
    return image_header_valid(h) && sp>APP_BASE+h->size && sp<=RAM_END && (sp&7)==0 &&
        (pc&1) && (pc&~1u)>=APP_BASE+8 && (pc&~1u)<APP_BASE+h->size;
}
bool boot_record_valid(const BootRecord *b) {
    return b->magic==BOOT_MAGIC && b->active<2 && (b->pending<2 || b->pending==NO_SLOT) &&
        b->pending!=b->active && b->tried<2 && (b->pending!=NO_SLOT || !b->tried) && b->reserved==0 &&
        b->crc==crc32(b,offsetof(BootRecord,crc));
}
void boot_record_seal(BootRecord *b) { b->magic=BOOT_MAGIC;b->crc=crc32(b,offsetof(BootRecord,crc)); }
int boot_running_slot(const BootRecord *b) { return b->pending<2 && b->tried ? b->pending : b->active; }
unsigned boot_select(BootRecord *b) {
    unsigned selected=b->active;
    if(b->pending<2) {
        if(b->tried){b->pending=NO_SLOT;b->tried=0;}
        else{selected=b->pending;b->tried=1;}
        b->generation++;boot_record_seal(b);
    }
    return selected;
}
bool homie_id_valid(const char *s) {
    size_t n=strlen(s);
    if(!n || n>48 || s[0]=='-' || s[n-1]=='-')return false;
    for(size_t i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='0'&&s[i]<='9')||s[i]=='-'))return false;
    return strstr(s,"--")==NULL;
}
static bool terminated(const char *s,size_t n) { return memchr(s,0,n)!=NULL; }
bool config_valid(const Config *c) {
    if(c->magic!=CONFIG_MAGIC || c->version!=1 || c->crc!=crc32(c,offsetof(Config,crc)))return false;
    if(!terminated(c->ssid,sizeof c->ssid)||!terminated(c->wifi_password,sizeof c->wifi_password)||
       !terminated(c->broker,sizeof c->broker)||!terminated(c->mqtt_user,sizeof c->mqtt_user)||
       !terminated(c->mqtt_password,sizeof c->mqtt_password)||!terminated(c->homie_id,sizeof c->homie_id)||
       !terminated(c->name,sizeof c->name)||!terminated(c->ntp_server,sizeof c->ntp_server))return false;
    size_t w=strlen(c->wifi_password);
    return c->ssid[0] && c->broker[0] && c->name[0] && homie_id_valid(c->homie_id) &&
        (!w || (w>=8 && w<=63)) && c->mqtt_port && c->volume<=100 && c->mqtt_tls<=1;
}
void config_seal(Config *c) { c->magic=CONFIG_MAGIC;c->version=1;c->crc=crc32(c,offsetof(Config,crc)); }
bool parse_uint(const char *s,uint32_t max,uint32_t *out) {
    if(!*s)return false;
    uint32_t value=0;
    for(;*s;s++) { if(*s<'0'||*s>'9')return false;unsigned d=(unsigned)(*s-'0');if(d>max||value>(max-d)/10)return false;value=value*10+d; }
    *out=value;return true;
}
static int hex(char c) { if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1; }
bool form_get(const char *form,const char *key,char *dst,size_t cap) {
    size_t k=strlen(key);bool found=false;
    if(!cap)return false;
    dst[0]=0;
    for(const char *p=form;*p;) {
        const char *end=strchr(p,'&');if(!end)end=p+strlen(p);
        if((size_t)(end-p)>k && !memcmp(p,key,k) && p[k]=='=') {
            if(found)return false;
            found=true;size_t n=0;
            for(const char *v=p+k+1;v<end;v++) {
                unsigned char c=(unsigned char)*v;
                if(c=='+')c=' ';
                else if(c=='%') { if(end-v<3||hex(v[1])<0||hex(v[2])<0)return false;c=(unsigned char)(hex(v[1])*16+hex(v[2]));v+=2; }
                if(c==0||c<0x20||c==0x7f||n+1>=cap)return false;
                dst[n++]=(char)c;
            }
            dst[n]=0;
        }
        p=*end?end+1:end;
    }
    return found;
}
size_t json_escape(char *out,size_t cap,const char *s) {
    size_t n=0;
    for(;*s;s++) {
        unsigned char c=(unsigned char)*s;
        if(c<0x20)return 0;
        if(c=='"'||c=='\\') { if(n+2>=cap)return 0;out[n++]='\\'; }
        if(n+1>=cap)return 0;
        out[n++]=(char)c;
    }
    if(n>=cap)return 0;
    out[n]=0;return n;
}
bool basic_password(const char *a,char *out,size_t cap) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char decoded[160];unsigned acc=0,bits=0;size_t n=0;
    if(strncmp(a,"Basic ",6))return false;
    for(a+=6;*a && *a!='=';a++) {
        const char *v=strchr(alphabet,*a);if(!v)return false;
        acc=(acc<<6)|(unsigned)(v-alphabet);bits+=6;
        if(bits>=8) { bits-=8;if(n+1>=sizeof decoded)return false;decoded[n++]=(char)(acc>>bits); }
    }
    decoded[n]=0;
    if(n<6||memcmp(decoded,"admin:",6)||memchr(decoded,0,n))return false;
    if(n-6>=cap)return false;
    memcpy(out,decoded+6,n-6);out[n-6]=0;return true;
}
bool wav_header(const uint8_t *p,size_t n,uint32_t file_size,uint32_t *offset,uint32_t *length) {
    if(n<12||file_size<12||memcmp(p,"RIFF",4)||memcmp(p+8,"WAVE",4)||le32(p+4)>file_size-8)return false;
    bool fmt=false;
    for(size_t i=12;i+8<=n;) {
        uint32_t size=le32(p+i+4);
        if(!memcmp(p+i,"fmt ",4)) {
            if(size<16||size>n-i-8)return false;
            fmt=le16(p+i+8)==1&&le16(p+i+10)==1&&le32(p+i+12)==22050&&
                le32(p+i+16)==44100&&le16(p+i+20)==2&&le16(p+i+22)==16;
        } else if(!memcmp(p+i,"data",4)) {
            if(!fmt||!size||(size&1)||i+8>file_size||size>file_size-i-8)return false;
            *offset=(uint32_t)i+8;*length=size;return true;
        }
        if(size>n-i-8)return false;
        i+=8+size+(size&1);
    }
    return false;
}
size_t homie_description(char *out,size_t cap,const char *name) {
    char escaped[200];if(!json_escape(escaped,sizeof escaped,name))return 0;
    int n=snprintf(out,cap,
        "{\"homie\":\"5.0\",\"version\":6,\"name\":\"%s\",\"type\":\"es.sunbit.chime\",\"nodes\":{"
        "\"chime\":{\"name\":\"Chime\",\"properties\":{"
        "\"play\":{\"name\":\"Play\",\"datatype\":\"enum\",\"format\":\"original\",\"settable\":true,\"retained\":false},"
        "\"playing\":{\"name\":\"Playing\",\"datatype\":\"boolean\"},"
        "\"volume\":{\"name\":\"Volume\",\"datatype\":\"integer\",\"format\":\"0:100\",\"unit\":\"%%\",\"settable\":true},"
        "\"led\":{\"name\":\"Blue LED\",\"datatype\":\"boolean\",\"settable\":true},"
        "\"led-command\":{\"name\":\"Last MQTT LED command\",\"datatype\":\"string\"},"
        "\"led-diagnostic\":{\"name\":\"LED diagnostic\",\"datatype\":\"string\"},"
        "\"error\":{\"datatype\":\"enum\",\"format\":\"none,codec,wav,underrun,storage,dma-timeout,audio-timeout,dma-error,i2s-error,i2s-start\"},"
        "\"diagnostic\":{\"datatype\":\"string\"}}},"
        "\"system\":{\"properties\":{\"firmware\":{\"datatype\":\"string\"},"
        "\"uptime\":{\"datatype\":\"integer\",\"unit\":\"s\",\"format\":\"0:\"},"
        "\"ip\":{\"name\":\"IP address\",\"datatype\":\"string\"},"
        "\"web\":{\"datatype\":\"string\"}}}}}",escaped);
    return n>0&&(size_t)n<cap?(size_t)n:0;
}
void ip_format(uint32_t ip,char out[16]) {
    snprintf(out,16,"%u.%u.%u.%u",(unsigned)(ip>>24),(unsigned)((ip>>16)&255),
             (unsigned)((ip>>8)&255),(unsigned)(ip&255));
}
size_t mqtt_varint(uint8_t out[4],size_t value) {
    if(value>268435455)return 0;size_t n=0;
    do { unsigned b=value%128;value/=128;if(value)b|=128;out[n++]=(uint8_t)b; } while(value);
    return n;
}
int mqtt_frame_size(const uint8_t *p,size_t n,size_t max,size_t *header) {
    if(n<2)return 0;size_t value=0,mul=1;
    for(size_t i=1;i<=4;i++) {
        if(i>=n)return 0;value+=(p[i]&127)*mul;
        if(!(p[i]&128)) { if(max<i+1||value>max-i-1)return -1;*header=i+1;return n>=value+i+1?(int)(value+i+1):0; }
        mul*=128;
    }
    return -1;
}
bool mqtt_parse_publish(const uint8_t *p,size_t n,MqttPublish *o) {
    size_t h=0;if(mqtt_frame_size(p,n,n,&h)!=(int)n||(p[0]>>4)!=3||n-h<2)return false;
    memset(o,0,sizeof *o);o->qos=(p[0]>>1)&3;o->retained=p[0]&1;o->duplicate=p[0]&8;
    if(o->qos==3)return false;
    size_t len=(size_t)p[h]*256+p[h+1];h+=2;
    if(!len||len>n-h)return false;
    o->topic=p+h;o->topic_len=len;h+=len;
    if(o->qos) { if(n-h<2)return false;o->packet_id=(uint16_t)(p[h]*256+p[h+1]);h+=2;if(!o->packet_id)return false; }
    size_t props=0,mul=1;bool ended=false;
    for(unsigned i=0;i<4&&h<n;i++) { unsigned v=p[h++];props+=(v&127)*mul;if(!(v&128)){ended=true;break;}mul*=128; }
    if(!ended||props>n-h)return false;h+=props;
    o->payload=p+h;o->payload_len=n-h;return true;
}
bool play_command_valid(const MqttPublish *p,const char *topic,bool ready,bool busy) {
    return ready&&!busy&&!p->retained&&!p->duplicate&&!p->qos&&p->topic_len==strlen(topic)&&
        !memcmp(p->topic,topic,p->topic_len)&&p->payload_len==8&&!memcmp(p->payload,"original",8);
}
