option casemap:none
.code
ALIGN 16
PUBLIC PairStart, PairEnd
; Copied verbatim at offset 123h of both code frames. The only operands
; naming memory are fixed guest linear addresses in the witness page.
PairStart LABEL BYTE
    mov eax, 1a040h
    mov dword ptr [rax], 50353038h
    hlt
    mov eax, 1a000h
    mov byte ptr [rax], 0b6h
    hlt
PairEnd LABEL BYTE
END
