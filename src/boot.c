#include "platform.h"
#include <string.h>
static bool load_image(unsigned slot) {
    ImageHeader h;uint8_t digest[32];Sha256 hash;
    int fd=fs_open_read(slot_path(slot));if(fd<0)return false;
    bool ok=fs_read(fd,0,&h,sizeof h)==sizeof h && image_header_valid(&h);
    if(ok && fs_size(slot_path(slot))!=(int)(sizeof h+h.size))ok=false;
    if(ok) {
        sha256_init(&hash);
        for(uint32_t off=0;off<h.size;) {
            uint32_t n=h.size-off;if(n>1024)n=1024;
            void *dst=(void*)(APP_BASE+off);
            if(fs_read(fd,sizeof h+off,dst,n)!=(int)n) { ok=false;break; }
            sha256_update(&hash,dst,n);off+=n;watchdog_feed();
        }
        sha256_final(&hash,digest);
        ok=ok && constant_equal(digest,h.sha,32) && image_vectors_valid(&h,(const uint8_t*)APP_BASE);
    }
    fs_close(fd);return ok;
}
__attribute__((naked,noreturn)) static void jump_to_app(uint32_t sp,uint32_t pc) {
    __asm volatile("cpsid i\n msr msp,r0\n bx r1");
}
int main(void) {
    board_init();log_line("Chime NG boot 0.1.0");
    if(sl_Start(NULL,NULL,NULL)<0) { log_line("NWP start failed");for(;;){} }
    BootRecord b;
    if(!boot_read(&b)) { log_line("Invalid boot record; UART recovery required");for(;;){} }
    bool changed=b.pending<2;
    if(changed&&b.tried)log_line("Unconfirmed image: rollback");
    unsigned slot=boot_select(&b);
    /* Persist trial BEFORE transferring control. Interrupted boots roll back. */
    if(changed&&!boot_save(&b))for(;;){}
    if(!load_image(slot)) {
        if(slot!=b.active) {
            log_line("Invalid trial image: rollback");b.pending=NO_SLOT;b.tried=0;b.generation++;
            if(!boot_save(&b)||!load_image(b.active))for(;;){}
        } else { log_line("Invalid active image; UART recovery required");for(;;){} }
    }
    log_line("Verified image: starting");
    sl_Stop(200);MAP_SysTickDisable();MAP_SysTickIntDisable();
    jump_to_app(*(uint32_t*)APP_BASE,*(uint32_t*)(APP_BASE+4));
}
