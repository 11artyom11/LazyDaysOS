#ifndef __UTIL_H__
#define __UTIL_H__

#define TEST$ARRAY_LIMIT 1024

#define UTIL$MEMTRUNC   (UTIL$SUCCESS + 1)              /* Code is success but memory was truncated */
#define UTIL$SUCCESS    0x1                             /* General success code */
#define UTIL$FAIL       0x0                             /* General failure code */

#define LAZY$LIMINV     0x2                             /* Lazy block limit is invalid */
#define LAZY$BASEINV    0x4                             /* Lazy block base address is invalid */
#define LAZY$BLOCKINV   (LAZY$LIMINV | LAZY$BASEINV)    /* Lazy block is invalid */

#include <stddef.h>
#include <stdint.h>

typedef char* PTR;

struct lazy$block
{
    PTR lb$base_addr;
    uint64_t lb$limit;
};

/* 
+-----------------lazy$block-----------------+
|                                            |
|  lb$base_addr  --------------------->   +--|-->  [0]  [1]  [2]  ....  [N]  (Memory Chunk)
|                                            |
|--------------------------------------------|
|                                            |
|  lb$limit  ---------------> size_t ----->  N (Length of the Memory Chunk) |
|                                            |
+--------------------------------------------+
*/

size_t lazy$check_integrity (const struct lazy$block* __lb);

size_t util$blkcpy_weak (struct lazy$block* __dest, const struct lazy$block* __src);
static size_t util$$blkcpy_weak_test (void);

size_t util$blkcpy_strong(struct lazy$block* __dest, const struct lazy$block* __src);
static size_t util$$blkcpy_strong_test(void);

static size_t util$paging_test (void);

size_t util$self_test (void);

#endif /* __UTIL__ */