/* SPDX-License-Identifier: GPL-2.0-only */
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

/*
 * Real permission probes, not page-table-bit assertions:
 * - ro_probe_word is placed in .rodata (R, no W).
 * - nx_probe_word is placed in writable .data/.bss (RW, no X).
 */
static const u64 ro_probe_word = 0x524f5f50524f4245ULL;
static u32 nx_probe_word __attribute__((aligned(4))) = 0x00008067u;

static bool verify_fault_result(const char *what, u64 want_cause,
                                u64 want_sepc, u64 want_stval)
{
        u64 cause = 0;
        u64 sepc = 0;
        u64 stval = 0;
        bool seen = arch_fault_result(&cause, &sepc, &stval);
        bool ok = seen && cause == want_cause && sepc == want_sepc &&
                  stval == want_stval;

        arch_puts("  note: ");
        arch_puts(what);
        arch_puts(" scause=0x");
        arch_puthex64(cause);
        arch_puts(" sepc=0x");
        arch_puthex64(sepc);
        arch_puts(" stval=0x");
        arch_puthex64(stval);
        arch_puts("\n");
        return ok;
}

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

        /*
         * Fault injection #1: a real store into an R-only page. The trap
         * dispatcher must observe store page fault (15), exact sepc/stval,
         * replace sepc with the exported resume label, and return normally.
         */
        const u64 ro_before = ro_probe_word;
        arch_fault_expect(15u,
                          (u64)(uintptr_t)arch_probe_store_fault,
                          (u64)(uintptr_t)&ro_probe_word,
                          (u64)(uintptr_t)arch_probe_store_resume);
        arch_probe_store_ro((void *)(uintptr_t)&ro_probe_word);
        mcheck("hardware blocks a store to an R-only page (scause 15)",
               verify_fault_result("store/R-only", 15u,
                                   (u64)(uintptr_t)arch_probe_store_fault,
                                   (u64)(uintptr_t)&ro_probe_word));
        mcheck("the rejected store leaves read-only data unchanged",
               ro_probe_word == ro_before);

        /* Fault injection #2: execute from the RW/NX data page. */
        arch_fault_expect(12u,
                          (u64)(uintptr_t)&nx_probe_word,
                          (u64)(uintptr_t)&nx_probe_word,
                          (u64)(uintptr_t)arch_probe_exec_resume);
        arch_probe_exec_nx((void *)(uintptr_t)&nx_probe_word);
        mcheck("hardware blocks instruction fetch from an RW/NX page (scause 12)",
               verify_fault_result("fetch/RW-NX", 12u,
                                   (u64)(uintptr_t)&nx_probe_word,
                                   (u64)(uintptr_t)&nx_probe_word));
        mcheck("normal RX execution resumes after both expected faults", true);

        arch_puts("\n  mmu failures = ");
        arch_putdec((u64)mfail);
        arch_puts("\n");

        return mfail;
}
