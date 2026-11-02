/* Exercise the real streamed WAV uploader and persisted selection with storage
 * failures. This model does not establish physical flash power-loss behavior. */
#include "app.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t files[3][SOUND_MAX_BYTES];
static int lengths[3]={-1,-1,-1},writing=-1;
static bool playing,busy,confirmed=true,fail_open,fail_write,fail_close,fail_save,fail_read;
static unsigned updates,writes;static uint32_t selected_offset,selected_length;
static const char *paths[]={"/chime/sound0.wav","/chime/sound1.wav","/chime/sound.bin"};
static int index_of(const char *p){for(int i=0;i<3;i++)if(!strcmp(paths[i],p))return i;return -1;}
bool app_busy(void){return busy||sound_busy();}
bool audio_playing(void){return playing;}
bool ota_confirmed(void){return confirmed;}
void watchdog_feed(void){}
void audio_use_sound(uint32_t offset,uint32_t length){updates++;selected_offset=offset;selected_length=length;}
int fs_size(const char *p){int i=index_of(p);return i<0?-1:lengths[i];}
int fs_open_read(const char *p){int i=index_of(p);return i<0||lengths[i]<0?-1:i;}
int fs_open_write(const char *p,uint32_t capacity,bool mirror){
    int i=index_of(p);assert(i>=0&&i<2&&capacity==SOUND_MAX_BYTES&&!mirror);
    assert(strcmp(sound_path(),p)); /* Never overwrite the currently selected WAV. */
    if(fail_open)return -1;writing=i;lengths[i]=0;return i;
}
int fs_read(int fd,uint32_t offset,void *p,size_t n){
    if(fail_read||fd<0||fd>2||offset+n>(unsigned)lengths[fd])return -1;
    memcpy(p,files[fd]+offset,n);return n;
}
int fs_write(int fd,uint32_t offset,const void *p,size_t n){
    assert(fd==writing&&fd<2&&offset+n<=SOUND_MAX_BYTES);
    if(fail_write)return -1;memcpy(files[fd]+offset,p,n);lengths[fd]=offset+n;writes++;return n;
}
int fs_close(int fd){assert(fd>=0&&fd<3);if(fd==writing)writing=-1;return fail_close?-1:0;}
bool fs_save(const char *p,const void *data,size_t n){
    assert(!strcmp(p,paths[2])&&n==48);
    if(fail_save)return false;memcpy(files[2],data,n);lengths[2]=n;return true;
}
static uint8_t wav[4096];
static void le16(uint8_t *p,unsigned n){p[0]=n;p[1]=n>>8;}
static void le32(uint8_t *p,unsigned n){le16(p,n);le16(p+2,n>>16);}
static void make_wav(void){
    memset(wav,0,sizeof wav);memcpy(wav,"RIFF",4);le32(wav+4,sizeof wav-8);memcpy(wav+8,"WAVEfmt ",8);
    le32(wav+16,16);le16(wav+20,1);le16(wav+22,1);le32(wav+24,22050);le32(wav+28,44100);
    le16(wav+32,2);le16(wav+34,16);memcpy(wav+36,"data",4);le32(wav+40,sizeof wav-44);
    for(unsigned i=44;i<sizeof wav;i++)wav[i]=(uint8_t)i;
}
static void reset(void){
    fail_open=fail_write=fail_close=fail_save=fail_read=playing=busy=false;confirmed=true;sound_abort();
    lengths[0]=lengths[1]=lengths[2]=-1;writing=-1;updates=writes=0;make_wav();sound_init();
    assert(!strcmp(sound_path(),WAV_PATH));
}
static void upload(unsigned chunk){
    assert(sound_begin(sizeof wav));
    for(unsigned off=0;off<sizeof wav;){unsigned n=sizeof wav-off;if(n>chunk)n=chunk;assert(sound_write(wav+off,n));off+=n;}
}
int main(void){
    reset();upload(7);assert(sound_finish());assert(updates==1&&selected_offset==44&&selected_length==sizeof wav-44);
    assert(!strcmp(sound_path(),paths[0])&&!memcmp(files[0],wav,sizeof wav));sound_init();assert(!strcmp(sound_path(),paths[0]));
    uint8_t previous[sizeof wav];memcpy(previous,files[0],sizeof previous);
    wav[100]^=1;upload(1024);assert(sound_finish());assert(!strcmp(sound_path(),paths[1]));assert(!memcmp(files[0],previous,sizeof previous));
    sound_init();assert(!strcmp(sound_path(),paths[1]));
    /* Interrupted replacement cannot select the partial inactive file. */
    assert(sound_begin(sizeof wav));assert(sound_write(wav,128));assert(!sound_finish());sound_init();assert(!strcmp(sound_path(),paths[1]));
    unsigned before=updates;
    for(unsigned byte=0;byte<4;byte++){
        make_wav();unsigned pos=(unsigned[]){0,20,22,24}[byte];wav[pos]^=1;
        upload(53);assert(!sound_finish());sound_abort();sound_init();assert(!strcmp(sound_path(),paths[1])&&updates==before);
    }
    make_wav();upload(71);files[0][200]^=1;assert(!sound_finish());sound_abort();sound_init();assert(!strcmp(sound_path(),paths[1]));
    upload(19);fail_close=true;assert(!sound_finish());fail_close=false;sound_abort();sound_init();assert(!strcmp(sound_path(),paths[1]));
    upload(61);fail_save=true;assert(!sound_finish());fail_save=false;sound_abort();sound_init();assert(!strcmp(sound_path(),paths[1]));
    upload(61);fail_read=true;assert(!sound_finish());fail_read=false;sound_abort();sound_init();assert(!strcmp(sound_path(),paths[1]));
    assert(sound_begin(sizeof wav));fail_write=true;assert(!sound_write(wav,50));fail_write=false;sound_abort();sound_init();assert(!strcmp(sound_path(),paths[1]));
    fail_open=true;assert(!sound_begin(sizeof wav));fail_open=false;
    playing=true;assert(!sound_begin(sizeof wav));playing=false;busy=true;assert(!sound_begin(sizeof wav));busy=false;
    confirmed=false;assert(!sound_begin(sizeof wav));confirmed=true;
    assert(!sound_begin(45)&&!sound_begin(SOUND_MAX_BYTES+1));
    assert(sound_begin(sizeof wav));assert(!sound_begin(sizeof wav));assert(!sound_write(wav,sizeof wav+1));sound_abort();
    assert(!sound_write(wav,1)&&!sound_finish());
    /* Corrupt selection or selected content falls back to the untouched initial sound. */
    files[1][200]^=1;sound_init();assert(!strcmp(sound_path(),WAV_PATH));
    reset();upload(1024);assert(sound_finish());files[2][0]^=1;sound_init();assert(!strcmp(sound_path(),WAV_PATH));
    /* A full-size valid upload is streamed; it is never kept in a firmware RAM buffer. */
    reset();assert(sound_begin(SOUND_MAX_BYTES));
    make_wav();le32(wav+4,SOUND_MAX_BYTES-8);le32(wav+40,SOUND_MAX_BYTES-44);
    for(unsigned off=0;off<SOUND_MAX_BYTES;off+=sizeof wav)assert(sound_write(wav,sizeof wav));
    assert(sound_finish()&&selected_length==SOUND_MAX_BYTES-44);
    puts("SOUND: real streaming upload, two slots, readback hash/format, persistent selection, interrupted writes, corruption, storage failures, busy guards, fallback and 512 KiB bound passed");
    return 0;
}
