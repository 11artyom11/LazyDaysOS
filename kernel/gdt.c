#include "gdt.h"

static segment_descriptor_t gdt_entries[GDT_ENTRY_MAX];

extern void gdt_buf_drain(uint32_t);

void gdt_set_entry_ready(segment_descriptor_t* gdt_entry, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags)
{
    gdt_entry->base = (base & 0xFFFFFF);
    gdt_entry->limit = (limit & 0xFFFF);
    gdt_entry->access = access;
    gdt_entry->flags = (flags & 0x0F);
    gdt_entry->limit_2 = (limit >> 16) & 0x0F;
    gdt_entry->base_2 = (base >> 24) & 0xFF;
}

void gdt_set_entry(segment_descriptor_t* gdt_entry, uint32_t limit,
                    uint32_t base, uint8_t present, uint8_t ring, 
                    uint8_t system, uint8_t executable, uint8_t conforming, uint8_t rw,
                        uint8_t granul, uint8_t db, uint8_t lmode) {
    uint8_t access = 0;
    uint8_t flags = 0;
    /* 
     7   6   5   4   3   2   1   0
    [0] [0] [0] [0] [0] [0] [0] [0] 
     */
    access |= (present & 0b1);
    access = access << 2;
    access |=  (ring & 0b11);
    access = access << 1;
    access |= (system & 0b1);
    access = access << 1;
    access |= (executable & 0b1);
    access = access << 1;
    access |= (conforming & 0b1);
    access = access << 1;
    access |= (rw & 0b1);
    access = access << 1;
    /* 
     7   6   5   4   3   2   1   0
    [p] [r0] [r1] [s] [e] [c] [rw] [0] <= this last bit is left for CPU
     */

    flags |= (granul & 0b1);
    flags = flags << 1;    
    
    flags |= (db & 0b1);
    flags = flags << 1;    
    
    flags |= (lmode & 0b1);
    flags = flags << 1;
    /* 
     3   2   1   0
    [G] [DB] [L] [0] <= last bit is reserved
    */

    gdt_set_entry_ready(gdt_entry, base, limit, access, flags);
    return;
}


void init_gdt()
{
    gdt_ptr_t gdt_ptr;
    segment_descriptor_t* gdt_entry_ptr = gdt_entries;
    gdt_ptr.gdt_size = sizeof(segment_descriptor_t) * 3 - 1;
    gdt_ptr.offset = gdt_entries;

    gdt_set_entry_ready(gdt_entry_ptr, 0, 0, 0, 0);
    gdt_entry_ptr += sizeof(segment_descriptor_t);
    gdt_set_entry_ready(gdt_entry_ptr,  0x004, 0x003FFFFF, 0x9A, 0xC);
    gdt_entry_ptr += sizeof(segment_descriptor_t);
    gdt_set_entry_ready(gdt_entry_ptr, 0x008, 0x003FFFFF, 0x92, 0xC);
    gdt_entry_ptr += sizeof(segment_descriptor_t);

    gdt_buf_drain((uint32_t)&gdt_ptr);
}