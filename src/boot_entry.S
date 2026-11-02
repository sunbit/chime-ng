.syntax unified
.cpu cortex-m4
.thumb
.section .text.entry,"ax",%progbits
.global _entry
/* ROM loads /sys/mcuimg.bin at 0x20004000. Move the real bootloader below
 * the application before running it, exactly as required for SRAM execution. */
.word 0x20010000
.word _entry + 1
.space 24
.thumb_func
_entry:
 cpsid i
 ldr r0, =0x20004100
 ldr r1, =0x20000000
 ldr r2, =BOOT_BYTES
1:
 ldr r3, [r0], #4
 str r3, [r1], #4
 subs r2, r2, #4
 bne 1b
 ldr r0, =0x20000000
 ldr r1, [r0]
 ldr r2, [r0, #4]
 msr msp, r1
 bx r2
.ltorg
