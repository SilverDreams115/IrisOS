OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0x200000;
  __kernel_start = .;

  .text : ALIGN(0x1000)
  {
    *(.text*)
    *(.rodata*)
  } :text

  .data : ALIGN(0x1000)
  {
    *(.data*)
  } :data

  .bss : ALIGN(0x1000)
  {
    *(.bss*)
    *(COMMON)
  } :data

  . = ALIGN(0x1000);
  __kernel_end = .;

  /DISCARD/ :
  {
    *(.comment)
    *(.note*)
    *(.eh_frame*)
  }
}
