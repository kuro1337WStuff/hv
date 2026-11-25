option casemap:none

.code
ALIGN 16
PUBLIC TickStart, TickEnd, TickIn, TickOut, TickUnmask, TickAfterUnmask
PUBLIC TickWait, TickPass, TickFail, TickHandler, TickBadEoi, TickDeny

; Copied as one blob. 7000h is the zeroed evidence page; RSP starts at 7ff0h.
TickStart LABEL BYTE
    sti
    mov edx, 21h
    mov eax, 0ffh
    call TickOut                ; mask IRQ0 while the first deadline expires
    mov edx, 43h
    mov al, 38h                 ; channel 0, low/high bytes, mode 4, binary
    call TickOut
    mov edx, 40h
    mov al, 32                  ; first reload: 32 ticks
    call TickOut
    xor al, al
    call TickOut

    mov edx, 21h
    mov al, 0feh
TickUnmask LABEL BYTE
    out dx, al
TickAfterUnmask LABEL BYTE
    cmp dword ptr [7000h], 1    ; pending IRQ must precede this instruction
    jne TickBad
    cmp rax, 0feh
    jne TickBad
    cmp rdx, 21h
    jne TickBad
    lea rax, TickAfterUnmask
    cmp qword ptr [7010h], rax
    jne TickBad
    cmp rsp, 7ff0h
    jne TickBad

    mov edx, 43h
    mov al, 38h
    call TickOut
    mov edx, 40h
    mov al, 64                  ; second reload: 64 ticks, IRQ0 already unmasked
    call TickOut
    xor al, al
    call TickOut
    mov eax, 12345678h
    mov edx, 23456789h
TickWait LABEL BYTE
    hlt
TickAfterWait LABEL BYTE
    cmp dword ptr [7000h], 2
    jne TickBad
    cmp rax, 12345678h
    jne TickBad
    cmp rdx, 23456789h
    jne TickBad
    lea rax, TickAfterWait
    cmp qword ptr [7018h], rax
    jne TickBad
    lea rax, TickAfterUnmask
    cmp qword ptr [7010h], rax
    jne TickBad
    cmp rsp, 7ff0h
    jne TickBad
    mov dword ptr [7004h], 1    ; only the post-HLT continuation publishes success
    mov dword ptr [7008h], 04871001h
    mov eax, 50415353h
TickPass LABEL BYTE
    hlt
    ud2

; Vector 30h interrupt gate: no error code, [entry RSP] is the return RIP.
TickHandler LABEL BYTE
    push rax
    push rdx
    mov edx, dword ptr [7000h]
    cmp edx, 2
    jae TickBad                 ; never index past the two evidence slots
    mov rax, [rsp+16]
    mov qword ptr [rdx*8+7010h], rax
    inc dword ptr [7000h]
    mov edx, 21h
    call TickIn
    cmp al, 0feh
    jne TickBad
    or al, 1
    call TickOut                ; mask before master IRQ0 specific EOI
    mov edx, 20h
    mov al, 60h
    call TickOut
    mov edx, 21h
    mov al, 0feh
    call TickOut
    pop rdx
    pop rax
    iretq

TickIn:
    in al, dx
    ret
TickOut:
    out dx, al
    ret

; Separate fresh-arm entry. The fixture forbids this request; no 8259 fault implied.
TickBadEoi LABEL BYTE
    mov edx, 20h
    mov eax, 61h
TickDeny LABEL BYTE
    out dx, al                  ; returning from this intercept is a test failure
TickBad:
    mov eax, 4641494ch
TickFail LABEL BYTE
    hlt
    ud2
TickEnd LABEL BYTE
END
