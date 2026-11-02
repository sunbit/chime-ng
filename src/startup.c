#include <stdint.h>
#include <stddef.h>
extern uint32_t _stack_top,_bss_start,_bss_end;
extern int main(void);
void Reset_Handler(void);
void SysTick_Handler(void);
void Default_Handler(void) { for(;;){} }
/* All code/data execute from SRAM. IntRegister installs handlers in this table. */
__attribute__((section(".vectors"),aligned(1024),used))
void (*vectors[256])(void) = {
    [0]=(void(*)(void))&_stack_top,[1]=Reset_Handler,
    [2 ... 14]=Default_Handler,[15]=SysTick_Handler,[16 ... 255]=Default_Handler
};
void Reset_Handler(void) {
    __asm volatile("cpsid i");
    /* ROM/previous application may leave interrupts and SysTick enabled. */
    *(volatile uint32_t*)0xe000e010=0;
    for(unsigned i=0;i<8;i++) { ((volatile uint32_t*)0xe000e180)[i]=~0u;((volatile uint32_t*)0xe000e280)[i]=~0u; }
    *(volatile uint32_t*)0xe000ed08=(uint32_t)vectors;
    for(uint32_t *p=&_bss_start;p<&_bss_end;p++)*p=0;
    main();for(;;){}
}
/* Newlib nano only needs these for formatting. No semihosting or target heap. */
void *_sbrk(ptrdiff_t n) { (void)n;return (void*)-1; }
int _write(int f,const char*p,int n) { (void)f;(void)p;return n; }
int _read(int f,char*p,int n) { (void)f;(void)p;(void)n;return 0; }
int _close(int f) { (void)f;return -1; }
int _lseek(int f,int o,int w) { (void)f;(void)o;(void)w;return 0; }
int _isatty(int f) { (void)f;return 1; }
int _fstat(int f,void*s) { (void)f;(void)s;return -1; }
int _getpid(void) { return 1; }
int _kill(int p,int s) { (void)p;(void)s;return -1; }
void _exit(int c) { (void)c;for(;;){} }
