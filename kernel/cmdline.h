#ifndef __CMDLINE_H__
#define __CMDLINE_H__

#include <stdbool.h>
#include <stddef.h>

/*
    Kernel command line: whitespace separated words and key=value options, e.g.
        test eth0=10.0.2.15/24 gw=10.0.2.2 ping=10.0.2.2
    The bootloader may put the kernel path first; it is just another word.
*/

#define CMDLINE_MAX 512

/* Keep a private copy; the bootloader's string is not used afterwards */
void cmdline_init(const char* cmdline);

const char* cmdline_raw(void);

/* True if `word` appears as a whole word */
bool cmdline_has(const char* word);

/* Copy the value of the nth (0 = first) `key=value` option into out; false if absent or too long */
bool cmdline_get(const char* key, size_t nth, char* out, size_t out_len);

#endif /* __CMDLINE_H__ */
