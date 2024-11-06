/*-------------------------------------------------------------------------
 *
 * llvmjit_wrap.cpp
 *	  Parts of the LLVM interface not (yet) exposed to C.
 *
 * Copyright (c) 2016-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/lib/llvm/llvmjit_wrap.cpp
 *
 *-------------------------------------------------------------------------
 */

extern "C"
{
#include "postgres.h"
}

#include <llvm-c/Core.h>

/* Avoid macro clash with LLVM's C++ headers */
#undef Min

#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#if LLVM_VERSION_MAJOR < 17
#include <llvm/MC/SubtargetFeature.h>
#endif
#if LLVM_VERSION_MAJOR > 16
#include <llvm/TargetParser/Host.h>
#else
#include <llvm/Support/Host.h>
#endif

#include "jit/llvmjit.h"
#include "jit/llvmjit_backport.h"

#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
#include <llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/SectionMemoryManager.h>
#include "jit/SectionMemoryManager.h"
#include <llvm/Support/CBindingWrapping.h>
#endif


/*
 * C-API extensions.
 */
#if defined(HAVE_DECL_LLVMGETHOSTCPUNAME) && !HAVE_DECL_LLVMGETHOSTCPUNAME
char *LLVMGetHostCPUName(void) {
	return strdup(llvm::sys::getHostCPUName().data());
}
#endif


#if defined(HAVE_DECL_LLVMGETHOSTCPUFEATURES) && !HAVE_DECL_LLVMGETHOSTCPUFEATURES
char *LLVMGetHostCPUFeatures(void) {
	llvm::SubtargetFeatures Features;
	llvm::StringMap<bool> HostFeatures;

	if (llvm::sys::getHostCPUFeatures(HostFeatures))
		for (auto &F : HostFeatures)
			Features.AddFeature(F.first(), F.second);

	return strdup(Features.getString().c_str());
}
#endif

/*
 * Like LLVM's LLVMGetAttributeCountAtIndex(), works around a bug in LLVM 3.9.
 *
 * In LLVM <= 3.9, LLVMGetAttributeCountAtIndex() segfaults if there are no
 * attributes at an index (fixed in LLVM commit ce9bb1097dc2).
 */
unsigned
LLVMGetAttributeCountAtIndexPG(LLVMValueRef F, uint32 Idx)
{
	/*
	 * This is more expensive, so only do when using a problematic LLVM
	 * version.
	 */
#if LLVM_VERSION_MAJOR < 4
	if (!llvm::unwrap<llvm::Function>(F)->getAttributes().hasAttributes(Idx))
		return 0;
#endif

	/*
	 * There is no nice public API to determine the count nicely, so just
	 * always fall back to LLVM's C API.
	 */
	return LLVMGetAttributeCountAtIndex(F, Idx);
}

LLVMTypeRef
LLVMGetFunctionReturnType(LLVMValueRef r)
{
	return llvm::wrap(llvm::unwrap<llvm::Function>(r)->getReturnType());
}

LLVMTypeRef
LLVMGetFunctionType(LLVMValueRef r)
{
	return llvm::wrap(llvm::unwrap<llvm::Function>(r)->getFunctionType());
}

#if LLVM_VERSION_MAJOR < 8
LLVMTypeRef
LLVMGlobalGetValueType(LLVMValueRef g)
{
	return llvm::wrap(llvm::unwrap<llvm::GlobalValue>(g)->getValueType());
}
#endif

#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(llvm::orc::ExecutionSession, LLVMOrcExecutionSessionRef)
DEFINE_SIMPLE_CONVERSION_FUNCTIONS(llvm::orc::ObjectLayer, LLVMOrcObjectLayerRef);

LLVMOrcObjectLayerRef
LLVMOrcCreateRTDyldObjectLinkingLayerWithSafeSectionMemoryManager(LLVMOrcExecutionSessionRef ES)
{
	return wrap(new llvm::orc::RTDyldObjectLinkingLayer(
		*unwrap(ES), [] { return std::make_unique<llvm::backport::SectionMemoryManager>(nullptr, true); }));
}
#endif
