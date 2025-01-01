/*-------------------------------------------------------------------------
 * llvmjit.h
 *	  LLVM JIT provider.
 *
 * Copyright (c) 2016-2025, PostgreSQL Global Development Group
 *
 * src/include/jit/llvmjit.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LLVMJIT_H
#define LLVMJIT_H

/*
 * To avoid breaking cpluspluscheck, allow including the file even when LLVM
 * is not available.
 */
#ifdef USE_LLVM

#include "jit/llvmjit_backport.h"

#include <llvm-c/Types.h>
#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
#include <llvm-c/OrcEE.h>
#endif


/*
 * File needs to be includable by both C and C++ code, and include other
 * headers doing the same. Therefore wrap C portion in our own extern "C" if
 * in C++ mode.
 */
#ifdef __cplusplus
extern "C"
{
#endif

#include "access/tupdesc.h"
#include "fmgr.h"
#include "jit/jit.h"
#include "nodes/pg_list.h"

typedef struct LLVMJitContext
{
	JitContext	base;

	/* used to ensure cleanup of context */
	ResourceOwner resowner;

	/* number of modules created */
	size_t		module_generation;

	/*
	 * The LLVM Context used by this JIT context. An LLVM context is reused
	 * across many compilations, but occasionally reset to prevent it using
	 * too much memory due to more and more types accumulating.
	 */
	LLVMContextRef llvm_context;

	/* current, "open for write", module */
	LLVMModuleRef module;

	/* is there any pending code that needs to be emitted */
	bool		compiled;

	/* # of objects emitted, used to generate non-conflicting names */
	int			counter;

	/* list of handles for code emitted via Orc */
	List	   *handles;
} LLVMJitContext;

/* type and struct definitions */
extern PGDLLIMPORT LLVMTypeRef TypeParamBool;
extern PGDLLIMPORT LLVMTypeRef TypePGFunction;
extern PGDLLIMPORT LLVMTypeRef TypeSizeT;
extern PGDLLIMPORT LLVMTypeRef TypeStorageBool;

extern PGDLLIMPORT LLVMTypeRef StructNullableDatum;
extern PGDLLIMPORT LLVMTypeRef StructTupleDescData;
extern PGDLLIMPORT LLVMTypeRef StructHeapTupleData;
extern PGDLLIMPORT LLVMTypeRef StructHeapTupleHeaderData;
extern PGDLLIMPORT LLVMTypeRef StructMinimalTupleData;
extern PGDLLIMPORT LLVMTypeRef StructTupleTableSlot;
extern PGDLLIMPORT LLVMTypeRef StructHeapTupleTableSlot;
extern PGDLLIMPORT LLVMTypeRef StructMinimalTupleTableSlot;
extern PGDLLIMPORT LLVMTypeRef StructMemoryContextData;
extern PGDLLIMPORT LLVMTypeRef StructFunctionCallInfoData;
extern PGDLLIMPORT LLVMTypeRef StructExprContext;
extern PGDLLIMPORT LLVMTypeRef StructExprEvalStep;
extern PGDLLIMPORT LLVMTypeRef StructExprState;
extern PGDLLIMPORT LLVMTypeRef StructAggState;
extern PGDLLIMPORT LLVMTypeRef StructAggStatePerTransData;
extern PGDLLIMPORT LLVMTypeRef StructAggStatePerGroupData;
extern PGDLLIMPORT LLVMTypeRef StructPlanState;

extern PGDLLIMPORT LLVMValueRef AttributeTemplate;
extern PGDLLIMPORT LLVMValueRef ExecEvalBoolSubroutineTemplate;
extern PGDLLIMPORT LLVMValueRef ExecEvalSubroutineTemplate;


extern void llvm_enter_fatal_on_oom(void);
extern void llvm_leave_fatal_on_oom(void);
extern bool llvm_in_fatal_on_oom(void);
extern void llvm_reset_after_error(void);
extern void llvm_assert_in_fatal_section(void);

extern LLVMJitContext *llvm_create_context(int jitFlags);
extern LLVMModuleRef llvm_mutable_module(LLVMJitContext *context);
extern char *llvm_expand_funcname(LLVMJitContext *context, const char *basename);
extern void *llvm_get_function(LLVMJitContext *context, const char *funcname);
extern void llvm_split_symbol_name(const char *name, char **modname, char **funcname);
extern LLVMTypeRef llvm_pg_var_type(const char *varname);
extern LLVMTypeRef llvm_pg_var_func_type(const char *varname);
extern LLVMValueRef llvm_pg_func(LLVMModuleRef mod, const char *funcname);
extern void llvm_copy_attributes(LLVMValueRef from, LLVMValueRef to);
extern LLVMValueRef llvm_function_reference(LLVMJitContext *context,
						LLVMBuilderRef builder,
						LLVMModuleRef mod,
						FunctionCallInfo fcinfo);

extern void llvm_inline_reset_caches(void);
extern void llvm_inline(LLVMModuleRef mod);

/*
 ****************************************************************************
 * Code generation functions.
 ****************************************************************************
 */
extern bool llvm_compile_expr(struct ExprState *state);
struct TupleTableSlotOps;
extern LLVMValueRef slot_compile_deform(struct LLVMJitContext *context, TupleDesc desc,
										const struct TupleTableSlotOps *ops, int natts);

/*
 ****************************************************************************
 * Extensions / Backward compatibility section of the LLVM C API
 * Error handling related functions.
 ****************************************************************************
 */
extern LLVMTypeRef LLVMGetFunctionReturnType(LLVMValueRef r);
extern LLVMTypeRef LLVMGetFunctionType(LLVMValueRef r);
#ifdef USE_LLVM_BACKPORT_SECTION_MEMORY_MANAGER
extern LLVMOrcObjectLayerRef LLVMOrcCreateRTDyldObjectLinkingLayerWithSafeSectionMemoryManager(LLVMOrcExecutionSessionRef ES);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif							/* USE_LLVM */
#endif							/* LLVMJIT_H */
