/* SPDX-License-Identifier: MIT */

/* Main storage of the 32-bit chips is raw NAND behind translation layers m1n1 cannot read yet. */

#include "../adt.h"
#include "../utils.h"

bool main_storage_init(void)
{
    printf("Storage: storage not supported!\n");
    return false;
}

bool main_storage_read(u64 lba, u64 *bfr)
{
    UNUSED(lba);
    UNUSED(bfr);
    return false;
}

bool main_storage_read_blocks(u64 lba, u32 count, void *bfr)
{
    UNUSED(lba);
    UNUSED(count);
    UNUSED(bfr);
    return false;
}

void main_storage_shutdown(void)
{
}

bool main_storage_flush(void)
{
    return false;
}
