option casemap:none

.code
ALIGN 16
PUBLIC TxStart, TxEnd, TxIn, TxOut, TxDone
TxStart LABEL BYTE
    jmp short Setup
TxIn:
    in al, dx
    ret
TxOut:
    out dx, al
    ret
TxDone:
    hlt
    ud2

; Fixed, polled COM1 only: no IRQ, BIOS, or Linux code. Report progress in
; guest RAM, never by using a string in the UART stream as a success token.
Setup:
    xor edi, edi                ; cumulative bytes whose OUT returned
    xor ebp, ebp                ; independently sampled busy intervals
    mov r10d, 811c9dc5h         ; FNV-1a over the bytes actually sent
    mov edx, 3fdh
    call TxIn
    cmp al, 60h
    jne Failed

    lea rsi, EarlyBytes
    mov ecx, EarlyEnd-EarlyBytes
    call SendPhase
    mov dword ptr [7000h], edi
    mov dword ptr [7004h], r10d
    lea rsi, LateBytes
    mov ecx, LateEnd-LateBytes
    call SendPhase
    cmp rsp, 7ff0h
    jne Failed
    mov dword ptr [7008h], edi
    mov dword ptr [700ch], r10d
    mov dword ptr [7010h], ebp
    movzx eax, al
    mov dword ptr [7014h], eax   ; final LSR, sampled after the last byte
    mov dword ptr [7018h], 0b060cafeh
    mov eax, 444f4e45h
    jmp TxDone

SendPhase:
    movzx eax, byte ptr [rsi]
    mov edx, 3f8h
    call TxOut
    xor r10d, eax
    imul r10d, r10d, 01000193h
    inc edi
    inc rsi
    mov edx, 3fdh
    call TxIn
    test al, al
    jnz Failed
    inc ebp
    call TxIn                   ; bounded drain: one busy, then THRE|TEMT
    cmp al, 60h
    jne Failed
    dec ecx
    jnz SendPhase
    ret
Failed:
    mov eax, 4641494ch
    hlt
    ud2
EarlyBytes db 'EARLY:console!', 0dh, 0ah
EarlyEnd LABEL BYTE
LateBytes db 'LATE:terminal!', 0dh, 0ah
LateEnd LABEL BYTE
TxEnd LABEL BYTE
END
