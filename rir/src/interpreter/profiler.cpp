#include "api.h"

#include "interp.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <iomanip>
#include <unordered_map>

#include "compiler/pir/type.h"
#include "profiler.h"

#include <time.h>

namespace rir {

static RuntimeProfiler instance;

RuntimeProfiler::RuntimeProfiler() {}

RuntimeProfiler::~RuntimeProfiler() {}

void RuntimeProfiler::sample(int signal) {
    auto ctx = (RCNTXT*)R_GlobalContext;
    auto& stack = ctx->nodestack;
    if (R_BCNodeStackTop == R_BCNodeStackBase)
        return;
    if ((stack)->tag != 0)
        return;
    auto tos = (stack)->u.sxpval;
    if (!tos)
        return;
    auto code = Code::check(tos);
    if (!code)
        return;
    auto md = code->pirTypeFeedback();
    if (!md)
        return;

    bool needReopt = false;
    size_t goodValues = 0;
    size_t slotCount = 0;

    md->forEachSlot([&](size_t i, PirTypeFeedback::MDEntry& mdEntry, Opcode* opcode) {
        auto slot = *(stack + i);
        assert(slot.tag == 0);
        if (auto sxpval = slot.u.sxpval) {
            mdEntry.feedback.record(sxpval);
            auto samples = ++(mdEntry.sampleCount);
            slotCount++;
            if (samples == 10) {
                mdEntry.readyForReopt = true;
                // check if this feedback justifies a reopt
                auto bc = BC::decodeShallow(opcode);
                auto opcode = bc.bc;
                assert(opcode == Opcode::record_type_);
                auto oldFeedback = bc.immediate.typeFeedback;
                pir::PirType before;
                pir::PirType after;
                before.merge(oldFeedback);
                after.merge(mdEntry.feedback);
                after.isA(before);
                if (!before.isA(after)) {
                    // mark slot as good for reopt
                    mdEntry.needReopt = true;
                }
            }
            if (samples >= 10) {
                goodValues++;
                if (mdEntry.needReopt) {
                    needReopt = true;
                }
            }
            if (samples > 100) {
                mdEntry.readyForReopt = false;
                mdEntry.sampleCount = 0;
                mdEntry.feedback.reset();
            }
        }
    });

    // only trigger reopt if at least 50% of all slots have enough samples and at least one slot justifies re-opt.
    if (goodValues >= (slotCount / 2) && needReopt) {
        // set global re-opt flag
        code->flags.set(Code::Reoptimise);
    }
}

#ifndef __APPLE__
static void handler(int signal) { instance.sample(signal); }

void RuntimeProfiler::initProfiler() {
    bool ENABLE_PROFILER = getenv("PIR_ENABLE_PROFILER") ? true : false;
    if (!ENABLE_PROFILER) {
        return;
    }
    struct sigaction recordAction;
    struct sigevent sev;
    struct itimerspec itime;
    timer_t timer_id;

    recordAction.sa_handler = handler;
    sigfillset(&recordAction.sa_mask);
    recordAction.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &recordAction, NULL);

    // create timer

    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    sev.sigev_value.sival_ptr = &timer_id;

    timer_create(CLOCK_MONOTONIC, &sev, &timer_id);

    itime.it_value.tv_sec = 0;
    /* 500 million nsecs = .5 secs */
    itime.it_value.tv_nsec = 1000000;
    itime.it_interval.tv_sec = 0;
    /* 500 million nsecs = .5 secs */
    itime.it_interval.tv_nsec = 1000000;
    timer_settime(timer_id, 0, &itime, NULL);
}
#else
void RuntimeProfiler::initProfiler() {}
#endif

} // namespace rir
