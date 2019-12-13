//===-- RegAllocSS.cpp - Sam + Seth Register Allocator ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the RegAllocSS function pass.
//
//===----------------------------------------------------------------------===//

#include "AllocationOrder.h"
#include "LiveDebugVariables.h"
#include "RegAllocBase.h"
#include "Spiller.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Support/Debug.h"
#include <stack>
#include <queue>

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
                     const LiveIntervals &_LIS): LIS(_LIS) {
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
      // in the loop, found a color
      dbgs() << "colored " << virtReg2str(reg) << " " << color << "\n";
      colors[reg] = color;
      return {true, color};
    }
    // no color found :'(
    return {false, 0};
  }

  bool empty() { return neighbors.empty(); }
};

class RegAllocSS : public MachineFunctionPass,
                private LiveRangeEdit::Delegate {
  // context
  MachineFunction *MF;

  const TargetRegisterInfo *TRI = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  VirtRegMap *VRM = nullptr;
  LiveIntervals *LIS = nullptr;
  LiveRegMatrix *Matrix = nullptr;
  RegisterClassInfo RCI;

  SmallPtrSet<MachineInstr *, 32> DeadRemats;


  // state
  std::unique_ptr<Spiller> SpillerInstance;

  std::vector<MCPhysReg> get_preferred_phys_regs(VirtReg reg);

public:
  RegAllocSS();

  /// Return the pass name.
  StringRef getPassName() const override { return "Naive Register Allocator"; }

  /// RegAllocSS analysis usage.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override;

  Spiller &spiller() { return *SpillerInstance; }

  /// Perform register allocation.
  bool runOnMachineFunction(MachineFunction &mf) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoPHIs);
  }

  static char ID;
};

char RegAllocSS::ID = 0;

} // end anonymous namespace

char &llvm::RegAllocSSID = RegAllocSS::ID;

INITIALIZE_PASS_BEGIN(RegAllocSS, "RegAllocSS", "Sam + Seth Naive Register Allocator",
                      false, false)
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
INITIALIZE_PASS_END(RegAllocSS, "RegAllocSS", "Sam + Seth Naive Register Allocator", false,
                    false)

RegAllocSS::RegAllocSS(): MachineFunctionPass(ID) {
}

void RegAllocSS::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesCFG();
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addRequired<LiveIntervals>();
  AU.addPreserved<LiveIntervals>();
  AU.addPreserved<SlotIndexes>();
  AU.addRequired<SlotIndexes>();
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

void RegAllocSS::releaseMemory() {
  SpillerInstance.reset();
}

std::vector<MCPhysReg> RegAllocSS::get_preferred_phys_regs(VirtReg reg) {
  auto array = RCI.getOrder(MRI->getRegClass(reg));
  std::vector<MCPhysReg> regs; 
  for (auto PhysReg: array) {
    switch (Matrix->checkInterference(LIS->getInterval(reg), PhysReg)) {
    case LiveRegMatrix::IK_Free:
      regs.push_back(PhysReg);
      break;
    default:
      continue;
    }
  }
  return regs;
}

bool RegAllocSS::runOnMachineFunction(MachineFunction &mf) {
  LLVM_DEBUG(dbgs() << "********** CHAITIN-BRIGGS REGISTER ALLOCATION **********\n"
                    << "********** Function: " << mf.getName() << '\n');

  MF = &mf;
  SlotIndexes& slotIndexes = getAnalysis<SlotIndexes>();
  MF->print(dbgs(), &slotIndexes);
  SS_DEBUG << std::endl;

  VRM = &getAnalysis<VirtRegMap>();
  LIS = &getAnalysis<LiveIntervals>();
  Matrix = &getAnalysis<LiveRegMatrix>();

  TRI = &VRM->getTargetRegInfo();
  MRI = &VRM->getRegInfo();
  MRI->freezeReservedRegs(VRM->getMachineFunction());
  RCI.runOnMachineFunction(VRM->getMachineFunction());

  SpillerInstance.reset(createInlineSpiller(*this, *MF, *VRM));

  using VirtRegVec = SmallVector<unsigned, 4>;
  std::stack<VirtReg> stack;
  std::vector<VirtReg> virt_regs;
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->reg_nodbg_empty(Reg))
      continue;
    virt_regs.push_back(Reg);
  }
  interference_graph graph{virt_regs, *LIS};

  size_t k = get_preferred_phys_regs(virt_regs[0]).size();
  for (VirtReg virt_reg : virt_regs) {
    k = std::min(k, get_preferred_phys_regs(virt_reg).size());
  }

  //======================= Actual allocation loop =======================

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

    if (!VRM->hasPhys(virt_reg)) {

      auto maybe_color = graph.maybe_insert_and_color(virt_reg, get_preferred_phys_regs(virt_reg));

      if (maybe_color.first) {
        Matrix->assign(LIS->getInterval(virt_reg), maybe_color.second);
      } else {
        dbgs() << "Spilling " << virtReg2str(virt_reg) << "\n";
        VirtRegVec SplitVRegs;
        LiveInterval &LI = LIS->getInterval(virt_reg);
        LiveRangeEdit LRE{&LI, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats};
        spiller().spill(LRE);

        for (VirtReg new_reg : SplitVRegs) {
          LiveInterval *split_vreg = &LIS->getInterval(new_reg);
          if (MRI->reg_nodbg_empty(split_vreg->reg)) {
            LIS->removeInterval(split_vreg->reg);
            continue;
          }
          stack.push(new_reg);
          dbgs() << "redoing " << virtReg2str(new_reg) << "\n";
        }
      }
    }
  }

  //======================= End Actual allocation loop =======================

  spiller().postOptimization();
  for (auto DeadInst : DeadRemats) {
    LIS->RemoveMachineInstrFromMaps(*DeadInst);
    DeadInst->eraseFromParent();
  }
  DeadRemats.clear();

  MF->dump();  SS_DEBUG << std::endl;

  // Diagnostic output before rewriting
  LLVM_DEBUG(dbgs() << "Post alloc VirtRegMap:\n" << *VRM << "\n");

  releaseMemory();
  return true;
}

FunctionPass *llvm::createSSRegisterAllocator() { return new RegAllocSS(); }
