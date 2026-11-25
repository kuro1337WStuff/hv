option casemap:none

; FpBank (vcpu_fpstate.h): the 16-byte-aligned FXSAVE64 image followed by
; BX,CX,DX,BP,SI,DI,R8..R15. RAX/RSP/RIP are in the VMCB, not this bank.
FpGprs EQU 200h
FpBankBytes EQU 270h
FpSeed EQU FpBankBytes
FpLive EQU 2*FpBankBytes
FpBefore EQU 3*FpBankBytes
FpAfter EQU 4*FpBankBytes

; Force REX.W for the 64-bit FIP/FDP layout. Only low base registers (RAX
; and RSP) are used by these macros, so there is no second REX prefix.
fp_save MACRO base, disp
    db 48h
    fxsave [base+disp]
ENDM
fp_restore MACRO base, disp
    db 48h
    fxrstor [base+disp]
ENDM
fp_store_gprs MACRO base, disp, unwind
    LOCAL slot
    slot = 0
    FOR regname, <rbx,rcx,rdx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15>
        mov [base+disp+FpGprs+slot*8], regname
        IF unwind AND ((slot EQ 0) OR ((slot GE 3) AND (slot LE 5)) OR (slot GE 10))
            .savereg regname, disp+FpGprs+slot*8
        ENDIF
        slot = slot + 1
    ENDM
ENDM
fp_load_gprs MACRO base, disp
    LOCAL slot
    slot = 0
    FOR regname, <rbx,rcx,rdx,rbp,rsi,rdi,r8,r9,r10,r11,r12,r13,r14,r15>
        mov regname, [base+disp+FpGprs+slot*8]
        slot = slot + 1
    ENDM
ENDM
fp_save_caller MACRO disp
    fp_save rsp, disp
    FOR n, <6,7,8,9,10,11,12,13,14,15>
        .savexmm128 xmm&n, disp+0a0h+16*n
    ENDM
    fp_store_gprs rsp, disp, 1
ENDM

.code
PUBLIC RunLegacyVcpu
; Reusable Microsoft x64 primitive: RCX = VMCB PA, RDX = private FpBank*.
; Switch all software-managed GPRs and legacy FP. Requires CR0.EM/TS clear,
; CR4.OSFXSR set, EFER.FFXSR clear. No fixture, allocation or callback here.
RunLegacyVcpu PROC FRAME
    sub rsp, FpBankBytes+8
    .allocstack FpBankBytes+8
    fp_save_caller 0
    .endprolog
    mov rax, rdx
    fp_restore rax, 0
    fp_load_gprs rax, 0
    mov rax, [rsp+FpGprs+8]       ; original RCX (VMCB PA)
    vmrun rax
    mov rax, [rsp+FpGprs+16]      ; original RDX, never an L2-owned pointer
    fp_save rax, 0
    fp_store_gprs rax, 0, 0
    fp_restore rsp, 0
    fp_load_gprs rsp, 0
    add rsp, FpBankBytes+8
    ret
RunLegacyVcpu ENDP

; Test harness only. The genuine caller stays on a private stack frame,
; disjoint from the synthetic L1 bank and from the primitive's own frame.
FpFixture PROC FRAME
    sub rsp, 20h+FpBankBytes+8     ; shadow space, aligned bank, workspace*
    .allocstack 20h+FpBankBytes+8
    fp_save_caller 20h
    .endprolog
    mov [rsp+20h+FpBankBytes], rdx
    mov rax, rdx
    fp_restore rax, FpSeed
    fp_load_gprs rax, FpSeed       ; includes real RCX/RDX arguments to primitive
    call RunLegacyVcpu
    mov rax, [rsp+20h+FpBankBytes]
    fp_save rax, FpLive           ; independent sample of the live restored L1
    fp_store_gprs rax, FpLive, 0
    fp_restore rsp, 20h
    fp_load_gprs rsp, 20h
    add rsp, 20h+FpBankBytes+8
    ret
FpFixture ENDP

PUBLIC ObserveLegacyRun
; Observe the genuine caller on both sides, outside even the test harness.
; Only RAX (VMCB-managed in L2, volatile in L1) and RFLAGS are scratch.
ObserveLegacyRun PROC FRAME
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    mov [rsp+20h], rdx
    mov rax, rdx
    fp_save rax, FpBefore
    fp_store_gprs rax, FpBefore, 0
    call FpFixture
    mov rax, [rsp+20h]
    fp_save rax, FpAfter
    fp_store_gprs rax, FpAfter, 0
    add rsp, 28h
    ret
ObserveLegacyRun ENDP

include fpstate_guest.inc
END
