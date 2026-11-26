#ifndef HV_W02_H
#define HV_W02_H
#ifndef HV_W02
#error W02 requires the isolated W02 build profile
#endif
/* The cold reset trace, media admission and SVM teardown are shared. W02
 * continues only from W01's independently verified, denied SEC fetch. */
#include "w01.h"
#endif
