#include "util.h"
#include "kio.h"

/**
 * @brief This function checks the integrity of the passed lazy block
 * 
 * @return size_t LAZY$BLOCKINV if lb is NULL
 * @return size_t LAZY$BASEINV  if lb's base address is invalid
 * @return size_t LAZY$LIMINV   if lb's limit is invalid
 * @return size_t UTIL$SUCCESS  if lb is integral/valid
 * 
 */
size_t lazy$check_integrity (const struct lazy$block* __lb)
{
    if (!__lb) {
        return LAZY$BLOCKINV; /* One of the blocks is overall invalid */
    }

    if (NULL == __lb->lb$base_addr) {
        return LAZY$BASEINV; /* Source's base address is invalid */
    }

    if (0 > __lb->lb$limit) {
        return LAZY$LIMINV; /* Source's limit value is invalid */
    }
    return UTIL$SUCCESS;
}

/**
 * @brief This test is intended to test the virtual memory tranlation
 *        process.
 * 
 * @return UTIL$SUCCESS if virtual address space is being successfullt trsltd into
 *         physical
 * @return UTIL$FAIL otherwise
 */
size_t util$paging_test (void)
{
    uint32_t *virt_addr = (uint32_t *)0x1FFFE + 0x100;
    *virt_addr = 0x12345678;  // Write data to virtual address

    // Read data back and verify
    if (*virt_addr != 0x12345678) {
        return UTIL$FAIL;
    }
    
    return UTIL$SUCCESS;
}

/**
 * @brief util$blkcpy_weak function performs weak copy operation on __dest.
 *        Weak copy operation does not allocate new space for __dest lazy block.
 *        __dest hold the address of the __src instead
 * 
 * @param __dest  lazy block which will be populated
 * @param __src   lazy block from which data will be copied
 * 
 * @return size_t UTIL$SUCCESS on success
 * @return size_t LAZY$BLOCKINV if one of the blocks is overall invalid
 * @return size_t LAZY$BASEINV if source's base address is invalid
 * @return size_t LAZY$LIMINV if source's limit value is invalid
 */
size_t util$blkcpy_weak (struct lazy$block* __dest, const struct lazy$block* __src)
{
    size_t status = UTIL$FAIL;
    
    /* Dest MUST NOT be NULL */
    if (__dest == NULL) {
        return LAZY$BLOCKINV;
    }

    /* Src MUST be integral */
    if ((status = lazy$check_integrity(__src)) != UTIL$SUCCESS) {
        return status;
    }
    /* All the checks passed, proceed to weak copy */

    /* NOTE* Alignment will not be taken into account for a good */    
    __dest->lb$base_addr = __src->lb$base_addr;
    __dest->lb$limit     = __src->lb$limit;

    return UTIL$SUCCESS;
}

/**
 * @brief util$blkcpy_strong performs strong copy operation on the lazy block dest based on the lazy block src
 *        Strong copy allocates memory for dest block to point, then copies to that memory the content of the 
 *        src block.
 * 
 * @param __dest destination block struct, new memory will be allocated through this struct.
 * @param __src  source block struct. The full content of the memory to which this struct points will be copied to 
 *               __dest. 
 *              If __src->lb$limit <= __dest->lb$limit, then all content will be copied and __dest->lb$limit will not be affected
 *              If __src->lb$limit >  __dest->lb$limit, then content will be copied up to the N element (N is __dest->lb$limit),
 *              and Truncate code will be returned
 * @return size_t UTIL$SUCCESS on success
 * @return size_t UTIL$FAIL on fail
 * @return size_t UTIL$MEMTRUNC if dest limit is less than that of source
 * 
 * @note _dest lazy block *MUST* be statically allocated before calling this function 
 */
size_t util$blkcpy_strong(struct lazy$block* __dest, const struct lazy$block* __src)
{
    char* d_ptr = NULL;
    char* s_ptr = NULL;
    size_t status = UTIL$FAIL;
    size_t i = 0;

    /* Check integrity of the __dest lazy block */
    if ((status = lazy$check_integrity(__dest)) != UTIL$SUCCESS) {
        k_print("[UTIL_FAIL] __dest integrity check was not passed, status:%d", status);
        return UTIL$FAIL;
    }
    
    /* Check integrity of __src lazy block */
    if ((status = lazy$check_integrity(__src)) != UTIL$SUCCESS) {
        k_print("[UTIL_FAIL] __src integrity check was not passed, status:%d", status);
        return UTIL$FAIL;
    }
    /* Integrity checks passed, good to go */
    
    d_ptr = (char*)__dest->lb$base_addr;
    s_ptr = (char*)__src->lb$base_addr;

    for (i = 0; i < __dest->lb$limit; ++i) {
        *(d_ptr+i) = *(s_ptr+i);
    }

    /* If copy was performed but dest limit is less than src limit, return MEMTRUNC */
    if (i < __src->lb$limit) {
        return UTIL$MEMTRUNC;
    }

    return UTIL$SUCCESS;
}

