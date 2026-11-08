/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * bus.h — 总线驱动接口
 *
 * 总线驱动只做一件事：发现物件，并把发现结果交给物件侧接入。
 * 它不碰运动控制，也不认识参照系。
 */
#ifndef LITEOS_BUS_H
#define LITEOS_BUS_H

#include "types.h"

/*
 * 扫描 PCI 总线，把发现的物件接入。
 * 返回本次发现的物件数。
 */
u32 pci_discover(void);

/*
 * 重扫 PCI 总线并与上一次结果比对：新设备到达，消失的设备离开。
 * 先对热插拔控制器标记为「请求拔出」的槽位执行弹出。
 * 返回本次变化的数目。
 */
u32 pci_poll(void);

/* ---- 由各架构提供 ---- */

/* 读 PCI 配置空间一个 32 位字。不存在的设备读回全 1。 */
u32 arch_pci_read32(u8 bus, u8 slot, u8 func, u8 off);

/* 对热插拔控制器标记为「请求拔出」的槽位执行弹出；无控制器时什么都不做。 */
void arch_pci_eject_pending(void);

#endif /* LITEOS_BUS_H */
