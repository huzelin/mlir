//===- DmaGeneration.cpp - DMA generation pass ------------------------ -*-===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file implements a pass to automatically promote accessed memref regions
// to buffers in a faster memory space that is explicitly managed, with the
// necessary data movement operations expressed as DMAs.
//
//===----------------------------------------------------------------------===//

#include "mlir/Analysis/AffineStructures.h"
#include "mlir/Analysis/Utils.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/StmtVisitor.h"
#include "mlir/Pass.h"
#include "mlir/StandardOps/StandardOps.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/Transforms/Utils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include <algorithm>

#define DEBUG_TYPE "dma-generate"

using namespace mlir;

static llvm::cl::opt<unsigned> clFastMemorySpace(
    "dma-fast-memory-space", llvm::cl::Hidden,
    llvm::cl::desc("Set fast memory space id for DMA generation"));

namespace {

/// Generates DMAs for memref's living in 'slowMemorySpace' into newly created
/// buffers in 'fastMemorySpace', and replaces memory operations to the former
/// by the latter. Only load op's handled for now.
/// TODO(bondhugula): extend this to store op's.
struct DmaGeneration : public FunctionPass, StmtWalker<DmaGeneration> {
  explicit DmaGeneration(unsigned slowMemorySpace = 0,
                         unsigned fastMemorySpaceArg = 1,
                         int minDmaTransferSize = 1024)
      : FunctionPass(&DmaGeneration::passID), slowMemorySpace(slowMemorySpace),
        minDmaTransferSize(minDmaTransferSize) {
    if (clFastMemorySpace.getNumOccurrences() > 0) {
      fastMemorySpace = clFastMemorySpace;
    } else {
      fastMemorySpace = fastMemorySpaceArg;
    }
  }

  // Not applicable to CFG functions.
  PassResult runOnCFGFunction(CFGFunction *f) override { return success(); }
  PassResult runOnMLFunction(MLFunction *f) override;
  void runOnForStmt(ForStmt *forStmt);

  void visitOperationStmt(OperationStmt *opStmt);
  bool generateDma(const MemRefRegion &region, ForStmt *forStmt);

  // List of memory regions to DMA for.
  std::vector<std::unique_ptr<MemRefRegion>> regions;

  // Map from original memref's to the DMA buffers that their accesses are
  // replaced with.
  DenseMap<SSAValue *, SSAValue *> fastBufferMap;

  // Slow memory space associated with DMAs.
  const unsigned slowMemorySpace;
  // Fast memory space associated with DMAs.
  unsigned fastMemorySpace;
  // Minimum DMA transfer size supported by the target in bytes.
  const int minDmaTransferSize;

  // The loop level at which DMAs should be generated. '0' is an outermost loop.
  unsigned dmaDepth;

  static char passID;
};

} // end anonymous namespace

char DmaGeneration::passID = 0;

/// Generates DMAs for memref's living in 'slowMemorySpace' into newly created
/// buffers in 'fastMemorySpace', and replaces memory operations to the former
/// by the latter. Only load op's handled for now.
/// TODO(bondhugula): extend this to store op's.
FunctionPass *mlir::createDmaGenerationPass(unsigned slowMemorySpace,
                                            unsigned fastMemorySpace,
                                            int minDmaTransferSize) {
  return new DmaGeneration(slowMemorySpace, fastMemorySpace,
                           minDmaTransferSize);
}

// Gather regions to promote to buffers in faster memory space.
// TODO(bondhugula): handle store op's; only load's handled for now.
void DmaGeneration::visitOperationStmt(OperationStmt *opStmt) {
  if (auto loadOp = opStmt->dyn_cast<LoadOp>()) {
    if (loadOp->getMemRefType().getMemorySpace() != slowMemorySpace)
      return;
  } else if (auto storeOp = opStmt->dyn_cast<StoreOp>()) {
    if (storeOp->getMemRefType().getMemorySpace() != slowMemorySpace)
      return;
  } else {
    // Neither load nor a store op.
    return;
  }

  // TODO(bondhugula): eventually, we need to be performing a union across all
  // regions for a given memref instead of creating one region per memory op.
  // This way we would be allocating O(num of memref's) sets instead of
  // O(num of load/store op's).
  auto region = std::make_unique<MemRefRegion>();
  if (!getMemRefRegion(opStmt, dmaDepth, region.get())) {
    LLVM_DEBUG(llvm::dbgs() << "Error obtaining memory region\n");
    return;
  }
  LLVM_DEBUG(llvm::dbgs() << "Memory region:\n");
  LLVM_DEBUG(region->getConstraints()->dump());

  regions.push_back(std::move(region));
}

