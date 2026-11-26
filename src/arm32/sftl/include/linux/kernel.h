/* SPDX-License-Identifier: MIT */

/*
 * The Linux services Orchard's flash translation reader uses, in terms of
 * m1n1: enough for drivers/mtd/apple-sftl-*.c and lib/rbtree.c to build
 * here unchanged. One core, no interrupts, no sleeping.
 */

#ifndef SFTL_LINUX_KERNEL_H
#define SFTL_LINUX_KERNEL_H

#include "../../../../types.h"
#include "../../../../utils.h"
#include <malloc.h>
#include <string.h>

typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;

#define U8_MAX  ((u8)~0U)
#define U16_MAX ((u16)~0U)
#define U32_MAX ((u32)~0U)
#define U64_MAX ((u64)~0ULL)

#define EPERM      1
#define EIO        5
#define E2BIG      7
#define EBUSY      16
#define ENODEV     19
#define ENOSPC     28
#define EROFS      30
#define ERANGE     34
#define ENODATA    61
#define EBADMSG    74
#define EOVERFLOW  75
#define EOPNOTSUPP 95
#define ETIMEDOUT  110
#include <errno.h>

#define BIT_ULL(x)         (1ULL << (x))
#define GENMASK_ULL(h, l)  (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define BITS_PER_LONG      32
#define BITS_TO_LONGS(n)   (((n) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define DECLARE_BITMAP(name, bits) unsigned long name[BITS_TO_LONGS(bits)]
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define round_up(x, y)      ((((x) - 1) | ((__typeof__(x))(y) - 1)) + 1)
#define round_down(x, y)    ((x) & ~((__typeof__(x))(y) - 1))

#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define max_t(type, a, b) ((type)(a) > (type)(b) ? (type)(a) : (type)(b))
#define swap(a, b)                                                                                 \
    do {                                                                                           \
        __typeof__(a) __tmp = (a);                                                                 \
        (a) = (b);                                                                                 \
        (b) = __tmp;                                                                               \
    } while (0)

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)
#define __always_inline inline __attribute__((always_inline))
#define noinline        __attribute__((noinline))
#define __maybe_unused  __attribute__((unused))
#define READ_ONCE(x)    (x)
#define WRITE_ONCE(x, v) ((x) = (v))

#define EXPORT_SYMBOL(x)
#define EXPORT_SYMBOL_GPL(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_AUTHOR(x)

#define rcu_assign_pointer(p, v) ((p) = (v))
#define rcu_dereference_raw(p)   (p)

static inline unsigned int hweight32(u32 v)
{
    return __builtin_popcount(v);
}
#define hweight16(v) hweight32((u16)(v))
#define hweight8(v)  hweight32((u8)(v))

static inline int fls(u32 v)
{
    return v ? 32 - __builtin_clz(v) : 0;
}

static inline bool test_bit(unsigned int nr, const unsigned long *map)
{
    return (map[nr / BITS_PER_LONG] >> (nr % BITS_PER_LONG)) & 1;
}

static inline void __set_bit(unsigned int nr, unsigned long *map)
{
    map[nr / BITS_PER_LONG] |= 1UL << (nr % BITS_PER_LONG);
}

static inline void __clear_bit(unsigned int nr, unsigned long *map)
{
    map[nr / BITS_PER_LONG] &= ~(1UL << (nr % BITS_PER_LONG));
}
#define set_bit   __set_bit
#define clear_bit __clear_bit

