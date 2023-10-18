/*
 * llvmjit_emit.h
 *	  Helpers to make emitting LLVM IR a bit more concise and pgindent proof.
 *
 * Copyright (c) 2018-2019, PostgreSQL Global Development Group
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
#include <llvm-c/Target.h>

#include "fmgr.h"
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

static inline LLVMValueRef
l_struct_gep(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, int32 idx, const char *name)
{
#if LLVM_VERSION_MAJOR < 16
	return LLVMBuildStructGEP(b, v, idx, "");
#else
	return LLVMBuildStructGEP2(b, t, v, idx, "");
#endif
}

static inline LLVMValueRef
l_gep(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, LLVMValueRef *indices, int32 nindices, const char *name)
{
#if LLVM_VERSION_MAJOR < 16
	return LLVMBuildGEP(b, v, indices, nindices, name);
#else
	return LLVMBuildGEP2(b, t, v, indices, nindices, name);
#endif
}

static inline LLVMValueRef
l_load(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, const char *name)
{
#if LLVM_VERSION_MAJOR < 16
	return LLVMBuildLoad(b, v, name);
#else
	return LLVMBuildLoad2(b, t, v, name);
#endif
}

static inline LLVMValueRef
l_call(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef fn, LLVMValueRef *args, int32 nargs, const char *name)
{
#if LLVM_VERSION_MAJOR < 16
	return LLVMBuildCall(b, fn, args, nargs, name);
#else
	return LLVMBuildCall2(b, t, fn, args, nargs, name);
#endif
}

/*
 * Load a pointer member idx from a struct.
 */
static inline LLVMValueRef
l_load_struct_gep(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, int32 idx, const char *name)
{
	return l_load(b,
				  LLVMStructGetTypeAtIndex(t, idx),
				  l_struct_gep(b, t, v, idx, ""),
				  name);
}

/*
 * Load value of a pointer, after applying one index operation.
 */
static inline LLVMValueRef
l_load_gep1(LLVMBuilderRef b, LLVMTypeRef t, LLVMValueRef v, LLVMValueRef idx, const char *name)
{
	return l_load(b, t, l_gep(b, t, v, &idx, 1, ""), name);
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
	ret = l_load(b, l_ptr(StructMemoryContextData), cur, cmc);
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

	v_args = l_struct_gep(b,
						  StructFunctionCallInfoData,
						  v_fcinfo,
						  FIELDNO_FUNCTIONCALLINFODATA_ARGS,
						  "");
	v_argn = l_struct_gep(b,
						  LLVMArrayType(StructNullableDatum, 0),
						  v_args,
						  argno,
						  "");
	return l_struct_gep(b,
						StructNullableDatum,
						v_argn,
						FIELDNO_NULLABLE_DATUM_ISNULL,
						"");
}

/*
 * Return pointer to the argno'th argument datum.
 */
static inline LLVMValueRef
l_funcvaluep(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno)
{
	LLVMValueRef v_args;
	LLVMValueRef v_argn;

	v_args = l_struct_gep(b,
						  StructFunctionCallInfoData,
						  v_fcinfo,
						  FIELDNO_FUNCTIONCALLINFODATA_ARGS,
						  "");
	v_argn = l_struct_gep(b,
						  LLVMArrayType(StructNullableDatum, 0),
						  v_args,
						  argno,
						  "");
	return l_struct_gep(b,
						StructNullableDatum,
						v_argn,
						FIELDNO_NULLABLE_DATUM_DATUM,
						"");
}

/*
 * Return argno'th argument nullness.
 */
static inline LLVMValueRef
l_funcnull(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno)
{
	return l_load(b, TypeStorageBool, l_funcnullp(b, v_fcinfo, argno), "");
}

/*
 * Return argno'th argument datum.
 */
static inline LLVMValueRef
l_funcvalue(LLVMBuilderRef b, LLVMValueRef v_fcinfo, size_t argno)
{
	return l_load(b, TypeSizeT, l_funcvaluep(b, v_fcinfo, argno), "");
}

#endif							/* USE_LLVM */
#endif
