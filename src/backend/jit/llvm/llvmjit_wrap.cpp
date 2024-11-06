/*-------------------------------------------------------------------------
 *
 * llvmjit_wrap.cpp
 *	  Parts of the LLVM interface not (yet) exposed to C.
 *
 * Copyright (c) 2016-2024, PostgreSQL Global Development Group
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
#include <llvm/IR/Function.h>

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
