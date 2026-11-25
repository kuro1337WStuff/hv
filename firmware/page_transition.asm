option casemap:none
.code
ALIGN 16
PUBLIC TransitionSetup, TransitionSetupEnd
; Fixed 64-bit L2 entry at linear 1000h, not a callable host routine. No
; inputs or stack. Clobbers RDI only; the two stores encode exactly
; MOV EAX,5452414Eh / HLT at linear 8000h. Stop at the intercepted HLT;
; L1 never resumes this setup site, nor accepts an entry address from L2.
TransitionSetup LABEL BYTE
    mov edi, 8000h
    mov dword ptr [rdi], 52414eb8h
    mov word ptr [rdi+4], 0f454h
    hlt
TransitionSetupEnd LABEL BYTE
END
