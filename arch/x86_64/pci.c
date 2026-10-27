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
 * 运行时热插拔：
 *
 *   启动时扫描一次（pci_discover），之后主循环周期性重扫（pci_poll），
 *   与上一次的结果比对：新出现的设备「到达」，消失的设备「离开」。
 *
 *   拔出需要客体配合：QEMU 的 PIIX4 ACPI 热插拔控制器在 0xAE00 处
 *   标记「请求拔出」的槽位，操作系统写回弹出寄存器后设备才真正移除。
 *   这里不解释 ACPI 表，只直接读写这组寄存器。
 *
 * 当前只扫描总线 0。多总线需要解析 PCI-to-PCI 桥，属于后续工作。
 */
#include "arch.h"
#include "bus.h"
#include "object.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_VENDOR_ID   0x00
#define PCI_CLASS_REV   0x08
#define PCI_HEADER_TYPE 0x0C

#define PCI_SLOTS 32
#define PCI_FUNCS 8

/* QEMU PIIX4 ACPI PCI 热插拔寄存器（hw/acpi/pcihp.c） */
#define PCIHP_BASE  0xAE00
#define PCIHP_DOWN  (PCIHP_BASE + 0x04)   /* 请求拔出的槽位位图 */
#define PCIHP_EJECT (PCIHP_BASE + 0x08)   /* 写入槽位位图即弹出 */
#define PCIHP_SEL   (PCIHP_BASE + 0x10)   /* 总线选择，总线 0 为 0 */

/*
 * 上一次扫描看到的设备。这是总线驱动自己的记录，不在基座里：
 * 基座只知道接线活着，不知道它来自哪个插槽。
 */
typedef struct {
        bool   present;
        u16    vendor;
        u16    device;
        wire_t wire;
} pci_known_t;

static pci_known_t known[PCI_SLOTS][PCI_FUNCS];

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

static void print_dev(const char *tag, u32 slot, u32 func,
                      u16 vendor, u16 device, wire_t w, bool with_class)
{
        arch_puts(tag);
        put_hex(slot, 2);
        arch_puts(".");
        put_hex(func, 1);
        arch_puts("  ");
        put_hex(vendor, 4);
        arch_puts(":");
        put_hex(device, 4);

        if (with_class) {
                u32 class_rev = pci_read32(0, (u8)slot, (u8)func, PCI_CLASS_REV);
                u8  prog_if   = (u8)((class_rev >> 8) & 0xFFu);
                u8  subclass  = (u8)((class_rev >> 16) & 0xFFu);
                u8  cls       = (u8)((class_rev >> 24) & 0xFFu);

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
                arch_puts("]");
        }

        arch_puts("  wire ");
        /* 句柄 = (世代 << 16) | 槽位，打印成 槽位/世代 更好读 */
        arch_putdec(WIRE_INDEX(w));
        arch_puts("/");
        arch_putdec(WIRE_GEN(w));
        arch_puts("\n");
}

/*
 * 扫描总线 0，与上一次结果比对。新出现的设备到达，消失或被替换的
 * 设备离开。返回本次变化的数目。
 *
 * PCI 设备不会自报运动特性，所以 unit/period 传 0：
 * 那要等它的驱动绑定之后才有意义。此刻它只是「发现了一个物件」。
 */
static u32 scan(const char *arrive_tag, const char *depart_tag)
{
        u32 changes = 0;

        for (u32 slot = 0; slot < PCI_SLOTS; slot++) {
                bool multi = false;

                for (u32 func = 0; func < PCI_FUNCS; func++) {
                        bool present = false;
                        u16  vendor = 0;
                        u16  device = 0;

                        /* 非多功能设备只有功能 0 */
                        if (func == 0 || multi) {
                                u32 id = pci_read32(0, (u8)slot, (u8)func, PCI_VENDOR_ID);
                                vendor = (u16)(id & 0xFFFFu);
                                if (vendor != 0xFFFFu) {
                                        present = true;
                                        device = (u16)((id >> 16) & 0xFFFFu);
                                        if (func == 0) {
                                                u32 h = pci_read32(0, (u8)slot, 0, PCI_HEADER_TYPE);
                                                multi = (h & 0x00800000u) != 0;
                                        }
                                }
                        }

                        pci_known_t *k = &known[slot][func];

                        if (k->present &&
                            (!present || k->vendor != vendor || k->device != device)) {
                                object_unplug(k->wire);
                                print_dev(depart_tag, slot, func,
                                          k->vendor, k->device, k->wire, false);
                                k->present = false;
                                k->wire = WIRE_NONE;
                                changes++;
                        }

                        if (present && !k->present) {
                                wire_t w = object_plug(0, 0);
                                k->present = true;
                                k->vendor = vendor;
                                k->device = device;
                                k->wire = w;
                                print_dev(arrive_tag, slot, func, vendor, device, w, true);
                                changes++;
                        }
                }
        }

        return changes;
}

u32 pci_discover(void)
{
        arch_puts("\n--- bus discovery (PCI) ---\n");

        for (u32 s = 0; s < PCI_SLOTS; s++) {
                for (u32 f = 0; f < PCI_FUNCS; f++) {
                        known[s][f].present = false;
                        known[s][f].vendor = 0;
                        known[s][f].device = 0;
                        known[s][f].wire = WIRE_NONE;
                }
        }

        u32 found = scan("  ", "  ");

        arch_puts("  discovered ");
        arch_putdec(found);
        arch_puts(" object(s)\n");

        return found;
}

/*
 * 对请求拔出的槽位执行弹出。没有热插拔控制器时端口读回全 1，忽略。
 */
static void pcihp_eject_pending(void)
{
        arch_outl(PCIHP_SEL, 0);
        u32 down = arch_inl(PCIHP_DOWN);
        if (down == 0 || down == 0xFFFFFFFFu)
                return;
        arch_outl(PCIHP_EJECT, down);
}

u32 pci_poll(void)
{
        pcihp_eject_pending();
        return scan("  [hotplug] arrive  ", "  [hotplug] depart  ");
}
