/** @file linux_acpi_layout_test.c Fixed in-memory host verifier, no firmware or guest execution.
 * From a repository-root x64 VS Developer Command Prompt:
 * @verbatim
 * cl /nologo /std:c11 /O2 /W4 /WX lab\tests\linux_acpi_layout_test.c /Fobuild\linux_acpi_layout_test.obj /Febuild\linux_acpi_layout_test.exe && build\linux_acpi_layout_test.exe
 * @endverbatim
 */
#include <stdio.h>
#include <string.h>
#include "../../firmware/linux_acpi_layout.h"

static uint8_t ram[LinuxRamBytes], before[LinuxRamBytes];
#define CHECK(e) do { if (!(e)) { fprintf(stderr,"line %d: %s\n",__LINE__,#e); return 1; } } while (0)

/* Starting from a valid published SDT, offset a single changed field with
 * the opposite checksum delta. Undo only those two fixture writes; the final
 * RAM comparison also catches writes elsewhere by the validator. */
static int rejects_field(unsigned gpa, unsigned field, uint8_t mask)
{
    uint8_t *t = ram + gpa;
    const uint8_t *original = before + gpa;
    uint8_t changed = (uint8_t)(original[field] ^ mask);
    uint8_t checksum = (uint8_t)(original[9] + original[field] - changed);
    t[field] = changed;
    t[9] = checksum;
    int refused = !linux_acpi_valid(ram, sizeof ram) && t[field] == changed && t[9] == checksum;
    if (!refused) fprintf(stderr, "guest SDT %#x field %u mask %#x\n", gpa, field, (unsigned)mask);
    t[field] = original[field];
    t[9] = original[9];
    return refused;
}

/** Exercise the real pinned placement code with a small protocol-compatible byte
 * fixture (not a bootable kernel). Check rejection atomicity against the entire
 * image, including poisoned backing for the nonpresent PC hole. */
int main(void)
{
    uint8_t image[4096] = {0}, rd[] = {0xa5}, cmd[] = "test";
    LinuxLayout layout;
    image[0x1fe]=0x55; image[0x1ff]=0xaa; image[0x200]=0xeb; image[0x201]=0x6a;
    linux_put32(image+0x202,0x53726448); image[0x206]=15; image[0x207]=2;
    image[0x211]=1; image[0x234]=1; image[0x236]=1;
    linux_put32(image+0x1f4,(4096-2560)/16); linux_put32(image+0x248,512); linux_put32(image+0x24c,1);
    linux_put64(image+0x258,0x1000000); linux_put32(image+0x230,LinuxLargePage);
    linux_put32(image+0x260,0x260a000); linux_put32(image+0x22c,LinuxRamBytes-1); linux_put32(image+0x238,32);
    memset(ram+LinuxHoleStart,0x5a,LinuxHighRam-LinuxHoleStart);
    CHECK(!build_linux_layout(ram,sizeof ram,image,sizeof image,rd,sizeof rd,cmd,sizeof cmd,&layout));
    uint8_t *bp = ram + LinuxParams;
    CHECK(bp[0x1e8] == 3 && !linux_le64(bp+0x70) && !linux_acpi_valid(ram,sizeof ram));
    const unsigned conflicts[] = {LinuxParams+0x1e8,LinuxParams+0x70,LinuxParams+0x250,
        LinuxParams+0x2d0+15,LinuxParams+0x2d0+60,PowerRsdpGpa,PowerFacsGpa+LinuxPage-1};
    memcpy(before,ram,sizeof ram);
    for (unsigned i=0; i<sizeof conflicts/sizeof conflicts[0]; ++i) {
        ram[conflicts[i]] ^= 1; before[conflicts[i]] ^= 1;
        CHECK(!linux_acpi_publish(ram,sizeof ram) && !memcmp(before,ram,sizeof ram));
        ram[conflicts[i]] ^= 1; before[conflicts[i]] ^= 1;
    }
    CHECK(!linux_acpi_publish(ram,sizeof ram-1) && linux_acpi_publish(ram,sizeof ram));
    CHECK(linux_acpi_valid(ram,sizeof ram));
    /* Independently encode the allowed deltas; compare all other backing bytes. */
    const uint32_t bounds[] = {0,0x90000,0x91000,0x92000,0xa0000,0x100000,0x8000000};
    const uint8_t types[] = {1,3,4,1,2,1};
    before[LinuxParams+0x1e8]=6; linux_put64(before+LinuxParams+0x70,0x90000);
    for (unsigned i=0; i<6; ++i)
        linux_e820(before+LinuxParams+0x2d0+20*i,bounds[i],bounds[i+1]-bounds[i],types[i]);
    CHECK(power_build_tables(before+0x90000,8192,0x90000));
    CHECK(!memcmp(before,ram,sizeof ram));
    CHECK(!linux_acpi_publish(ram,sizeof ram) && !memcmp(before,ram,sizeof ram));
    const unsigned corrupt[] = {LinuxParams+0x70+7,LinuxParams+0x1e8,LinuxParams+0x2d0+20+16,
        LinuxParams+0x2d0+40+8,LinuxParams+0x2d0+80+16,PowerRsdpGpa+8,PowerRsdpGpa+32,
        PowerRsdpGpa+24,PowerRsdpGpa+PowerXsdtOffset+36,PowerRsdpGpa+PowerFadtOffset+140,
        PowerRsdpGpa+PowerDsdtOffset+7,PowerFacsGpa+4};
    for (unsigned i=0; i<sizeof corrupt/sizeof corrupt[0]; ++i) {
        ram[corrupt[i]] ^= 1; CHECK(!linux_acpi_valid(ram,sizeof ram)); ram[corrupt[i]] ^= 1;
    }
    /* Mutate the already-published guest bytes independently. Include both
     * low and upper address bits: a truncated port/GAS match is not enough. */
    const unsigned fadt_fields[] = {
        8, 131, 46, 47, 48, 51, 52, 53,     /* revision, SCI, SMI/enable/disable */
        56, 59, 60, 64, 67, 68, 72, 76, 80, 84, /* legacy PM/GPE addresses */
        88, 89, 90, 91, 92, 93, 94, 109, 116, 128,
        148, 149, 150, 151, 152, 159,       /* event GAS and full address */
        160, 171, 172, 173, 174, 175, 176, 183, /* PM1b event, control GAS */
        184, 195, 196, 208, 220, 232, 244, 256 /* absent optional GAS */
    };
    for (unsigned i=0; i<sizeof fadt_fields/sizeof fadt_fields[0]; ++i)
        CHECK(rejects_field(0x90080, fadt_fields[i], 1));
    CHECK(rejects_field(0x90080, 114, 0x10)); /* HW_REDUCED_ACPI */
    CHECK(rejects_field(0x90040, 8, 1)); /* XSDT revision */
    CHECK(rejects_field(0x90200, 8, 1)); /* DSDT revision */
    for (unsigned offset=36; offset<50; ++offset)
        CHECK(rejects_field(0x90200, offset, 1)); /* each fixed AML byte */
    ram[PowerFacsGpa+16] = 1; /* global lock is guest-writable, not sealed */
    CHECK(linux_acpi_valid(ram,sizeof ram));
    ram[PowerFacsGpa+16] = 0;
    CHECK(!memcmp(before,ram,sizeof ram));
    puts("PASS: host ACPI publication, fixed power fields and S5 AML, writable FACS, unchanged backing");
    return 0;
}
