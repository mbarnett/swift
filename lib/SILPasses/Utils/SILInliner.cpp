//===--- SILInliner.cpp - Inlines SIL functions ----------------------------==//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-inliner"
#include "swift/SILPasses/Utils/SILInliner.h"
#include "swift/SIL/SILDebugScope.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Debug.h"
using namespace swift;

/// \brief Inlines the callee of a given ApplyInst (which must be the value of a
/// FunctionRefInst referencing a function with a known body), into the caller
/// containing the ApplyInst, which must be the same function as provided to the
/// constructor of SILInliner. It only performs one step of inlining: it does
/// not recursively inline functions called by the callee.
///
/// \returns true on success or false if it is unable to inline the function
/// (for any reason).
bool SILInliner::inlineFunction(ApplyInst *AI, ArrayRef<SILValue> Args) {
  SILFunction *CalleeFunction = &Original;
  this->CalleeFunction = CalleeFunction;

  // Do not attempt to inline an apply into its parent function.
  if (AI->getFunction() == CalleeFunction)
    return false;

  SILFunction &F = getBuilder().getFunction();

  assert(AI->getFunction() && AI->getFunction() == &F &&
         "Inliner called on apply instruction in wrong function?");
  assert(((CalleeFunction->getAbstractCC() != AbstractCC::ObjCMethod &&
           CalleeFunction->getAbstractCC() != AbstractCC::C) ||
          IKind == InlineKind::PerformanceInline) &&
         "Cannot inline Objective-C methods or C functions in mandatory "
         "inlining");

  CalleeEntryBB = CalleeFunction->begin();

  // Compute the SILLocation which should be used by all the inlined
  // instructions.
  if (IKind == InlineKind::PerformanceInline) {
    Loc = InlinedLocation::getInlinedLocation(AI->getLoc());
  } else {
    assert(IKind == InlineKind::MandatoryInline && "Unknown InlineKind.");
    Loc = MandatoryInlinedLocation::getMandatoryInlinedLocation(AI->getLoc());
  }

  auto AIScope = AI->getDebugScope();
  // FIXME: Turn this into an assertion instead.
  if (!AIScope)
    AIScope = AI->getFunction()->getDebugScope();

  if (IKind == InlineKind::MandatoryInline) {
    // Mandatory inlining: every instruction inherits scope/location
    // from the call site.
    CallSiteScope = AIScope;
  } else {
    // Performance inlining. Construct a proper inline scope pointing
    // back to the call site.
    CallSiteScope = new (F.getModule())
      SILDebugScope(AI->getLoc(), F, AIScope);
    CallSiteScope->InlinedCallSite = AIScope->InlinedCallSite;
  }

  // Increment the ref count for the inlined function, so it doesn't
  // get deleted before we can emit abstract debug info for it.
  F.getModule().markFunctionAsInlined(CalleeFunction);

  // If the caller's BB is not the last BB in the calling function, then keep
  // track of the next BB so we always insert new BBs before it; otherwise,
  // we just leave the new BBs at the end as they are by default.
  auto IBI = std::next(SILFunction::iterator(AI->getParent()));
  InsertBeforeBB = IBI != F.end() ? IBI : nullptr;

  // Clear argument map and map ApplyInst arguments to the arguments of the
  // callee's entry block.
  ValueMap.clear();
  assert(CalleeEntryBB->bbarg_size() == Args.size() &&
         "Unexpected number of arguments to entry block of function?");
  auto BAI = CalleeEntryBB->bbarg_begin();
  for (auto AI = Args.begin(), AE = Args.end(); AI != AE; ++AI, ++BAI)
    ValueMap.insert(std::make_pair(*BAI, *AI));

  InstructionMap.clear();
  BBMap.clear();
  // Do not allow the entry block to be cloned again
  BBMap.insert(std::make_pair(CalleeEntryBB, nullptr));
  SILBasicBlock::iterator InsertPoint = std::next(SILBasicBlock::iterator(AI));
  getBuilder().setInsertionPoint(InsertPoint);
  // Recursively visit callee's BB in depth-first preorder, starting with the
  // entry block, cloning all instructions other than terminators.
  visitSILBasicBlock(CalleeEntryBB);

  // If the callee's entry block ends in a return, then we can avoid a split.
  if (ReturnInst *RI = dyn_cast<ReturnInst>(CalleeEntryBB->getTerminator())) {
    // Replace all uses of the apply instruction with the operands of the
    // return instruction, appropriately mapped.
    SILValue(AI).replaceAllUsesWith(remapValue(RI->getOperand()));
    // And get rid of the no-longer-needed ApplyInst.
    AI->eraseFromParent();
    return true;
  }

  // Otherwise, split the caller's basic block to create a return-to BB.
  SILBasicBlock *CallerBB = AI->getParent();
  // Split the BB and do NOT create a branch between the old and new
  // BBs; we will create the appropriate terminator manually later.
  SILBasicBlock *ReturnToBB = CallerBB->splitBasicBlock(InsertPoint);
  // Place the return-to BB after all the other mapped BBs.
  if (InsertBeforeBB)
    F.getBlocks().splice(SILFunction::iterator(InsertBeforeBB), F.getBlocks(),
                         SILFunction::iterator(ReturnToBB));
  else
    F.getBlocks().splice(F.getBlocks().end(), F.getBlocks(),
                         SILFunction::iterator(ReturnToBB));
  // Create an argument on the return-to BB representing the returned value.
  SILValue RetArg = new (F.getModule()) SILArgument(AI->getType(0),
                                                    ReturnToBB);
  // Replace all uses of the ApplyInst with the new argument.
  SILValue(AI).replaceAllUsesWith(RetArg);
  // And get rid of the no-longer-needed ApplyInst.
  AI->eraseFromParent();

  // Now iterate over the callee BBs and fix up the terminators.
  getBuilder().setInsertionPoint(CallerBB);
  // We already know that the callee's entry block does not terminate with a
  // Return Inst, so it can definitely be cloned with the normal SILCloner
  // visit function.
  visit(CalleeEntryBB->getTerminator());
  for (auto BI = BBMap.begin(), BE = BBMap.end(); BI != BE; ++BI) {
    // Ignore entry block
    if (BI->first == CalleeEntryBB)
      continue;

    getBuilder().setInsertionPoint(BI->second);

    // Modify return terminators to branch to the return-to BB, rather than
    // trying to clone the ReturnInst.
    if (ReturnInst *RI = dyn_cast<ReturnInst>(BI->first->getTerminator())) {
      getBuilder().createBranch(Loc.getValue(), ReturnToBB,
                                remapValue(RI->getOperand()));
      continue;
    }

    assert(!isa<AutoreleaseReturnInst>(BI->first->getTerminator()) &&
           "Unexpected autorelease return while inlining non-Objective-C "
           "function?");
    // Otherwise use normal visitor, which clones the existing instruction
    // but remaps basic blocks and values.
    visit(BI->first->getTerminator());
  }

  return true;
}

