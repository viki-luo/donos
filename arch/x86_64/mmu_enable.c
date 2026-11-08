/*
 * mmu_enable.c — x86_64 分页保护：装上去，不做探测
 *
 * 这个文件只做一件事：把能装的保护装上。它**不**做任何故障注入，
 * 因此必须无条件编入发布镜像 —— 保护不是测试，测试才是。
 *
 * 探测（真实违规访问、与期望错误码比对）住在 mmu_verify.c，只在
 * LITEOS_SELFTEST=1 时编入。
 */
#include "arch.h"

/* CR0 */
#define CR0_WP   0x00010000ULL
/* EFER */
#define EFER_MSR 0xC0000080u
#define EFER_NXE 0x00000800ULL

/* 页目录项标志 */
#define PDE_P    0x001ULL
#define PDE_RW   0x002ULL
#define PDE_PS   0x080ULL
#define PDE_NX   (1ULL << 63)

/* 4 KiB 页表项标志 */
#define PTE_P    0x001ULL
#define PTE_RW   0x002ULL
#define PTE_NX   (1ULL << 63)

/* 引导阶段建的页目录（恒等映射）与新建的 4 KiB 页表 */
static volatile u64 *const pd = (volatile u64 *)(uintptr_t)0x72000ULL;
#define PT_FIRST_PHYS 0x73000ULL
static volatile u64 *const pt_first =
        (volatile u64 *)(uintptr_t)PT_FIRST_PHYS;

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];

static u64 paddr_of(const char *p)
{
        return (u64)(uintptr_t)p;
}

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
                        flags = PTE_P;                  /* R|X */
                } else if (addr >= rodata_lo && addr < rodata_hi) {
                        flags = PTE_P | PTE_NX;         /* R */
                } else if (addr >= data_lo && addr < data_hi) {
                        flags = PTE_P | PTE_RW | PTE_NX; /* R|W */
                }

                if (flags != 0) {
                        pt_first[i] = (addr & ~0xFFFULL) | flags;
                        mapped++;
                } else {
                        pt_first[i] = 0;
                }
        }

        /*
         * PDE 的 RW 位管辖整棵子树，必须置 1；真正的写权限由每个 PTE 决定。
         */
        pd[0] = (PT_FIRST_PHYS & ~0xFFFULL) | PDE_P | PDE_RW;
        reload_cr3();

        return mapped;
}

u32 arch_mmu_mark_data_nx(void)
{
        u32 marked = 0;

        for (u32 i = 1; i < 512; i++) {
                u64 e = pd[i];

                if ((e & PDE_P) == 0 || (e & PDE_PS) == 0)
                        continue;
                if ((e & PDE_NX) != 0)
                        continue;

                pd[i] = e | PDE_NX;
                marked++;
        }

        reload_cr3();
        return marked;
}

/*
 * 装保护：唯一入口。发布镜像与自检镜像都走这一条路径，
 * 因此「自检通过」与「发布镜像真的受保护」是同一件事。
 */
bool arch_mmu_enable(void)
{
        if (!arch_mmu_enable_nx())
                return false;           /* 无 NX 就无法做 W^X，如实报告 */

        arch_mmu_split_kernel();
        arch_mmu_mark_data_nx();
        arch_mmu_enable_wp();

        return true;
}
