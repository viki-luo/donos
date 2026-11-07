; mmu_probe.asm — recoverable #PF probes (x86_64)
;
; The #PF dispatcher validates the exact error code, saved RIP and CR2 before
; replacing saved RIP with an exported resume label. The store is skipped on
; recovery. The NX probe uses JMP rather than CALL so no extra return address
; is pushed when instruction fetch faults.
BITS 64
DEFAULT ABS
section .text

global arch_probe_write
global arch_probe_write_fault
global arch_probe_write_resume
arch_probe_write:
arch_probe_write_fault:
        mov     byte [rdi], 0xA5
arch_probe_write_resume:
        ret

global arch_probe_exec
global arch_probe_exec_resume
arch_probe_exec:
        jmp     rdi
arch_probe_exec_resume:
        ret

section .note.GNU-stack noalloc noexec nowrite progbits
