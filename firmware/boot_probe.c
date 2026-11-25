#include <Uefi.h>
#include <intrin.h>

static void print_hex32(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out, UINT32 value)
{
    CHAR16 text[] = L"00000000\r\n";
    for (UINTN i = 8; i != 0; value >>= 4) {
        text[--i] = L"0123456789ABCDEF"[value & 15];
    }
    out->OutputString(out, text);
}

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    (void)image;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *out = st->ConOut;
    EFI_BOOT_SERVICES *bs = st->BootServices;
    int regs[4];
    CHAR16 vendor[13];
    const UINTN vendor_order[] = { 1, 3, 2 }; /* CPUID.0: EBX, EDX, ECX */

    out->ClearScreen(out);
    out->OutputString(out, L"HV boot probe | x86-64 UEFI\r\n\r\nCPU vendor: ");
    __cpuid(regs, 0);
    for (UINTN word = 0; word < 3; ++word) {
        UINT32 bytes = (UINT32)regs[vendor_order[word]];
        for (UINTN i = 0; i < 4; ++i, bytes >>= 8) {
            vendor[4 * word + i] = (CHAR16)(bytes & 0xff);
        }
    }
    vendor[12] = 0;
    out->OutputString(out, vendor);
    out->OutputString(out, L"\r\n");

    __cpuid(regs, (int)0x80000000u);
    UINT32 max_ext = (UINT32)regs[0];
    if (max_ext < 0x80000001u) {
        out->OutputString(out, L"AMD SVM: feature leaf unavailable\r\n");
    } else {
        __cpuid(regs, (int)0x80000001u);
        out->OutputString(out, L"CPUID.80000001h ECX: 0x");
        print_hex32(out, (UINT32)regs[2]);
        if ((regs[2] & (1 << 2)) == 0) {
            out->OutputString(out, L"AMD SVM: not advertised\r\n");
        } else {
            out->OutputString(out, L"AMD SVM: advertised\r\n");
            if (max_ext >= 0x8000000au) {
                __cpuid(regs, (int)0x8000000au);
                out->OutputString(out, L"SVM revision (hex): 0x");
                print_hex32(out, (UINT32)regs[0] & 0xff);
            }
        }
    }

    out->OutputString(out, L"\r\nRead-only probe complete. Esc returns to firmware.\r\n");
    EFI_STATUS status = bs->SetWatchdogTimer(0, 0, 0, NULL);
    if (EFI_ERROR(status)) return status;
    for (;;) {
        UINTN index;
        EFI_INPUT_KEY key;
        status = bs->WaitForEvent(1, &st->ConIn->WaitForKey, &index);
        if (EFI_ERROR(status)) return status;
        status = st->ConIn->ReadKeyStroke(st->ConIn, &key);
        if (status == EFI_NOT_READY) continue;
        if (EFI_ERROR(status)) return status;
        if (key.ScanCode == SCAN_ESC) return EFI_SUCCESS;
    }
}
