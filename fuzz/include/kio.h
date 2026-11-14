#ifndef __KIO_LIB__
#define __KIO_LIB__

/* Host build of the network stack: k_print goes to stderr when FUZZ_LOG is set
   (fuzz/stubs.c). Shadows libk/include/kio.h, whose tty.h refuses to build on Linux. */

#include <stddef.h>

size_t k_print(char* format, ...);

#endif /* __KIO_LIB__ */
