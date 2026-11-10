/* SPDX-License-Identifier: MIT */

#ifndef __PAYLOAD_H__
#define __PAYLOAD_H__

#include "types.h"

bool payload_logo(void **custom_128, void **custom_256);
bool payload_splash(u32 **pixels, u32 *width, u32 *height);

int payload_run(void);

#endif
