/* SPDX-License-Identifier: MIT */

#include "../exception.h"
#include "../iodev.h"
#include "../uart.h"
#include "../utils.h"

extern char _vectors_start[0];
extern char _exc_stack_bot[0];

void exc_set_stacks(void *stack_bottom);
void exc_handler(u32 type, struct exc_info *ctx);

volatile enum exc_guard_t exc_guard = GUARD_OFF;
volatile int exc_count = 0;

enum {
    EXC_RESET = 0,
    EXC_UNDEF = 1,
    EXC_SVC = 2,
    EXC_PABT = 3,
    EXC_DABT = 4,
    EXC_IRQ = 6,
    EXC_FIQ = 7,
};

static const char *const exc_names[8] = {
    "reset",          "undefined instruction", "supervisor call", "prefetch abort",
    "data abort",     "?",                     "IRQ",             "FIQ",
};

static const char *const mode_names[32] = {
    [0x10] = "user", [0x11] = "FIQ",        [0x12] = "IRQ",    [0x13] = "supervisor",
    [0x17] = "abort", [0x1b] = "undefined", [0x1f] = "system",
};

void exception_initialize(void)
{
    exc_set_stacks(_exc_stack_bot);
    /* Vectors at VBAR, not at the high or low fixed address */
    cp15_write(CP15_SCTLR, cp15_read(CP15_SCTLR) & ~SCTLR_V);
    cp15_write(CP15_VBAR, (u32)_vectors_start);
    sysop("isb");
}

void exception_shutdown(void)
{
}

static void print_exception(u32 type, struct exc_info *ctx, u32 fsr, u32 far)
{
    const char *mode = mode_names[ctx->spsr & 0x1f];
    u32 *r = ctx->regs;

    printf("Exception: %s\n", exc_names[type & 7]);
    printf("Exception taken from %s mode\n", mode ? mode : "?");
    printf("MPIDR: 0x%x\n", cp15_read(CP15_MPIDR));
    printf("Registers: (@%p)\n", ctx);
    printf("  r0-r3: %08x %08x %08x %08x\n", r[0], r[1], r[2], r[3]);
    printf("  r4-r7: %08x %08x %08x %08x\n", r[4], r[5], r[6], r[7]);
    printf(" r8-r11: %08x %08x %08x %08x\n", r[8], r[9], r[10], r[11]);
    printf("    r12: %08x\n", r[12]);
    printf("PC:       0x%x (rel: 0x%x)\n", ctx->pc, ctx->pc - (u32)_base);
    printf("LR:       0x%x (rel: 0x%x)\n", ctx->lr, ctx->lr - (u32)_base);
    printf("SP:       0x%x\n", ctx->sp);
    printf("SPSR:     0x%x\n", ctx->spsr);
    if (type == EXC_DABT || type == EXC_PABT) {
        printf("FAR:      0x%x\n", far);
        printf("FSR:      0x%x\n", fsr);
    }
}

void exc_handler(u32 type, struct exc_info *ctx)
{
    u32 fsr = 0, far = 0;

    if (type == EXC_DABT) {
        fsr = cp15_read(CP15_DFSR);
        far = cp15_read(CP15_DFAR);
    } else if (type == EXC_PABT) {
        fsr = cp15_read(CP15_IFSR);
        far = cp15_read(CP15_IFAR);
    }

    if (!(exc_guard & GUARD_SILENT))
        print_exception(type, ctx, fsr, far);

    switch (exc_guard & GUARD_TYPE_MASK) {
        case GUARD_SKIP:
            ctx->pc += 4;
            break;
        case GUARD_MARK: {
            /* A load that faulted returns a recognisable value in its Rt */
            u32 insn = type == EXC_DABT ? read32(ctx->pc) : 0;
            u32 rt = (insn >> 12) & 0xf;

            if (type == EXC_DABT && rt < 13)
                ctx->regs[rt] = 0xabad1dea;
            ctx->pc += 4;
            break;
        }
        case GUARD_RETURN:
            /* Leave the function that faulted: a 64-bit result is r1:r0 */
            ctx->regs[0] = 0xabad1dea;
            ctx->regs[1] = 0xacce5515;
            ctx->pc = ctx->lr;
            exc_guard = GUARD_OFF;
            break;
        case GUARD_OFF:
        default:
            printf("Unhandled exception, rebooting...\n");
            flush_and_reboot();
    }

    exc_count++;

    if (!(exc_guard & GUARD_SILENT))
        printf("Recovering from exception (PC=0x%x)\n", ctx->pc);
}
