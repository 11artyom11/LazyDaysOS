#ifndef __KERN_PWRUP_H__
#define __KERN_PWRUP_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <multiboot.h>


#define KERN$PWRUP      1
#define KERN$PWRDOWN    0

/* Values written to QEMU's isa-debug-exit port; QEMU exits with (value << 1) | 1 */
#define KERN$TEST_PASS  0x00        /* qemu exit status 1 */
#define KERN$TEST_FAIL  0x01        /* qemu exit status 3 */

/* Set when the kernel command line contains the word "test" */
extern bool kern$test_mode;

/* Memory, paging, heap, timer, PCI and network card bring-up. Interrupts stay disabled */
uint16_t powerup_kernel_i386(const multiboot_info_t* mbi);

/* Run every boot-time self test; needs interrupts enabled for the timer test */
size_t kern$self_test(void);

/* Leave QEMU with the given status; halts forever where isa-debug-exit is absent */
void kern$test_exit(uint8_t status) __attribute__((noreturn));

#endif /* __KERN_PWRUP_H__ */
