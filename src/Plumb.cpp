//===-- Plumb.cpp ---------------------------------------------===//
//
//  Plumb · LLVM-IR Cost Analysis Pass  (v2.0)
//
//  Tier 1 :  per-group instruction count + weighted cost
//  Tier 2 :  per-BasicBlock breakdown, ASCII bar chart, hot-spot warnings
//  Tier 3 :  loop-depth multipliers, indirect-call surcharge, function ranking
//  Tier 4 :  cyclomatic complexity, critical-path (worst-case) analysis,
//            inline / vectorisation recommendations, energy estimation,
//            JSON export for the interactive dashboard.
//
//===---------------------------------------------------------------------===//

#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Format.h"

#include <map>
#include <set>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <ctime>

using namespace llvm;

// ═══════════════════════════════════════════════════════════════
//  COMMAND-LINE OPTIONS
// ═══════════════════════════════════════════════════════════════

// All flags use the `plumb-` prefix (Plumb) to avoid
// collisions with LLVM's own command-line options — for example LLVM 14
// itself defines `-inline-threshold` for its inliner pass, which would cause
// `report_fatal_error("inconsistency in registered CommandLine options")`
// at dlopen time.

static cl::opt<std::string> WeightFile(
    "plumb-weight-file",
    cl::desc("[Plumb] path to key=value weight config file"),
    cl::value_desc("filename"),
    cl::init("")
);

static cl::opt<int> HotThreshold(
    "plumb-hot-threshold",
    cl::desc("[Plumb] warn if a function total cost exceeds this value"),
    cl::value_desc("N"),
    cl::init(50)
);

static cl::opt<std::string> OutputFile(
    "plumb-out-file",
    cl::desc("[Plumb] write CSV results to this file"),
    cl::value_desc("filename"),
    cl::init("")
);

static cl::opt<std::string> JsonFile(
    "plumb-json-file",
    cl::desc("[Plumb] write structured JSON results (consumed by dashboard.html)"),
    cl::value_desc("filename"),
    cl::init("")
);

static cl::opt<int> InlineThreshold(
    "plumb-inline-threshold",
    cl::desc("[Plumb] functions with cost below this are flagged INLINE_CANDIDATE"),
    cl::value_desc("N"),
    cl::init(20)
);

static cl::opt<std::string> RunLabel(
    "plumb-run-label",
    cl::desc("[Plumb] label for this run (e.g. 'O0' / 'O2'); embedded in JSON metadata"),
    cl::value_desc("string"),
    cl::init("default")
);

// ═══════════════════════════════════════════════════════════════
//  ENERGY MODEL  (picojoules per executed op, Horowitz '14 inspired)
// ═══════════════════════════════════════════════════════════════

static const std::map<std::string, double> ENERGY_PJ = {
    {"add",      0.4},   // int add ~0.1, float add ~0.4 (avg)
    {"mul",      3.4},   // int mul 3.1, float mul 3.7
    {"memory",  50.0},   // L1 cache hit lower bound
    {"call",   100.0},   // call overhead + linkage
    {"branch",   0.1},
    {"compare",  0.1},
    {"cast",     0.5},
    {"alloca",   5.0},
    {"phi",      0.05},
    {"other",    1.0}
};

// ═══════════════════════════════════════════════════════════════
//  MODULE-LEVEL STATE  (shared across all function invocations)
// ═══════════════════════════════════════════════════════════════

// Per-function captured data, kept until doFinalization writes JSON.
struct BBInfo {
    std::string label;
    long        cost;
    int         instructionCount;
    unsigned    loopDepth;
    bool        isCritical;
    std::vector<std::string> successors;
};

struct GroupInfo {
    std::string group;
    int         count;
    int         weight;
    long        cost;
    double      pct;
    int         indirectCount;   // only meaningful for "call"
};

struct FuncInfo {
    std::string name;
    long        totalCost;
    int         totalInstructions;
    int         cyclomaticComplexity;
    int         loopCount;
    unsigned    maxLoopDepth;
    double      energyPj;
    long        criticalPathCost;
    bool        isHotspot;
    bool        isRecursive;
    std::vector<std::string>          recommendations;
    std::vector<std::string>          criticalPath;
    std::vector<BBInfo>               basicBlocks;
    std::vector<GroupInfo>            groups;
    std::string                       mostExpensiveGroup;
};

