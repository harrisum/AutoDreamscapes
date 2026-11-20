#include <stddef.h>

void *memset(void *target, int byte, size_t count)
{
    unsigned char *out = target;
    for (size_t i = 0; i < count; ++i)
        out[i] = (unsigned char)byte;
    return target;
}

void *memcpy(void *target, const void *source, size_t count)
{
    unsigned char *out = target;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; ++i)
        out[i] = in[i];
    return target;
}

void *memmove(void *target, const void *source, size_t count)
{
    unsigned char *out = target;
    const unsigned char *in = source;
    if (out <= in)
        return memcpy(target, source, count);
    for (size_t i = count; i > 0; --i)
        out[i - 1] = in[i - 1];
    return target;
}
