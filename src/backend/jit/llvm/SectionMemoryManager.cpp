/*
 * This file is from https://github.com/llvm/llvm-project/pull/71968
 * with minor modifications to avoid name clash and work with older
 * LLVM versions.  The llvm::backport::SectionMemoryManager class is a
 * drop-in replacement for llvm::SectionMemoryManager, for use with
 * llvm::RuntimeDyld.  It fixes a memory layout bug on large memory
 * ARM systems (see pull request for details).  If the LLVM project
 * eventually commits the change, we may need to resynchronize our
 * copy with any further modifications, but they would be unlikely to
 * backport it into the LLVM versions that we target so we would still
 * need this copy.
 *
 * In the future we will switch to using JITLink instead of
 * RuntimeDyld where possible, and later remove this code (.cpp, .h,
 * .LICENSE) after all LLVM versions that we target allow it.
 *
 * This file is a modified copy of a part of the LLVM source code that
 * we would normally access from the LLVM library.  It is therefore
 * covered by the license at https://llvm.org/LICENSE.txt, reproduced
 * verbatim in SectionMemoryManager.LICENSE in fulfillment of clause
 * 4a.  The bugfix changes from the pull request are also covered, per
 * clause 5.
 */

//===- SectionMemoryManager.cpp - Memory manager for MCJIT/RtDyld *- C++ -*-==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the section-based memory manager used by the MCJIT
// execution engine and RuntimeDyld
//
//===----------------------------------------------------------------------===//

#include "jit/llvmjit_backport.h"

#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER

#include "jit/SectionMemoryManager.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Process.h"