// Creates a buffer in the faster memory space for the specified region;
// generates a DMA from the lower memory space to this one, and replaces all
// loads to load from the buffer. Returns true if DMAs are generated.
bool DmaGeneration::generateDma(const MemRefRegion &region, ForStmt *forStmt) {
  // DMAs for read regions are going to be inserted just before the for loop.
  MLFuncBuilder prologue(forStmt);
  // DMAs for write regions are going to be inserted just after the for loop.
  MLFuncBuilder epilogue(forStmt->getBlock(),
                         std::next(StmtBlock::iterator(forStmt)));
  MLFuncBuilder *b = region.isWrite() ? &epilogue : &prologue;

  // Builder to create constants at the top level.
  MLFuncBuilder top(forStmt->findFunction());

  FlatAffineConstraints *cst =
      const_cast<FlatAffineConstraints *>(region.getConstraints());

  auto loc = forStmt->getLoc();
  auto *memref = region.memref;
  auto memRefType = memref->getType().cast<MemRefType>();

  // Indices to use for DmaStart op.
  SmallVector<SSAValue *, 4> srcIndices, destIndices;

  SSAValue *zeroIndex = top.create<ConstantIndexOp>(loc, 0);

  unsigned rank = memRefType.getRank();
  SmallVector<int, 4> shape;

  // Compute the extents of the buffer.
  Optional<int64_t> numElements = region.getConstantSize();
  if (!numElements.hasValue()) {
    LLVM_DEBUG(llvm::dbgs() << "Non-constant region size\n");
    return false;
  }

  if (numElements.getValue() == 0) {
    LLVM_DEBUG(llvm::dbgs() << "Nothing to DMA\n");
    return false;
  }

  region.getConstantShape(&shape);

  // Index start offsets for faster memory buffer relative to the original.
  SmallVector<AffineExpr, 4> offsets;
  offsets.reserve(rank);
  for (unsigned d = 0; d < rank; d++) {
    unsigned lbPos;
    cst->getConstantBoundDifference(d, &lbPos);

    // Construct the index expressions for the fast memory buffer. The index
    // expression for a particular dimension of the fast buffer is obtained by
    // subtracting out the lower bound on the original memref's data region
    // along the corresponding dimension.
    AffineExpr offset = top.getAffineConstantExpr(0);
    for (unsigned j = rank; j < cst->getNumCols() - 1; j++) {
      offset = offset - cst->atIneq(lbPos, j) * top.getAffineDimExpr(j - rank);
    }
    offset = offset - cst->atIneq(lbPos, cst->getNumCols() - 1);
    offsets.push_back(offset);

    auto ids = cst->getIds();
    SmallVector<SSAValue *, 8> operands;
    for (unsigned i = rank, e = ids.size(); i < e; i++) {
      auto id = cst->getIds()[i];
      assert(id.hasValue());
      operands.push_back(id.getValue());
    }
    // Set DMA start location for this dimension in the lower memory space
    // memref.
    if (auto caf = offsets[d].dyn_cast<AffineConstantExpr>()) {
      srcIndices.push_back(cast<MLValue>(
          top.create<ConstantIndexOp>(loc, caf.getValue())->getResult()));
    } else {
      auto map =
          top.getAffineMap(cst->getNumDimIds() + cst->getNumSymbolIds() - rank,
                           0, offsets[d], {});
      srcIndices.push_back(cast<MLValue>(
          b->create<AffineApplyOp>(loc, map, operands)->getResult(0)));
    }
    // The fast buffer is DMAed into at location zero; addressing is relative.
    destIndices.push_back(zeroIndex);
  }

  SSAValue *fastMemRef;

  // Check if a buffer was already created.
  // TODO(bondhugula): union across all memory op's per buffer. For now assuming
  // that multiple memory op's on the same memref have the *same* memory
  // footprint.
  if (fastBufferMap.find(memref) == fastBufferMap.end()) {
    auto fastMemRefType = top.getMemRefType(shape, memRefType.getElementType(),
                                            {}, fastMemorySpace);

    LLVM_DEBUG(llvm::dbgs() << "Creating a new buffer of type: ");
    LLVM_DEBUG(fastMemRefType.dump(); llvm::dbgs() << "\n");

    // Create the fast memory space buffer just before the 'for' statement.
    fastMemRef = prologue.create<AllocOp>(loc, fastMemRefType)->getResult();
    // Record it.
    fastBufferMap[memref] = fastMemRef;
  } else {
    // Reuse the one already created.
    fastMemRef = fastBufferMap[memref];
  }
  // Create a tag (single element 1-d memref) for the DMA.
  auto tagMemRefType = top.getMemRefType({1}, top.getIntegerType(32));
  auto tagMemRef = prologue.create<AllocOp>(loc, tagMemRefType);
  auto numElementsSSA =
      top.create<ConstantIndexOp>(loc, numElements.getValue());

  // TODO(bondhugula): check for transfer sizes not being a multiple of
  // minDmaTransferSize and handle them appropriately.

  // TODO(bondhugula): Need to use strided DMA for multi-dimensional (>= 2-d)
  // case.

  if (!region.isWrite()) {
    b->create<DmaStartOp>(loc, memref, srcIndices, fastMemRef, destIndices,
                          numElementsSSA, tagMemRef, zeroIndex);
  } else {
    // dest and src is switched for the writes (since DMA is from the faster
    // memory space to the slower one).
    b->create<DmaStartOp>(loc, fastMemRef, destIndices, memref, srcIndices,
                          numElementsSSA, tagMemRef, zeroIndex);
  }

  // Matching DMA wait to block on completion; tag always has a 0 index.
  b->create<DmaWaitOp>(loc, tagMemRef, zeroIndex, numElementsSSA);

  // Replace all uses of the old memref with the faster one while remapping
  // access indices (subtracting out lower bound offsets for each dimension).
  SmallVector<AffineExpr, 4> remapExprs;
  remapExprs.reserve(rank);
  for (unsigned i = 0; i < rank; i++) {
    auto dim = b->getAffineDimExpr(i);
    remapExprs.push_back(dim - offsets[i]);
  }
  auto indexRemap = b->getAffineMap(rank, 0, remapExprs, {});
  // *Only* those uses within the body of 'forStmt' are replaced.
  replaceAllMemRefUsesWith(memref, cast<MLValue>(fastMemRef), {}, indexRemap,
                           &*forStmt->begin());
  return true;
}

