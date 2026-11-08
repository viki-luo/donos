/*
 * mmu.c — x86_64 分页保护（W^X 的可用部分）
 *
 * 引导阶段（boot.asm）建的恒等映射是 512 个 2 MiB 大页，每项 0x83：
 * 存在、可写、可执行，无 NX，无只读。分页存在，但不提供保护。
 * 本文件把能装上的保护装上，并把装不上的说清楚。
 *
 * 诚实边界（不得夸大）：
 *   第 0 个大页 0..2 MiB 同时容纳 .text/.rodata/.data/.bss/栈，
 *   2 MiB 是唯一可用粒度，因此无法把代码设只读而数据设可写。
 *   由此 NX 只能标在第 1..511 个大页上：2 MiB 以上的区域不可执行，
 *   内核自己的代码页仍可执行。这不是完整的 W^X，只是一半。
 *   CR0.WP 同理：目前没有哪一页标了只读，所以它只是把位的语义接通，
 *   真正的拦截要等 4 KiB 页表阶段。
 */
#include "arch.h"

/* 页错误错误码位 */
#define PF_P     0x01u
#define PF_W     0x02u
#define PF_U     0x04u
#define PF_RSVD  0x08u
#define PF_I     0x10u

/* 2 MiB 页目录项标志 */
#define PDE_P    0x001ULL
#define PDE_RW   0x002ULL
#define PDE_PS   0x080ULL
#define PDE_NX   (1ULL << 63)

/* CR0 */
#define CR0_WP   0x00010000ULL
/* CR4 */
#define CR4_PAE  0x00000020ULL
#define CR4_PGE  0x00000080ULL
/* EFER */
#define EFER_MSR 0xC0000080u
#define EFER_NXE 0x00000800ULL

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

static u64 read_msr(u32 msr)
{
        u32 lo, hi;

        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
        return ((u64)hi << 32) | lo;
}

static void write_msr(u32 msr, u64 v)
{
        __asm__ volatile("wrmsr"
                         :
                         : "c"(msr), "a"((u32)v), "d"((u32)(v >> 32)));
}

/* 页目录本身：恒等映射的 0x72000 */
static volatile u64 *const pd = (volatile u64 *)(uintptr_t)0x72000ULL;

/*
 * 4 KiB 页表重建用的新页表，放在 0x73000（紧接 PD，仍在低 1 MiB 内）。
 *
 * 引导阶段的 2 MiB 大页是唯一可用粒度，于是代码与数据被迫同页，
 * NX 只能整块打开。这里把第 0 个大页拆成 512 个 4 KiB 页，按段赋权：
 *   .text        R|X（可执行、不可写）
 *   .rodata      R  （可读、不可写、不可执行）
 *   .data/.bss/栈 R|W（可写、不可执行）
 *   低 1 MiB     R|W（页表自身与 PVH 引导信息住在这里）
 *   其余        不存在（访问即 #PF）
 * 这才是完整 W^X：同一页永不同时可写又可执行。
 */
#define PT_FIRST_PHYS 0x73000ULL
static volatile u64 *const pt_first =
        (volatile u64 *)(uintptr_t)PT_FIRST_PHYS;

/* PTE 标志 */
#define PTE_P    0x001ULL
#define PTE_RW   0x002ULL
#define PTE_NX   (1ULL << 63)

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];

static u64 paddr_of(const char *p)
{
        return (u64)(uintptr_t)p;
}

static void reload_cr3(void)
{
        u64 cr3;

        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* CPUID：扩展叶子上界 >= 0x80000001，且 EDX[20] 置位才支持 NX */
static bool cpu_has_nx(void)
{
        u32 a, b, c, d, max;

        __asm__ volatile("cpuid" : "=a"(max), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000000u), "c"(0));
        if (max < 0x80000001u)
                return false;

        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000001u), "c"(0));
        return (d & (1u << 20)) != 0;
}

bool arch_mmu_enable_nx(void)
{
        if (!cpu_has_nx())
                return false;

        write_msr(EFER_MSR, read_msr(EFER_MSR) | EFER_NXE);
        reload_cr3();

        return (read_msr(EFER_MSR) & EFER_NXE) != 0;
}

