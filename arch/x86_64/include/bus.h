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

#endif /* LITEOS_BUS_H */
