#ifndef HV_FETCH_FIXTURE_H
#define HV_FETCH_FIXTURE_H

/* The four-level guest tree uses only 4 KiB leaves. No guest VA maps a
 * paging structure, staging page, or L1 address. All nested leaves stay NX. */
static void fetch_fixture_layout(UINT8 *ram)
{
    enum { CodeA = 0x321000, CodeB = 0x634000, StackPage = LinuxStackTop - LinuxPage };
    linux_clear(ram + LinuxGdt, LinuxStackTop - LinuxGdt);
    linux_put64(ram + LinuxGdt + LinuxCs, UINT64_C(0x00af9b000000ffff));
    linux_put64(ram + LinuxGdt + LinuxDs, UINT64_C(0x00cf93000000ffff));
    linux_put64(ram + LinuxPml4, LinuxPdpt | 0x23);
    linux_put64(ram + LinuxPdpt, LinuxPd | 0x23);
    linux_put64(ram + LinuxPd, LinuxPt | 0x23);
    linux_put64(ram + LinuxPt + 8 * (LinuxGdt / LinuxPage), LinuxGdt | 0x63);
    linux_put64(ram + LinuxPt + 8 * (StackPage / LinuxPage), StackPage | 0x63);
    linux_put64(ram + LinuxPt + 8 * 0x81, CodeA | 0x63);
    linux_put64(ram + LinuxPt + 8 * 0x82, CodeB | 0x63);

    /* Fixed CLD cannot clear TF or create a debug shadow. Even the filler is
     * intercepted HLT. Neither a #DB nor a HLT exit admits the next arm. */
    __stosb(ram + CodeA, 0xf4, LinuxPage);
    __stosb(ram + CodeB, 0xf4, LinuxPage);
    ram[CodeA + 0x123] = ram[CodeA + 0x678] = ram[CodeB + 0x123] = 0xfc;
}

static const char *run_fetch_fixture(UINT8 *base, int *precision)
{
    static const struct {
        UINT32 rip, gpa;
        char label[5][9];
    } arms[] = {
        {0x81123, 0x321123, {"A RIP ", "A WALK ", "A RAW ", "A INFO1 ", "A EXIT "}},
        {0x81678, 0x321678, {"B RIP ", "B WALK ", "B RAW ", "B INFO1 ", "B EXIT "}},
        {0x82123, 0x634123, {"C RIP ", "C WALK ", "C RAW ", "C INFO1 ", "C EXIT "}}
    };
    UINT8 *vmcb = base, *ram = base + LinuxRamFirst * LinuxPage;
    int mode = FETCH_INVALID;
    *precision = FETCH_INVALID;
    fetch_fixture_layout(ram);
    linux_build_npt(base);
    for (UINTN i = 0; i < sizeof(arms) / sizeof(arms[0]); ++i) {
        UINT64 frame[14];
        uint64_t walked;
        linux_fetch_vmcb(vmcb, arms[i].rip); /* fresh VMCB only, never the active HSAVE */
        __stosb((UINT8 *)frame, 0, sizeof(frame));
        if (!linux_tree_ok(vmcb) || !linux_guard_ok(base)) return "FIXTURE ISOLATION";
        if (FIELD(UINT64,Cr3) != LinuxPml4 || FIELD(UINT64,Rip) != arms[i].rip ||
            !fetch_walk(ram, LinuxRamBytes, LinuxPml4, arms[i].rip, &walked) ||
            walked != arms[i].gpa || ram[walked] != 0xfc) return "FIXTURE MAPPING";
        if (FIELD(UINT64,Flags) != 0x102 || !(FIELD(UINT32,Exceptions) & (1u << 1)) ||
            FIELD(UINT8,TlbControl) != 1 || FIELD(UINT32,0x0c0) != 0) return "FIXTURE CONTROLS";

        RunGprGuest((UINTN)vmcb, frame); /* one entry per arm, no resume or permission change */
        UINT64 rip = FIELD(UINT64,Rip), raw = FIELD(UINT64,Info2);
        UINT64 info = FIELD(UINT64,Info1), exitcode = FIELD(UINT64,ExitCode);
        pkg_observe(arms[i].label[0], rip);
        pkg_observe(arms[i].label[1], walked);
        pkg_observe(arms[i].label[2], raw);
        pkg_observe(arms[i].label[3], info);
        pkg_observe(arms[i].label[4], exitcode);
        if (!linux_tree_ok(vmcb) || !linux_guard_ok(base)) return "FIXTURE ISOLATION";
        /* Only the nested U/S bit may vary: final GPA, fetch, protection
         * fault, not a write or a guest-page-walk fault. Never mask raw. */
        if (exitcode != 0x400 || (info & ~4ull) != ((1ull << 32) | 0x11) ||
            rip != arms[i].rip) return "FIXTURE FETCH NPF";
        if (FIELD(UINT64,Cr3) != LinuxPml4 || FIELD(UINT64,Rsp) != LinuxStackTop ||
            FIELD(UINT64,Flags) != 0x102 || FIELD(UINT64,Rax) != 0) return "FIXTURE ENTRY STATE";
        for (UINTN r = 0; r < sizeof(frame) / sizeof(frame[0]); ++r)
            if (frame[r] != 0) return "FIXTURE GPR STATE";
        int observed = fetch_precision(walked, raw);
        if (observed != FETCH_EXACT && observed != FETCH_PAGE_BASE) return "FIXTURE RAW GPA";
        if (i && observed != mode) return "FIXTURE MIXED RAW";
        mode = observed;
    }
    *precision = mode;
    return NULL;
}
#endif