static inline u16 get_unaligned_le16(const void *p)
{
    u16 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline u32 get_unaligned_le32(const void *p)
{
    u32 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline u64 get_unaligned_le64(const void *p)
{
    u64 v;
    memcpy(&v, p, sizeof(v));
    return v;
}

static inline void put_unaligned_le16(u16 v, void *p)
{
    memcpy(p, &v, sizeof(v));
}

static inline void put_unaligned_le32(u32 v, void *p)
{
    memcpy(p, &v, sizeof(v));
}

static inline void put_unaligned_le64(u64 v, void *p)
{
    memcpy(p, &v, sizeof(v));
}

/* Allocation: one heap, nothing to choose */
#define GFP_KERNEL 0
#define GFP_NOIO   0
typedef unsigned int gfp_t;

static inline size_t array_size(size_t a, size_t b)
{
    size_t r;
    return __builtin_mul_overflow(a, b, &r) ? (size_t)-1 : r;
}

static inline void *kmalloc(size_t size, gfp_t flags)
{
    UNUSED(flags);
    return size == (size_t)-1 ? NULL : malloc(size);
}

static inline void *kzalloc(size_t size, gfp_t flags)
{
    UNUSED(flags);
    return size == (size_t)-1 ? NULL : calloc(1, size);
}

static inline void *kcalloc(size_t n, size_t size, gfp_t flags)
{
    return kzalloc(array_size(n, size), flags);
}

#define kvmalloc(size, flags)          kmalloc(size, flags)
#define kvzalloc(size, flags)          kzalloc(size, flags)
#define kvcalloc(n, size, flags)       kcalloc(n, size, flags)
#define kvmalloc_array(n, size, flags) kmalloc(array_size(n, size), flags)
#define kmalloc_array(n, size, flags)  kmalloc(array_size(n, size), flags)
#define kvrealloc(p, size, flags)      realloc(p, size)
#define vmalloc(size)                  kmalloc(size, 0)
#define vzalloc(size)                  kzalloc(size, 0)
#define kfree(p)                       free((void *)(p))
#define kvfree(p)                      free((void *)(p))
#define vfree(p)                       free((void *)(p))

struct mutex {
    int unused;
};
#define mutex_init(m)   ((void)(m))
#define mutex_lock(m)   ((void)(m))
#define mutex_unlock(m) ((void)(m))

/* Time in milliseconds */
#define HZ      1000
#define jiffies ((unsigned long)ticks_to_msecs(get_ticks()))
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after(b, a)
#define cond_resched()   do { } while (0)

static inline u64 div_u64(u64 a, u32 b)
{
    return a / b;
}

void *memchr_inv(const void *start, int c, size_t bytes);
void sort(void *base, size_t num, size_t size, int (*cmp)(const void *, const void *),
          void (*swap_func)(void *, void *, int));

/* Device registers */
#define __iomem
#define readl(addr)       read32((u32)(addr))
#define writel(val, addr) write32((u32)(addr), val)

#define readl_poll_timeout(addr, val, cond, sleep_us, timeout_us)                                  \
    ({                                                                                             \
        u64 __deadline = timeout_calculate(timeout_us);                                            \
        int __ret = 0;                                                                             \
        for (;;) {                                                                                 \
            (val) = readl(addr);                                                                   \
            if (cond)                                                                              \
                break;                                                                             \
            if (timeout_expired(__deadline)) {                                                     \
                (val) = readl(addr);                                                               \
                __ret = (cond) ? 0 : -ETIMEDOUT;                                                   \
                break;                                                                             \
            }                                                                                      \
        }                                                                                          \
        __ret;                                                                                     \
    })

/* ktime in microseconds */
typedef u64 ktime_t;
#define ktime_get()             ticks_to_usecs(get_ticks())
#define ktime_add_us(t, us)     ((t) + (us))
#define ktime_add_ms(t, ms)     ((t) + (u64)(ms) * 1000)
#define ktime_after(a, b)       ((a) > (b))
#define ktime_before(a, b)      ((a) < (b))
#define cpu_relax()             do { } while (0)

#define usleep_range(min, max) udelay(min)
#define msleep(ms)             mdelay(ms)
#define mutex_lock_nested(m, c) ((void)(m))
#define SINGLE_DEPTH_NESTING    1
#define BUILD_BUG_ON(c)         _Static_assert(!(c), #c)

struct device;
#define dev_err(dev, ...)  printf("sftl: " __VA_ARGS__)
#define dev_warn(dev, ...) printf("sftl: " __VA_ARGS__)
#define dev_info(dev, ...) printf("sftl: " __VA_ARGS__)
#define pr_err(...)        printf("sftl: " __VA_ARGS__)
#define pr_warn(...)       printf("sftl: " __VA_ARGS__)
#define pr_info(...)       printf("sftl: " __VA_ARGS__)

#endif
