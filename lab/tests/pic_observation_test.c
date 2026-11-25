/* Compile against either PIC source tree; exercise its actual pkg_observe and
 * screen, never the firmware entry point. The only platform adapters here are
 * interrupt-disable (ordinary process) and halt (return the refusal to the test).
 * Receipt values are fixture inputs, not guest measurements. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#undef NULL /* EDK Base.h provides its own C definition. */
#include <l1_handoff.c>

static jmp_buf refusal;
void L1DisableInterrupts(void) { /* No interrupts to mask in this process. */ }
__declspec(noreturn) void L1Halt(void) { longjmp(refusal, 1); }

static int append(const char *label, UINT64 value)
{
    if (setjmp(refusal)) return 0;
    pkg_observe(label,value);
    return 1;
}

static const struct {const char *label; UINT64 value;} receipts[] = {
    {"OK EXIT ",0x78}, {"OK RIP ",0x10bb}, {"PROBE ",0xfb}, {"MASTER IMR ",0xa5},
    {"SLAVE IMR ",0x5a}, {"OK COOKIE ",0x50494331}, {"OK ENTRIES ",0x13},
    {"BAD BYTE ",4}, {"BAD RIP ",0x107a}, {"ACCEPTED ",0}, {"MASTER STATE ",0x4a5},
    {"SLAVE STATE ",0x200}, {"BAD COOKIE ",0}, {"BAD ENTRIES ",0xd}
};

/* Use private RAM with guards, not GOP or a physical framebuffer. Set the
 * viewport to the minimum preflight geometry for this profile. */
enum { W=404, H=LOG_Y(LOG_ROWS-1)+14, Guard=32 };
static UINT32 fb[Guard+W*H+Guard];
static int prior_intact(unsigned n)
{
    char expected[48];
    if (strcmp(log_lines[0],"HV:HANDOFF")) return 0;
    for (unsigned i=0;i<n;++i) {
        snprintf(expected,sizeof expected,"%s%016llX",receipts[i].label,(unsigned long long)receipts[i].value);
        if (!log_lines[i+1] || strcmp(expected,log_lines[i+1])) return 0;
    }
    return 1;
}

int main(int argc,char **argv)
{
    if (argc!=2 || (strcmp(argv[1],"legacy") && strcmp(argv[1],"candidate"))) return 2;
    const int legacy=!strcmp(argv[1],"legacy");
    const unsigned count=sizeof receipts/sizeof receipts[0];
    memset(fb,0x5a,sizeof fb);
    pixels=fb+Guard; width=pitch=W; height=H;
    log_lines[logged++]="HV:HANDOFF";
    for (unsigned i=0;i<count;++i) {
        if (!append(receipts[i].label,receipts[i].value)) {
            if (!legacy || i!=10 || logged!=11 || log_lines[logged] || !prior_intact(i)) return 3;
            printf("legacy REFUSED [%s] length=%zu used=%u/%u; previous receipts intact\n",
                   receipts[i].label,strlen(receipts[i].label),logged,LOG_ROWS);
            /* Change only the trailing space at the same insertion point. */
            if (!append("MASTER STATE",0x4a5) || strcmp(log_lines[11],"MASTER STATE00000000000004A5")) return 4;
            puts("same slot: 12-character control accepted");
            break;
        }
        if (legacy && i==10) return 5;
    }
    if (!legacy) {
        if (logged!=count+1 || !prior_intact(count)) return 6;
        printf("candidate: full sequence verified, %u/%u rows; %u spare\n",logged,LOG_ROWS,LOG_ROWS-logged);
    }
    /* Discriminate width and exhaustion. A refused append must not publish a
     * pointer or overwrite any accepted evidence (checked again after halt). */
    unsigned used=logged;
    if (append("0123456789ABCDEFG",1) || logged!=used || log_lines[used]) return 7;
    while (logged<LOG_ROWS) {
        if (!append("BOUNDARY ",logged)) return 8;
    }
    const unsigned last=LOG_ROWS-1;
    char expected[40];
    snprintf(expected,sizeof expected,"BOUNDARY %016X",last);
    if (append("X",0) || logged!=LOG_ROWS || strcmp(log_lines[last],expected) ||
        !prior_intact(legacy ? 10 : count)) return 9;
    /* The failure screen must actually paint the last row; guard words outside
     * the viewport must survive both the clearing pass and the glyph draws. */
    int ink=0;
    for (unsigned i=LOG_Y(last)*W;i<H*W;++i) if (pixels[i]==0xffffff) ink=1;
    for (unsigned i=0;i<Guard;++i) if (fb[i]!=0x5a5a5a5a || fb[Guard+W*H+i]!=0x5a5a5a5a) return 10;
    if (!ink) return 11;
    printf("%s: row %u drawn; row %u refused; framebuffer guards intact\n",argv[1],LOG_ROWS,LOG_ROWS+1);
    return 0;
}
