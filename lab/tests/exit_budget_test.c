/* Host only. From an x64 VS prompt at the repository root:
 * cl /nologo /std:c11 /O2 /W4 /WX lab\tests\exit_budget_test.c /Fobuild\exit_budget_test.obj /Febuild\exit_budget_test.exe
 * build\exit_budget_test.exe
 */
#include <stdio.h>
#include "../../firmware/exit_budget.h"

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"line %d: %s\n",__LINE__,#expr); return 0; } } while (0)
#define CYCLE(rip,reason,disp,progress,expected) do { \
    CHECK(exit_budget_enter(&b,&now)); \
    CHECK(exit_budget_return(&b,rip,reason,disp,progress,&now)==expected); \
} while (0)

static int disposition(void)
{
    const ExitBudgetLimits limits={10,100,4,2};
    const ExitDisposition unknown[]={ExitDispositionUnknown, ExitDispositionUnobserved, (ExitDisposition)99};
    uint64_t now=0;
    ExitBudget b;
    for (size_t i=0;i<sizeof unknown/sizeof unknown[0];++i) {
        exit_budget_init(&b,limits,&now);
        CHECK(b.first_disposition==ExitDispositionUnobserved);
        CHECK(exit_budget_enter(&b,&now));
        CHECK(exit_budget_return(&b,0,0,unknown[i],1,NULL)==ExitBudgetUnknown);
        CHECK(b.returns==1 && b.first_disposition==ExitDispositionUnknown);
        CHECK(!exit_budget_enter(&b,&now));
        CHECK(exit_budget_return(&b,0,0,ExitDispositionComplete,1,&now)==ExitBudgetUnknown);
        CHECK(b.returns==1);
    }
    exit_budget_init(&b,limits,&now);
    CYCLE(1,2,ExitDispositionHandled,0,ExitBudgetReady);
    CYCLE(1,2,ExitDispositionUnknown,0,ExitBudgetUnknown);
    CHECK(!exit_budget_enter(&b,&now) && b.first_disposition==ExitDispositionHandled);
    exit_budget_init(&b,limits,&now);
    CYCLE(1,2,ExitDispositionComplete,0,ExitBudgetDone);
    CHECK(!exit_budget_enter(&b,&now) && b.first_disposition==ExitDispositionComplete);
    return 1;
}

static int bounds(void)
{
    ExitBudgetLimits limits={10,100,12,2};
    ExitBudget b;
    uint64_t now=0;
    exit_budget_init(&b,limits,&now);
    CYCLE(1,2,ExitDispositionHandled,0,ExitBudgetReady); /* establishes baseline */
    CYCLE(1,2,ExitDispositionHandled,0,ExitBudgetReady);
    CYCLE(2,2,ExitDispositionHandled,0,ExitBudgetReady); /* RIP changed */
    CYCLE(2,2,ExitDispositionHandled,0,ExitBudgetReady);
    CYCLE(2,3,ExitDispositionHandled,0,ExitBudgetReady); /* reason changed */
    CYCLE(2,3,ExitDispositionHandled,0,ExitBudgetReady);
    CYCLE(2,3,ExitDispositionHandled,1,ExitBudgetReady); /* verified progress */
    CYCLE(2,3,ExitDispositionHandled,0,ExitBudgetReady);
    CYCLE(2,3,ExitDispositionHandled,0,ExitBudgetNoProgress);
    CHECK(!exit_budget_enter(&b,&now) && b.stalled==2 && b.returns==9);

    limits.max_exits=2;
    exit_budget_init(&b,limits,&now);
    CYCLE(0,0,ExitDispositionHandled,1,ExitBudgetReady);
    CYCLE(1,1,ExitDispositionHandled,1,ExitBudgetExitLimit);
    CHECK(!exit_budget_enter(&b,&now) && b.returns==2);
    exit_budget_init(&b,limits,&now);
    CYCLE(0,0,ExitDispositionHandled,0,ExitBudgetReady);
    CYCLE(0,0,ExitDispositionComplete,0,ExitBudgetDone); /* exact last slot */
    return 1;
}

