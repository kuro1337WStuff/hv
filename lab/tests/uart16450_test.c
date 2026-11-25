/* Host-only 16450 existence probe, IRQ level, cause and drain controls. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t UINT8, BOOLEAN;
typedef uint16_t UINT16;
enum { FALSE=0, TRUE=1 };
#include "../../firmware/uart16450.h"
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"line %u: %s\n",(unsigned)__LINE__,#x); return 1;} } while (0)
static int put(Uart16450 *u, unsigned reg, UINT8 b) {return uart16450_access(u,(UINT16)(0x3f8+reg),FALSE,&b);}
static int got(Uart16450 *u, unsigned reg, UINT8 want) {UINT8 b=0xca; return uart16450_access(u,(UINT16)(0x3f8+reg),TRUE,&b) && b==want;}
int main(void)
{
    Uart16450 u={0};
    CHECK(put(&u,1,0) && got(&u,1,0) && put(&u,1,15) && got(&u,1,15));
    CHECK(!uart16450_irq4(&u) && got(&u,2,2) && got(&u,2,1));
    CHECK(put(&u,1,0) && put(&u,4,8) && put(&u,1,2) && uart16450_irq4(&u));
    CHECK(put(&u,4,0) && !uart16450_irq4(&u) && put(&u,4,8) && uart16450_irq4(&u));
    CHECK(got(&u,2,2) && !uart16450_irq4(&u) && put(&u,1,2) && got(&u,2,1));
    CHECK(put(&u,3,0x83) && put(&u,1,0) && put(&u,0,1) && got(&u,0,1));
    CHECK(put(&u,3,3) && got(&u,1,2) && u.base.count==0 && !uart16450_irq4(&u));
    CHECK(put(&u,0,'T') && !uart16450_irq4(&u) && !put(&u,0,'X'));
    CHECK(got(&u,5,0) && uart16450_irq4(&u) && got(&u,5,0x60) && uart16450_irq4(&u));
    CHECK(got(&u,2,2) && !uart16450_irq4(&u) && got(&u,2,1) && u.base.tx[0]=='T' && u.base.count==1);
    CHECK(put(&u,2,1) && got(&u,2,1) && put(&u,7,0xa5) && got(&u,7,0xa5) && put(&u,7,0x5a) && got(&u,7,0x5a));
    Uart16450 prior=u;
    CHECK(!put(&u,1,0x80) && !put(&u,4,0x10) && memcmp(&u,&prior,sizeof u)==0);
    puts("PASS: opt-in 16450 probe, DLAB alias, THRE clear/rearm, OUT2 gate, exact TX");
    return 0;
}
