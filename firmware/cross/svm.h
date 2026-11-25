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

#endif
