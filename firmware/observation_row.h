/** @file observation_row.h Bounded framebuffer evidence rows, independent of GOP. */
#ifndef HV_OBSERVATION_ROW_H
#define HV_OBSERVATION_ROW_H
#include <stdint.h>
enum { ObservationColumns=32, ObservationDigits=16 };

/** Format a trusted NUL-terminated label and a 64-bit hexadecimal value into
 * a caller-owned row. Leave the row untouched on refusal; never truncate a
 * label or a digit. The preflight viewport uses the same column count. */
static inline int observation_row(char out[ObservationColumns+1], const char *label, uint64_t value)
{
    static const char hex[]="0123456789ABCDEF";
    unsigned n=0;
    while (n<=ObservationColumns-ObservationDigits && label[n]) ++n;
    if (n>ObservationColumns-ObservationDigits) return 0;
    for (unsigned i=0;i<n;++i) out[i]=label[i];
    for (unsigned i=ObservationDigits;i;value>>=4) out[n+--i]=hex[value&15];
    out[n+ObservationDigits]='\0';
    return 1;
}
#endif
