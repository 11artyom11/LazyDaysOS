#ifndef __MULTIBOOT_H__
#define __MULTIBOOT_H__

#include <stdint.h>

/* Multiboot (v1) structures the bootloader hands to the kernel in EBX */

#define MULTIBOOT_BOOTLOADER_MAGIC  0x2BADB002

#define MULTIBOOT_INFO_MEMORY       0x00000001  /* mem_lower / mem_upper valid */
#define MULTIBOOT_INFO_CMDLINE      0x00000004  /* cmdline valid */
#define MULTIBOOT_INFO_MEM_MAP      0x00000040  /* mmap_addr / mmap_length valid */

#define MULTIBOOT_MEMORY_AVAILABLE  1

typedef struct multiboot_info
{
    uint32_t flags;
    uint32_t mem_lower;                 /* KiB below 1 MiB */
    uint32_t mem_upper;                 /* KiB above 1 MiB, up to the first hole */
    uint32_t boot_device;
    uint32_t cmdline;                   /* physical address of a C string */
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
} __attribute__((packed)) multiboot_info_t;

typedef struct multiboot_mmap_entry
{
    uint32_t size;                      /* size of the rest of the entry, excluding this field */
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry_t;

#endif /* __MULTIBOOT_H__ */
