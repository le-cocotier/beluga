
#include "../include/utils.h"
#include "../include/galloc.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief realloc that zero-initializes newly added memory (like calloc does)
 * @param ptr       Existing allocation (or NULL for a fresh allocation)
 * @param old_size  Size in bytes of the existing allocation (0 if ptr is NULL)
 * @param new_size  Desired new size in bytes
 * @return New pointer on success, NULL on failure (original ptr untouched, per realloc semantics)
 */
void* recalloc(void* ptr, const size_t old_size, const size_t new_size)
{
    void* new_ptr = grealloc(ptr, new_size);
    if (new_ptr == NULL)
        return NULL;

    if (new_size > old_size)
        memset((char*)new_ptr + old_size, 0, new_size - old_size);

    return new_ptr;
}
