#ifndef HV_W03_RELOC_H
#define HV_W03_RELOC_H
/* Private file and loaded image extents are checked by the caller. DIR64
 * fields need not be naturally aligned; read them without an aligned cast. */
static UINT64 w03_le64(const UINT8 *p)
{
    return (UINT64)p[0] | (UINT64)p[1]<<8 | (UINT64)p[2]<<16 | (UINT64)p[3]<<24 |
           (UINT64)p[4]<<32 | (UINT64)p[5]<<40 | (UINT64)p[6]<<48 | (UINT64)p[7]<<56;
}
static BOOLEAN w03_equal(const UINT8 *a, const UINT8 *b, UINT32 n)
{
    while (n>=8) { if (w03_le64(a)!=w03_le64(b)) return FALSE; a+=8; b+=8; n-=8; }
    while (n--) if (*a++!=*b++) return FALSE;
    return TRUE;
}
static BOOLEAN w03_text_matches(const UINT8 *file, const UINT8 *loaded, UINT32 base)
{
    static const UINT32 relocs[]=W03_TEXT_RELOCS;
    _Static_assert(sizeof(relocs)/sizeof(*relocs)==W03_TEXT_RELOC_COUNT,"relocation pin count");
    const UINT8 *raw=file+W03_TEXT_FILE_OFFSET;
    UINT32 pos=0;
    for (UINT32 i=0;i<W03_TEXT_RELOC_COUNT;++i) {
        UINT32 r=relocs[i];
        if (r<W03_TEXT_RVA) return FALSE;
        UINT32 off=r-W03_TEXT_RVA;
        if (off<pos || off>W03_TEXT_LEN-8 || (r<W03_ENTRY_RVA+4 && r+8>W03_ENTRY_RVA) ||
            !w03_equal(raw+pos,loaded+pos,off-pos) ||
            w03_le64(loaded+off)!=w03_le64(raw+off)+(UINT64)base-W03_PREFERRED_BASE) return FALSE;
        pos=off+8;
    }
    return w03_equal(raw+pos,loaded+pos,W03_TEXT_LEN-pos);
}
#endif
