#include <iostream>
#include "FunctionCallLogs.h"
#include "R/Printing.h"
#include <unordered_map>
#include <vector>

namespace rir {

namespace {

struct LoggingFunction {
    std::unordered_map<size_t, int> functionID;
    std::vector<std::string> functionName;
    std::vector<size_t> functionIR;
    std::vector<std::unordered_map<unsigned long, std::pair<int, int> >> contextFrequency; 
    
    Context toContext(unsigned long iContext) {
        Context cContext;
        #pragma GCC diagnostic ignored "-Wclass-memaccess"
        memcpy(&cContext, &iContext, sizeof(iContext));
        return cContext;
    }

    std::string getFunctionName(CallContext& call) {
        SEXP lhs = CAR(call.ast);
        SEXP name = R_NilValue;
        if (TYPEOF(lhs) == SYMSXP)
            name = lhs;
        std::string nameStr = CHAR(PRINTNAME(name));
        return nameStr;
    }

    int getFunctionId(CallContext& call, size_t irID) {

        if(functionID.find(irID) == functionID.end()) {
            int id = functionName.size();
            functionID[irID] = id;
            functionIR.push_back(irID);
            functionName.push_back(getFunctionName(call));
            contextFrequency.push_back({});
        }

        return functionID[irID];
    }

    void updateGivenContext(int fId, CallContext& call) {
        unsigned long icontext = call.givenContext.toI();

        if(contextFrequency[fId].find(icontext) == contextFrequency[fId].end()) {
            contextFrequency[fId][icontext] = {0, 0};
        }

        contextFrequency[fId][icontext].first++;
    }

    void updateDispatchedContext(int fId, Function* fun) {
        unsigned long icontext = ((Context)(fun->context())).toI();

        if(contextFrequency[fId].find(icontext) == contextFrequency[fId].end()) {
            contextFrequency[fId][icontext] = {0, 0};
        }

        contextFrequency[fId][icontext].second++;
    }

    ~LoggingFunction() {
        std::cerr <<"\n\n\n\n----------Function Call Logs----------\nThese logs exclude the first few(mostly 2) calls for every function\n\n\n";

        for(int i = 0; i < (int)functionID.size(); i++) {
            std::cerr << i + 1 <<". Function: "<< functionName[i] <<"\t"
                    << functionIR[i] <<"\n\n";

            for(auto it:contextFrequency[i]) {
                std::cerr <<"\tContext: "<< it.first <<"\t"<< this->toContext(it.first) <<"\n";
                std::cerr <<"\t\t"<<"Given: "<< it.second.first <<"\n";
                std::cerr <<"\t\t"<<"Dispatched: "<< it.second.second <<"\n\n";
            }        
        } 
    }

    public: 

    void updateFunctionLogs(CallContext& call, Function* fun, size_t irID) {
        int fId = getFunctionId(call, irID);
        // std::cerr <<"Function: "<< getFunctionName(call) <<" "<< call.callee <<"\n";
        updateGivenContext(fId, call);
        updateDispatchedContext(fId, fun);    
    }
};

} // namespace

std::unique_ptr<LoggingFunction> functionLogger = std::unique_ptr<LoggingFunction>(
    getenv("PIR_ANALYSIS_LOGS") ? new LoggingFunction : nullptr);
std::unique_ptr<std::unordered_map<SEXP, size_t> > functionASTHash = 
    std::unique_ptr<std::unordered_map<SEXP, size_t> >(getenv("PIR_ANALYSIS_LOGS") ? 
    new std::unordered_map<SEXP, size_t> : nullptr);    

void FunctionCallLogs::recordCallLog(CallContext& call, Function* fun) {
    if(!functionLogger)
        return;
    functionLogger->updateFunctionLogs(call, fun, getASTHash(call.callee));
    return;
}

size_t FunctionCallLogs::getASTHash(SEXP closure) {
    if((functionASTHash->find(closure)) != (functionASTHash->end()))
        return (*functionASTHash)[closure];

    assert(isValidClosureSEXP(closure));
    std::string fBody = dumpSexp(closure, ULLONG_MAX);

    for (int i = fBody.length();; i--) {
        if (fBody[i] == '<') {
            fBody = fBody.substr(0, i);
            break;
        }
    }
    
    std::hash<std::string> str_hash;
    size_t aSTHash = str_hash(fBody);
    (*functionASTHash)[closure] = aSTHash;
    return aSTHash;
}

} // namespace rir