/* UNIT TESTS */

/**
 * @brief Unit test for util$$blkcpy_weak_test 
 * 
 * @return size_t 1 on success
 */
static size_t util$$blkcpy_weak_test (void)
{
    char test_array[TEST$ARRAY_LIMIT] = {0};
    size_t status;
    struct lazy$block src;
    struct lazy$block dest = {0,0};

    src.lb$base_addr = test_array;
    src.lb$limit     = TEST$ARRAY_LIMIT;
    
    status = util$blkcpy_weak(&dest, &src);
    if (status != UTIL$SUCCESS) {
        k_print("[UTIL_FAIL] Failed to weak-copy src block\n");
        return status;
    }

    if (dest.lb$base_addr != src.lb$base_addr) {
        k_print("[UTIL_FAIL] dest base address does not match src base address\n");
        return UTIL$FAIL;
    }

    if (dest.lb$limit != src.lb$limit) {
        k_print("[UTIL_FAIL] dest limit does not match dest limit\n");
        return UTIL$FAIL;
    }

    return UTIL$SUCCESS;
}

static size_t util$$blkcpy_strong_test(void)
{
  
    /* UTIL$SUCCESS case */
    {
        char dest_arr[TEST$ARRAY_LIMIT] = {0};
        char src_arr[TEST$ARRAY_LIMIT] = {0xFF-1};
        struct lazy$block dest = {dest_arr, sizeof(dest_arr)};
        struct lazy$block src = {src_arr, sizeof(src_arr)};
        size_t status = 0;
        
        if ((status = util$blkcpy_strong(&dest, &src)) != UTIL$SUCCESS) {
            k_print("[UTIL_FAIL] Failed to stong-copy src block\n");
            return status;
        }

        for (size_t i = 0; i < dest.lb$limit; ++i) {
            if (src.lb$base_addr[i] != dest.lb$base_addr[i]) {
                return UTIL$FAIL;
            }
        }

        if (dest.lb$limit != src.lb$limit) {
            k_print("[UTIL_FAIL] dest limit does not match dest limit\n");
            return UTIL$FAIL;
        }
    }

    /* UTIL$MEMTRUCT case */
    {
        char dest_arr[TEST$ARRAY_LIMIT/2] = {0};
        char src_arr[TEST$ARRAY_LIMIT] = {0xFF-1};
        struct lazy$block dest = {dest_arr, sizeof(dest_arr)};
        struct lazy$block src = {src_arr, sizeof(src_arr)};
        size_t status = 0;
        
        if ((status = util$blkcpy_strong(&dest, &src)) != UTIL$MEMTRUNC) {
            k_print("[UTIL_FAIL] Failed to stong-copy src block\n");
            return status;
        }

        for (size_t i = 0; i < dest.lb$limit; ++i) {
            if (src.lb$base_addr[i] != dest.lb$base_addr[i]) {
                return UTIL$FAIL;
            }
        }

        /* If MEMTRUNK is returned it will not make sense to compare limits */
        if (status != UTIL$MEMTRUNC) {
            if (dest.lb$limit != src.lb$limit) {
                k_print("[UTIL_FAIL] dest limit does not match dest limit\n");
                return UTIL$FAIL;
            }
        }
    }
    

    return UTIL$SUCCESS;
}

/**
 * @brief Unit tests compiled in this function
 * TODO: Remove this function to special tests location
 * 
 * @return size_t 1 on sucess
 * @return size_t error code on specific failure
 */
size_t util$self_test (void)
{
    size_t status = 0;

    k_print("[K_INFO] .........  Test util$$blkcpy_weak_test()\n");
    if ((status = util$$blkcpy_weak_test()) != UTIL$SUCCESS) {
        return status;
    }

    k_print("[K_INFO] .........  Test util$$blkcpy_strong_test()\n");
    if ((status = util$$blkcpy_strong_test()) != UTIL$SUCCESS) {
        return status;
    }

    k_print("[K_INFO] .........  Test util$$paging_test()\n");
    if ((status = util$paging_test()) != UTIL$SUCCESS) {
        return status;
    }

    return UTIL$SUCCESS;
}