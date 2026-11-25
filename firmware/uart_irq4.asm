option casemap:none
.code
PUBLIC U4Start, U4Mcr, U4Ier, U4First, U4Again, U4Poll, U4Pass, U4Handler, U4Ack, U4Eoi, U4End
U4Start LABEL BYTE
    mov edx, 3fch             ; AL = matched arm's OUT2 setting
U4Mcr LABEL BYTE
    out dx, al
    mov edx, 3f9h
    mov al, 2
U4Ier LABEL BYTE
    out dx, al
    sti
    nop
U4First LABEL BYTE
    hlt
    inc dword ptr [7004h]     ; reached after IRETQ or ordinary HLT resume
U4Again LABEL BYTE
    hlt
    mov edx, 3fah
U4Poll LABEL BYTE
    in al, dx                 ; independent mainline IIR receipt
    mov byte ptr [700dh], al
    mov dword ptr [7008h], 34515249h
U4Pass LABEL BYTE
    hlt
    ud2
U4Handler LABEL BYTE
    inc dword ptr [7000h]
    mov edx, 3fah
U4Ack LABEL BYTE
    in al, dx
    mov byte ptr [700ch], al
    mov edx, 20h
    mov al, 64h               ; master 8259 specific EOI, IRQ4
U4Eoi LABEL BYTE
    out dx, al
    iretq
U4End LABEL BYTE
END
