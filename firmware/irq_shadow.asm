option casemap:none

.code
; Copied as one guest-local blob to GPA 1000h.
PUBLIC IrqStart
IrqStart LABEL BYTE
    mov qword ptr [7010h], 1    ; IF is initially clear: still masked
IrqCheckpoint LABEL BYTE
    db 0fh, 01h, 0d9h          ; VMMCALL (exit 81h) while the V_IRQ is masked
IrqSti LABEL BYTE
    sti
    inc qword ptr [7010h]       ; sole instruction in the STI shadow
IrqAfterShadow LABEL BYTE
    cmp qword ptr [7000h], 1    ; exactly one delivery
    jne short IrqBad
    lea rax, IrqAfterShadow
    cmp qword ptr [7008h], rax  ; ISR's stacked return RIP
    jne short IrqBad
    cmp qword ptr [7018h], 2    ; ISR ran after the shadow instruction
    jne short IrqBad
    cmp rsp, 7ff0h              ; IRETQ restored the continuation stack
    jne short IrqBad
    mov qword ptr [7010h], 3    ; success is terminal and only after all checks
    mov eax, 50415353h
IrqPass LABEL BYTE
    hlt
IrqBad:
    mov eax, 4641494ch
IrqFail LABEL BYTE
    hlt
    ud2                         ; never fall through into the interrupt handler

; Guest IDT vector 41h; the hardware frame contains RIP, CS, and RFLAGS.
IrqHandler LABEL BYTE
    mov rax, [rsp]
    mov qword ptr [7008h], rax
    mov rax, [7010h]
    mov qword ptr [7018h], rax
    inc qword ptr [7000h]
    iretq
IrqEnd LABEL BYTE

; Single data object exports offsets into the copied guest blob.
.const
PUBLIC IrqLayout
IrqLayout DWORD IrqEnd-IrqStart, IrqCheckpoint-IrqStart, IrqSti-IrqStart
          DWORD IrqAfterShadow-IrqStart, IrqHandler-IrqStart
          DWORD IrqPass-IrqStart, IrqFail-IrqStart
END
