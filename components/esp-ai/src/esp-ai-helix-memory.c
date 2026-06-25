#include <stdlib.h>

void *helix_malloc(int size)
{
    if (size <= 0) {
        return NULL;
    }
    return malloc((size_t)size);
}

void helix_free(void *ptr)
{
    free(ptr);
}
