/*
 * llvmjit_emit.h
 *	  Helpers to make emitting LLVM IR a bit more concise and pgindent proof.
 *
 * Copyright (c) 2018-2020, PostgreSQL Global Development Group
 *
 * src/include/lib/llvmjit_emit.h
 */
#ifndef LLVMJIT_EMIT_H
#define LLVMJIT_EMIT_H

/*
 * To avoid breaking cpluspluscheck, allow including the file even when LLVM
 * is not available.
 */
#ifdef USE_LLVM

#include <llvm-c/Core.h>

#include "jit/llvmjit.h"


/*
 * Emit a non-LLVM pointer as an LLVM constant.
 */
static inline LLVMValueRef
l_ptr_const(void *ptr, LLVMTypeRef type)
{
	LLVMValueRef c = LLVMConstInt(TypeSizeT, (uintptr_t) ptr, false);

	return LLVMConstIntToPtr(c, type);
}

/*
 * Emit pointer.
 */
static inline LLVMTypeRef
l_ptr(LLVMTypeRef t)
{
	return LLVMPointerType(t, 0);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_int8_const(int8 i)
{
	return LLVMConstInt(LLVMInt8Type(), i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_int16_const(int16 i)
{
	return LLVMConstInt(LLVMInt16Type(), i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_int32_const(int32 i)
{
	return LLVMConstInt(LLVMInt32Type(), i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_int64_const(int64 i)
{
	return LLVMConstInt(LLVMInt64Type(), i, false);
}

/*
 * Emit constant integer.
 */
static inline LLVMValueRef
l_sizet_const(size_t i)
{
	return LLVMConstInt(TypeSizeT, i, false);
}

/*
 * Emit constant boolean, as used for storage (e.g. global vars, structs).
 */
static inline LLVMValueRef
l_sbool_const(bool i)
{
	return LLVMConstInt(TypeStorageBool, (int) i, false);
}

/*
 * Emit constant boolean, as used for parameters (e.g. function parameters).
 */
static inline LLVMValueRef
l_pbool_const(bool i)
{
	return LLVMConstInt(TypeParamBool, (int) i, false);
}

/*
 * Load a pointer member idx from a struct.
 */
static inline LLVMValueRef
l_load_struct_gep(LLVMBuilderRef b, LLVMValueRef v, int32 idx, const char *name)
{
	LLVMValueRef v_ptr = LLVMBuildStructGEP(b, v, idx, "");

	return LLVMBuildLoad(b, v_ptr, name);
}

/*
 * Load value of a pointer, after applying one index operation.
 */
static inline LLVMValueRef
l_load_gep1(LLVMBuilderRef b, LLVMValueRef v, LLVMValueRef idx, const char *name)
{
	LLVMValueRef v_ptr = LLVMBuildGEP(b, v, &idx, 1, "");

	return LLVMBuildLoad(b, v_ptr, name);
}

/* separate, because pg_attribute_printf(2, 3) can't appear in definition */
static inline LLVMBasicBlockRef l_bb_before_v(LLVMBasicBlockRef r, const char *fmt,...) pg_attribute_printf(2, 3);

/*
 * Insert a new basic block, just before r, the name being determined by fmt
 * and arguments.
 */
static inline LLVMBasicBlockRef
l_bb_before_v(LLVMBasicBlockRef r, const char *fmt,...)
{
	char		buf[512];
	va_list		args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	return LLVMInsertBasicBlock(r, buf);
}

/* separate, because pg_attribute_printf(2, 3) can't appear in definition */
static inline LLVMBasicBlockRef l_bb_append_v(LLVMValueRef f, const char *fmt,...) pg_attribute_printf(2, 3);

/*
 * Insert a new basic block after previous basic blocks, the name being
 * determined by fmt and arguments.
 */
static inline LLVMBasicBlockRef
l_bb_append_v(LLVMValueRef f, const char *fmt,...)
{
	char		buf[512];
	va_list		args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	return LLVMAppendBasicBlock(f, buf);
}

/*
 * Mark a callsite as readonly.
 */
static inline void
l_callsite_ro(LLVMValueRef f)
{
	const char	argname[] = "readonly";
	LLVMAttributeRef ref;

	ref = LLVMCreateStringAttribute(LLVMGetGlobalContext(),
									argname,
									sizeof(argname) - 1,
									NULL, 0);

	LLVMAddCallSiteAttribute(f, LLVMAttributeFunctionIndex, ref);
}

/*
 * Mark a callsite as alwaysinline.
 */
static inline void
l_callsite_alwaysinline(LLVMValueRef f)
{
	const char	argname[] = "alwaysinline";
	int			id;
	LLVMAttributeRef attr;

	id = LLVMGetEnumAttributeKindForName(argname,
										 sizeof(argname) - 1);
	attr = LLVMCreateEnumAttribute(LLVMGetGlobalContext(), id, 0);
	LLVMAddCallSiteAttribute(f, LLVMAttributeFunctionIndex, attr);
}

/*
 * Emit code to switch memory context.
 */
static inline LLVMValueRef
l_mcxt_switch(LLVMModuleRef mod, LLVMBuilderRef b, LLVMValueRef nc)
{
	const char *cmc = "CurrentMemoryContext";
	LLVMValueRef cur;
	LLVMValueRef ret;

	if (!(cur = LLVMGetNamedGlobal(mod, cmc)))
		cur = LLVMAddGlobal(mod, l_ptr(StructMemoryContextData), cmc);
	ret = LLVMBuildLoad(b, cur, cmc);
	LLVMBuildStore(b, nc, cur);

	return ret;
}

/*
 * Return pointer to the argno'th argument nullness.
 */
static inline LLVMValueRef
l_funcnullp(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno)
{
	LLVMValueRef v_args;
	LLVMValueRef v_argn;

	v_args = LLVMBuildStructGEP(b,
								v_fcinfo,
								FIELDNO_FUNCTIONCALLINFODATA_ARGS,
								"");
	v_argn = LLVMBuildStructGEP(b, v_args, argno, "");

	return LLVMBuildStructGEP(b, v_argn, FIELDNO_NULLABLE_DATUM_ISNULL, "");
}

/*
 * Return pointer to the argno'th argument datum.
 */
static inline LLVMValueRef
l_funcvaluep(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno)
{
	LLVMValueRef v_args;
	LLVMValueRef v_argn;

	v_args = LLVMBuildStructGEP(b,
								v_fcinfo,
								FIELDNO_FUNCTIONCALLINFODATA_ARGS,
								"");
	v_argn = LLVMBuildStructGEP(b, v_args, argno, "");

	return LLVMBuildStructGEP(b, v_argn, FIELDNO_NULLABLE_DATUM_DATUM, "");
}

/*
 * Return argno'th argument nullness.
 */
static inline LLVMValueRef
l_funcnull(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno)
{
	return LLVMBuildLoad(b, l_funcnullp(b, v_fcinfo, argno), "");
}

/*
 * Return argno'th argument datum.
 */
static inline LLVMValueRef
l_funcvalue(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno)
{
	return LLVMBuildLoad(b, l_funcvaluep(b, v_fcinfo, argno), "");
}

#endif							/* USE_LLVM */
#endif
