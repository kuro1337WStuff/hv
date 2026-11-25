#ifndef HV_PIC_INIT_H
#define HV_PIC_INIT_H
#include "pic8259.h"

extern void RunGprGuest(UINTN vmcb_pa, UINT64 frame[14]);
extern volatile unsigned linux_vmrun_count;

/* Equal-sized fixed x86-64 instruction groups, not a guest instruction decoder.
 * OUT: MOV AL,imm8; OUT imm8,AL; six NOPs. The variant takes AL from DL.
 * IN: IN AL,imm8; MOV [RCX+disp8],AL; CMP AL,imm8; JE +1; HLT (failure).
 * The last group writes the independent guest cookie and halts. */
#define PIC_OUT(p,v) {0xb0,v,0xe6,p,0x90,0x90,0x90,0x90,0x90,0x90}
#define PIC_IN(p,d,v) {0xe4,p,0x88,0x41,d,0x3c,v,0x74,1,0xf4}
enum { PicVariant=12, PicDone=18, PicStride=10, PicCookie=0x50494331 };
static const UINT8 pic_image[][PicStride]={
    PIC_OUT(0xa1,0xff), PIC_OUT(0x21,0xfb), PIC_IN(0x21,0,0xfb),
    PIC_OUT(0x20,0x11), PIC_OUT(0xa0,0x11),
    PIC_OUT(0x21,0x30), PIC_OUT(0xa1,0x38), PIC_OUT(0x21,4), PIC_OUT(0x21,1),
    PIC_IN(0x21,1,0), PIC_OUT(0x21,0xa5), PIC_IN(0x21,2,0xa5),
    {0x8a,0xc2,0xe6,0xa1,0x90,0x90,0x90,0x90,0x90,0x90}, /* MOV AL,DL; OUT A1h,AL */
    PIC_OUT(0xa1,1), PIC_IN(0xa1,3,0), PIC_OUT(0xa1,0x5a),
    PIC_IN(0xa1,4,0x5a), PIC_IN(0x21,5,0xa5),
    {0xc7,0x41,8,0x31,0x43,0x49,0x50,0xf4}
};
#undef PIC_IN
#undef PIC_OUT
_Static_assert(sizeof(pic_image)==(PicDone+1)*PicStride && sizeof(pic_image)<=EFI_PAGE_SIZE,"PIC image bounds");

/* AMD APM vol. 2, IOIO intercept: require the pinned RIP, next RIP, port,
 * direction and scalar byte size before touching either guest or model. */
