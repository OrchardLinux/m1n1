#include "ringbuffer.h"
#include "malloc.h"
#include "types.h"
#include "utils.h"

ringbuffer_t *ringbuffer_alloc(size_t len)
{
    ringbuffer_t *bfr = calloc(1, sizeof(*bfr));
    if (!bfr)
        return NULL;

    bfr->buffer = calloc(len, 1);
    if (!bfr->buffer) {
        free(bfr);
        return NULL;
    }

    bfr->read = 0;
    bfr->write = 0;
    bfr->len = len;

    return bfr;
}

void ringbuffer_free(ringbuffer_t *bfr)
{
    if (bfr)
        free(bfr->buffer);
    free(bfr);
}

/* Whole words: this carries all of a USB bulk stream, in both directions. */
static void ringbuffer_copy(u8 *dst, const u8 *src, size_t len)
{
    while (((u64)dst | (u64)src) & 7 && len) {
        *dst++ = *src++;
        len--;
    }
    if (!(((u64)dst | (u64)src) & 7)) {
        for (; len >= 64; len -= 64, dst += 64, src += 64) {
            u64 *d = (u64 *)dst;
            const u64 *s = (const u64 *)src;
            d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = s[3];
            d[4] = s[4], d[5] = s[5], d[6] = s[6], d[7] = s[7];
        }
    }
    while (len--)
        *dst++ = *src++;
}

size_t ringbuffer_read(u8 *target, size_t len, ringbuffer_t *bfr)
{
    size_t read = 0;

    while (read < len && bfr->read != bfr->write) {
        size_t end = bfr->write > bfr->read ? bfr->write : bfr->len;
        size_t chunk = min(end - bfr->read, len - read);

        ringbuffer_copy(target + read, bfr->buffer + bfr->read, chunk);
        read += chunk;
        bfr->read = (bfr->read + chunk) % bfr->len;
    }

    return read;
}

size_t ringbuffer_write(const u8 *src, size_t len, ringbuffer_t *bfr)
{
    size_t written = 0;

    while (written < len) {
        /* One slot stays empty, so that read == write means empty. */
        size_t end = bfr->read > bfr->write ? bfr->read - 1 : bfr->len - (bfr->read == 0);
        size_t chunk = min(end - bfr->write, len - written);

        if (!chunk)
            break;
        ringbuffer_copy(bfr->buffer + bfr->write, src + written, chunk);
        written += chunk;
        bfr->write = (bfr->write + chunk) % bfr->len;
    }

    return written;
}

size_t ringbuffer_get_used(ringbuffer_t *bfr)
{
    size_t read = bfr->read;
    size_t write = bfr->write;

    if (write < read)
        write += bfr->len;

    return write - read;
}

size_t ringbuffer_get_free(ringbuffer_t *bfr)
{
    return bfr->len - ringbuffer_get_used(bfr);
}
