#include <algorithm>
#include <cassert>
#include <functional>
#include <set>
#include <stack>
#include <unordered_map>
#include <utility>
#include <vector>

#include "LiveDebugVariables.h"
#include "Spiller.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "regalloc"

static RegisterRegAlloc ssRegAlloc("rass", "Sam + Seth's Register Allocator",
                                   createSSRegisterAllocator);

namespace {

using VirtReg = unsigned int;

std::string virtReg2str(VirtReg reg) {
  static const std::string alphabet = "abcdefghijklmnopqrstuvwxyz";
  VirtReg reduced_reg = (reg - 2147483648);
  std::string out = "";
  while (reduced_reg > 0) {
    unsigned letter_no = reduced_reg % alphabet.size();
    reduced_reg /= alphabet.size();
    out = alphabet[letter_no] + out;
  }
  return out;
}

// https://llvm.org/doxygen/classllvm_1_1MachineFunctionPass.html
class RegAllocSS : public MachineFunctionPass, private LiveRangeEdit::Delegate {
private:
  MachineFunction *MF;
  MachineRegisterInfo *MRI;
  const MachineFrameInfo *MFI;
  const TargetSubtargetInfo *STI;

  const TargetRegisterInfo *TRI;
  const TargetInstrInfo *TII;

  VirtRegMap *VRM;
  LiveIntervals *LIS;
  LiveRegMatrix *LRM;
  RegisterClassInfo *RCI;

  std::vector<MCPhysReg> get_preferred_phys_regs(VirtReg reg);

  std::vector<VirtReg> get_all_virt_regs();

public:
  static char ID;

  RegAllocSS() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override { return "SS Register Allocator"; }

  void getAnalysisUsage(AnalysisUsage &AU) const {
    // TODO: prune these
    AU.setPreservesCFG();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addPreserved<AAResultsWrapperPass>();
    AU.addRequired<LiveIntervals>();
    AU.addPreserved<LiveIntervals>();
    AU.addPreserved<SlotIndexes>();
    AU.addRequired<LiveDebugVariables>();
    AU.addPreserved<LiveDebugVariables>();
    AU.addRequired<LiveStacks>();
    AU.addPreserved<LiveStacks>();
    AU.addRequired<MachineBlockFrequencyInfo>();
    AU.addPreserved<MachineBlockFrequencyInfo>();
    AU.addRequiredID(MachineDominatorsID);
    AU.addPreservedID(MachineDominatorsID);
    AU.addRequired<MachineLoopInfo>();
    AU.addPreserved<MachineLoopInfo>();
    AU.addRequired<VirtRegMap>();
    AU.addPreserved<VirtRegMap>();
    AU.addRequired<LiveRegMatrix>();
    AU.addPreserved<LiveRegMatrix>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoPHIs);
  }

  bool runOnMachineFunction(MachineFunction &MF);

  bool LRE_CanEraseVirtReg(unsigned) override;
  void LRE_WillShrinkVirtReg(unsigned) override;
};

class interference_graph {
private:
  std::unordered_map<VirtReg, std::set<VirtReg>> neighbors;
  std::unordered_map<VirtReg, MCPhysReg> colors;
  const LiveIntervals &LIS;

  void insert(VirtReg reg1) {
    dbgs() << "Insert " << virtReg2str(reg1) << ": ";
    for (const auto &pair : neighbors) {
      VirtReg reg2 = pair.first;
      if (reg1 != reg2 &&
          LIS.getInterval(reg1).overlaps(LIS.getInterval(reg2))) {
        dbgs() << virtReg2str(reg2) << " ";
        neighbors[reg1].insert(reg2);
        neighbors[reg2].insert(reg1);
      }
    }
    dbgs() << "\n";
  }

public:
  interference_graph(const std::vector<VirtReg> &virt_regs,
                     const LiveIntervals &_LIS)
      : LIS(_LIS) {
    for (VirtReg virt_reg1 : virt_regs) {
      // initialize the nieghbor list for this one
      neighbors[virt_reg1];
    }
    for (VirtReg virt_reg1 : virt_regs) {
      insert(virt_reg1);
    }
  }

  std::pair<bool, VirtReg> get_less_than_k(unsigned int k) {
    for (const auto &pair : neighbors) {
      if (pair.second.size() < k) {
        return {true, pair.first};
      }
    }
    return {false, 0};
  }

