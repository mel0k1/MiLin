# SysV -> Win64 мост EFI-вызовов: перекладка аргументов, shadow space.
        .text

        .section .text.start,"ax"
        .globl _start
_start:
        sub $8, %rsp
        mov %rcx, %rdi
        mov %rdx, %rsi
        call efi_main
        add $8, %rsp
        ret

        .text
        .globl efi_call1
efi_call1:
        push %rbp
        mov %rsi, %rcx
        sub $32, %rsp
        call *%rdi
        add $32, %rsp
        pop %rbp
        ret

        .globl efi_call2
efi_call2:
        push %rbp
        mov %rsi, %rcx
        sub $32, %rsp
        call *%rdi
        add $32, %rsp
        pop %rbp
        ret

        .globl efi_call3
efi_call3:
        push %rbp
        mov %rcx, %r8
        mov %rsi, %rcx
        sub $32, %rsp
        call *%rdi
        add $32, %rsp
        pop %rbp
        ret

        .globl efi_call4
efi_call4:
        push %rbp
        mov %r8, %r9
        mov %rcx, %r8
        mov %rsi, %rcx
        sub $32, %rsp
        call *%rdi
        add $32, %rsp
        pop %rbp
        ret

        .globl efi_call5
efi_call5:
        push %rbp
        sub $48, %rsp
        mov %r9, 32(%rsp)
        mov %r8, %r9
        mov %rcx, %r8
        mov %rsi, %rcx
        call *%rdi
        add $48, %rsp
        pop %rbp
        ret

        .globl efi_call6
efi_call6:
        push %rbp
        sub $48, %rsp
        mov %r9, 32(%rsp)
        mov 64(%rsp), %r10
        mov %r10, 40(%rsp)
        mov %r8, %r9
        mov %rcx, %r8
        mov %rsi, %rcx
        call *%rdi
        add $48, %rsp
        pop %rbp
        ret
