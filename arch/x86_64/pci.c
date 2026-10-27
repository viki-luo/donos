/*
 * pci.c — PCI 总线驱动（物件发现的真实来源）
 *
 * 为什么它不在基座里：
 *
 *   基座的职责只有一件 —— 维护「当前有哪些活着的接线」。
 *   至于「怎么发现这些接线」，是总线的事。把扫描逻辑塞进基座，
 *   基座的窄腰就会被撑宽，而且每加一种总线都要改基座。
 *
 *   所以分工是：总线驱动负责**发现**，基座负责**登记**。
 *   驱动发现一个物件后调用物件侧的接入（object_plug），
 *   后者再通过 base_arrive 在基座登记一条接线。
 *
 * 也就是说：接线现在来自真实的硬件扫描，而不是测试代码里
 * 手写的一句 object_plug。
 *
 * 当前只扫描总线 0。多总线需要解析 PCI-to-PCI 桥，属于后续工作。
 */
#include "arch.h"
#include "object.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_VENDOR_ID   0x00
#define PCI_CLASS_REV   0x08
#define PCI_HEADER_TYPE 0x0C

static u32 pci_read32(u8 bus, u8 slot, u8 func, u8 off)
{
        u32 addr = 0x80000000u
                 | ((u32)bus  << 16)
                 | ((u32)(slot & 0x1Fu) << 11)
                 | ((u32)(func & 0x07u) << 8)
                 | ((u32)off & 0xFCu);

        arch_outl(PCI_CONFIG_ADDR, addr);
        return arch_inl(PCI_CONFIG_DATA);
}

/* 只打印低 digits 位十六进制，避免 0x00008086 这种噪音 */
static void put_hex(u32 v, u32 digits)
{
        for (int i = (int)(digits - 1) * 4; i >= 0; i -= 4) {
                u32 nib = (v >> i) & 0xFu;
                char c = (nib < 10) ? (char)('0' + nib) : (char)('a' + nib - 10);
                char s[2];
                s[0] = c;
                s[1] = '\0';
                arch_puts(s);
        }
}

/* 常见类别的短名，纯粹为了让串口输出好读 */
static const char *class_name(u8 cls)
{
        switch (cls) {
        case 0x00: return "unclassified";
        case 0x01: return "storage";
        case 0x02: return "network";
        case 0x03: return "display";
        case 0x04: return "multimedia";
        case 0x06: return "bridge";
        case 0x08: return "system";
        case 0x0C: return "serial-bus";
        default:   return "other";
        }
}

/*
 * 扫描总线 0 上的所有设备，把发现的物件交给物件侧接入。
 * 返回发现的物件数。
 */
u32 pci_discover(void)
{
        u32 found = 0;

        arch_puts("\n--- bus discovery (PCI) ---\n");

        for (u32 slot = 0; slot < 32; slot++) {
                for (u32 func = 0; func < 8; func++) {
                        u32 id = pci_read32(0, (u8)slot, (u8)func, PCI_VENDOR_ID);
                        u16 vendor = (u16)(id & 0xFFFFu);

                        if (vendor == 0xFFFFu)
                                continue;       /* 该位置没有设备 */

                        u16 device = (u16)((id >> 16) & 0xFFFFu);

                        u32 class_rev = pci_read32(0, (u8)slot, (u8)func, PCI_CLASS_REV);
                        u8  prog_if   = (u8)((class_rev >> 8) & 0xFFu);
                        u8  subclass  = (u8)((class_rev >> 16) & 0xFFu);
                        u8  cls       = (u8)((class_rev >> 24) & 0xFFu);

                        /*
                         * 让物件侧接入。unit/period 传 0：
                         * PCI 设备不会自报运动特性，那要等它的驱动
                         * 绑定之后才有意义。此刻它只是「发现了一个物件」。
                         */
                        wire_t w = object_plug(0, 0);

                        arch_puts("  ");
                        put_hex(slot, 2);
                        arch_puts(".");
                        put_hex(func, 1);
                        arch_puts("  ");
                        put_hex(vendor, 4);
                        arch_puts(":");
                        put_hex(device, 4);
                        arch_puts("  [");
                        arch_puts(class_name(cls));
                        arch_puts(" ");
                        put_hex(cls, 2);
                        arch_puts("/");
                        put_hex(subclass, 2);
                        if (prog_if != 0) {
                                arch_puts(" if=");
                                put_hex(prog_if, 2);
                        }
                        arch_puts("]  wire ");
                        /* 句柄 = (世代 << 16) | 槽位，打印成 槽位/世代 更好读 */
                        arch_putdec(WIRE_INDEX(w));
                        arch_puts("/");
                        arch_putdec(WIRE_GEN(w));
                        arch_puts("\n");

                        found++;

                        /* 非多功能设备只有功能 0 */
                        u32 header = pci_read32(0, (u8)slot, (u8)func, PCI_HEADER_TYPE);
                        if (func == 0 && (header & 0x00800000u) == 0)
                                break;
                }
        }

        arch_puts("  discovered ");
        arch_putdec(found);
        arch_puts(" object(s)\n");

        return found;
}