  VirtReg get_max_node(std::function<bool(VirtReg, VirtReg)> func) {
    using pair = std::pair<VirtReg, std::set<VirtReg>>;
    const auto &m = std::min_element(neighbors.cbegin(), neighbors.cend(),
                                     [&func](const pair &r1, const pair &r2) {
                                       return func(r1.first, r2.first);
                                     });
    return m->first;
  }

  void remove(VirtReg reg) {
    for (VirtReg neighbor : neighbors[reg]) {
      neighbors[neighbor].erase(reg);
    }
    neighbors.erase(reg);
  }

  std::pair<bool, MCPhysReg>
  maybe_insert_and_color(VirtReg reg, const std::vector<MCPhysReg> &phys_regs) {
    insert(reg);
    std::set<MCPhysReg> possible_colors{phys_regs.cbegin(), phys_regs.cend()};
    for (VirtReg neighbor : neighbors[reg]) {
      if (colors.count(neighbor) != 0) {
        possible_colors.erase(colors[neighbor]);
      }
    }
    for (MCPhysReg color : possible_colors) {
      // in the loop
      // found a color
      dbgs() << "colored " << virtReg2str(reg) << " " << color << "\n";
      colors[reg] = color;
      return {true, color};
    }
    // no color found :'(
    return {false, 0};
  }

  bool empty() { return neighbors.empty(); }
};

} // end anonymous namespace

char RegAllocSS::ID = 0;
char &llvm::RegAllocSSID = RegAllocSS::ID;

INITIALIZE_PASS_BEGIN(RegAllocSS, "RegAllocSS", "Sam + Seth Register Allocator",
                      false, false)
// TODO: prune these
INITIALIZE_PASS_DEPENDENCY(LiveDebugVariables)
INITIALIZE_PASS_DEPENDENCY(SlotIndexes)
INITIALIZE_PASS_DEPENDENCY(LiveIntervals)
INITIALIZE_PASS_DEPENDENCY(RegisterCoalescer)
INITIALIZE_PASS_DEPENDENCY(MachineScheduler)
INITIALIZE_PASS_DEPENDENCY(LiveStacks)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_DEPENDENCY(VirtRegMap)
INITIALIZE_PASS_DEPENDENCY(LiveRegMatrix)
INITIALIZE_PASS_END(RegAllocSS, "RegAllocSS", "Sam + Seth Register Allocator",
                    false, false)

bool RegAllocSS::LRE_CanEraseVirtReg(unsigned VirtReg) {
  return true;

  // LiveInterval &LI = LIS->getInterval(VirtReg);
  // if (VRM->hasPhys(VirtReg)) {
  //   return true;
  // }

  // // Unassigned virtreg is probably in the priority queue.
  // // RegAllocBase will erase it after dequeueing.
  // // Nonetheless, clear the live-range so that the debug
  // // dump will show the right state for that VirtReg.
  // LI.clear();
  // return false;
}

void RegAllocSS::LRE_WillShrinkVirtReg(unsigned VirtReg) {
  // if (!VRM->hasPhys(VirtReg))
  //   return;

  // Register is assigned, put it back on the queue for reassignment.
  // regs.push(VirtReg);
}

// returns a list of possible physical registers in preferred order
std::vector<MCPhysReg> RegAllocSS::get_preferred_phys_regs(VirtReg reg) {
  auto array = RCI->getOrder(MRI->getRegClass(reg));
  return std::vector<MCPhysReg>{array.begin(), array.end()};
}

std::vector<VirtReg> RegAllocSS::get_all_virt_regs() {
  std::vector<VirtReg> out;
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned virtReg = TargetRegisterInfo::index2VirtReg(i);
    if (!MRI->reg_nodbg_empty(virtReg)) {
      out.push_back(virtReg);
    }
  }
  return out;
}

