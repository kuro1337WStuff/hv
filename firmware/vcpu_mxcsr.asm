option casemap:none
.code
PUBLIC RunMxGuest
; RCX = VMCB PA, RDX = private L1 MxFrame. The host stack pointer is
; restored by VMEXIT; the guest may have overwritten the integer arguments.
RunMxGuest PROC FRAME
    sub rsp, 18h
    .allocstack 18h
    .endprolog
    mov [rsp+8], rdx
    stmxcsr dword ptr [rsp]         ; real caller, outside the switch frames
    ldmxcsr dword ptr [rdx+4]       ; install the synthetic L1 for this entry
    stmxcsr dword ptr [rdx+8]       ; save live L1
    ldmxcsr dword ptr [rdx]         ; restore L2
    mov rax, rcx
    vmrun rax
    mov rdx, [rsp+8]
    stmxcsr dword ptr [rdx]         ; save live L2
    ldmxcsr dword ptr [rdx+8]       ; restore L1
    stmxcsr dword ptr [rdx+12]      ; independently sample live L1
    ldmxcsr dword ptr [rsp]
    add rsp, 18h
    ret
RunMxGuest ENDP

; Use only volatile integer registers. No XMM register is touched. All six
; exceptions are masked; 1.5 is exact as binary32, its conversion is not.
probe MACRO dest, mode, result
    mov edx, dest
    stmxcsr dword ptr [rdx]
    cmp dword ptr [rdx], mode
    jne MxBad
    cvtss2si eax, dword ptr [MxOneHalf]
    mov [rdx+4], eax
    stmxcsr dword ptr [rdx+8]
    cmp eax, result
    jne MxBad
    cmp dword ptr [rdx+8], (mode OR 20h) ; precision sticky, no other exceptions
    jne MxBad
ENDM
PUBLIC MxStart
MxStart LABEL BYTE
    probe 7000h, 3f80h, 1           ; down
    ldmxcsr dword ptr [MxUp]        ; change RC and clear precision before exit
MxCall LABEL BYTE
    db 0fh, 01h, 0d9h
MxResume LABEL BYTE
    probe 700ch, 5f80h, 2           ; up; must survive VMMCALL/resume
    mov eax, 50415353h
MxPass LABEL BYTE
    hlt
MxBad:
    mov eax, 4641494ch
MxFail LABEL BYTE
    hlt
MxOneHalf DWORD 3fc00000h           ; 1.5f
MxUp DWORD 5f80h
MxEnd LABEL BYTE

.const
PUBLIC MxLayout
MxLayout DWORD MxEnd-MxStart, MxCall-MxStart, MxResume-MxStart
         DWORD MxPass-MxStart, MxFail-MxStart
END
