#ifndef __UTIL_H__
#define __UTIL_H__

#define TEST_LIMIT 1024

#include <stddef.h>
#include <stdint.h>

struct __lazy_block
{
    void* _base_addr;
    uint64_t _limit;
};

size_t k_blkcpy_weak (struct __lazy_block* __dest, const struct __lazy_block* __src);

size_t self_test (void);

#endif /* __UTIL__ */