#include <cassert>
#include <vector>

#include "LiveDebugVariables.h"
#include "Spiller.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/LiveStacks.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveRangeEdit.h"
#include "llvm/CodeGen/LiveRegMatrix.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/RegisterClassInfo.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/CodeGen/VirtRegMap.h"
#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "regalloc"

static RegisterRegAlloc ssRegAlloc("rass", "Sam + Seth's Register Allocator",
				   createSSRegisterAllocator);

namespace {

  using VirtReg = unsigned int;

  // https://llvm.org/doxygen/classllvm_1_1MachineFunctionPass.html
  class RegAllocSS : public MachineFunctionPass,
                     private LiveRangeEdit::Delegate  {
  private:

    MachineFunction* MF;
    MachineRegisterInfo* MRI;
    const MachineFrameInfo* MFI;
    const TargetSubtargetInfo* STI;

    const TargetRegisterInfo* TRI;
    const TargetInstrInfo* TII;

    VirtRegMap* VRM;
    LiveIntervals* LIS;
    const LiveRegMatrix* LRM;
    RegisterClassInfo* RCI;

    std::queue<VirtReg> regs;

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

bool RABasic::LRE_CanEraseVirtReg(unsigned VirtReg) {
  LiveInterval &LI = LIS->getInterval(VirtReg);
  if (VRM->hasPhys(VirtReg)) {
    return true;
  }

  // Unassigned virtreg is probably in the priority queue.
  // RegAllocBase will erase it after dequeueing.
  // Nonetheless, clear the live-range so that the debug
  // dump will show the right state for that VirtReg.
  LI.clear();
  return false;
}

void RABasic::LRE_WillShrinkVirtReg(unsigned VirtReg) {
  if (!VRM->hasPhys(VirtReg))
    return;

  // Register is assigned, put it back on the queue for reassignment.
  // regs.push(VirtReg);
  VRM.assign
}

// returns a list of possible physical registers in preferred order
std::vector<MCPhysReg> RegAllocSS::get_preferred_phys_regs(VirtReg reg) {
  auto array = RCI->getOrder(MRI->getRegClass(reg));
  return std::vector<MCPhysReg> {array.begin(), array.end()};
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
  MRI = &MF->getRegInfo();
  MFI = &MF->getFrameInfo();
  STI = &MF->getSubtarget();

  TRI = STI->getRegisterInfo();
  TII = STI->getInstrInfo();

  VRM = &getAnalysis<VirtRegMap>();
  LIS = &getAnalysis<LiveIntervals>();
  LRM = &getAnalysis<LiveRegMatrix>();
  RCI = new RegisterClassInfo{};

  // TODO: make this happen when debug flag is on
  dbgs() << "********** SS REGISTER ALLOCATION (spill all registers) **********\n"
	 << "********** Function: " << MF->getName() << '\n';
  MF->dump(); dbgs() << "\n";

  ///// Instantiate relevant objects /////

  // TODO: factor out the malloc for efficiency
  std::unique_ptr<Spiller> spiller {createInlineSpiller(*this, *MF, *VRM)};
  SmallPtrSet<MachineInstr *, 32> DeadRemats;
  std::vector<VirtReg> virt_regs = get_all_virt_regs();
  using VirtRegVec = SmallVector<unsigned, 4>;

  ///// Preparation /////
  MRI->freezeReservedRegs(*MF);
  RCI->runOnMachineFunction(*MF);

  for (VirtReg virt_reg : virt_regs) {
    regs.push(virt_reg);
  }

  ///// Initial pass through virt_regs /////
  while (!regs.empty()) {
    Virtreg virt_reg = reg.front();
    reg.pop();

    // according to comment in RegAllocBase.cpp:
    // > Unused registers can appear when the spiller coalesces snippets.
    if (!MRI->reg_nodbg_empty(virt_reg)) {
      MCPhysReg phys_reg = get_preferred_phys_regs(virt_reg)[0];

      // VRM->assignVirt2StackSlot(virt_reg);
      // VRM->assignVirt2Phys(virt_reg, phys_reg);

      VirtRegVec SplitVRegs;
      LiveInterval& LI = LIS->getInterval(virt_reg);
      LiveRangeEdit LRE {&LI, SplitVRegs, *MF, *LIS, VRM, this, &DeadRemats};
      spiller->spill(LRE);
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

  ///// Error reporting /////
  // TODO: make this happen when VerifyEnabled
  dbgs() << "Post alloc VirtRegMap:\n" << VRM << "\n";
  dbgs() << "Verifying\n";
  MF->verify(this, "After register allocation");

  return true;
}

FunctionPass *llvm::createSSRegisterAllocator() {
  return new RegAllocSS();
}
