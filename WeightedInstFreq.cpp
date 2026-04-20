#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"

#include <map>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iomanip>

using namespace llvm;

// ═══════════════════════════════════════════════════════════════
//  COMMAND-LINE OPTIONS
// ═══════════════════════════════════════════════════════════════

static cl::opt<std::string> WeightFile(
    "weight-file",
    cl::desc("Path to key=value weight config file (e.g. weights.cfg)"),
    cl::value_desc("filename"),
    cl::init("")
);

static cl::opt<int> HotThreshold(
    "hot-threshold",
    cl::desc("Warn if a function total weighted cost exceeds this value"),
    cl::value_desc("N"),
    cl::init(50)
);

static cl::opt<std::string> OutputFile(
    "out-file",
    cl::desc("Write CSV results to this file"),
    cl::value_desc("filename"),
    cl::init("")
);

// ═══════════════════════════════════════════════════════════════
//  MODULE-LEVEL STATE  (shared across all function invocations)
// ═══════════════════════════════════════════════════════════════

// Stores <functionName, totalWeightedCost> for final ranking table
static std::map<std::string, long> g_funcRank;

// CSV lines accumulated across all functions
static std::vector<std::string> g_csvLines;

// ═══════════════════════════════════════════════════════════════
//  WEIGHT TABLE LOADER
// ═══════════════════════════════════════════════════════════════

static std::map<std::string, int> loadWeights(const std::string &path) {
    // Hardcoded defaults — all 9 groups
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
        errs() << "[WeightedInstFreq] WARNING: cannot open weight file '"
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
            // trim trailing whitespace/CR from key
            while (!key.empty() &&
                   (key.back() == ' ' || key.back() == '\r' || key.back() == '\t'))
                key.pop_back();
            w[key] = val;
        }
    }
    errs() << "[WeightedInstFreq] Loaded weights from: " << path << "\n";
    return w;
}

// ═══════════════════════════════════════════════════════════════
//  INSTRUCTION CLASSIFIER  — 9 groups + direct/indirect calls
// ═══════════════════════════════════════════════════════════════

struct ClassifyResult {
    std::string group;
    bool        isIndirectCall;  // only meaningful when group == "call"
};

static ClassifyResult classifyInst(const Instruction &I) {
    ClassifyResult R{"other", false};

    // ── Arithmetic ──────────────────────────────────────────
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(&I)) {
        unsigned op = BO->getOpcode();
        if (op == Instruction::Add  || op == Instruction::FAdd ||
            op == Instruction::Sub  || op == Instruction::FSub)
            R.group = "add";
        else if (op == Instruction::Mul  || op == Instruction::FMul)
            R.group = "mul";
        else
            R.group = "other";   // div, rem, bitwise ops
        return R;
    }

    // ── Memory ──────────────────────────────────────────────
    if (isa<LoadInst>(I)  || isa<StoreInst>(I)) { R.group = "memory";  return R; }
    if (isa<AllocaInst>(I))                      { R.group = "alloca";  return R; }

    // ── Calls — direct vs indirect ───────────────────────────
    if (const CallInst *CI = dyn_cast<CallInst>(&I)) {
        R.group = "call";
        // getCalledFunction() returns null for indirect (function pointer) calls
        R.isIndirectCall = (CI->getCalledFunction() == nullptr);
        return R;
    }

    // ── Control flow ────────────────────────────────────────
    if (isa<BranchInst>(I) || isa<SwitchInst>(I) ||
        isa<IndirectBrInst>(I))                   { R.group = "branch";  return R; }

    // ── Comparisons ─────────────────────────────────────────
    if (isa<ICmpInst>(I) || isa<FCmpInst>(I))     { R.group = "compare"; return R; }

    // ── Type casts ──────────────────────────────────────────
    if (isa<CastInst>(I))                         { R.group = "cast";    return R; }

    // ── PHI nodes ───────────────────────────────────────────
    if (isa<PHINode>(I))                          { R.group = "phi";     return R; }

    return R;   // "other" for ret, unreachable, getelementptr, etc.
}

