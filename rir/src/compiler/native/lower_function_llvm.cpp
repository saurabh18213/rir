#include "lower_function_llvm.h"
#include "compiler/native/types_llvm.h"
#include "representation_llvm.h"

#include "llvm/IR/Intrinsics.h"

#include "R/BuiltinIds.h"
#include "R/Funtab.h"
#include "R/Symbols.h"
#include "R/r.h"

#include "builtins.h"
#include "interpreter/LazyArglist.h"
#include "interpreter/LazyEnvironment.h"
#include "interpreter/builtins.h"
#include "interpreter/instance.h"

#include "runtime/DispatchTable.h"
#include "utils/Pool.h"

#include "compiler/parameter.h"
#include "compiler/pir/pir_impl.h"
#include "compiler/util/lowering/allocators.h"
#include "compiler/util/visitor.h"

#include "compiler/analysis/reference_count.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rir {
namespace pir {

using namespace llvm;

LLVMContext& C = rir::pir::JitLLVM::C;

extern "C" size_t R_NSize;
extern "C" size_t R_NodesInUse;

void LowerFunctionLLVM::PhiBuilder::addInput(llvm::Value* v) {
    addInput(v, builder.GetInsertBlock());
}
llvm::Value* LowerFunctionLLVM::PhiBuilder::operator()() {
    assert(!created);
    created = true;
    assert(inputs.size() > 0);
    if (inputs.size() == 1)
        return inputs[0].first;
    assert(builder.GetInsertBlock()->hasNPredecessors(inputs.size()));
    auto phi = builder.CreatePHI(type, inputs.size());
    for (auto& in : inputs)
        phi->addIncoming(in.first, in.second);
    return phi;
}

class NativeAllocator : public SSAAllocator {
  public:
    NativeAllocator(Code* code, ClosureVersion* cls,
                    const LivenessIntervals& livenessIntervals, LogStream& log)
        : SSAAllocator(code, cls, livenessIntervals, log) {}

