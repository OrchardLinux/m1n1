/* SPDX-License-Identifier: MIT */

/*
 * USB on the 32-bit chips: one device-mode port, a Synopsys DWC2 controller
 * behind Apple's S5L PHY. iBoot powers the PHY down before it starts its
 * kernel, so it is brought up here the way the chip's own firmware does.
 */

#include "../adt.h"
#include "../iodev.h"
#include "../memory.h"
#include "../usb.h"
#include "../usb_dwc2.h"
#include "../utils.h"
#include "malloc.h"
#include "string.h"

#define OPHYPWR  0x00
#define OPHYCLK  0x04
#define ORSTCON  0x08
#define OPHYUNK1 0x1c
#define OPHYTUNE1 0x40
#define OPHYTUNE2 0x44
#define OPHYUNK4 0x60

#define OPHYPWR_ACTIVE      0x6
#define OPHYCLK_CLKSEL_MASK 0x3
#define ORSTCON_PHYSWRESET  BIT(0)
#define OPHYUNK1_START      0x4
#define OPHYUNK1_OTG_ENABLE BIT(1)
#define OPHYUNK4_START      0x200

#define OPHYTUNE1_DEVICE 0x549
#define OPHYTUNE2_DEVICE 0x2ff3

/*
 * Power-state registers of the A5 family's power manager (S5L8940X, 8942X,
 * 8945X). Their positions come from the pmgr node's device-clocks table, not
 * from a formula over the clock-gate ids.
 */
#define PMGR_UPERF   0x1080
#define PMGR_USB_OTG 0x1084
#define PMGR_USBREG  0x1098
#define PMGR_GATE_ON 0xf

#define DWC2_PCGCTL          0xe00
#define DWC2_PCGCTL_STOPPCLK BIT(0)
#define DWC2_PCGCTL_GATEHCLK BIT(1)

usb_type_t usb_type;
static bool usb_is_initialized;

struct iodev iodev_usb_vuart = {
    .usage = 0,
    .lock = SPINLOCK_INIT,
};

#define USB_IODEV_WRAPPER(name, pipe)                                                              \
    static ssize_t usb_##name##_can_read(void *dev)                                                \
    {                                                                                              \
        return usb_dwc2_can_read(dev, pipe);                                                       \
    }                                                                                              \
    static bool usb_##name##_can_write(void *dev)                                                  \
    {                                                                                              \
        return usb_dwc2_can_write(dev, pipe);                                                      \
    }                                                                                              \
    static ssize_t usb_##name##_read(void *dev, void *buf, size_t count)                           \
    {                                                                                              \
        return usb_dwc2_read(dev, pipe, buf, count);                                               \
    }                                                                                              \
    static ssize_t usb_##name##_write(void *dev, const void *buf, size_t count)                    \
    {                                                                                              \
        return usb_dwc2_write(dev, pipe, buf, count);                                              \
    }                                                                                              \
    static ssize_t usb_##name##_queue(void *dev, const void *buf, size_t count)                    \
    {                                                                                              \
        return usb_dwc2_queue(dev, pipe, buf, count);                                              \
    }                                                                                              \
    static void usb_##name##_handle_events(void *dev)                                              \
    {                                                                                              \
        usb_dwc2_handle_events(dev);                                                               \
    }                                                                                              \
    static void usb_##name##_flush(void *dev)                                                      \
    {                                                                                              \
        usb_dwc2_flush(dev, pipe);                                                                 \
    }                                                                                              \
    static struct iodev_ops iodev_usb_##name##_ops = {                                             \
        .can_read = usb_##name##_can_read,                                                         \
        .can_write = usb_##name##_can_write,                                                       \
        .read = usb_##name##_read,                                                                 \
        .write = usb_##name##_write,                                                               \
        .queue = usb_##name##_queue,                                                               \
        .flush = usb_##name##_flush,                                                               \
        .handle_events = usb_##name##_handle_events,                                               \
    };

USB_IODEV_WRAPPER(pri, CDC_ACM_PIPE_0)
USB_IODEV_WRAPPER(sec, CDC_ACM_PIPE_1)

/* Ask for a power state in bits 3:0 and wait for bits 7:4 to follow, as XNU does */
static int pmgr_gate_on(u64 reg)
{
    u32 v = read32(reg);

    if ((v & 0xff) == 0xff)
        return 0;

    write32(reg, (v & ~0x10f) | PMGR_GATE_ON);
    for (int n = 0; n < 1000; n++) {
        v = read32(reg);
        if (!((v ^ (v >> 4)) & 0xf))
            return 0;
        udelay(1);
    }
    printf("usb: power state at 0x%lx stuck at 0x%x\n", reg, v);
    return -1;
}

static bool is_a5_family(void)
{
    return chip_id == 0x8940 || chip_id == 0x8942 || chip_id == 0x8945;
}

static bool phy_is_ready(u64 phy, u32 clksel)
{
    return (read32(phy + OPHYCLK) & OPHYCLK_CLKSEL_MASK) == clksel &&
           read32(phy + OPHYPWR) == OPHYPWR_ACTIVE &&
           !(read32(phy + ORSTCON) & ORSTCON_PHYSWRESET) &&
           read32(phy + OPHYTUNE1) == OPHYTUNE1_DEVICE &&
           read32(phy + OPHYTUNE2) == OPHYTUNE2_DEVICE && read32(phy + OPHYUNK4) == OPHYUNK4_START;
}

