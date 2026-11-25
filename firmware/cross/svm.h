#ifndef HV_CROSS_SVM_H
#define HV_CROSS_SVM_H

#include <stdint.h>

struct cross_sample { uint64_t exit, rip, ax; };

/* Check on each pinned CPU before using SVM on either. */
int cross_supported(void);
void cross_init(uint64_t kernel_va, uint64_t kernel_pa);
/* Epoch 0 selects A, epoch 1 selects B; both guests must be stopped. */
void cross_map(unsigned epoch);
/* Slot 0 is used exclusively on the BSP, slot 1 exclusively on the chosen AP. */
struct cross_sample cross_read(unsigned slot);

#ifdef HV_NPT_BROADCAST
/* Raw Hyper-V nested feature EAX, or zero if the Hv#1 leaf is unavailable. */
uint32_t cross_broadcast_caps(void);
/* Probe each pinned CPU before cross_init; zero means SKIP before guest entry. */
int cross_broadcast_available(void);
/* BSP only, after cross_init and before the first guest entry; zero means SKIP. */
int cross_broadcast_setup(void);
/* Both guests must be stopped. Returns the synchronous hypercall's HV_STATUS. */
uint16_t cross_broadcast_flush(void);
#endif

#endif
