option casemap:none

.code
PUBLIC RunCpuidGuest
; RCX = VMCB physical address, RDX = in/out {RBX, RCX, RDX} frame.
; Only for the fixed guest below: it touches no other software-managed GPRs.
; Host RSP and RAX are switched by VMRUN. Keep the frame pointer in host
; caller-provided home space; RBX is nonvolatile in the Microsoft x64 ABI.
RunCpuidGuest PROC FRAME
    push rbx
    .pushreg rbx
    .endprolog
    mov [rsp+16], rdx
    mov rax, rcx
    mov rbx, [rdx]
    mov rcx, [rdx+8]
    mov rdx, [rdx+16]
    vmrun rax
    mov rax, [rsp+16]
    mov [rax], rbx
    mov [rax+8], rcx
    mov [rax+16], rdx
    pop rbx
    ret
RunCpuidGuest ENDP

; Bytes copied into the single read-only L2 page, never called as host code.
; These immediate-register forms have the same encoding in 32/64-bit mode;
; 66h makes their operand size 32 in the guest's 16-bit code segment.
g32 MACRO op, regname, imm
    db 66h
    op regname, imm
ENDM
PUBLIC L2Start, L2Cpuid, L2AfterCpuid, L2PassHlt, L2End
L2Start LABEL BYTE
    g32 mov, eax, 400000feh
    g32 mov, ecx, 7
L2Cpuid LABEL BYTE
    cpuid
L2AfterCpuid LABEL BYTE
    g32 cmp, eax, 11223344h
    jne short bad_reply
    g32 cmp, ebx, 55667788h
    jne short bad_reply
    g32 cmp, ecx, 099aabbcch
    jne short bad_reply
    g32 cmp, edx, 0ddeeff00h
    jne short bad_reply
    g32 mov, eax, 50415353h      ; PASS marker, generated only by L2
L2PassHlt LABEL BYTE
    hlt
bad_reply:
    g32 mov, eax, 4641494ch      ; FAIL marker, distinct halt address
    hlt
L2End LABEL BYTE
END
