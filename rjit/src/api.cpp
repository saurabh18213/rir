/** Enables the use of R internals for us so that we can manipulate R structures
 * in low level.
 */
#define USE_RINTERNALS 1

#include <llvm/IR/Module.h>

#include "Compiler.h"

#include "api.h"

#include "RIntlns.h"

#include "ir/Ir.h"
#include "ir/Builder.h"
#include "ir/Intrinsics.h"

using namespace rjit;

/** Compiles given ast and returns the NATIVESXP for it.
 */
REXPORT SEXP jitAst(SEXP ast, SEXP formals, SEXP rho) {
    Compiler c("module");
    SEXP result = c.compile("rfunction", ast, formals);
    c.jitAll();
    return result;
}

REXPORT SEXP jitSwapForNative(SEXP original, SEXP native) {
    CAR(original) = CAR(native);
    CDR(original) = CDR(native);
    TAG(original) = TAG(native);
    return original;
}

/** More complex compilation method that compiles multiple functions into a
  specified module name.

  The module name is expected to be a STRSXP and the functions is expected to be
  a pairlist. If pairlist has tags associated with the elements, they will be
  used as function names.
 */
REXPORT SEXP jitFunctions(SEXP moduleName, SEXP functions) {
    char const* mName = CHAR(STRING_ELT(moduleName, 0));
    Compiler c(mName);
    while (functions != R_NilValue) {
        SEXP f = CAR(functions);
        // get the function ast
        SEXP body = BODY(f);
        SEXP formals = FORMALS(f);
        SEXP name = TAG(functions);
        char const* fName =
            (name == R_NilValue) ? "unnamed function" : CHAR(PRINTNAME(name));
        if (TYPEOF(body) == NATIVESXP)
            warning("Ignoring %s because it is already compiled", fName);
        else
            SET_BODY(f, c.compileFunction(fName, body, formals));
        // move to next function
        functions = CDR(functions);
    }
    c.jitAll();
    return moduleName;
}

/** Returns the constant pool associated with the given NATIVESXP.
 */
REXPORT SEXP jitConstants(SEXP expression) {
    assert(TYPEOF(expression) == NATIVESXP and
           "JIT constants can only be extracted from a NATIVESXP argument");
    return CDR(expression);
}

/** Displays the LLVM IR for given NATIVESXP.
 */
REXPORT SEXP jitLLVM(SEXP expression) {
    assert(TYPEOF(expression) == NATIVESXP and
           "LLVM code can only be extracted from a NATIVESXP argument");
    llvm::Function* f = reinterpret_cast<llvm::Function*>(TAG(expression));
    f->dump();
    return R_NilValue;
}

// Should rjit code recompile uncompiled functions before calling them
int RJIT_COMPILE = getenv("RJIT_COMPILE") ? atoi(getenv("RJIT_COMPILE")) : 0;
// The status of R_ENABLE_JIT variable used by gnur
int R_ENABLE_JIT = getenv("R_ENABLE_JIT") ? atoi(getenv("R_ENABLE_JIT")) : 0;

int RJIT_DEBUG = getenv("RJIT_DEBUG") ? atoi(getenv("RJIT_DEBUG")) : 0;

REXPORT SEXP jitDisable(SEXP expression) {
    RJIT_COMPILE = false;
    return R_NilValue;
}

REXPORT SEXP jitEnable(SEXP expression) {
    RJIT_COMPILE = true;
    return R_NilValue;
}
