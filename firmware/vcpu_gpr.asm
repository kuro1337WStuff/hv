option casemap:none

IFDEF HV_INPUT_TEST
.data
PUBLIC linux_vmrun_count
linux_vmrun_count DWORD 0
ENDIF

.code
PUBLIC RunGprGuest
; Microsoft x64: RCX = VMCB PA, RDX = UINT64 frame[14]. VMRUN switches
; RAX/RSP/RIP via the VMCB; the other integer registers switch here.
; The fixed L2 below uses no SIMD or floating-point state.
RunGprGuest PROC FRAME
    push rbx
    .pushreg rbx
    push rbp
    .pushreg rbp
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push r12
    .pushreg r12
    push r13
    .pushreg r13
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    push rdx                     ; frame pointer; align host RSP for the run
    .allocstack 8                ; unwind discards this volatile slot
    .endprolog

    mov rax, rcx
    mov rbx, [rdx+0*8]
    mov rcx, [rdx+1*8]
    mov rbp, [rdx+3*8]
    mov rsi, [rdx+4*8]
    mov rdi, [rdx+5*8]
    mov r8,  [rdx+6*8]
    mov r9,  [rdx+7*8]
    mov r10, [rdx+8*8]
    mov r11, [rdx+9*8]
    mov r12, [rdx+10*8]
    mov r13, [rdx+11*8]
    mov r14, [rdx+12*8]
    mov r15, [rdx+13*8]
    mov rdx, [rdx+2*8]           ; load last: RDX was the frame pointer
IFDEF HV_INPUT_TEST
    inc linux_vmrun_count        ; BSP-only trial: count attempts at the instruction site
ENDIF
    vmrun rax
    mov rax, [rsp]               ; VMRUN restored host RSP; guest RAX is in VMCB
    mov [rax+0*8], rbx
    mov [rax+1*8], rcx
    mov [rax+2*8], rdx
    mov [rax+3*8], rbp
    mov [rax+4*8], rsi
    mov [rax+5*8], rdi
    mov [rax+6*8], r8
    mov [rax+7*8], r9
    mov [rax+8*8], r10
    mov [rax+9*8], r11
    mov [rax+10*8], r12
    mov [rax+11*8], r13
    mov [rax+12*8], r14
    mov [rax+13*8], r15
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    ret
RunGprGuest ENDP

; This guest is copied as raw bytes to GPA 0x1000. It uses only immediates
; and relative branches, so it needs neither relocations nor a data page.
; n and digit are single hex digits: e.g. R10 in round 1 = A501000A5A00010A.
seed_reg MACRO n, digit, regname
    mov regname, 0A50&n&000&digit&5A000&n&0&digit&h
ENDM

; All successful comparisons preserve every unchecked register. Only the
; first mismatch branches to the failure HLT, carrying the register encoding.
require_equal MACRO n, digit
    LOCAL matched
    je short matched
    mov eax, (n SHL 8) + 0&digit&h
    jmp GprFailHlt
matched:
ENDM

check_seed MACRO n, digit, regname
    seed_reg n, digit, rax
    cmp regname, rax
    require_equal n, digit
ENDM

preserved_regs MACRO visit, n
    visit n, 5, rbp
    visit n, 6, rsi
    visit n, 7, rdi
    visit n, 8, r8
    visit n, 9, r9
    visit n, A, r10
    visit n, B, r11
    visit n, C, r12
    visit n, D, r13
    visit n, E, r14
    visit n, F, r15
ENDM

gpr_round MACRO n, stacktop, trapip, resumeip
    mov esp, stacktop
    seed_reg n, 3, rbx
    seed_reg n, 2, rdx
    preserved_regs seed_reg, n
    mov eax, 400000feh
    mov ecx, n
trapip LABEL BYTE
    cpuid
resumeip LABEL BYTE
    ; Validate RAX without scratch. CMP r64,imm32 would sign-extend the
    ; high-bit-set reply; compare both halves before using RAX as scratch.
    cmp eax, 0a1000000h + n
    require_equal n, 0
    shr rax, 32
    require_equal n, 0
    mov eax, 0b2000000h + n
    cmp rbx, rax
    require_equal n, 3
    mov eax, 0c3000000h + n
    cmp rcx, rax
    require_equal n, 1
    mov eax, 0d4000000h + n
    cmp rdx, rax
    require_equal n, 2
    cmp rsp, stacktop
    require_equal n, 4
    preserved_regs check_seed, n
ENDM

PUBLIC GprStart, GprCpuid1, GprAfter1, GprCpuid2, GprAfter2
PUBLIC GprPassHlt, GprFailHlt, GprEnd
GprStart LABEL BYTE
    gpr_round 1, 7ff0h, GprCpuid1, GprAfter1
    gpr_round 2, 7fc0h, GprCpuid2, GprAfter2
    mov eax, 50415353h
GprPassHlt LABEL BYTE
    hlt
GprFailHlt:
    hlt
GprEnd LABEL BYTE
END