static std::vector<FuncInfo>            g_funcs;
static std::map<std::string, long>      g_funcRank;
static std::vector<std::string>         g_csvLines;
static std::map<std::string, int>       g_weightsUsed;

// ═══════════════════════════════════════════════════════════════
//  WEIGHT TABLE LOADER
// ═══════════════════════════════════════════════════════════════

static std::map<std::string, int> loadWeights(const std::string &path) {
    std::map<std::string, int> w = {
        {"add",     1},
        {"mul",     2},
        {"memory",  3},
        {"call",    5},
        {"branch",  1},
        {"compare", 1},
        {"cast",    1},
        {"alloca",  1},
        {"phi",     0},
        {"other",   0}
    };

    if (path.empty()) return w;

    std::ifstream f(path);
    if (!f.is_open()) {
        errs() << "[Plumb] WARNING: cannot open weight file '"
               << path << "' — using defaults\n";
        return w;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string key;
        int val;
        if (std::getline(ss, key, '=') && ss >> val) {
            while (!key.empty() &&
                   (key.back() == ' ' || key.back() == '\r' || key.back() == '\t'))
                key.pop_back();
            w[key] = val;
        }
    }
    errs() << "[Plumb] Loaded weights from: " << path << "\n";
    return w;
}

// ═══════════════════════════════════════════════════════════════
//  INSTRUCTION CLASSIFIER  — 9 groups + direct/indirect calls
// ═══════════════════════════════════════════════════════════════

struct ClassifyResult {
    std::string group;
    bool        isIndirectCall;
};

static ClassifyResult classifyInst(const Instruction &I) {
    ClassifyResult R{"other", false};

    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(&I)) {
        unsigned op = BO->getOpcode();
        if (op == Instruction::Add  || op == Instruction::FAdd ||
            op == Instruction::Sub  || op == Instruction::FSub)
            R.group = "add";
        else if (op == Instruction::Mul  || op == Instruction::FMul)
            R.group = "mul";
        else
            R.group = "other";
        return R;
    }

    if (isa<LoadInst>(I)  || isa<StoreInst>(I)) { R.group = "memory";  return R; }
    if (isa<AllocaInst>(I))                      { R.group = "alloca";  return R; }

    if (const CallInst *CI = dyn_cast<CallInst>(&I)) {
        R.group = "call";
        R.isIndirectCall = (CI->getCalledFunction() == nullptr);
        return R;
    }

    if (isa<BranchInst>(I) || isa<SwitchInst>(I) ||
        isa<IndirectBrInst>(I))                   { R.group = "branch";  return R; }

    if (isa<ICmpInst>(I) || isa<FCmpInst>(I))     { R.group = "compare"; return R; }

    if (isa<CastInst>(I))                         { R.group = "cast";    return R; }

    if (isa<PHINode>(I))                          { R.group = "phi";     return R; }

    return R;
}

// ═══════════════════════════════════════════════════════════════
//  ASCII BAR CHART helper
// ═══════════════════════════════════════════════════════════════

static void printBarChart(
        const std::vector<BBInfo> &bbs,
        long maxCost)
{
    const int BAR_WIDTH = 30;
    errs() << "\n  BasicBlock cost bar chart:\n";
    errs() << "  " << std::string(60, '-') << "\n";

    for (auto &b : bbs) {
        int bars = (maxCost > 0)
            ? (int)((double)b.cost / maxCost * BAR_WIDTH)
            : 0;
        errs() << "  ";
        std::string name = b.label;
        if (name.size() > 18) name = name.substr(0, 15) + "...";
        errs() << name;
        for (int i = name.size(); i < 20; i++) errs() << " ";
        errs() << "|";
        for (int i = 0; i < bars; i++) errs() << "#";
        for (int i = bars; i < BAR_WIDTH; i++) errs() << " ";
        errs() << "| " << b.cost;
        if (b.isCritical) errs() << "  [CRIT]";
        errs() << "\n";
    }
}

// ═══════════════════════════════════════════════════════════════
//  JSON helpers
// ═══════════════════════════════════════════════════════════════

static std::string jsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

