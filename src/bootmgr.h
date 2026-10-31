/* SPDX-License-Identifier: MIT */

#ifndef BOOTMGR_H
#define BOOTMGR_H

#include "types.h"

int bootmgr_start(const char *id, const char *devicetree, u64 flags);

#endif
