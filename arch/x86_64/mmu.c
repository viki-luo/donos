/*
 * mmu.c — x86_64 分页保护的探测辅助（仅自检构建）
 *
 * 「装保护」在 mmu_enable.c，无条件编入发布镜像；
 * 这里只放**故意违约**所需的修改与读取原语，只在 LITEOS_SELFTEST=1
 * 时参与链接。发布镜像里不存在这些符号。
 */
#include "arch.h"

/* 页目录项标志 */
#define PDE_RW   0x002ULL

/* CR0 */
#define CR0_WP   0x00010000ULL

static u64 read_cr0(void)
{
        u64 v;

        __asm__ volatile("mov %%cr0, %0" : "=r"(v));
        return v;
}

static void write_cr0(u64 v)
{
        __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static volatile u64 *const pd = (volatile u64 *)(uintptr_t)0x72000ULL;
static volatile u64 *const pt_first =
        (volatile u64 *)(uintptr_t)0x73000ULL;

static void reload_cr3(void)
{
        u64 cr3;

        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* 读某一 2 MiB 页的页目录项。 */
u64 arch_mmu_entry(u32 page_2mib)
{
        return pd[page_2mib];
}

/* 精确恢复页目录项并刷新地址翻译。 */
void arch_mmu_set_entry(u32 page_2mib, u64 entry)
{
        pd[page_2mib] = entry;
        reload_cr3();
}

/* 改某一 2 MiB 页的可写位，返回该项原值（用于复原）。 */
u64 arch_mmu_set_writable(u32 page_2mib, bool writable)
{
        u64 old = pd[page_2mib];

        if (writable)
                pd[page_2mib] = old | PDE_RW;
        else
                pd[page_2mib] = old & ~PDE_RW;

        reload_cr3();
        return old;
}

/* 读第 0 个 4 KiB 页表项（page_4kib 为 0..511）。 */
u64 arch_mmu_pte(u32 page_4kib)
{
        if (page_4kib >= 512)
                return 0;
        return pt_first[page_4kib];
}

/* 强行拨动 CR0.WP，用于证明「权限位 + 保护位」缺一不可。 */
void arch_mmu_set_wp(bool on)
{
        u64 cr0 = read_cr0();

        if (on)
                write_cr0(cr0 | CR0_WP);
        else
                write_cr0(cr0 & ~CR0_WP);

        reload_cr3();
}