/// Returns the nesting depth of this statement, i.e., the number of loops
/// surrounding this statement.
// TODO(bondhugula): move this to utilities later.
static unsigned getNestingDepth(const Statement &stmt) {
  const Statement *currStmt = &stmt;
  unsigned depth = 0;
  while ((currStmt = currStmt->getParentStmt())) {
    if (isa<ForStmt>(currStmt))
      depth++;
  }
  return depth;
}

// TODO(bondhugula): make this run on a StmtBlock instead of a 'for' stmt.
void DmaGeneration::runOnForStmt(ForStmt *forStmt) {
  // For now (for testing purposes), we'll run this on the outermost among 'for'
  // stmt's with unit stride, i.e., right at the top of the tile if tiling has
  // been done. In the future, the DMA generation has to be done at a level
  // where the generated data fits in a higher level of the memory hierarchy; so
  // the pass has to be instantiated with additional information that we aren't
  // provided with at the moment.
  if (forStmt->getStep() != 1) {
    if (auto *innerFor = dyn_cast<ForStmt>(&*forStmt->begin())) {
      runOnForStmt(innerFor);
    }
    return;
  }

  // DMAs will be generated for this depth, i.e., for all data accessed by this
  // loop.
  dmaDepth = getNestingDepth(*forStmt);

  regions.clear();
  fastBufferMap.clear();

  // Walk this 'for' statement to gather all memory regions.
  walk(forStmt);

  for (const auto &region : regions) {
    generateDma(*region, forStmt);
  }
}

PassResult DmaGeneration::runOnMLFunction(MLFunction *f) {
  for (auto &stmt : *f) {
    if (auto *forStmt = dyn_cast<ForStmt>(&stmt)) {
      runOnForStmt(forStmt);
    }
  }
  // This function never leaves the IR in an invalid state.
  return success();
}

static PassRegistration<DmaGeneration>
    pass("dma-generate", "Generate DMAs for memory operations");