    bool needsAVariable(Value* v) const {
        return v->producesRirResult() && !LdConst::Cast(v) &&
               !(CastType::Cast(v) &&
                 LdConst::Cast(CastType::Cast(v)->arg(0).val()));
    }
    bool needsASlot(Value* v) const override final {
        return needsAVariable(v) && Representation::Of(v) == t::SEXP;
    }
    bool interfere(Instruction* a, Instruction* b) const override final {
        // Ensure we preserve slots for variables with typefeedback to make them
        // accessible to the runtime profiler.
        // TODO: this needs to be replaced by proper mapping of slots.
        if (a != b && (a->typeFeedback.origin || b->typeFeedback.origin))
            return true;
        return SSAAllocator::interfere(a, b);
    }
};

llvm::Value* LowerFunctionLLVM::globalConst(llvm::Constant* init,
                                            llvm::Type* ty) {
    if (!ty)
        ty = init->getType();
    return new llvm::GlobalVariable(JitLLVM::module(), ty, true,
                                    llvm::GlobalValue::PrivateLinkage, init);
}

void LowerFunctionLLVM::setVisible(int i) {
    builder.CreateStore(c(i), convertToPointer(&R_Visible, t::IntPtr));
}

llvm::Value* LowerFunctionLLVM::force(Instruction* i, llvm::Value* arg) {

    auto isProm = BasicBlock::Create(C, "", fun);
    auto needsEval = BasicBlock::Create(C, "", fun);
    auto isVal = BasicBlock::Create(C, "", fun);
    auto isPromVal = BasicBlock::Create(C, "", fun);
    auto done = BasicBlock::Create(C, "", fun);

    auto res = phiBuilder(t::SEXP);

    checkIsSexp(arg, "force argument");

    auto type = sexptype(arg);
    auto tt = builder.CreateICmpEQ(type, c(PROMSXP));

    builder.CreateCondBr(tt, isProm, isVal);

    builder.SetInsertPoint(isProm);
    auto val = car(arg);
    checkIsSexp(arg, "prval");
    auto tv = builder.CreateICmpEQ(val, constant(R_UnboundValue, t::SEXP));
    builder.CreateCondBr(tv, needsEval, isPromVal, branchMostlyFalse);

    builder.SetInsertPoint(needsEval);
    auto evaled = call(NativeBuiltins::forcePromise, {arg});
    checkIsSexp(evaled, "force result");
    res.addInput(evaled);
    builder.CreateBr(done);

    builder.SetInsertPoint(isVal);
    res.addInput(arg);
    builder.CreateBr(done);

    builder.SetInsertPoint(isPromVal);
    res.addInput(val);
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
    auto result = res();
#ifdef ENABLE_SLOWASSERT
    insn_assert(builder.CreateICmpNE(sexptype(result), c(PROMSXP)),
                "Force returned promise");
#endif
    return result;
}

void LowerFunctionLLVM::insn_assert(llvm::Value* v, const char* msg,
                                    llvm::Value* p) {
    auto nok = BasicBlock::Create(C, "assertFail", fun);
    auto ok = BasicBlock::Create(C, "assertOk", fun);

    builder.CreateCondBr(v, ok, nok, branchAlwaysTrue);

    builder.SetInsertPoint(nok);
    if (p)
        call(NativeBuiltins::printValue, {p});
    call(NativeBuiltins::assertFail, {convertToPointer((void*)msg)});
    builder.CreateRet(builder.CreateIntToPtr(c(nullptr), t::SEXP));

    builder.SetInsertPoint(ok);
}

llvm::Value* LowerFunctionLLVM::constant(SEXP co, llvm::Type* needed) {
    static std::unordered_set<SEXP> eternal = {
        R_TrueValue,  R_NilValue,  R_FalseValue,     R_UnboundValue,
        R_MissingArg, R_GlobalEnv, R_LogicalNAValue, R_EmptyEnv};
    if (needed == t::Int) {
        assert(Rf_length(co) == 1);
        if (TYPEOF(co) == INTSXP)
            return llvm::ConstantInt::get(C, llvm::APInt(32, INTEGER(co)[0]));
        if (TYPEOF(co) == REALSXP) {
            if (std::isnan(REAL(co)[0]))
                return llvm::ConstantInt::get(C, llvm::APInt(32, NA_INTEGER));
            return llvm::ConstantInt::get(C, llvm::APInt(32, (int)REAL(co)[0]));
        }
        if (TYPEOF(co) == LGLSXP)
            return llvm::ConstantInt::get(C, llvm::APInt(32, LOGICAL(co)[0]));
    }

    if (needed == t::Double) {
        assert(Rf_length(co) == 1);
        if (TYPEOF(co) == INTSXP) {
            if (INTEGER(co)[0] == NA_INTEGER)
                return llvm::ConstantFP::get(C, llvm::APFloat(R_NaN));
            return llvm::ConstantFP::get(C,
                                         llvm::APFloat((double)INTEGER(co)[0]));
        }
        if (TYPEOF(co) == REALSXP)
            return llvm::ConstantFP::get(C, llvm::APFloat(REAL(co)[0]));
        if (TYPEOF(co) == LGLSXP) {
            if (LOGICAL(co)[0] == NA_LOGICAL)
                return llvm::ConstantFP::get(C, llvm::APFloat(R_NaN));
            return llvm::ConstantInt::get(C, llvm::APInt(32, LOGICAL(co)[0]));
        }
    }

    assert(needed == t::SEXP);
    // Normalize scalar logicals
    if (IS_SIMPLE_SCALAR(co, LGLSXP)) {
        auto t = LOGICAL(co)[0];
        if (t == 0) {
            co = R_FalseValue;
        } else if (t == NA_LOGICAL) {
            co = R_LogicalNAValue;
        } else {
            co = R_TrueValue;
        }
    }
    if (TYPEOF(co) == SYMSXP || eternal.count(co))
        return convertToPointer(co);

    auto i = Pool::insert(co);
    llvm::Value* pos = builder.CreateLoad(constantpool);
    pos = builder.CreateBitCast(dataPtr(pos, false),
                                PointerType::get(t::SEXP, 0));
    pos = builder.CreateGEP(pos, c(i));
    return builder.CreateLoad(pos);
}

llvm::Value* LowerFunctionLLVM::nodestackPtr() {
    return builder.CreateLoad(nodestackPtrAddr);
}

llvm::Value* LowerFunctionLLVM::stack(int i) {
    auto offset = -(i + 1);
    auto pos = builder.CreateGEP(nodestackPtr(), {c(offset), c(1)});
    return builder.CreateLoad(t::SEXP, pos);
}

void LowerFunctionLLVM::stack(const std::vector<llvm::Value*>& args) {
    auto stackptr = nodestackPtr();
    // set type tag to 0
    builder.CreateMemSet(builder.CreateGEP(stackptr, c(-args.size())), c(0, 8),
                         args.size() * sizeof(R_bcstack_t), 1);
    auto pos = -args.size();
    for (auto arg = args.begin(); arg != args.end(); arg++) {
        // store the value
        auto valS = builder.CreateGEP(stackptr, {c(pos), c(1)});
        builder.CreateStore(*arg, valS);
        pos++;
    }
    assert(pos == 0);
}

void LowerFunctionLLVM::setLocal(size_t i, llvm::Value* v) {
    assert(i < numLocals);
    assert(v->getType() == t::SEXP);
    auto pos = builder.CreateGEP(basepointer, {c(i), c(1)});
    builder.CreateStore(v, pos, true);
}

void LowerFunctionLLVM::incStack(int i, bool zero) {
    if (i == 0)
        return;
    auto cur = nodestackPtr();
    auto offset = sizeof(R_bcstack_t) * i;
    if (zero)
        builder.CreateMemSet(cur, c(0, 8), offset, 1);
    auto up = builder.CreateGEP(cur, c(i));
    builder.CreateStore(up, nodestackPtrAddr);
}

void LowerFunctionLLVM::decStack(int i) {
    if (i == 0)
        return;
    auto cur = nodestackPtr();
    auto up = builder.CreateGEP(cur, c(-i));
    builder.CreateStore(up, nodestackPtrAddr);
}

llvm::Value* LowerFunctionLLVM::callRBuiltin(SEXP builtin,
                                             const std::vector<Value*>& args,
                                             int srcIdx, CCODE builtinFun,
                                             llvm::Value* env) {
    if (supportsFastBuiltinCall(builtin)) {
        return withCallFrame(args, [&]() -> llvm::Value* {
            return call(NativeBuiltins::callBuiltin,
                        {
                            paramCode(),
                            c(srcIdx),
                            constant(builtin, t::SEXP),
                            env,
                            c(args.size()),
                        });
        });
    }

    auto f = convertToPointer((void*)builtinFun, t::builtinFunctionPtr);

    auto arglist = constant(R_NilValue, t::SEXP);
    for (auto v = args.rbegin(); v != args.rend(); v++) {
        auto a = loadSxp(*v);
#ifdef ENABLE_SLOWASSERT
        insn_assert(builder.CreateICmpNE(sexptype(a), c(PROMSXP)),
                    "passing promise to builtin");
#endif
        arglist = call(NativeBuiltins::consNr, {a, arglist});
    }
    if (args.size() > 0)
        protectTemp(arglist);

    auto ast = constant(cp_pool_at(globalContext(), srcIdx), t::SEXP);
    // TODO: ensure that we cover all the fast builtin cases
    int flag = getFlag(builtin);
    if (flag < 2)
        setVisible(flag != 1);
    auto res = builder.CreateCall(f, {
                                         ast,
                                         constant(builtin, t::SEXP),
                                         arglist,
                                         env,
                                     });
    if (flag < 2)
        setVisible(flag != 1);
    return res;
}

llvm::Value*
LowerFunctionLLVM::withCallFrame(const std::vector<Value*>& args,
                                 const std::function<llvm::Value*()>& theCall,
                                 bool pop) {
    auto nargs = args.size();
    incStack(nargs, false);
    std::vector<llvm::Value*> jitArgs;
    for (auto& arg : args)
        jitArgs.push_back(load(arg, Representation::Sexp));
    stack(jitArgs);
    auto res = theCall();
    if (pop)
        decStack(nargs);
    return res;
}

llvm::Value* LowerFunctionLLVM::load(Value* v, Representation r) {
    return load(v, v->type, r);
}

llvm::Value* LowerFunctionLLVM::load(Value* v) {
    return load(v, v->type, Representation::Of(v));
}
llvm::Value* LowerFunctionLLVM::loadSxp(Value* v) {
    return load(v, Representation::Sexp);
}

llvm::Value* LowerFunctionLLVM::load(Value* val, PirType type,
                                     Representation needed) {
    llvm::Value* res;
    auto vali = Instruction::Cast(val);

    if (auto ct = CastType::Cast(val)) {
        if (LdConst::Cast(ct->arg(0).val())) {
            return load(ct->arg(0).val(), type, needed);
        }
    }

    if (vali && variables_.count(vali))
        res = getVariable(vali);
    else if (val == Env::elided())
        res = constant(R_NilValue, needed);
    else if (auto e = Env::Cast(val)) {
        if (e == Env::notClosed()) {
            res = tag(paramClosure());
        } else if (e == Env::nil()) {
            res = constant(R_NilValue, needed);
        } else if (Env::isStaticEnv(e)) {
            res = constant(e->rho, t::SEXP);
        } else {
            assert(false);
        }
    } else if (val == True::instance())
        res = constant(R_TrueValue, needed);
    else if (val == False::instance())
        res = constant(R_FalseValue, needed);
    else if (val == MissingArg::instance())
        res = constant(R_MissingArg, t::SEXP);
    else if (val == UnboundValue::instance())
        res = constant(R_UnboundValue, t::SEXP);
    else if (auto ld = LdConst::Cast(val))
        res = constant(ld->c(), needed);
    else if (val == NaLogical::instance())
        res = constant(R_LogicalNAValue, needed);
    else if (val == Nil::instance())
        res = constant(R_NilValue, needed);
    else {
        val->printRef(std::cerr);
        assert(false);
    }

    if (res->getType() == t::SEXP && needed != t::SEXP) {
        if (type.isA(PirType(RType::integer).scalar().notObject())) {
            res = unboxInt(res);
            assert(res->getType() == t::Int);
        } else if (type.isA(PirType(RType::logical).scalar().notObject())) {
            res = unboxLgl(res);
            assert(res->getType() == t::Int);
        } else if (type.isA((PirType() | RType::integer | RType::logical)
                                .scalar()
                                .notObject())) {
            res = unboxIntLgl(res);
            assert(res->getType() == t::Int);
        } else if (type.isA(PirType(RType::real).scalar().notObject())) {
            res = unboxReal(res);
            assert(res->getType() == t::Double);
        } else if (type.isA(
                       (PirType(RType::real) | RType::integer | RType::logical)
                           .scalar()
                           .notObject())) {
            res = unboxRealIntLgl(res, type);
            assert(res->getType() == t::Double);
        } else {
            // code->printCode(std::cout, true, true);
            std::cout << "Don't know how to unbox a " << type << "\n";
            val->printRef(std::cout);
            std::cout << "\n";
            assert(false);
        }
        // fall through, since more conversions might be needed after
        // unboxing
    }

    if (res->getType() == t::Int && needed == t::Double) {
        // TODO should we deal with na here?
        res = builder.CreateSIToFP(res, t::Double);
    } else if (res->getType() == t::Double && needed == t::Int) {
        // TODO should we deal with na here?
        res = builder.CreateFPToSI(res, t::Int);
    } else if ((res->getType() == t::Int || res->getType() == t::Double) &&
               needed == t::SEXP) {
        if (type.isA(PirType() | RType::integer)) {
            res = boxInt(res);
        } else if (type.isA(PirType() | RType::logical)) {
            res = boxLgl(res);
        } else if (type.isA(NativeType::test)) {
            res = boxTst(res);
        } else if (type.isA(PirType() | RType::real)) {
            res = boxReal(res);
        } else {
            std::cout << "Failed to convert int/float to " << type << "\n";
            Instruction::Cast(val)->print(std::cout);
            std::cout << "\n";
            code->printCode(std::cout, true, true);
            assert(false);
        }
    }

    if (res->getType() != needed) {
        std::cout << "Failed to load ";
        if (auto i = Instruction::Cast(val))
            i->print(std::cout, true);
        else
            val->printRef(std::cout);
        std::cout << " in the representation " << needed << "\n";
        assert(false);
    }

    return res;
}

llvm::Value* LowerFunctionLLVM::computeAndCheckIndex(Value* index,
                                                     llvm::Value* vector,
                                                     BasicBlock* fallback,
                                                     llvm::Value* max) {
    BasicBlock* hit1 = BasicBlock::Create(C, "", fun);
    BasicBlock* hit = BasicBlock::Create(C, "", fun);

    auto representation = Representation::Of(index);
    llvm::Value* nativeIndex = load(index);

    if (representation == Representation::Sexp) {
        if (Representation::Of(index->type) == Representation::Integer) {
            nativeIndex = unboxInt(nativeIndex);
            representation = Representation::Integer;
        } else {
            nativeIndex = unboxRealIntLgl(nativeIndex, index->type);
            representation = Representation::Real;
        }
    }

    if (representation == Representation::Real) {
        auto indexUnderRange = builder.CreateFCmpULT(nativeIndex, c(1.0));
        auto indexOverRange =
            builder.CreateFCmpUGE(nativeIndex, c((double)ULONG_MAX));
        auto indexNa = builder.CreateFCmpUNE(nativeIndex, nativeIndex);
        auto fail = builder.CreateOr(indexUnderRange,
                                     builder.CreateOr(indexOverRange, indexNa));

        builder.CreateCondBr(fail, fallback, hit1, branchMostlyFalse);
        builder.SetInsertPoint(hit1);

        nativeIndex = builder.CreateFPToUI(nativeIndex, t::i64);
    } else {
        assert(representation == Representation::Integer);
        auto indexUnderRange = builder.CreateICmpSLT(nativeIndex, c(1));
        auto indexNa = builder.CreateICmpEQ(nativeIndex, c(NA_INTEGER));
        auto fail = builder.CreateOr(indexUnderRange, indexNa);

        builder.CreateCondBr(fail, fallback, hit1, branchMostlyFalse);
        builder.SetInsertPoint(hit1);

        nativeIndex = builder.CreateZExt(nativeIndex, t::i64);
    }
    // R indexing is 1-based
    nativeIndex = builder.CreateSub(nativeIndex, c(1ul), "", true, true);

    auto ty = vector->getType();
    assert(ty == t::SEXP || ty == t::Int || ty == t::Double);
    if (!max)
        max = (ty == t::SEXP) ? vectorLength(vector) : c(1ul);
    auto indexOverRange = builder.CreateICmpUGE(nativeIndex, max);
    builder.CreateCondBr(indexOverRange, fallback, hit, branchMostlyFalse);
    builder.SetInsertPoint(hit);
    return nativeIndex;
}

void LowerFunctionLLVM::compilePopContext(Instruction* i) {
    auto popc = PopContext::Cast(i);
    auto data = contexts.at(popc->push());
    auto arg = load(popc->result());
    builder.CreateStore(arg, data.result);
    builder.CreateBr(data.popContextTarget);

    builder.SetInsertPoint(data.popContextTarget);
    llvm::Value* ret = builder.CreateLoad(data.result);
    llvm::Value* boxedRet = ret;
    if (ret->getType() == t::Int) {
        boxedRet = boxInt(ret, false);
    } else if (ret->getType() == t::Double) {
        boxedRet = boxReal(ret, false);
    }
    call(NativeBuiltins::endClosureContext, {data.rcntxt, boxedRet});
    inPushContext--;
    setVal(i, ret);
}

void LowerFunctionLLVM::compilePushContext(Instruction* i) {
    auto ct = PushContext::Cast(i);
    auto ast = loadSxp(ct->ast());
    auto op = loadSxp(ct->op());
    auto sysparent = loadSxp(ct->env());

    inPushContext++;

    // initialize a RCNTXT on the stack
    auto& data = contexts[i];

    std::vector<Value*> arglist;
    for (size_t i = 0; i < ct->narglist(); ++i) {
        arglist.push_back(ct->arg(i).val());
    }

    withCallFrame(arglist,
                  [&]() -> llvm::Value* {
                      return call(
                          NativeBuiltins::initClosureContext,
                          {ast, data.rcntxt, sysparent, op, c(ct->narglist())});
                  },
                  false);

    // Create a copy of all live variables to be able to restart
    // SEXPs are stored as local vars, primitive values are placed in an
    // alloca'd buffer
    std::vector<std::pair<Instruction*, Variable>> savedLocals;
    {
        for (auto& v : variables_) {
            auto& var = v.second;
            if (!var.initialized)
                continue;
            auto j = v.first;
            if (liveness.live(i, j)) {
                if (Representation::Of(j) == t::SEXP) {
                    savedLocals.push_back({j, Variable::MutableRVariable(
                                                  j, data.savedSexpPos.at(j),
                                                  builder, basepointer)});
                } else {
                    savedLocals.push_back(
                        {j, Variable::Mutable(
                                j, topAlloca(Representation::Of(j)))});
                }
            }
        }
        for (auto& v : savedLocals)
            v.second.set(builder, getVariable(v.first));
    }

    // Do a setjmp
    auto didLongjmp = BasicBlock::Create(C, "", fun);
    auto cont = BasicBlock::Create(C, "", fun);
    {
#ifdef __APPLE__
        auto setjmpBuf = builder.CreateGEP(data.rcntxt, {c(0), c(2), c(0)});
        auto setjmpType = FunctionType::get(
            t::i32, {PointerType::get(t::i32, 0), t::i32}, false);
        auto setjmpFun =
            JitLLVM::getFunctionDeclaration("sigsetjmp", setjmpType, builder);
#else
        auto setjmpBuf = builder.CreateGEP(data.rcntxt, {c(0), c(2)});
        auto setjmpType =
            FunctionType::get(t::i32, {t::setjmp_buf_ptr, t::i32}, false);
        auto setjmpFun =
            JitLLVM::getFunctionDeclaration("__sigsetjmp", setjmpType, builder);
#endif
        auto longjmp = builder.CreateCall(setjmpFun, {setjmpBuf, c(0)});

        builder.CreateCondBr(builder.CreateICmpEQ(longjmp, c(0)), cont,
                             didLongjmp);
    }

    // Handle Incomming longjumps
    {
        builder.SetInsertPoint(didLongjmp);
        llvm::Value* returned = builder.CreateLoad(
            builder.CreateIntToPtr(c((void*)&R_ReturnedValue), t::SEXP_ptr));
        auto restart =
            builder.CreateICmpEQ(returned, constant(R_RestartToken, t::SEXP));

        auto longjmpRestart = BasicBlock::Create(C, "", fun);
        auto longjmpRet = BasicBlock::Create(C, "", fun);
        builder.CreateCondBr(restart, longjmpRestart, longjmpRet);

        // The longjump returned a restart token.
        // In this case we need to restore all local variables as we
        // preserved them before the setjmp and then continue
        // execution
        builder.SetInsertPoint(longjmpRestart);
        for (auto& v : savedLocals)
            updateVariable(v.first, v.second.get(builder));

        // Also clear all binding caches
        for (const auto& be : bindingsCache)
            for (const auto& b : be.second)
                builder.CreateStore(
                    convertToPointer(nullptr, t::SEXP),
                    builder.CreateGEP(bindingsCacheBase, c(b.second)));
        builder.CreateBr(cont);

        // The longjump returned a value to return.
        // In this case we store the result and skip everything
        // until the matching popcontext
        builder.SetInsertPoint(longjmpRet);
        if (data.result->getType()->getPointerElementType() == t::Int) {
            returned = unboxIntLgl(returned);
        } else if (data.result->getType()->getPointerElementType() ==
                   t::Double) {
            returned = unboxRealIntLgl(returned, PirType(RType::real).scalar());
        }
        builder.CreateStore(returned, data.result);
        builder.CreateBr(data.popContextTarget);
    }

    builder.SetInsertPoint(cont);
}

llvm::Value* LowerFunctionLLVM::dataPtr(llvm::Value* v, bool enableAsserts) {
    assert(v->getType() == t::SEXP);
#ifdef ENABLE_SLOWASSERT
    if (enableAsserts)
        insn_assert(builder.CreateNot(isAltrep(v)),
                    "Trying to access an altrep vector");
#endif
    auto pos = builder.CreateBitCast(v, t::VECTOR_SEXPREC_ptr);
    return builder.CreateGEP(pos, c(1));
}

bool LowerFunctionLLVM::vectorTypeSupport(Value* vector) {
    auto type = vector->type;
    return type.isA(PirType(RType::vec).notObject()) ||
           type.isA(PirType(RType::integer).notObject()) ||
           type.isA(PirType(RType::logical).notObject()) ||
           type.isA(PirType(RType::real).notObject());
}

llvm::Value* LowerFunctionLLVM::vectorPositionPtr(llvm::Value* vector,
                                                  llvm::Value* position,
                                                  PirType type) {
    assert(vector->getType() == t::SEXP);
    PointerType* nativeType;
    if (type.isA(PirType(RType::integer).notObject()) ||
        type.isA(PirType(RType::logical).notObject())) {
        nativeType = t::IntPtr;
    } else if (type.isA(PirType(RType::real).notObject())) {
        nativeType = t::DoublePtr;
    } else if (type.isA(PirType(RType::vec).notObject())) {
        nativeType = t::SEXP_ptr;
    } else {
        nativeType = t::SEXP_ptr;
        assert(false);
    }
    auto pos = builder.CreateBitCast(dataPtr(vector), nativeType);
    return builder.CreateInBoundsGEP(pos, builder.CreateZExt(position, t::i64));
}

llvm::Value* LowerFunctionLLVM::accessVector(llvm::Value* vector,
                                             llvm::Value* position,
                                             PirType type) {
    return builder.CreateLoad(vectorPositionPtr(vector, position, type));
}

llvm::Value* LowerFunctionLLVM::assignVector(llvm::Value* vector,
                                             llvm::Value* position,
                                             llvm::Value* value, PirType type) {
    return builder.CreateStore(value,
                               vectorPositionPtr(vector, position, type));
}

llvm::Value* LowerFunctionLLVM::unboxIntLgl(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
    checkSexptype(v, {LGLSXP, INTSXP});
    auto pos = builder.CreateBitCast(dataPtr(v), t::IntPtr);
    return builder.CreateLoad(pos);
}
llvm::Value* LowerFunctionLLVM::unboxInt(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
#ifdef ENABLE_SLOWASSERT
    checkSexptype(v, {INTSXP});
    insn_assert(isScalar(v), "expected scalar int");
#endif
    auto pos = builder.CreateBitCast(dataPtr(v), t::IntPtr);
    return builder.CreateLoad(pos);
}
llvm::Value* LowerFunctionLLVM::unboxLgl(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
#ifdef ENABLE_SLOWASSERT
    checkSexptype(v, {LGLSXP});
    insn_assert(isScalar(v), "expected scalar lgl");
#endif
    auto pos = builder.CreateBitCast(dataPtr(v), t::IntPtr);
    return builder.CreateLoad(pos);
}
llvm::Value* LowerFunctionLLVM::unboxReal(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
#ifdef ENABLE_SLOWASSERT
    checkSexptype(v, {REALSXP});
    insn_assert(isScalar(v), "expected scalar real");
#endif
    auto pos = builder.CreateBitCast(dataPtr(v), t::DoublePtr);
    auto res = builder.CreateLoad(pos);
    return res;
}
llvm::Value* LowerFunctionLLVM::unboxRealIntLgl(llvm::Value* v,
                                                PirType toType) {
    assert(v->getType() == t::SEXP);
    auto done = BasicBlock::Create(C, "", fun);
    auto isReal = BasicBlock::Create(C, "isReal", fun);
    auto notReal = BasicBlock::Create(C, "notReal", fun);

    auto res = phiBuilder(t::Double);

    auto type = sexptype(v);
    auto tt = builder.CreateICmpEQ(type, c(REALSXP));
    builder.CreateCondBr(tt, isReal, notReal);

    builder.SetInsertPoint(notReal);

    auto intres = unboxIntLgl(v);

    auto isNaBr = BasicBlock::Create(C, "isNa", fun);
    nacheck(intres, toType, isNaBr);

    res.addInput(builder.CreateSIToFP(intres, t::Double));
    builder.CreateBr(done);

    builder.SetInsertPoint(isNaBr);
    res.addInput(c(R_NaN));
    builder.CreateBr(done);

    builder.SetInsertPoint(isReal);
    res.addInput(unboxReal(v));
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
    return res();
}

llvm::Value* LowerFunctionLLVM::argument(int i) {
    auto pos = builder.CreateGEP(paramArgs(), c(i));
    pos = builder.CreateGEP(pos, {c(0), c(1)});
    return builder.CreateLoad(t::SEXP, pos);
}

AllocaInst* LowerFunctionLLVM::topAlloca(llvm::Type* t, size_t len) {
    auto cur = builder.GetInsertBlock();
    builder.SetInsertPoint(entryBlock);
    auto res = builder.CreateAlloca(t, 0, c(len));
    builder.SetInsertPoint(cur);
    return res;
}

llvm::Value* LowerFunctionLLVM::convert(llvm::Value* val, PirType toType,
                                        bool protect) {
    auto to = Representation::Of(toType);
    auto from = val->getType();
    if (from == to)
        return val;

    if (from == t::SEXP && to == t::Int)
        return unboxIntLgl(val);
    if (from == t::SEXP && to == t::Double)
        return unboxRealIntLgl(val, toType);
    if (from != t::SEXP && to == t::SEXP)
        return box(val, toType, protect);

    if (from == t::Int && to == t::Double) {
        return builder.CreateSelect(builder.CreateICmpEQ(val, c(NA_INTEGER)),
                                    c(NA_REAL),
                                    builder.CreateSIToFP(val, t::Double));
    }
    if (from == t::Double && to == t::Int) {
        return builder.CreateSelect(builder.CreateFCmpUNE(val, val),
                                    c(NA_INTEGER),
                                    builder.CreateFPToSI(val, t::Int));
    }

    std::cout << "\nFailed to convert a " << val->getType() << " to " << toType
              << "\n";
    assert(false);
    return nullptr;
}

void LowerFunctionLLVM::setVal(Instruction* i, llvm::Value* val) {
    assert(i->producesRirResult() && !PushContext::Cast(i));
    val = convert(val, i->type, false);
    if (!val->hasName())
        val->setName(i->getRef());

    setVariable(i, val, inPushContext && escapesInlineContext.count(i));
}

llvm::Value* LowerFunctionLLVM::isExternalsxp(llvm::Value* v, uint32_t magic) {
    assert(v->getType() == t::SEXP);
    auto isExternalsxp = builder.CreateICmpEQ(c(EXTERNALSXP), sexptype(v));
    auto es = builder.CreateBitCast(dataPtr(v, false),
                                    PointerType::get(t::RirRuntimeObject, 0));
    auto magicVal = builder.CreateLoad(builder.CreateGEP(es, {c(0), c(2)}));
    auto isCorrectMagic = builder.CreateICmpEQ(magicVal, c(magic));
    return builder.CreateAnd(isExternalsxp, isCorrectMagic);
}

void LowerFunctionLLVM::checkSexptype(llvm::Value* v,
                                      const std::vector<SEXPTYPE>& types) {
#ifdef ENABLE_SLOWASSERT
    auto type = sexptype(v);
    llvm::Value* match = builder.getTrue();
    assert(types.size());
    for (auto t : types) {
        auto test = builder.CreateICmpEQ(type, c(t));
        match = builder.CreateOr(match, test);
    }
    insn_assert(match, "unexpexted sexptype");
#endif
}

void LowerFunctionLLVM::checkIsSexp(llvm::Value* v, const std::string& msg) {
#ifdef ENABLE_SLOWASSERT
    static bool checking = false;
    if (checking)
        return;
    checking = true;
    static std::vector<std::string> strings;
    strings.push_back(std::string("expected sexp got null ") + msg);
    insn_assert(builder.CreateICmpNE(convertToPointer(nullptr, t::SEXP), v),
                strings.back().c_str());
    auto type = sexptype(v);
    auto validType =
        builder.CreateOr(builder.CreateICmpULE(type, c(EXTERNALSXP)),
                         builder.CreateICmpEQ(type, c(FUNSXP)));
    strings.push_back(std::string("invalid sexptype ") + msg);
    insn_assert(validType, strings.back().c_str());
    checking = false;
#endif
}

llvm::Value* LowerFunctionLLVM::sxpinfoPtr(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
    checkIsSexp(v, "in sxpinfoPtr");
    auto sxpinfoPtr = builder.CreateGEP(t::SEXPREC, v, {c(0), c(0)});
    sxpinfoPtr->setName("sxpinfo");
    return builder.CreateBitCast(sxpinfoPtr, t::i64ptr);
}

void LowerFunctionLLVM::setSexptype(llvm::Value* v, int t) {
    auto ptr = sxpinfoPtr(v);
    llvm::Value* sxpinfo = builder.CreateLoad(ptr);
    sxpinfo =
        builder.CreateAnd(sxpinfo, c(~((unsigned long)(MAX_NUM_SEXPTYPE - 1))));
    sxpinfo = builder.CreateOr(sxpinfo, c(t, 64));
    builder.CreateStore(sxpinfo, ptr);
}

llvm::Value* LowerFunctionLLVM::sexptype(llvm::Value* v) {
    auto sxpinfo = builder.CreateLoad(sxpinfoPtr(v));
    auto t = builder.CreateAnd(sxpinfo, c(MAX_NUM_SEXPTYPE - 1, 64));
    return builder.CreateTrunc(t, t::Int);
}

llvm::Value* LowerFunctionLLVM::isVector(llvm::Value* v) {
    auto t = sexptype(v);
    return builder.CreateOr(
        builder.CreateICmpEQ(t, c(LGLSXP)),
        builder.CreateOr(
            builder.CreateICmpEQ(t, c(INTSXP)),
            builder.CreateOr(
                builder.CreateICmpEQ(t, c(REALSXP)),
                builder.CreateOr(
                    builder.CreateICmpEQ(t, c(CPLXSXP)),
                    builder.CreateOr(
                        builder.CreateICmpEQ(t, c(STRSXP)),
                        builder.CreateOr(
                            builder.CreateICmpEQ(t, c(RAWSXP)),
                            builder.CreateOr(
                                builder.CreateICmpEQ(t, c(VECSXP)),
                                builder.CreateICmpEQ(t, c(EXPRSXP)))))))));
}

llvm::Value* LowerFunctionLLVM::isMatrix(llvm::Value* v) {
    auto res = phiBuilder(t::i1);
    auto isVec = BasicBlock::Create(C, "", fun);
    auto notVec = BasicBlock::Create(C, "", fun);
    auto done = BasicBlock::Create(C, "", fun);
    builder.CreateCondBr(isVector(v), isVec, notVec);

    builder.SetInsertPoint(isVec);
    auto t =
        call(NativeBuiltins::getAttrb, {v, constant(R_DimSymbol, t::SEXP)});
    res.addInput(
        builder.CreateAnd(builder.CreateICmpEQ(sexptype(t), c(INTSXP)),
                          builder.CreateICmpEQ(vectorLength(t), c(2, 64))));
    builder.CreateBr(done);

    builder.SetInsertPoint(notVec);
    res.addInput(builder.getFalse());
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
    return res();
}

llvm::Value* LowerFunctionLLVM::isArray(llvm::Value* v) {
    auto res = phiBuilder(t::i1);
    auto isVec = BasicBlock::Create(C, "", fun);
    auto notVec = BasicBlock::Create(C, "", fun);
    auto done = BasicBlock::Create(C, "", fun);
    builder.CreateCondBr(isVector(v), isVec, notVec);

    builder.SetInsertPoint(isVec);
    auto t =
        call(NativeBuiltins::getAttrb, {v, constant(R_DimSymbol, t::SEXP)});
    res.addInput(
        builder.CreateAnd(builder.CreateICmpEQ(sexptype(t), c(INTSXP)),
                          builder.CreateICmpUGT(vectorLength(t), c(0, 64))));
    builder.CreateBr(done);

    builder.SetInsertPoint(notVec);
    res.addInput(builder.getFalse());
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
    return res();
}

llvm::Value* LowerFunctionLLVM::tag(llvm::Value* v) {
    auto pos = builder.CreateGEP(v, {c(0), c(4), c(2)});
    return builder.CreateLoad(pos);
}

void LowerFunctionLLVM::setCar(llvm::Value* x, llvm::Value* y,
                               bool needsWriteBarrier) {
    auto fast = [&]() {
        auto xx = builder.CreateGEP(x, {c(0), c(4), c(0)});
        builder.CreateStore(y, xx);
    };
    if (!needsWriteBarrier) {
        fast();
        return;
    }
    writeBarrier(x, y, fast, [&]() { call(NativeBuiltins::setCar, {x, y}); });
}

void LowerFunctionLLVM::setCdr(llvm::Value* x, llvm::Value* y,
                               bool needsWriteBarrier) {
    auto fast = [&]() {
        auto xx = builder.CreateGEP(x, {c(0), c(4), c(1)});
        builder.CreateStore(y, xx);
    };
    if (!needsWriteBarrier) {
        fast();
        return;
    }
    writeBarrier(x, y, fast, [&]() { call(NativeBuiltins::setCdr, {x, y}); });
}

void LowerFunctionLLVM::setTag(llvm::Value* x, llvm::Value* y,
                               bool needsWriteBarrier) {
    auto fast = [&]() {
        auto xx = builder.CreateGEP(x, {c(0), c(4), c(2)});
        builder.CreateStore(y, xx);
    };
    if (!needsWriteBarrier) {
        fast();
        return;
    }
    writeBarrier(x, y, fast, [&]() { call(NativeBuiltins::setTag, {x, y}); });
}

llvm::Value* LowerFunctionLLVM::car(llvm::Value* v) {
    v = builder.CreateGEP(v, {c(0), c(4), c(0)});
    return builder.CreateLoad(v);
}

llvm::Value* LowerFunctionLLVM::cdr(llvm::Value* v) {
    v = builder.CreateGEP(v, {c(0), c(4), c(1)});
    return builder.CreateLoad(v);
}

llvm::Value* LowerFunctionLLVM::attr(llvm::Value* v) {
    auto pos = builder.CreateGEP(v, {c(0), c(1)});
    return builder.CreateLoad(pos);
}

llvm::Value* LowerFunctionLLVM::isScalar(llvm::Value* v) {
    auto va = builder.CreateBitCast(v, t::VECTOR_SEXPREC_ptr);
    auto lp = builder.CreateGEP(va, {c(0), c(4), c(0)});
    auto l = builder.CreateLoad(lp);
    return builder.CreateICmpEQ(l, c(1, 64));
}

llvm::Value* LowerFunctionLLVM::isSimpleScalar(llvm::Value* v, SEXPTYPE t) {
    auto sxpinfo = builder.CreateLoad(sxpinfoPtr(v));

    auto type = builder.CreateAnd(sxpinfo, c(MAX_NUM_SEXPTYPE - 1, 64));
    auto okType = builder.CreateICmpEQ(c(t), builder.CreateTrunc(type, t::Int));

    auto isScalar = builder.CreateICmpNE(
        c(0, 64),
        builder.CreateAnd(sxpinfo, c((unsigned long)(1ul << (TYPE_BITS)))));

    auto noAttrib =
        builder.CreateICmpEQ(attr(v), constant(R_NilValue, t::SEXP));

    return builder.CreateAnd(okType, builder.CreateAnd(isScalar, noAttrib));
}

llvm::Value* LowerFunctionLLVM::vectorLength(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
    auto pos = builder.CreateBitCast(v, t::VECTOR_SEXPREC_ptr);
    pos = builder.CreateGEP(pos, {c(0), c(4), c(0)});
    return builder.CreateLoad(pos);
}
void LowerFunctionLLVM::assertNamed(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
    auto sxpinfoP = builder.CreateBitCast(sxpinfoPtr(v), t::i64ptr);
    auto sxpinfo = builder.CreateLoad(sxpinfoP);

    static auto namedMask = ((unsigned long)pow(2, NAMED_BITS) - 1) << 32;
    auto named = builder.CreateAnd(sxpinfo, c(namedMask));
    auto isNotNamed = builder.CreateICmpEQ(named, c(0, 64));

    auto notNamed = BasicBlock::Create(C, "notNamed", fun);
    auto ok = BasicBlock::Create(C, "", fun);

    builder.CreateCondBr(isNotNamed, notNamed, ok);

    builder.SetInsertPoint(notNamed);
    insn_assert(builder.getFalse(), "Value is not named");
    builder.CreateBr(ok);

    builder.SetInsertPoint(ok);
};

llvm::Value* LowerFunctionLLVM::shared(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
    auto sxpinfoP = builder.CreateBitCast(sxpinfoPtr(v), t::i64ptr);
    auto sxpinfo = builder.CreateLoad(sxpinfoP);

    static auto namedMask = ((unsigned long)pow(2, NAMED_BITS) - 1);
    auto named = builder.CreateLShr(sxpinfo, c(32ul));
    named = builder.CreateAnd(named, c(namedMask));
    return builder.CreateICmpUGT(named, c(1ul));
}

void LowerFunctionLLVM::ensureNamedIfNeeded(Instruction* i, llvm::Value* val) {
    if ((Representation::Of(i) == t::SEXP && variables_.count(i) &&
         variables_.at(i).initialized)) {

        auto adjust = refcount.atCreation.find(i);
        if (adjust != refcount.atCreation.end()) {
            if (adjust->second == NeedsRefcountAdjustment::SetShared) {
                if (!val)
                    val = load(i);
                ensureShared(val);
            } else if (adjust->second == NeedsRefcountAdjustment::EnsureNamed) {
                if (!val)
                    val = load(i);
                ensureShared(val);
            }
        }
    }
}

void LowerFunctionLLVM::ensureNamed(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
    auto sxpinfoP = builder.CreateBitCast(sxpinfoPtr(v), t::i64ptr);
    auto sxpinfo = builder.CreateLoad(sxpinfoP);

    static auto namedMask = ((unsigned long)pow(2, NAMED_BITS) - 1) << 32;
    unsigned long namedLSB = 1ul << 32;

    auto named = builder.CreateAnd(sxpinfo, c(namedMask));
    auto isNotNamed = builder.CreateICmpEQ(named, c(0, 64));

    auto notNamed = BasicBlock::Create(C, "notNamed", fun);
    auto ok = BasicBlock::Create(C, "", fun);

    builder.CreateCondBr(isNotNamed, notNamed, ok);

    builder.SetInsertPoint(notNamed);
    auto namedSxpinfo = builder.CreateOr(sxpinfo, c(namedLSB));
    builder.CreateStore(namedSxpinfo, sxpinfoP);
    builder.CreateBr(ok);

    builder.SetInsertPoint(ok);
};

void LowerFunctionLLVM::ensureShared(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
    auto sxpinfoP = sxpinfoPtr(v);
    auto sxpinfo = builder.CreateLoad(sxpinfoP);

    static auto namedMask = ((unsigned long)pow(2, NAMED_BITS) - 1);
    static auto namedNegMask = ~(namedMask << 32);

    auto named = builder.CreateLShr(sxpinfo, c(32, 64));
    named = builder.CreateAnd(named, c(namedMask));

    auto isNamedShared = builder.CreateICmpUGE(named, c(2, 64));

    auto incrementBr = BasicBlock::Create(C, "", fun);
    auto done = BasicBlock::Create(C, "", fun);

    builder.CreateCondBr(isNamedShared, done, incrementBr);

    builder.SetInsertPoint(incrementBr);
    auto newNamed = c(2ul << 32, 64);

    auto newSxpinfo = builder.CreateAnd(sxpinfo, c(namedNegMask));
    newSxpinfo = builder.CreateOr(newSxpinfo, newNamed);
    builder.CreateStore(newSxpinfo, sxpinfoP);
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
};

void LowerFunctionLLVM::incrementNamed(llvm::Value* v, int max) {
    assert(v->getType() == t::SEXP);
    auto sxpinfoP = sxpinfoPtr(v);
    auto sxpinfo = builder.CreateLoad(sxpinfoP);

    static auto namedMask = ((unsigned long)pow(2, NAMED_BITS) - 1);
    static auto namedNegMask = ~(namedMask << 32);

    auto named = builder.CreateLShr(sxpinfo, c(32, 64));
    named = builder.CreateAnd(named, c(namedMask));

    auto isNamedMax = builder.CreateICmpEQ(named, c(max, 64));

    auto incrementBr = BasicBlock::Create(C, "", fun);
    auto done = BasicBlock::Create(C, "", fun);

    builder.CreateCondBr(isNamedMax, done, incrementBr);

    builder.SetInsertPoint(incrementBr);
    auto newNamed = builder.CreateAdd(named, c(1, 64), "", true, true);
    newNamed = builder.CreateShl(newNamed, c(32, 64));

    auto newSxpinfo = builder.CreateAnd(sxpinfo, c(namedNegMask));
    newSxpinfo = builder.CreateOr(newSxpinfo, newNamed);
    builder.CreateStore(newSxpinfo, sxpinfoP);
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
};

void LowerFunctionLLVM::nacheck(llvm::Value* v, PirType type, BasicBlock* isNa,
                                BasicBlock* notNa) {
    assert(type.isA(PirType::num().scalar()));
    if (!notNa)
        notNa = BasicBlock::Create(C, "", fun);
    llvm::Value* isNotNa;
    if (!type.maybeNAOrNaN()) {
        // Don't actually check NA
        isNotNa = builder.getTrue();
    } else if (v->getType() == t::Double) {
        isNotNa = builder.CreateFCmpUEQ(v, v);
    } else {
        assert(v->getType() == t::Int);
        isNotNa = builder.CreateICmpNE(v, c(NA_INTEGER));
    }
    builder.CreateCondBr(isNotNa, notNa, isNa, branchMostlyTrue);
    builder.SetInsertPoint(notNa);
}

llvm::Value* LowerFunctionLLVM::checkDoubleToInt(llvm::Value* ld) {
    auto gt = builder.CreateFCmpOGT(ld, c((double)INT_MIN - 1));
    auto lt = builder.CreateFCmpOLT(ld, c((double)INT_MAX + 1));
    auto inrange = builder.CreateAnd(lt, gt);
    auto conv = createSelect2(inrange,
                              [&]() {
                                  // converting to signed int is not undefined
                                  // here since we first check that it does not
                                  // overflow
                                  auto conv = builder.CreateFPToSI(ld, t::i64);
                                  conv = builder.CreateSIToFP(conv, t::Double);
                                  return builder.CreateFCmpOEQ(ld, conv);
                              },
                              [&]() { return builder.getFalse(); });
    return conv;
}

void LowerFunctionLLVM::checkMissing(llvm::Value* v) {
    assert(v->getType() == t::SEXP);
    auto ok = BasicBlock::Create(C, "", fun);
    auto nok = BasicBlock::Create(C, "", fun);
    auto t = builder.CreateICmpEQ(v, constant(R_MissingArg, t::SEXP));
    builder.CreateCondBr(t, nok, ok, branchAlwaysFalse);

    builder.SetInsertPoint(nok);
    auto msg =
        builder.CreateGlobalString("argument is missing, with no default");
    call(NativeBuiltins::error, {builder.CreateInBoundsGEP(msg, {c(0), c(0)})});
    builder.CreateBr(ok);

    builder.SetInsertPoint(ok);
}

void LowerFunctionLLVM::checkUnbound(llvm::Value* v) {
    auto ok = BasicBlock::Create(C, "", fun);
    auto nok = BasicBlock::Create(C, "", fun);
    auto t = builder.CreateICmpEQ(v, constant(R_UnboundValue, t::SEXP));
    builder.CreateCondBr(t, nok, ok, branchAlwaysFalse);

    builder.SetInsertPoint(nok);
    auto msg = builder.CreateGlobalString("object not found");
    call(NativeBuiltins::error, {builder.CreateInBoundsGEP(msg, {c(0), c(0)})});
    builder.CreateBr(ok);

    builder.SetInsertPoint(ok);
}

llvm::Value* LowerFunctionLLVM::container(llvm::Value* v) {
    auto casted = builder.CreatePtrToInt(v, t::i64);
    auto container = builder.CreateSub(casted, c(sizeof(VECTOR_SEXPREC)));
    return builder.CreateIntToPtr(container, t::SEXP);
}

llvm::CallInst* LowerFunctionLLVM::call(const NativeBuiltin& builtin,
                                        const std::vector<llvm::Value*>& args) {
#ifdef ENABLE_SLOWASSERT
    // abuse BB label as comment
    auto callBB = BasicBlock::Create(C, builtin.name, fun);
    builder.CreateBr(callBB);
    builder.SetInsertPoint(callBB);
#endif
    return builder.CreateCall(JitLLVM::getBuiltin(builtin), args);
}

llvm::Value* LowerFunctionLLVM::box(llvm::Value* v, PirType t, bool protect) {
    llvm::Value* res = nullptr;
    if (t.isA(PirType(RType::integer).notObject()))
        res = boxInt(v, protect);
    if (t.isA(PirType(RType::logical).notObject()))
        res = boxLgl(v, protect);
    if (t.isA(PirType(RType::real).notObject()))
        res = boxReal(v, protect);
    assert(res);
    if (protect)
        protectTemp(res);
    return res;
}
llvm::Value* LowerFunctionLLVM::boxInt(llvm::Value* v, bool protect) {
    if (v->getType() == t::Int) {
        // std::ostringstream dbg;
        // (*currentInstr)->printRecursive(dbg, 2);
        // auto l = new std::string;
        // l->append(dbg.str());
        // return call(NativeBuiltins::newIntDebug,
        //             {v, c((unsigned long)l->data())});
        return call(NativeBuiltins::newInt, {v});
    }
    assert(v->getType() == t::Double);
    return call(NativeBuiltins::newIntFromReal, {v});
}
llvm::Value* LowerFunctionLLVM::boxReal(llvm::Value* v, bool potect) {
    if (v->getType() == t::Double)
        return call(NativeBuiltins::newReal, {v});
    assert(v->getType() == t::Int);
    return call(NativeBuiltins::newRealFromInt, {v});
}
llvm::Value* LowerFunctionLLVM::boxLgl(llvm::Value* v, bool protect) {
    if (v->getType() == t::Int)
        return call(NativeBuiltins::newLgl, {v});
    assert(v->getType() == t::Double);
    return call(NativeBuiltins::newLglFromReal, {v});
}
llvm::Value* LowerFunctionLLVM::boxTst(llvm::Value* v, bool protect) {
    assert(v->getType() == t::Int);
    return builder.CreateSelect(builder.CreateICmpNE(v, c(0)),
                                constant(R_TrueValue, t::SEXP),
                                constant(R_FalseValue, t::SEXP));
}

void LowerFunctionLLVM::protectTemp(llvm::Value* val) {
    assert(numTemps < MAX_TEMPS);
    setLocal(numLocals - 1 - numTemps++, val);
}

llvm::Value* LowerFunctionLLVM::depromise(llvm::Value* arg, const PirType& t) {

    if (!t.maybePromiseWrapped()) {
#ifdef ENABLE_SLOWASSERT
        insn_assert(builder.CreateICmpNE(sexptype(arg), c(PROMSXP)),
                    "Expected no promise");
#endif
        return arg;
    }
    auto isProm = BasicBlock::Create(C, "isProm", fun);
    auto isVal = BasicBlock::Create(C, "", fun);
    auto ok = BasicBlock::Create(C, "", fun);

    auto res = phiBuilder(t::SEXP);

    auto type = sexptype(arg);
    auto tt = builder.CreateICmpEQ(type, c(PROMSXP));
    builder.CreateCondBr(tt, isProm, isVal, branchMostlyFalse);

    builder.SetInsertPoint(isProm);
    auto val = car(arg);
    res.addInput(val);
    builder.CreateBr(ok);

    builder.SetInsertPoint(isVal);
#ifdef ENABLE_SLOWASSERT
    insn_assert(builder.CreateICmpNE(sexptype(arg), c(PROMSXP)),
                "Depromise returned promise");
#endif
    res.addInput(arg);
    builder.CreateBr(ok);

    builder.SetInsertPoint(ok);
    return res();
}

llvm::Value* LowerFunctionLLVM::depromise(Value* v) {
    if (!v->type.maybePromiseWrapped())
        return loadSxp(v);
    assert(Representation::Of(v) == t::SEXP);
    return depromise(loadSxp(v), v->type);
}

void LowerFunctionLLVM::compileRelop(
    Instruction* i,
    const std::function<llvm::Value*(llvm::Value*, llvm::Value*)>& intInsert,
    const std::function<llvm::Value*(llvm::Value*, llvm::Value*)>& fpInsert,
    BinopKind kind) {
    auto rep = Representation::Of(i);
    auto lhs = i->arg(0).val();
    auto rhs = i->arg(1).val();
    auto lhsRep = Representation::Of(lhs);
    auto rhsRep = Representation::Of(rhs);
    if (lhsRep == Representation::Sexp || rhsRep == Representation::Sexp) {
        auto a = loadSxp(lhs);
        auto b = loadSxp(rhs);

        llvm::Value* res;
        if (i->hasEnv()) {
            auto e = loadSxp(i->env());
            res = call(NativeBuiltins::binopEnv,
                       {a, b, e, c(i->srcIdx), c((int)kind)});
        } else {
            res = call(NativeBuiltins::binop, {a, b, c((int)kind)});
        }
        setVal(i, res);
        return;
    }

    auto isNaBr = BasicBlock::Create(C, "isNa", fun);
    auto done = BasicBlock::Create(C, "", fun);

    auto res = phiBuilder(t::Int);
    auto a = load(lhs, lhsRep);
    auto b = load(rhs, rhsRep);

    nacheck(a, lhs->type, isNaBr);
    nacheck(b, rhs->type, isNaBr);

    if (a->getType() == t::Int && b->getType() == t::Int) {
        res.addInput(builder.CreateZExt(intInsert(a, b), t::Int));
    } else {
        if (a->getType() == t::Int)
            a = builder.CreateSIToFP(a, t::Double);
        if (b->getType() == t::Int)
            b = builder.CreateSIToFP(b, t::Double);
        res.addInput(builder.CreateZExt(fpInsert(a, b), t::Int));
    }

    builder.CreateBr(done);

    builder.SetInsertPoint(isNaBr);
    res.addInput(c(NA_INTEGER));
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
    if (rep == Representation::Sexp) {
        setVal(i, boxLgl(res(), false));
    } else {
        setVal(i, res());
    }
};

void LowerFunctionLLVM::compileBinop(
    Instruction* i, Value* lhs, Value* rhs,
    const std::function<llvm::Value*(llvm::Value*, llvm::Value*)>& intInsert,
    const std::function<llvm::Value*(llvm::Value*, llvm::Value*)>& fpInsert,
    BinopKind kind) {
    auto rep = Representation::Of(i);
    auto lhsRep = Representation::Of(lhs);
    auto rhsRep = Representation::Of(rhs);

    if (lhsRep == Representation::Sexp || rhsRep == Representation::Sexp ||
        (!fpInsert && (lhsRep != Representation::Integer ||
                       rhsRep != Representation::Integer))) {
        auto a = loadSxp(lhs);
        auto b = loadSxp(rhs);

        llvm::Value* res = nullptr;
        if (i->hasEnv()) {
            auto e = loadSxp(i->env());
            res = call(NativeBuiltins::binopEnv,
                       {a, b, e, c(i->srcIdx), c((int)kind)});
        } else {
            res = call(NativeBuiltins::binop, {a, b, c((int)kind)});
        }

        setVal(i, res);
        return;
    }

    BasicBlock* isNaBr = nullptr;
    auto done = BasicBlock::Create(C, "", fun);

    auto r = (lhsRep == Representation::Real || rhsRep == Representation::Real)
                 ? t::Double
                 : t::Int;

    auto res = phiBuilder(r);
    auto a = load(lhs, lhsRep);
    auto b = load(rhs, rhsRep);

    auto checkNa = [&](llvm::Value* llvmValue, PirType type, Representation r) {
        if (type.maybeNAOrNaN()) {
            if (r == Representation::Integer) {
                if (!isNaBr)
                    isNaBr = BasicBlock::Create(C, "isNa", fun);
                nacheck(llvmValue, type, isNaBr);
            }
        }
    };
    checkNa(a, lhs->type, lhsRep);
    checkNa(b, rhs->type, rhsRep);

    if (a->getType() == t::Int && b->getType() == t::Int) {
        res.addInput(intInsert(a, b));
    } else {
        if (a->getType() == t::Int)
            a = builder.CreateSIToFP(a, t::Double);
        if (b->getType() == t::Int)
            b = builder.CreateSIToFP(b, t::Double);
        res.addInput(fpInsert(a, b));
    }
    builder.CreateBr(done);

    if (lhsRep == Representation::Integer ||
        rhsRep == Representation::Integer) {
        if (isNaBr) {
            builder.SetInsertPoint(isNaBr);

            if (r == t::Int)
                res.addInput(c(NA_INTEGER));
            else
                res.addInput(c((double)R_NaN));

            builder.CreateBr(done);
        }
    }

    builder.SetInsertPoint(done);
    if (rep == Representation::Sexp) {
        setVal(i, box(res(), lhs->type.mergeWithConversion(rhs->type), false));
    } else {
        setVal(i, res());
    }
};

void LowerFunctionLLVM::compileUnop(
    Instruction* i, Value* arg,
    const std::function<llvm::Value*(llvm::Value*)>& intInsert,
    const std::function<llvm::Value*(llvm::Value*)>& fpInsert, UnopKind kind) {
    auto argRep = Representation::Of(arg);

    if (argRep == Representation::Sexp) {
        auto a = loadSxp(arg);

        llvm::Value* res = nullptr;
        if (i->hasEnv()) {
            auto e = loadSxp(i->env());
            res = call(NativeBuiltins::unopEnv,
                       {a, e, c(i->srcIdx), c((int)kind)});
        } else {
            res = call(NativeBuiltins::unop, {a, c((int)kind)});
        }

        setVal(i, res);
        return;
    }

    BasicBlock* isNaBr = nullptr;
    auto done = BasicBlock::Create(C, "", fun);

    auto r = (argRep == Representation::Real) ? t::Double : t::Int;

    auto res = phiBuilder(r);
    auto a = load(arg, argRep);

    auto checkNa = [&](llvm::Value* value, PirType type, Representation r) {
        if (type.maybeNAOrNaN()) {
            if (r == Representation::Integer) {
                if (!isNaBr)
                    isNaBr = BasicBlock::Create(C, "isNa", fun);
                nacheck(value, type, isNaBr);
            }
        }
    };
    checkNa(a, arg->type, argRep);

    if (a->getType() == t::Int) {
        res.addInput(intInsert(a));
    } else {
        res.addInput(fpInsert(a));
    }
    builder.CreateBr(done);

    if (argRep == Representation::Integer) {
        if (isNaBr) {
            builder.SetInsertPoint(isNaBr);

            if (r == t::Int)
                res.addInput(c(NA_INTEGER));
            else
                res.addInput(c((double)R_NaN));

            builder.CreateBr(done);
        }
    }

    builder.SetInsertPoint(done);
    setVal(i, res());
};

void LowerFunctionLLVM::writeBarrier(llvm::Value* x, llvm::Value* y,
                                     std::function<void()> no,
                                     std::function<void()> yes) {
    auto sxpinfoX = builder.CreateLoad(sxpinfoPtr(x));

    auto markBitPos = c((unsigned long)(1ul << (TYPE_BITS + 19)));
    auto genBitPos = c((unsigned long)(1ul << (TYPE_BITS + 23)));

    auto done = BasicBlock::Create(C, "", fun);
    auto noBarrier = BasicBlock::Create(C, "", fun);
    auto maybeNeedsBarrier = BasicBlock::Create(C, "", fun);
    auto maybeNeedsBarrier2 = BasicBlock::Create(C, "", fun);
    auto needsBarrier = BasicBlock::Create(C, "", fun);

    auto markBitX =
        builder.CreateICmpNE(builder.CreateAnd(sxpinfoX, markBitPos), c(0, 64));
    builder.CreateCondBr(markBitX, maybeNeedsBarrier, noBarrier);

    builder.SetInsertPoint(maybeNeedsBarrier);
    auto sxpinfoY = builder.CreateLoad(sxpinfoPtr(y));
    auto markBitY =
        builder.CreateICmpNE(builder.CreateAnd(sxpinfoY, markBitPos), c(0, 64));
    builder.CreateCondBr(markBitY, maybeNeedsBarrier2, needsBarrier);
    builder.SetInsertPoint(maybeNeedsBarrier2);

    auto genBitX = builder.CreateAnd(sxpinfoX, genBitPos);
    auto genBitY = builder.CreateAnd(sxpinfoY, genBitPos);
    auto olderGen = builder.CreateICmpUGT(genBitX, genBitY);
    builder.CreateCondBr(olderGen, needsBarrier, noBarrier, branchMostlyFalse);

    builder.SetInsertPoint(noBarrier);
    no();
    builder.CreateBr(done);

    builder.SetInsertPoint(needsBarrier);
    yes();
    builder.CreateBr(done);

    builder.SetInsertPoint(done);
};

bool LowerFunctionLLVM::compileDotcall(
    Instruction* i, const std::function<llvm::Value*()>& callee,
    const std::function<SEXP(size_t)>& names) {
    auto calli = CallInstruction::CastCall(i);
    assert(calli);
    std::vector<Value*> args;
    std::vector<BC::PoolIdx> newNames;
    bool seenDots = false;
    size_t pos = 0;
    calli->eachCallArg([&](Value* v) {
        if (auto exp = ExpandDots::Cast(v)) {
            args.push_back(exp);
            newNames.push_back(Pool::insert(R_DotsSymbol));
            seenDots = true;
        } else {
            assert(!DotsList::Cast(v));
            newNames.push_back(Pool::insert(names(pos)));
            args.push_back(v);
        }
        pos++;
    });
    if (!seenDots)
        return false;
    Context asmpt = calli->inferAvailableAssumptions();
    auto namesConst = c(newNames);
    auto namesStore = globalConst(namesConst);

    setVal(i,
           withCallFrame(
               args,
               [&]() -> llvm::Value* {
                   return call(NativeBuiltins::dotsCall,
                               {
                                   paramCode(),
                                   c(i->srcIdx),
                                   callee(),
                                   i->hasEnv() ? loadSxp(i->env())
                                               : constant(R_BaseEnv, t::SEXP),
                                   c(calli->nCallArgs()),
                                   builder.CreateBitCast(namesStore, t::IntPtr),
                                   c(asmpt.toI()),
                               });
               },
               /* dotCall pops arguments : */ false));
    return true;
}

llvm::Value* LowerFunctionLLVM::loadPromise(llvm::Value* x, int i) {
    assert(x->getType() != t::SEXP);
    auto code = builder.CreatePtrToInt(x, t::i64);
    auto extraPos = builder.CreateAdd(code, c(rir::Code::extraPtrOffset(), 64));
    auto extraPtr = builder.CreateIntToPtr(extraPos, t::SEXP_ptr);
    auto extra = builder.CreateLoad(extraPtr);
    return accessVector(extra, c(i), PirType(RType::vec).notObject());
}

llvm::Value* LowerFunctionLLVM::envStubGet(llvm::Value* x, int i, size_t size) {
    // We could use externalsxpGetEntry, but this is faster
    assert(x->getType() == t::SEXP);
#ifdef ENABLE_SLOWASSERT
    insn_assert(isExternalsxp(x, LAZY_ENVIRONMENT_MAGIC),
                "envStubGet on something which is not an env stub");
#endif
    auto le = builder.CreateBitCast(dataPtr(x, false),
                                    PointerType::get(t::LazyEnvironment, 0));
    auto missingBits =
        builder.CreateBitCast(builder.CreateGEP(le, c(1)), t::i8ptr);
    auto payload = builder.CreateBitCast(
        builder.CreateGEP(missingBits, c(size)), t::SEXP_ptr);
    auto pos = builder.CreateGEP(payload, c(i + LazyEnvironment::ArgOffset));
    return builder.CreateLoad(pos);
}

void LowerFunctionLLVM::envStubSetNotMissing(llvm::Value* x, int i) {
    auto le = builder.CreateBitCast(dataPtr(x, false),
                                    PointerType::get(t::LazyEnvironment, 0));
    auto missingBits =
        builder.CreateBitCast(builder.CreateGEP(le, c(1)), t::i8ptr);
    auto pos = builder.CreateGEP(missingBits, c(i));
    builder.CreateStore(c(0, 8), pos);
}

void LowerFunctionLLVM::envStubSetMissing(llvm::Value* x, int i) {
    auto le = builder.CreateBitCast(dataPtr(x, false),
                                    PointerType::get(t::LazyEnvironment, 0));
    auto missingBits =
        builder.CreateBitCast(builder.CreateGEP(le, c(1)), t::i8ptr);
    auto pos = builder.CreateGEP(missingBits, c(i));
    builder.CreateStore(c(1, 8), pos);
}

void LowerFunctionLLVM::envStubSet(llvm::Value* x, int i, llvm::Value* y,
                                   size_t size, bool setNotMissing) {
    // We could use externalsxpSetEntry, but this is faster
    writeBarrier(
        x, y,
        [&]() {
            assert(x->getType() == t::SEXP);
#ifdef ENABLE_SLOWASSERT
            insn_assert(isExternalsxp(x, LAZY_ENVIRONMENT_MAGIC),
                        "envStubGet on something which is not an env stub");
#endif
            auto le = builder.CreateBitCast(
                dataPtr(x, false), PointerType::get(t::LazyEnvironment, 0));
            auto missingBits =
                builder.CreateBitCast(builder.CreateGEP(le, c(1)), t::i8ptr);
            auto payload = builder.CreateBitCast(
                builder.CreateGEP(missingBits, c(size)), t::SEXP_ptr);
            auto pos =
                builder.CreateGEP(payload, c(i + LazyEnvironment::ArgOffset));
            builder.CreateStore(y, pos);
        },
        [&]() {
            call(NativeBuiltins::externalsxpSetEntry,
                 {{x, c(i + LazyEnvironment::ArgOffset), y}});
        });
    if (setNotMissing) {
        auto le = builder.CreateBitCast(
            dataPtr(x, false), PointerType::get(t::LazyEnvironment, 0));
        auto missingBits =
            builder.CreateBitCast(builder.CreateGEP(le, c(1)), t::i8ptr);
        auto pos = builder.CreateGEP(missingBits, c(i));
        builder.CreateStore(c(0, 8), pos);
    }
}

llvm::Value* LowerFunctionLLVM::isObj(llvm::Value* v) {
    checkIsSexp(v, "in IsObj");
    auto sxpinfo = builder.CreateLoad(sxpinfoPtr(v));
    return builder.CreateICmpNE(
        c(0, 64),
        builder.CreateAnd(sxpinfo, c((unsigned long)(1ul << (TYPE_BITS + 1)))));
};

llvm::Value* LowerFunctionLLVM::fastVeceltOkNative(llvm::Value* v) {
    checkIsSexp(v, "in IsFastVeceltOkNative");
    auto attrs = attr(v);
    auto isNil = builder.CreateICmpEQ(attrs, constant(R_NilValue, t::SEXP));
    auto isMatr1 =
        builder.CreateICmpEQ(tag(attrs), constant(R_DimSymbol, t::SEXP));
    auto isMatr2 =
        builder.CreateICmpEQ(cdr(attrs), constant(R_NilValue, t::SEXP));
    auto isMatr = builder.CreateAnd(isMatr1, isMatr2);
    return builder.CreateOr(isNil, isMatr);
};

llvm::Value* LowerFunctionLLVM::isAltrep(llvm::Value* v) {
    checkIsSexp(v, "in is altrep");
    auto sxpinfo = builder.CreateLoad(sxpinfoPtr(v));
    return builder.CreateICmpNE(
        c(0, 64),
        builder.CreateAnd(sxpinfo, c((unsigned long)(1ul << (TYPE_BITS + 2)))));
};

llvm::Value* LowerFunctionLLVM::createSelect2(
    llvm::Value* cond, std::function<llvm::Value*()> trueValueAction,
    std::function<llvm::Value*()> falseValueAction) {

    auto intialInsertionPoint = builder.GetInsertBlock();

    auto trueBranch = BasicBlock::Create(C, "", fun);
    auto truePred = intialInsertionPoint;
    builder.SetInsertPoint(trueBranch);
    auto trueValue = trueValueAction();
    auto trueBranchIsEmpty = trueBranch->empty();
    if (trueBranchIsEmpty) {
        trueBranch->removeFromParent();
        delete trueBranch;
    } else {
        truePred = builder.GetInsertBlock();
    }

    auto falseBranch = BasicBlock::Create(C, "", fun);
    auto falsePred = intialInsertionPoint;
    builder.SetInsertPoint(falseBranch);
    auto falseValue = falseValueAction();
    auto falseBranchIsEmpty = falseBranch->empty();
    if (falseBranchIsEmpty) {
        falseBranch->removeFromParent();
        delete falseBranch;
    } else {
        falsePred = builder.GetInsertBlock();
    }

    if (trueBranchIsEmpty && falseBranchIsEmpty) {
        builder.SetInsertPoint(intialInsertionPoint);
        return builder.CreateSelect(cond, trueValue, falseValue);
    }

    auto next = BasicBlock::Create(C, "", fun);

    PhiBuilder res(builder, trueValue->getType());

    auto trueBranchForCond = trueBranch;
    builder.SetInsertPoint(truePred);
    if (!trueBranchIsEmpty)
        builder.CreateBr(next);
    else
        trueBranchForCond = next;
    res.addInput(trueValue);

    auto falseBranchForCond = falseBranch;
    builder.SetInsertPoint(falsePred);
    if (!falseBranchIsEmpty)
        builder.CreateBr(next);
    else
        falseBranchForCond = next;
    res.addInput(falseValue);

    builder.SetInsertPoint(intialInsertionPoint);
    builder.CreateCondBr(cond, trueBranchForCond, falseBranchForCond);

    builder.SetInsertPoint(next);
    auto r = res();

    return r;
};

void LowerFunctionLLVM::compile() {

    {
        auto arg = fun->arg_begin();
        for (size_t i = 0; i < argNames.size(); ++i) {
            args.push_back(arg);
            args.back()->setName(argNames[i]);
            arg++;
        }
    }

    std::unordered_map<BB*, BasicBlock*> blockMapping_;
    auto getBlock = [&](BB* bb) {
        auto b = blockMapping_.find(bb);
        if (b != blockMapping_.end()) {
            return b->second;
        }
        std::stringstream ss;
        ss << "BB" << bb->id;
        return blockMapping_[bb] = BasicBlock::Create(C, ss.str(), fun);
    };
    entryBlock = BasicBlock::Create(C, "", fun);
    builder.SetInsertPoint(entryBlock);
    nodestackPtrAddr = convertToPointer(&R_BCNodeStackTop,
                                        PointerType::get(t::stackCellPtr, 0));
    basepointer = nodestackPtr();

    numLocals++;
    // Store the code object as the first element of our frame, for the value
    // profiler to find it.
    incStack(1, false);
    stack({container(paramCode())});
    {
        SmallSet<std::pair<Value*, SEXP>> bindings;
        Visitor::run(code->entry, [&](Instruction* i) {
            SEXP varName = nullptr;
            if (auto l = LdVar::Cast(i))
                varName = l->varName;
            else if (auto l = StVar::Cast(i))
                varName = l->varName;
            else if (LdDots::Cast(i))
                varName = R_DotsSymbol;

            if (varName) {
                auto e = MkEnv::Cast(i->env());
                if (e && !e->stub) {
                    bindings.insert(std::pair<Value*, SEXP>(i->env(), varName));
                }
            }
        });
        size_t idx = 0;
        for (auto& b : bindings) {
            bindingsCache[b.first][b.second] = idx++;
        }
        bindingsCacheBase = topAlloca(t::SEXP, idx);
    }

    std::unordered_map<Instruction*, Instruction*> phis;
    {
        NativeAllocator allocator(code, cls, liveness, log);
        allocator.compute();
        allocator.verify();
        auto numLocalsBase = numLocals;
        numLocals += allocator.slots();

        auto createVariable = [&](Instruction* i, bool mut) -> void {
            if (Representation::Of(i) == Representation::Sexp) {
                if (mut)
                    variables_[i] = Variable::MutableRVariable(
                        i, allocator[i] + numLocalsBase, builder, basepointer);
                else
                    variables_[i] = Variable::RVariable(
                        i, allocator[i] + numLocalsBase, builder, basepointer);
            } else {
                if (mut)
                    variables_[i] =
                        Variable::Mutable(i, topAlloca(Representation::Of(i)));
                else
                    variables_[i] = Variable::Immutable(i);
            }
        };

        constantpool = builder.CreateIntToPtr(c(globalContext()), t::SEXP_ptr);
        constantpool = builder.CreateGEP(constantpool, c(1));

        Visitor::run(code->entry, [&](BB* bb) {
            for (auto i : *bb) {
                if (!liveness.count(i) || !allocator.needsAVariable(i))
                    continue;
                if (auto phi = Phi::Cast(i)) {
                    createVariable(phi, true);
                    phi->eachArg([&](BB*, Value* v) {
                        auto i = Instruction::Cast(v);
                        assert(i);
                        phis[i] = phi;
                    });
                }
            }
        });

        Visitor::run(code->entry, [&](Instruction* i) {
            if (auto pop = PopContext::Cast(i)) {
                auto res = pop->result();
                auto push = pop->push();
                auto resStore = topAlloca(Representation::Of(res));
                auto rcntxt = topAlloca(t::RCNTXT);
                contexts[push] = {rcntxt, resStore,
                                  BasicBlock::Create(C, "", fun)};

                // Everything which is live at the Push context needs to be
                // mutable, to be able to restore on restart
                Visitor::run(code->entry, [&](Instruction* j) {
                    if (allocator.needsAVariable(j)) {
                        if (Representation::Of(j) == t::SEXP &&
                            liveness.live(push, j)) {
                            contexts[push].savedSexpPos[j] = numLocals++;
                        }
                        if (!liveness.live(push, j) && liveness.live(pop, j))
                            escapesInlineContext.insert(j);
                        if (!variables_.count(j) &&
                            (liveness.live(push, j) || liveness.live(pop, j)))
                            createVariable(j, true);
                    }
                });
            }
        });
        Visitor::run(code->entry, [&](Instruction* i) {
            if (allocator.needsAVariable(i) && liveness.count(i) &&
                !variables_.count(i))
                createVariable(i, false);
        });
    }

    numLocals += MAX_TEMPS;
    if (numLocals > 1)
        incStack(numLocals - 1, true);

    std::unordered_map<BB*, int> blockInPushContext;
    blockInPushContext[code->entry] = 0;

    LoweringVisitor::run(code->entry, [&](BB* bb) {
        currentBB = bb;

        builder.SetInsertPoint(getBlock(bb));
        inPushContext = blockInPushContext.at(bb);

        for (auto it = bb->begin(); it != bb->end(); ++it) {
            currentInstr = it;
            auto i = *it;

            auto adjustRefcount = refcount.beforeUse.find(i);
            if (adjustRefcount != refcount.beforeUse.end()) {
                i->eachArg([&](Value* v) {
                    if (Representation::Of(v) != t::SEXP)
                        return;
                    if (auto j = Instruction::Cast(v->followCasts())) {
                        auto needed = adjustRefcount->second.find(j);
                        if (needed != adjustRefcount->second.end()) {
                            auto kind = needed->second;
                            if (kind == NeedsRefcountAdjustment::SetShared)
                                ensureShared(load(v));
                            else if (kind ==
                                     NeedsRefcountAdjustment::EnsureNamed)
                                ensureNamed(load(v));
                        }
                    }
                });
            }

            switch (i->tag) {
            case Tag::ExpandDots: {
                auto in = i->arg(0).val();
                if (!deadMove(in, i))
                    setVal(i, load(i->arg(0).val()));
                break;
            }

            case Tag::DotsList: {
                auto mk = DotsList::Cast(i);
                auto arglist = constant(R_NilValue, t::SEXP);
                mk->eachElementRev([&](SEXP name, Value* v) {
                    auto val = loadSxp(v);
                    incrementNamed(val);
                    arglist = call(NativeBuiltins::consNr, {val, arglist});
                    setTag(arglist, constant(name, t::SEXP), false);
                });
                setSexptype(arglist, DOTSXP);
                setVal(i, arglist);
                break;
            }

            case Tag::RecordDeoptReason: {
                auto rec = RecordDeoptReason::Cast(i);
                auto reason = llvm::ConstantStruct::get(
                    t::DeoptReason, {
                                        c(rec->reason.reason, 32),
                                        convertToPointer(rec->reason.srcCode),
                                        c(rec->reason.originOffset),
                                    });
                call(NativeBuiltins::recordDeopt,
                     {loadSxp(rec->arg<0>().val()), globalConst(reason)});
                break;
            }

            case Tag::PushContext: {
                compilePushContext(i);
                break;
            }

            case Tag::PopContext: {
                compilePopContext(i);
                break;
            }

            case Tag::CastType: {
                auto in = i->arg(0).val();
                if (LdConst::Cast(i->followCasts()) || deadMove(in, i))
                    break;
                setVal(i, load(in, i->type, Representation::Of(i)));
                break;
            }

            case Tag::PirCopy: {
                auto in = i->arg(0).val();
                if (!deadMove(in, i))
                    setVal(i, load(in, Representation::Of(i)));
                break;
            }

            case Tag::Phi:
                break;

            case Tag::LdArg:
                setVal(i, argument(LdArg::Cast(i)->id));
                break;

            case Tag::LdFunctionEnv:
                setVal(i, paramEnv());
                break;

            case Tag::Invisible:
                setVisible(0);
                break;

            case Tag::Visible:
                setVisible(1);
                break;

            case Tag::Identical: {
                auto a = i->arg(0).val();
                auto b = i->arg(1).val();
                auto cc = LdConst::Cast(a);
                if (!cc)
                    cc = LdConst::Cast(b);

                if (cc) {
                    auto v = cc == a ? b : a;
                    auto vi = depromise(v);
                    auto res =
                        builder.CreateICmpEQ(constant(cc->c(), t::SEXP), vi);
                    if (TYPEOF(cc->c()) == CLOSXP) {
                        res = createSelect2(
                            res, [&]() { return builder.getTrue(); },
                            [&]() {
                                return createSelect2(
                                    builder.CreateICmpEQ(sexptype(vi),
                                                         c(CLOSXP)),
                                    [&]() {
                                        return call(
                                            NativeBuiltins::clsEq,
                                            {constant(cc->c(), t::SEXP), vi});
                                    },
                                    [&]() { return builder.getFalse(); });
                            });
                    }
                    setVal(i, builder.CreateZExt(res, t::Int));
                    break;
                }

                auto ai = depromise(a);
                auto bi = depromise(b);

                auto res = builder.CreateICmpEQ(ai, bi);
                res = createSelect2(
                    res, [&]() { return builder.getTrue(); },
                    [&]() {
                        auto cls = builder.CreateAnd(
                            builder.CreateICmpEQ(sexptype(ai), c(CLOSXP)),
                            builder.CreateICmpEQ(sexptype(bi), c(CLOSXP)));
                        return createSelect2(
                            cls,
                            [&]() {
                                return call(NativeBuiltins::clsEq, {ai, bi});
                            },
                            [&]() { return builder.getFalse(); });
                    });
                setVal(i, builder.CreateZExt(res, t::Int));
                break;
            }

            case Tag::CallSafeBuiltin: {
                auto b = CallSafeBuiltin::Cast(i);
                if (compileDotcall(
                        b, [&]() { return constant(b->builtinSexp, t::SEXP); },
                        [&](size_t i) { return R_NilValue; })) {
                    break;
                }
                std::vector<Value*> args;
                b->eachCallArg([&](Value* v) { args.push_back(v); });

                auto callTheBuiltin = [&]() -> llvm::Value* {
                    // Some "safe" builtins still look up functions in the base
                    // env
                    return callRBuiltin(b->builtinSexp, args, i->srcIdx,
                                        b->builtin,
                                        constant(R_BaseEnv, t::SEXP));
                };

                auto fixVisibility = [&]() {
                    if (!b->effects.contains(Effect::Visibility))
                        return;
                    int flag = getFlag(b->builtinId);
                    if (flag < 2)
                        setVisible(flag != 1);
                };

                // TODO: this should probably go somewhere else... This is
                // an inlined version of bitwise builtins
                if (Representation::Of(b) == Representation::Integer) {
                    if (b->nargs() == 2) {
                        auto x = b->arg(0).val();
                        auto y = b->arg(1).val();
                        auto xRep = Representation::Of(x);
                        auto yRep = Representation::Of(y);

                        static auto bitwise = {
                            blt("bitwiseShiftL"), blt("bitwiseShiftR"),
                            blt("bitwiseAnd"),    blt("bitwiseOr"),
                            blt("bitwiseXor"),
                        };
                        auto found = std::find(bitwise.begin(), bitwise.end(),
                                               b->builtinId);
                        if (found != bitwise.end()) {
                            const static PirType num =
                                (PirType() | RType::integer | RType::logical |
                                 RType::real)
                                    .notObject()
                                    .scalar();

                            if (xRep == Representation::Sexp &&
                                x->type.isA(num))
                                xRep = Representation::Real;
                            if (yRep == Representation::Sexp &&
                                y->type.isA(num))
                                yRep = Representation::Real;

                            if (xRep != Representation::Sexp &&
                                yRep != Representation::Sexp) {

                                BasicBlock* isNaBr = nullptr;
                                auto done = BasicBlock::Create(C, "", fun);

                                auto res = phiBuilder(t::Int);

                                auto xInt = load(x, Representation::Integer);
                                auto yInt = load(y, Representation::Integer);

                                auto naCheck = [&](Value* v, llvm::Value* asInt,
                                                   Representation rep) {
                                    if (v->type.maybeNAOrNaN()) {
                                        if (rep == Representation::Real) {
                                            auto vv = load(v, rep);
                                            if (!isNaBr)
                                                isNaBr = BasicBlock::Create(
                                                    C, "isNa", fun);
                                            nacheck(vv, v->type, isNaBr);
                                        } else {
                                            assert(rep ==
                                                   Representation::Integer);
                                            if (!isNaBr)
                                                isNaBr = BasicBlock::Create(
                                                    C, "isNa", fun);
                                            nacheck(asInt, v->type, isNaBr);
                                        }
                                    }
                                };
                                naCheck(x, xInt, xRep);
                                naCheck(y, yInt, yRep);

                                switch (found - bitwise.begin()) {
                                case 0: {
                                    if (!isNaBr)
                                        isNaBr =
                                            BasicBlock::Create(C, "isNa", fun);
                                    auto ok = BasicBlock::Create(C, "", fun);
                                    auto ofl =
                                        builder.CreateICmpSLT(yInt, c(0));
                                    builder.CreateCondBr(ofl, isNaBr, ok,
                                                         branchMostlyFalse);
                                    builder.SetInsertPoint(ok);

                                    ok = BasicBlock::Create(C, "", fun);
                                    ofl = builder.CreateICmpSGT(yInt, c(31));
                                    builder.CreateCondBr(ofl, isNaBr, ok,
                                                         branchMostlyFalse);
                                    builder.SetInsertPoint(ok);

                                    res.addInput(builder.CreateShl(xInt, yInt));
                                    break;
                                }
                                case 1: {
                                    if (!isNaBr)
                                        isNaBr =
                                            BasicBlock::Create(C, "isNa", fun);
                                    auto ok = BasicBlock::Create(C, "", fun);
                                    auto ofl =
                                        builder.CreateICmpSLT(yInt, c(0));
                                    builder.CreateCondBr(ofl, isNaBr, ok,
                                                         branchMostlyFalse);
                                    builder.SetInsertPoint(ok);

                                    ok = BasicBlock::Create(C, "", fun);
                                    ofl = builder.CreateICmpSGT(yInt, c(31));
                                    builder.CreateCondBr(ofl, isNaBr, ok,
                                                         branchMostlyFalse);
                                    builder.SetInsertPoint(ok);

                                    res.addInput(
                                        builder.CreateLShr(xInt, yInt));
                                    break;
                                }
                                case 2: {
                                    res.addInput(builder.CreateAnd(xInt, yInt));
                                    break;
                                }
                                case 3: {
                                    res.addInput(builder.CreateOr(xInt, yInt));
                                    break;
                                }
                                case 4: {
                                    res.addInput(builder.CreateXor(xInt, yInt));
                                    break;
                                }
                                }

                                builder.CreateBr(done);

                                if (isNaBr) {
                                    builder.SetInsertPoint(isNaBr);
                                    res.addInput(c(NA_INTEGER));
                                    builder.CreateBr(done);
                                }

                                builder.SetInsertPoint(done);
                                setVal(i, res());
                                fixVisibility();
                                break;
                            }
                        }
                    }
                }

                if (b->nargs() == 1) {
                    auto a = load(b->callArg(0).val());
                    auto irep = Representation::Of(b->arg(0).val());
                    auto orep = Representation::Of(i);
                    bool done = true;

                    auto doTypetest = [&](int type) {
                        if (irep == t::SEXP) {
                            setVal(i, builder.CreateSelect(
                                          builder.CreateICmpEQ(sexptype(a),
                                                               c(type)),
                                          constant(R_TrueValue, orep),
                                          constant(R_FalseValue, orep)));

                        } else {
                            setVal(i, constant(R_FalseValue, orep));
                        }
                    };

                    switch (b->builtinId) {
                    case blt("length"):
                        if (irep == t::SEXP) {
                            llvm::Value* r = call(NativeBuiltins::length, {a});
                            if (orep == t::SEXP) {
                                r = createSelect2(
                                    builder.CreateICmpUGT(r, c(INT_MAX, 64)),
                                    [&]() {
                                        return boxReal(
                                            builder.CreateUIToFP(r, t::Double));
                                    },
                                    [&]() {
                                        return boxInt(
                                            builder.CreateTrunc(r, t::Int));
                                    });

                            } else if (orep == t::Double) {
                                r = builder.CreateUIToFP(r, t::Double);
                            } else {
                                assert(orep == Representation::Integer);
                                r = builder.CreateTrunc(r, t::Int);
                            }
                            setVal(i, r);
                        } else {
                            setVal(i, constant(ScalarInteger(1), orep));
                        }
                        break;
                    case blt("names"): {
                        auto itype = b->callArg(0).val()->type;
                        if (Representation::Of(b->callArg(0).val()) !=
                            t::SEXP) {
                            setVal(i, constant(R_NilValue, t::SEXP));
                        } else if (itype.isA(PirType::vecs()
                                                 .orObject()
                                                 .orAttribs())) {
                            if (!itype.maybeHasAttrs() && !itype.maybeObj()) {
                                setVal(i, constant(R_NilValue, t::SEXP));
                            } else {
                                auto res = phiBuilder(t::SEXP);
                                auto done = BasicBlock::Create(C, "", fun);
                                auto hasAttr = BasicBlock::Create(C, "", fun);
                                auto noAttr = BasicBlock::Create(C, "", fun);
                                auto mightHaveNames = builder.CreateICmpNE(
                                    attr(a), constant(R_NilValue, t::SEXP));
                                if (itype.maybeObj())
                                    mightHaveNames = builder.CreateOr(
                                        mightHaveNames, isObj(a));
                                builder.CreateCondBr(mightHaveNames, hasAttr,
                                                     noAttr);

                                builder.SetInsertPoint(hasAttr);
                                res.addInput(callTheBuiltin());
                                builder.CreateBr(done);

                                builder.SetInsertPoint(noAttr);
                                res.addInput(constant(R_NilValue, t::SEXP));
                                builder.CreateBr(done);

                                builder.SetInsertPoint(done);
                                setVal(i, res());
                            }
                        } else {
                            done = false;
                        }
                        break;
                    }
                    case blt("abs"): {
                        if (irep == Representation::Integer) {
                            assert(orep == irep);
                            setVal(i, builder.CreateSelect(
                                          builder.CreateICmpSGE(a, c(0)), a,
                                          builder.CreateNeg(a)));

                        } else if (irep == Representation::Real) {
                            assert(orep == irep);

                            setVal(i, builder.CreateSelect(
                                          builder.CreateFCmpOGE(a, c(0.0)), a,
                                          builder.CreateFNeg(a)));

                        } else {
                            done = false;
                        }
                        break;
                    }
                    case blt("sqrt"): {
                        if (orep == Representation::Real &&
                            irep == Representation::Integer) {
                            a = convert(a, i->type);
                            setVal(i, builder.CreateIntrinsic(
                                          Intrinsic::sqrt, {t::Double}, {a}));
                        } else if (orep == irep &&
                                   irep == Representation::Real) {
                            setVal(i, builder.CreateIntrinsic(
                                          Intrinsic::sqrt, {t::Double}, {a}));
                        } else {
                            done = false;
                        }
                        break;
                    }
                    case blt("sum"):
                    case blt("prod"): {
                        if (irep == Representation::Integer ||
                            irep == Representation::Real) {
                            setVal(i, convert(a, i->type));
                        } else if (orep == Representation::Real ||
                                   orep == Representation::Integer) {
                            assert(irep == Representation::Sexp);
                            auto itype = b->callArg(0).val()->type;
                            if (itype.isA(PirType::intReal())) {
                                auto trg = b->builtinId == blt("sum")
                                               ? NativeBuiltins::sumr
                                               : NativeBuiltins::prodr;
                                llvm::Value* res = call(trg, {a});
                                if (orep == Representation::Integer)
                                    res = convert(res, i->type);
                                setVal(i, res);
                            } else {
                                done = false;
                            }
                        } else {
                            done = false;
                        }
                        break;
                    }
                    case blt("as.logical"):
                        if (irep == Representation::Integer &&
                            orep == Representation::Integer) {
                            setVal(i,
                                   builder.CreateSelect(
                                       builder.CreateICmpEQ(a, c(NA_INTEGER)),
                                       constant(R_LogicalNAValue, orep),
                                       builder.CreateSelect(
                                           builder.CreateICmpEQ(a, c(0)),
                                           constant(R_FalseValue, orep),
                                           constant(R_TrueValue, orep))));

                        } else if (irep == Representation::Real &&
                                   (orep == Representation::Integer ||
                                    orep == Representation::Real)) {

                            setVal(i, builder.CreateSelect(
                                          builder.CreateFCmpUNE(a, a),
                                          constant(R_LogicalNAValue, orep),
                                          builder.CreateSelect(
                                              builder.CreateFCmpOEQ(a, c(0.0)),
                                              constant(R_FalseValue, orep),
                                              constant(R_TrueValue, orep))));

                        } else {
                            done = false;
                        }
                        break;
                    case blt("as.integer"):
                        if (irep == Representation::Integer &&
                            orep == Representation::Integer) {
                            setVal(i, a);
                        } else if (irep == Representation::Real &&
                                   orep == Representation::Integer) {
                            setVal(i, builder.CreateSelect(
                                          builder.CreateFCmpUNE(a, a),
                                          c(NA_INTEGER),
                                          builder.CreateFPToSI(a, t::Int)));

                        } else if (irep == Representation::Real &&
                                   orep == Representation::Real) {

                            setVal(i, createSelect2(
                                          builder.CreateFCmpUNE(a, a),
                                          [&]() { return a; },
                                          [&]() {
                                              return builder.CreateIntrinsic(
                                                  Intrinsic::floor,
                                                  {a->getType()}, {a});
                                          }));

                        } else if (irep == t::SEXP) {
                            auto isSimpleInt = builder.CreateAnd(
                                builder.CreateICmpEQ(
                                    attr(a), constant(R_NilValue, t::SEXP)),
                                builder.CreateICmpEQ(sexptype(a), c(INTSXP)));

                            setVal(i, createSelect2(
                                          isSimpleInt,
                                          [&]() { return convert(a, i->type); },
                                          [&]() {
                                              return convert(callTheBuiltin(),
                                                             i->type);
                                          }));

                        } else {
                            done = false;
                        }
                        break;
                    case blt("is.logical"):
                        if (b->arg(0).val()->type.isA(RType::logical)) {
                            // ensure that logicals represented as ints are
                            // handled.
                            setVal(i, constant(R_TrueValue, orep));
                        } else {
                            doTypetest(LGLSXP);
                        }
                        break;
                    case blt("is.complex"):
                        doTypetest(CPLXSXP);
                        break;
                    case blt("is.character"):
                        doTypetest(STRSXP);
                        break;
                    case blt("is.symbol"):
                        doTypetest(SYMSXP);
                        break;
                    case blt("is.expression"):
                        doTypetest(EXPRSXP);
                        break;
                    case blt("is.call"):
                        doTypetest(LANGSXP);
                        break;
                    case blt("is.function"): {
                        if (irep == Representation::Sexp) {
                            auto t = sexptype(a);
                            auto is = builder.CreateOr(
                                builder.CreateICmpEQ(t, c(CLOSXP)),
                                builder.CreateOr(
                                    builder.CreateICmpEQ(t, c(BUILTINSXP)),
                                    builder.CreateICmpEQ(t, c(SPECIALSXP))));
                            setVal(i, builder.CreateSelect(
                                          is, constant(R_TrueValue, orep),
                                          constant(R_FalseValue, orep)));

                        } else {
                            setVal(i, constant(R_FalseValue, orep));
                        }
                        break;
                    }
                    case blt("anyNA"):
                    case blt("is.na"):
                        if (irep == Representation::Integer) {
                            setVal(i,
                                   builder.CreateSelect(
                                       builder.CreateICmpEQ(a, c(NA_INTEGER)),
                                       constant(R_TrueValue, orep),
                                       constant(R_FalseValue, orep)));
                        } else if (irep == Representation::Real) {
                            setVal(i, builder.CreateSelect(
                                          builder.CreateFCmpUNE(a, a),
                                          constant(R_TrueValue, orep),
                                          constant(R_FalseValue, orep)));
                        } else {
                            done = false;
                        }
                        break;
                    case blt("is.object"):
                        if (irep == Representation::Sexp) {
                            setVal(i, builder.CreateSelect(
                                          isObj(a), constant(R_TrueValue, orep),
                                          constant(R_FalseValue, orep)));
                        } else {
                            setVal(i, constant(R_FalseValue, orep));
                        }
                        break;
                    case blt("is.array"):
                        if (irep == Representation::Sexp) {
                            setVal(i,
                                   builder.CreateSelect(
                                       isArray(a), constant(R_TrueValue, orep),
                                       constant(R_FalseValue, orep)));
                        } else {
                            setVal(i, constant(R_FalseValue, orep));
                        }
                        break;
                    case blt("is.atomic"):
                        if (irep == Representation::Sexp) {
                            auto t = sexptype(a);
                            auto isatomic = builder.CreateOr(
                                builder.CreateICmpEQ(t, c(NILSXP)),
                                builder.CreateOr(
                                    builder.CreateICmpEQ(t, c(CHARSXP)),
                                    builder.CreateOr(
                                        builder.CreateICmpEQ(t, c(LGLSXP)),
                                        builder.CreateOr(
                                            builder.CreateICmpEQ(t, c(INTSXP)),
                                            builder.CreateOr(
                                                builder.CreateICmpEQ(
                                                    t, c(REALSXP)),
                                                builder.CreateOr(
                                                    builder.CreateICmpEQ(
                                                        t, c(CPLXSXP)),
                                                    builder.CreateOr(
                                                        builder.CreateICmpEQ(
                                                            t, c(STRSXP)),
                                                        builder.CreateICmpEQ(
                                                            t,
                                                            c(RAWSXP)))))))));
                            setVal(i, builder.CreateSelect(
                                          isatomic, constant(R_TrueValue, orep),

                                          constant(R_FalseValue, orep)));
                        } else {
                            setVal(i, constant(R_TrueValue, orep));
                        }
                        break;
                    case blt("bodyCode"): {
                        assert(irep == Representation::Sexp && orep == irep);
                        llvm::Value* res = nullptr;
                        if (i->arg(0).val()->type.isA(RType::closure)) {
                            res = cdr(a);
                        } else {
                            res = createSelect2(
                                builder.CreateICmpEQ(c(CLOSXP), sexptype(a)),
                                [&]() { return cdr(a); },
                                [&]() {
                                    return constant(R_NilValue, t::SEXP);
                                });
                        }
                        setVal(i, res);
                        break;
                    }
                    case blt("environment"):
                        if (!i->arg(0).val()->type.isA(RType::closure)) {
                            done = false;
                            break;
                        }
                        assert(irep == Representation::Sexp && orep == irep);
                        setVal(i, tag(a));
                        break;
                    default:
                        done = false;
                    };
                    if (done) {
                        fixVisibility();
                        break;
                    }
                }

                if (b->nargs() == 2) {
                    bool fastcase = false;
                    auto arep = Representation::Of(b->arg(0).val());
                    auto brep = Representation::Of(b->arg(1).val());
                    auto orep = Representation::Of(b);
                    auto aval = load(b->arg(0).val());
                    auto bval = load(b->arg(1).val());

                    switch (b->builtinId) {
                    case blt("vector"): {
                        auto l = b->arg(1).val();
                        if (l->type.isA(PirType::simpleScalarInt())) {
                            if (auto con = LdConst::Cast(b->arg(0).val())) {
                                if (TYPEOF(con->c()) == STRSXP &&
                                    XLENGTH(con->c()) == 1) {
                                    SEXPTYPE type =
                                        str2type(CHAR(STRING_ELT(con->c(), 0)));
                                    switch (type) {
                                    case LGLSXP:
                                    case INTSXP:
                                    case REALSXP:
                                    case CPLXSXP:
                                    case STRSXP:
                                    case EXPRSXP:
                                    case VECSXP:
                                    case RAWSXP:
                                        setVal(
                                            i,
                                            call(NativeBuiltins::makeVector,
                                                 {c(type),
                                                  builder.CreateZExt(
                                                      load(l, Representation::
                                                                  Integer),
                                                      t::i64)}));
                                        fastcase = true;
                                        break;
                                    default: {}
                                    }
                                }
                            }
                        }
                        break;
                    }
                    case blt("min"):
                    case blt("max"): {
                        bool isMin = b->builtinId == blt("min");
                        if (arep == Representation::Integer &&
                            brep == Representation::Integer &&
                            orep != Representation::Real) {
                            auto res = builder.CreateSelect(
                                isMin ? builder.CreateICmpSLT(bval, aval)
                                      : builder.CreateICmpSLT(aval, bval),
                                bval, aval);
                            if (orep == Representation::Integer) {
                                setVal(i, res);
                            } else {
                                assert(orep == Representation::Sexp);
                                setVal(i, boxInt(res, false));
                            }
                            fastcase = true;
                        } else if (arep == Representation::Real &&
                                   brep == Representation::Real &&
                                   orep != Representation::Integer) {
                            auto res = builder.CreateSelect(
                                isMin ? builder.CreateFCmpUGT(bval, aval)
                                      : builder.CreateFCmpUGT(aval, bval),
                                aval, bval);
                            if (orep == Representation::Real) {
                                setVal(i, res);
                            } else {
                                assert(orep == Representation::Sexp);
                                setVal(i, boxReal(res, false));
                            }
                            fastcase = true;
                        }
                        break;
                    }
                    case blt("is.vector"):
                        if (auto cnst = LdConst::Cast(b->arg(1).val())) {
                            if (!b->arg(0).val()->type.maybeHasAttrs()) {
                                if (TYPEOF(cnst->c()) == STRSXP &&
                                    LENGTH(cnst->c()) == 1) {
                                    auto kind = STRING_ELT(cnst->c(), 0);
                                    if (std::string("any") == CHAR(kind)) {
                                        if (arep == Representation::Sexp) {
                                            setVal(i, builder.CreateSelect(
                                                          isVector(aval),
                                                          constant(R_TrueValue,
                                                                   orep),
                                                          constant(R_FalseValue,
                                                                   orep)));
                                        } else {
                                            setVal(i,
                                                   constant(R_TrueValue, orep));
                                        }
                                        fastcase = true;
                                    }
                                }
                            }
                        }
                        break;
                    }
                    if (fastcase) {
                        fixVisibility();
                        break;
                    }
                }
                if (b->builtinId == blt("c") && !b->type.maybeHasAttrs()) {
                    bool allScalar = true;
                    b->eachArg([&](Value* v) {
                        if (!v->type.isA(PirType::simpleScalar()))
                            allScalar = false;
                    });
                    if (allScalar) {
                        SEXPTYPE typ = 100;
                        if (b->type.isA(RType::real)) {
                            typ = REALSXP;
                        } else if (b->type.isA(RType::integer)) {
                            typ = INTSXP;
                        } else if (b->type.isA(RType::logical)) {
                            typ = LGLSXP;
                        }
                        if (typ != 100) {
                            auto res = call(NativeBuiltins::makeVector,
                                            {c(typ), c(b->nCallArgs(), 64)});
                            auto pos = 0;
                            b->eachCallArg([&](Value* v) {
                                assignVector(res, c(pos),
                                             convert(load(v), b->type.scalar()),
                                             b->type);
                                pos++;
                            });
                            setVal(i, res);
                            fixVisibility();
                            break;
                        }
                    }
                }

                if (b->builtinId == blt("list")) {
                    auto res = call(NativeBuiltins::makeVector,
                                    {c(VECSXP), c(b->nCallArgs(), 64)});
                    protectTemp(res);
                    auto pos = 0;
                    auto resT = PirType(RType::vec).notObject();

                    b->eachCallArg([&](Value* v) {
                        assignVector(res, c(pos), loadSxp(v), resT);
                        pos++;
                    });
                    setVal(i, res);
                    fixVisibility();
                    break;
                }

                setVal(i, callTheBuiltin());
                break;
            }

            case Tag::CallBuiltin: {
                auto b = CallBuiltin::Cast(i);
                if (compileDotcall(
                        b, [&]() { return constant(b->builtinSexp, t::SEXP); },
                        [&](size_t i) { return R_NilValue; })) {
                    break;
                }
                std::vector<Value*> args;
                b->eachCallArg([&](Value* v) { args.push_back(v); });
                setVal(i, callRBuiltin(
                              b->builtinSexp, args, i->srcIdx, b->builtin,
                              b->hasEnv() ? loadSxp(b->env())
                                          : constant(R_BaseEnv, t::SEXP)));
                break;
            }

            case Tag::Call: {
                auto b = Call::Cast(i);

                if (compileDotcall(b, [&]() { return loadSxp(b->cls()); },
                                   [&](size_t i) { return R_NilValue; })) {
                    break;
                }

                std::vector<Value*> args;
                b->eachCallArg([&](Value* v) { args.push_back(v); });
                Context asmpt = b->inferAvailableAssumptions();
                setVal(i, withCallFrame(args, [&]() -> llvm::Value* {
                           return call(NativeBuiltins::call,
                                       {paramCode(), c(b->srcIdx),
                                        loadSxp(b->cls()), loadSxp(b->env()),
                                        c(b->nCallArgs()), c(asmpt.toI())});
                       }));
                break;
            }

            case Tag::NamedCall: {
                auto b = NamedCall::Cast(i);
                if (compileDotcall(b, [&]() { return loadSxp(b->cls()); },
                                   [&](size_t i) { return b->names[i]; })) {
                    break;
                }
                std::vector<Value*> args;
                b->eachCallArg([&](Value* v) { args.push_back(v); });
                Context asmpt = b->inferAvailableAssumptions();

                std::vector<BC::PoolIdx> names;
                for (size_t i = 0; i < b->names.size(); ++i)
                    names.push_back(Pool::insert((b->names[i])));
                auto namesConst = c(names);
                auto namesStore = globalConst(namesConst);

                setVal(i, withCallFrame(args, [&]() -> llvm::Value* {
                           return call(
                               NativeBuiltins::namedCall,
                               {
                                   paramCode(),
                                   c(b->srcIdx),
                                   loadSxp(b->cls()),
                                   loadSxp(b->env()),
                                   c(b->nCallArgs()),
                                   builder.CreateBitCast(namesStore, t::IntPtr),
                                   c(asmpt.toI()),
                               });
                       }));
                break;
            }

            case Tag::StaticCall: {
                auto calli = StaticCall::Cast(i);
                calli->eachArg([](Value* v) { assert(!ExpandDots::Cast(v)); });
                auto target = calli->tryDispatch();
                auto bestTarget = calli->tryOptimisticDispatch();
                std::vector<Value*> args;
                calli->eachCallArg([&](Value* v) { args.push_back(v); });
                Context asmpt = calli->inferAvailableAssumptions();

                if (!target->owner()->hasOriginClosure()) {
                    setVal(i, withCallFrame(args, [&]() -> llvm::Value* {
                               return call(NativeBuiltins::call,
                                           {paramCode(), c(calli->srcIdx),
                                            loadSxp(calli->runtimeClosure()),
                                            loadSxp(calli->env()),
                                            c(calli->nCallArgs()),
                                            c(asmpt.toI())});
                           }));
                    break;
                }

                if (target == bestTarget) {
                    auto callee = target->owner()->rirClosure();
                    auto dt = DispatchTable::check(BODY(callee));
                    assert(cls);
                    rir::Function* nativeTarget = nullptr;
                    for (size_t i = 0; i < dt->size(); i++) {
                        auto entry = dt->get(i);
                        if (entry->context() == target->context() &&
                            entry->signature().numArguments >= args.size()) {
                            nativeTarget = entry;
                        }
                    }
                    if (nativeTarget) {
                        llvm::Value* trg = JitLLVM::get(target);
                        if (trg &&
                            target->properties.includes(
                                ClosureVersion::Property::NoReflection)) {
                            auto code = builder.CreateIntToPtr(
                                c(nativeTarget->body()), t::voidPtr);
                            llvm::Value* arglist = nodestackPtr();
                            auto rr = withCallFrame(args, [&]() {
                                return builder.CreateCall(
                                    trg, {code, arglist, loadSxp(i->env()),
                                          constant(callee, t::SEXP)});
                            });
                            setVal(i, rr);
                            break;
                        }

                        assert(
                            asmpt.includes(Assumption::StaticallyArgmatched));
                        auto idx = Pool::makeSpace();
                        Pool::patch(idx, nativeTarget->container());
                        assert(asmpt.smaller(nativeTarget->context()));
                        auto res = withCallFrame(args, [&]() {
                            return call(NativeBuiltins::nativeCallTrampoline,
                                        {
                                            constant(callee, t::SEXP),
                                            c(idx),
                                            c(calli->srcIdx),
                                            loadSxp(calli->env()),
                                            c(args.size()),
                                            c(asmpt.toI()),
                                        });
                        });
                        setVal(i, res);
                        break;
                    }
                }

                assert(asmpt.includes(Assumption::StaticallyArgmatched));
                setVal(i, withCallFrame(args, [&]() -> llvm::Value* {
                           return call(
                               NativeBuiltins::call,
                               {
                                   paramCode(),
                                   c(calli->srcIdx),
                                   builder.CreateIntToPtr(
                                       c(calli->cls()->rirClosure()), t::SEXP),
                                   loadSxp(calli->env()),
                                   c(calli->nCallArgs()),
                                   c(asmpt.toI()),
                               });
                       }));
                break;
            }

            case Tag::Inc: {
                auto arg = i->arg(0).val();
                llvm::Value* res = nullptr;
                assert(Representation::Of(arg) == Representation::Integer);
                res = load(arg, Representation::Integer);
                res = builder.CreateAdd(res, c(1), "", true, true);
                setVal(i, res);
                break;
            }

            case Tag::LdConst:
            case Tag::Nop:
                break;

            case Tag::ForSeqSize: {
                auto a = i->arg(0).val();
                if (Representation::Of(a) != t::SEXP) {
                    setVal(i, c(1));
                    break;
                }
                llvm::Value* res = call(NativeBuiltins::forSeqSize,
                                        {loadSxp(i->arg(0).val())});
                setVal(i, convert(res, i->type));
                break;
            }

            case Tag::Branch: {
                auto cond = load(i->arg(0).val(), Representation::Integer);
                cond = builder.CreateICmpNE(cond, c(0));

                auto t = bb->trueBranch();
                auto f = bb->falseBranch();
                MDNode* weight = nullptr;
                if (t->isDeopt() || (t->isJmp() && t->next()->isDeopt()))
                    weight = branchAlwaysFalse;
                else if (f->isDeopt() || (f->isJmp() && f->next()->isDeopt()))
                    weight = branchAlwaysTrue;
                builder.CreateCondBr(cond, getBlock(bb->trueBranch()),
                                     getBlock(bb->falseBranch()), weight);
                break;
            }

            case Tag::ScheduledDeopt: {
                // TODO, this is copied from pir2rir... rather ugly
                DeoptMetadata* m = nullptr;
                {
                    auto deopt = ScheduledDeopt::Cast(i);
                    size_t nframes = deopt->frames.size();
                    SEXP store =
                        Rf_allocVector(RAWSXP, sizeof(DeoptMetadata) +
                                                   nframes * sizeof(FrameInfo));
                    m = new (DATAPTR(store)) DeoptMetadata;
                    m->numFrames = nframes;

                    size_t i = 0;
                    // Frames in the ScheduledDeopt are in pir argument
                    // order (from left to right). On the other hand frames
                    // in the rir deopt_ instruction are in stack order,
                    // from tos down.
                    for (auto fi = deopt->frames.rbegin();
                         fi != deopt->frames.rend(); fi++)
                        m->frames[i++] = *fi;
                    Pool::insert(store);
                }

                std::vector<Value*> args;
                i->eachArg([&](Value* v) { args.push_back(v); });
                llvm::CallInst* res;
                withCallFrame(args, [&]() {
                    res = call(NativeBuiltins::deopt,
                               {paramCode(), paramClosure(),
                                convertToPointer(m), paramArgs()});
                    return res;
                });
                res->setTailCall(true);
                builder.CreateUnreachable();
                break;
            }

            case Tag::MkEnv: {
                auto mkenv = MkEnv::Cast(i);
                auto parent = loadSxp(mkenv->env());

                std::vector<BC::PoolIdx> names;
                for (size_t i = 0; i < mkenv->nLocals(); ++i) {
                    auto n = mkenv->varName[i];
                    if (mkenv->missing[i])
                        n = CONS_NR(n, R_NilValue);
                    names.push_back(Pool::insert(n));
                }
                auto namesConst = c(names);
                auto namesStore = globalConst(namesConst);

                if (mkenv->stub) {
                    auto env =
                        call(NativeBuiltins::createStubEnvironment,
                             {parent, c((int)mkenv->nLocals()),
                              builder.CreateBitCast(namesStore, t::IntPtr),
                              c(mkenv->context)});
                    size_t pos = 0;
                    mkenv->eachLocalVar([&](SEXP name, Value* v, bool miss) {
                        auto vn = loadSxp(v);
                        envStubSet(env, pos, vn, mkenv->nLocals(), false);
                        if (miss)
                            envStubSetMissing(env, pos);
                        pos++;
                        incrementNamed(vn);
                    });
                    setVal(i, env);
                    break;
                }

                auto arglist = constant(R_NilValue, t::SEXP);
                mkenv->eachLocalVarRev([&](SEXP name, Value* v, bool miss) {
                    if (miss) {
                        arglist = call(
                            NativeBuiltins::createMissingBindingCell,
                            {loadSxp(v), constant(name, t::SEXP), arglist});
                    } else {
                        arglist = call(
                            NativeBuiltins::createBindingCell,
                            {loadSxp(v), constant(name, t::SEXP), arglist});
                    }
                });

                setVal(i, call(NativeBuiltins::createEnvironment,
                               {parent, arglist, c(mkenv->context)}));

                if (bindingsCache.count(i))
                    for (auto b : bindingsCache.at(i))
                        builder.CreateStore(
                            convertToPointer(nullptr, t::SEXP),
                            builder.CreateGEP(bindingsCacheBase, c(b.second)));
                break;
            }

            case Tag::MaterializeEnv: {
                auto materialize = MaterializeEnv::Cast(i);
                setVal(i, call(NativeBuiltins::materializeEnvironment,
                               {loadSxp(materialize->env())}));
                break;
            }

            case Tag::Add:
                compileBinop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 // TODO: Check NA
                                 return builder.CreateAdd(a, b, "", false,
                                                          true);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFAdd(a, b);
                             },
                             BinopKind::ADD);
                break;
            case Tag::Sub:
                compileBinop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 // TODO: Check NA
                                 return builder.CreateSub(a, b, "", false,
                                                          true);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFSub(a, b);
                             },
                             BinopKind::SUB);
                break;
            case Tag::Mul:
                compileBinop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 // TODO: Check NA
                                 return builder.CreateMul(a, b, "", false,
                                                          true);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFMul(a, b);
                             },
                             BinopKind::MUL);
                break;
            case Tag::Div:
                compileBinop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 // TODO: Check NA
                                 return builder.CreateSDiv(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFDiv(a, b);
                             },
                             BinopKind::DIV);
                break;
            case Tag::Pow:
                compileBinop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 // TODO: Check NA?
                                 return builder.CreateIntrinsic(
                                     Intrinsic::powi,
                                     {a->getType(), b->getType()}, {a, b});
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateIntrinsic(
                                     Intrinsic::pow,
                                     {a->getType(), b->getType()}, {a, b});
                             },
                             BinopKind::POW);
                break;

            case Tag::Neq:
                compileRelop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateICmpNE(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFCmpUNE(a, b);
                             },
                             BinopKind::NE);
                break;

            case Tag::Minus: {
                compileUnop(
                    i, [&](llvm::Value* a) { return builder.CreateNeg(a); },
                    [&](llvm::Value* a) { return builder.CreateFNeg(a); },
                    UnopKind::MINUS);
                break;
            }

            case Tag::Plus: {
                compileUnop(i, [&](llvm::Value* a) { return a; },
                            [&](llvm::Value* a) { return a; }, UnopKind::PLUS);
                break;
            }

            case Tag::Not: {
                auto resultRep = Representation::Of(i);
                auto argument = i->arg(0).val();
                auto argumentRep = Representation::Of(argument);
                if (argumentRep == Representation::Sexp) {
                    auto argumentNative = loadSxp(argument);

                    llvm::Value* res = nullptr;
                    if (i->hasEnv()) {
                        res = call(
                            NativeBuiltins::notEnv,
                            {argumentNative, loadSxp(i->env()), c(i->srcIdx)});
                    } else {
                        res = call(NativeBuiltins::notOp, {argumentNative});
                    }
                    setVal(i, res);
                    break;
                }

                auto done = BasicBlock::Create(C, "", fun);
                auto isNa = BasicBlock::Create(C, "", fun);

                auto argumentNative = load(argument, argumentRep);

                nacheck(argumentNative, argument->type, isNa);

                auto res = phiBuilder(t::Int);

                res.addInput(builder.CreateZExt(
                    builder.CreateICmpEQ(argumentNative, c(0)), t::Int));
                builder.CreateBr(done);

                builder.SetInsertPoint(isNa);
                // Maybe we need to model R_LogicalNAValue?
                res.addInput(c(NA_INTEGER));
                builder.CreateBr(done);
                builder.SetInsertPoint(done);

                if (resultRep == Representation::Sexp) {
                    setVal(i, boxLgl(res(), true));
                } else {
                    setVal(i, res());
                }
                break;
            }

            case Tag::Eq:
                compileRelop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateICmpEQ(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFCmpUEQ(a, b);
                             },
                             BinopKind::EQ);
                break;

            case Tag::Lte:
                compileRelop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateICmpSLE(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFCmpULE(a, b);
                             },
                             BinopKind::LTE);
                break;
            case Tag::Lt:
                compileRelop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateICmpSLT(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFCmpULT(a, b);
                             },
                             BinopKind::LT);
                break;
            case Tag::Gte:
                compileRelop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateICmpSGE(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFCmpUGE(a, b);
                             },
                             BinopKind::GTE);
                break;
            case Tag::Gt:
                compileRelop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateICmpSGT(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateFCmpUGT(a, b);
                             },
                             BinopKind::GT);
                break;
            case Tag::LAnd:
                compileRelop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 a = builder.CreateZExt(
                                     builder.CreateICmpNE(a, c(0)), t::Int);
                                 b = builder.CreateZExt(
                                     builder.CreateICmpNE(b, c(0)), t::Int);
                                 return builder.CreateAnd(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 a = builder.CreateZExt(
                                     builder.CreateFCmpUNE(a, c(0.0)), t::Int);
                                 b = builder.CreateZExt(
                                     builder.CreateFCmpUNE(b, c(0.0)), t::Int);
                                 return builder.CreateAnd(a, b);
                             },
                             BinopKind::LAND);
                break;
            case Tag::LOr:
                compileRelop(i,
                             [&](llvm::Value* a, llvm::Value* b) {
                                 return builder.CreateOr(a, b);
                             },
                             [&](llvm::Value* a, llvm::Value* b) {
                                 a = builder.CreateZExt(
                                     builder.CreateFCmpUNE(a, c(0.0)), t::Int);
                                 b = builder.CreateZExt(
                                     builder.CreateFCmpUNE(b, c(0.0)), t::Int);
                                 return builder.CreateOr(a, b);
                             },
                             BinopKind::LOR);
                break;
            case Tag::IDiv:
                compileBinop(
                    i,
                    [&](llvm::Value* a, llvm::Value* b) {
                        auto isZero = BasicBlock::Create(C, "", fun);
                        auto notZero = BasicBlock::Create(C, "", fun);
                        auto cnt = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(builder.CreateICmpEQ(b, c(0)),
                                             isZero, notZero,
                                             branchMostlyFalse);

                        auto res = phiBuilder(t::Int);

                        builder.SetInsertPoint(isZero);
                        res.addInput(c(NA_INTEGER));
                        builder.CreateBr(cnt);

                        builder.SetInsertPoint(notZero);
                        auto r = builder.CreateFDiv(
                            builder.CreateSIToFP(a, t::Double),
                            builder.CreateSIToFP(b, t::Double));
                        res.addInput(builder.CreateFPToSI(r, t::Int));
                        builder.CreateBr(cnt);

                        builder.SetInsertPoint(cnt);
                        return res();
                    },
                    [&](llvm::Value* a, llvm::Value* b) {
                        // from myfloor
                        auto q = builder.CreateFDiv(a, b);
                        auto isZero = BasicBlock::Create(C, "", fun);
                        auto notZero = BasicBlock::Create(C, "", fun);
                        auto cnt = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(builder.CreateFCmpUEQ(b, c(0.0)),
                                             isZero, notZero,
                                             branchMostlyFalse);

                        auto res = phiBuilder(t::Double);

                        builder.SetInsertPoint(isZero);
                        res.addInput(q);
                        builder.CreateBr(cnt);

                        builder.SetInsertPoint(notZero);
                        auto fq = builder.CreateIntrinsic(Intrinsic::floor,
                                                          {t::Double}, {q});
                        auto tmp =
                            builder.CreateFSub(a, builder.CreateFMul(fq, b));
                        auto frem = builder.CreateIntrinsic(
                            Intrinsic::floor, {t::Double},
                            {builder.CreateFDiv(tmp, b)});
                        res.addInput(builder.CreateFAdd(fq, frem));
                        builder.CreateBr(cnt);

                        builder.SetInsertPoint(cnt);
                        return res();
                    },
                    BinopKind::IDIV);
                break;
            case Tag::Mod: {
                auto myfmod = [&](llvm::Value* a, llvm::Value* b) {
                    // from myfmod
                    auto isZero = BasicBlock::Create(C, "", fun);
                    auto notZero = BasicBlock::Create(C, "", fun);
                    auto cnt = BasicBlock::Create(C, "", fun);
                    auto res = phiBuilder(t::Double);
                    builder.CreateCondBr(builder.CreateFCmpUEQ(b, c(0.0)),
                                         isZero, notZero, branchMostlyFalse);

                    builder.SetInsertPoint(isZero);
                    res.addInput(c(R_NaN));
                    builder.CreateBr(cnt);

                    builder.SetInsertPoint(notZero);
                    auto q = builder.CreateFDiv(a, b);
                    auto fq = builder.CreateIntrinsic(Intrinsic::floor,
                                                      {t::Double}, {q});

                    auto absq = builder.CreateIntrinsic(Intrinsic::fabs,
                                                        {t::Double}, {q});
                    auto finite = builder.CreateFCmpUNE(
                        absq, c((double)0x7FF0000000000000));
                    auto gt =
                        builder.CreateFCmpUGT(absq, c(1 / R_AccuracyInfo.eps));

                    auto warn = BasicBlock::Create(C, "", fun);
                    auto noWarn = BasicBlock::Create(C, "", fun);
                    builder.CreateCondBr(builder.CreateAnd(finite, gt), warn,
                                         noWarn, branchMostlyFalse);

                    builder.SetInsertPoint(warn);
                    auto msg = builder.CreateGlobalString(
                        "probable complete loss of accuracy in modulus");
                    call(NativeBuiltins::warn,
                         {builder.CreateInBoundsGEP(msg, {c(0), c(0)})});
                    builder.CreateBr(noWarn);

                    builder.SetInsertPoint(noWarn);
                    auto tmp = builder.CreateFSub(a, builder.CreateFMul(fq, b));
                    auto frem =
                        builder.CreateIntrinsic(Intrinsic::floor, {t::Double},
                                                {builder.CreateFDiv(tmp, b)});
                    res.addInput(
                        builder.CreateFSub(tmp, builder.CreateFMul(frem, b)));
                    builder.CreateBr(cnt);

                    builder.SetInsertPoint(cnt);
                    return res();
                };

                compileBinop(
                    i,
                    [&](llvm::Value* a, llvm::Value* b) {
                        auto fast = BasicBlock::Create(C, "", fun);
                        auto fast1 = BasicBlock::Create(C, "", fun);
                        auto slow = BasicBlock::Create(C, "", fun);
                        auto cnt = BasicBlock::Create(C, "", fun);
                        auto res = phiBuilder(t::Int);
                        builder.CreateCondBr(builder.CreateICmpSGE(a, c(0)),
                                             fast1, slow, branchMostlyTrue);

                        builder.SetInsertPoint(fast1);
                        builder.CreateCondBr(builder.CreateICmpSGT(b, c(0)),
                                             fast, slow, branchMostlyTrue);

                        builder.SetInsertPoint(fast);
                        res.addInput(builder.CreateSRem(a, b));
                        builder.CreateBr(cnt);

                        builder.SetInsertPoint(slow);
                        res.addInput(builder.CreateFPToSI(
                            myfmod(builder.CreateSIToFP(a, t::Double),
                                   builder.CreateSIToFP(b, t::Double)),
                            t::Int));
                        builder.CreateBr(cnt);

                        builder.SetInsertPoint(cnt);
                        return res();
                    },
                    myfmod, BinopKind::MOD);
                break;
            }
            case Tag::Colon: {
                assert(Representation::Of(i) == t::SEXP);
                auto a = i->arg(0).val();
                auto b = i->arg(1).val();
                llvm::Value* res;
                if (i->hasEnv()) {
                    auto e = loadSxp(i->env());
                    res = call(NativeBuiltins::binopEnv,
                               {loadSxp(a), loadSxp(b), e, c(i->srcIdx),
                                c((int)BinopKind::COLON)});
                } else if (Representation::Of(a) == Representation::Integer &&
                           Representation::Of(b) == Representation::Integer) {
                    res = call(NativeBuiltins::colon, {load(a), load(b)});
                } else {
                    res =
                        call(NativeBuiltins::binop, {loadSxp(a), loadSxp(b),
                                                     c((int)BinopKind::COLON)});
                }
                setVal(i, res);
                break;
            }

            case Tag::Return: {
                auto res = loadSxp(Return::Cast(i)->arg<0>().val());
                if (numLocals > 0)
                    decStack(numLocals);
                builder.CreateRet(res);
                break;
            }

            case Tag::NonLocalReturn: {
                call(NativeBuiltins::nonLocalReturn,
                     {loadSxp(i->arg(0).val()), loadSxp(i->env())});
                builder.CreateUnreachable();
                break;
            }

            case Tag::IsEnvStub: {
                auto arg = loadSxp(i->arg(0).val());
                auto env = MkEnv::Cast(i->env());

                auto isStub = BasicBlock::Create(C, "", fun);
                auto isNotMaterialized = BasicBlock::Create(C, "", fun);
                auto isNotStub = BasicBlock::Create(C, "", fun);
                auto done = BasicBlock::Create(C, "", fun);

                auto r = Representation::Of(i);
                auto res = phiBuilder(r);

                builder.CreateCondBr(isExternalsxp(arg, LAZY_ENVIRONMENT_MAGIC),
                                     isStub, isNotStub, branchAlwaysTrue);

                builder.SetInsertPoint(isStub);
                auto materialized = envStubGet(arg, -2, env->nLocals());
                builder.CreateCondBr(
                    builder.CreateICmpEQ(materialized,
                                         convertToPointer(nullptr, t::SEXP)),
                    isNotMaterialized, isNotStub, branchAlwaysTrue);

                builder.SetInsertPoint(isNotMaterialized);
                res.addInput(constant(R_TrueValue, r));
                builder.CreateBr(done);

                builder.SetInsertPoint(isNotStub);
                res.addInput(constant(R_FalseValue, r));
                builder.CreateBr(done);

                builder.SetInsertPoint(done);

                setVal(i, res());
                break;
            }

            case Tag::MkFunCls: {
                auto mkFunction = MkFunCls::Cast(i);
                auto closure = mkFunction->cls;
                auto srcRef = constant(closure->srcRef(), t::SEXP);
                auto formals = constant(closure->formals().original(), t::SEXP);
                auto body =
                    constant(mkFunction->originalBody->container(), t::SEXP);
                assert(DispatchTable::check(
                    mkFunction->originalBody->container()));
                setVal(i, call(NativeBuiltins::createClosure,
                               {body, formals, loadSxp(mkFunction->env()),
                                srcRef}));
                break;
            }

            case Tag::MkCls: {
                auto mk = MkCls::Cast(i);
                auto formals = loadSxp(mk->arg(0).val());
                auto body = loadSxp(mk->arg(1).val());
                auto srcRef = loadSxp(mk->arg(2).val());
                auto env = loadSxp(mk->arg(3).val());
                setVal(i, call(NativeBuiltins::createClosure,
                               {body, formals, env, srcRef}));
                break;
            }

            case Tag::IsType: {
                assert(Representation::Of(i) == Representation::Integer);

                auto t = IsType::Cast(i);
                auto arg = i->arg(0).val();
                if (Representation::Of(arg) == Representation::Sexp) {
                    auto a = loadSxp(arg);
                    if (t->typeTest.maybePromiseWrapped())
                        a = depromise(a, arg->type);

                    if (t->typeTest.notPromiseWrapped() ==
                        PirType::simpleScalarInt()) {
                        setVal(i, builder.CreateZExt(isSimpleScalar(a, INTSXP),
                                                     t::Int));
                        break;
                    } else if (t->typeTest.notPromiseWrapped() ==
                               PirType::simpleScalarLogical()) {
                        setVal(i, builder.CreateZExt(isSimpleScalar(a, LGLSXP),
                                                     t::Int));
                        break;
                    } else if (t->typeTest.notPromiseWrapped() ==
                               PirType::simpleScalarReal()) {
                        setVal(i, builder.CreateZExt(isSimpleScalar(a, REALSXP),
                                                     t::Int));
                        break;
                    }

                    llvm::Value* res = nullptr;
                    if (t->typeTest.noAttribs().isA(
                            PirType(RType::logical).orPromiseWrapped())) {
                        res = builder.CreateICmpEQ(sexptype(a), c(LGLSXP));
                    } else if (t->typeTest.noAttribs().isA(
                                   PirType(RType::integer)
                                       .orPromiseWrapped())) {
                        res = builder.CreateICmpEQ(sexptype(a), c(INTSXP));
                    } else if (t->typeTest.noAttribs().isA(
                                   PirType(RType::real).orPromiseWrapped())) {
                        res = builder.CreateICmpEQ(sexptype(a), c(REALSXP));
                    } else {
                        assert(arg->type.notMissing()
                                   .notPromiseWrapped()
                                   .noAttribs()
                                   .isA(t->typeTest));
                        res = builder.CreateICmpNE(
                            a, constant(R_UnboundValue, t::SEXP));
                    }
                    if (t->typeTest.isScalar() && !arg->type.isScalar()) {
                        assert(a->getType() == t::SEXP);
                        res = builder.CreateAnd(res, isScalar(a));
                    }
                    if (arg->type.maybeHasAttrs() &&
                        !t->typeTest.maybeHasAttrs()) {
                        res = builder.CreateAnd(res, fastVeceltOkNative(a));
                    }
                    if (arg->type.maybeObj() && !t->typeTest.maybeObj()) {
                        res =
                            builder.CreateAnd(res, builder.CreateNot(isObj(a)));
                    }
                    setVal(i, builder.CreateZExt(res, t::Int));
                } else {
                    setVal(i, c(1));
                }
                break;
            }

            case Tag::Is: {
                assert(Representation::Of(i) == Representation::Integer);
                auto is = Is::Cast(i);
                auto arg = i->arg(0).val();
                llvm::Value* res;
                if (Representation::Of(arg) == Representation::Sexp) {
                    auto argNative = loadSxp(arg);
                    auto expectedTypeNative = c(is->sexpTag);
                    auto typeNative = sexptype(argNative);
                    switch (is->sexpTag) {
                    case NILSXP:
                    case LGLSXP:
                    case REALSXP:
                        res = builder.CreateICmpEQ(typeNative,
                                                   expectedTypeNative);
                        break;

                    case VECSXP: {
                        auto operandLhs =
                            builder.CreateICmpEQ(typeNative, c(VECSXP));
                        auto operandRhs =
                            builder.CreateICmpEQ(typeNative, c(LISTSXP));
                        res = builder.CreateOr(operandLhs, operandRhs);
                        break;
                    }

                    case LISTSXP: {
                        auto operandLhs =
                            builder.CreateICmpEQ(typeNative, c(LISTSXP));
                        auto operandRhs =
                            builder.CreateICmpEQ(typeNative, c(NILSXP));
                        res = builder.CreateOr(operandLhs, operandRhs);
                        break;
                    }

                    default:
                        assert(false);
                        res = builder.getFalse();
                        break;
                    }
                } else {
                    assert(i->type.isA(RType::integer) ||
                           i->type.isA(RType::logical) ||
                           i->type.isA(RType::real));
                    assert(Representation::Of(i) == Representation::Integer ||
                           Representation::Of(i) == Representation::Real);

                    bool matchInt =
                        (is->sexpTag == INTSXP) && i->type.isA(RType::integer);
                    bool matchLgl =
                        (is->sexpTag == LGLSXP) && i->type.isA(RType::logical);
                    bool matchReal =
                        (is->sexpTag == REALSXP) && i->type.isA(RType::real);

                    res = (matchInt || matchLgl || matchReal)
                              ? builder.getTrue()
                              : builder.getFalse();
                }
                setVal(i, builder.CreateZExt(res, t::Int));
                break;
            }

            case Tag::AsTest: {
                assert(Representation::Of(i) == Representation::Integer);

                auto arg = i->arg(0).val();

                if (Representation::Of(arg) == Representation::Sexp) {
                    auto a = loadSxp(arg);
                    setVal(i, call(NativeBuiltins::asTest, {a}));
                    break;
                }

                auto r = Representation::Of(arg);

                auto done = BasicBlock::Create(C, "", fun);
                auto isNa = BasicBlock::Create(C, "asTestIsNa", fun);

                if (r == Representation::Real) {
                    auto narg = load(arg, r);
                    auto isNotNa = builder.CreateFCmpUEQ(narg, narg);
                    narg = builder.CreateFPToSI(narg, t::Int);
                    setVal(i, narg);
                    builder.CreateCondBr(isNotNa, done, isNa, branchMostlyTrue);
                } else {
                    auto narg = load(arg, Representation::Integer);
                    auto isNotNa = builder.CreateICmpNE(narg, c(NA_INTEGER));
                    setVal(i, narg);
                    builder.CreateCondBr(isNotNa, done, isNa, branchMostlyTrue);
                }

                builder.SetInsertPoint(isNa);
                auto msg = builder.CreateGlobalString(
                    "missing value where TRUE/FALSE needed");
                call(NativeBuiltins::error,
                     {builder.CreateInBoundsGEP(msg, {c(0), c(0)})});
                builder.CreateRet(builder.CreateIntToPtr(c(nullptr), t::SEXP));

                builder.SetInsertPoint(done);
                break;
            }

            case Tag::AsLogical: {
                auto arg = i->arg(0).val();

                auto r1 = Representation::Of(arg);
                auto r2 = Representation::Of(i);

                assert(r2 == Representation::Integer);

                llvm::Value* res;
                if (r1 == Representation::Sexp) {
                    res = call(NativeBuiltins::asLogicalBlt, {loadSxp(arg)});
                } else if (r1 == Representation::Real) {
                    auto phi = phiBuilder(t::Int);
                    auto nin = load(arg, Representation::Real);

                    auto done = BasicBlock::Create(C, "", fun);
                    auto isNaBr = BasicBlock::Create(C, "isNa", fun);
                    auto notNaBr = BasicBlock::Create(C, "", fun);
                    nacheck(nin, arg->type, isNaBr, notNaBr);

                    builder.SetInsertPoint(isNaBr);
                    phi.addInput(c(NA_INTEGER));
                    builder.CreateBr(done);

                    builder.SetInsertPoint(notNaBr);
                    auto cnv =
                        builder.CreateSelect(builder.CreateFCmpOEQ(c(0.0), nin),
                                             constant(R_FalseValue, t::Int),
                                             constant(R_TrueValue, t::Int));
                    phi.addInput(cnv);
                    builder.CreateBr(done);

                    builder.SetInsertPoint(done);
                    res = phi();
                } else {
                    assert(r1 == Representation::Integer);
                    res = load(arg, Representation::Integer);
                    if (!arg->type.isA(RType::logical)) {
                        res = builder.CreateSelect(
                            builder.CreateICmpEQ(res, c(NA_INTEGER)),
                            c(NA_LOGICAL),
                            builder.CreateSelect(
                                builder.CreateICmpEQ(res, c(0)),
                                constant(R_FalseValue, t::Int),
                                constant(R_TrueValue, t::Int)));
                    }
                }

                setVal(i, res);
                break;
            }

            case Tag::Force: {
                auto f = Force::Cast(i);
                auto arg = f->arg<0>().val();
                if (!f->effects.includes(Effect::Force)) {
                    if (!arg->type.maybePromiseWrapped()) {
                        setVal(i, load(arg, Representation::Of(i)));
                    } else {
                        auto res = depromise(arg);
                        setVal(i, res);
#ifdef ENABLE_SLOWASSERT
                        insn_assert(builder.CreateICmpNE(
                                        constant(R_UnboundValue, t::SEXP), res),
                                    "Expected evaluated promise");
#endif
                    }
                } else {
                    setVal(i, force(i, loadSxp(arg)));
                }
                break;
            }

            case Tag::LdFun: {
                auto ld = LdFun::Cast(i);
                auto res =
                    call(NativeBuiltins::ldfun,
                         {constant(ld->varName, t::SEXP), loadSxp(ld->env())});
                setVal(i, res);
                setVisible(1);
                break;
            }

            case Tag::MkArg: {
                auto p = MkArg::Cast(i);
                auto id = promMap.at(p->prom());
                auto exp = loadPromise(paramCode(), id.first);
                // if the env of a promise is elided we need to put a dummy env,
                // to forcePromise complaining.
                if (p->hasEnv()) {
                    auto e = loadSxp(p->env());
                    if (p->isEager()) {
                        setVal(i, call(NativeBuiltins::createPromiseEager,
                                       {exp, e, loadSxp(p->eagerArg())}));
                    } else {
                        setVal(i,
                               call(NativeBuiltins::createPromise, {exp, e}));
                    }
                } else {
                    if (p->isEager()) {
                        setVal(i, call(NativeBuiltins::createPromiseNoEnvEager,
                                       {exp, loadSxp(p->eagerArg())}));
                    } else {
                        setVal(i,
                               call(NativeBuiltins::createPromiseNoEnv, {exp}));
                    }
                }
                break;
            }

            case Tag::UpdatePromise: {
                auto val = loadSxp(i->arg(1).val());
                ensureShared(val);
                setCar(loadSxp(i->arg(0).val()), val);
                break;
            }

            case Tag::LdVarSuper: {
                auto ld = LdVarSuper::Cast(i);

                auto env = cdr(loadSxp(ld->env()));

                auto res = call(NativeBuiltins::ldvar,
                                {constant(ld->varName, t::SEXP), env});
                res->setName(CHAR(PRINTNAME(ld->varName)));

                checkMissing(res);
                checkUnbound(res);
                setVal(i, res);
                break;
            }

            case Tag::LdDots:
            case Tag::LdVar: {
                auto maybeLd = LdVar::Cast(i);
                auto varName = maybeLd ? maybeLd->varName : R_DotsSymbol;

                auto env = MkEnv::Cast(i->env());
                if (LdFunctionEnv::Cast(i->env()))
                    env = myPromenv;

                if (env && env->stub) {
                    auto e = loadSxp(env);
                    llvm::Value* res =
                        envStubGet(e, env->indexOf(varName), env->nLocals());
                    if (env->argNamed(varName).val() ==
                        UnboundValue::instance()) {

                        res = createSelect2(
                            builder.CreateICmpEQ(
                                res, constant(R_UnboundValue, t::SEXP)),
                            // if unsassigned in the stub, fall through
                            [&]() {
                                return call(
                                    NativeBuiltins::ldvar,
                                    {constant(varName, t::SEXP),
                                     envStubGet(e, -1, env->nLocals())});
                            },
                            [&]() { return res; });
                    }
                    setVal(i, res);
                    break;
                }

                llvm::Value* res;
                if (bindingsCache.count(i->env())) {
                    auto phi = phiBuilder(t::SEXP);
                    auto offset = bindingsCache.at(i->env()).at(varName);

                    auto cachePtr =
                        builder.CreateGEP(bindingsCacheBase, c(offset));
                    llvm::Value* cache = builder.CreateLoad(cachePtr);

                    auto hit1 = BasicBlock::Create(C, "", fun);
                    auto hit2 = BasicBlock::Create(C, "", fun);
                    auto miss = BasicBlock::Create(C, "", fun);
                    auto done = BasicBlock::Create(C, "", fun);

                    builder.CreateCondBr(
                        builder.CreateICmpULE(
                            builder.CreatePtrToInt(cache, t::i64),
                            c(NativeBuiltins::bindingsCacheFails)),
                        miss, hit1, branchMostlyFalse);
                    builder.SetInsertPoint(hit1);
                    auto val = car(cache);
                    builder.CreateCondBr(
                        builder.CreateICmpEQ(val,
                                             constant(R_UnboundValue, t::SEXP)),
                        miss, hit2, branchMostlyFalse);
                    builder.SetInsertPoint(hit2);
                    ensureNamed(val);
                    phi.addInput(val);
                    builder.CreateBr(done);

                    builder.SetInsertPoint(miss);
                    auto res0 = call(NativeBuiltins::ldvarCacheMiss,
                                     {constant(varName, t::SEXP),
                                      loadSxp(i->env()), cachePtr});
                    if (needsLdVarForUpdate.count(i))
                        ensureShared(res0);
                    phi.addInput(res0);
                    builder.CreateBr(done);
                    builder.SetInsertPoint(done);
                    res = phi();
                } else {
                    auto setter = needsLdVarForUpdate.count(i)
                                      ? NativeBuiltins::ldvarForUpdate
                                      : NativeBuiltins::ldvar;
                    res = call(setter,
                               {constant(varName, t::SEXP), loadSxp(i->env())});
                }
                res->setName(CHAR(PRINTNAME(varName)));

                if (maybeLd) {
                    checkMissing(res);
                    checkUnbound(res);
                }
                setVal(i, res);
                break;
            }

            case Tag::Extract1_1D: {
                auto extract = Extract1_1D::Cast(i);
                auto vector = loadSxp(extract->vec());

                bool fastcase = !extract->vec()->type.maybe(RType::vec) &&
                                !extract->vec()->type.maybeObj() &&
                                vectorTypeSupport(extract->vec()) &&
                                extract->idx()->type.isA(
                                    PirType::intReal().notObject().scalar());
                BasicBlock* done;
                auto res = phiBuilder(Representation::Of(i));

                if (fastcase) {
                    auto fallback = BasicBlock::Create(C, "", fun);
                    done = BasicBlock::Create(C, "", fun);

                    llvm::Value* vector = load(extract->vec());

                    if (Representation::Of(extract->vec()) == t::SEXP) {
                        auto hit2 = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(isAltrep(vector), fallback, hit2,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit2);

                        if (extract->vec()->type.maybeHasAttrs()) {
                            auto hit3 = BasicBlock::Create(C, "", fun);
                            builder.CreateCondBr(fastVeceltOkNative(vector),
                                                 hit3, fallback,
                                                 branchMostlyTrue);
                            builder.SetInsertPoint(hit3);
                        }
                    }

                    llvm::Value* index =
                        computeAndCheckIndex(extract->idx(), vector, fallback);
                    auto res0 =
                        extract->vec()->type.isScalar()
                            ? vector
                            : accessVector(vector, index, extract->vec()->type);
                    res.addInput(convert(res0, i->type));
                    builder.CreateBr(done);

                    builder.SetInsertPoint(fallback);
                }

                auto env = constant(R_NilValue, t::SEXP);
                if (extract->hasEnv())
                    env = loadSxp(extract->env());
                auto idx = loadSxp(extract->idx());
                auto res0 = call(NativeBuiltins::extract11,
                                 {vector, idx, env, c(extract->srcIdx)});

                res.addInput(convert(res0, i->type));
                if (fastcase) {
                    builder.CreateBr(done);

                    builder.SetInsertPoint(done);
                }

                setVal(i, res());
                break;
            }

            case Tag::Extract1_2D: {
                auto extract = Extract1_2D::Cast(i);

                bool fastcase = !extract->vec()->type.maybe(RType::vec) &&
                                !extract->vec()->type.maybeObj() &&
                                vectorTypeSupport(extract->vec()) &&
                                extract->idx1()->type.isA(
                                    PirType::intReal().notObject().scalar()) &&
                                extract->idx2()->type.isA(
                                    PirType::intReal().notObject().scalar());

                BasicBlock* done;
                auto res = phiBuilder(Representation::Of(i));

                if (fastcase) {
                    auto fallback = BasicBlock::Create(C, "", fun);
                    done = BasicBlock::Create(C, "", fun);

                    llvm::Value* vector = load(extract->vec());

                    if (Representation::Of(extract->vec()) == t::SEXP) {
                        auto hit2 = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(isAltrep(vector), fallback, hit2,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit2);

                        if (extract->vec()->type.maybeHasAttrs()) {
                            auto hit3 = BasicBlock::Create(C, "", fun);
                            builder.CreateCondBr(fastVeceltOkNative(vector),
                                                 hit3, fallback,
                                                 branchMostlyTrue);
                            builder.SetInsertPoint(hit3);
                        }
                    }

                    auto ncol = builder.CreateZExt(
                        call(NativeBuiltins::matrixNcols, {vector}), t::i64);
                    auto nrow = builder.CreateZExt(
                        call(NativeBuiltins::matrixNrows, {vector}), t::i64);
                    llvm::Value* index1 = computeAndCheckIndex(
                        extract->idx1(), vector, fallback, nrow);
                    llvm::Value* index2 = computeAndCheckIndex(
                        extract->idx2(), vector, fallback, ncol);

                    llvm::Value* index =
                        builder.CreateMul(nrow, index2, "", true, true);
                    index = builder.CreateAdd(index, index1, "", true, true);

                    auto res0 =
                        extract->vec()->type.isScalar()
                            ? vector
                            : accessVector(vector, index, extract->vec()->type);

                    res.addInput(convert(res0, i->type));
                    builder.CreateBr(done);

                    builder.SetInsertPoint(fallback);
                }

                auto vector = loadSxp(extract->vec());
                auto idx1 = loadSxp(extract->idx1());
                auto idx2 = loadSxp(extract->idx2());
                auto res0 = call(NativeBuiltins::extract12,
                                 {vector, idx1, idx2, loadSxp(extract->env()),
                                  c(extract->srcIdx)});

                res.addInput(convert(res0, i->type));
                if (fastcase) {
                    builder.CreateBr(done);

                    builder.SetInsertPoint(done);
                }
                setVal(i, res());
                break;
            }

            case Tag::Extract2_1D: {
                auto extract = Extract2_1D::Cast(i);
                // TODO: Extend a fastPath for generic vectors.
                bool fastcase = vectorTypeSupport(extract->vec()) &&
                                extract->idx()->type.isA(
                                    PirType::intReal().notObject().scalar());

                BasicBlock* done;
                auto res = phiBuilder(Representation::Of(i));

                if (fastcase) {
                    auto fallback = BasicBlock::Create(C, "", fun);
                    auto hit2 = BasicBlock::Create(C, "", fun);
                    done = BasicBlock::Create(C, "", fun);

                    llvm::Value* vector = load(extract->vec());

                    if (Representation::Of(extract->vec()) == t::SEXP) {
                        builder.CreateCondBr(isAltrep(vector), fallback, hit2,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit2);
                    }

                    llvm::Value* index =
                        computeAndCheckIndex(extract->idx(), vector, fallback);
                    auto res0 =
                        extract->vec()->type.isScalar()
                            ? vector
                            : accessVector(vector, index, extract->vec()->type);
                    res.addInput(convert(res0, i->type));
                    builder.CreateBr(done);

                    builder.SetInsertPoint(fallback);
                }

                auto irep = Representation::Of(extract->idx());
                llvm::Value* res0;

                if (irep != t::SEXP) {
                    NativeBuiltin getter;
                    if (irep == t::Int) {
                        getter = NativeBuiltins::extract21i;
                    } else {
                        assert(irep == t::Double);
                        getter = NativeBuiltins::extract21r;
                    }
                    auto vector = loadSxp(extract->vec());
                    res0 = call(getter,
                                {vector, load(extract->idx()),
                                 loadSxp(extract->env()), c(extract->srcIdx)});
                } else {
                    auto vector = loadSxp(extract->vec());
                    auto idx = loadSxp(extract->idx());
                    res0 = call(NativeBuiltins::extract21,
                                {vector, idx, loadSxp(extract->env()),
                                 c(extract->srcIdx)});
                }

                res.addInput(convert(res0, i->type));
                if (fastcase) {
                    builder.CreateBr(done);

                    builder.SetInsertPoint(done);
                }
                setVal(i, res());
                break;
            }

            case Tag::Extract1_3D: {
                auto extract = Extract1_3D::Cast(i);
                auto vector = loadSxp(extract->vec());
                auto idx1 = loadSxp(extract->idx1());
                auto idx2 = loadSxp(extract->idx2());
                auto idx3 = loadSxp(extract->idx3());

                // We should implement the fast cases (known and primitive
                // types) speculatively here
                auto env = constant(R_NilValue, t::SEXP);
                if (extract->hasEnv())
                    env = loadSxp(extract->env());

                auto res =
                    call(NativeBuiltins::extract13,
                         {vector, idx1, idx2, idx3, env, c(extract->srcIdx)});
                setVal(i, res);

                break;
            }

            case Tag::Extract2_2D: {
                auto extract = Extract2_2D::Cast(i);

                bool fastcase = vectorTypeSupport(extract->vec()) &&
                                extract->idx1()->type.isA(
                                    PirType::intReal().notObject().scalar()) &&
                                extract->idx2()->type.isA(
                                    PirType::intReal().notObject().scalar());

                BasicBlock* done;
                auto res = phiBuilder(Representation::Of(i));

                if (fastcase) {
                    auto fallback = BasicBlock::Create(C, "", fun);
                    auto hit2 = BasicBlock::Create(C, "", fun);
                    done = BasicBlock::Create(C, "", fun);

                    llvm::Value* vector = load(extract->vec());

                    if (Representation::Of(extract->vec()) == t::SEXP) {
                        builder.CreateCondBr(isAltrep(vector), fallback, hit2,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit2);
                    }

                    auto ncol = builder.CreateZExt(
                        call(NativeBuiltins::matrixNcols, {vector}), t::i64);
                    auto nrow = builder.CreateZExt(
                        call(NativeBuiltins::matrixNrows, {vector}), t::i64);
                    llvm::Value* index1 = computeAndCheckIndex(
                        extract->idx1(), vector, fallback, nrow);
                    llvm::Value* index2 = computeAndCheckIndex(
                        extract->idx2(), vector, fallback, ncol);

                    llvm::Value* index =
                        builder.CreateMul(nrow, index2, "", true, true);
                    index = builder.CreateAdd(index, index1, "", true, true);

                    auto res0 =
                        extract->vec()->type.isScalar()
                            ? vector
                            : accessVector(vector, index, extract->vec()->type);

                    res.addInput(convert(res0, i->type));
                    builder.CreateBr(done);

                    builder.SetInsertPoint(fallback);
                }

                auto irep = Representation::Of(extract->idx1());
                llvm::Value* res0;

                if (irep != t::SEXP &&
                    Representation::Of(extract->idx2()) == irep) {
                    NativeBuiltin getter;
                    if (irep == t::Int) {
                        getter = NativeBuiltins::extract22ii;
                    } else {
                        assert(irep == t::Double);
                        getter = NativeBuiltins::extract22rr;
                    }

                    auto vector = loadSxp(extract->vec());
                    res0 = call(getter,
                                {vector, load(extract->idx1()),
                                 load(extract->idx2()), loadSxp(extract->env()),
                                 c(extract->srcIdx)});
                } else {

                    auto vector = loadSxp(extract->vec());
                    auto idx1 = loadSxp(extract->idx1());
                    auto idx2 = loadSxp(extract->idx2());
                    res0 = call(NativeBuiltins::extract22,
                                {vector, idx1, idx2, loadSxp(extract->env()),
                                 c(extract->srcIdx)});
                }

                res.addInput(convert(res0, i->type));
                if (fastcase) {
                    builder.CreateBr(done);

                    builder.SetInsertPoint(done);
                }
                setVal(i, res());
                break;
            }

            case Tag::Subassign1_3D: {
                auto subAssign = Subassign1_3D::Cast(i);
                auto vector = loadSxp(subAssign->lhs());
                auto val = loadSxp(subAssign->rhs());
                auto idx1 = loadSxp(subAssign->idx1());
                auto idx2 = loadSxp(subAssign->idx2());
                auto idx3 = loadSxp(subAssign->idx3());

                // We should implement the fast cases (known and primitive
                // types) speculatively here
                auto res =
                    call(NativeBuiltins::subassign13,
                         {vector, idx1, idx2, idx3, val,
                          loadSxp(subAssign->env()), c(subAssign->srcIdx)});
                setVal(i, res);
                break;
            }

            case Tag::Subassign1_2D: {
                auto subAssign = Subassign1_2D::Cast(i);
                auto vector = loadSxp(subAssign->lhs());
                auto val = loadSxp(subAssign->rhs());
                auto idx1 = loadSxp(subAssign->idx1());
                auto idx2 = loadSxp(subAssign->idx2());

                // We should implement the fast cases (known and primitive
                // types) speculatively here
                auto res =
                    call(NativeBuiltins::subassign12,
                         {vector, idx1, idx2, val, loadSxp(subAssign->env()),
                          c(subAssign->srcIdx)});
                setVal(i, res);
                break;
            }

            case Tag::Subassign2_2D: {
                auto subAssign = Subassign2_2D::Cast(i);

                auto idx1Type = subAssign->idx1()->type;
                auto idx2Type = subAssign->idx2()->type;
                auto valType = subAssign->rhs()->type;
                auto vecType = subAssign->lhs()->type;

                BasicBlock* done = nullptr;
                auto res = phiBuilder(Representation::Of(i));

                // Missing cases: store int into double matrix / store double
                // into int matrix
                auto fastcase =
                    idx1Type.isA(PirType::intReal().notObject().scalar()) &&
                    idx2Type.isA(PirType::intReal().notObject().scalar()) &&
                    valType.isScalar() && !vecType.maybeObj() &&
                    ((vecType.isA(RType::integer) &&
                      valType.isA(RType::integer)) ||
                     (vecType.isA(RType::real) && valType.isA(RType::real)));
                // Conversion from scalar to vector. eg. `a = 1; a[10] = 2`
                if (Representation::Of(subAssign->lhs()) != t::SEXP &&
                    Representation::Of(i) == t::SEXP)
                    fastcase = false;

                if (fastcase) {
                    auto fallback = BasicBlock::Create(C, "", fun);
                    auto hit = BasicBlock::Create(C, "", fun);
                    done = BasicBlock::Create(C, "", fun);

                    llvm::Value* vector = load(subAssign->lhs());
                    if (Representation::Of(subAssign->lhs()) == t::SEXP) {
                        builder.CreateCondBr(shared(vector), fallback, hit,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit);
                    }

                    auto ncol = builder.CreateZExt(
                        call(NativeBuiltins::matrixNcols, {vector}), t::i64);
                    auto nrow = builder.CreateZExt(
                        call(NativeBuiltins::matrixNrows, {vector}), t::i64);
                    llvm::Value* index1 = computeAndCheckIndex(
                        subAssign->idx1(), vector, fallback, nrow);
                    llvm::Value* index2 = computeAndCheckIndex(
                        subAssign->idx2(), vector, fallback, ncol);

                    auto val = load(subAssign->rhs());
                    if (Representation::Of(i) == Representation::Sexp) {
                        llvm::Value* index =
                            builder.CreateMul(nrow, index2, "", true, true);
                        index =
                            builder.CreateAdd(index, index1, "", true, true);
                        assignVector(vector, index, val, vecType);
                        res.addInput(convert(vector, i->type));
                    } else {
                        res.addInput(convert(val, i->type));
                    }

                    builder.CreateBr(done);

                    builder.SetInsertPoint(fallback);
                }

                auto idx1 = loadSxp(subAssign->idx1());
                auto idx2 = loadSxp(subAssign->idx2());

                llvm::Value* assign = nullptr;
                auto irep = Representation::Of(subAssign->idx1());
                auto vrep = Representation::Of(subAssign->rhs());
                if (Representation::Of(subAssign->idx2()) == irep &&
                    irep != t::SEXP && vrep != t::SEXP &&
                    subAssign->rhs()->type.isA(subAssign->lhs()->type)) {
                    NativeBuiltin setter;
                    if (irep == t::Int && vrep == t::Int)
                        setter = NativeBuiltins::subassign22iii;
                    else if (irep == t::Double && vrep == t::Int)
                        setter = NativeBuiltins::subassign22rri;
                    else if (irep == t::Int && vrep == t::Double)
                        setter = NativeBuiltins::subassign22iir;
                    else {
                        assert(irep == t::Double && vrep == t::Double);
                        setter = NativeBuiltins::subassign22rrr;
                    }

                    assign = call(
                        setter,
                        {loadSxp(subAssign->lhs()), load(subAssign->idx1()),
                         load(subAssign->idx2()), load(subAssign->rhs()),
                         loadSxp(subAssign->env()), c(subAssign->srcIdx)});
                } else {
                    assign =
                        call(NativeBuiltins::subassign22,
                             {loadSxp(subAssign->lhs()), idx1, idx2,
                              loadSxp(subAssign->rhs()),
                              loadSxp(subAssign->env()), c(subAssign->srcIdx)});
                }

                res.addInput(assign);
                if (fastcase) {
                    builder.CreateBr(done);
                    builder.SetInsertPoint(done);
                }
                setVal(i, res());

                break;
            }

            case Tag::Subassign1_1D: {
                auto subAssign = Subassign1_1D::Cast(i);

                // TODO: Extend a fastPath for generic vectors.
                // TODO: Support type conversions
                auto vecType = subAssign->vector()->type;
                auto valType = subAssign->val()->type;
                auto idxType = subAssign->idx()->type;

                BasicBlock* done = nullptr;
                auto resultRep = Representation::Of(i);
                auto res = phiBuilder(resultRep);

                // Missing cases: store int into double vect / store double into
                // int vect
                bool fastcase =
                    idxType.isA(PirType::intReal().notObject().scalar()) &&
                    valType.isScalar() && !vecType.maybeObj() &&
                    ((vecType.isA(RType::integer) &&
                      valType.isA(RType::integer)) ||
                     (vecType.isA(RType::real) && valType.isA(RType::real)));
                // Conversion from scalar to vector. eg. `a = 1; a[10] = 2`
                if (Representation::Of(subAssign->vector()) != t::SEXP &&
                    Representation::Of(i) == t::SEXP)
                    fastcase = false;

                if (fastcase) {
                    auto fallback = BasicBlock::Create(C, "", fun);
                    done = BasicBlock::Create(C, "", fun);

                    llvm::Value* vector = load(subAssign->vector());
                    if (Representation::Of(subAssign->vector()) == t::SEXP) {
                        auto hit1 = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(isAltrep(vector), fallback, hit1,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit1);

                        if (vecType.maybeHasAttrs()) {
                            auto hit2 = BasicBlock::Create(C, "", fun);
                            builder.CreateCondBr(fastVeceltOkNative(vector),
                                                 hit2, fallback,
                                                 branchMostlyTrue);
                            builder.SetInsertPoint(hit2);
                        }

                        auto hit3 = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(shared(vector), fallback, hit3,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit3);
                    }

                    llvm::Value* index = computeAndCheckIndex(subAssign->idx(),
                                                              vector, fallback);

                    auto val = load(subAssign->val());
                    if (Representation::Of(i) == Representation::Sexp) {
                        assignVector(vector, index, val,
                                     subAssign->vector()->type);
                        res.addInput(convert(vector, i->type));
                    } else {
                        res.addInput(convert(val, i->type));
                    }

                    builder.CreateBr(done);

                    builder.SetInsertPoint(fallback);
                }

                llvm::Value* res0 =
                    call(NativeBuiltins::subassign11,
                         {loadSxp(subAssign->vector()),
                          loadSxp(subAssign->idx()), loadSxp(subAssign->val()),
                          loadSxp(subAssign->env()), c(subAssign->srcIdx)});

                res.addInput(convert(res0, i->type));
                if (fastcase) {
                    builder.CreateBr(done);

                    builder.SetInsertPoint(done);
                }
                setVal(i, res());
                break;
            }

            case Tag::Subassign2_1D: {
                auto subAssign = Subassign2_1D::Cast(i);

                // TODO: Extend a fastPath for generic vectors.
                // TODO: Support type conversions
                auto vecType = subAssign->vector()->type;
                auto valType = subAssign->val()->type;
                auto idxType = subAssign->idx()->type;

                BasicBlock* done = nullptr;
                auto resultRep = Representation::Of(i);
                auto res = phiBuilder(resultRep);

                // Missing cases: store int into double vect / store double into
                // int vect
                bool fastcase =
                    idxType.isA(PirType::intReal().notObject().scalar()) &&
                    valType.isScalar() && !vecType.maybeObj() &&
                    ((vecType.isA(RType::integer) &&
                      valType.isA(RType::integer)) ||
                     (vecType.isA(RType::real) && valType.isA(RType::real)));
                // Conversion from scalar to vector. eg. `a = 1; a[10] = 2`
                if (Representation::Of(subAssign->vector()) != t::SEXP &&
                    Representation::Of(i) == t::SEXP)
                    fastcase = false;

                fastcase = false;
                if (fastcase) {
                    auto fallback = BasicBlock::Create(C, "", fun);
                    done = BasicBlock::Create(C, "", fun);

                    llvm::Value* vector = load(subAssign->vector());
                    if (Representation::Of(subAssign->vector()) == t::SEXP) {
                        auto hit1 = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(isAltrep(vector), fallback, hit1,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit1);

                        auto hit3 = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(shared(vector), fallback, hit3,
                                             branchMostlyFalse);
                        builder.SetInsertPoint(hit3);
                    }

                    llvm::Value* index = computeAndCheckIndex(subAssign->idx(),
                                                              vector, fallback);

                    auto val = load(subAssign->val());
                    if (Representation::Of(i) == Representation::Sexp) {
                        assignVector(vector, index, val,
                                     subAssign->vector()->type);
                        res.addInput(convert(vector, i->type));
                    } else {
                        res.addInput(convert(val, i->type));
                    }

                    builder.CreateBr(done);

                    builder.SetInsertPoint(fallback);
                }

                llvm::Value* res0 = nullptr;
                auto irep = Representation::Of(subAssign->idx());
                auto vrep = Representation::Of(subAssign->val());
                if (irep != t::SEXP && vrep != t::SEXP &&
                    subAssign->val()->type.isA(subAssign->vector()->type)) {
                    NativeBuiltin setter;
                    if (irep == t::Int && vrep == t::Int)
                        setter = NativeBuiltins::subassign21ii;
                    else if (irep == t::Double && vrep == t::Int)
                        setter = NativeBuiltins::subassign21ri;
                    else if (irep == t::Int && vrep == t::Double)
                        setter = NativeBuiltins::subassign21ir;
                    else {
                        assert(irep == t::Double && vrep == t::Double);
                        setter = NativeBuiltins::subassign21rr;
                    }

                    res0 =
                        call(setter,
                             {loadSxp(subAssign->vector()),
                              load(subAssign->idx()), load(subAssign->val()),
                              loadSxp(subAssign->env()), c(subAssign->srcIdx)});
                } else {
                    res0 = call(
                        NativeBuiltins::subassign21,
                        {loadSxp(subAssign->vector()),
                         loadSxp(subAssign->idx()), loadSxp(subAssign->val()),
                         loadSxp(subAssign->env()), c(subAssign->srcIdx)});
                }

                res.addInput(convert(res0, i->type));
                if (fastcase) {
                    builder.CreateBr(done);

                    builder.SetInsertPoint(done);
                }
                setVal(i, res());
                break;
            }

            case Tag::StVar: {
                auto st = StVar::Cast(i);
                auto environment = MkEnv::Cast(st->env());
                if (LdFunctionEnv::Cast(st->env()))
                    environment = myPromenv;

                if (environment && environment->stub) {
                    auto idx = environment->indexOf(st->varName);
                    auto e = loadSxp(environment);
                    BasicBlock* done = BasicBlock::Create(C, "", fun);
                    auto cur = envStubGet(e, idx, environment->nLocals());

                    if (Representation::Of(st->val()) != t::SEXP) {
                        auto fastcase = BasicBlock::Create(C, "", fun);
                        auto fallback = BasicBlock::Create(C, "", fun);

                        auto expected = Representation::Of(st->val()) == t::Int
                                            ? INTSXP
                                            : REALSXP;
                        auto reuse =
                            builder.CreateAnd(isSimpleScalar(cur, expected),
                                              builder.CreateNot(shared(cur)));
                        builder.CreateCondBr(reuse, fastcase, fallback,
                                             branchMostlyTrue);

                        builder.SetInsertPoint(fastcase);
                        auto store =
                            vectorPositionPtr(cur, c(0), st->val()->type);
                        builder.CreateStore(load(st->val()), store);
                        builder.CreateBr(done);

                        builder.SetInsertPoint(fallback);
                    }

                    auto val = loadSxp(st->val());
                    if (Representation::Of(st->val()) == t::SEXP) {
                        auto same = BasicBlock::Create(C, "", fun);
                        auto different = BasicBlock::Create(C, "", fun);
                        builder.CreateCondBr(builder.CreateICmpEQ(val, cur),
                                             same, different);

                        builder.SetInsertPoint(same);
                        ensureNamed(val);
                        if (!st->isStArg)
                            envStubSetNotMissing(e, idx);
                        builder.CreateBr(done);

                        builder.SetInsertPoint(different);
                        incrementNamed(val);
                        envStubSet(e, idx, val, environment->nLocals(),
                                   !st->isStArg);
                        builder.CreateBr(done);
                    } else {
                        ensureNamed(val);
                        envStubSet(e, idx, val, environment->nLocals(),
                                   !st->isStArg);
                    }

                    builder.CreateBr(done);
                    builder.SetInsertPoint(done);
                    break;
                }

                auto pirVal = st->arg<0>().val();
                bool integerValueCase =
                    Representation::Of(pirVal) == Representation::Integer &&
                    pirVal->type.isA(RType::integer);
                bool realValueCase =
                    Representation::Of(pirVal) == Representation::Real &&
                    pirVal->type.isA(RType::real);
                auto setter = NativeBuiltins::stvar;
                if (st->isStArg)
                    setter = NativeBuiltins::starg;
                if (!st->isStArg && integerValueCase)
                    setter = NativeBuiltins::stvari;
                // if (!st->isStArg && realValueCase)
                //     setter = NativeBuiltins::stvarr;
                bool unboxed =
                    setter.llvmSignature->getFunctionParamType(1) != t::SEXP;

                if (bindingsCache.count(environment)) {
                    auto offset = bindingsCache.at(environment).at(st->varName);
                    auto cachePtr =
                        builder.CreateGEP(bindingsCacheBase, c(offset));
                    llvm::Value* cache = builder.CreateLoad(cachePtr);

                    auto hit1 = BasicBlock::Create(C, "", fun);
                    auto hit2 = BasicBlock::Create(C, "", fun);
                    auto hit3 = BasicBlock::Create(C, "", fun);
                    auto identical = BasicBlock::Create(C, "", fun);
                    auto miss = BasicBlock::Create(C, "", fun);
                    auto done = BasicBlock::Create(C, "", fun);

                    builder.CreateCondBr(
                        builder.CreateICmpULE(
                            builder.CreatePtrToInt(cache, t::i64),
                            c(NativeBuiltins::bindingsCacheFails)),
                        miss, hit1, branchMostlyFalse);

                    builder.SetInsertPoint(hit1);
                    auto val = car(cache);
                    builder.CreateCondBr(
                        builder.CreateICmpEQ(val,
                                             constant(R_UnboundValue, t::SEXP)),
                        miss, hit2, branchMostlyFalse);

                    builder.SetInsertPoint(hit2);

                    llvm::Value* newVal = nullptr;
                    if (integerValueCase || realValueCase) {
                        auto hitUnbox = BasicBlock::Create(C, "", fun);
                        auto hitUnbox2 = BasicBlock::Create(C, "", fun);
                        auto fallbackUnbox = BasicBlock::Create(C, "", fun);
                        auto storeType =
                            integerValueCase ? RType::integer : RType::real;
                        auto isScalarType = isSimpleScalar(
                            val, integerValueCase ? INTSXP : REALSXP);
                        auto notShared = builder.CreateNot(shared(val));
                        builder.CreateCondBr(
                            builder.CreateAnd(isScalarType, notShared),
                            hitUnbox, fallbackUnbox);

                        builder.SetInsertPoint(hitUnbox);
                        auto newValNative = load(pirVal);
                        auto oldVal = accessVector(val, c(0), storeType);
                        auto same =
                            integerValueCase
                                ? builder.CreateICmpEQ(newValNative, oldVal)
                                : builder.CreateFCmpOEQ(newValNative, oldVal);
                        builder.CreateCondBr(same, identical, hitUnbox2);

                        builder.SetInsertPoint(hitUnbox2);
                        assignVector(val, c(0), newValNative, storeType);
                        builder.CreateBr(done);

                        builder.SetInsertPoint(fallbackUnbox);
                        newVal = loadSxp(pirVal);
                        builder.CreateBr(hit3);
                    } else {
                        newVal = loadSxp(pirVal);
                        builder.CreateCondBr(builder.CreateICmpEQ(val, newVal),
                                             identical, hit3,
                                             branchMostlyFalse);
                    }

                    builder.SetInsertPoint(hit3);
                    incrementNamed(newVal);
                    assert(cache->getType() == t::SEXP);
                    assert(newVal->getType() == t::SEXP);
                    setCar(cache, newVal);
                    builder.CreateBr(done);

                    builder.SetInsertPoint(identical);
                    // In the fast case (where the value is not updated) we
                    // still need to ensure it is named.
                    ensureNamed(val);
                    builder.CreateBr(done);

                    builder.SetInsertPoint(miss);
                    llvm::Value* theValue =
                        unboxed ? load(pirVal) : loadSxp(pirVal);
                    call(setter, {constant(st->varName, t::SEXP), theValue,
                                  loadSxp(st->env())});
                    builder.CreateBr(done);

                    builder.SetInsertPoint(done);

                } else {
                    llvm::Value* theValue =
                        unboxed ? load(pirVal) : loadSxp(pirVal);
                    call(setter, {constant(st->varName, t::SEXP), theValue,
                                  loadSxp(st->env())});
                }
                break;
            }

            case Tag::StVarSuper: {
                auto st = StVarSuper::Cast(i);
                auto environment = MkEnv::Cast(st->env());
                if (environment) {
                    auto parent = MkEnv::Cast(environment->lexicalEnv());
                    if (environment->stub || (parent && parent->stub)) {
                        call(NativeBuiltins::stvarSuper,
                             {constant(st->varName, t::SEXP),
                              loadSxp(st->arg<0>().val()), loadSxp(st->env())});
                        break;
                    }
                }

                // In case we statically knew the parent PIR already converted
                // super assigns to standard stores
                call(NativeBuiltins::defvar,
                     {constant(st->varName, t::SEXP),
                      loadSxp(st->arg<0>().val()), loadSxp(st->env())});
                break;
            }

            case Tag::Missing: {
                assert(Representation::Of(i) == Representation::Integer);
                auto missing = Missing::Cast(i);
                setVal(i, call(NativeBuiltins::isMissing,
                               {constant(missing->varName, t::SEXP),
                                loadSxp(i->env())}));
                break;
            }

            case Tag::ChkMissing: {
                auto arg = i->arg(0).val();
                if (Representation::Of(arg) == Representation::Sexp)
                    checkMissing(loadSxp(arg));
                setVal(i, load(arg, arg->type.notMissing(),
                               Representation::Of(i)));
                break;
            }

            case Tag::ChkClosure: {
                auto arg = loadSxp(i->arg(0).val());
                call(NativeBuiltins::chkfun,
                     {constant(Rf_install(ChkClosure::Cast(i)->name().c_str()),
                               t::SEXP),
                      arg});
                setVal(i, arg);
                break;
            }

            case Tag::ColonInputEffects: {
                auto a = i->arg(0).val();
                auto b = i->arg(1).val();
                if (Representation::Of(a) == t::SEXP ||
                    Representation::Of(b) == t::SEXP) {
                    setVal(i, call(NativeBuiltins::colonInputEffects,
                                   {loadSxp(a), loadSxp(b), c(i->srcIdx)}));
                    break;
                }

                // Native version of colonInputEffects
                auto checkRhs = [&]() -> llvm::Value* {
                    if (Representation::Of(b) == Representation::Real) {
                        auto ld = builder.CreateFPToSI(load(b), t::i64);
                        return builder.CreateICmpNE(ld, c(INT_MAX, 64));
                    }
                    assert(Representation::Of(b) == Representation::Integer);
                    return builder.CreateICmpNE(load(b), c(INT_MAX));
                };

                auto sequenceIsReal =
                    Representation::Of(a) != Representation::Real
                        ? builder.getFalse()
                        : builder.CreateNot(checkDoubleToInt(load(a)));

                auto res = createSelect2(
                    sequenceIsReal,
                    [&]() -> llvm::Value* {
                        // If the lhs is truly real, then the sequence is
                        // real and we always go into fastcase
                        return builder.getTrue();
                    },
                    [&]() -> llvm::Value* {
                        auto sequenceIsAmbiguous =
                            Representation::Of(b) == Representation::Real
                                ? builder.CreateNot(checkDoubleToInt(load(b)))
                                : builder.getFalse();

                        return createSelect2(
                            sequenceIsAmbiguous,
                            [&]() -> llvm::Value* {
                                // If the lhs is integer and the rhs is
                                // real we don't support it as fastcase
                                return builder.getFalse();
                            },
                            // This is the case where both sides are int-ish,
                            // we need to check for overflow here.
                            checkRhs);
                    });

                setVal(i, builder.CreateZExt(res, t::i32));
                break;
            }

            case Tag::ColonCastLhs: {
                auto a = i->arg(0).val();
                if (Representation::Of(a) == t::SEXP ||
                    Representation::Of(i) == t::SEXP) {
                    setVal(i, call(NativeBuiltins::colonCastLhs, {loadSxp(a)}));
                    break;
                }
                auto ld = load(a);

                auto naBr = BasicBlock::Create(C, "", fun);
                auto contBr = BasicBlock::Create(C, "", fun);
                nacheck(ld, a->type, naBr, contBr);

                builder.SetInsertPoint(naBr);
                auto msg = builder.CreateGlobalString("NA/NaN argument");
                call(NativeBuiltins::error,
                     {builder.CreateInBoundsGEP(msg, {c(0), c(0)})});
                builder.CreateUnreachable();

                builder.SetInsertPoint(contBr);
                setVal(i, convert(ld, i->type));
                break;
            }

            case Tag::ColonCastRhs: {
                auto a = i->arg(0).val();
                auto b = i->arg(1).val();
                if (Representation::Of(a) == t::SEXP ||
                    Representation::Of(b) == t::SEXP ||
                    Representation::Of(i) == t::SEXP) {
                    setVal(i, call(NativeBuiltins::colonCastRhs,
                                   {loadSxp(a), loadSxp(b)}));
                    break;
                }

                auto ldb = load(b);

                auto naBr = BasicBlock::Create(C, "", fun);
                auto contBr = BasicBlock::Create(C, "", fun);
                nacheck(ldb, b->type, naBr, contBr);

                builder.SetInsertPoint(naBr);
                auto msg = builder.CreateGlobalString("NA/NaN argument");
                call(NativeBuiltins::error,
                     {builder.CreateInBoundsGEP(msg, {c(0), c(0)})});
                builder.CreateUnreachable();

                builder.SetInsertPoint(contBr);

                // This is such a mess, but unfortunately a more or less literal
                // translation of the corresponding bytecode...

                if (ldb->getType() != t::Double)
                    ldb = builder.CreateSIToFP(ldb, t::Double);
                auto lda = load(a);
                if (lda->getType() != t::Double)
                    lda = builder.CreateSIToFP(lda, t::Double);

                auto increasing = builder.CreateFCmpOLE(lda, ldb);
                auto upwards = [&]() {
                    return builder.CreateFAdd(
                        lda, builder.CreateFAdd(
                                 builder.CreateIntrinsic(
                                     Intrinsic::floor, {ldb->getType()},
                                     {builder.CreateFSub(ldb, lda)}),
                                 c(1.0)));
                };
                auto downwards = [&]() {
                    return builder.CreateFSub(
                        lda, builder.CreateFSub(
                                 builder.CreateIntrinsic(
                                     Intrinsic::floor, {ldb->getType()},
                                     {builder.CreateFSub(lda, ldb)}),
                                 c(1.0)));
                };

                auto res = createSelect2(increasing, upwards, downwards);
                setVal(i, convert(res, i->type));
                break;
            }

            case Tag::Names:
                setVal(i,
                       call(NativeBuiltins::names, {loadSxp(i->arg(0).val())}));
                break;

            case Tag::SetNames:
                setVal(i, call(NativeBuiltins::setNames,
                               {loadSxp(i->arg(0).val()),
                                loadSxp(i->arg(1).val())}));
                break;

            case Tag::XLength:
                setVal(i, call(NativeBuiltins::xlength_,
                               {loadSxp(i->arg(0).val())}));
                break;

            case Tag::Int3:
            case Tag::PrintInvocation:
                assert(false);
                break;

            case Tag::_UNUSED_:
                assert(false && "Invalid instruction tag");
                break;

            case Tag::FrameState:
            case Tag::Checkpoint:
            case Tag::Assume:
            case Tag::Deopt:
                assert(false && "Expected scheduled deopt");
                break;

