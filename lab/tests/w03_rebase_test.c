/* Native host check only: never execute Windows instructions. The second file
 * is the initialized entry section independently rebased to 64 MiB. */
#include <Uefi.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../build/w03_pin.h"
#include "../../firmware/w03_reloc.h"

static int exact_file(const char *name, UINT8 *out, size_t size)
{
    FILE *f = NULL;
    if (fopen_s(&f, name, "rb")) return 0;
    int ok = fread(out, 1, size, f) == size && fgetc(f) == EOF && !ferror(f);
    return fclose(f) == 0 && ok;
}

int main(int argc, char **argv)
{
    static const UINT32 fixups[] = W03_TEXT_RELOCS;
    const UINT32 base = 0x04000000u;
    UINT8 *raw = malloc(W03_FILE_SIZE), *loaded = malloc(W03_TEXT_LEN);
    int result = 1;
    if (argc != 3 || !raw || !loaded || !exact_file(argv[1],raw,W03_FILE_SIZE) ||
        !exact_file(argv[2],loaded,W03_TEXT_LEN)) goto done;
    if (!w03_text_matches(raw,loaded,base) || w03_text_matches(raw,loaded,base + 4096)) goto done;
    UINT8 *entry = loaded + (W03_ENTRY_RVA-W03_TEXT_RVA);
    if (!w03_entry_prologue(entry)) goto done;
    /* Start from the independently rebased file. Every one-byte change must
     * be refused, including the other fifteen REX prefixes and a missing REX.W. */
    for (unsigned i=0; i<4; ++i) {
        UINT8 original = entry[i];
        for (unsigned b=0; b<256; ++b) {
            if (b==original) continue;
            entry[i]=(UINT8)b;
            if (w03_entry_prologue(entry)) goto done;
        }
        entry[i]=original;
    }
    /* Mutations exercise both paths: an ordinary instruction byte and an
     * actual relocation. Neither change touches the original file. */
    loaded[W03_ENTRY_RVA-W03_TEXT_RVA] ^= 1;
    if (w03_text_matches(raw,loaded,base)) goto done;
    loaded[W03_ENTRY_RVA-W03_TEXT_RVA] ^= 1;
    loaded[fixups[W03_TEXT_RELOC_COUNT-1]-W03_TEXT_RVA+7] ^= 1;
    if (w03_text_matches(raw,loaded,base)) goto done;
    puts("Rebased .text and exact entry accepted; 1020 entry variants, wrong base, changed entry and changed final fixup refused.");
    result = 0;
done:
    free(loaded); free(raw);
    return result;
}
