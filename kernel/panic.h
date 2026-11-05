#ifndef __PANIC_H__
#define __PANIC_H__

#include "isr.h"

/* Print the reason (and register state, if any) in red, then halt forever */
void k_panic(const char* msg) __attribute__((noreturn));
void k_panic_frame(const char* msg, const struct int_frame* frame) __attribute__((noreturn));

#endif /* __PANIC_H__ */
