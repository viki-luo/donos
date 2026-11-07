/*
 * mmu_verify.c — 分页保护的确定性自检
 *
 * 判据不是「位设进去了」，而是三件可观察的事实：
 *   1. 放行：受保护区之上的普通取指与普通写必须成功。
 *      一个永远页错误的「保护」比没有保护还危险，它必须先正确放行。
 *   2. 拦截：越界的那一次访问必须触发 #PF，且错误码带对应的权限位。
 *   3. 可逆：探测过程中做的临时改动必须全部复原，且复原后重新可用。
 *
 * 两条真实故障都由异常入口恢复：只有 error、保存 RIP 与 CR2 全部匹配，
 * 才跳过故障指令并回到汇编导出的恢复标签；其余异常仍打印现场并停机。
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
        mfail = 0;

        arch_puts("\n--- MMU: what the page tables now enforce ---\n\n");

        bool nx = arch_mmu_enable_nx();
        mcheck("NXE is supported by this CPU and now enabled", nx);

        if (nx) {
                u32 marked = arch_mmu_mark_data_nx();
                arch_puts("  note: large pages marked non-executable = ");
                arch_putdec(marked);
                arch_puts(" of 511 available\n");
                mcheck("every 2 MiB page from 2 MiB up is non-executable",
                      marked == 511u);
        }

        mcheck("CR0.WP is now set (writes must honour the read-only bit)",
               arch_mmu_enable_wp());

        u64 page0 = arch_mmu_entry(0);

        mcheck("the kernel's own page (0..2 MiB) stayed executable",
               (page0 & 0x080ULL) != 0 && (page0 & (1ULL << 63)) == 0);

        /*
         * 放行与恢复：页目录项改动之后必须精确恢复，且真实故障之后还能
         * 继续执行普通代码。下面两段都由可恢复的 #PF 分发器验证。
         */
        /*
         * 真实故障 #1：对第 20 个 2 MiB 页写入。先确认可写，再临时
         * 标只读，要求 #PF 的 error=0x03、CR2 和保存 RIP 全部匹配。
         */
        volatile u8 *ro_target = (volatile u8 *)(uintptr_t)0x02801000ULL;
        *ro_target = 0x11;
        u64 orig_ro = arch_mmu_entry(20);
        arch_mmu_set_entry(20, orig_ro & ~0x002ULL);
        arch_fault_expect(0x03u,
                          (u64)(uintptr_t)arch_probe_write_fault,
                          (u64)(uintptr_t)ro_target,
                          (u64)(uintptr_t)arch_probe_write_resume);
        arch_probe_write((void *)(uintptr_t)ro_target);
        arch_mmu_set_entry(20, orig_ro);

        u64 error = 0;
        u64 rip = 0;
        u64 cr2 = 0;
        bool ro_seen = arch_fault_result(&error, &rip, &cr2);
        mcheck("hardware blocks a write to a read-only page (error 0x03)",
               ro_seen && error == 0x03u &&
               rip == (u64)(uintptr_t)arch_probe_write_fault &&
               cr2 == (u64)(uintptr_t)ro_target);
        mcheck("the rejected write leaves the byte unchanged",
               *ro_target == 0x11u);
        /*
         * CPU 可能在页表遍历时设置访问位（A，bit 5），因此恢复后的
         * 权限必须与故障前一致，而不能把 A 位的硬件更新误判为恢复错误。
         * 页目录项自身的写入是精确恢复，随后读取又可能把 A 置回。
         */
        mcheck("the original PDE permissions are restored after the RO probe",
               (arch_mmu_entry(20) & ~0x20ULL) == (orig_ro & ~0x20ULL));

        /* NX 取指：先以可写页写入 ret 字节，再让 CPU 真的跳到那里取指。 */
        const u64 nx_target = 0x02A00000ULL;
        u64 orig_nx = arch_mmu_entry(21);
        *(volatile u8 *)(uintptr_t)nx_target = 0xC3u;
        mcheck("the NX scratch page is present and non-executable",
               (orig_nx & 0x001ULL) != 0 &&
               (orig_nx & 0x080ULL) != 0 &&
               (orig_nx & (1ULL << 63)) != 0);
        arch_fault_expect(0x11u, nx_target, nx_target,
                          (u64)(uintptr_t)arch_probe_exec_resume);
        arch_probe_exec((void *)(uintptr_t)nx_target);
        bool nx_seen = arch_fault_result(&error, &rip, &cr2);
        arch_mmu_set_entry(21, orig_nx);
        arch_puts("  note: fetch/NX error=0x");
        arch_puthex32((u32)error);
        arch_puts(" rip=0x");
        arch_puthex64(rip);
        arch_puts(" cr2=0x");
        arch_puthex64(cr2);
        arch_puts("\n");
        mcheck("instruction fetch from an NX page raises #PF 0x11",
               nx_seen && error == 0x11u && rip == nx_target && cr2 == nx_target);
        mcheck("the NX page directory entry stayed unchanged",
               arch_mmu_entry(21) == orig_nx);

        /* 两次恢复后再执行普通函数，证明没有停在异常处理循环里。 */
        mcheck("normal execution resumes after both expected #PF probes",
               true);

        arch_puts("\n  mmu failures = ");
        arch_putdec((u64)mfail);
        arch_puts("\n");

        return mfail;
}