static std::string nowISO() {
    time_t t = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

// ═══════════════════════════════════════════════════════════════
//  CRITICAL-PATH (longest weighted path through CFG, back-edges removed)
// ═══════════════════════════════════════════════════════════════

static std::vector<BasicBlock*> computeCriticalPath(
        Function &F,
        const std::map<BasicBlock*, long> &bbCost,
        long &outCost)
{
    outCost = 0;

    // Reverse post-order numbering from entry
    ReversePostOrderTraversal<Function*> RPOT(&F);
    std::map<BasicBlock*, int> rpoNum;
    std::vector<BasicBlock*>   rpoOrder;
    int idx = 0;
    for (BasicBlock *BB : RPOT) {
        rpoNum[BB] = idx++;
        rpoOrder.push_back(BB);
    }

    std::map<BasicBlock*, long>          dp;
    std::map<BasicBlock*, BasicBlock*>   parent;

    for (BasicBlock *BB : rpoOrder) {
        long best = 0;
        BasicBlock *bestPred = nullptr;
        for (BasicBlock *Pred : predecessors(BB)) {
            // Skip back-edges: predecessor must come earlier in RPO
            auto itP = rpoNum.find(Pred);
            if (itP == rpoNum.end()) continue;
            if (itP->second >= rpoNum[BB]) continue;
            auto itDP = dp.find(Pred);
            if (itDP == dp.end()) continue;
            if (itDP->second > best) {
                best     = itDP->second;
                bestPred = Pred;
            }
        }
        long c = 0;
        auto itC = bbCost.find(BB);
        if (itC != bbCost.end()) c = itC->second;
        dp[BB]     = best + c;
        parent[BB] = bestPred;
    }

    // Pick the "exit" with the highest dp value
    BasicBlock *endBB = nullptr;
    long maxPath = -1;
    for (BasicBlock &BB : F) {
        auto it = dp.find(&BB);
        if (it == dp.end()) continue;
        if (it->second > maxPath) {
            maxPath = it->second;
            endBB   = &BB;
        }
    }

    std::vector<BasicBlock*> path;
    if (!endBB) return path;
    outCost = maxPath;
    for (BasicBlock *cur = endBB; cur; cur = parent[cur]) {
        path.push_back(cur);
        if (parent[cur] == nullptr) break;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

// ═══════════════════════════════════════════════════════════════
//  THE PASS
// ═══════════════════════════════════════════════════════════════

namespace {
struct Plumb : public FunctionPass {
    static char ID;
    Plumb() : FunctionPass(ID) {}

    void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.addRequired<LoopInfoWrapperPass>();
        AU.setPreservesAll();
    }

    bool runOnFunction(Function &F) override {
        if (F.isDeclaration()) return false;

        std::map<std::string, int> weights = loadWeights(WeightFile);
        g_weightsUsed = weights;

        LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

        std::map<std::string, int>  instrCount;
        std::map<std::string, long> instrCost;
        std::map<std::string, int>  indirectCount;

        // Per-BB info, indexed both by pointer (for graph algos) and label
        std::map<BasicBlock*, long>        bbCostByPtr;
        std::map<BasicBlock*, int>         bbInstByPtr;
        std::map<BasicBlock*, unsigned>    bbDepthByPtr;
        std::map<BasicBlock*, std::string> bbLabelByPtr;

        long totalCost  = 0;
        int  totalInsts = 0;
        int  cfgEdges   = 0;
        bool isRecursive = false;
        bool sawCallInLoop = false;
        bool sawLoop       = false;

        int bbIndex = 0;
        for (BasicBlock &BB : F) {
            std::string bbLabel = BB.hasName()
                ? BB.getName().str()
                : ("bb." + std::to_string(bbIndex));
            bbIndex++;

            unsigned loopDepth = LI.getLoopDepth(&BB);
            unsigned depthMul  = (loopDepth > 0) ? loopDepth : 1;
            if (loopDepth > 0) sawLoop = true;

            long bbLocal = 0;
            int  bbInsts = 0;

            for (Instruction &I : BB) {
                ClassifyResult CR = classifyInst(I);
                const std::string &group = CR.group;

                int baseWeight = weights.count(group) ? weights[group] : 0;
                int effectiveWeight = baseWeight;
                if (group == "call" && CR.isIndirectCall)
                    effectiveWeight = (int)(baseWeight * 1.6);

                long cost = (long)effectiveWeight * depthMul;

                instrCount[group] += 1;
                instrCost[group]  += cost;
                if (CR.isIndirectCall) indirectCount[group]++;

                bbLocal += cost;
                bbInsts++;
                totalInsts++;

                // Recursion detection (direct self-call)
                if (group == "call") {
                    if (const CallInst *CI = dyn_cast<CallInst>(&I)) {
                        Function *callee = CI->getCalledFunction();
                        if (callee && callee == &F) isRecursive = true;
                        if (loopDepth > 0) sawCallInLoop = true;
                    }
                }
            }

            bbCostByPtr[&BB]   = bbLocal;
            bbInstByPtr[&BB]   = bbInsts;
            bbDepthByPtr[&BB]  = loopDepth;
            bbLabelByPtr[&BB]  = bbLabel;
            cfgEdges          += BB.getTerminator()->getNumSuccessors();

            totalCost += bbLocal;
        }

        g_funcRank[F.getName().str()] = totalCost;

        // Most expensive group
        std::string mostExpensive;
        long        maxGroupCost = -1;
        for (auto &kv : instrCost) {
            if (kv.second > maxGroupCost) {
                maxGroupCost  = kv.second;
                mostExpensive = kv.first;
            }
        }

        // Most expensive BB
        long maxBBCost = 0;
        for (auto &kv : bbCostByPtr)
            if (kv.second > maxBBCost) maxBBCost = kv.second;

        // Cyclomatic complexity:  M = E - N + 2
        int N = (int)bbCostByPtr.size();
        int M = (N > 0) ? (cfgEdges - N + 2) : 0;

        // Loop count (top-level) + max depth across BBs
        int loopCount = 0;
        unsigned maxDepth = 0;
        for (auto &kv : bbDepthByPtr) maxDepth = std::max(maxDepth, kv.second);
        for (auto it = LI.begin(), e = LI.end(); it != e; ++it) loopCount++;

        // Energy estimate
        double energyPj = 0.0;
        for (auto &kv : instrCount) {
            auto e = ENERGY_PJ.find(kv.first);
            if (e != ENERGY_PJ.end())
                energyPj += e->second * kv.second;
        }

        // Critical path
        long criticalCost = 0;
        std::vector<BasicBlock*> critPath =
            computeCriticalPath(F, bbCostByPtr, criticalCost);
        std::set<BasicBlock*> critSet(critPath.begin(), critPath.end());

        // Recommendations
        std::vector<std::string> recs;
        if (totalCost < InlineThreshold && F.getName() != "main")
            recs.push_back("INLINE_CANDIDATE");
        if (sawLoop && !sawCallInLoop)
            recs.push_back("VECTORIZABLE");
        if (isRecursive)
            recs.push_back("RECURSIVE");
        if (totalCost > HotThreshold)
            recs.push_back("HOTSPOT");
        if (M >= 10)
            recs.push_back("HIGH_COMPLEXITY");

        // ════════════════════════════════════════════════════
        //  PRINT REPORT (terminal)
        // ════════════════════════════════════════════════════
        errs() << "\n";
        errs() << "+==========================================================+\n";
        errs() << "  Plumb  >>  Function: " << F.getName() << "\n";
        errs() << "+==========================================================+\n";

        errs() << "\n  Instruction-Type Analysis:\n";
        errs() << "  +----------+-------+--------+--------+--------------+\n";
        errs() << "  | Group    | Count | Weight |  Cost  | Contribution |\n";
        errs() << "  +----------+-------+--------+--------+--------------+\n";

        std::vector<std::string> groupOrder = {
            "add","mul","memory","call","branch","compare","cast","alloca","phi","other"
        };

        std::vector<GroupInfo> groupInfos;
        for (auto &g : groupOrder) {
            if (instrCount.find(g) == instrCount.end()) continue;
            int   cnt  = instrCount[g];
            int   wt   = weights.count(g) ? weights[g] : 0;
            long  cost = instrCost[g];
            double pct = (totalCost > 0) ? (100.0 * cost / totalCost) : 0.0;
            int   ic   = indirectCount.count(g) ? indirectCount[g] : 0;

            std::string gLabel = g;
            if (g == "call" && ic > 0) gLabel += "(i)";

            errs() << "  | ";
            errs() << gLabel;
            for (int i = gLabel.size(); i < 8; i++) errs() << " ";
            errs() << " |  ";
            errs() << cnt;
            for (int i = std::to_string(cnt).size(); i < 4; i++) errs() << " ";
            errs() << " |   ";
            errs() << wt;
            for (int i = std::to_string(wt).size(); i < 5; i++) errs() << " ";
            errs() << " | ";
            errs() << cost;
            for (int i = std::to_string(cost).size(); i < 5; i++) errs() << " ";
            errs() << "  |    ";
            errs() << (int)pct << "%";
            for (int i = std::to_string((int)pct).size(); i < 7; i++) errs() << " ";
            errs() << "|\n";

            GroupInfo gi{g, cnt, wt, cost, pct, ic};
            groupInfos.push_back(gi);
        }
        errs() << "  +----------+-------+--------+--------+--------------+\n";

        // ── Per-BB cost table ─────────────────────────────────
        errs() << "\n  Per-BasicBlock Cost:\n";
        errs() << "  +----------------------+--------+--------------+\n";
        errs() << "  | BasicBlock           |  Cost  |  % of Func   |\n";
        errs() << "  +----------------------+--------+--------------+\n";

        std::vector<BBInfo> bbInfos;
        // walk function in order so labels stay deterministic
        for (BasicBlock &BB : F) {
            BBInfo bi;
            bi.label            = bbLabelByPtr[&BB];
            bi.cost             = bbCostByPtr[&BB];
            bi.instructionCount = bbInstByPtr[&BB];
            bi.loopDepth        = bbDepthByPtr[&BB];
            bi.isCritical       = critSet.count(&BB) > 0;
            for (BasicBlock *Succ : successors(&BB))
                bi.successors.push_back(bbLabelByPtr[Succ]);
            bbInfos.push_back(bi);

            double pct = (totalCost > 0) ? (100.0 * bi.cost / totalCost) : 0.0;
            std::string nm = bi.label;
            if (nm.size() > 20) nm = nm.substr(0,17) + "...";
            errs() << "  | " << nm;
            for (int i = nm.size(); i < 20; i++) errs() << " ";
            errs() << " | " << bi.cost;
            for (int i = std::to_string(bi.cost).size(); i < 5; i++) errs() << " ";
            errs() << "  |   " << (int)pct << "%\n";
        }
        errs() << "  +----------------------+--------+--------------+\n";

        printBarChart(bbInfos, maxBBCost);

        // ── Critical path summary ─────────────────────────────
        errs() << "\n  Critical Path (worst-case):  cost = " << criticalCost << "\n  ";
        for (size_t i = 0; i < critPath.size(); i++) {
            errs() << bbLabelByPtr[critPath[i]];
            if (i + 1 < critPath.size()) errs() << " -> ";
        }
        errs() << "\n";

        // ── Summary ───────────────────────────────────────────
        errs() << "\n  Summary:\n";
        errs() << "  Total weighted cost     : " << totalCost      << "\n";
        errs() << "  Total instructions      : " << totalInsts     << "\n";
        errs() << "  BasicBlocks visited     : " << bbInfos.size() << "\n";
        errs() << "  Cyclomatic complexity   : " << M              << "\n";
        errs() << "  Loop count / max depth  : " << loopCount << " / " << maxDepth << "\n";
        errs() << "  Most expensive group    : " << mostExpensive
               << " (cost=" << maxGroupCost << ")\n";
        errs() << "  Estimated energy        : " << (long)energyPj << " pJ ("
               << format("%.2f", energyPj / 1000.0) << " nJ)\n";

        if (!recs.empty()) {
            errs() << "  Recommendations         : ";
            for (size_t i = 0; i < recs.size(); i++) {
                errs() << recs[i];
                if (i + 1 < recs.size()) errs() << ", ";
            }
            errs() << "\n";
        }

        if (totalCost > HotThreshold) {
            errs() << "\n  *** HOTSPOT WARNING: cost " << totalCost
                   << " exceeds threshold " << HotThreshold.getValue()
                   << " ***\n";
        }
        errs() << "\n";

        // ── CSV accumulation ──────────────────────────────────
        if (!OutputFile.empty()) {
            for (auto &gi : groupInfos) {
                std::ostringstream row;
                row << F.getName().str() << ","
                    << gi.group << ","
                    << gi.count << ","
                    << gi.weight << ","
                    << gi.cost  << ","
                    << std::fixed << std::setprecision(1) << gi.pct;
                g_csvLines.push_back(row.str());
            }
        }

        // ── Capture FuncInfo for JSON ─────────────────────────
        FuncInfo fi;
        fi.name                  = F.getName().str();
        fi.totalCost             = totalCost;
        fi.totalInstructions     = totalInsts;
        fi.cyclomaticComplexity  = M;
        fi.loopCount             = loopCount;
        fi.maxLoopDepth          = maxDepth;
        fi.energyPj              = energyPj;
        fi.criticalPathCost      = criticalCost;
        fi.isHotspot             = (totalCost > HotThreshold);
        fi.isRecursive           = isRecursive;
        fi.recommendations       = recs;
        fi.basicBlocks           = bbInfos;
        fi.groups                = groupInfos;
        fi.mostExpensiveGroup    = mostExpensive;
        for (BasicBlock *BB : critPath)
            fi.criticalPath.push_back(bbLabelByPtr[BB]);
        g_funcs.push_back(fi);

        return false;
    }

    // ═══════════════════════════════════════════════════════════
    //  doFinalization — runs once after ALL functions analyzed
    // ═══════════════════════════════════════════════════════════
    bool doFinalization(Module &M) override {

        // ── Function ranking table ────────────────────────────
        if (!g_funcRank.empty()) {
            std::vector<std::pair<std::string,long>> ranked(
                g_funcRank.begin(), g_funcRank.end());
            std::sort(ranked.begin(), ranked.end(),
                [](const std::pair<std::string,long> &a,
                   const std::pair<std::string,long> &b){
                    return a.second > b.second;
                });

            errs() << "+==========================================================+\n";
            errs() << "  FUNCTION RANKING  (by total weighted cost, highest first)\n";
            errs() << "+==========================================================+\n";
            errs() << "  +----+--------------------------+----------+\n";
            errs() << "  | #  | Function                 |   Cost   |\n";
            errs() << "  +----+--------------------------+----------+\n";

            int rank = 1;
            for (auto &p : ranked) {
                std::string fn = p.first;
                if (fn.size() > 24) fn = fn.substr(0,21) + "...";
                errs() << "  | " << rank;
                for (int i = std::to_string(rank).size(); i < 2; i++) errs() << " ";
                errs() << " | " << fn;
                for (int i = fn.size(); i < 24; i++) errs() << " ";
                errs() << " | " << p.second;
                for (int i = std::to_string(p.second).size(); i < 8; i++) errs() << " ";
                errs() << " |\n";
                rank++;
            }
            errs() << "  +----+--------------------------+----------+\n\n";
        }

        // ── CSV ───────────────────────────────────────────────
        if (!OutputFile.empty() && !g_csvLines.empty()) {
            std::ofstream csv(OutputFile);
            if (csv.is_open()) {
                csv << "function,group,count,weight,cost,pct\n";
                for (auto &line : g_csvLines)
                    csv << line << "\n";
                csv.close();
                errs() << "[Plumb] CSV written to: "
                       << OutputFile << "\n";
            } else {
                errs() << "[Plumb] ERROR: cannot write to: "
                       << OutputFile << "\n";
            }
        }

        // ── JSON ──────────────────────────────────────────────
        if (!JsonFile.empty()) {
            std::ofstream js(JsonFile);
            if (!js.is_open()) {
                errs() << "[Plumb] ERROR: cannot write JSON to: "
                       << JsonFile << "\n";
            } else {
                long totalCostAll  = 0;
                long totalInstsAll = 0;
                double totalEnergy = 0.0;
                for (auto &f : g_funcs) {
                    totalCostAll  += f.totalCost;
                    totalInstsAll += f.totalInstructions;
                    totalEnergy   += f.energyPj;
                }

                js << "{\n";
                js << "  \"metadata\": {\n";
                js << "    \"tool\": \"Plumb\",\n";
                js << "    \"version\": \"2.0\",\n";
                js << "    \"runLabel\": \"" << jsonEscape(RunLabel) << "\",\n";
                js << "    \"module\": \"" << jsonEscape(M.getName().str()) << "\",\n";
                js << "    \"timestamp\": \"" << jsonEscape(nowISO()) << "\",\n";
                js << "    \"hotThreshold\": " << HotThreshold.getValue() << ",\n";
                js << "    \"inlineThreshold\": " << InlineThreshold.getValue() << ",\n";
                js << "    \"weights\": {";
                {
                    bool first = true;
                    for (auto &kv : g_weightsUsed) {
                        if (!first) js << ", ";
                        first = false;
                        js << "\"" << jsonEscape(kv.first) << "\": " << kv.second;
                    }
                }
                js << "},\n";
                js << "    \"energyModelPj\": {";
                {
                    bool first = true;
                    for (auto &kv : ENERGY_PJ) {
                        if (!first) js << ", ";
                        first = false;
                        js << "\"" << jsonEscape(kv.first) << "\": " << kv.second;
                    }
                }
                js << "},\n";
                js << "    \"totals\": {\n";
                js << "      \"totalCost\": "         << totalCostAll  << ",\n";
                js << "      \"totalInstructions\": " << totalInstsAll << ",\n";
                js << "      \"functionCount\": "     << g_funcs.size() << ",\n";
                js << "      \"estimatedEnergyPj\": " << totalEnergy   << "\n";
                js << "    }\n";
                js << "  },\n";

                js << "  \"functions\": [\n";
                for (size_t fi = 0; fi < g_funcs.size(); fi++) {
                    const FuncInfo &f = g_funcs[fi];
                    js << "    {\n";
                    js << "      \"name\": \"" << jsonEscape(f.name) << "\",\n";
                    js << "      \"totalCost\": "            << f.totalCost            << ",\n";
                    js << "      \"totalInstructions\": "    << f.totalInstructions    << ",\n";
                    js << "      \"cyclomaticComplexity\": " << f.cyclomaticComplexity << ",\n";
                    js << "      \"loopCount\": "            << f.loopCount            << ",\n";
                    js << "      \"maxLoopDepth\": "         << f.maxLoopDepth         << ",\n";
                    js << "      \"energyPj\": "             << f.energyPj             << ",\n";
                    js << "      \"criticalPathCost\": "     << f.criticalPathCost     << ",\n";
                    js << "      \"isHotspot\": "            << (f.isHotspot ? "true":"false") << ",\n";
                    js << "      \"isRecursive\": "          << (f.isRecursive ? "true":"false") << ",\n";
                    js << "      \"mostExpensiveGroup\": \"" << jsonEscape(f.mostExpensiveGroup) << "\",\n";

                    js << "      \"recommendations\": [";
                    for (size_t i = 0; i < f.recommendations.size(); i++) {
                        if (i) js << ", ";
                        js << "\"" << jsonEscape(f.recommendations[i]) << "\"";
                    }
                    js << "],\n";

                    js << "      \"criticalPath\": [";
                    for (size_t i = 0; i < f.criticalPath.size(); i++) {
                        if (i) js << ", ";
                        js << "\"" << jsonEscape(f.criticalPath[i]) << "\"";
                    }
                    js << "],\n";

                    js << "      \"groups\": [\n";
                    for (size_t i = 0; i < f.groups.size(); i++) {
                        const GroupInfo &g = f.groups[i];
                        js << "        {\"group\":\""   << jsonEscape(g.group) << "\","
                           << " \"count\":"      << g.count
                           << ", \"weight\":"    << g.weight
                           << ", \"cost\":"      << g.cost
                           << ", \"pct\":"       << g.pct
                           << ", \"indirect\":"  << g.indirectCount
                           << "}";
                        if (i + 1 < f.groups.size()) js << ",";
                        js << "\n";
                    }
                    js << "      ],\n";

                    js << "      \"basicBlocks\": [\n";
                    for (size_t i = 0; i < f.basicBlocks.size(); i++) {
                        const BBInfo &b = f.basicBlocks[i];
                        js << "        {\"label\":\"" << jsonEscape(b.label) << "\","
                           << " \"cost\":"        << b.cost
                           << ", \"instructions\":" << b.instructionCount
                           << ", \"loopDepth\":" << b.loopDepth
                           << ", \"isCritical\":" << (b.isCritical ? "true":"false")
                           << ", \"successors\": [";
                        for (size_t k = 0; k < b.successors.size(); k++) {
                            if (k) js << ", ";
                            js << "\"" << jsonEscape(b.successors[k]) << "\"";
                        }
                        js << "]}";
                        if (i + 1 < f.basicBlocks.size()) js << ",";
                        js << "\n";
                    }
                    js << "      ]\n";

                    js << "    }";
                    if (fi + 1 < g_funcs.size()) js << ",";
                    js << "\n";
                }
                js << "  ]\n";
                js << "}\n";
                js.close();
                errs() << "[Plumb] JSON written to: "
                       << JsonFile << "\n";
            }
        }

        // Reset module-level state
        g_funcRank.clear();
        g_csvLines.clear();
        g_funcs.clear();
        g_weightsUsed.clear();
        return false;
    }
};
} // end anonymous namespace

char Plumb::ID = 0;

static RegisterPass<Plumb>
    X("plumb",
      "Plumb · LLVM-IR Cost Analysis (Tier 1+2+3+4)",
      false,
      true
    );