bool RegAllocSS::runOnMachineFunction(MachineFunction &MF_) {
  ///// Get information /////
  MF = &MF_;
  MFI = &MF->getFrameInfo();
  STI = &MF->getSubtarget();

  VRM = &getAnalysis<VirtRegMap>();
  LIS = &getAnalysis<LiveIntervals>();
  LRM = &getAnalysis<LiveRegMatrix>();
  RCI = new RegisterClassInfo{};

  MRI = &VRM->getRegInfo();       // &MF->getRegInfo(); // use
  TRI = &VRM->getTargetRegInfo(); // STI->getRegisterInfo();
  TII = STI->getInstrInfo();

  // RegClassInfo.runOnMachineFunction(VRM->getMachineFunction());

  // TODO: make this happen when debug flag is on
  dbgs()
      << "********** SS REGISTER ALLOCATION (spill all registers) **********\n"
      << "********** Function: " << MF->getName() << '\n';
  MF->dump();
  dbgs() << "\n";

  ///// Instantiate relevant objects /////

  // TODO: factor out the malloc for efficiency
  std::unique_ptr<Spiller> spiller{createInlineSpiller(*this, *MF, *VRM)};
  SmallPtrSet<MachineInstr *, 32> DeadRemats;
  std::vector<VirtReg> virt_regs = get_all_virt_regs();
  using VirtRegVec = SmallVector<unsigned, 4>;
  interference_graph graph{virt_regs, *LIS};
  std::stack<VirtReg> stack;

  ///// Preparation /////

  MRI->freezeReservedRegs(*MF);
  RCI->runOnMachineFunction(*MF);
  size_t k = get_preferred_phys_regs(virt_regs[0]).size();
  for (VirtReg virt_reg : virt_regs) {
    k = std::min(k, get_preferred_phys_regs(virt_reg).size());
  }
  dbgs() << "k " << k << "\n";

  ///// Initial pass through all virt_regs /////

  while (!graph.empty()) {
    auto maybe_reg = graph.get_less_than_k(k);
    if (maybe_reg.first) {
      dbgs() << "less than k chose: " << virtReg2str(maybe_reg.second) << "\n";
      graph.remove(maybe_reg.second);
      stack.push(maybe_reg.second);
    } else {
      auto comparison = [](VirtReg r1, VirtReg r2) { return r1 < r2; };
      VirtReg reg = graph.get_max_node(comparison);
      dbgs() << "heuristic chose: " << virtReg2str(reg) << "\n";
      graph.remove(reg);
      stack.push(reg);
    }
  }

  ///// Initial pass through stack /////

  while (!stack.empty()) {
    VirtReg virt_reg = stack.top();
    stack.pop();

    if (!VRM->hasPhys(virt_reg)
        // && !MRI->reg_nodbg_empty(virt_reg)
    ) {
      auto maybe_color = graph.maybe_insert_and_color(
          virt_reg, get_preferred_phys_regs(virt_reg));
      if (maybe_color.first) {
        LRM->assign(LIS->getInterval(virt_reg), maybe_color.second);
        // VRM->assignVirt2Phys(virt_reg, maybe_color.second);
      } else {
        dbgs() << "failed on " << virtReg2str(virt_reg) << "\n";
        VirtRegVec SplitVRegs;
        LiveInterval &LI = LIS->getInterval(virt_reg);
        LiveRangeEdit LRE{&LI, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats};
        spiller->spill(LRE);

        for (VirtReg new_reg : SplitVRegs) {
          LiveInterval *split_vreg = &LIS->getInterval(new_reg);
          // if (MRI->reg_nodbg_empty(split_vreg->reg)) {
          //   LIS->removeInterval(split_vreg->reg);
          //   continue;
          // }
          stack.push(new_reg);
          dbgs() << "redoing " << virtReg2str(new_reg) << "\n";
        }
      }
    }
  }

  ///// Post optimization /////
  spiller->postOptimization();
  for (auto DeadInst : DeadRemats) {
    LIS->RemoveMachineInstrFromMaps(*DeadInst);
    DeadInst->eraseFromParent();
  }
  DeadRemats.clear();
  // delete spiller;
  delete RCI;

  for (VirtReg virt_reg : virt_regs) {
    assert(VRM->hasPhys(virt_reg));
  }

  ///// Error reporting /////
  // TODO: make this happen when VerifyEnabled
  dbgs() << "Post alloc VirtRegMap:\n" << VRM << "\n";
  dbgs() << "Verifying\n";
  MF->verify(this, "After register allocation");

  return true;
}

FunctionPass *llvm::createSSRegisterAllocator() { return new RegAllocSS(); }