static int time_bounds(void)
{
    ExitBudgetLimits limits={5,9,10,2};
    ExitBudget b;
    uint64_t now=UINT64_MAX-2;
    exit_budget_init(&b,limits,&now);
    CHECK(exit_budget_enter(&b,&now));
    now=1; /* 4 ticks elapsed across rollover; first return in time */
    CHECK(exit_budget_return(&b,1,2,ExitDispositionHandled,0,&now)==ExitBudgetReady);
    now=3;
    CHECK(exit_budget_enter(&b,&now));
    now=6; /* 9 ticks total, including the 2 spent outside */
    CHECK(exit_budget_return(&b,2,2,ExitDispositionComplete,1,&now)==ExitBudgetPhaseDeadline);
    CHECK(!exit_budget_enter(&b,&now));

    now=UINT64_MAX-2;
    exit_budget_init(&b,limits,&now);
    ++now;
    CHECK(exit_budget_enter(&b,&now));
    now=2; /* exactly 5 ticks since start, just 4 since entry */
    CHECK(exit_budget_return(&b,1,2,ExitDispositionComplete,1,&now)==ExitBudgetFirstDeadline);
    CHECK(b.returns==1 && b.first_disposition==ExitDispositionComplete && !exit_budget_enter(&b,&now));

    limits.phase_ticks=3;
    exit_budget_init(&b,limits,&now);
    CHECK(exit_budget_enter(&b,&now));
    now+=6; /* both deadlines crossed; phase bound was earlier */
    CHECK(exit_budget_return(&b,1,2,ExitDispositionHandled,0,&now)==ExitBudgetPhaseDeadline);
    exit_budget_init(&b,limits,&now);
    CYCLE(1,2,ExitDispositionHandled,0,ExitBudgetReady);
    now+=3;
    CHECK(!exit_budget_enter(&b,&now) && b.state==ExitBudgetPhaseDeadline && b.returns==1);
    return 1;
}

static int unavailable(void)
{
    const ExitBudgetLimits limits={1,3,2,1};
    const ExitBudgetLimits zeros[]={{0,3,2,1},{1,0,2,1},{1,3,0,1},{1,3,2,0}};
    ExitBudget b={0};
    uint64_t now=0;
    CHECK(!exit_budget_enter(&b,&now) && b.state==ExitBudgetBadOrder);
    for (size_t i=0;i<sizeof zeros/sizeof zeros[0];++i) {
        exit_budget_init(&b,zeros[i],&now);
        CHECK(!exit_budget_enter(&b,&now) && b.state==ExitBudgetZero && b.returns==0);
    }
    exit_budget_init(&b,limits,NULL);
    CHECK(!exit_budget_enter(&b,&now) && b.state==ExitBudgetNoTime);
    exit_budget_init(&b,limits,&now);
    CHECK(!exit_budget_enter(&b,NULL) && b.state==ExitBudgetNoTime);
    exit_budget_init(&b,limits,&now);
    CHECK(exit_budget_enter(&b,&now));
    now+=10; /* external clock observation alone: no actual return */
    CHECK(b.state==ExitBudgetPending && b.returns==0 && b.first_disposition==ExitDispositionUnobserved);
    CHECK(!exit_budget_enter(&b,&now) && b.state==ExitBudgetBadOrder); /* no second grant */
    exit_budget_init(&b,limits,&now);
    CHECK(exit_budget_enter(&b,&now));
    CHECK(exit_budget_return(&b,1,2,ExitDispositionHandled,0,NULL)==ExitBudgetNoTime);
    CHECK(b.returns==1 && b.first_disposition==ExitDispositionHandled && !exit_budget_enter(&b,&now));
    exit_budget_init(&b,limits,&now);
    CHECK(exit_budget_return(&b,1,2,ExitDispositionHandled,0,&now)==ExitBudgetBadOrder);
    CHECK(!exit_budget_enter(&b,&now) && !b.returns);
    return 1;
}

int main(void)
{
    if (!disposition() || !bounds() || !time_bounds() || !unavailable()) return 1;
    puts("exit budget: PASS");
    return 0;
}
