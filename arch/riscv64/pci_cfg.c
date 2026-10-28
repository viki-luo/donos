/*
 * pci_cfg.c — riscv64 的 PCI 配置空间访问（QEMU virt，ECAM）
 *
 * QEMU virt 的 PCIe 主桥（gpex）把配置空间映射在 0x30000000：
 *     地址 = 基址 + (bus << 20) | (slot << 15) | (func << 12) | off
 * 分页关闭，物理地址直接访问。
 */
#include "arch.h"
#include "bus.h"

#define PCI_ECAM_BASE 0x30000000UL

u32 arch_pci_read32(u8 bus, u8 slot, u8 func, u8 off)
{
        unsigned long addr = PCI_ECAM_BASE
                           | ((unsigned long)bus << 20)
                           | ((unsigned long)(slot & 0x1Fu) << 15)
                           | ((unsigned long)(func & 0x07u) << 12)
                           | ((unsigned long)off & 0xFCu);

        return *(volatile u32 *)addr;
}

/* virt 的根总线不支持热插拔弹出，这里什么都不做。 */
void arch_pci_eject_pending(void)
{
}
