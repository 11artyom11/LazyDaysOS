#ifndef __GDT_H__
#define __GDT_H__

#include <stdint.h>

#define GDT_ENTRY_MAX 8192

/* 
    Structure to hold Global descriptor table entry info
 */
typedef struct  {
    uint16_t limit;     /* The lowest 16 bits of the limit */
    uint32_t base : 24; /* The lowest 24 bits of the base address of the segment */
    uint8_t access;     /* Access byte */
    /* 
        Access byte is splitted to these bitfields
    P: Present bit. Allows an entry to refer to a valid segment. Must be set (1) for any valid segment.
    DPL: Descriptor privilege level field. Contains the CPU Privilege level of the segment. 0 = highest privilege (kernel), 3 = lowest privilege (user applications).
    S: Descriptor type bit. If clear (0) the descriptor defines a system segment (eg. a Task State Segment). If set (1) it defines a code or data segment.
    E: Executable bit. If clear (0) the descriptor defines a data segment. If set (1) it defines a code segment which can be executed from.
    DC: Direction bit/Conforming bit.
        For data selectors: Direction bit. If clear (0) the segment grows up. If set (1) the segment grows down, ie. the Offset has to be greater than the Limit.
        For code selectors: Conforming bit.
            If clear (0) code in this segment can only be executed from the ring set in DPL.
            If set (1) code in this segment can be executed from an equal or lower privilege level. For example, code in ring 3 can far-jump to conforming code in a ring 2 segment. The DPL field represent the highest privilege level that is allowed to execute the segment. For example, code in ring 0 cannot far-jump to a conforming code segment where DPL is 2, while code in ring 2 and 3 can. Note that the privilege level remains the same, ie. a far-jump from ring 3 to a segment with a DPL of 2 remains in ring 3 after the jump. 
    RW: Readable bit/Writable bit.
        For code segments: Readable bit. If clear (0), read access for this segment is not allowed. If set (1) read access is allowed. !!!!Write access is never allowed for code segments.!!!!
        For data segments: Writeable bit. If clear (0), write access for this segment is not allowed. If set (1) write access is allowed. Read access is always allowed for data segments. 
    A: Accessed bit. The CPU will set it when the segment is accessed unless set to 1 in advance. This means that in case the GDT descriptor is stored in read only pages and this bit is set to 0, the CPU trying to set this bit will trigger a page fault. Best left set to 1 unless otherwise needed. 
     */
    uint8_t limit_2 : 4; /* The highest 4 bits of the limit of descriptor */
    uint8_t flags : 4; /* flags quartet */
    /* 
    flags quartet is splitted into these bitfields
    G: Granularity flag, indicates the size the Limit value is scaled by. If clear (0), the Limit is in 1 Byte blocks (byte granularity). If set (1), the Limit is in 4 KiB blocks (page granularity).
    DB: Size flag. If clear (0), the descriptor defines a 16-bit protected mode segment. If set (1) it defines a 32-bit protected mode segment. A GDT can have both 16-bit and 32-bit selectors at once.
    L: Long-mode code flag. If set (1), the descriptor defines a 64-bit code segment. When set, DB should always be clear. For any other type of segment (other code types or any data segment), it should be clear (0). 
     */
    uint8_t base_2; /* The highest 8 bit of the base of the segment */
} __attribute__((packed)) segment_descriptor_t;


/* 
    Structure to hold the pointer and overall size
    of Global descriptor table
 */
typedef struct {
    uint16_t gdt_size; /* The size of the table in bytes subtracted by 1 */
    segment_descriptor_t* offset; /* The start LINEAR address of the GDT */
} __attribute__((packed)) gdt_ptr_t;


#endif /* __GDT_H__ */