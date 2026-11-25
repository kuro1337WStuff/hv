option casemap:none

.code
ALIGN 16
PUBLIC UartStart, UartEnd, UartIn, UartOut, UartPass, UartNegStart, UartDeny
UartStart LABEL BYTE
    mov rsp, 7ff0h
    mov rax, 0bead123456789000h ; upper 56 bits must survive every byte IN
    mov edx, 3fbh
    mov al, 03h
    call WriteByte
    mov edx, 3f9h
    xor al, al
    call WriteByte
    mov edx, 3fbh
    mov al, 83h
    call WriteByte
    call ReadByte
    cmp al, 83h
    jne Bad
    mov edx, 3f8h
    mov al, 1
    call WriteByte              ; DLL, not THR: must not start a busy interval
    inc edx
    xor al, al
    call WriteByte
    dec edx
    call ReadByte
    cmp al, 1
    jne Bad
    inc edx
    mov al, 0ffh
    call ReadByte
    test al, al
    jnz Bad
    mov edx, 3fbh
    mov al, 03h
    call WriteByte
    call ReadByte
    cmp al, 03h
    jne Bad
    mov edx, 3f9h
    call ReadByte
    test al, al
    jnz Bad
    dec edx
    mov al, 0ffh
    call ReadByte               ; empty RBR, not DLL
    test al, al
    jnz Bad
    mov edx, 3fch
    mov al, 3
    call WriteByte
    xor al, al
    call ReadByte
    cmp al, 3
    jne Bad
    mov edx, 3ffh
    mov al, 5ah
    call WriteByte
    xor al, al
    call ReadByte
    cmp al, 5ah
    jne Bad
    mov edx, 3fah
    mov al, 7
    call WriteByte
    call ReadByte               ; IIR: no pending IRQ, FIFO bits clear
    cmp al, 1
    jne Bad

    lea rsi, Payload
    xor edi, edi                ; bytes transmitted
    xor ebp, ebp                ; independently observed busy samples
Poll:
    mov edx, 3fdh
    call ReadByte
    test edi, edi
    jz ExpectReady              ; initial LSR proves DLL write wasn't TX
    test al, al
    jnz Bad                     ; exactly one busy sample per THR write
    inc ebp
    call ReadByte               ; bound: at most two reads per poll
ExpectReady:
    cmp al, 60h                 ; both THRE and TEMT, no data-ready bit
    jne Bad
    cmp edi, PayloadEnd-Payload
    je Drained
    mov al, byte ptr [rsi+rdi]
    mov edx, 3f8h
    call WriteByte
    inc edi
    jmp Poll
Drained:
    cmp ebp, 8
    jne Bad
    cmp rsp, 7ff0h
    jne Bad
    movzx eax, al
    mov dword ptr [7004h], eax
    mov dword ptr [7008h], ebp
    mov dword ptr [7000h], 82504701h
    mov eax, 50415353h
UartPass LABEL BYTE
    hlt
    ud2

ReadByte:
    mov r8, rax
    mov r9, rdx
UartIn LABEL BYTE
    in al, dx
    mov r8b, al                 ; only AL may differ
    jmp CheckIo
WriteByte:
    mov r8, rax
    mov r9, rdx
UartOut LABEL BYTE
    out dx, al
CheckIo:
    cmp rax, r8
    jne Bad
    cmp rdx, r9
    jne Bad
    ret
Bad:
    mov eax, 4641494ch
    hlt
    ud2

UartNegStart LABEL BYTE
    mov edx, 2f8h
    mov eax, 0a5h
UartDeny LABEL BYTE
    out dx, al
    jmp Bad                     ; denied OUT must never return
Payload db 'L2:8250', 0ah
PayloadEnd LABEL BYTE
UartEnd LABEL BYTE
END
