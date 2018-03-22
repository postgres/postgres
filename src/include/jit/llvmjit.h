/*-------------------------------------------------------------------------
 * llvmjit.h
 *	  LLVM JIT provider.
 *
 * Copyright (c) 2016-2018, PostgreSQL Global Development Group
 *
 * src/include/jit/llvmjit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LLVMJIT_H
#define LLVMJIT_H

#ifndef USE_LLVM
#error "llvmjit.h should only be included by code dealing with llvm"
#endif

#include <llvm-c/Types.h>


/*
 * File needs to be includable by both C and C++ code, and include other
 * headers doing the same. Therefore wrap C portion in our own extern "C" if
 * in C++ mode.
 */
#ifdef __cplusplus
extern "C"
{
#endif


#include "jit/jit.h"


typedef struct LLVMJitContext
{
	JitContext	base;
} LLVMJitContext;

extern void llvm_enter_fatal_on_oom(void);
extern void llvm_leave_fatal_on_oom(void);
extern void llvm_reset_after_error(void);
extern void llvm_assert_in_fatal_section(void);

extern LLVMJitContext *llvm_create_context(int jitFlags);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif							/* LLVMJIT_H */
