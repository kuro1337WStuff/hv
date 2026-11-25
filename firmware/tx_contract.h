#ifndef HV_TX_CONTRACT_H
#define HV_TX_CONTRACT_H
#include "guest_tx_capture.h"

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern const UINT8 TxStart[], TxEnd[], TxIn[], TxOut[], TxDone[];

/* L1's oracle is independent of the copied guest image. These are raw bytes,
 * not line-oriented console text; the trailing C terminator is not on the wire. */
static const UINT8 tx_expected[]="EARLY:console!\r\nLATE:terminal!\r\n";
enum { TxEarly=16, TxShort=24, TxFull=sizeof(tx_expected)-1,
       TxHlt=0x78, TxIoio=0x7b, TxBudget=2+3*TxFull };
typedef struct {
    UINT32 early_count, early_hash, count, hash, busy, lsr, cookie;
} TxEvidence;
_Static_assert(TxFull==32 && LongStackGpa==0x7000 && sizeof(TxEvidence)==28,"TX guest ABI");

static UINT64 tx_ip(const UINT8 *site) { return FixedGuestGpa+((UINTN)site-(UINTN)TxStart); }

static UINT32 tx_hash(const UINT8 *p, UINTN n)
{
    UINT32 hash=0x811c9dc5u;
    for (UINTN i=0; i<n; ++i) hash=(hash^p[i])*0x01000193u;
    return hash;
}

static void tx_words(const UINT8 *bytes, UINTN count, BOOLEAN short_arm)
{
    static const char *const full[]={"TX 00 ","TX 08 ","TX 10 ","TX 18 "};
    static const char *const prefix[]={"SHORT 00 ","SHORT 08 ","SHORT 10 "};
    for (UINTN start=0; start<count; start+=8) {
        UINT64 word=0;
        for (UINTN i=start; i<count && i<start+8; ++i) word=(word<<8)|bytes[i];
        pkg_observe(short_arm ? prefix[start/8] : full[start/8],word);
    }
}

/* Only two immutable, single-byte, scalar sites. Neither ASIZE nor SEG selects
 * a memory operand here. EXITINFO2 corroborates, never supplies, the resume IP. */
static BOOLEAN tx_decode(UINT8 *vmcb, UINT16 dx, BOOLEAN input)
{
    UINT64 ip=tx_ip(input ? TxIn : TxOut), info=FIELD(UINT64,Info1);
    return FIELD(UINT64,ExitCode)==TxIoio && FIELD(UINT64,Rip)==ip &&
           FIELD(UINT64,Info2)==ip+1 && (UINT16)(info>>16)==dx &&
           dx==(input ? Uart8250Base+5 : Uart8250Base) &&
           (info&0x7d)==(UINT64)(0x10|input);
}

