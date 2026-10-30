// C symbol names in assembly: Mach-O puts an underscore before every C name, ELF (Android) does not. Assembly writes SYM(name) for a symbol C also sees
#pragma once
#ifdef __APPLE__
#define SYM(name) _##name
#define SYM_HIDDEN .private_extern
#else
#define SYM(name) name
#define SYM_HIDDEN .hidden
#endif
// The page and offset halves of a symbol's address (adrp / add): PAGE(name), PAGEOFF(name)
#ifdef __APPLE__
#define PAGE(name) _##name@PAGE
#define PAGEOFF(name) _##name@PAGEOFF
#else
#define PAGE(name) name
#define PAGEOFF(name) :lo12:name
#endif
// Section of C strings
#ifdef __APPLE__
#define CSTRING_SECTION .cstring
#else
#define CSTRING_SECTION .section .rodata.str1.1,"aMS",%progbits,1
#endif
