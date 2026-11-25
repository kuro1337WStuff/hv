option casemap:none
.code
PUBLIC EferStart, EferEnd
ALIGN 16
EferStart LABEL BYTE
    mov ecx, 0c0000080h
    jmp first_read
ALIGN 16
first_read:
    rdmsr
    jmp got_first
ALIGN 16
barrier_a:
    cpuid
    jmp got_a
ALIGN 16
barrier_b:
    cpuid
    jmp got_b
ALIGN 16
second_read:
    rdmsr
    jmp got_second
ALIGN 16
efer_stop:
    hlt
got_first:
    mov [rsp-0ff0h], eax
    mov [rsp-0fech], edx
    cmp eax, 500h
    jne bad
    test edx, edx
    jne bad
    mov eax, 7
    xor ecx, ecx
    jmp barrier_a
got_a:
    or eax, ebx
    or eax, ecx
    or eax, edx
    jne bad
    mov eax, 7
    mov ecx, 1
    jmp barrier_b
got_b:
    or eax, ebx
    or eax, ecx
    or eax, edx
    jne bad
    mov ecx, r8d             ; immutable per-arm input, never external
    mov eax, 0feedbeefh
    mov edx, 0badc0deh
    jmp second_read
got_second:
    mov [rsp-0fe8h], eax
    mov [rsp-0fe4h], edx
    cmp eax, 500h
    jne bad
    test edx, edx
    jne bad
    mov dword ptr [rsp-0fe0h], 45464552h
    jmp efer_stop
bad:
    hlt
EferEnd LABEL BYTE
END
