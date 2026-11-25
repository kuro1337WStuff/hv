option casemap:none

.code
PUBLIC FloorStart, FloorEnd
; Fixed entry and intercept slots. The linker alignment of .code is at least
; 16; no external references or self-modification in the copied image.
ALIGN 16
FloorStart LABEL BYTE
    jmp floor_begin
ALIGN 16
floor_vendor:
    cpuid
    jmp floor_got_vendor
ALIGN 16
floor_features:
    cpuid
    jmp floor_got_features
ALIGN 16
floor_again:
    cpuid
    jmp floor_got_again
ALIGN 16
floor_extmax:
    cpuid
    jmp floor_got_extmax
ALIGN 16
floor_ext:
    cpuid
    jmp floor_got_ext
ALIGN 16
floor_hv:
    cpuid
    jmp floor_got_hv
ALIGN 16
floor_seven:
    cpuid
    jmp floor_got_seven
ALIGN 16
floor_stop:
    rdmsr
    hlt

floor_begin:
    lea rdi, [rsp-0ff0h]        ; private guest evidence, LongStackGpa
    xor eax, eax
    xor ecx, ecx
    jmp floor_vendor
floor_got_vendor:
    cmp eax, 1
    jne floor_bad_leaf
    cmp ebx, 68747541h
    jne floor_bad_leaf
    cmp edx, 69746e65h
    jne floor_bad_leaf
    cmp ecx, 444d4163h
    jne floor_bad_leaf
    mov eax, 1
    xor ecx, ecx
    jmp floor_features
floor_got_features:
    mov [rdi+48], eax
    mov [rdi+52], edx
    mov r8d, 1
    jmp floor_check_features
floor_got_again:
    mov [rdi+56], edx
    xor r8d, r8d
floor_check_features:           ; same architectural reply for either ECX input
    cmp eax, 00020f32h
    jne floor_bad_leaf
    cmp ebx, 00010000h
    jne floor_bad_leaf
    cmp ecx, 80000000h
    jne floor_bad_leaf
    cmp edx, 07008161h
    jne floor_bad_leaf
    test r8d, r8d
    jz floor_request_extmax
    mov eax, 1
    mov ecx, 0a5a5a5a5h
    jmp floor_again
floor_request_extmax:
    mov eax, 80000000h
    xor ecx, ecx
    jmp floor_extmax
floor_got_extmax:
    cmp eax, 80000001h
    jne floor_bad_leaf
    or ebx, ecx
    or ebx, edx
    jne floor_bad_leaf
    mov eax, 80000001h
    xor ecx, ecx
    jmp floor_ext
floor_got_ext:
    mov [rdi+60], edx
    cmp eax, 00020f32h
    jne floor_bad_leaf
    or ebx, ecx
    jne floor_bad_leaf
    cmp edx, 21008161h
    jne floor_bad_leaf
    mov eax, 40000000h
    xor ecx, ecx
    jmp floor_hv
floor_got_hv:
    cmp eax, 40000000h
    jne floor_bad_leaf
    cmp ebx, 614c7648h
    jne floor_bad_leaf
    cmp ecx, 444d4162h
    jne floor_bad_leaf
    cmp edx, 74736554h
    jne floor_bad_leaf

    mov eax, 5
    xor edx, edx
    mov ebx, 9
    xor ecx, ecx
    mov r8d, 11h
    mov r9d, 22h
    cmpxchg8b qword ptr [rdi]   ; EDX:EAX=5 -> memory becomes ECX:EBX=9
    setz r10b
    cmovz r8d, r9d
    movzx r10d, r10b
    mov [rdi+16], r10d
    mov [rdi+24], r8d
    cmp r10d, 1
    jne floor_bad_integer
    cmp r8d, 22h
    jne floor_bad_integer
    cmp qword ptr [rdi], 9
    jne floor_bad_integer

    mov eax, 5                 ; wrong compare; alteration to 17 must not occur
    xor edx, edx
    mov ebx, 17
    xor ecx, ecx
    mov r8d, 33h
    cmpxchg8b qword ptr [rdi]
    setz r10b
    cmovz r8d, r9d
    movzx r10d, r10b
    mov [rdi+20], r10d
    mov [rdi+28], r8d
    mov [rdi+8], eax
    mov [rdi+12], edx
    test r10d, r10d
    jnz floor_bad_integer
    cmp r8d, 33h
    jne floor_bad_integer
    cmp qword ptr [rdi+8], 9
    jne floor_bad_integer
    cmp qword ptr [rdi], 9
    jne floor_bad_integer

    db 0fh, 1fh, 47h, 20h      ; NOP dword ptr [rdi+32], not a zero-length pseudo-op
    cmp dword ptr [rdi+32], 6e6f706ch
    jne floor_bad_integer
    mov eax, 21
    movd xmm0, eax
    mov eax, 7
    xor ecx, ecx
    jmp floor_seven
floor_got_seven:
    or eax, ebx
    or eax, ecx
    or eax, edx
    jne floor_bad_leaf
    paddd xmm0, xmm0
    movd eax, xmm0
    mov [rdi+36], eax
    cmp eax, 42
    jne floor_bad_sse
    mov dword ptr [rdi+40], 464c5231h
    mov ecx, 0deadbeefh
    jmp floor_stop
floor_bad_leaf:
    mov eax, 0bad10001h
    hlt
floor_bad_integer:
    mov eax, 0bad10002h
    hlt
floor_bad_sse:
    mov eax, 0bad10003h
    hlt
FloorEnd LABEL BYTE
END
