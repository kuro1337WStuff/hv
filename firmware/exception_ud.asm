option casemap:none

.code
PUBLIC UdStart
UdStart LABEL BYTE
    mov qword ptr [7010h], 1    ; reached the fault, handler has not run
UdFault LABEL BYTE
    ud2
UdAfter LABEL BYTE
    cmp qword ptr [7000h], 1
    jne UdBad
    lea rax, UdFault
    cmp qword ptr [7008h], rax
    jne UdBad
    cmp qword ptr [7010h], 2    ; only the handler can advance phase 1 to 2
    jne UdBad
    cmp rsp, 7ff0h
    jne UdBad
    mov qword ptr [7010h], 3    ; IRETQ resumed at the checked continuation
    mov eax, 50415353h
UdPass LABEL BYTE
    hlt
UdBad:
    mov eax, 4641494ch
UdFail LABEL BYTE
    hlt

UdHandler LABEL BYTE
    ; Aligned original RSP minus SS:RSP, RFLAGS, CS:RIP; #UD has no error code.
    cmp rsp, 7fc8h
    jne UdBad
    inc qword ptr [7000h]
    mov rax, [rsp]
    mov qword ptr [7008h], rax
    cmp qword ptr [7000h], 1
    jne UdBad
    cmp qword ptr [7010h], 1
    jne UdBad
    cmp word ptr [rsp+8], 8     ; stacked CS
    jne UdBad
    cmp qword ptr [rsp+24], 7ff0h
    jne UdBad
    cmp word ptr [rsp+32], 10h  ; stacked SS
    jne UdBad
    lea rax, UdFault
    cmp [rsp], rax
    jne UdBad
    cmp word ptr [rax], 0b0fh   ; accept only our immutable 0F 0B
    jne UdBad
    add qword ptr [rsp], 2
    mov qword ptr [7010h], 2
    iretq
UdEnd LABEL BYTE

.const
PUBLIC UdLayout
UdLayout DWORD UdEnd-UdStart, UdFault-UdStart, UdAfter-UdStart
         DWORD UdHandler-UdStart, UdPass-UdStart, UdFail-UdStart
END