// ═══════════════════════════════════════════════════════════════
//  ASCII BAR CHART helper
// ═══════════════════════════════════════════════════════════════

static void printBarChart(
        const std::map<std::string, long> &bbCost,
        long maxCost)
{
    const int BAR_WIDTH = 30;
    errs() << "\n  BasicBlock cost bar chart:\n";
    errs() << "  " << std::string(60, '-') << "\n";

    for (auto &kv : bbCost) {
        int bars = (maxCost > 0)
            ? (int)((double)kv.second / maxCost * BAR_WIDTH)
            : 0;
        errs() << "  ";
        // pad BB name to 20 chars
        std::string name = kv.first;
        if (name.size() > 18) name = name.substr(0, 15) + "...";
        errs() << name;
        for (int i = name.size(); i < 20; i++) errs() << " ";
        errs() << "|";
        for (int i = 0; i < bars; i++) errs() << "#";
        for (int i = bars; i < BAR_WIDTH; i++) errs() << " ";
        errs() << "| " << kv.second << "\n";
    }
}

// ═══════════════════════════════════════════════════════════════
//  THE PASS
// ═══════════════════════════════════════════════════════════════

namespace {
struct WeightedInstFreq : public FunctionPass {
    static char ID;
    WeightedInstFreq() : FunctionPass(ID) {}

    // ── Tell the pass manager we need LoopInfo ──────────────
    void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.addRequired<LoopInfoWrapperPass>();
        AU.setPreservesAll();   // we never modify IR
    }

