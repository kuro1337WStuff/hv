#ifndef HV_EXIT_BUDGET_H
#define HV_EXIT_BUDGET_H

#include <stdint.h>

/* Host policy only: no clock reader, guest entry, exit decoder or watchdog.
 * One trusted caller owns a phase. Only a NEW transition from Ready to Pending
 * grants one entry. Pending is not a prediction that the guest will return.
 * Never reset the budget to resume a stopped phase. */
typedef enum {
    ExitBudgetNotStarted, ExitBudgetReady, ExitBudgetPending, ExitBudgetDone,
    ExitBudgetZero, ExitBudgetNoTime, ExitBudgetFirstDeadline,
    ExitBudgetPhaseDeadline, ExitBudgetExitLimit, ExitBudgetNoProgress,
    ExitBudgetUnknown, ExitBudgetBadOrder
} ExitBudgetState;

typedef enum {
    ExitDispositionUnobserved, ExitDispositionUnknown,
    ExitDispositionHandled, ExitDispositionComplete
} ExitDisposition;

typedef struct {
    uint64_t first_ticks; /* phase start through first observed return */
    uint64_t phase_ticks; /* includes host time between entries */
    uint32_t max_exits;   /* last permitted return may complete, not resume */
    uint32_t stall_limit; /* stop on this many consecutive unchanged pairs */
} ExitBudgetLimits;

typedef struct {
    ExitBudgetState state;
    ExitDisposition first_disposition;
    uint64_t first_left, phase_left;
    uint32_t max_exits, stall_limit;
    uint64_t last_tick, last_rip, last_reason;
    uint32_t returns, stalled;
} ExitBudget;

/* All limits mandatory, no sentinel for unlimited. A null sample is not zero
 * elapsed time. Ticks are from one monotonic modulo-2^64 source; adjacent
 * samples must be less than one full wrap apart for elapsed time to be known. */
static inline void exit_budget_init(ExitBudget *b, ExitBudgetLimits limits, const uint64_t *start)
{
    b->first_left=limits.first_ticks; b->phase_left=limits.phase_ticks;
    b->max_exits=limits.max_exits; b->stall_limit=limits.stall_limit;
    b->first_disposition=ExitDispositionUnobserved;
    b->returns=b->stalled=0;
    b->last_rip=b->last_reason=0;
    b->last_tick=start ? *start : 0;
    if (!limits.first_ticks || !limits.phase_ticks || !limits.max_exits || !limits.stall_limit)
        b->state=ExitBudgetZero;
    else b->state=start ? ExitBudgetReady : ExitBudgetNoTime;
}

/* Charge elapsed ticks before each entry and at each known return. Debit
 * remaining time, rather than comparing wrapping absolute deadlines. If both
 * deadlines expired, report the earlier one (first-return wins ties). */
static inline ExitBudgetState exit_budget_charge(ExitBudget *b, const uint64_t *now)
{
    if (!now) return ExitBudgetNoTime;
    uint64_t delta=*now-b->last_tick;
    b->last_tick=*now;
    if (!b->returns && b->first_left<=b->phase_left && delta>=b->first_left)
        return ExitBudgetFirstDeadline;
    if (delta>=b->phase_left) return ExitBudgetPhaseDeadline;
    b->phase_left-=delta;
    if (!b->returns) b->first_left-=delta;
    return ExitBudgetReady;
}

/* Boolean one-shot permission, NOT a poll for an existing grant. */
static inline int exit_budget_enter(ExitBudget *b, const uint64_t *now)
{
    if (b->state!=ExitBudgetReady) {
        if (b->state==ExitBudgetPending || b->state==ExitBudgetNotStarted) b->state=ExitBudgetBadOrder;
        return 0;
    }
    b->state=exit_budget_charge(b,now);
    if (b->state!=ExitBudgetReady) return 0;
    b->state=ExitBudgetPending;
    return 1;
}

/* Only call after an actual return. The trusted handler supplies an explicit
 * disposition: unrecognized/unspecified values always stop, regardless of RIP,
 * claimed progress or clock. progress means verified semantic state advancement
 * since the preceding return, not merely another entry, exit or clock sample.
 * A success terminal exit need not demonstrate further progress. */
static inline ExitBudgetState exit_budget_return(ExitBudget *b, uint64_t rip, uint64_t reason,
    ExitDisposition disposition, int progress, const uint64_t *now)
{
    if (b->state!=ExitBudgetPending) {
        if (b->state==ExitBudgetReady || b->state==ExitBudgetNotStarted) b->state=ExitBudgetBadOrder;
        return b->state;
    }
    if (disposition!=ExitDispositionHandled && disposition!=ExitDispositionComplete)
        disposition=ExitDispositionUnknown;
    if (!b->returns) b->first_disposition=disposition;
    if (disposition==ExitDispositionUnknown) b->state=ExitBudgetUnknown;
    else {
        b->state=exit_budget_charge(b,now);
        if (b->state==ExitBudgetReady) {
            if (disposition==ExitDispositionComplete) b->state=ExitBudgetDone;
            else if (b->returns && rip==b->last_rip && reason==b->last_reason && !progress) {
                if (++b->stalled==b->stall_limit) b->state=ExitBudgetNoProgress;
            } else b->stalled=0;
        }
    }
    b->last_rip=rip; b->last_reason=reason;
    /* Never enter with returns == max_exits. Neither uint32_t counter wraps. */
    if (++b->returns==b->max_exits && b->state==ExitBudgetReady) b->state=ExitBudgetExitLimit;
    return b->state;
}
#endif