bool arch_mmu_enable_wp(void)
{
        u64 cr0 = read_cr0();

        if ((cr0 & CR0_WP) == 0)
                write_cr0(cr0 | CR0_WP);

        return (read_cr0() & CR0_WP) != 0;
}

/*
 * 把 2 MiB 及以上区域标为不可执行。
 *
 * 第 0 项（0..2 MiB）**不动**：它含 .text、入口和中断存根，
 * 标了就会在第一条指令上取指失败。这不是保守，是唯一正确的边界。
 */
u32 arch_mmu_mark_data_nx(void)
{
        u32 marked = 0;

        for (u32 i = 1; i < 512; i++) {
                u64 e = pd[i];

                if ((e & PDE_P) == 0 || (e & PDE_PS) == 0)
                        continue;               /* 不是大页，不归这里管 */
                if ((e & PDE_NX) != 0)
                        continue;               /* 已是不可执行 */

                pd[i] = e | PDE_NX;
                marked++;
        }

        reload_cr3();
        return marked;
}

/*
 * 让某一页暂时有写权：用来验证 CR0.WP 真的起作用。
 * page_2mib 是 2 MiB 页号。返回它原来的内容，便于复原。
 */
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

/* 取页目录项当前值（只读用） */
u64 arch_mmu_entry(u32 page_2mib)
{
        return pd[page_2mib];
}

/* 精确恢复页目录项：RW/NX 等位不能被一次只读改动意外丢失。 */
void arch_mmu_set_entry(u32 page_2mib, u64 entry)
{
        pd[page_2mib] = entry;
        reload_cr3();
}

/*
 * 把第 0 个 2 MiB 大页拆成 4 KiB 页，实现完整 W^X。
 *
 * 返回值是实际建立的页数（不含「不存在」的页）。
 * 调用前必须先打开 EFER.NXE，否则 PTE 的 NX 位是保留位，置位即 #PF。
 */
u32 arch_mmu_split_kernel(void)
{
        const u64 text_lo   = paddr_of(__text_start);
        const u64 text_hi   = paddr_of(__text_end);
        const u64 rodata_lo = paddr_of(__rodata_start);
        const u64 rodata_hi = paddr_of(__rodata_end);
        const u64 data_lo   = paddr_of(__data_start);
        const u64 data_hi   = paddr_of(__data_end);
        u32 mapped = 0;

        for (u32 i = 0; i < 512; i++) {
                u64 addr = (u64)i * 0x1000ULL;
                u64 flags = 0;

                if (addr < 0x100000ULL) {
                        /* 页表自身、PVH 引导信息与 32 位引导栈都住在这里 */
                        flags = PTE_P | PTE_RW | PTE_NX;
                } else if (addr >= text_lo && addr < text_hi) {
                        flags = PTE_P;                  /* R|X：可执行不可写 */
                } else if (addr >= rodata_lo && addr < rodata_hi) {
                        flags = PTE_P | PTE_NX;         /* R：只读不可执行 */
                } else if (addr >= data_lo && addr < data_hi) {
                        flags = PTE_P | PTE_RW | PTE_NX; /* R|W：可写不可执行 */
                }

                if (flags != 0) {
                        pt_first[i] = (addr & ~0xFFFULL) | flags;
                        mapped++;
                } else {
                        pt_first[i] = 0;                /* 不存在 */
                }
        }

        /*
         * 把 PDE[0] 从 2 MiB 大页换成指向新页表的指针。
         *
         * PDE 的 RW 位管辖整棵子树：这里必须置 1，否则下级所有页一律
         * 只读。真正的写权限由每个 PTE 的 RW 位决定。
         */
        pd[0] = (PT_FIRST_PHYS & ~0xFFFULL) | PTE_P | PTE_RW;
        reload_cr3();

        return mapped;
}

/* 取第 0 个 4 KiB 页表项（自检用） */
u64 arch_mmu_pte(u32 page_4kib)
{
        if (page_4kib >= 512)
                return 0;
        return pt_first[page_4kib];
}

/* 直接把 CR0.WP 置位或清零，用于证明「位 + 权限」缺一不可 */
void arch_mmu_set_wp(bool on)
{
        u64 cr0 = read_cr0();

        if (on)
                write_cr0(cr0 | CR0_WP);
        else
                write_cr0(cr0 & ~CR0_WP);

        reload_cr3();
}
