#include "app.h"
#include <string.h>

/* The initial WAV is immutable. Uploads alternate between two separate files;
 * only a verified file can be selected by the small mirrored record. */
#define SOUND_RECORD_PATH "/chime/sound.bin"
#define SOUND_MAGIC 0x43485331u
typedef struct {
    uint32_t magic,slot,bytes;
    uint8_t digest[32];
    uint32_t crc;
} SoundRecord;
_Static_assert(sizeof(SoundRecord)==48,"Sound record layout changed");
static const char *const paths[]={"/chime/sound0.wav","/chime/sound1.wav"};
static unsigned active=2,target;
static int fd=-1;
static bool uploading;
static uint32_t expected,received;
static Sha256 incoming;

const char *sound_path(void) { return active<2?paths[active]:WAV_PATH; }
bool sound_busy(void) { return uploading; }
void sound_abort(void) {
    if(fd>=0)fs_close(fd);
    fd=-1;uploading=false;received=0;
}
static bool record_read(SoundRecord *r) {
    if(fs_size(SOUND_RECORD_PATH)!=(int)sizeof *r)return false;
    int file=fs_open_read(SOUND_RECORD_PATH);if(file<0)return false;
    int n=fs_read(file,0,r,sizeof *r),closed=fs_close(file);
    return n==sizeof *r&&closed>=0&&r->magic==SOUND_MAGIC&&r->slot<2&&
        r->bytes>=46&&r->bytes<=SOUND_MAX_BYTES&&r->crc==crc32(r,sizeof *r-4);
}
static bool verify(unsigned slot,uint32_t bytes,const uint8_t expected_hash[32],
                   uint32_t *offset,uint32_t *length) {
    if(fs_size(paths[slot])!=(int)bytes)return false;
    int file=fs_open_read(paths[slot]);if(file<0)return false;
    uint8_t block[1024],digest[32];Sha256 hash;sha256_init(&hash);
    bool ok=true;
    for(uint32_t pos=0;pos<bytes;) {
        size_t n=bytes-pos;if(n>sizeof block)n=sizeof block;
        if(fs_read(file,pos,block,n)!=(int)n){ok=false;break;}
        if(!pos&&!wav_header(block,n,bytes,offset,length)){ok=false;break;}
        sha256_update(&hash,block,n);pos+=n;watchdog_feed();
    }
    int closed=fs_close(file);sha256_final(&hash,digest);
    return ok&&closed>=0&&constant_equal(digest,expected_hash,32);
}
void sound_init(void) {
    sound_abort();active=2;
    SoundRecord record;uint32_t offset,length;
    if(record_read(&record)&&verify(record.slot,record.bytes,record.digest,&offset,&length))active=record.slot;
}
bool sound_begin(uint32_t bytes) {
    if(app_busy()||audio_playing()||!ota_confirmed()||bytes<46||bytes>SOUND_MAX_BYTES)return false;
    target=active==0?1:0;expected=bytes;received=0;
    fd=fs_open_write(paths[target],SOUND_MAX_BYTES,false);
    if(fd<0)return false;
    uploading=true;sha256_init(&incoming);return true;
}
bool sound_write(const void *data,size_t n) {
    if(!uploading||fd<0||n>expected-received)return false;
    if(fs_write(fd,received,data,n)!=(int)n)return false;
    sha256_update(&incoming,data,n);received+=n;return true;
}
bool sound_finish(void) {
    if(!uploading||fd<0||received!=expected)return false;
    int closed=fs_close(fd);fd=-1;if(closed<0)return false;
    SoundRecord record={0};uint32_t offset,length;
    record.magic=SOUND_MAGIC;record.slot=target;record.bytes=expected;
    sha256_final(&incoming,record.digest);
    if(!verify(target,expected,record.digest,&offset,&length))return false;
    record.crc=crc32(&record,sizeof record-4);
    if(!fs_save(SOUND_RECORD_PATH,&record,sizeof record))return false;
    SoundRecord saved;
    if(!record_read(&saved)||memcmp(&saved,&record,sizeof record))return false;
    active=target;uploading=false;audio_use_sound(offset,length);return true;
}
