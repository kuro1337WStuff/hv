option casemap:none

.code
ALIGN 16
PUBLIC PowerStart, PowerNegative, PowerPoll, PowerTx, PowerSelect, PowerFinal
PUBLIC PowerWait, PowerFail, PowerEnd
PowerStart LABEL BYTE
    mov r8d, 3401h              ; type 5 with SLP_EN and SCI_EN
    jmp short PowerCommon
PowerNegative LABEL BYTE
    mov r8d, 1401h              ; type 5 without SLP_EN: not a request
PowerCommon:
    lea rsi, PowerMarker
    xor edi, edi
PowerNext:
    mov ecx, 2                  ; initial ready, or one busy read then ready
    mov edx, 3fdh
PowerPoll:
    in al, dx
    cmp al, 60h
    je short PowerReady
    test al, al
    jnz short PowerFail
    dec ecx
    jnz short PowerPoll
    jmp short PowerFail
PowerReady:
    cmp edi, PowerMarkerEnd-PowerMarker
    je short PowerDrained
    mov al, [rsi+rdi]
    mov edx, 3f8h
PowerTx LABEL BYTE
    out dx, al
    inc edi
    jmp short PowerNext
PowerDrained:
    mov dword ptr [7000h], 53354701h
    mov dword ptr [7004h], edi
    mov edx, 604h
    mov eax, 1401h
PowerSelect LABEL BYTE
    out dx, ax
    mov eax, r8d
PowerFinal LABEL BYTE
    out dx, ax
    cmp r8d, 1401h              ; positive guest must never be resumed
    jne short PowerFail
    sti
    nop
PowerWait LABEL BYTE
    hlt
PowerFail:
    hlt
    ud2
PowerMarker db 'L2:S5', 0ah
PowerMarkerEnd LABEL BYTE
PowerEnd LABEL BYTE
END
