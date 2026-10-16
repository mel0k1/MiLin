/* MiLin efiboot (M22): PE32+ образ фиксированной геометрии.
 * Заголовки (секция .pe из efiboot.c) — первые 512 байт файла,
 * тело (.flat) — с LMA 0x200, VA = ImageBase 0x100000 + 0x200.
 * Хвост добивается нулями до ровно 16К: SizeOfImage/VirtualSize
 * зашиты в PE-заголовке константой и не зависят от линковки. */
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
  . = 0x100000;

  .pe 0x100000 : AT(0) {
    *(.pe)
  }

  .flat 0x100200 : AT(0x200) {
    *(.text.start)
    *(.text .text.*)
    *(.rodata .rodata.*)
    *(.data .data.*)
    *(.bss .bss.*) *(COMMON)
    . = 0xFE00;   /* относительно начала секции: тело ровно 0xFE00 */
  }

  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.*) *(.rela*) }
}
