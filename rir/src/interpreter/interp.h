#ifndef RIR_INTERPRETER_C_H
#define RIR_INTERPRETER_C_H

#include "builtins.h"
#include "call_context.h"
#include "instance.h"

#include "interp_incl.h"
#include "ir/Deoptimization.h"

#include <R/r.h>

#undef length

#if defined(__GNUC__) && (!defined(NO_THREADED_CODE))
#define THREADED_CODE
#endif

namespace rir {
extern DeoptReason::Reason lastDeoptReason;
extern unsigned lastRelativePc;

SEXP dispatchApply(SEXP ast, SEXP obj, SEXP actuals, SEXP selector,
                   SEXP callerEnv, InterpreterInstance* ctx);
bool isMissing(SEXP symbol, SEXP environment, Code* code, Opcode* op);

inline RCNTXT* getFunctionContext(size_t pos = 0,
                                  RCNTXT* cptr = (RCNTXT*)R_GlobalContext) {
    while (cptr->nextcontext != NULL) {
        if (cptr->callflag & CTXT_FUNCTION) {
            if (pos == 0)
                return cptr;
            pos--;
        }
        cptr = cptr->nextcontext;
    }
    assert(false);
    return nullptr;
}

inline RCNTXT* findFunctionContextFor(SEXP e) {
    auto cptr = (RCNTXT*)R_GlobalContext;
    while (cptr->nextcontext != NULL) {
        if (cptr->callflag & CTXT_FUNCTION) {
            if (cptr->cloenv == e)
                return cptr;
        }
        cptr = cptr->nextcontext;
    }
    return nullptr;
}

SEXP builtinCall(CallContext& call, InterpreterInstance* ctx);
SEXP doCall(CallContext& call, InterpreterInstance* ctx);
size_t expandDotDotDotCallArgs(InterpreterInstance* ctx, size_t n,
                               Immediate* names_, SEXP env, bool explicitDots);
void deoptFramesWithContext(InterpreterInstance* ctx,
                            const CallContext* callCtxt,
                            DeoptMetadata* deoptData, SEXP sysparent,
                            size_t pos, size_t stackHeight);
void recordDeoptReason(SEXP val, const DeoptReason& reason,
                       unsigned relativePc = (unsigned)-1);
void jit(SEXP cls, SEXP name, InterpreterInstance* ctx);

SEXP seq_int(int n1, int n2);
bool doubleCanBeCastedToInteger(double n);
int colonInputEffects(SEXP lhs, SEXP rhs, unsigned srcIdx);
bool isColonFastcase(SEXP, SEXP);
SEXP colonCastLhs(SEXP lhs);
SEXP colonCastRhs(SEXP newLhs, SEXP rhs);

} // namespace rir
#endif // RIR_INTERPRETER_C_H
