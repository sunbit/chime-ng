#include "app.h"
#include <string.h>

static int fd=-1;
static uint32_t expected, received;
static ImageHeader header;
static BootRecord initial;
static unsigned target;
static bool uploading, confirmed;

bool ota_busy(void) { return uploading; }
bool ota_confirmed(void) { return confirmed; }
void ota_abort(void) {
    if(fd>=0)fs_close(fd);
    fd=-1;uploading=false;received=0;
    /* A partial inactive image is harmless: no boot record was changed. */
}
bool ota_begin(uint32_t bytes) {
    if(uploading||audio_playing()||!confirmed||bytes<sizeof header+1024||
       bytes>sizeof header+IMAGE_MAX||!boot_read(&initial)||initial.pending!=NO_SLOT)return false;
    target=1-initial.active;expected=bytes;received=0;fd=-1;uploading=true;
    memset(&header,0,sizeof header);return true;
}
bool ota_write(const void *data,size_t n) {
    const uint8_t *p=data;
    if(!uploading||n>expected-received)return false;
    if(received<sizeof header) {
        size_t take=sizeof header-received;if(take>n)take=n;
        memcpy((uint8_t*)&header+received,p,take);received+=take;p+=take;n-=take;
        if(received==sizeof header) {
            if(!image_header_valid(&header)||header.size+sizeof header!=expected)return false;
            fd=fs_open_write(slot_path(target),IMAGE_MAX+sizeof header,false);
            if(fd<0||fs_write(fd,0,&header,sizeof header)!=sizeof header)return false;
        }
    }
    if(n) {
        if(fd<0||fs_write(fd,received,p,n)!=(int)n)return false;
        received+=n;
    }
    return true;
}
bool ota_finish(void) {
    if(!uploading||fd<0||received!=expected)return false;
    int rc=fs_close(fd);fd=-1;if(rc<0)return false;
    /* Verify the actual flash contents, not merely the upload's RAM buffer. */
    fd=fs_open_read(slot_path(target));if(fd<0)return false;
    uint8_t block[1024],digest[32],vectors[8];ImageHeader stored;Sha256 sha;
    if(fs_size(slot_path(target))!=(int)expected||fs_read(fd,0,&stored,sizeof stored)!=sizeof stored||
       memcmp(&header,&stored,sizeof header))return false;
    sha256_init(&sha);
    for(uint32_t off=0;off<header.size;) {
        size_t n=header.size-off;if(n>sizeof block)n=sizeof block;
        if(fs_read(fd,sizeof header+off,block,n)!=(int)n)return false;
        if(!off)memcpy(vectors,block,sizeof vectors);
        sha256_update(&sha,block,n);off+=n;watchdog_feed();
    }
    sha256_final(&sha,digest);fs_close(fd);fd=-1;
    if(!constant_equal(digest,header.sha,32)||!image_vectors_valid(&header,vectors))return false;
    BootRecord now;
    if(!boot_read(&now)||memcmp(&now,&initial,sizeof now))return false;
    now.pending=target;now.tried=0;now.generation++;
    if(!boot_save(&now))return false;
    uploading=false;return true;
}
bool ota_confirm(void) {
    BootRecord b;
    if(!boot_read(&b))return false;
    if(b.pending<2) {
        if(!b.tried)return false;
        b.active=b.pending;b.pending=NO_SLOT;b.tried=0;b.generation++;
        if(!boot_save(&b))return false;
    }
    confirmed=true;return true;
}
