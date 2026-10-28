/*
 * pci_cfg.c — x86_64 的 PCI 配置空间访问与热插拔弹出
 *
 * 配置空间走端口 I/O（0xCF8 / 0xCFC，配置机制 #1）。
 * 弹出走 QEMU PIIX4 ACPI 热插拔寄存器（hw/acpi/pcihp.c），不解析 ACPI 表。
 */
#include "arch.h"
#include "bus.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCIHP_BASE  0xAE00
#define PCIHP_DOWN  (PCIHP_BASE + 0x04)   /* 请求拔出的槽位位图 */
#define PCIHP_EJECT (PCIHP_BASE + 0x08)   /* 写入槽位位图即弹出 */
#define PCIHP_SEL   (PCIHP_BASE + 0x10)   /* 总线选择，总线 0 为 0 */

u32 arch_pci_read32(u8 bus, u8 slot, u8 func, u8 off)
{
        u32 addr = 0x80000000u
                 | ((u32)bus  << 16)
                 | ((u32)(slot & 0x1Fu) << 11)
                 | ((u32)(func & 0x07u) << 8)
                 | ((u32)off & 0xFCu);

        arch_outl(PCI_CONFIG_ADDR, addr);
        return arch_inl(PCI_CONFIG_DATA);
}

/* 没有热插拔控制器时端口读回全 1，忽略。 */
void arch_pci_eject_pending(void)
{
        arch_outl(PCIHP_SEL, 0);
        u32 down = arch_inl(PCIHP_DOWN);
        if (down == 0 || down == 0xFFFFFFFFu)
                return;
        arch_outl(PCIHP_EJECT, down);
}
