#include "util.h"
#include "kio.h"

/**
 * @brief k_blkcpy_weak function performs weak copy operation on __dest.
 *        Weak copy operation does not allocate new space for __dest lazy block.
 *        __dest hold the address of the __src instead
 * 
 * @param __dest  lazy block which will be populated
 * @param __src   lazy block from which data will be copied
 * @return size_t 1 on success
 * @return size_t error code (< 0) on failure
 * @return size_t 0 IDK
 */
size_t k_blkcpy_weak (struct __lazy_block* __dest, const struct __lazy_block* __src)
{
    if (!(__dest && __src)) {
        return -1; /* TODO: set normal status code */
    }

    if (NULL == __src->_base_addr) {
        return -1; /* TODO: set normal status code */
    }

    if (0 > __src->_limit) {
        return -1; /* TODO: set normal status code */
    }

    /* All the checks passed, proceed to weak copy */

    /* NOTE* Alignment will not be taken into account for a good */    
    __dest->_base_addr = __src->_base_addr;
    __dest->_limit     = __src->_limit;

    /* 

    +-__lazy_block--+
    |               |               0   1   2   ....    N
    |  _base_addr   | --ptr--->  |   |   |   |   |   |   |
    |               |
    |---------------|
    |               |
    |    _limit     | --size_t--> N
    |               |
    +---------------+
    
     */
    return 1;
}

/**
 * @brief Unit tests compiled in this function
 * TODO: Remove this function to special tests location
 * 
 * @return size_t 1 on sucess
 * @return size_t error code on specific failure
 */
size_t self_test (void)
{
    char test_array[TEST_LIMIT] = {0};
    size_t status;
    struct __lazy_block src;
    struct __lazy_block dest = {0,0};

    src._base_addr = test_array;
    src._limit     = TEST_LIMIT;
    
    status = k_blkcpy_weak(&dest, &src);
    if (status != 1) {
        k_print("[FAIL] Failed to weak-copy src block\n");
        return status;
    }

    if (dest._base_addr != src._base_addr) {
        k_print("[FAIL] dest base address does not match src base address\n");
        return -1;
    }

    if (dest._limit != src._limit) {
        k_print("[FAIL] dest limit does not match dest limit\n");
        return -1;
    }

    return 1;
}