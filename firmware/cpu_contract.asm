option casemap:none

.code
PUBLIC CpuStart, CpuStop, CpuEnd
; Copied to the long-mode guest code page; only blob-local branches.
; The evidence page begins at RSP - 0ff0h (entry RSP = 7ff0h).
CpuStart LABEL BYTE
    xor eax, eax
    xor ecx, ecx
cpu_leaf_vendor LABEL BYTE
    cpuid
    cmp eax, 1
    jne cpu_bad_vendor
    cmp ebx, 68747541h            ; Auth
    jne cpu_bad_vendor
    cmp edx, 69746e65h            ; enti
    jne cpu_bad_vendor
    cmp ecx, 444d4163h            ; cAMD
    jne cpu_bad_vendor

    mov eax, 1
    xor ecx, ecx
cpu_leaf_features LABEL BYTE
    cpuid
    mov [rsp-0ff0h+4], ecx
    mov [rsp-0ff0h+8], edx
    test eax, eax
    jne cpu_bad_features
    cmp ebx, 00010000h
    jne cpu_bad_features
    cmp ecx, 80000000h
    jne cpu_bad_features
    cmp edx, 07000001h
    jne cpu_bad_features
    shr ebx, 16                   ; logical count; APIC ID checked zero above
    mov [rsp-0ff0h], ebx

    mov eax, 40000000h
    xor ecx, ecx
cpu_leaf_hv LABEL BYTE
    cpuid
    cmp eax, 40000000h
    jne cpu_bad_hv
    cmp ebx, 614c7648h            ; HvLa
    jne cpu_bad_hv
    cmp ecx, 444d4162h            ; bAMD
    jne cpu_bad_hv
    cmp edx, 74736554h            ; Test
    jne cpu_bad_hv

    mov eax, 21
    movd xmm0, eax
    mov eax, 7
    xor ecx, ecx
cpu_leaf_seven LABEL BYTE
    cpuid
    test eax, eax
    jne cpu_bad_leaf7
    test ebx, ebx
    jne cpu_bad_leaf7
    test ecx, ecx
    jne cpu_bad_leaf7
    test edx, edx
    jne cpu_bad_leaf7
    paddd xmm0, xmm0
    movd eax, xmm0
    mov [rsp-0ff0h+12], eax
    cmp eax, 42
    jne cpu_bad_sse2
    mov dword ptr [rsp-0ff0h+16], 43505531h

    mov ecx, 0deadbeefh
CpuStop LABEL BYTE
    rdmsr
    hlt
cpu_bad_vendor:
    mov eax, 0bad00001h
    hlt
cpu_bad_features:
    mov eax, 0bad00002h
    hlt
cpu_bad_hv:
    mov eax, 0bad00003h
    hlt
cpu_bad_leaf7:
    mov eax, 0bad00004h
    hlt
cpu_bad_sse2:
    mov eax, 0bad00005h
    hlt
CpuEnd LABEL BYTE

.const
ALIGN 4
PUBLIC CpuCpuidOffsets
CpuCpuidOffsets LABEL DWORD
    dd cpu_leaf_vendor - CpuStart
    dd cpu_leaf_features - CpuStart
    dd cpu_leaf_hv - CpuStart
    dd cpu_leaf_seven - CpuStart
END
