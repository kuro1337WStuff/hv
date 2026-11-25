option casemap:none
.code
PUBLIC CrGuardStart, CrGuardWrite, CrGuardHlt, CrGuardEnd
; Fixed L2 entry, not a callable host procedure. RCX = guest-linear address of
; the identity-mapped witness; EDX = 0 (read/HLT) or 1 (refused write).
; Changes RAX, arithmetic flags and witness memory. The write arm may not
; reach after_write. See cr4_guard_run for the entry/exit contract (run 055).
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