static int usb_phy_init(int phy_node, u64 phy)
{
    /* Reference clock select: 0 = 12 MHz, 1 = 24 MHz, 2 = 48 MHz. These boards run it at 24. */
    u32 clksel = 1;
    u32 tune1 = OPHYTUNE1_DEVICE, tune2 = OPHYTUNE2_DEVICE;

    ADT_GETPROP(adt, phy_node, "uotgtune1-device", &tune1);
    ADT_GETPROP(adt, phy_node, "uotgtune2-device", &tune2);

    if (is_a5_family()) {
        int path[8];
        u64 pmgr;

        if (adt_path_offset_trace(adt, "/arm-io/pmgr", path) < 0 ||
            adt_get_reg(adt, path, "reg", 0, &pmgr, NULL)) {
            printf("usb: no power manager\n");
            return -1;
        }
        /* The chip's own firmware enables these parents in this order */
        if (pmgr_gate_on(pmgr + PMGR_UPERF) || pmgr_gate_on(pmgr + PMGR_USB_OTG) ||
            pmgr_gate_on(pmgr + PMGR_USBREG))
            return -1;
    } else {
        printf("usb: unknown chip 0x%x, keeping the power states the boot stage left\n", chip_id);
    }

    if (!phy_is_ready(phy, clksel)) {
        printf("usb: PHY was powered down, starting it\n");
        write32(phy + OPHYUNK4, OPHYUNK4_START);
        write32(phy + OPHYPWR, OPHYPWR_ACTIVE);
        write32(phy + OPHYUNK1, OPHYUNK1_START);
        write32(phy + OPHYTUNE1, tune1);
        write32(phy + OPHYTUNE2, tune2);
        udelay(10);
        mask32(phy + OPHYCLK, OPHYCLK_CLKSEL_MASK, clksel);
    }

    set32(phy + ORSTCON, ORSTCON_PHYSWRESET);
    udelay(20);
    clear32(phy + ORSTCON, ORSTCON_PHYSWRESET);
    if (poll32(phy + ORSTCON, ORSTCON_PHYSWRESET, 0, 1000)) {
        printf("usb: PHY reset did not clear\n");
        return -1;
    }
    udelay(1500);

    /* The OTG side of the PHY is on before the controller connects */
    set32(phy + OPHYUNK1, OPHYUNK1_OTG_ENABLE);
    return 0;
}

void usb_init(void)
{
    int phy_path[8], dev_path[8];
    u64 phy, regs;

    if (usb_is_initialized)
        return;

    int phy_node = adt_path_offset_trace(adt, "/arm-io/otgphyctrl", phy_path);
    if (phy_node < 0 || adt_get_reg(adt, phy_path, "reg", 0, &phy, NULL)) {
        printf("usb: no /arm-io/otgphyctrl\n");
        return;
    }
    if (adt_path_offset_trace(adt, "/arm-io/usb-complex/usb-device", dev_path) < 0 ||
        adt_get_reg(adt, dev_path, "reg", 0, &regs, NULL)) {
        printf("usb: no /arm-io/usb-complex/usb-device\n");
        return;
    }

    printf("usb: PHY at 0x%lx, controller at 0x%lx\n", phy, regs);

    if (usb_phy_init(phy_node, phy))
        return;

    /* The boot stage may have stopped the controller's own clocks as well */
    if (read32(regs + DWC2_PCGCTL) & (DWC2_PCGCTL_STOPPCLK | DWC2_PCGCTL_GATEHCLK)) {
        clear32(regs + DWC2_PCGCTL, DWC2_PCGCTL_STOPPCLK | DWC2_PCGCTL_GATEHCLK);
        udelay(100);
    }

    usb_type = USB_TYPE_DWC2;

    dwc2_dev_t *opaque = usb_dwc2_init(regs);
    if (!opaque)
        return;

    struct iodev *usb_iodev = memalign(SPINLOCK_ALIGN, sizeof(*usb_iodev));
    if (!usb_iodev)
        return;

    memset(usb_iodev, 0, sizeof(*usb_iodev));
    usb_iodev->ops = &iodev_usb_pri_ops;
    usb_iodev->opaque = opaque;
    usb_iodev->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
    spin_init(&usb_iodev->lock);

    iodev_register_device(IODEV_USB0, usb_iodev);
    printf("USB0: initialized at %p\n", opaque);

    /* The proxy also listens on the second pipe, the host's raw way in. */
    usb_iodev_vuart_setup(IODEV_USB0);
    iodev_usb_vuart.usage = USAGE_UARTPROXY;

    usb_is_initialized = true;
}

void usb_hpm_restore_irqs(bool force)
{
    UNUSED(force);
}

void usb_iodev_init(void)
{
    /* One port: usb_init() has set it up */
}

void usb_iodev_shutdown(void)
{
    struct iodev *usb_iodev = iodev_unregister_device(IODEV_USB0);

    if (!usb_iodev)
        return;

    printf("USB0: shutdown\n");
    usb_dwc2_shutdown(usb_iodev->opaque);
    dma_release();
    usb_is_initialized = false;
}

void usb_iodev_vuart_setup(iodev_id_t iodev)
{
    if (iodev < IODEV_USB0 || iodev >= IODEV_USB0 + USB_IODEV_COUNT)
        return;

    iodev_usb_vuart.ops = &iodev_usb_sec_ops;
    iodev_usb_vuart.opaque = iodev_get_opaque(iodev);
}
