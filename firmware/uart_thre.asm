option casemap:none

; Each read is copied from AL into the guest-owned receipt before comparison.
Sample MACRO value
    call ReadByte
    mov byte ptr [rdi], al
    inc edi
    cmp al, value
    jne Bad
ENDM
SendByte MACRO value
    mov al, value
    call WriteByte
ENDM
.code
ALIGN 16
PUBLIC ThreStart, ThreEnd, ThreIn, ThreOut, ThrePass, ThreNegative, ThreDeny
ThreStart LABEL BYTE
    mov rsp, 7ff0h
    mov edi, 7000h
    mov edx, 3f9h
    Sample 0
    mov bl, al
    SendByte 0
    Sample 0
    SendByte 0fh
    Sample 0fh
    SendByte bl
    Sample 0
    mov edx, 3fah
    SendByte 7
    Sample 1
    mov edx, 3ffh
    SendByte 55h
    Sample 55h
    SendByte 0aah
    Sample 0aah
    mov edx, 3f9h
    SendByte 2                    ; first cause: enabling while empty
    mov edx, 3fch
    SendByte 8
    SendByte 0                    ; masking the line does not consume the cause
    SendByte 8
    mov edx, 3fah
    Sample 2
    Sample 1
    mov edx, 3f8h
    SendByte 54h
    mov edx, 3fah
    Sample 1                      ; not rearmed by THR itself
    mov edx, 3fdh
    Sample 0                      ; one logical poll drains TX and rearms
    Sample 60h
    mov edx, 3fah
    Sample 2
    Sample 1
    mov dword ptr [7010h], 54485245h
ThrePass LABEL BYTE
    hlt
    ud2
ReadByte:
ThreIn LABEL BYTE
    in al, dx
    ret
WriteByte:
ThreOut LABEL BYTE
    out dx, al
    ret
Bad:
    hlt
    ud2
ThreNegative LABEL BYTE
    mov edx, 3f9h
    mov eax, 82h
ThreDeny LABEL BYTE
    out dx, al
    mov dword ptr [7010h], 444f4e45h
    hlt
    ud2
ThreEnd LABEL BYTE
END
