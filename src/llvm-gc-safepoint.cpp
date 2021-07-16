// This file is a part of Julia. License is MIT: https://julialang.org/license

#define DEBUG_TYPE "gc_safepoint"
#undef DEBUG
#include "llvm-version.h"

#include <llvm-c/Core.h>
#include <llvm-c/Types.h>

#include <llvm/IR/Value.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>

#include "codegen_shared.h"
#include "julia.h"
#include "julia_internal.h"
#include "julia_assert.h"
#include "llvm-pass-helpers.h"

using namespace llvm;

struct GCSafepoint : public FunctionPass, private JuliaPassContext {
    static char ID;
    GCSafepoint() : FunctionPass(ID)
    {}

protected:
    void getAnalysisUsage(AnalysisUsage &AU) const override
    {
        AU.addRequired<LoopInfoWrapperPass>();
        AU.addPreserved<LoopInfoWrapperPass>();
    }

private:
    CallInst *pgcstack;
    void emitGCSafepoint(IRBuilder<> &B);
    Value *getCurrentSignalPage(IRBuilder<> &B);

    bool runOnFunction(Function &F) override;
};

Value *GCSafepoint::getCurrentSignalPage(IRBuilder<> &B)
{
    assert(pgcstack);
    int nthfield = offsetof(jl_tls_states_t, safepoint) / sizeof(void *);
    Value *field = B.CreateInBoundsGEP(T_ppjlvalue, pgcstack, ConstantInt::get(T_size, nthfield));
    field = B.CreateBitCast(field, PointerType::get(PointerType::get(T_size, 0), 0));
    // TODO: TBAA annotation
    return B.CreateLoad(field);
}

void GCSafepoint::emitGCSafepoint(IRBuilder<> &B)
{
    B.CreateCall(getOrDeclare(jl_intrinsics::GCRootFlush));
    B.CreateFence(AtomicOrdering::SequentiallyConsistent, SyncScope::SingleThread);
    B.CreateLoad(T_size, getCurrentSignalPage(B), true);
    B.CreateFence(AtomicOrdering::SequentiallyConsistent, SyncScope::SingleThread);
}

bool GCSafepoint::runOnFunction(Function &F)
{
    LLVM_DEBUG(dbgs() << "GCSafepoint: Processing function " << F.getName() << "\n");
    // Check availability of functions again since they might have been deleted.
    initAll(*F.getParent());

    IRBuilder<> B(F.getEntryBlock().getFirstNonPHI());

    pgcstack = getPGCstack(F);
    if (!pgcstack) {
        LLVM_DEBUG(dbgs() << "GCSafepoint: Function " << F.getName() << " has no pgcstack, inserting one\n");
        pgcstack = B.CreateCall(getOrDeclare(jl_intrinsics::getPGCStack));
    }
    B.SetInsertPoint(pgcstack->getNextNode());

    LLVM_DEBUG(dbgs() << "GCSafepoint: Inserting Safepoint at function entry of " << F.getName() << "\n");
    emitGCSafepoint(B);

    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    for (auto *Loop : LI.getLoopsInPreorder()) {
        // Iterate over backedges
        BasicBlock *Header = Loop->getHeader();
        for (const auto Edge : children<Inverse<BasicBlock *>>(Header)) {
            if (!Loop->contains(Edge))
                continue;
            B.SetInsertPoint(Edge->getTerminator());
            LLVM_DEBUG(dbgs() << "GCSafepoint: Inserting Safepoint at loop backedge: " << *Edge << "\n");
            emitGCSafepoint(B);
        }
    }

    return true;
}

char GCSafepoint::ID = 0;
static RegisterPass<GCSafepoint> X("GCSafepoint", "Insert safepoints",
                                     false /* Only looks at CFG */,
                                     false /* Analysis Pass */);

Pass *createGCSafepointPass()
{
    return new GCSafepoint();
}

extern "C" JL_DLLEXPORT void LLVMExtraAddGCSafepointPass(LLVMPassManagerRef PM)
{
    unwrap(PM)->add(createGCSafepointPass());
}
