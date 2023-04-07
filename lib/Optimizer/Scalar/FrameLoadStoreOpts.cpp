/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#define DEBUG_TYPE "frameloadstoreopts"

#include "hermes/IR/IRBuilder.h"
#include "hermes/IR/Instrs.h"
#include "hermes/Optimizer/PassManager/Pass.h"
#include "hermes/Support/Statistic.h"

#include "llvh/Support/Debug.h"

namespace hermes {
namespace {

static const int kFrameSizeThreshold = 128;

/// \returns the single initializer if the variable \p V is initializes once
/// (in the lexical scope that it belongs to).
static bool getSingleInitializer(Variable *V) {
  StoreFrameInst *singleStore = nullptr;

  for (auto *U : V->getUsers()) {
    if (auto *S = llvh::dyn_cast<StoreFrameInst>(U)) {
      // This is not the first store.
      if (singleStore)
        return false;

      // Initialization happens not in the lexical scope.
      if (S->getParent()->getParent() != V->getParent()->getFunction())
        return false;

      singleStore = S;
    }
  }

  return singleStore;
}

/// Add the variables that the function \p F is capturing into \p capturedVars.
/// Notice that the function F may have sub-closures that capture variables.
/// This method does a recursive scan and collects all captured variables.
void collectCapturedVariables(
    llvh::DenseSet<Variable *> &capturedLoads,
    llvh::DenseSet<Variable *> &capturedStores,
    Function *F) {
  // For all instructions in the function:
  for (auto blockIter = F->begin(), e = F->end(); blockIter != e; ++blockIter) {
    BasicBlock *BB = &*blockIter;
    for (auto &instIter : *BB) {
      Instruction *II = &instIter;

      // Recursively check capturing functions by inspecting the created
      // closure.
      if (auto *CF = llvh::dyn_cast<BaseCreateLexicalChildInst>(II)) {
        collectCapturedVariables(
            capturedLoads, capturedStores, CF->getFunctionCode());
        continue;
      }

      if (auto *LF = llvh::dyn_cast<LoadFrameInst>(II)) {
        Variable *V = LF->getLoadVariable();
        if (V->getParent()->getFunction() != F) {
          capturedLoads.insert(V);
        }
      }

      if (auto *SF = llvh::dyn_cast<StoreFrameInst>(II)) {
        auto *V = SF->getVariable();
        if (V->getParent()->getFunction() != F) {
          capturedStores.insert(V);
        }
      }
    }
  }
}

bool eliminateLoads(BasicBlock *BB) {
  // Check if this block is the entry block.
  Function *F = BB->getParent();
  bool isEntryBlock = (BB == &*F->begin());

  // A list of un-clobbered variable stored values in flight.
  // All of these values are known to be valid for replacement at the current
  // iteration point.
  llvh::DenseMap<Variable *, Value *> knownFrameValues;

  /// A list of variables that are known to stay constant during the lifetime
  /// of the current function.
  llvh::DenseMap<Variable *, Value *> constFrameValues;

  // A list of captured variables that are accessed by loads.
  llvh::DenseSet<Variable *> capturedVariableLoads;
  // A list of captured variables that are accessed by stores.
  llvh::DenseSet<Variable *> capturedVariableStores;

  // In the entry block we can keep track of which variables have been captured
  // by inspecting the closures that we generate.
  bool usePreciseCaptureAnalysis = isEntryBlock;

  IRBuilder::InstructionDestroyer destroyer;

  bool changed = false;

  for (auto &it : *BB) {
    Instruction *II = &it;
    if (auto *SF = llvh::dyn_cast<StoreFrameInst>(II)) {
      Variable *var = SF->getVariable();

      // Record the value stored to the frame:
      knownFrameValues[var] = SF->getValue();
      continue;
    }

    // Try to replace the LoadFrame with a recently saved value.
    if (auto *LF = llvh::dyn_cast<LoadFrameInst>(II)) {
      Variable *dest = LF->getLoadVariable();

      // If this variable is known to be constant during the lifetime of the
      // function then use a previous load.
      auto constEntry = constFrameValues.find(dest);
      if (constEntry != constFrameValues.end() &&
          dest->getParent()->getFunction() != LF->getParent()->getParent()) {
        // Replace all uses of the load with the recently stored value.
        LF->replaceAllUsesWith(constEntry->second);

        // We have no use of this load now. Remove it.
        destroyer.add(LF);
        changed = true;
        continue;
      }

      // The first time we load from a constant variable we need to save the
      // content we are loading.
      if (getSingleInitializer(dest)) {
        constFrameValues[dest] = LF;
      }

      // Search the list of volatile loads.
      auto entry = knownFrameValues.find(dest);

      // We can replace a load with a previously saved value for that variable.
      // unless the saved value was generated by a "non-throwing" load and this
      // load is "throwing".
      if (entry == knownFrameValues.end() || !entry->second) {
        knownFrameValues[dest] = LF;
        continue;
      }

      // Replace all uses of the load with the recently stored value.
      LF->replaceAllUsesWith(entry->second);

      // We have no use of this load now. Remove it.
      destroyer.add(LF);
      changed = true;
      continue;
    }

    if (auto *CF = llvh::dyn_cast<BaseCreateLexicalChildInst>(II)) {
      // Collect the captured variables.
      if (usePreciseCaptureAnalysis) {
        collectCapturedVariables(
            capturedVariableLoads,
            capturedVariableStores,
            CF->getFunctionCode());
      }
    }

    // We know StoreStack cannot write to variables.
    if (llvh::isa<StoreStackInst>(II))
      continue;

    // Invalidate the variable storage if we can't be sure that the instruction
    // is side-effect free and can't touch our variables.
    if (II->mayWriteMemory()) {
      // limit the size of knownFrameValues in case a function is large, as
      // large functions slow down considerably here
      if (usePreciseCaptureAnalysis &&
          knownFrameValues.size() < kFrameSizeThreshold) {
        // Erase all non-local variables.
        for (auto &I : knownFrameValues) {
          // We don't care about variables that are captured as "load variables"
          // because loading the variable does not invalidate the loaded value.
          if (I.first->getParent()->getFunction() != F ||
              capturedVariableStores.count(I.first)) {
            I.second = nullptr;
          }
        }
      } else {
        knownFrameValues.clear();
      }
    }
  }

  return changed;
}

bool eliminateStores(BasicBlock *BB) {
  // Check if this block is the entry block.
  Function *F = BB->getParent();
  bool isEntryBlock = (BB == &*F->begin());

  // A list of un-clobbered frame stored values in flight.
  llvh::DenseMap<Variable *, StoreFrameInst *> prevStoreFrame;

  // Deletes instructions when we leave the function.
  IRBuilder::InstructionDestroyer destroyer;

  // A list of variables that are known to be captured.
  llvh::DenseSet<Variable *> capturedVariables;

  // In the entry block we can keep track of which variables have been captured
  // by inspecting the closures that we generate.
  bool usePreciseCaptureAnalysis = isEntryBlock;

  bool changed = false;

  for (auto &it : *BB) {
    Instruction *II = &it;

    // Try to delete the previous store based on the current store.
    if (auto *SF = llvh::dyn_cast<StoreFrameInst>(II)) {
      auto *V = SF->getVariable();
      auto entry = prevStoreFrame.find(V);

      if (entry != prevStoreFrame.end()) {
        // Found store-after-store. Mark the previous store for deletion.
        if (entry->second) {
          destroyer.add(entry->second);
          changed = true;
        }

        entry->second = SF;
        continue;
      }

      prevStoreFrame[V] = SF;
      continue;
    }

    // Invalidate the frame store storage.
    if (auto *LF = llvh::dyn_cast<LoadFrameInst>(II)) {
      auto *V = LF->getLoadVariable();
      prevStoreFrame[V] = nullptr;
      continue;
    }

    // We know stack operations cannot read variables.
    if (llvh::isa<StoreStackInst>(II) || llvh::isa<LoadStackInst>(II))
      continue;

    // Invalidate the store frame storage if we can't be sure that the
    // instruction is side-effect free and can't touch our variables.
    if (II->mayReadMemory()) {
      // In no-capture mode the local variables are preserved because they have
      // not been captured. This means that we only need to invalidate the
      // variables that don't belong to this function.
      // limit the size of knownFrameValues in case a function is large, as
      // large functions slow down considerably here
      if (usePreciseCaptureAnalysis &&
          prevStoreFrame.size() < kFrameSizeThreshold) {
        // Erase all non-local variables.
        for (auto &I : prevStoreFrame) {
          if (I.first->getParent()->getFunction() != F ||
              capturedVariables.count(I.first)) {
            I.second = nullptr;
          }
        }
      } else {
        // Invalidate all variables.
        prevStoreFrame.clear();
      }
    }

    if (auto *CF = llvh::dyn_cast<BaseCreateLexicalChildInst>(II)) {
      // Collect the captured variables.
      if (usePreciseCaptureAnalysis) {
        collectCapturedVariables(
            capturedVariables, capturedVariables, CF->getFunctionCode());
      }
    }
  }

  return changed;
}

bool runFrameLoadStoreOpts(Module *M) {
  bool changed = false;
  for (auto &F : *M) {
    for (auto &BB : F) {
      changed |= eliminateLoads(&BB);
      changed |= eliminateStores(&BB);
    }
  }
  return changed;
}

} // namespace

Pass *createFrameLoadStoreOpts() {
  class ThisPass : public ModulePass {
   public:
    explicit ThisPass() : ModulePass("FrameLoadStoreOpts") {}
    ~ThisPass() override = default;

    bool runOnModule(Module *M) override {
      return runFrameLoadStoreOpts(M);
    }
  };
  return new ThisPass();
}

} // namespace hermes
#undef DEBUG_TYPE