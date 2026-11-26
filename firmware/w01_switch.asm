option casemap:none
EXTERN RunGprGuest:PROC
.code
; #VMEXIT may reread the host CS/SS/DS/ES descriptors while guest LDTR is
; still live (AMD APM 15.6). Use a writable, private copy of the firmware
; GDT, not an assumption about permissions on the firmware table. No
; selector changes. Prepare two ten-byte GDTRs without changing hardware.
check_selector MACRO sreg
    mov ax, sreg
    test ax, 7              ; only CPL0/GDT selectors (or null)
    jnz gdt_reject
    movzx eax, ax
    add eax, 7
    cmp eax, r8d
    ja gdt_reject
ENDM
PUBLIC W01PrepareHostGdt
W01PrepareHostGdt PROC
    sgdt fword ptr [rdx]
    movzx r8d, word ptr [rdx]
    cmp r8d, 4095
    ja gdt_reject
    check_selector cs
    check_selector ss
    check_selector ds
    check_selector es
    mov r9, [rdx+2]
    mov word ptr [rdx+10], r8w
    mov [rdx+12], rcx
gdt_copy:
    mov r10b, [r9+r8]
    mov [rcx+r8], r10b
    sub r8, 1
    jnc gdt_copy
    mov eax, 1
    ret
gdt_reject:
    xor eax, eax
    ret
W01PrepareHostGdt ENDP
PUBLIC W01LoadGdtr
W01LoadGdtr PROC
    lgdt fword ptr [rcx]
    ret
W01LoadGdtr ENDP

PUBLIC W01Switch
; RCX: guest VMCB PA; RDX: private GPR bank; R8: separate host VMSAVE PA.
; FS/GS/TR/LDTR and syscall MSRs are NOT all switched by VMRUN. Never let
; C run between guest VMLOAD and restoration. The admitted integer-only
; instruction forms cannot observe or modify x87/SIMD/XSTATE/DR0-3.
W01Switch PROC FRAME
    sub rsp, 38h
    .allocstack 38h
    .endprolog
    mov [rsp+20h], rcx
    mov [rsp+28h], r8
    mov rax, r8
    vmsave rax
    mov rax, rcx
    vmload rax
    call RunGprGuest
    mov rax, [rsp+20h]
    vmsave rax
    mov rax, [rsp+28h]
    vmload rax
    add rsp, 38h
    ret
W01Switch ENDP
END