namespace llvm {
namespace backport {

bool SectionMemoryManager::hasSpace(const MemoryGroup &MemGroup,
                                    uintptr_t Size) const {
  for (const FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    if (FreeMB.Free.allocatedSize() >= Size)
      return true;
  }
  return false;
}

#if LLVM_VERSION_MAJOR < 16
void SectionMemoryManager::reserveAllocationSpace(uintptr_t CodeSize,
                                                  uint32_t CodeAlign_i,
                                                  uintptr_t RODataSize,
                                                  uint32_t RODataAlign_i,
                                                  uintptr_t RWDataSize,
                                                  uint32_t RWDataAlign_i) {
  Align CodeAlign(CodeAlign_i);
  Align RODataAlign(RODataAlign_i);
  Align RWDataAlign(RWDataAlign_i);
#else
void SectionMemoryManager::reserveAllocationSpace(
    uintptr_t CodeSize, Align CodeAlign, uintptr_t RODataSize,
    Align RODataAlign, uintptr_t RWDataSize, Align RWDataAlign) {
#endif
  if (CodeSize == 0 && RODataSize == 0 && RWDataSize == 0)
    return;

  static const size_t PageSize = sys::Process::getPageSizeEstimate();

  // Code alignment needs to be at least the stub alignment - however, we
  // don't have an easy way to get that here so as a workaround, we assume
  // it's 8, which is the largest value I observed across all platforms.
  constexpr uint64_t StubAlign = 8;
  CodeAlign = Align(std::max(CodeAlign.value(), StubAlign));
  RODataAlign = Align(std::max(RODataAlign.value(), StubAlign));
  RWDataAlign = Align(std::max(RWDataAlign.value(), StubAlign));

  // Get space required for each section. Use the same calculation as
  // allocateSection because we need to be able to satisfy it.
  uint64_t RequiredCodeSize = alignTo(CodeSize, CodeAlign) + CodeAlign.value();
  uint64_t RequiredRODataSize =
      alignTo(RODataSize, RODataAlign) + RODataAlign.value();
  uint64_t RequiredRWDataSize =
      alignTo(RWDataSize, RWDataAlign) + RWDataAlign.value();

  if (hasSpace(CodeMem, RequiredCodeSize) &&
      hasSpace(RODataMem, RequiredRODataSize) &&
      hasSpace(RWDataMem, RequiredRWDataSize)) {
    // Sufficient space in contiguous block already available.
    return;
  }

  // MemoryManager does not have functions for releasing memory after it's
  // allocated. Normally it tries to use any excess blocks that were allocated
  // due to page alignment, but if we have insufficient free memory for the
  // request this can lead to allocating disparate memory that can violate the
  // ARM ABI. Clear free memory so only the new allocations are used, but do
  // not release allocated memory as it may still be in-use.
  CodeMem.FreeMem.clear();
  RODataMem.FreeMem.clear();
  RWDataMem.FreeMem.clear();

  // Round up to the nearest page size. Blocks must be page-aligned.
  RequiredCodeSize = alignTo(RequiredCodeSize, PageSize);
  RequiredRODataSize = alignTo(RequiredRODataSize, PageSize);
  RequiredRWDataSize = alignTo(RequiredRWDataSize, PageSize);
  uint64_t RequiredSize =
      RequiredCodeSize + RequiredRODataSize + RequiredRWDataSize;

  std::error_code ec;
  sys::MemoryBlock MB = MMapper->allocateMappedMemory(
      AllocationPurpose::RWData, RequiredSize, nullptr,
      sys::Memory::MF_READ | sys::Memory::MF_WRITE, ec);
  if (ec) {
    return;
  }
  // CodeMem will arbitrarily own this MemoryBlock to handle cleanup.
  CodeMem.AllocatedMem.push_back(MB);
  uintptr_t Addr = (uintptr_t)MB.base();
  FreeMemBlock FreeMB;
  FreeMB.PendingPrefixIndex = (unsigned)-1;

  if (CodeSize > 0) {
    assert(isAddrAligned(CodeAlign, (void *)Addr));
    FreeMB.Free = sys::MemoryBlock((void *)Addr, RequiredCodeSize);
    CodeMem.FreeMem.push_back(FreeMB);
    Addr += RequiredCodeSize;
  }

  if (RODataSize > 0) {
    assert(isAddrAligned(RODataAlign, (void *)Addr));
    FreeMB.Free = sys::MemoryBlock((void *)Addr, RequiredRODataSize);
    RODataMem.FreeMem.push_back(FreeMB);
    Addr += RequiredRODataSize;
  }

  if (RWDataSize > 0) {
    assert(isAddrAligned(RWDataAlign, (void *)Addr));
    FreeMB.Free = sys::MemoryBlock((void *)Addr, RequiredRWDataSize);
    RWDataMem.FreeMem.push_back(FreeMB);
  }
}

uint8_t *SectionMemoryManager::allocateDataSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID,
                                                   StringRef SectionName,
                                                   bool IsReadOnly) {
  if (IsReadOnly)
    return allocateSection(SectionMemoryManager::AllocationPurpose::ROData,
                           Size, Alignment);
  return allocateSection(SectionMemoryManager::AllocationPurpose::RWData, Size,
                         Alignment);
}

uint8_t *SectionMemoryManager::allocateCodeSection(uintptr_t Size,
                                                   unsigned Alignment,
                                                   unsigned SectionID,
                                                   StringRef SectionName) {
  return allocateSection(SectionMemoryManager::AllocationPurpose::Code, Size,
                         Alignment);
}

uint8_t *SectionMemoryManager::allocateSection(
    SectionMemoryManager::AllocationPurpose Purpose, uintptr_t Size,
    unsigned Alignment) {
  if (!Alignment)
    Alignment = 16;

  assert(!(Alignment & (Alignment - 1)) && "Alignment must be a power of two.");

  uintptr_t RequiredSize = Alignment * ((Size + Alignment - 1) / Alignment + 1);
  uintptr_t Addr = 0;

  MemoryGroup &MemGroup = [&]() -> MemoryGroup & {
    switch (Purpose) {
    case AllocationPurpose::Code:
      return CodeMem;
    case AllocationPurpose::ROData:
      return RODataMem;
    case AllocationPurpose::RWData:
      return RWDataMem;
    }
    llvm_unreachable("Unknown SectionMemoryManager::AllocationPurpose");
  }();

  // Look in the list of free memory regions and use a block there if one
  // is available.
  for (FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    if (FreeMB.Free.allocatedSize() >= RequiredSize) {
      Addr = (uintptr_t)FreeMB.Free.base();
      uintptr_t EndOfBlock = Addr + FreeMB.Free.allocatedSize();
      // Align the address.
      Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

      if (FreeMB.PendingPrefixIndex == (unsigned)-1) {
        // The part of the block we're giving out to the user is now pending
        MemGroup.PendingMem.push_back(sys::MemoryBlock((void *)Addr, Size));

        // Remember this pending block, such that future allocations can just
        // modify it rather than creating a new one
        FreeMB.PendingPrefixIndex = MemGroup.PendingMem.size() - 1;
      } else {
        sys::MemoryBlock &PendingMB =
            MemGroup.PendingMem[FreeMB.PendingPrefixIndex];
        PendingMB = sys::MemoryBlock(PendingMB.base(),
                                     Addr + Size - (uintptr_t)PendingMB.base());
      }

      // Remember how much free space is now left in this block
      FreeMB.Free =
          sys::MemoryBlock((void *)(Addr + Size), EndOfBlock - Addr - Size);
      return (uint8_t *)Addr;
    }
  }

  // No pre-allocated free block was large enough. Allocate a new memory region.
  // Note that all sections get allocated as read-write.  The permissions will
  // be updated later based on memory group.
  //
  // FIXME: It would be useful to define a default allocation size (or add
  // it as a constructor parameter) to minimize the number of allocations.
  //
  // FIXME: Initialize the Near member for each memory group to avoid
  // interleaving.
  std::error_code ec;
  sys::MemoryBlock MB = MMapper->allocateMappedMemory(
      Purpose, RequiredSize, &MemGroup.Near,
      sys::Memory::MF_READ | sys::Memory::MF_WRITE, ec);
  if (ec) {
    // FIXME: Add error propagation to the interface.
    return nullptr;
  }

  // Save this address as the basis for our next request
  MemGroup.Near = MB;

  // Copy the address to all the other groups, if they have not
  // been initialized.
  if (CodeMem.Near.base() == nullptr)
    CodeMem.Near = MB;
  if (RODataMem.Near.base() == nullptr)
    RODataMem.Near = MB;
  if (RWDataMem.Near.base() == nullptr)
    RWDataMem.Near = MB;

  // Remember that we allocated this memory
  MemGroup.AllocatedMem.push_back(MB);
  Addr = (uintptr_t)MB.base();
  uintptr_t EndOfBlock = Addr + MB.allocatedSize();

  // Align the address.
  Addr = (Addr + Alignment - 1) & ~(uintptr_t)(Alignment - 1);

  // The part of the block we're giving out to the user is now pending
  MemGroup.PendingMem.push_back(sys::MemoryBlock((void *)Addr, Size));

  // The allocateMappedMemory may allocate much more memory than we need. In
  // this case, we store the unused memory as a free memory block.
  unsigned FreeSize = EndOfBlock - Addr - Size;
  if (FreeSize > 16) {
    FreeMemBlock FreeMB;
    FreeMB.Free = sys::MemoryBlock((void *)(Addr + Size), FreeSize);
    FreeMB.PendingPrefixIndex = (unsigned)-1;
    MemGroup.FreeMem.push_back(FreeMB);
  }

  // Return aligned address
  return (uint8_t *)Addr;
}

bool SectionMemoryManager::finalizeMemory(std::string *ErrMsg) {
  // FIXME: Should in-progress permissions be reverted if an error occurs?
  std::error_code ec;

  // Make code memory executable.
  ec = applyMemoryGroupPermissions(CodeMem,
                                   sys::Memory::MF_READ | sys::Memory::MF_EXEC);
  if (ec) {
    if (ErrMsg) {
      *ErrMsg = ec.message();
    }
    return true;
  }

  // Make read-only data memory read-only.
  ec = applyMemoryGroupPermissions(RODataMem, sys::Memory::MF_READ);
  if (ec) {
    if (ErrMsg) {
      *ErrMsg = ec.message();
    }
    return true;
  }

  // Read-write data memory already has the correct permissions

  // Some platforms with separate data cache and instruction cache require
  // explicit cache flush, otherwise JIT code manipulations (like resolved
  // relocations) will get to the data cache but not to the instruction cache.
  invalidateInstructionCache();

  return false;
}

static sys::MemoryBlock trimBlockToPageSize(sys::MemoryBlock M) {
  static const size_t PageSize = sys::Process::getPageSizeEstimate();

  size_t StartOverlap =
      (PageSize - ((uintptr_t)M.base() % PageSize)) % PageSize;

  size_t TrimmedSize = M.allocatedSize();
  TrimmedSize -= StartOverlap;
  TrimmedSize -= TrimmedSize % PageSize;

  sys::MemoryBlock Trimmed((void *)((uintptr_t)M.base() + StartOverlap),
                           TrimmedSize);

  assert(((uintptr_t)Trimmed.base() % PageSize) == 0);
  assert((Trimmed.allocatedSize() % PageSize) == 0);
  assert(M.base() <= Trimmed.base() &&
         Trimmed.allocatedSize() <= M.allocatedSize());

  return Trimmed;
}

std::error_code
SectionMemoryManager::applyMemoryGroupPermissions(MemoryGroup &MemGroup,
                                                  unsigned Permissions) {
  for (sys::MemoryBlock &MB : MemGroup.PendingMem)
    if (std::error_code EC = MMapper->protectMappedMemory(MB, Permissions))
      return EC;

  MemGroup.PendingMem.clear();

  // Now go through free blocks and trim any of them that don't span the entire
  // page because one of the pending blocks may have overlapped it.
  for (FreeMemBlock &FreeMB : MemGroup.FreeMem) {
    FreeMB.Free = trimBlockToPageSize(FreeMB.Free);
    // We cleared the PendingMem list, so all these pointers are now invalid
    FreeMB.PendingPrefixIndex = (unsigned)-1;
  }

  // Remove all blocks which are now empty
  erase_if(MemGroup.FreeMem, [](FreeMemBlock &FreeMB) {
    return FreeMB.Free.allocatedSize() == 0;
  });

  return std::error_code();
}

void SectionMemoryManager::invalidateInstructionCache() {
  for (sys::MemoryBlock &Block : CodeMem.PendingMem)
    sys::Memory::InvalidateInstructionCache(Block.base(),
                                            Block.allocatedSize());
}

SectionMemoryManager::~SectionMemoryManager() {
  for (MemoryGroup *Group : {&CodeMem, &RWDataMem, &RODataMem}) {
    for (sys::MemoryBlock &Block : Group->AllocatedMem)
      MMapper->releaseMappedMemory(Block);
  }
}

SectionMemoryManager::MemoryMapper::~MemoryMapper() = default;

void SectionMemoryManager::anchor() {}

namespace {
// Trivial implementation of SectionMemoryManager::MemoryMapper that just calls
// into sys::Memory.
class DefaultMMapper final : public SectionMemoryManager::MemoryMapper {
public:
  sys::MemoryBlock
  allocateMappedMemory(SectionMemoryManager::AllocationPurpose Purpose,
                       size_t NumBytes, const sys::MemoryBlock *const NearBlock,
                       unsigned Flags, std::error_code &EC) override {
    return sys::Memory::allocateMappedMemory(NumBytes, NearBlock, Flags, EC);
  }

  std::error_code protectMappedMemory(const sys::MemoryBlock &Block,
                                      unsigned Flags) override {
    return sys::Memory::protectMappedMemory(Block, Flags);
  }

  std::error_code releaseMappedMemory(sys::MemoryBlock &M) override {
    return sys::Memory::releaseMappedMemory(M);
  }
};
} // namespace

SectionMemoryManager::SectionMemoryManager(MemoryMapper *UnownedMM,
                                           bool ReserveAlloc)
    : MMapper(UnownedMM), OwnedMMapper(nullptr),
      ReserveAllocation(ReserveAlloc) {
  if (!MMapper) {
    OwnedMMapper = std::make_unique<DefaultMMapper>();
    MMapper = OwnedMMapper.get();
  }
}

} // namespace backport
} // namespace llvm

#endif
