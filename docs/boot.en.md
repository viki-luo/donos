# Boot Path

English | [简体中文](boot.md)

This document records the x86_64 boot details. This is not design
philosophy (see [design.en.md](design.en.md)) but **implementation
constraints** — get any of them wrong and the kernel prints exactly one
character and stops.

---

## 1. Why PVH

QEMU's `-kernel` accepts several kernel formats. What we measured:

| Format | Result |
| --- | --- |
| 64-bit multiboot 1 | `Cannot load x86-64 image, give a 32bit one` |
| multiboot 2 | Needs an extra 32-bit header and alignment; QEMU support is limited |
| **PVH (Xen ELF note)** | Works |

PVH places a note in the ELF telling the loader where to start:

```asm
section .note.xen note alloc
        dd 4                    ; namesz
        dd 4                    ; descsz
        dd 18                   ; XEN_ELFNOTE_PHYS32_ENTRY
        db "Xen", 0
        dd pvh_entry32          ; entry address
```

That note **must live in its own `PT_NOTE` segment**. Put it in a regular
load segment and QEMU will not see it.

---

## 2. `PHYS32` means a 32-bit entry

This is the easiest trap to fall into, and it deserves its own section.

The **32 in `XEN_ELFNOTE_PHYS32_ENTRY` is not a typo**: when the CPU
enters through this note it is in **32-bit protected mode**, not long
mode.

What happens if the entry is written as 64-bit code?

On x86_64 the `REX` prefix is `0x40`–`0x4F`. In 32-bit mode those bytes
are single-byte `inc`/`dec` instructions. So:

```
48 8d 05 fe 2e 00 00   64-bit decoding: lea rax, [rip+0x2efe]  -> rax = 0x104000
                       32-bit decoding: dec eax
                                        lea eax, [0x2efe]       -> eax = 0x2efe
```

The result is that `rax` holds **the displacement itself** rather than a
computed address. The symptom: after `lea rsp, [rel stack_top]`, `rsp`
becomes a small number; after `mov cr3`, `cr3` becomes the displacement.
Nothing crashes immediately, because every instruction is being decoded
incorrectly — the behaviour is simply wrong. This class of bug is very
hard to infer from the symptoms.

**Therefore: the PVH entry must be a 32-bit stub that performs the
32→64 transition.**

---

## 3. The 32→64 transition sequence

The order matters:

```
1. CR4.PAE  = 1            enable physical address extension (prerequisite)
2. CR3      = PML4 physical address
3. EFER.LME = 1            via WRMSR to IA32_EFER
4. CR0.PG   = 1            enable paging -> long mode compatible (still 32-bit code)
5. LGDT     + far jump to an L=1 code segment -> truly 64-bit
```

Note that after step 4 and before step 5 the CPU is in **long mode
compatibility**: already in long mode, but still decoding as 32-bit. A
far jump (`jmp 0x08:entry64`) combined with an `L=1` code descriptor is
what actually switches to 64-bit.

The 64-bit code descriptor in the GDT:

```
dq 0x00AF9A000000FFFF       ; P=1 DPL=0 S=1 type=code, G=1 L=1 D=0
```

---

## 4. Page tables live at fixed low addresses

The page tables are built at fixed physical addresses rather than inside
the kernel image's `.bss`:

```
PML4  0x70000
PDPT  0x71000
PD    0x72000
```

For two reasons:

1. **Identity mapping.** The first 1 GiB is identity-mapped with 2 MiB
   pages, so virtual address = physical address. The stub writes these
   tables by physical address in 32-bit mode and can still reach them
   after switching to 64-bit.
2. **Decoupling from the `.bss` clear.** Startup code has to zero `.bss`.
   If the page tables and `.bss` share memory, a small ordering mistake
   wipes them, which looks like "paging mysteriously stopped working".

---

## 5. Writable segments must be file-backed

QEMU's PVH loader maps ELF segments according to **`FileSiz`**. A purely
`NOBITS` `.bss` in its own segment has `FileSiz = 0` — **that segment is
never mapped at all**, and writing it faults. Since there is no IDT yet,
that page fault becomes a triple fault, i.e. "it just stops silently".

So the linker script folds `.bss` into the file-backed `.data` segment:

```ld
.data : ALIGN(4K) {
        *(.data .data.*)
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
        *(.bootstack)
} :data
```

For the same reason the stack uses `times 64 * 1024 db 0` rather than
`resb`, so it has real bytes in the file.

How to check:

```sh
readelf -l liteos.bin | grep -A2 LOAD
# FileSiz of the writable segment must be non-zero
```

---

## 6. Where the stack goes

The stack sits at the **end** of the writable segment, after `__bss_end`.
Two reasons:

- The stack grows downward, so putting it last means it can never run
  into any data.
- Zeroing `.bss` at startup cannot wipe the return addresses on the
  stack.

An early bug was exactly this: the top of the stack landed on the start
of the wire table. The return address of `base_init()` was cleared the
moment it was pushed, and `ret` jumped into garbage.

---

## 7. Debugging techniques

### Boot stage markers

Off by default. Turn them on to get one letter per stage:

```sh
make BOOT_TRACE=1
```

```
'A' serial ready (32-bit stage)
'B' in 64-bit, paging live
'C' about to enter C
'D' kernel_main returned
```

`A` without `B` means the problem is in the page tables or the 32→64
transition. `C` without further output means the problem is in C code.

### Inspecting real registers with gdb

```sh
qemu-system-x86_64 -kernel liteos.bin -m 256M \
        -serial file:build/serial.log -display none \
        -no-reboot -no-shutdown -gdb tcp::1240 -S
gdb -q liteos.bin -ex 'target remote localhost:1240'
```

**Caution**: kill leftover QEMU processes before each session. Otherwise
the new instance cannot bind the port and gdb connects to the
**previous** one (running an old kernel). The registers then disagree
with the disassembly and will lead you badly astray.

### Confirming paging is actually on

Knowing that `mov cr3` executed is not enough. Read the control
registers back in C:

```c
u64 cr0, cr4, cr3;
__asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
__asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
__asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
```

Correct state (first 1 GiB identity-mapped):

```
cr0 = 0x80000011     ; PG=1 PE=1 ET=1
cr4 = 0x00000020     ; PAE=1
cr3 = 0x00070000     ; our PML4
```

If `cr3` is not `0x70000`, `mov cr3` did not take effect as expected —
first suspect that you are still in 32-bit mode (see section 2).
