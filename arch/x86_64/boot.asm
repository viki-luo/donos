; boot.asm — x86_64 引导入口（PVH 模式）
;
; 引导路径：QEMU 读到 ELF 里的 XEN_ELFNOTE_PHYS32_ENTRY note，
; 直接跳进 pvh_entry，此时已在长模式。
;
; 本文件带分步标记，便于定位失败点：
;   'A' 串口就绪
;   'B' 页表建立完成
;   'C' 即将进入 C
; 若只看到 A 就断，问题在 setup_paging。

BITS 64
DEFAULT ABS

%define COM1 0x3F8

; ---------------------------------------------------------------------
; PVH 入口 note —— 必须落在 PT_NOTE 段
; ---------------------------------------------------------------------
section .note.xen note alloc
align 4
        dd 4                    ; namesz
        dd 4                    ; descsz
        dd 18                   ; XEN_ELFNOTE_PHYS32_ENTRY
        db "Xen", 0
align 4
        dd pvh_entry            ; 入口地址
align 4

; ---------------------------------------------------------------------
section .bss
align 16
stack_bottom:
        resb 64 * 1024
stack_top:

align 4096
pml4:   resq 512
pdpt:   resq 512
pd:     resq 512

; ---------------------------------------------------------------------
section .text
global _start
global pvh_entry
extern kernel_main

; 输出 al 中的字符
putc:
        push    rax
        push    rdx
.wait:
        mov     dx, COM1 + 5
        in      al, dx
        test    al, 0x20
        jz      .wait
        pop     rdx
        pop     rax
        push    rdx
        mov     dx, COM1
        out     dx, al
        pop     rdx
        ret

_start:
pvh_entry:
        cli
        cld

        lea     rsp, [rel stack_top]
        xor     rbp, rbp

        mov     r12, rbx                ; PVH start_info 指针

        ; ---- 串口初始化（115200 8N1）----
        xor     eax, eax
        mov     dx, COM1 + 1
        out     dx, al                  ; 关中断

        mov     al, 0x80
        mov     dx, COM1 + 3
        out     dx, al                  ; DLAB

        mov     al, 0x01
        mov     dx, COM1 + 0
        out     dx, al                  ; 除数低字节 = 1
        xor     eax, eax
        mov     dx, COM1 + 1
        out     dx, al                  ; 除数高字节 = 0

        mov     al, 0x03
        mov     dx, COM1 + 3
        out     dx, al                  ; 8N1

        mov     al, 0xC7
        mov     dx, COM1 + 2
        out     dx, al                  ; FIFO

        mov     al, 0x0B
        mov     dx, COM1 + 4
        out     dx, al                  ; DTR/RTS/OUT2

        mov     al, 'A'
        call    putc

        ; ---- 页表 ----
        call    setup_paging

        mov     al, 'B'
        call    putc

        ; ---- 进 C ----
        mov     al, 'C'
        call    putc

        mov     rdi, r12
        call    kernel_main

        mov     al, 'D'
        call    putc

.hang:
        hlt
        jmp     .hang

; ---------------------------------------------------------------------
; setup_paging — 恒等映射前 1 GiB（2 MiB 大页）
; ---------------------------------------------------------------------
setup_paging:
        push    rdi
        push    rcx
        push    rax

        lea     rdi, [rel pml4]
        xor     eax, eax
        mov     ecx, 3 * 512
        rep     stosq                   ; 三张表清零

        lea     rcx, [rel pml4]
        lea     rax, [rel pdpt]
        or      rax, 0x03
        mov     [rcx], rax              ; PML4[0] -> PDPT

        lea     rcx, [rel pdpt]
        lea     rax, [rel pd]
        or      rax, 0x03
        mov     [rcx], rax              ; PDPT[0] -> PD

        lea     rdi, [rel pd]
        mov     rax, 0x83               ; present | writable | PS
        mov     ecx, 512
.fill:
        mov     [rdi], rax
        add     rax, 0x200000
        add     rdi, 8
        loop    .fill

        lea     rax, [rel pml4]
        mov     cr3, rax

        pop     rax
        pop     rcx
        pop     rdi
        ret

section .note.GNU-stack noalloc noexec nowrite progbits