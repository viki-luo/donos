; isr.asm — 中断与异常入口存根（x86_64）
;
; 每个向量一个存根，统一压入 [向量号, 错误码] 后跳到公共处理流程。
; 统一压入错误码（不产生错误码的向量补 0），这样 C 侧的栈布局一致，
; 不必为每个向量写特例。
;
; 注意：8/10/11/12/13/14/17/21/29/30 由 CPU 自动压入错误码，
; 这些存根不再补 0，否则布局会错位。
;
; 有了这套入口，异常就不会再变成「三重故障后静默停住」——
; 会明确打印向量号、错误码和出错指令地址。这一点在裸机调试里
; 价值极高。

BITS 64
DEFAULT ABS

section .text
extern isr_dispatch 

%macro ISR_NOERR 1
global isr%1
isr%1:
        push    qword 0                 ; 没有错误码的向量补 0
        push    qword %1
        jmp     isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
        push    qword %1                ; CPU 已压入错误码
        jmp     isr_common
%endmacro

; ---- 0..31：CPU 异常 ----
ISR_NOERR 0                             ; #DE 除零
ISR_NOERR 1                             ; #DB 调试
ISR_NOERR 2                             ; NMI
ISR_NOERR 3                             ; #BP 断点
ISR_NOERR 4                             ; #OF 溢出
ISR_NOERR 5                             ; #BR 越界
ISR_NOERR 6                             ; #UD 非法指令
ISR_NOERR 7                             ; #NM 设备不可用
ISR_ERR   8                             ; #DF 双重故障
ISR_NOERR 9
ISR_ERR   10                            ; #TS 非法 TSS
ISR_ERR   11                            ; #NP 段不存在
ISR_ERR   12                            ; #SS 栈故障
ISR_ERR   13                            ; #GP 通用保护
ISR_ERR   14                            ; #PF 页错误
ISR_NOERR 15
ISR_NOERR 16                            ; #MF 浮点
ISR_ERR   17                            ; #AC 对齐检查
ISR_NOERR 18                            ; #MC 机器检查
ISR_NOERR 19                            ; #XM SIMD 浮点
ISR_NOERR 20                            ; #VE 虚拟化
ISR_ERR   21                            ; #CP 控制流保护
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29                            ; #VC VMM 通信
ISR_ERR   30                            ; #SX 安全异常
ISR_NOERR 31

; ---- 32..47：8259 PIC 的 IRQ0..IRQ15 ----
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

; ---------------------------------------------------------------------
; 公共处理流程
;
; 进入时栈上依次是：
;   [rsp+0]  向量号
;   [rsp+8]  错误码
;   [rsp+16] 被打断的 rip
;   [rsp+24] cs
;   [rsp+32] rflags
;   [rsp+40] rsp（仅特权级切换时）
;   [rsp+48] ss （仅特权级切换时）
;
; 我们只保存调用者可能还在用的易失寄存器；被调用者保存寄存器
; 由 C ABI 负责。因为编译时禁用了 SSE（-mgeneral-regs-only），
; 这里不必保存 XMM，也不必保证 16 字节对齐。
; ---------------------------------------------------------------------
isr_common:
        push    rax
        push    rcx
        push    rdx
        push    rsi
        push    rdi
        push    r8
        push    r9
        push    r10
        push    r11

        ; 9 次压栈 = 72 字节，因此原来的栈顶在 +72 处
        mov     rdi, [rsp + 72]         ; 向量号
        mov     rsi, [rsp + 80]         ; 错误码
        mov     rdx, [rsp + 88]         ; 被打断的 rip
        call    isr_dispatch

        pop     r11
        pop     r10
        pop     r9
        pop     r8
        pop     rdi
        pop     rsi
        pop     rdx
        pop     rcx
        pop     rax

        add     rsp, 16                 ; 丢弃向量号与错误码
        iretq

; 48 之后未使用的向量：直接返回。
global isr_spurious
isr_spurious:
        iretq

; ---------------------------------------------------------------------
; 存根地址表：C 侧按序号取地址填 IDT，避免写 48 个 extern 声明。
; ---------------------------------------------------------------------
section .data
global isr_stub_table
align 8
isr_stub_table:
        dq isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
        dq isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
        dq isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
        dq isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
        dq isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39
        dq isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47

section .note.GNU-stack noalloc noexec nowrite progbits