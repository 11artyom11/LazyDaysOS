#include "kio.h"
#include "hwio.h"
#include "idt.h"
#include "isr.h"
#include "panic.h"
#include "../libk/include/ttyin.h"
#include <pit.h>
#include <util.h>

#define PIC1_PORT_A 0x20
#define PIC2_PORT_A 0xA0

/* The PIC interrupts have been remapped */
#define PIC1_START_INTERRUPT 0x20
#define PIC2_START_INTERRUPT 0x28
#define PIC2_END_INTERRUPT   PIC2_START_INTERRUPT + 7

#define PIC_ACK     0x20

#define CPU_EXCEPTION_COUNT 32

typedef void (*isr_routine_ptr) (void);

static isr_routine_ptr isr_map[255] = {NULL};

static const char* exception_names[CPU_EXCEPTION_COUNT] = {
    "#DE divide error",             "#DB debug",
    "NMI",                          "#BP breakpoint",
    "#OF overflow",                 "#BR bound range exceeded",
    "#UD invalid opcode",           "#NM device not available",
    "#DF double fault",             "coprocessor segment overrun",
    "#TS invalid TSS",              "#NP segment not present",
    "#SS stack-segment fault",      "#GP general protection fault",
    "#PF page fault",               "reserved exception 15",
    "#MF x87 floating-point error", "#AC alignment check",
    "#MC machine check",            "#XM SIMD floating-point error",
    "#VE virtualization exception", "#CP control protection",
    "reserved exception 22",        "reserved exception 23",
    "reserved exception 24",        "reserved exception 25",
    "reserved exception 26",        "reserved exception 27",
    "#HV hypervisor injection",     "#VC VMM communication",
    "#SX security exception",       "reserved exception 31",
};

void register_routines()
{
    k_print("[K_INFO] Registering routines\n");
    isr_map[TIMER_INTERRUPT] = &pit_tick;
    isr_map[KBD_INTERRUPT] = &keyboard_read;
}

void interrupt_handler(struct int_frame* frame)
{
    uint32_t intno = frame->intno;

    /* A CPU exception nobody handles is fatal: returning would re-run the faulting instruction */
    if (intno < CPU_EXCEPTION_COUNT && isr_map[intno] == NULL) {
        k_panic_frame(exception_names[intno], frame);
    }

    if (intno >= 40) {
        outb(0xA0, 0x20);
    }
    if (intno >= 32) {
        outb(0x20, 0x20);
    }
    if (isr_map[intno] != NULL) {
        isr_map[intno]();
    }
    /* Call corresponding isr routine and return wait for new interrupts */
    return;
}

/**
 * @brief Loads known values into the general purpose registers, waits for the
 *        next interrupt with hlt and verifies that the handler restored them.
 *
 * @return UTIL$SUCCESS if all registers survived
 * @return UTIL$FAIL otherwise
 */
size_t isr_self_test(void)
{
    uint32_t eax, ebx, ecx, edx, esi, edi;

    asm volatile(
        "mov $0x11111111, %%eax\n\t"
        "mov $0x22222222, %%ebx\n\t"
        "mov $0x33333333, %%ecx\n\t"
        "mov $0x44444444, %%edx\n\t"
        "mov $0x55555555, %%esi\n\t"
        "mov $0x66666666, %%edi\n\t"
        "hlt\n\t"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx), "=S"(esi), "=D"(edi)
        :
        : "memory");

    if (eax != 0x11111111 || ebx != 0x22222222 || ecx != 0x33333333 ||
        edx != 0x44444444 || esi != 0x55555555 || edi != 0x66666666) {
        k_print("[UTIL_FAIL] registers changed across an interrupt\n");
        k_print("  eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x esi=0x%x edi=0x%x\n",
                eax, ebx, ecx, edx, esi, edi);
        return UTIL$FAIL;
    }
    return UTIL$SUCCESS;
}