void SILInliner::visitDebugValueInst(DebugValueInst *Inst) {
  // The mandatory inliner drops debug_value instructions when inlining, as if
  // it were a "nodebug" function in C.
  if (IKind == InlineKind::MandatoryInline) return;

  return SILCloner<SILInliner>::visitDebugValueInst(Inst);
}
void SILInliner::visitDebugValueAddrInst(DebugValueAddrInst *Inst) {
  // The mandatory inliner drops debug_value_addr instructions when inlining, as
  // if it were a "nodebug" function in C.
  if (IKind == InlineKind::MandatoryInline) return;

  return SILCloner<SILInliner>::visitDebugValueAddrInst(Inst);
}

SILDebugScope *SILInliner::getOrCreateInlineScope(SILInstruction *Orig) {
  auto CalleeScope = Orig->getDebugScope();
  // We need to fake a scope to add the inline info to it.
  if (!CalleeScope)
    CalleeScope = Orig->getFunction()->getDebugScope();

  auto it = InlinedScopeCache.find(CalleeScope);
  if (it != InlinedScopeCache.end())
    return it->second;

  auto InlineScope = new (getBuilder().getFunction().getModule())
    SILDebugScope(CallSiteScope, CalleeScope, CalleeScope->SILFn);
  assert(CallSiteScope->Parent == InlineScope->InlinedCallSite->Parent);

  InlinedScopeCache.insert({CalleeScope, InlineScope});
  return InlineScope;
}


