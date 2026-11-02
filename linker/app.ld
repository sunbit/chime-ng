ENTRY(Reset_Handler)
MEMORY { RAM (rwx) : ORIGIN = 0x20010000, LENGTH = 192K }
SECTIONS {
 .vectors : { KEEP(*(.vectors)) } > RAM
 .text : { *(.text*) *(.rodata*) } > RAM
 .ARM.extab : { *(.ARM.extab*) } > RAM
 .ARM.exidx : { *(.ARM.exidx*) } > RAM
 .data : { *(.data*) . = ALIGN(4); _image_end = .; } > RAM
 .bss (NOLOAD) : { . = ALIGN(1024); _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); _bss_end = .; } > RAM
 _stack_top = ORIGIN(RAM) + LENGTH(RAM);
 ASSERT(_image_end - ORIGIN(RAM) <= 128K, "Application image exceeds OTA limit")
 ASSERT(_bss_end <= _stack_top - 12K, "Application leaves less than 12 KiB stack")
 /DISCARD/ : { *(.comment) *(.note*) }
}
