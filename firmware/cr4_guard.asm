option casemap:none
.code
PUBLIC CrGuardStart, CrGuardWrite, CrGuardHlt, CrGuardEnd
; RCX: fixed guest witness, EDX: zero for read control, one for refused write.
CrGuardStart LABEL BYTE
    mov rax, cr4
    mov [rcx], eax
    test edx, edx
    jz short after_write
    or eax, 40000h
CrGuardWrite LABEL BYTE
    mov cr4, rax
after_write:
    mov byte ptr [rcx+4], 0b6h
CrGuardHlt LABEL BYTE
    hlt
CrGuardEnd LABEL BYTE
END
