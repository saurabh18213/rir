#ifndef FUNCTION_CALL_LOGS_H
#define FUNCTION_CALL_LOGS_H

#include "../src/interpreter/call_context.h"

namespace rir {

class FunctionCallLogs {
  public:
    static void recordCallLog(CallContext& call, Function* fun);
    static size_t getASTHash(SEXP closure);
};

} // namespace rir

#endif