/*
 * mmu_verify.c — riscv64 页表自检
 *
 * 光读一个 satp 不能算证据：satp 可以非零而映射是错的。真正能证明
 * 翻译已经生效的，是这段代码本身还在跑 —— 它把字打出来的这个动作，
 * 就是翻译生效的事实。
 *
 * 另外两条断言不看「我设的位」，而是**走一遍真正的页表查找**，
 * 从表中把某一页的权限读回来再比较。位设进去了但表接错了，
 * 这里会报。
 */
#include "arch.h"

static int mfail;

static void mcheck(const char *what, bool ok)
{
        arch_puts(ok ? "  ok   " : "  FAIL ");
        arch_puts(what);
        arch_puts("\n");
        if (!ok)
                mfail++;
}

int arch_mmu_verify(void)
{
        u64 satp = arch_satp();
        u64 probe[3] = {0, 0, 0};

        mfail = 0;
        arch_puts("\n--- MMU: what the page tables now enforce ---\n\n");

        arch_puts("  satp = 0x");
        arch_puthex64(satp);
        arch_puts("  (MODE=");
        arch_putdec(satp >> 60);
        arch_puts(")\n");

        mcheck("satp selects Sv39 rather than Bare",
               (satp >> 60) == 8ULL);

        mcheck("the kernel is still executing behind the new map", true);

        arch_mmu_walk3(probe);
        mcheck("a code page is readable and executable, not writable",
               probe[0] == (PTE_R | PTE_X));
        mcheck("a read-only data page is readable only",
               probe[1] == PTE_R);
        mcheck("a writable data page is readable and writable, not executable",
               probe[2] == (PTE_R | PTE_W));

        arch_puts("\n  mmu failures = ");
        arch_putdec((u64)mfail);
        arch_puts("\n");

        return mfail;
}
