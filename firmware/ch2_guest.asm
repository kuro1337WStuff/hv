option casemap:none
.code
PUBLIC Ch2Start, Ch2Hlt, Ch2Deny, Ch2In, Ch2Out, Ch2Long, Ch2End
; Fixed long-mode L2, no caller or arguments. Start/Long select literal counts;
; RAX/RBX/RDX/RDI and flags are scratch. Near calls use one slot on the owned
; stack at 7ff0h. Guest stores raw IN bytes at 7000h in observation order.
; Only Ch2In/Out resume (at the RET). Hlt and Deny are never resumed.
Ch2Start LABEL BYTE
    jmp short Ch2Short
Ch2Hlt:                          ; GPA 1002h
    hlt
Ch2Deny:                         ; GPA 1003h: unsupported mode 3 command
    out dx, al
    mov byte ptr [rdi+12], 1    ; must remain unreachable
Ch2Fail:
    hlt
Ch2In:
    in al, dx
    ret
Ch2Out:
    out dx, al
    ret
Ch2Short:
    mov ebx, 11931
    jmp short Ch2Program
Ch2Long LABEL BYTE
    mov ebx, 59659
Ch2Program:
    mov edi, 7000h
    mov edx, 61h
    call Ch2In
    mov [rdi], al
    and al, 0fdh                 ; speaker disabled, gate high
    or al, 1
    call Ch2Out
    mov edx, 43h
    mov al, 0b0h
    call Ch2Out
    mov edx, 42h
    mov eax, ebx
    call Ch2Out
    shr eax, 8
    call Ch2Out
    mov edx, 61h
    call Ch2In                   ; load pulse
    mov [rdi+1], al
    mov edx, 42h
    call Ch2In                   ; count xx00, stable pair
    mov [rdi+2], al
    call Ch2In
    mov [rdi+3], al
    call Ch2In                   ; low at xx00 again, high one pulse later
    mov [rdi+4], al
    call Ch2In
    mov [rdi+5], al
    mov edx, 61h
    call Ch2In                   ; load + N - 1: one count left
    mov [rdi+6], al
    cmp al, 1
    jne Ch2Fail
    call Ch2In                   ; load + N: OUT2 must rise now
    mov [rdi+7], al
    cmp al, 21h
    jne Ch2Fail
    mov dword ptr [rdi+8], 43483250h
    cmp ebx, 11931
    je Ch2Hlt
    mov edx, 43h
    mov eax, 0b6h
    jmp Ch2Deny
Ch2End LABEL BYTE
END
