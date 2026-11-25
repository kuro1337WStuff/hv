option casemap:none

; Byte offsets of the four XmmWorkspace banks in vcpu_xmm.h.
GuestFrame   EQU 0
HostSaved    EQU 100h
HostSeed     EQU 200h
HostObserved EQU 300h

; Instrumentation, separate from the switch transfers below. MOVDQU also
; works on the unaligned tables embedded in the copied guest image.
load_bank MACRO base, disp
    FOR n, <0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15>
        movdqu xmm&n, xmmword ptr [base+disp+16*n]
    ENDM
ENDM
snapshot_bank MACRO base, disp
    FOR n, <0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15>
        movdqu xmmword ptr [base+disp+16*n], xmm&n
    ENDM
ENDM

.code
PUBLIC RunXmmGuest
; Microsoft x64: RCX = identity-mapped VMCB PA, RDX = private workspace.
; Preserve the real caller (even volatile XMM0..5) outside both test frames.
; The fixed guest below modifies only volatile integer registers and RAX,
; which VMRUN exchanges via the VMCB. Never trust guest RDX as an L1 pointer.
RunXmmGuest PROC FRAME
    sub rsp, 108h
    .allocstack 108h
    FOR n, <0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15>
        movdqa xmmword ptr [rsp+16*n], xmm&n
        IF n GE 6
            .savexmm128 xmm&n, 16*n
        ENDIF
    ENDM
    .endprolog
    mov [rsp+100h], rdx
    load_bank rdx, HostSeed

    ; Switch under test. AMD VMRUN does not exchange these registers.
    FOR n, <0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15>
        movdqa xmmword ptr [rdx+HostSaved+16*n], xmm&n
        movdqa xmm&n, xmmword ptr [rdx+GuestFrame+16*n]
    ENDM
    mov rax, rcx
    vmrun rax
    mov rdx, [rsp+100h]
    FOR n, <0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15>
        movdqa xmmword ptr [rdx+GuestFrame+16*n], xmm&n
        movdqa xmm&n, xmmword ptr [rdx+HostSaved+16*n]
    ENDM

    ; Sample the *live* host registers, not the memory used to restore them.
    snapshot_bank rdx, HostObserved
    load_bank rsp, 0
    add rsp, 108h
    ret
RunXmmGuest ENDP

; Snapshot before touching any XMM, then compare all 32 words in integer
; registers against the read-only guest table. On failure the snapshot stays
; in guest RAM, so L1 can name the exact register, lane and unexpected value.
check_entry MACRO dest, expected
    LOCAL compare_next
    mov edx, dest
    snapshot_bank rdx, 0
    lea r8, expected
    xor ecx, ecx
compare_next:
    mov rax, [rdx+rcx*8]
    cmp rax, [r8+rcx*8]
    jne XmmBad
    inc ecx
    cmp ecx, 32
    jne compare_next
ENDM

PUBLIC XmmStart
XmmStart LABEL BYTE
    check_entry 7000h, XmmInitial
    lea r8, XmmCheckpoint
    load_bank r8, 0
XmmCallSite LABEL BYTE
    db 0fh, 01h, 0d9h          ; VMMCALL (SVM exit 81h)
XmmResume LABEL BYTE
    check_entry 7100h, XmmCheckpoint
    lea r8, XmmFinal
    load_bank r8, 0
    mov eax, 50415353h
XmmPass LABEL BYTE
    hlt
XmmBad:
    mov eax, 4641494ch
XmmFail LABEL BYTE
    hlt

; One little-endian QWORD per lane; both DWORD halves vary with phase and
; physical lane. No absolute L1 addresses are embedded in this guest image.
guest_bank MACRO phase_id
    LOCAL slot
    slot = 0
    REPT 32
        DWORD 0a6e90000h + 100h*phase_id + slot, 47530000h + 100h*phase_id + slot
        slot = slot + 1
    ENDM
ENDM
XmmInitial LABEL BYTE
    guest_bank 0
XmmCheckpoint LABEL BYTE
    guest_bank 1
XmmFinal LABEL BYTE
    guest_bank 2
XmmEnd LABEL BYTE

.const
PUBLIC XmmLayout
XmmLayout DWORD XmmEnd-XmmStart, XmmCallSite-XmmStart, XmmResume-XmmStart
          DWORD XmmPass-XmmStart, XmmFail-XmmStart
END
