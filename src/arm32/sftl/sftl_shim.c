// SPDX-License-Identifier: MIT

/* Library pieces the kernel's flash translation reader expects (include/linux/kernel.h). */

#include <linux/kernel.h>
#include <linux/mtd/apple-ppn.h>

#include "apple-sftl-vfl.h"

void *memchr_inv(const void *start, int c, size_t bytes)
{
    const u8 *p = start;

    for (; bytes; bytes--, p++)
        if (*p != (u8)c)
            return (void *)p;
    return NULL;
}

static void swap_bytes(u8 *a, u8 *b, size_t size)
{
    while (size--) {
        u8 t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Heap sort: in place, no recursion, n log n whatever the input */
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
          void (*swap_func)(void *, void *, int))
{
    u8 *a = base;

    UNUSED(swap_func);
    if (num < 2)
        return;

    for (size_t start = num / 2; start-- > 0;) {
        for (size_t root = start, child; (child = 2 * root + 1) < num; root = child) {
            if (child + 1 < num && cmp(a + child * size, a + (child + 1) * size) < 0)
                child++;
            if (cmp(a + root * size, a + child * size) >= 0)
                break;
            swap_bytes(a + root * size, a + child * size, size);
        }
    }
    for (size_t end = num - 1; end > 0; end--) {
        swap_bytes(a, a + end * size, size);
        for (size_t root = 0, child; (child = 2 * root + 1) < end; root = child) {
            if (child + 1 < end && cmp(a + child * size, a + (child + 1) * size) < 0)
                child++;
            if (cmp(a + root * size, a + child * size) >= 0)
                break;
            swap_bytes(a + root * size, a + child * size, size);
        }
    }
}

/* m1n1 only reads: the VFL is never rewritten here */
int apple_sftl_vfl_rotate_checked(struct apple_ppn *ppn, const struct apple_sftl_vfl_source *source,
                                  const struct apple_sftl_vfl_ops *ops, void *priv, bool *started)
{
    UNUSED(ppn);
    UNUSED(source);
    UNUSED(ops);
    UNUSED(priv);
    if (started)
        *started = false;
    return -EROFS;
}
