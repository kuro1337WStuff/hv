option casemap:none

.code
PUBLIC IrqStart
IrqStart LABEL BYTE
    mov qword ptr [7010h], 1    ; IF already set, no STI shadow
IrqCheckpoint LABEL BYTE
    db 0fh, 01h, 0d9h          ; VMMCALL while V_TPR equals the request class
IrqAfter LABEL BYTE
    cmp qword ptr [7000h], 1    ; interrupt must precede this instruction
    jne short IrqBad
    lea rax, IrqAfter
    cmp qword ptr [7008h], rax
    jne short IrqBad
    cmp qword ptr [7018h], 1    ; phase observed by ISR
    jne short IrqBad
    cmp rsp, 7ff0h              ; IRETQ restored the original stack
    jne short IrqBad
    mov qword ptr [7010h], 2
    mov eax, 50415353h
IrqPass LABEL BYTE
    hlt
IrqBad:
    mov eax, 4641494ch
IrqFail LABEL BYTE
    hlt
    ud2

IrqHandler LABEL BYTE
    mov rax, [rsp]
    mov qword ptr [7008h], rax
    mov rax, [7010h]
    mov qword ptr [7018h], rax
    inc qword ptr [7000h]
    iretq
IrqEnd LABEL BYTE

.const
PUBLIC IrqLayout
IrqLayout DWORD IrqEnd-IrqStart, IrqCheckpoint-IrqStart, IrqAfter-IrqStart
          DWORD IrqHandler-IrqStart, IrqPass-IrqStart, IrqFail-IrqStart
END