//===----------------------------------------------------------------------===//
//                                 Cost Model
//===----------------------------------------------------------------------===//

namespace {

// For now Free is 0 and Expensive is 1. This can be changed in the future by
// adding more categories.
enum class InlineCost : unsigned {
  Free = 0,
  Expensive = 1,
  CannotBeInlined = UINT_MAX,
};

} // end anonymous namespace

/// For now just assume that every SIL instruction is one to one with an LLVM
/// instruction. This is of course very much so not true.
///
/// TODO: Fill this out.
static InlineCost instructionInlineCost(SILInstruction &I,
                                        SILFunction *Caller) {
  switch (I.getKind()) {
    case ValueKind::BuiltinFunctionRefInst:
    case ValueKind::GlobalAddrInst:
    case ValueKind::IntegerLiteralInst:
    case ValueKind::FloatLiteralInst:
    case ValueKind::DebugValueInst:
    case ValueKind::DebugValueAddrInst:
    case ValueKind::StringLiteralInst:
    case ValueKind::FixLifetimeInst:
    case ValueKind::FunctionRefInst:
    case ValueKind::SILGlobalAddrInst:
      return InlineCost::Free;

    // Typed GEPs are free.
    case ValueKind::TupleElementAddrInst:
    case ValueKind::StructElementAddrInst:
    case ValueKind::ProjectBlockStorageInst:
      return InlineCost::Free;

    // Aggregates are exploded at the IR level; these are effectively no-ops.
    case ValueKind::TupleInst:
    case ValueKind::StructInst:
    case ValueKind::StructExtractInst:
    case ValueKind::TupleExtractInst:
      return InlineCost::Free;

    // Unchecked casts are free.
    case ValueKind::AddressToPointerInst:
    case ValueKind::PointerToAddressInst:

    case ValueKind::UncheckedRefCastInst:
    case ValueKind::UncheckedAddrCastInst:
    case ValueKind::UncheckedRefBitCastInst:
    case ValueKind::UncheckedTrivialBitCastInst:

    case ValueKind::RawPointerToRefInst:
    case ValueKind::RefToRawPointerInst:

    case ValueKind::UpcastExistentialRefInst:
    case ValueKind::UpcastInst:

    case ValueKind::ThinToThickFunctionInst:
    case ValueKind::ConvertFunctionInst:

    case ValueKind::ThickToObjCMetatypeInst:
    case ValueKind::ObjCToThickMetatypeInst:
      return InlineCost::Free;

    case ValueKind::MetatypeInst:
      // Thin metatypes are always free.
      if (I.getType(0).castTo<MetatypeType>()->getRepresentation()
            == MetatypeRepresentation::Thin)
        return InlineCost::Free;
      // TODO: Thick metatypes are free if they don't require generic or lazy
      // instantiation.
      return InlineCost::Expensive;

    // Protocol descriptor references are free.
    case ValueKind::ObjCProtocolInst:
      return InlineCost::Free;

    // Metatype-to-object conversions are free.
    case ValueKind::ObjCExistentialMetatypeToObjectInst:
    case ValueKind::ObjCMetatypeToObjectInst:
      return InlineCost::Free;

    // Return and unreachable are free.
    case ValueKind::UnreachableInst:
    case ValueKind::ReturnInst:
      return InlineCost::Free;

    // TODO
    case ValueKind::ApplyInst: {
      // Don't inline functions that contain recursions.
      ApplyInst *AI = cast<ApplyInst>(&I);
      auto *FRI = dyn_cast<FunctionRefInst>(AI->getCallee());
      if (FRI && FRI->getReferencedFunction() == AI->getFunction())
        return InlineCost::CannotBeInlined;

      return InlineCost::Expensive;
    }

    case ValueKind::AllocBoxInst:
    case ValueKind::AllocRefInst:
    case ValueKind::AllocRefDynamicInst:
    case ValueKind::AllocStackInst:
    case ValueKind::ValueMetatypeInst:
    case ValueKind::WitnessMethodInst:
    case ValueKind::AssignInst:
    case ValueKind::AutoreleaseReturnInst:
    case ValueKind::BranchInst:
    case ValueKind::CheckedCastBranchInst:
    case ValueKind::CheckedCastAddrBranchInst:
    case ValueKind::ClassMethodInst:
    case ValueKind::CondBranchInst:
    case ValueKind::CondFailInst:
    case ValueKind::CopyBlockInst:
    case ValueKind::CopyAddrInst:
    case ValueKind::RetainValueInst:
    case ValueKind::DeallocBoxInst:
    case ValueKind::DeallocRefInst:
    case ValueKind::DeallocStackInst:
    case ValueKind::DeinitExistentialInst:
    case ValueKind::DestroyAddrInst:
    case ValueKind::ReleaseValueInst:
    case ValueKind::AutoreleaseValueInst:
    case ValueKind::DynamicMethodBranchInst:
    case ValueKind::DynamicMethodInst:
    case ValueKind::EnumInst:
    case ValueKind::EnumIsTagInst:
    case ValueKind::IndexAddrInst:
    case ValueKind::IndexRawPointerInst:
    case ValueKind::InitEnumDataAddrInst:
    case ValueKind::InitExistentialInst:
    case ValueKind::InitExistentialRefInst:
    case ValueKind::InjectEnumAddrInst:
    case ValueKind::IsNonnullInst:
    case ValueKind::LoadInst:
    case ValueKind::LoadWeakInst:
    case ValueKind::OpenExistentialInst:
    case ValueKind::OpenExistentialRefInst:
    case ValueKind::PartialApplyInst:
    case ValueKind::ProjectExistentialInst:
    case ValueKind::ProjectExistentialRefInst:
    case ValueKind::ExistentialMetatypeInst:
    case ValueKind::ProtocolMethodInst:
    case ValueKind::RefElementAddrInst:
    case ValueKind::RefToUnmanagedInst:
    case ValueKind::RefToUnownedInst:
    case ValueKind::StoreInst:
    case ValueKind::StoreWeakInst:
    case ValueKind::StrongReleaseInst:
    case ValueKind::StrongRetainAutoreleasedInst:
    case ValueKind::StrongRetainInst:
    case ValueKind::StrongRetainUnownedInst:
    case ValueKind::SuperMethodInst:
    case ValueKind::SwitchEnumAddrInst:
    case ValueKind::SwitchEnumInst:
    case ValueKind::SwitchIntInst:
    case ValueKind::UncheckedEnumDataInst:
    case ValueKind::UncheckedTakeEnumDataAddrInst:
    case ValueKind::UnconditionalCheckedCastInst:
    case ValueKind::UnconditionalCheckedCastAddrInst:
    case ValueKind::UnmanagedToRefInst:
    case ValueKind::UnownedReleaseInst:
    case ValueKind::UnownedRetainInst:
    case ValueKind::UnownedToRefInst:
    case ValueKind::UpcastExistentialInst:
    case ValueKind::InitBlockStorageHeaderInst:
      return InlineCost::Expensive;

    case ValueKind::SILArgument:
    case ValueKind::SILUndef:
      llvm_unreachable("Only instructions should be passed into this "
                       "function.");
    case ValueKind::MarkFunctionEscapeInst:
    case ValueKind::MarkUninitializedInst:
      llvm_unreachable("not valid in canonical sil");
  }
}

/// \brief Returns the inlining cost of the function.
///
/// \param Caller is nonnull if the function is being evaluated for inlining.
unsigned swift::getFunctionCost(SILFunction *F, SILFunction *Caller,
                                unsigned Cutoff) {
  DEBUG(llvm::dbgs() << "            Calculating cost for " << F->getName()
        << ".\n");

  if (F->isTransparent() == IsTransparent_t::IsTransparent)
    return 0;

  unsigned Cost = 0;
  for (auto &BB : *F) {
    for (auto &I : BB) {
      auto ICost = instructionInlineCost(I, Caller);
      if (ICost == InlineCost::CannotBeInlined)
        return UINT_MAX;

      Cost += unsigned(ICost);

      // If we're debugging, continue calculating the total cost even if we
      // passed the threshold.
      DEBUG(continue);

      // If i is greater than the Cutoff, we already know we are
      // not going to inline this given function, so there is no point in
      // continuing to visit instructions.
      if (Cost > Cutoff)
        return Cost;
    }
  }

  DEBUG(llvm::dbgs() << "            Found cost: " << Cost << "\n");
  return Cost;
}
