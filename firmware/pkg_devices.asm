option casemap:none

.code
; One relocation-free L2 blob. Only the fixed ISR, virtual UART OUT and
; intercepted HLT/VMMCALL sites below are part of the fixture ABI.
ALIGN 16
PUBLIC P4Start, P4AfterShadow, P4Equal, P4Wait, P4Out, P4Stop
PUBLIC P4Handler, P4Eoi, P4End
P4Start LABEL BYTE
    db 0fh, 01h, 0d9h           ; checkpoint: queued vector, IF still clear
    sti
    inc qword ptr [7000h]        ; the one instruction protected by STI
P4AfterShadow LABEL BYTE
    db 0fh, 01h, 0d9h           ; checkpoint after first IRETQ
    nop                         ; run with IF=1 and V_TPR equal to IRQ class
P4Equal LABEL BYTE
    db 0fh, 01h, 0d9h           ; checkpoint: same pending request, no new ISR
P4Wait LABEL BYTE
    hlt                         ; intercepted sleep; resume only on timer expiry
    inc qword ptr [7010h]        ; independent post-wake progress
    lea rsi, P4Marker
    mov ecx, P4MarkerEnd-P4Marker
    mov edx, 3f8h
P4NextByte:
    lodsb
P4Out LABEL BYTE
    out dx, al
    loop P4NextByte
    cmp qword ptr [7000h], 1
    jne short P4Bad
    cmp qword ptr [7008h], 3
    jne short P4Bad
    cmp qword ptr [7010h], 1
    jne short P4Bad
    cmp rsp, 7ff0h
    jne short P4Bad
    mov eax, 50415353h
P4Stop LABEL BYTE
    hlt
P4Bad:
    mov eax, 4641494ch
    hlt
    ud2

P4Handler LABEL BYTE
    mov rax, [rsp]
    mov qword ptr [7018h], rax   ; unmodified hardware-stacked return IP
    inc qword ptr [7008h]
    mov eax, 41h                ; fixed controller ABI: EOI for vector 41h
P4Eoi LABEL BYTE
    db 0fh, 01h, 0d9h
    iretq
P4Marker db 'L2:UART:TX'
P4MarkerEnd LABEL BYTE
P4End LABEL BYTE
END