    bool runOnFunction(Function &F) override {
        // Skip declarations (no body)
        if (F.isDeclaration()) return false;

        // Load weights (cheap — small file)
        std::map<std::string, int> weights = loadWeights(WeightFile);

        // Get loop nesting info for this function
        LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

        // ── Per-group accumulators ───────────────────────────
        std::map<std::string, int>  instrCount;   // group → count
        std::map<std::string, long> instrCost;    // group → weighted cost
        std::map<std::string, int>  indirectCount;// count of indirect calls

        // ── Per-BB accumulators ──────────────────────────────
        std::map<std::string, long> bbCost;       // BB label → cost
        int bbIndex = 0;

        long totalCost = 0;

        // ── Traversal ────────────────────────────────────────
        for (BasicBlock &BB : F) {
            long bbLocal = 0;

            // Get a clean BB label
            std::string bbLabel = BB.hasName()
                ? BB.getName().str()
                : ("bb." + std::to_string(bbIndex));
            bbIndex++;

            // Loop depth for this BB (0 = not in any loop)
            unsigned loopDepth = LI.getLoopDepth(&BB);
            // Multiplier: depth 0 → ×1, depth 1 → ×1, depth 2 → ×2, etc.
            unsigned depthMul  = (loopDepth > 0) ? loopDepth : 1;

            for (Instruction &I : BB) {
                ClassifyResult CR = classifyInst(I);
                const std::string &group = CR.group;

                int baseWeight = weights.count(group) ? weights[group] : 0;

                // Indirect call surcharge: ×1.6 of call weight
                int effectiveWeight = baseWeight;
                if (group == "call" && CR.isIndirectCall)
                    effectiveWeight = (int)(baseWeight * 1.6);

                // Apply loop depth multiplier
                long cost = (long)effectiveWeight * depthMul;

                instrCount[group] += 1;
                instrCost[group]  += cost;
                if (CR.isIndirectCall) indirectCount[group]++;

                bbLocal += cost;
            }

            bbCost[bbLabel] = bbLocal;
            totalCost      += bbLocal;
        }

        // ── Record for module-level ranking ──────────────────
        g_funcRank[F.getName().str()] = totalCost;

        // ── Find most expensive group ─────────────────────────
        std::string mostExpensive;
        long        maxGroupCost = -1;
        for (auto &kv : instrCost) {
            if (kv.second > maxGroupCost) {
                maxGroupCost  = kv.second;
                mostExpensive = kv.first;
            }
        }

        // ── Find most expensive BB ────────────────────────────
        long maxBBCost = 0;
        for (auto &kv : bbCost)
            if (kv.second > maxBBCost) maxBBCost = kv.second;

        // ════════════════════════════════════════════════════
        //  PRINT REPORT
        // ════════════════════════════════════════════════════

        errs() << "\n";
        errs() << "╔══════════════════════════════════════════════════════════╗\n";
        errs() << "  WeightedInstFreq  >>  Function: " << F.getName() << "\n";
        errs() << "╚══════════════════════════════════════════════════════════╝\n";

        // ── Instruction type table ────────────────────────────
        errs() << "\n  Instruction-Type Analysis:\n";
        errs() << "  ┌──────────┬───────┬────────┬────────┬──────────────┐\n";
        errs() << "  │ Group    │ Count │ Weight │  Cost  │ Contribution │\n";
        errs() << "  ├──────────┼───────┼────────┼────────┼──────────────┤\n";

        // Print groups in a fixed order for readability
        std::vector<std::string> groupOrder = {
            "add","mul","memory","call","branch","compare","cast","alloca","phi","other"
        };

        for (auto &g : groupOrder) {
            if (instrCount.find(g) == instrCount.end()) continue;
            int   cnt  = instrCount[g];
            int   wt   = weights.count(g) ? weights[g] : 0;
            long  cost = instrCost[g];
            double pct = (totalCost > 0) ? (100.0 * cost / totalCost) : 0.0;

            // Mark indirect calls
            std::string gLabel = g;
            if (g == "call" && indirectCount.count(g) && indirectCount[g] > 0)
                gLabel += "(i)";

            errs() << "  │ ";
            errs() << gLabel;
            for (int i = gLabel.size(); i < 8; i++) errs() << " ";
            errs() << " │  ";
            errs() << cnt;
            for (int i = std::to_string(cnt).size(); i < 4; i++) errs() << " ";
            errs() << " │   ";
            errs() << wt;
            for (int i = std::to_string(wt).size(); i < 5; i++) errs() << " ";
            errs() << " │ ";
            errs() << cost;
            for (int i = std::to_string(cost).size(); i < 5; i++) errs() << " ";
            errs() << "  │    ";
            errs() << (int)pct << "%";
            for (int i = std::to_string((int)pct).size(); i < 7; i++) errs() << " ";
            errs() << "│\n";
        }
        errs() << "  └──────────┴───────┴────────┴────────┴──────────────┘\n";

        // ── Per-BB cost table ─────────────────────────────────
        errs() << "\n  Per-BasicBlock Cost:\n";
        errs() << "  ┌──────────────────────┬────────┬──────────────┐\n";
        errs() << "  │ BasicBlock           │  Cost  │  % of Func   │\n";
        errs() << "  ├──────────────────────┼────────┼──────────────┤\n";
        for (auto &kv : bbCost) {
            double pct = (totalCost > 0) ? (100.0 * kv.second / totalCost) : 0.0;
            std::string nm = kv.first;
            if (nm.size() > 20) nm = nm.substr(0,17) + "...";
            errs() << "  │ " << nm;
            for (int i = nm.size(); i < 20; i++) errs() << " ";
            errs() << " │ " << kv.second;
            for (int i = std::to_string(kv.second).size(); i < 5; i++) errs() << " ";
            errs() << "  │   " << (int)pct << "%\n";
        }
        errs() << "  └──────────────────────┴────────┴──────────────┘\n";

        // ── ASCII bar chart ────────────────────────────────────
        printBarChart(bbCost, maxBBCost);

        // ── Summary ───────────────────────────────────────────
        errs() << "\n  Summary:\n";
        errs() << "  Total weighted cost  : " << totalCost << "\n";
        errs() << "  BasicBlocks visited  : " << bbCost.size() << "\n";
        errs() << "  Most expensive group : " << mostExpensive
               << " (cost=" << maxGroupCost << ")\n";
        errs() << "  Loop depth weighting : enabled\n";

        // ── Hotspot warning ───────────────────────────────────
        if (totalCost > HotThreshold) {
            errs() << "\n  *** HOTSPOT WARNING: cost " << totalCost
                   << " exceeds threshold " << HotThreshold.getValue()
                   << " ***\n";
        }
        errs() << "\n";

        // ── CSV accumulation ──────────────────────────────────
        if (!OutputFile.empty()) {
            for (auto &g : groupOrder) {
                if (instrCount.find(g) == instrCount.end()) continue;
                int   cnt  = instrCount[g];
                int   wt   = weights.count(g) ? weights[g] : 0;
                long  cost = instrCost[g];
                double pct = (totalCost > 0) ? (100.0 * cost / totalCost) : 0.0;

                std::ostringstream row;
                row << F.getName().str() << ","
                    << g << ","
                    << cnt << ","
                    << wt  << ","
                    << cost << ","
                    << std::fixed << std::setprecision(1) << pct;
                g_csvLines.push_back(row.str());
            }
        }

        return false;  // never modify IR
    }

