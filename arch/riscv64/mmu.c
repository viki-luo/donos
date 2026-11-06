/*
 * mmu.c — riscv64 Sv39 page tables
 *
 * x86_64 runs into the 2 MiB large-page granularity, so code and data share a
 * page and only "above 2 MiB is non-executable" is achievable. Sv39 has 4 KiB
 * pages, so code and data pages are naturally separate and the other half of
 * W^X becomes possible:
 *
 *    .text        R|X   executable, not writable
 *    .rodata      R     read-only, not executable
 *    .data/.bss   R|W   writable, not executable
 *    stack        R|W   writable, not executable
 */
#include "arch.h"

#define PTE_V 0x001ULL
#define PTE_R 0x002ULL
#define PTE_W 0x004ULL
#define PTE_X 0x008ULL
#define PTE_A 0x040ULL
#define PTE_D 0x080ULL

#define SATP_SV39 8ULL

#define PT_ALIGN __attribute__((aligned(4096)))

/* The kernel sits at 0x80200000, so the map starts at 0x80000000 (index 2). */
#define KERNEL_BASE 0x80000000ULL
#define TWO_MIB     0x200000ULL

extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __data_end[];
extern char __stack_bottom[], __stack_top[];

/*
 * 设备窗口直接用 2 MiB 大页在二级（L1）处映射，不另设叶表。
 *
 * 为什么不能只靠 PMP：PMP 与分页是两道独立检查，PMP 放行不等于
 * 分页放行。satp 打开后访问 0x10000000 若无页表项，第一次 load
 * 就触发 load_page_fault（QEMU 日志 tval=0x10000005 即 UART+5）。
 *
 * 大页 PTE 的 PPN 在 bit 10..53，与 4 KiB 叶项完全相同，只有低位
 * 9 位在物理地址上恒为 0（2 MiB 对齐）。旧版曾写成
 * (pa >> 21) << 10，PPN 被左移不足，UART 被映射到 0x80000，
 * 这是切换后静默的真因。
 */
#define MMIO_UART_BASE  0x10000000ULL
#define MMIO_PCIE_BASE  0x30000000ULL

static u64 root_pt[512] PT_ALIGN;
static u64 mid_pt[512] PT_ALIGN;
static u64 leaf_a[512] PT_ALIGN;
static u64 leaf_b[512] PT_ALIGN;

static u64 paddr(const void *p)
{
        return (u64)(uintptr_t)p;
}

/* A leaf entry carries the PPN in bits 10..53 and the flags in bits 0..9. */
static u64 leaf(u64 pa, u64 flags)
{
        return ((pa >> 12) << 10) | flags | PTE_A | PTE_D | PTE_V;
}

/* A branch entry points at the next level table and carries only V. */
static u64 branch(u64 pa)
{
        return ((pa >> 12) << 10) | PTE_V;
}

/* 2 MiB 大页项：PPN 字段与普通四级叶项完全相同，只有 PTE 对齐要求不同。 */
static u64 megapage(u64 pa, u64 flags)
{
        return ((pa >> 12) << 10) | flags | PTE_A | PTE_D | PTE_V;
}

/* 写 satp 是 TLB 失效点；需要能串行取指才算生效。 */
static void write_satp(u64 satp)
{
        __asm__ volatile(
                "csrw satp, %0\n\t"
                "sfence.vma zero, zero" : : "r"(satp) : "memory");
}

/* Which of the four regions does a page base address belong to? */
static u64 flags_for(u64 page)
{
        if (page >= paddr(__text_start) && page < paddr(__text_end))
                return PTE_R | PTE_X;
        if (page >= paddr(__rodata_start) && page < paddr(__rodata_end))
                return PTE_R;
        return PTE_R | PTE_W;
}