static const char *tx_arm(UINT8 *vmcb, UINTN size, BOOLEAN short_arm)
{
    UINT8 wire[TxFull+1]; /* sentinel tail incl. the first byte past each capacity */
    UINTN limit=short_arm ? TxShort : TxFull;
    GuestTxCapture c;
    __stosb((UINT8 *)&c,0,sizeof(c));
    c.bytes=wire;
    c.capacity=limit;
    __stosb(wire,0xa5,sizeof(wire));
    pkg_reset(vmcb); /* all-deny IOPM, cold guest RAM and VMCB for each arm */
    __movsb(vmcb+Code*EFI_PAGE_SIZE,TxStart,size);
    long_guest_state(vmcb,(UINTN)vmcb);
    volatile TxEvidence *e=(volatile TxEvidence *)(vmcb+GuestStack*EFI_PAGE_SIZE);
    UINT64 frame[14];
    __stosb((UINT8 *)frame,0,sizeof(frame));
    const char *error=NULL;
    unsigned entries;
    for (entries=1;; ++entries) {
        FIELD(UINT64,ExitCode)=~0ull;
        FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
        RunGprGuest((UINTN)vmcb,frame);
        FIELD(UINT8,TlbControl)=0;
        if (FIELD(UINT64,ExitCode)==TxHlt) break;
        if (entries==TxBudget) { error="TX EXIT BUDGET"; break; }
        BOOLEAN input=FIELD(UINT64,Rip)==tx_ip(TxIn);
        UINT16 dx=(UINT16)frame[2];
        if (!tx_decode(vmcb,dx,input)) { error="TX IO DECODE"; break; }
        if (!input && c.count==TxEarly &&
            (e->early_count!=c.count || e->early_hash!=tx_hash(wire,TxEarly) ||
             c.uart.polls_busy!=TxEarly || c.uart.busy || e->cookie)) {
            error="TX EARLY CHECKPOINT"; break;
        }
        UINT64 ax=FIELD(UINT64,Rax);
        UINT8 byte=(UINT8)ax;
        GuestTxResult result=guest_tx_access(&c,dx,input,&byte);
        if (result==GuestTxOverflow) {
            if (!short_arm || input || c.count!=limit || byte!=tx_expected[TxShort])
                error="TX UNEXPECTED OVERFLOW";
            break; /* no RIP advance, guest success, or re-entry after refusal */
        }
        if (result!=GuestTxOk) { error="TX IO REFUSED"; break; }
        if (input) FIELD(UINT64,Rax)=(ax&~0xffull)|byte;
        FIELD(UINT64,Rip)=tx_ip(input ? TxIn : TxOut)+1;
    }

    tx_words(wire,c.count<limit ? c.count : limit,short_arm);
    pkg_observe(short_arm ? "SHORT COUNT " : "TX COUNT ",c.count);
    pkg_observe(short_arm ? "SHORT EXIT " : "TX EXIT ",FIELD(UINT64,ExitCode));
    pkg_observe(short_arm ? "SHORT RIP " : "TX RIP ",FIELD(UINT64,Rip));
    pkg_observe(short_arm ? "SHORT RAX " : "TX RAX ",FIELD(UINT64,Rax));
    pkg_observe(short_arm ? "SHORT ENTRY " : "TX ENTRIES ",entries);
    pkg_observe(short_arm ? "SHORT CK " : "L2 COOKIE ",e->cookie);
    if (short_arm) pkg_observe("OVERFLOW ",c.overflow);
    else {
        pkg_observe("L2 EARLY NH ",((UINT64)e->early_count<<32)|e->early_hash);
        pkg_observe("L2 FINAL NH ",((UINT64)e->count<<32)|e->hash);
        pkg_observe("L2 BUSY LSR ",((UINT64)e->busy<<32)|e->lsr);
    }
    if (error) return error;
    if (c.count!=limit || c.uart.count || c.uart.busy || c.uart.polls_busy!=limit)
        return "TX CAPTURE STATE";
    for (UINTN i=0; i<limit; ++i) if (wire[i]!=tx_expected[i]) return "TX WIRE BYTES";
    for (UINTN i=limit; i<sizeof(wire); ++i) if (wire[i]!=0xa5) return "TX GUARD CHANGED";
    if (e->early_count!=TxEarly || e->early_hash!=tx_hash(wire,TxEarly)) return "TX EARLY EVIDENCE";
    if (short_arm) {
        if (!c.overflow || entries!=2+3*TxShort || !tx_decode(vmcb,(UINT16)frame[2],FALSE) ||
            FIELD(UINT64,Rax)!=tx_expected[TxShort] || e->cookie || e->count || e->hash || e->busy || e->lsr)
            return "TX NEGATIVE TERMINAL";
    } else {
        if (c.overflow || entries!=TxBudget || FIELD(UINT64,ExitCode)!=TxHlt ||
            FIELD(UINT64,Rip)!=tx_ip(TxDone) || FIELD(UINT64,Rax)!=0x444f4e45 ||
            FIELD(UINT64,Rsp)!=LongInitialRsp) return "TX GUEST TERMINAL";
        if (e->cookie!=0xb060cafeu || e->count!=c.count || e->hash!=tx_hash(wire,c.count) ||
            e->busy!=c.uart.polls_busy || e->lsr!=Uart8250Ready) return "TX L2 EVIDENCE";
    }
    return NULL;
}

static const char *tx_contract_run(UINT8 *vmcb)
{
    UINTN size=(UINTN)TxEnd-(UINTN)TxStart;
    if (size<=8 || size>EFI_PAGE_SIZE || tx_ip(TxIn)!=FixedGuestGpa+2 ||
        tx_ip(TxOut)!=FixedGuestGpa+4 || tx_ip(TxDone)!=FixedGuestGpa+6 ||
        TxIn[0]!=0xec || TxOut[0]!=0xee || TxDone[0]!=0xf4) return "TX BLOB LAYOUT";
    const char *error=tx_arm(vmcb,size,FALSE);
    return error ? error : tx_arm(vmcb,size,TRUE);
}
#endif