    // ── doFinalization: runs once after ALL functions ─────────
    bool doFinalization(Module &M) override {

        // ── Function ranking table ────────────────────────────
        if (!g_funcRank.empty()) {
            // Sort by cost descending
            std::vector<std::pair<std::string,long>> ranked(
                g_funcRank.begin(), g_funcRank.end());
            std::sort(ranked.begin(), ranked.end(),
                [](const std::pair<std::string,long> &a,
                   const std::pair<std::string,long> &b){
                    return a.second > b.second;
                });

            errs() << "╔══════════════════════════════════════════════════════════╗\n";
            errs() << "  FUNCTION RANKING  (by total weighted cost, highest first)\n";
            errs() << "╚══════════════════════════════════════════════════════════╝\n";
            errs() << "  ┌────┬──────────────────────────┬──────────┐\n";
            errs() << "  │ #  │ Function                 │   Cost   │\n";
            errs() << "  ├────┼──────────────────────────┼──────────┤\n";

            int rank = 1;
            for (auto &p : ranked) {
                std::string fn = p.first;
                if (fn.size() > 24) fn = fn.substr(0,21) + "...";
                errs() << "  │ " << rank;
                for (int i = std::to_string(rank).size(); i < 2; i++) errs() << " ";
                errs() << " │ " << fn;
                for (int i = fn.size(); i < 24; i++) errs() << " ";
                errs() << " │ " << p.second;
                for (int i = std::to_string(p.second).size(); i < 8; i++) errs() << " ";
                errs() << " │\n";
                rank++;
            }
            errs() << "  └────┴──────────────────────────┴──────────┘\n\n";
        }

        // ── Write CSV file ────────────────────────────────────
        if (!OutputFile.empty() && !g_csvLines.empty()) {
            std::ofstream csv(OutputFile);
            if (csv.is_open()) {
                csv << "function,group,count,weight,cost,pct\n";
                for (auto &line : g_csvLines)
                    csv << line << "\n";
                csv.close();
                errs() << "[WeightedInstFreq] CSV written to: "
                       << OutputFile << "\n\n";
            } else {
                errs() << "[WeightedInstFreq] ERROR: cannot write to: "
                       << OutputFile << "\n";
            }
        }

        // Clear module-level state so re-runs are clean
        g_funcRank.clear();
        g_csvLines.clear();

        return false;
    }
};
} // end anonymous namespace

char WeightedInstFreq::ID = 0;

static RegisterPass<WeightedInstFreq>
    X("weighted-inst-freq",
      "Weighted Instruction Frequency Analysis (Tier 1+2+3)",
      false,  // does not modify CFG
      true    // is a pure analysis pass
    );
