/*
 * kmain.c — 内核入口
 *
 * 只做启动编排，不含任何语义代码：
 *   1. 基座与物件侧初始化
 *   2. 平台：IDT → PIC → PIT → 开中断
 *   3. 总线发现：接线由此自动产生
 *   4. 验证：原语语义 + 统一模型
 *
 * 语义验证在 base/verify.c —— 那是关于「模型成立与否」的检查，
 * 不该和「这台机器怎么启动」混在一起。
 */
#include "arch.h"
#include "base.h"
#include "bus.h"
#include "object.h"
#include "verify.h"

void kernel_main(u32 magic, u32 info)
{
        arch_puts("=== liteOS ===\n");

        arch_puts("[boot] magic = 0x");
        arch_puthex32(magic);
        arch_puts("\n[boot] info  = 0x");
        arch_puthex32(info);
        arch_puts("\n");

        /* ---- 1. 基座与物件侧 ---- */
        base_init();
        object_init();

        /* ---- 2. 平台 ---- */
        arch_puts("\n--- platform ---\n");
        arch_idt_init();
        arch_puts("  idt   installed (48 vectors)\n");
        arch_pic_init();
        arch_puts("  pic   remapped to 0x20/0x28, only IRQ0 open\n");
        arch_pit_init(100);
        arch_puts("  pit   100 Hz\n");

        /*
         * 开中断，并等到第一个节拍到来 —— 这样「定时器真的在跑」
         * 是被证实的事实，不是一个假设。
         */
        arch_irq_enable();
        u64 t0 = arch_ticks();
        while (arch_ticks() == t0)
                __asm__ volatile("hlt");
        arch_puts("  irq   enabled, timer ticking\n");

        /* ---- 3. 总线发现：接线由此产生 ---- */
        pci_discover();

        /* ---- 4. 验证 ---- */
        base_verify();

        /*
         * 可选的故障注入：make FAULT_TEST=1
         *
         * 碰一段恒等映射之外的内存，触发页错误，用来验证异常处理器
         * 真的会打印现场。没有这一步，「装了 IDT」和「IDT 有用」
         * 只是两个不同的说法。
         */
#ifdef FAULT_TEST
        arch_puts("\n[fault-test] touching unmapped memory at 0x40000000...\n");
        *(volatile u64 *)0x40000000ULL = 1;
        arch_puts("[fault-test] NOT REACHED: handler did not fire\n");
#endif

        arch_puts("\n[idle] halting\n");

        for (;;)
                __asm__ volatile("hlt");
}
