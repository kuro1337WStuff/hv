option casemap:none

EXTERN L1OwnedMain:PROC

.data?
ALIGN 16
L1Idt BYTE 4096 DUP (?)

.data
L1Idtr WORD 4095
       QWORD 0

.code
; Prepare, but do not load, a private fail-stop IDT while still in EFI.
PUBLIC L1PrepareIdt
L1PrepareIdt PROC
    lea rdx, L1Idt
    mov qword ptr [L1Idtr+2], rdx
    lea r8, L1Halt
    mov r9w, cs
    mov ecx, 256
fill_gate:
    mov word ptr [rdx], r8w
    mov word ptr [rdx+2], r9w
    mov word ptr [rdx+4], 8e00h ; IST 0, present, interrupt gate
    mov rax, r8
    shr rax, 16
    mov word ptr [rdx+6], ax
    shr rax, 16
    mov dword ptr [rdx+8], eax
    mov dword ptr [rdx+12], 0
    add rdx, 16
    dec ecx
    jnz fill_gate
    ret
L1PrepareIdt ENDP

PUBLIC L1DisableInterrupts
L1DisableInterrupts PROC
    cli
    ret
L1DisableInterrupts ENDP

PUBLIC L1Enter
L1Enter PROC
    cli
    mov rsp, rcx                ; page-aligned end of private stack
    lidt fword ptr [L1Idtr]     ; long mode reads a 10-byte pseudodescriptor
    cld
    sub rsp, 20h                ; Win64 shadow space; aligned before CALL
    call L1OwnedMain
L1Enter ENDP

PUBLIC L1Halt
L1Halt PROC
    cli
stop_cpu:
    hlt
    jmp stop_cpu
L1Halt ENDP
END
