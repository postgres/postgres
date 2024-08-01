/*-------------------------------------------------------------------------
 *
 * llvmjit_types.c
 *	  List of types needed by JIT emitting code.
 *
 * JIT emitting code often needs to access struct elements, create functions
 * with the correct signature etc. To allow synchronizing these types with a
 * low chance of definitions getting out of sync, this file lists types and
 * functions that directly need to be accessed from LLVM.
 *
 * When LLVM is first used in a backend, a bitcode version of this file will
 * be loaded. The needed types and signatures will be stored into Struct*,
 * Type*, Func* variables.
 *
 * NB: This file will not be linked into the server, it's just converted to
 * bitcode.
 *
 *
 * Copyright (c) 2016-2024, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/jit/llvm/llvmjit_types.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "catalog/pg_attribute.h"
#include "executor/execExpr.h"
#include "executor/nodeAgg.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "nodes/execnodes.h"
#include "nodes/memnodes.h"
#include "utils/expandeddatum.h"
#include "utils/palloc.h"


/*
 * List of types needed for JITing. These have to be non-static, otherwise
 * clang/LLVM will omit them.  As this file will never be linked into
 * anything, that's harmless.
 */
PGFunction	TypePGFunction;
size_t		TypeSizeT;
bool		TypeStorageBool;

ExecEvalSubroutine TypeExecEvalSubroutine;
ExecEvalBoolSubroutine TypeExecEvalBoolSubroutine;

NullableDatum StructNullableDatum;
AggState	StructAggState;
AggStatePerGroupData StructAggStatePerGroupData;
AggStatePerTransData StructAggStatePerTransData;
ExprContext StructExprContext;
ExprEvalStep StructExprEvalStep;
ExprState	StructExprState;
FunctionCallInfoBaseData StructFunctionCallInfoData;
HeapTupleData StructHeapTupleData;
HeapTupleHeaderData StructHeapTupleHeaderData;
MemoryContextData StructMemoryContextData;
TupleTableSlot StructTupleTableSlot;
HeapTupleTableSlot StructHeapTupleTableSlot;
MinimalTupleTableSlot StructMinimalTupleTableSlot;
TupleDescData StructTupleDescData;
PlanState	StructPlanState;
MinimalTupleData StructMinimalTupleData;


/*
 * To determine which attributes functions need to have (depends e.g. on
 * compiler version and settings) to be compatible for inlining, we simply
 * copy the attributes of this function.
 */
extern Datum AttributeTemplate(PG_FUNCTION_ARGS);
Datum
AttributeTemplate(PG_FUNCTION_ARGS)
{
	AssertVariableIsOfType(&AttributeTemplate, PGFunction);

	PG_RETURN_NULL();
}

/*
 * And some more "templates" to give us examples of function types
 * corresponding to function pointer types.
 */

extern void ExecEvalSubroutineTemplate(ExprState *state,
									   struct ExprEvalStep *op,
									   ExprContext *econtext);
void
ExecEvalSubroutineTemplate(ExprState *state,
						   struct ExprEvalStep *op,
						   ExprContext *econtext)
{
	AssertVariableIsOfType(&ExecEvalSubroutineTemplate,
						   ExecEvalSubroutine);
}

extern bool ExecEvalBoolSubroutineTemplate(ExprState *state,
										   struct ExprEvalStep *op,
										   ExprContext *econtext);
bool
ExecEvalBoolSubroutineTemplate(ExprState *state,
							   struct ExprEvalStep *op,
							   ExprContext *econtext)
{
	AssertVariableIsOfType(&ExecEvalBoolSubroutineTemplate,
						   ExecEvalBoolSubroutine);

	return false;
}

/*
 * Clang represents stdbool.h style booleans that are returned by functions
 * differently (as i1) than stored ones (as i8). Therefore we do not just need
 * TypeBool (above), but also a way to determine the width of a returned
 * integer. This allows us to keep compatible with non-stdbool using
 * architectures.
 */
extern bool FunctionReturningBool(void);
bool
FunctionReturningBool(void)
{
	return false;
}

/*
 * To force signatures of functions used during JITing to be present,
 * reference the functions required. This again has to be non-static, to avoid
 * being removed as unnecessary.
 */
void	   *referenced_functions[] =
{
	ExecAggInitGroup,
	ExecAggCopyTransValue,
	ExecEvalPreOrderedDistinctSingle,
	ExecEvalPreOrderedDistinctMulti,
	ExecEvalAggOrderedTransDatum,
	ExecEvalAggOrderedTransTuple,
	ExecEvalArrayCoerce,
	ExecEvalArrayExpr,
	ExecEvalConstraintCheck,
	ExecEvalConstraintNotNull,
	ExecEvalConvertRowtype,
	ExecEvalCurrentOfExpr,
	ExecEvalFieldSelect,
	ExecEvalFieldStoreDeForm,
	ExecEvalFieldStoreForm,
	ExecEvalFuncExprFusage,
	ExecEvalFuncExprStrictFusage,
	ExecEvalGroupingFunc,
	ExecEvalMergeSupportFunc,
	ExecEvalMinMax,
	ExecEvalNextValueExpr,
	ExecEvalParamExec,
	ExecEvalParamExtern,
	ExecEvalParamSet,
	ExecEvalRow,
	ExecEvalRowNotNull,
	ExecEvalRowNull,
	ExecEvalCoerceViaIOSafe,
	ExecEvalSQLValueFunction,
	ExecEvalScalarArrayOp,
	ExecEvalHashedScalarArrayOp,
	ExecEvalSubPlan,
	ExecEvalSysVar,
	ExecEvalWholeRowVar,
	ExecEvalXmlExpr,
	ExecEvalJsonConstructor,
	ExecEvalJsonIsPredicate,
	ExecEvalJsonCoercion,
	ExecEvalJsonCoercionFinish,
	ExecEvalJsonExprPath,
	MakeExpandedObjectReadOnlyInternal,
	slot_getmissingattrs,
	slot_getsomeattrs_int,
	strlen,
	varsize_any,
	ExecInterpExprStillValid,
};
