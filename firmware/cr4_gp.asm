option casemap:none
.code
PUBLIC GpStart, GpWrite, GpContinue, GpHandler, GpHlt, GpEnd
; Fixed long-mode L2. RCX names the identity-mapped witness page; EDX selects
; read-only (0) or refused CR4 write (1). Clobbers RAX and status flags.
; Only the guest handler redirects its own saved RIP; L1 never skips MOV CR4.
GpStart LABEL BYTE
    mov rax, cr4
    mov [rcx], rax
    test edx, edx
    jz short GpContinue
    or eax, 40000h
GpWrite LABEL BYTE
    mov cr4, rax
GpContinue:
    mov rax, cr4
    mov [rcx+8], rax
    mov dword ptr [rcx+48], 47505254h
GpHlt LABEL BYTE
    hlt
GpHandler LABEL BYTE
    inc qword ptr [rcx+16]
    mov rax, [rsp]               ; vector 13 pushes a 64-bit error-code slot
    mov [rcx+24], rax
    mov rax, [rsp+8]
    mov [rcx+32], rax
    mov [rcx+40], rsp
    cmp qword ptr [rcx+16], 1
    jne GpBad
    cmp qword ptr [rsp], 0
    jne GpBad
    lea rax, GpWrite
    cmp [rsp+8], rax
    jne GpBad
    cmp word ptr [rsp+16], 8
    jne GpBad
    cmp qword ptr [rsp+32], 7ff0h
    jne GpBad
    cmp word ptr [rsp+40], 10h
    jne GpBad
    lea rax, GpContinue
    mov [rsp+8], rax
    add rsp, 8
    iretq
GpBad:
    hlt                          ; distinct RIP, never a successful terminal
GpEnd LABEL BYTE
END
