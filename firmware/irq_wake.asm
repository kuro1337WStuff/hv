option casemap:none

.code
; Copied as a unit to guest GPA 1000h. All data addresses are guest-local.
PUBLIC IrqStart
IrqStart LABEL BYTE
    sti
    nop                         ; retire the instruction in the STI shadow
IrqWaitHlt LABEL BYTE
    hlt
IrqAfterHlt LABEL BYTE
    cmp qword ptr [7000h], 1
    jne short irq_bad
    lea rax, IrqAfterHlt        ; RIP-relative, valid in the copied fixture
    cmp qword ptr [7008h], rax
    jne short irq_bad
    cmp rsp, 7ff0h
    jne short irq_bad
    mov qword ptr [7010h], 1
    mov eax, 50415353h
IrqPassHlt LABEL BYTE
    hlt
irq_bad:
    mov eax, 4641494ch
IrqFailHlt LABEL BYTE
    hlt
    ud2                         ; even an erroneous resume cannot enter the ISR

; Entered through guest IDT vector 41h only. No exception error code.
IrqHandler LABEL BYTE
    mov rax, [rsp]              ; examine return RIP without modifying the frame
    mov qword ptr [7008h], rax
    inc qword ptr [7000h]
    iretq
IrqEnd LABEL BYTE
; Export offsets as data: the labels above are sites within one blob, not
; separate C arrays whose relative addresses the optimizer can reason about.
.const
PUBLIC IrqLayout
IrqLayout DWORD IrqEnd-IrqStart, IrqWaitHlt-IrqStart, IrqAfterHlt-IrqStart
          DWORD IrqHandler-IrqStart, IrqPassHlt-IrqStart, IrqFailHlt-IrqStart
END
