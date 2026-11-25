/* Offline fixed-profile table walker. Treat GPA links/lengths as data and validate
 * them before dereferencing; never cast a GPA to a process or machine address. */
#include <stdio.h>
#include <string.h>
#include "../../firmware/power_tables.h"

#define CHECK(e) do { if (!(e)) { fprintf(stderr, "line %d: %s\n", __LINE__, #e); return 1; } } while (0)

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8*i);
    return v;
}

static unsigned sum(const uint8_t *p, size_t n)
{
    unsigned s = 0;
    for (size_t i = 0; i < n; ++i) s += p[i];
    return s & 255;
}

static const uint8_t *extent(const uint8_t *view, uint64_t base, size_t size, uint64_t gpa, size_t n)
{
    if (!view || gpa < base || gpa - base > size || n > size - (size_t)(gpa-base)) return NULL;
    return view + (size_t)(gpa-base);
}

/* Only immutable SDTs belong in the first page. Exactly one XSDT entry. */
static const uint8_t *table(const uint8_t *page, uint64_t gpa, const char *kind, unsigned length, unsigned rev)
{
    const uint8_t *p = extent(page, 0x90000, 4096, gpa, length);
    if (!p || memcmp(p, kind, 4) || le(p+4,4) != length || p[8] != rev || sum(p,length)) return NULL;
    return p;
}

static int gas(const uint8_t *p, unsigned bits, unsigned port)
{
    return p[0] == 1 && p[1] == bits && p[2] == 0 && p[3] == 0 && le(p+4,8) == port;
}

static int discover(const uint8_t *view, size_t size, uint64_t root)
{
    if (size < 8192 || root != 0x90000) return 0;
    const uint8_t *r = extent(view, 0x90000, 4096, root, 36);
    if (!r || memcmp(r,"RSD PTR ",8) || r[15] != 2 || le(r+16,4) || le(r+20,4) != 36 ||
        sum(r,20) || sum(r,36)) return 0;
    const uint8_t *x = table(view,le(r+24,8),"XSDT",44,1);
    if (!x) return 0;
    const uint8_t *f = table(view,le(x+36,8),"FACP",276,6);
    if (!f || f[131] != 5 || le(f+46,2) != 9 || le(f+48,4) || le(f+56,4) != 0x600 ||
        le(f+60,4) || le(f+64,4) != 0x604 || le(f+68,4) || le(f+76,4) || f[91] ||
        f[88] != 4 || f[89] != 2 || (le(f+112,4) & (1u << 20)) ||
        !gas(f+148,32,0x600) || !gas(f+172,16,0x604)) return 0;
    if (le(f+40,4) != le(f+140,8) || le(f+36,4) != le(f+132,8)) return 0;
    const uint8_t *d = table(view,le(f+140,8),"DSDT",50,2);
    /* Decode the sole AML object: NameOp + NameSeg + PackageOp, checked single-
     * byte PkgLength, 4 elements, two ByteConst(5), two ZeroOps, exact end. */
    if (!d || d[36] != 8 || memcmp(d+37,"_S5_",4) || d[41] != 0x12 ||
        d[42] != 50-42 || d[43] != 4 || d[44] != 0x0a || d[45] != 5 ||
        d[46] != 0x0a || d[47] != 5 || d[48] != 0 || d[49] != 0) return 0;
    uint64_t facs = le(f+132,8);
    const uint8_t *s = extent(view+4096,0x91000,4096,facs,64);
    return s && !(facs & 63) && !memcmp(s,"FACS",4) && le(s+4,4) == 64 && s[32] == 2;
}

/* Repair only the table checksum, so a malformed pointer cannot fail merely
 * because the mutation incidentally spoiled byte integrity. */
static void reseal(uint8_t *p, size_t len, unsigned offset)
{
    p[offset] = 0;
    p[offset] = (uint8_t)(0u - sum(p,len));
}

int main(void)
{
    static uint8_t storage[8194];
    uint8_t *p = storage + 1;
    memset(storage,0xA5,sizeof(storage));
    CHECK(!power_build_tables(p,8191,0x90000) && !power_build_tables(p,8192,UINT64_MAX));
    for (size_t i = 0; i < sizeof(storage); ++i) CHECK(storage[i] == 0xA5);
    CHECK(power_build_tables(p,8192,0x90000) && storage[0] == 0xA5 && storage[8193] == 0xA5);
    CHECK(discover(p,8192,0x90000) && !discover(p,8191,0x90000) && !discover(p,8192,0));
    uint8_t *x = p + (size_t)(le(p+24,8)-0x90000);
    uint8_t *f = p + (size_t)(le(x+36,8)-0x90000);
    uint8_t *d = p + (size_t)(le(f+140,8)-0x90000);
    p[8] ^= 1; reseal(p,36,32); CHECK(!discover(p,8192,0x90000));
    p[8] ^= 1; reseal(p,36,32); CHECK(discover(p,8192,0x90000));
    p[32] ^= 1; CHECK(!discover(p,8192,0x90000)); p[32] ^= 1;
    d[9] ^= 1; CHECK(!discover(p,8192,0x90000)); d[9] ^= 1;
    d[45] = 4; reseal(d,50,9); CHECK(!discover(p,8192,0x90000));
    d[45] = 5; reseal(d,50,9); CHECK(discover(p,8192,0x90000));
    memset(f+36,0xff,4); memset(f+132,0,8); memset(f+132,0xff,4);
    reseal(f,276,9); CHECK(!discover(p,8192,0x90000)); /* dangling FACS, valid checksum */
    CHECK(power_build_tables(p,8192,0x90000));
    linux_put32(f+40,0x90fff); linux_put64(f+140,0x90fff); reseal(f,276,9);
    CHECK(!discover(p,8192,0x90000)); /* first byte owned, complete DSDT crosses page */
    CHECK(power_build_tables(p,8192,0x90000));
    memset(x+36,0xff,8); reseal(x,44,9); CHECK(!discover(p,8192,0x90000));
    CHECK(power_build_tables(p,8192,0x90000));
    linux_put32(f+4,277); reseal(f,277,9); CHECK(!discover(p,8192,0x90000));
    puts("power tables: guest root/SDTs/FACS, S5 AML and GAS; corrupt and dangling controls refused");
    return 0;
}
