//===-- RegAllocNaive.cpp - Basic Register Allocator ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the RegAllocNaive function pass, which provides a minimal
// implementation of the basic register allocator.
//
//===----------------------------------------------------------------------===//

#include "AllocationOrder.h"
#include "LiveDebugVariables.h"
#include "RegAllocBase.h"
#include "Spiller.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/PassAnalysisSupport.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/Statistic.h"
#include <cstdlib>
#include <queue>
#include <iostream>

using namespace llvm;

#define DEBUG_TYPE "regalloc"

STATISTIC(NumNewQueued    , "Number of new live ranges queued");

static RegisterRegAlloc naiveRegAlloc("ranaive", "Sam + Seth's Naive Register Allocator",
                                      createNaiveRegisterAllocator);

namespace {
class RegAllocNaive : public MachineFunctionPass,
                public RegAllocBase,
                private LiveRangeEdit::Delegate {
  // context
  MachineFunction *MF;

  // state
  std::unique_ptr<Spiller> SpillerInstance;
  std::queue<LiveInterval*> Queue;

  // Scratch space.  Allocated here to avoid repeated malloc calls in
  // selectOrSplit().
  BitVector UsableRegs;

public:
  RegAllocNaive();

  /// Return the pass name.
  StringRef getPassName() const override { return "Naive Register Allocator"; }

  /// RegAllocNaive analysis usage.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override;

  Spiller &spiller() override { return *SpillerInstance; }

  void enqueue(LiveInterval *LI) override {
    Queue.push(LI);
  }

  LiveInterval *dequeue() override {
    if (Queue.empty())
      return nullptr;
    LiveInterval *LI = Queue.front();
    Queue.pop();
    return LI;
  }

  unsigned selectOrSplit(LiveInterval &VirtReg,
                         SmallVectorImpl<unsigned> &SplitVRegs) override;

  /// Perform register allocation.
  bool runOnMachineFunction(MachineFunction &mf) override;

  MachineFunctionProperties getRequiredProperties() const override {
    return MachineFunctionProperties().set(
        MachineFunctionProperties::Property::NoPHIs);
  }

  static char ID;
};

char RegAllocNaive::ID = 0;

} // end anonymous namespace

char &llvm::RegAllocNaiveID = RegAllocNaive::ID;

INITIALIZE_PASS_BEGIN(RegAllocNaive, "RegAllocNaive", "Sam + Seth Naive Register Allocator",
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
INITIALIZE_PASS_END(RegAllocNaive, "RegAllocNaive", "Sam + Seth Naive Register Allocator", false,
                    false)

RegAllocNaive::RegAllocNaive(): MachineFunctionPass(ID) {
}

void RegAllocNaive::getAnalysisUsage(AnalysisUsage &AU) const {
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

void RegAllocNaive::releaseMemory() {
  SpillerInstance.reset();
}

unsigned RegAllocNaive::selectOrSplit(LiveInterval &VirtReg,
                                SmallVectorImpl<unsigned> &SplitVRegs) {
  if (!VirtReg.isSpillable()) {
    SS_DEBUG << "Virtual register not spillable" << std::endl;
    AllocationOrder Order(VirtReg.reg, *VRM, RegClassInfo, Matrix);
    while (unsigned PhysReg = Order.next()) {
      // Check for interference in PhysReg
      switch (Matrix->checkInterference(VirtReg, PhysReg)) {
      case LiveRegMatrix::IK_Free:
        // PhysReg is available, allocate it.
        SS_DEBUG << "Allocating Physical Register " << PhysReg << std::endl;
        return PhysReg;
      default:
        continue;
      }
    }
    assert(false && "Unable to find physical register for unspillable virtual register!");
  }
  LiveRangeEdit LRE(&VirtReg, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats);
  spiller().spill(LRE);

  // The live virtual register requesting allocation was spilled, so tell
  // the caller not to allocate anything during this round.
  SS_DEBUG << "Live virtual register requesting allocation was spilled" << std::endl;
  return 0;
}

bool RegAllocNaive::runOnMachineFunction(MachineFunction &mf) {
  LLVM_DEBUG(dbgs() << "********** NAIVE REGISTER ALLOCATION (spill all registers) **********\n"
                    << "********** Function: " << mf.getName() << '\n');

  MF = &mf;
  SlotIndexes& slotIndexes = getAnalysis<SlotIndexes>();
  MF->print(dbgs(), &slotIndexes);
  SS_DEBUG << std::endl;
  RegAllocBase::init(getAnalysis<VirtRegMap>(),
                     getAnalysis<LiveIntervals>(),
                     getAnalysis<LiveRegMatrix>());

  SpillerInstance.reset(createInlineSpiller(*this, *MF, *VRM));

  // allocatePhysRegs();
    // seedLiveRegs();
  for (unsigned i = 0, e = MRI->getNumVirtRegs(); i != e; ++i) {
    unsigned Reg = TargetRegisterInfo::index2VirtReg(i);
    if (MRI->reg_nodbg_empty(Reg))
      continue;
    enqueue(&LIS->getInterval(Reg));
  }

  // Continue assigning vregs one at a time to available physical registers.
  while (LiveInterval *VirtReg = dequeue()) {
    SS_DEBUG << "Trying to assign vreg to phys reg" << std::endl;
    VirtReg->dump();

    // Unused registers can appear when the spiller coalesces snippets.
    if (MRI->reg_nodbg_empty(VirtReg->reg)) {
      LLVM_DEBUG(dbgs() << "Dropping unused " << *VirtReg << '\n');
      aboutToRemoveInterval(*VirtReg);
      LIS->removeInterval(VirtReg->reg);
      continue;
    }

    // Invalidate all interference queries, live ranges could have changed.
    Matrix->invalidateVirtRegs();

    SmallVector<unsigned, 4> SplitVRegs;
    unsigned AvailablePhysReg = selectOrSplit(*VirtReg, SplitVRegs);

    if (AvailablePhysReg)
      Matrix->assign(*VirtReg, AvailablePhysReg);

    for (unsigned Reg : SplitVRegs) {
      SS_DEBUG << "Split VReg " << Reg << std::endl;
      LiveInterval *SplitVirtReg = &LIS->getInterval(Reg);
      if (MRI->reg_nodbg_empty(SplitVirtReg->reg)) {
        LIS->removeInterval(SplitVirtReg->reg);
        continue;
      }
      enqueue(SplitVirtReg);
      ++NumNewQueued;
    }
  }
  postOptimization();

  MF->dump();  SS_DEBUG << std::endl;

  // Diagnostic output before rewriting
  LLVM_DEBUG(dbgs() << "Post alloc VirtRegMap:\n" << *VRM << "\n");

  releaseMemory();
  return true;
}

FunctionPass* llvm::createNaiveRegisterAllocator()
{
  return new RegAllocNaive();
}