#define V(Value) case Tag::Value:
                COMPILER_VALUES(V)
#undef V
                assert(false && "Values should not occur in instructions");
                break;
            }

            // Here we directly access the variable to bypass liveness
            // checks when loading the variable. This is ok, since this is
            // the current instructoin and we have already written to it...
            assert(*currentInstr == i);
            assert(!variables_.count(i) || variables_.at(i).initialized);
            ++currentInstr;
            if (!Phi::Cast(i))
                ensureNamedIfNeeded(i);

            if (Parameter::RIR_CHECK_PIR_TYPES > 0 &&
                Representation::Of(i) == t::SEXP) {
                if (variables_.count(i) && i->type != PirType::voyd() &&
                    i->type != RType::expandedDots &&
                    i->type != NativeType::context && !CastType::Cast(i) &&
                    !LdConst::Cast(i)) {
                    static std::vector<std::string> leaky;
                    const char* msg = nullptr;
                    if (Parameter::RIR_CHECK_PIR_TYPES > 1) {
                        std::stringstream str;
                        i->printRecursive(str, 2);
                        leaky.push_back(str.str());
                        msg = leaky.back().c_str();
                    }
                    call(NativeBuiltins::checkType,
                         {load(i), c(i->type.serialize()),
                          convertToPointer(msg)});
                }
            }

            numTemps = 0;
        }

        // Copy of phi input values
        for (auto i : *bb) {
            if (phis.count(i)) {
                auto phi = phis.at(i);
                if (deadMove(i, phi))
                    continue;
                auto r = Representation::Of(phi->type);
                auto inpv = load(i, r);
                ensureNamedIfNeeded(phi, inpv);
                updateVariable(phi, inpv);
            }
        }

        if (bb->isJmp())
            builder.CreateBr(getBlock(bb->next()));

        for (auto suc : bb->successors())
            blockInPushContext[suc] = inPushContext;
    });

    // Delayed insertion of the branch, so we can still easily add instructions
    // to the entry block while compiling
    builder.SetInsertPoint(entryBlock);
    builder.CreateBr(getBlock(code->entry));

    std::unordered_set<rir::Code*> codes;
    std::unordered_map<size_t, const pir::TypeFeedback&> variableMapping;
#ifdef DEBUG_REGISTER_MAP
    std::unordered_set<size_t> usedSlots;
#endif
    for (auto& var : variables_) {
        auto i = var.first;
        if (Representation::Of(i) != Representation::Sexp)
            continue;
        if (!i->typeFeedback.origin)
            continue;
        if (!var.second.initialized)
            continue;
        if (var.second.stackSlot < PirTypeFeedback::MAX_SLOT_IDX) {
            codes.insert(i->typeFeedback.srcCode);
            variableMapping.emplace(var.second.stackSlot, i->typeFeedback);
#ifdef DEBUG_REGISTER_MAP
            assert(!usedSlots.count(var.second.stackSlot));
            usedSlots.insert(var.second.stackSlot);
#endif
        }
        if (variableMapping.size() == PirTypeFeedback::MAX_SLOT_IDX)
            break;
    }
    if (!variableMapping.empty()) {
        pirTypeFeedback = PirTypeFeedback::New(codes, variableMapping);
#ifdef DEBUG_REGISTER_MAP
        for (auto m : variableMapping) {
            auto origin = registerMap->getOriginOfSlot(m.first);
            assert(origin == m.second.second);
        }
#endif
    }
}

} // namespace pir
} // namespace rir
