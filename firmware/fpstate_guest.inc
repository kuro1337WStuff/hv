; Fixed L2, copied to GPA 1000h. All embedded data is read-only and all
; references to it are RIP-relative. Only the guest stack page is written.
FpSnapshotGpa EQU 7000h              ; aligned FXSAVE64 image (512 bytes)
FpGprGpa      EQU FpSnapshotGpa+200h ; 14 qwords immediately after image
FpResultsGpa  EQU FpGprGpa+14*8      ; rounded SSE and x87 results (DWORDs)

; The first fourteen qwords in each reference table follow the switch ABI.
fp_reg MACRO loading, slot, gp
    IF loading
        mov gp, [rax+slot*8]
    ELSE
        mov [rax+slot*8], gp
    ENDIF
ENDM
fp_regs MACRO loading
    fp_reg loading, 0, rbx
    fp_reg loading, 1, rcx
    fp_reg loading, 2, rdx
    fp_reg loading, 3, rbp
    fp_reg loading, 4, rsi
    fp_reg loading, 5, rdi
    fp_reg loading, 6, r8
    fp_reg loading, 7, r9
    fp_reg loading, 8, r10
    fp_reg loading, 9, r11
    fp_reg loading, 10, r12
    fp_reg loading, 11, r13
    fp_reg loading, 12, r14
    fp_reg loading, 13, r15
ENDM
FpRefXmm   EQU 14*8
FpRefFcw   EQU FpRefXmm+16*16
FpRefMxcsr EQU FpRefFcw+4
FpRefSt0   EQU FpRefMxcsr+4
FpRefSt1   EQU FpRefSt0+10

.code
PUBLIC FpStart
FpStart LABEL BYTE
    call FpSample
    lea r8, FpInitialState
    mov r9d, 1
    call FpCheck
    lea rax, FpUpdatedState
    call FpInstall
    mov eax, 46504332h                 ; "FPC2", VMCB-managed checkpoint
FpCallSite LABEL BYTE
    db 0fh, 01h, 0d9h                 ; VMMCALL, no operands
FpResume LABEL BYTE
    call FpSample
    lea r8, FpUpdatedState
    mov r9d, 2
    call FpCheck

    ; Use the *live* resumed controls/stack before installing the final
    ; bank. Round-up is the only rounding mode that gives either answer.
    mov edx, FpResultsGpa
    cvtss2si eax, dword ptr [FpQuarter] ; +0.25 -> +1 (MXCSR)
    mov [rdx], eax
    mov ecx, 40h
    cmp eax, 1
    jne FpBad
    faddp st(1), st(0)                 ; resumed ST1 + ST0 = 6 + 5
    fadd dword ptr [FpQuarter]
    fistp dword ptr [rdx+4]            ; +11.25 -> +12 (FCW)
    mov ecx, 41h
    cmp dword ptr [rdx+4], 12
    jne FpBad
    lea rax, FpFinalState
    call FpInstall
    mov eax, 50415353h
FpPass LABEL BYTE
    hlt
FpCheckBad:
    add rsp, 8                         ; discard only FpCheck's return address
FpBad:
    shl r9d, 8                         ; EAX = generation:detail
    or ecx, r9d
    push rcx
    mov eax, FpGprGpa
    fp_regs 1                          ; leave offending entry GPRs live
    pop rax
FpFail LABEL BYTE
    hlt

FpSample:
    ; RAX is VMCB-managed, not a member of the software GPR bank.
    mov eax, FpSnapshotGpa
    db 48h, 0fh, 0aeh, 00h             ; FXSAVE64 [rax]
    mov eax, FpGprGpa
    fp_regs 0
    ret

; Check the entry image without consuming any live x87/XMM data. In the
; FXSAVE64 image ST(i) data is logical, but the abridged FTW is physical.
; Empty registers, reserved bytes, FIP/FDP and unrelated FSW bits aren't
; part of this oracle. R9D retains the generation for any diagnostic.
FpCheck:
    mov edx, FpSnapshotGpa
    xor ecx, ecx
FpCheckGpr:
    mov rax, [rdx+200h+rcx*8]
    cmp rax, [r8+rcx*8]
    jne FpCheckBad
    inc ecx
    cmp ecx, 14
    jb FpCheckGpr
    mov ecx, 10h
    mov ax, [r8+FpRefFcw]
    cmp [rdx], ax
    jne FpCheckBad
    inc ecx
    mov ax, [rdx+2]
    and ax, 3800h
    cmp ax, 3000h                      ; TOP=6
    jne FpCheckBad
    inc ecx
    cmp byte ptr [rdx+4], 0c0h         ; physical R6 and R7 occupied
    jne FpCheckBad
    inc ecx
    mov eax, [r8+FpRefMxcsr]
    cmp [rdx+18h], eax
    jne FpCheckBad
    inc ecx
    mov rax, [r8+FpRefSt0]
    cmp [rdx+20h], rax
    jne FpCheckBad
    mov ax, [r8+FpRefSt0+8]
    cmp [rdx+28h], ax
    jne FpCheckBad
    inc ecx
    mov rax, [r8+FpRefSt1]
    cmp [rdx+30h], rax
    jne FpCheckBad
    mov ax, [r8+FpRefSt1+8]
    cmp [rdx+38h], ax
    jne FpCheckBad
    xor ecx, ecx
FpCheckXmm:
    mov rax, [rdx+0a0h+rcx*8]
    cmp rax, [r8+FpRefXmm+rcx*8]
    jne FpBadLane
    inc ecx
    cmp ecx, 32
    jb FpCheckXmm
    ret
FpBadLane:
    add ecx, 20h
    jmp FpCheckBad

; Build the next live guest state with ordinary loads, independently of
; the runtime's FXRSTOR64. Reset sticky flags and start with an empty stack.
FpInstall:
    fninit
    fldcw word ptr [rax+FpRefFcw]
    fld tbyte ptr [rax+FpRefSt1]
    fld tbyte ptr [rax+FpRefSt0]
    ldmxcsr dword ptr [rax+FpRefMxcsr]
    FOR n, <0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15>
        movdqu xmm&n, xmmword ptr [rax+FpRefXmm+16*n]
    ENDM
    fp_regs 1
    ret

; No implicit alignment inside the reference table. Low and high DWORDs
; independently distinguish owner, generation and physical register/lane.
fp_reference MACRO generation, cwval, mxval, stzero, stone
    LOCAL slot
    slot = 0
    REPT 14
        DWORD 0b7000000h+100h*generation+slot, 67000000h+10000h*generation+slot
        slot = slot+1
    ENDM
    slot = 0
    REPT 32
        DWORD 0a6000000h+100h*generation+slot, 47000000h+10000h*generation+slot
        slot = slot+1
    ENDM
    WORD cwval, 0
    DWORD mxval
    REAL10 stzero, stone
ENDM
FpQuarter DWORD 3e800000h
FpInitialState LABEL BYTE
    fp_reference 1, 077fh, 3f80h, 3.0, 4.0
FpUpdatedState LABEL BYTE
    fp_reference 2, 0b7fh, 5f80h, 5.0, 6.0
FpFinalState LABEL BYTE
    fp_reference 3, 0f7fh, 7f80h, 7.0, 8.0
FpEnd LABEL BYTE

.const
PUBLIC FpLayout
FpLayout DWORD FpEnd-FpStart, FpCallSite-FpStart, FpResume-FpStart
         DWORD FpPass-FpStart, FpFail-FpStart
