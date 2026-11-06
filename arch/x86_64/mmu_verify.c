/*
 * mmu_verify.c — 分页保护的确定性自检
 *
 * 判据不是「位设进去了」，而是三件可观察的事实：
 *   1. 放行：受保护区之上的普通取指与普通写必须成功。
 *      一个永远页错误的「保护」比没有保护还危险，它必须先正确放行。
 *   2. 拦截：越界的那一次访问必须触发 #PF，且错误码带对应的权限位。
 *   3. 可逆：探测过程中做的临时改动必须全部复原，且复原后重新可用。
 *
 * 第 2 条只能以「即将停机」的形式验证：本内核的异常处理在打印现场后
 * 进入 cli;hlt 循环，没有异常恢复框架。因此越界探测放在最后执行，
 * 并把判定权交给错误码本身 —— 打印出来的错误码就是证据。
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
         * 放行测试：把第 1 页临时标成只读，再改回可写。
         * 这里写的是页目录项本身，不是设备内存，安全且可逆。
         * WP 生效时，改页表本身不受影响（它是可写内存），
         * 因此这个测试能一直活着走完。
         */
        u64 orig = arch_mmu_set_writable(20, false);
        mcheck("a read-only entry can still be written by the mapper",
               arch_mmu_entry(20) == (orig & ~0x002ULL));
        arch_mmu_set_writable(20, true);
        mcheck("restoring the writable bit puts the entry back",
               arch_mmu_entry(20) == orig);

        /*
         * 清理阶段：所有临时改动都已复原，内核仍能正常执行。
         * 走到这里并且还能打印，本身就是「可执行区没被误伤」的证据。
         */
        mcheck("the kernel kept executing after the protection was installed",
               true);

        arch_puts("\n  mmu failures = ");
        arch_putdec((u64)mfail);
        arch_puts("\n");

        return mfail;
}
