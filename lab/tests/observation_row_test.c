/* Run using a host C11 compiler; does not allocate a framebuffer or enter a VM. */
#include <stdio.h>
#include <string.h>
#include "../../firmware/observation_row.h"
int main(void)
{
    char row[ObservationColumns+3];
    memset(row,'!',sizeof row);
    if (!observation_row(row+1,"ROOT ENTRIES ",0) || strcmp(row+1,"ROOT ENTRIES 0000000000000000")) return 1;
    if (!observation_row(row+1,"0123456789abcdef",UINT64_C(0x0123456789abcdef)) ||
        strcmp(row+1,"0123456789abcdef0123456789ABCDEF") || row[0]!='!' || row[sizeof row-1]!='!') return 2;
    char prior[sizeof row]; memcpy(prior,row,sizeof row);
    if (observation_row(row+1,"0123456789abcdef!",1) || memcmp(row,prior,sizeof row)) return 3;
    puts("PASS: 057 diagnostic, full 32-column row, longer label refused without mutation");
    return 0;
}
