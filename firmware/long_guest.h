#ifndef HV_LONG_GUEST_H
#define HV_LONG_GUEST_H

enum { LongRootGpa=0x2000, LongStackGpa=0x7000, LongInitialRsp=0x7ff0 };

static void long_guest_state(UINT8 *vmcb, EFI_PHYSICAL_ADDRESS base)
{
    /* CR3 and its links are GPAs. The walk frames also need NPT translations. */
    FixedNpt *guest=(FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    FixedNpt *npt=(FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    for (UINTN level=0; level<NptTableCount; ++level) {
        UINTN gpa=LongRootGpa+level*EFI_PAGE_SIZE;
        npt->table[3][gpa/EFI_PAGE_SIZE]=(base+(GuestTables+level)*EFI_PAGE_SIZE)|7;
        if (level+1<NptTableCount) guest->table[level][0]=(gpa+EFI_PAGE_SIZE)|0x23; /* P/RW/A */
    }
    guest->table[3][FixedGuestGpa/EFI_PAGE_SIZE]=FixedGuestGpa|0x21; /* P/A, read only */
    guest->table[3][LongStackGpa/EFI_PAGE_SIZE]=LongStackGpa|0x63; /* P/RW/A/D */
    npt->table[3][LongStackGpa/EFI_PAGE_SIZE]=(base+GuestStack*EFI_PAGE_SIZE)|7;

    /* AMD APM vol. 2 §15.5.1: CS.L=1, CS.D=0 with LME, PAE, PE and PG. */
    for (UINTN seg=Es; seg<=Ds; seg+=16) {
        FIELD(UINT16,seg)=(seg==Cs)?8:16;
        FIELD(UINT16,seg+2)=(seg==Cs)?0xa9b:0xc93;
        FIELD(UINT32,seg+4)=0xffffffffu;
    }
    FIELD(UINT64,GuestEfer)=SVME|(1u<<8)|(1u<<10); /* LME, LMA */
    FIELD(UINT64,Cr4)=1u<<5; /* PAE */
    FIELD(UINT64,Cr3)=LongRootGpa;
    FIELD(UINT64,Cr0)=0x80010031u; /* PG/WP/NE/ET/PE */
    FIELD(UINT64,Rsp)=LongInitialRsp;
}

#ifdef HV_GUEST_IDT
static void long_guest_gate(UINT8 *vmcb, EFI_PHYSICAL_ADDRESS base, UINT8 vector, UINT64 handler)
{
    enum { Gdtr=0x460, Idtr=0x480, DescGpa=0x6000, IdtOffset=0x100 };
    UINT64 *gdt=(UINT64 *)(vmcb+GuestDescriptors*EFI_PAGE_SIZE);
    FixedNpt *guest=(FixedNpt *)(vmcb+GuestTables*EFI_PAGE_SIZE);
    FixedNpt *npt=(FixedNpt *)(vmcb+Pml4*EFI_PAGE_SIZE);
    guest->table[3][DescGpa/EFI_PAGE_SIZE]=DescGpa|0x21;
    npt->table[3][DescGpa/EFI_PAGE_SIZE]=(base+GuestDescriptors*EFI_PAGE_SIZE)|5;
    /* Pre-accessed 64-bit code and flat data; the descriptor page is read-only. */
    gdt[1]=0x00af9b000000ffffull;
    gdt[2]=0x00cf93000000ffffull;
    FIELD(UINT32,Gdtr+4)=3*8-1;
    FIELD(UINT64,Gdtr+8)=DescGpa;
    UINT64 *gate=gdt+(IdtOffset+16*vector)/8;
    gate[0]=(handler&0xffff)|(8ull<<16)|(0x8eull<<40)|((handler&0xffff0000)<<32);
    gate[1]=handler>>32; /* present DPL0 interrupt gate, IST=0, selector 8 */
    FIELD(UINT32,Idtr+4)=16*(vector+1)-1;
    FIELD(UINT64,Idtr+8)=DescGpa+IdtOffset;
}
#endif
#endif
