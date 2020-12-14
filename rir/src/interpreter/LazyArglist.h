#ifndef RIR_ARGS_LAZY_H
#define RIR_ARGS_LAZY_H

#include "runtime/ArglistOrder.h"
#include "runtime/RirRuntimeObject.h"

#include "interp_incl.h"

#include <cassert>
#include <cstdint>
#include <functional>

namespace rir {

#pragma pack(push)
#pragma pack(1)

static constexpr size_t LAZY_ARGS_MAGIC = 0x1a27a000;

/**
 * ArgsLazyCreation holds the information needed to recreate the
 * arguments list needed by gnu-r contexts whenever a function
 * is called, lazily. In RCNTXT the field that hold the list is
 * promargs.
 */

struct LazyArglist : public RirRuntimeObject<LazyArglist, LAZY_ARGS_MAGIC> {
  public:
    LazyArglist() = delete;
    LazyArglist(const LazyArglist&) = delete;
    LazyArglist& operator=(const LazyArglist&) = delete;

    static size_t size(size_t length) {
        return sizeof(LazyArglist) + sizeof(SEXP) * length;
    }

    bool isReordered() const {
        return callId != ArglistOrder::ORIGINAL_ARGLIST_ORDER;
    }

    ArglistOrder* arglistOrder() {
        assert(reordering == getEntry(0));
        assert(reordering);
        return ArglistOrder::unpack(reordering);
    }

    SEXP getArg(size_t i) {
        assert(data[i] == getEntry(i + 1));
        assert(!stackArgs);
        return getEntry(i + 1);
    }

    size_t nargs() {
        if (length == 0)
            return 0;

        // Cache
        if (actualNargs != 0)
            return actualNargs;

        for (size_t i = 0; i < length; ++i) {
            SEXP arg;
            if (stackArgs) {
                assert(stackArgs[i].tag == 0);
                arg = stackArgs[i].u.sxpval;
            } else {
                arg = getEntry(i);
            }
            if (TYPEOF(arg) == DOTSXP) {
                while (arg != R_NilValue) {
                    actualNargs++;
                    arg = CDR(arg);
                }
                continue;
            }
            actualNargs++;
        }
        return actualNargs;
    }

    SEXP createArglist(InterpreterInstance* ctx) {
        SLOWASSERT(!wrong);
        return createLegacyArglistFromStackValues(
            length, stackArgs, this, nullptr, ast, false, true, ctx);
    }

  private:
    LazyArglist(ArglistOrder::CallId id, SEXP arglistOrder, size_t length,
                const R_bcstack_t* args, SEXP ast, bool onStack)
        // GC tracked pointers are the reordering and length args
        : RirRuntimeObject((intptr_t)&reordering - (intptr_t)this,
                           1 + (onStack ? 0 : length)),
          callId(id), length(length), ast(ast), reordering(arglistOrder) {
#ifdef ENABLE_SLOWASSERT
        for (size_t i = 0; i < length; ++i) {
            assert(args[i].tag == 0);
            assert(args[i].u.sxpval);
        }
#endif
        if (onStack) {
            stackArgs = args;
        } else {
            for (size_t i = 0; i < length; ++i) {
                assert(args[i].tag == 0);
                setEntry(i + 1, args[i].u.sxpval);
            }
            stackArgs = nullptr;
        }
#ifdef ENABLE_SLOWASSERT
        assert(getEntry(0) == reordering);
        for (size_t i = 0; i < length; ++i) {
            assert(getEntry(i + 1) == data[i]);
        }
#endif
    }

    friend struct LazyArglistOnHeap;
    friend struct LazyArglistOnStack;

    const ArglistOrder::CallId callId;
    const uint32_t length;
    uint32_t actualNargs = 0;
    const R_bcstack_t* stackArgs;
    // Needed to recover the names
    SEXP ast;
    // Needed to reorder arguments. Has to be the last field to be seen by the
    // GC!
    SEXP reordering;
    SEXP data[];
};

#pragma pack(pop)

struct LazyArglistOnStack {
  public:
    // This needs to come first and provides a SEXPREC header to not confuse
    // the R garbage collector.
    LazyArglistOnStack() = delete;
    LazyArglistOnStack(const LazyArglistOnStack&) = delete;
    LazyArglistOnStack& operator=(const LazyArglistOnStack&) = delete;

    LazyArglistOnStack(ArglistOrder::CallId id, SEXP arglistOrder,
                       size_t length, const R_bcstack_t* args, SEXP ast)
        : content(id, arglistOrder, length, args, ast, true) {
        fakeSEXP.attrib = R_NilValue;
        fakeSEXP.gengc_next_node = R_NilValue;
        fakeSEXP.gengc_prev_node = R_NilValue;
        fakeSEXP.sxpinfo.gcgen = 1;
        fakeSEXP.sxpinfo.mark = 1;
        fakeSEXP.sxpinfo.named = 2;
        fakeSEXP.sxpinfo.type = EXTERNALSXP;
    }

    // LazyArglistOnStack(size_t length, const R_bcstack_t* args, SEXP ast)
    //     : LazyArglistOnStack(ArglistOrder::ORIGINAL_ARGLIST_ORDER, nullptr,
    //                          length, args, ast) {}

    SEXP asSexp() { return (SEXP)this; }

  private:
    VECTOR_SEXPREC fakeSEXP;

  public:
    LazyArglist content;
};

struct LazyArglistOnHeap {
  public:
    static SEXP New(ArglistOrder::CallId id, SEXP arglistOrder, size_t length,
                    const R_bcstack_t* args, SEXP ast) {
        SEXP wrapper = Rf_allocVector(EXTERNALSXP, LazyArglist::size(length));
        auto la = new (DATAPTR(wrapper))
            LazyArglist(id, arglistOrder, length, args, ast, false);
        return la->container();
    }

    // static SEXP New(size_t length, const R_bcstack_t* args, SEXP ast) {
    //     return New(ArglistOrder::ORIGINAL_ARGLIST_ORDER, nullptr, length,
    //     args,
    //                ast);
    // }
};

} // namespace rir

#endif