static const char *pic_init_run(UINT8 *vmcb)
{
    for (unsigned bad=0;bad<2;++bad) {
        Pic8259 dev;
        UINT64 frame[14];
        pkg_reset(vmcb);
        long_guest_state(vmcb,(UINTN)vmcb);
        __movsb(vmcb+Code*EFI_PAGE_SIZE,&pic_image[0][0],sizeof(pic_image));
        __stosb((UINT8 *)frame,0,sizeof(frame));
        frame[1]=LongStackGpa;
        frame[2]=bad ? 4 : 2; /* only arm difference: slave ICW3 */
        volatile UINT8 *seen=vmcb+GuestStack*EFI_PAGE_SIZE;
        volatile UINT32 *cookie=(volatile UINT32 *)(seen+8);
        __stosb(vmcb+GuestStack*EFI_PAGE_SIZE,0xcc,6);
        pic8259_init(&dev);
        unsigned before=linux_vmrun_count;
        for (unsigned step=0;step<=PicDone;++step) {
            UINT64 base=FixedGuestGpa+step*PicStride;
            const UINT8 *group=pic_image[step];
            FIELD(UINT64,ExitCode)=~0ull;
            FIELD(UINT64,Info1)=FIELD(UINT64,Info2)=0;
            RunGprGuest((UINTN)vmcb,frame);
            FIELD(UINT8,TlbControl)=0;
            UINT64 rip=FIELD(UINT64,Rip), code=FIELD(UINT64,ExitCode);
            unsigned entries=linux_vmrun_count-before;
            if (step==PicDone) {
                pkg_observe("OK EXIT ",code); pkg_observe("OK RIP ",rip);
                pkg_observe("PROBE ",seen[0]); pkg_observe("MASTER IMR ",seen[5]);
                pkg_observe("SLAVE IMR ",seen[4]); pkg_observe("OK COOKIE ",*cookie);
                pkg_observe("OK ENTRIES ",entries);
                if (bad || code!=0x78 || rip!=base+7 || *cookie!=PicCookie || entries!=PicDone+1 ||
                    !pic8259_ready(&dev) || dev.master.imr!=0xa5 || dev.slave.imr!=0x5a ||
                    seen[0]!=0xfb || seen[1]!=0 || seen[2]!=0xa5 || seen[3]!=0 || seen[4]!=0x5a || seen[5]!=0xa5)
                    return "PIC COMPLETION";
                break;
            }
            BOOLEAN input=group[0]==0xe4;
            UINT64 site=base+(input ? 0 : 2), info=FIELD(UINT64,Info1);
            UINT8 port=group[input ? 1 : 3], byte=(UINT8)FIELD(UINT64,Rax);
            if (code!=0x7b || rip!=site || FIELD(UINT64,Info2)!=site+2 ||
                (UINT16)(info>>16)!=port || (info&0x7d)!=(UINT64)(0x10|input)) {
                pkg_observe("ARM ",bad); pkg_observe("STEP ",step); pkg_observe("EXIT ",code);
                pkg_observe("RIP ",rip); pkg_observe("INFO1 ",info); pkg_observe("NEXT ",FIELD(UINT64,Info2));
                return "PIC FIXED IO";
            }
            if (bad && step==PicVariant) {
                if (byte!=4 || dev.master.phase!=PIC8259_READY || dev.master.imr!=0xa5 ||
                    dev.slave.phase!=PIC8259_ICW3 || dev.slave.imr!=0) return "PIC DENY PRESTATE";
                int accepted=pic8259_write(&dev,port,1,byte);
                pkg_observe("BAD BYTE ",byte); pkg_observe("BAD RIP ",FIELD(UINT64,Rip));
                pkg_observe("ACCEPTED ",(UINT64)accepted);
                pkg_observe("MASTER STATE ",((UINT64)dev.master.phase<<8)|dev.master.imr);
                pkg_observe("SLAVE STATE ",((UINT64)dev.slave.phase<<8)|dev.slave.imr);
                pkg_observe("BAD COOKIE ",*cookie); pkg_observe("BAD ENTRIES ",linux_vmrun_count-before);
                if (accepted || dev.master.phase!=PIC8259_READY || dev.master.imr!=0xa5 ||
                    dev.slave.phase!=PIC8259_ICW3 || dev.slave.imr!=0 || FIELD(UINT64,Rip)!=site ||
                    *cookie || linux_vmrun_count-before!=PicVariant+1 || seen[0]!=0xfb || seen[1]!=0 ||
                    seen[2]!=0xa5 || seen[3]!=0xcc || seen[4]!=0xcc || seen[5]!=0xcc)
                    return "PIC DENY EFFECT";
                return NULL; /* never advance or re-enter */
            }
            if (input) {
                if (!pic8259_read(&dev,port,1,&byte)) return "PIC READ";
                FIELD(UINT64,Rax)=(FIELD(UINT64,Rax)&~0xffull)|byte;
            } else if (byte!=(step==PicVariant ? 2 : group[1]) || !pic8259_write(&dev,port,1,byte)) {
                pkg_observe("STEP ",step); pkg_observe("BYTE ",byte);
                return "PIC WRITE";
            }
            FIELD(UINT64,Rip)=site+2;
        }
    }
    return "PIC MISSING DENY";
}
#endif