int arch_mmu_init(void)
{
        const u64 kernel_l2 = (KERNEL_BASE >> 21) & 0x1FFu;

        for (u32 i = 0; i < 512; i++) {
                root_pt[i] = 0;
                mid_pt[i] = 0;
                leaf_a[i] = 0;
                leaf_b[i] = 0;
        }

        /*
         * Sv39 三级：root[2] 覆盖 0x80000000..0xFFFFFFFF（1 GiB），
         * root[0] 覆盖 0x00000000..0x3FFFFFFF（同 1 GiB）。
         * mid_pt 是两段共有的二级表；L2 索引由虚拟地址决定：
         *   kernel     (0x80200000>>21)&0x1FF = 1 与 2
         *   uart       (0x10000000>>21)&0x1FF = 0x80
         *   pcie ecam  (0x30000000>>21)&0x1FF = 0x180
         */
        root_pt[(KERNEL_BASE >> 30) & 0x1FFu] = branch(paddr(mid_pt));
        root_pt[MMIO_UART_BASE >> 30] = branch(paddr(mid_pt));

        mid_pt[kernel_l2] = branch(paddr(leaf_a));
        mid_pt[kernel_l2 + 1u] = branch(paddr(leaf_b));

        /*
         * 设备只用 L1 的一个 2 MiB 大页项。UART 在 0x10000000、
         * PCIe ECAM 在 0x30000000，都落在 root[0] 的 1 GiB 区间里，
         * 所以它们落在 mid_pt。上面已把 root[0] 接到同一张 mid_pt，
         * 这里直接写大页项就是本级叶片，无需再设叶表。
         * 若将来要分别映射一整个 MMIO 窗口，就必须为
         * UART 与 PCIe 拆成各自的叶表，不能共用一张表。
         */
        mid_pt[(MMIO_UART_BASE >> 21) & 0x1FFu] =
                megapage(MMIO_UART_BASE, PTE_R | PTE_W);
        mid_pt[(MMIO_PCIE_BASE >> 21) & 0x1FFu] =
                megapage(MMIO_PCIE_BASE, PTE_R | PTE_W);

        for (u32 i = 0; i < 1024; i++) {
                u64 page = KERNEL_BASE + (u64)i * 4096ULL;
                u64 *tbl = i < 512 ? leaf_a : leaf_b;

                tbl[i & 0x1FFu] = leaf(page, flags_for(page));
        }

        /*
         * Sv39 satp: MODE in bits 60..63, ASID in 44..59, root PPN in 0..43.
         * 写完后当前 PC 的下一条取指即走新表；新表必须覆盖当前代码、
         * 栈、页表自身和 UART，不遗漏任何一条。
         */
        write_satp((SATP_SV39 << 60) | (paddr(root_pt) >> 12));
        return 0;
}

u64 arch_satp(void)
{
        u64 v;

        __asm__ volatile("csrr %0, satp" : "=r"(v));
        return v;
}

/*
 * 走一遍真正的三级查找，把某一页的权限位从表里读回来。
 *
 * page 是从 KERNEL_BASE 起算的 4 KiB 页号。二级索引要按
 * 「页号 / 512」算：每张叶表只覆盖 512 页（2 MiB）。
 */
u64 arch_mmu_walk(u64 page)
{
        u64 root = (arch_satp() & 0xFFFFFFFFFFFULL) << 12;
        u64 l1_idx = (KERNEL_BASE >> 30) & 0x1FFu;
        u64 l2_idx = ((KERNEL_BASE >> 21) + (page / 512)) & 0x1FFu;
        volatile u64 *root_tbl = (volatile u64 *)(uintptr_t)root;
        volatile u64 *mid_tbl;
        volatile u64 *l0_tbl;

        mid_tbl = (volatile u64 *)(uintptr_t)((root_tbl[l1_idx] >> 10) << 12);
        l0_tbl = (volatile u64 *)(uintptr_t)((mid_tbl[l2_idx] >> 10) << 12);

        return l0_tbl[page % 512] & (PTE_R | PTE_W | PTE_X);
}

void arch_mmu_walk3(u64 out[3])
{
        out[0] = arch_mmu_walk(((u64)(uintptr_t)__text_start -
                                KERNEL_BASE) >> 12);
        out[1] = arch_mmu_walk(((u64)(uintptr_t)__rodata_start -
                                KERNEL_BASE) >> 12);
        out[2] = arch_mmu_walk(((u64)(uintptr_t)__data_start -
                                KERNEL_BASE) >> 12);
}
