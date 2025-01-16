/*-------------------------------------------------------------------------
 *
 * execExprInterp.c
 *	  Interpreted evaluation of an expression step list.
 *
 * This file provides either a "direct threaded" (for gcc, clang and
 * compatible) or a "switch threaded" (for all compilers) implementation of
 * expression evaluation.  The former is amongst the fastest known methods
 * of interpreting programs without resorting to assembly level work, or
 * just-in-time compilation, but it requires support for computed gotos.
 * The latter is amongst the fastest approaches doable in standard C.
 *
 * In either case we use ExprEvalStep->opcode to dispatch to the code block
 * within ExecInterpExpr() that implements the specific opcode type.
 *
 * Switch-threading uses a plain switch() statement to perform the
 * dispatch.  This has the advantages of being plain C and allowing the
 * compiler to warn if implementation of a specific opcode has been forgotten.
 * The disadvantage is that dispatches will, as commonly implemented by
 * compilers, happen from a single location, requiring more jumps and causing
 * bad branch prediction.
 *
 * In direct threading, we use gcc's label-as-values extension - also adopted
 * by some other compilers - to replace ExprEvalStep->opcode with the address
 * of the block implementing the instruction. Dispatch to the next instruction
 * is done by a "computed goto".  This allows for better branch prediction
 * (as the jumps are happening from different locations) and fewer jumps
 * (as no preparatory jump to a common dispatch location is needed).
 *
 * When using direct threading, ExecReadyInterpretedExpr will replace
 * each step's opcode field with the address of the relevant code block and
 * ExprState->flags will contain EEO_FLAG_DIRECT_THREADED to remember that
 * that's been done.
 *
 * For very simple instructions the overhead of the full interpreter
 * "startup", as minimal as it is, is noticeable.  Therefore
 * ExecReadyInterpretedExpr will choose to implement certain simple
 * opcode patterns using special fast-path routines (ExecJust*).
 *
 * Complex or uncommon instructions are not implemented in-line in
 * ExecInterpExpr(), rather we call out to a helper function appearing later
 * in this file.  For one reason, there'd not be a noticeable performance
 * benefit, but more importantly those complex routines are intended to be
 * shared between different expression evaluation approaches.  For instance
 * a JIT compiler would generate calls to them.  (This is why they are
 * exported rather than being "static" in this file.)
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/executor/execExprInterp.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heaptoast.h"
#include "catalog/pg_type.h"
#include "commands/sequence.h"
#include "executor/execExpr.h"
#include "executor/nodeSubplan.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "nodes/nodeFuncs.h"
#include "pgstat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/expandedrecord.h"
#include "utils/json.h"
#include "utils/jsonfuncs.h"
#include "utils/jsonpath.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"
#include "utils/xml.h"

/*
 * Use computed-goto-based opcode dispatch when computed gotos are available.
 * But use a separate symbol so that it's easy to adjust locally in this file
 * for development and testing.
 */
#ifdef HAVE_COMPUTED_GOTO
#define EEO_USE_COMPUTED_GOTO
#endif							/* HAVE_COMPUTED_GOTO */

/*
 * Macros for opcode dispatch.
 *
 * EEO_SWITCH - just hides the switch if not in use.
 * EEO_CASE - labels the implementation of named expression step type.
 * EEO_DISPATCH - jump to the implementation of the step type for 'op'.
 * EEO_OPCODE - compute opcode required by used expression evaluation method.
 * EEO_NEXT - increment 'op' and jump to correct next step type.
 * EEO_JUMP - jump to the specified step number within the current expression.
 */
#if defined(EEO_USE_COMPUTED_GOTO)

/* struct for jump target -> opcode lookup table */
typedef struct ExprEvalOpLookup
{
	const void *opcode;
	ExprEvalOp	op;
} ExprEvalOpLookup;

/* to make dispatch_table accessible outside ExecInterpExpr() */
static const void **dispatch_table = NULL;

/* jump target -> opcode lookup table */
static ExprEvalOpLookup reverse_dispatch_table[EEOP_LAST];

#define EEO_SWITCH()
#define EEO_CASE(name)		CASE_##name:
#define EEO_DISPATCH()		goto *((void *) op->opcode)
#define EEO_OPCODE(opcode)	((intptr_t) dispatch_table[opcode])

#else							/* !EEO_USE_COMPUTED_GOTO */

#define EEO_SWITCH()		starteval: switch ((ExprEvalOp) op->opcode)
#define EEO_CASE(name)		case name:
#define EEO_DISPATCH()		goto starteval
#define EEO_OPCODE(opcode)	(opcode)

#endif							/* EEO_USE_COMPUTED_GOTO */

#define EEO_NEXT() \
	do { \
		op++; \
		EEO_DISPATCH(); \
	} while (0)

#define EEO_JUMP(stepno) \
	do { \
		op = &state->steps[stepno]; \
		EEO_DISPATCH(); \
	} while (0)


static Datum ExecInterpExpr(ExprState *state, ExprContext *econtext, bool *isnull);
static void ExecInitInterpreter(void);

/* support functions */
static void CheckVarSlotCompatibility(TupleTableSlot *slot, int attnum, Oid vartype);
static void CheckOpSlotCompatibility(ExprEvalStep *op, TupleTableSlot *slot);
static TupleDesc get_cached_rowtype(Oid type_id, int32 typmod,
									ExprEvalRowtypeCache *rowcache,
									bool *changed);
static void ExecEvalRowNullInt(ExprState *state, ExprEvalStep *op,
							   ExprContext *econtext, bool checkisnull);

/* fast-path evaluation functions */
static Datum ExecJustInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustScanVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignScanVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustApplyFuncToCase(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustConst(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustAssignScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustHashInnerVarWithIV(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustHashOuterVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustHashInnerVar(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustHashOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustHashInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull);
static Datum ExecJustHashOuterVarStrict(ExprState *state, ExprContext *econtext, bool *isnull);

/* execution helper functions */
static pg_attribute_always_inline void ExecAggPlainTransByVal(AggState *aggstate,
															  AggStatePerTrans pertrans,
															  AggStatePerGroup pergroup,
															  ExprContext *aggcontext,
															  int setno);
static pg_attribute_always_inline void ExecAggPlainTransByRef(AggState *aggstate,
															  AggStatePerTrans pertrans,
															  AggStatePerGroup pergroup,
															  ExprContext *aggcontext,
															  int setno);
static char *ExecGetJsonValueItemString(JsonbValue *item, bool *resnull);

/*
 * ScalarArrayOpExprHashEntry
 * 		Hash table entry type used during EEOP_HASHED_SCALARARRAYOP
 */
typedef struct ScalarArrayOpExprHashEntry
{
	Datum		key;
	uint32		status;			/* hash status */
	uint32		hash;			/* hash value (cached) */
} ScalarArrayOpExprHashEntry;

#define SH_PREFIX saophash
#define SH_ELEMENT_TYPE ScalarArrayOpExprHashEntry
#define SH_KEY_TYPE Datum
#define SH_SCOPE static inline
#define SH_DECLARE
#include "lib/simplehash.h"

static bool saop_hash_element_match(struct saophash_hash *tb, Datum key1,
									Datum key2);
static uint32 saop_element_hash(struct saophash_hash *tb, Datum key);

/*
 * ScalarArrayOpExprHashTable
 *		Hash table for EEOP_HASHED_SCALARARRAYOP
 */
typedef struct ScalarArrayOpExprHashTable
{
	saophash_hash *hashtab;		/* underlying hash table */
	struct ExprEvalStep *op;
	FmgrInfo	hash_finfo;		/* function's lookup data */
	FunctionCallInfoBaseData hash_fcinfo_data;	/* arguments etc */
} ScalarArrayOpExprHashTable;

/* Define parameters for ScalarArrayOpExpr hash table code generation. */
#define SH_PREFIX saophash
#define SH_ELEMENT_TYPE ScalarArrayOpExprHashEntry
#define SH_KEY_TYPE Datum
#define SH_KEY key
#define SH_HASH_KEY(tb, key) saop_element_hash(tb, key)
#define SH_EQUAL(tb, a, b) saop_hash_element_match(tb, a, b)
#define SH_SCOPE static inline
#define SH_STORE_HASH
#define SH_GET_HASH(tb, a) a->hash
#define SH_DEFINE
#include "lib/simplehash.h"

/*
 * Prepare ExprState for interpreted execution.
 */
void
ExecReadyInterpretedExpr(ExprState *state)
{
	/* Ensure one-time interpreter setup has been done */
	ExecInitInterpreter();

	/* Simple validity checks on expression */
	Assert(state->steps_len >= 1);
	Assert(state->steps[state->steps_len - 1].opcode == EEOP_DONE);

	/*
	 * Don't perform redundant initialization. This is unreachable in current
	 * cases, but might be hit if there's additional expression evaluation
	 * methods that rely on interpreted execution to work.
	 */
	if (state->flags & EEO_FLAG_INTERPRETER_INITIALIZED)
		return;

	/*
	 * First time through, check whether attribute matches Var.  Might not be
	 * ok anymore, due to schema changes. We do that by setting up a callback
	 * that does checking on the first call, which then sets the evalfunc
	 * callback to the actual method of execution.
	 */
	state->evalfunc = ExecInterpExprStillValid;

	/* DIRECT_THREADED should not already be set */
	Assert((state->flags & EEO_FLAG_DIRECT_THREADED) == 0);

	/*
	 * There shouldn't be any errors before the expression is fully
	 * initialized, and even if so, it'd lead to the expression being
	 * abandoned.  So we can set the flag now and save some code.
	 */
	state->flags |= EEO_FLAG_INTERPRETER_INITIALIZED;

	/*
	 * Select fast-path evalfuncs for very simple expressions.  "Starting up"
	 * the full interpreter is a measurable overhead for these, and these
	 * patterns occur often enough to be worth optimizing.
	 */
	if (state->steps_len == 5)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;
		ExprEvalOp	step1 = state->steps[1].opcode;
		ExprEvalOp	step2 = state->steps[2].opcode;
		ExprEvalOp	step3 = state->steps[3].opcode;

		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_HASHDATUM_SET_INITVAL &&
			step2 == EEOP_INNER_VAR &&
			step3 == EEOP_HASHDATUM_NEXT32)
		{
			state->evalfunc_private = (void *) ExecJustHashInnerVarWithIV;
			return;
		}
	}
	else if (state->steps_len == 4)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;
		ExprEvalOp	step1 = state->steps[1].opcode;
		ExprEvalOp	step2 = state->steps[2].opcode;

		if (step0 == EEOP_OUTER_FETCHSOME &&
			step1 == EEOP_OUTER_VAR &&
			step2 == EEOP_HASHDATUM_FIRST)
		{
			state->evalfunc_private = (void *) ExecJustHashOuterVar;
			return;
		}
		else if (step0 == EEOP_INNER_FETCHSOME &&
				 step1 == EEOP_INNER_VAR &&
				 step2 == EEOP_HASHDATUM_FIRST)
		{
			state->evalfunc_private = (void *) ExecJustHashInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_OUTER_VAR &&
				 step2 == EEOP_HASHDATUM_FIRST_STRICT)
		{
			state->evalfunc_private = (void *) ExecJustHashOuterVarStrict;
			return;
		}
	}
	else if (state->steps_len == 3)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;
		ExprEvalOp	step1 = state->steps[1].opcode;

		if (step0 == EEOP_INNER_FETCHSOME &&
			step1 == EEOP_INNER_VAR)
		{
			state->evalfunc_private = ExecJustInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_OUTER_VAR)
		{
			state->evalfunc_private = ExecJustOuterVar;
			return;
		}
		else if (step0 == EEOP_SCAN_FETCHSOME &&
				 step1 == EEOP_SCAN_VAR)
		{
			state->evalfunc_private = ExecJustScanVar;
			return;
		}
		else if (step0 == EEOP_INNER_FETCHSOME &&
				 step1 == EEOP_ASSIGN_INNER_VAR)
		{
			state->evalfunc_private = ExecJustAssignInnerVar;
			return;
		}
		else if (step0 == EEOP_OUTER_FETCHSOME &&
				 step1 == EEOP_ASSIGN_OUTER_VAR)
		{
			state->evalfunc_private = ExecJustAssignOuterVar;
			return;
		}
		else if (step0 == EEOP_SCAN_FETCHSOME &&
				 step1 == EEOP_ASSIGN_SCAN_VAR)
		{
			state->evalfunc_private = ExecJustAssignScanVar;
			return;
		}
		else if (step0 == EEOP_CASE_TESTVAL &&
				 step1 == EEOP_FUNCEXPR_STRICT &&
				 state->steps[0].d.casetest.value)
		{
			state->evalfunc_private = ExecJustApplyFuncToCase;
			return;
		}
		else if (step0 == EEOP_INNER_VAR &&
				 step1 == EEOP_HASHDATUM_FIRST)
		{
			state->evalfunc_private = (void *) ExecJustHashInnerVarVirt;
			return;
		}
		else if (step0 == EEOP_OUTER_VAR &&
				 step1 == EEOP_HASHDATUM_FIRST)
		{
			state->evalfunc_private = (void *) ExecJustHashOuterVarVirt;
			return;
		}
	}
	else if (state->steps_len == 2)
	{
		ExprEvalOp	step0 = state->steps[0].opcode;

		if (step0 == EEOP_CONST)
		{
			state->evalfunc_private = ExecJustConst;
			return;
		}
		else if (step0 == EEOP_INNER_VAR)
		{
			state->evalfunc_private = ExecJustInnerVarVirt;
			return;
		}
		else if (step0 == EEOP_OUTER_VAR)
		{
			state->evalfunc_private = ExecJustOuterVarVirt;
			return;
		}
		else if (step0 == EEOP_SCAN_VAR)
		{
			state->evalfunc_private = ExecJustScanVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_INNER_VAR)
		{
			state->evalfunc_private = ExecJustAssignInnerVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_OUTER_VAR)
		{
			state->evalfunc_private = ExecJustAssignOuterVarVirt;
			return;
		}
		else if (step0 == EEOP_ASSIGN_SCAN_VAR)
		{
			state->evalfunc_private = ExecJustAssignScanVarVirt;
			return;
		}
	}

#if defined(EEO_USE_COMPUTED_GOTO)

	/*
	 * In the direct-threaded implementation, replace each opcode with the
	 * address to jump to.  (Use ExecEvalStepOp() to get back the opcode.)
	 */
	for (int off = 0; off < state->steps_len; off++)
	{
		ExprEvalStep *op = &state->steps[off];

		op->opcode = EEO_OPCODE(op->opcode);
	}

	state->flags |= EEO_FLAG_DIRECT_THREADED;
#endif							/* EEO_USE_COMPUTED_GOTO */

	state->evalfunc_private = ExecInterpExpr;
}


/*
 * Evaluate expression identified by "state" in the execution context
 * given by "econtext".  *isnull is set to the is-null flag for the result,
 * and the Datum value is the function result.
 *
 * As a special case, return the dispatch table's address if state is NULL.
 * This is used by ExecInitInterpreter to set up the dispatch_table global.
 * (Only applies when EEO_USE_COMPUTED_GOTO is defined.)
 */
static Datum
ExecInterpExpr(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op;
	TupleTableSlot *resultslot;
	TupleTableSlot *innerslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *scanslot;
	TupleTableSlot *oldslot;
	TupleTableSlot *newslot;

	/*
	 * This array has to be in the same order as enum ExprEvalOp.
	 */
#if defined(EEO_USE_COMPUTED_GOTO)
	static const void *const dispatch_table[] = {
		&&CASE_EEOP_DONE,
		&&CASE_EEOP_INNER_FETCHSOME,
		&&CASE_EEOP_OUTER_FETCHSOME,
		&&CASE_EEOP_SCAN_FETCHSOME,
		&&CASE_EEOP_OLD_FETCHSOME,
		&&CASE_EEOP_NEW_FETCHSOME,
		&&CASE_EEOP_INNER_VAR,
		&&CASE_EEOP_OUTER_VAR,
		&&CASE_EEOP_SCAN_VAR,
		&&CASE_EEOP_OLD_VAR,
		&&CASE_EEOP_NEW_VAR,
		&&CASE_EEOP_INNER_SYSVAR,
		&&CASE_EEOP_OUTER_SYSVAR,
		&&CASE_EEOP_SCAN_SYSVAR,
		&&CASE_EEOP_OLD_SYSVAR,
		&&CASE_EEOP_NEW_SYSVAR,
		&&CASE_EEOP_WHOLEROW,
		&&CASE_EEOP_ASSIGN_INNER_VAR,
		&&CASE_EEOP_ASSIGN_OUTER_VAR,
		&&CASE_EEOP_ASSIGN_SCAN_VAR,
		&&CASE_EEOP_ASSIGN_OLD_VAR,
		&&CASE_EEOP_ASSIGN_NEW_VAR,
		&&CASE_EEOP_ASSIGN_TMP,
		&&CASE_EEOP_ASSIGN_TMP_MAKE_RO,
		&&CASE_EEOP_CONST,
		&&CASE_EEOP_FUNCEXPR,
		&&CASE_EEOP_FUNCEXPR_STRICT,
		&&CASE_EEOP_FUNCEXPR_FUSAGE,
		&&CASE_EEOP_FUNCEXPR_STRICT_FUSAGE,
		&&CASE_EEOP_BOOL_AND_STEP_FIRST,
		&&CASE_EEOP_BOOL_AND_STEP,
		&&CASE_EEOP_BOOL_AND_STEP_LAST,
		&&CASE_EEOP_BOOL_OR_STEP_FIRST,
		&&CASE_EEOP_BOOL_OR_STEP,
		&&CASE_EEOP_BOOL_OR_STEP_LAST,
		&&CASE_EEOP_BOOL_NOT_STEP,
		&&CASE_EEOP_QUAL,
		&&CASE_EEOP_JUMP,
		&&CASE_EEOP_JUMP_IF_NULL,
		&&CASE_EEOP_JUMP_IF_NOT_NULL,
		&&CASE_EEOP_JUMP_IF_NOT_TRUE,
		&&CASE_EEOP_NULLTEST_ISNULL,
		&&CASE_EEOP_NULLTEST_ISNOTNULL,
		&&CASE_EEOP_NULLTEST_ROWISNULL,
		&&CASE_EEOP_NULLTEST_ROWISNOTNULL,
		&&CASE_EEOP_BOOLTEST_IS_TRUE,
		&&CASE_EEOP_BOOLTEST_IS_NOT_TRUE,
		&&CASE_EEOP_BOOLTEST_IS_FALSE,
		&&CASE_EEOP_BOOLTEST_IS_NOT_FALSE,
		&&CASE_EEOP_PARAM_EXEC,
		&&CASE_EEOP_PARAM_EXTERN,
		&&CASE_EEOP_PARAM_CALLBACK,
		&&CASE_EEOP_PARAM_SET,
		&&CASE_EEOP_CASE_TESTVAL,
		&&CASE_EEOP_MAKE_READONLY,
		&&CASE_EEOP_IOCOERCE,
		&&CASE_EEOP_IOCOERCE_SAFE,
		&&CASE_EEOP_DISTINCT,
		&&CASE_EEOP_NOT_DISTINCT,
		&&CASE_EEOP_NULLIF,
		&&CASE_EEOP_SQLVALUEFUNCTION,
		&&CASE_EEOP_CURRENTOFEXPR,
		&&CASE_EEOP_NEXTVALUEEXPR,
		&&CASE_EEOP_RETURNINGEXPR,
		&&CASE_EEOP_ARRAYEXPR,
		&&CASE_EEOP_ARRAYCOERCE,
		&&CASE_EEOP_ROW,
		&&CASE_EEOP_ROWCOMPARE_STEP,
		&&CASE_EEOP_ROWCOMPARE_FINAL,
		&&CASE_EEOP_MINMAX,
		&&CASE_EEOP_FIELDSELECT,
		&&CASE_EEOP_FIELDSTORE_DEFORM,
		&&CASE_EEOP_FIELDSTORE_FORM,
		&&CASE_EEOP_SBSREF_SUBSCRIPTS,
		&&CASE_EEOP_SBSREF_OLD,
		&&CASE_EEOP_SBSREF_ASSIGN,
		&&CASE_EEOP_SBSREF_FETCH,
		&&CASE_EEOP_DOMAIN_TESTVAL,
		&&CASE_EEOP_DOMAIN_NOTNULL,
		&&CASE_EEOP_DOMAIN_CHECK,
		&&CASE_EEOP_HASHDATUM_SET_INITVAL,
		&&CASE_EEOP_HASHDATUM_FIRST,
		&&CASE_EEOP_HASHDATUM_FIRST_STRICT,
		&&CASE_EEOP_HASHDATUM_NEXT32,
		&&CASE_EEOP_HASHDATUM_NEXT32_STRICT,
		&&CASE_EEOP_CONVERT_ROWTYPE,
		&&CASE_EEOP_SCALARARRAYOP,
		&&CASE_EEOP_HASHED_SCALARARRAYOP,
		&&CASE_EEOP_XMLEXPR,
		&&CASE_EEOP_JSON_CONSTRUCTOR,
		&&CASE_EEOP_IS_JSON,
		&&CASE_EEOP_JSONEXPR_PATH,
		&&CASE_EEOP_JSONEXPR_COERCION,
		&&CASE_EEOP_JSONEXPR_COERCION_FINISH,
		&&CASE_EEOP_AGGREF,
		&&CASE_EEOP_GROUPING_FUNC,
		&&CASE_EEOP_WINDOW_FUNC,
		&&CASE_EEOP_MERGE_SUPPORT_FUNC,
		&&CASE_EEOP_SUBPLAN,
		&&CASE_EEOP_AGG_STRICT_DESERIALIZE,
		&&CASE_EEOP_AGG_DESERIALIZE,
		&&CASE_EEOP_AGG_STRICT_INPUT_CHECK_ARGS,
		&&CASE_EEOP_AGG_STRICT_INPUT_CHECK_NULLS,
		&&CASE_EEOP_AGG_PLAIN_PERGROUP_NULLCHECK,
		&&CASE_EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_BYVAL,
		&&CASE_EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF,
		&&CASE_EEOP_AGG_PLAIN_TRANS_STRICT_BYREF,
		&&CASE_EEOP_AGG_PLAIN_TRANS_BYREF,
		&&CASE_EEOP_AGG_PRESORTED_DISTINCT_SINGLE,
		&&CASE_EEOP_AGG_PRESORTED_DISTINCT_MULTI,
		&&CASE_EEOP_AGG_ORDERED_TRANS_DATUM,
		&&CASE_EEOP_AGG_ORDERED_TRANS_TUPLE,
		&&CASE_EEOP_LAST
	};

	StaticAssertDecl(lengthof(dispatch_table) == EEOP_LAST + 1,
					 "dispatch_table out of whack with ExprEvalOp");

	if (unlikely(state == NULL))
		return PointerGetDatum(dispatch_table);
#else
	Assert(state != NULL);
#endif							/* EEO_USE_COMPUTED_GOTO */

	/* setup state */
	op = state->steps;
	resultslot = state->resultslot;
	innerslot = econtext->ecxt_innertuple;
	outerslot = econtext->ecxt_outertuple;
	scanslot = econtext->ecxt_scantuple;
	oldslot = econtext->ecxt_oldtuple;
	newslot = econtext->ecxt_newtuple;

#if defined(EEO_USE_COMPUTED_GOTO)
	EEO_DISPATCH();
#endif

	EEO_SWITCH()
	{
		EEO_CASE(EEOP_DONE)
		{
			goto out;
		}

		EEO_CASE(EEOP_INNER_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, innerslot);

			slot_getsomeattrs(innerslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, outerslot);

			slot_getsomeattrs(outerslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, scanslot);

			slot_getsomeattrs(scanslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OLD_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, oldslot);

			slot_getsomeattrs(oldslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NEW_FETCHSOME)
		{
			CheckOpSlotCompatibility(op, newslot);

			slot_getsomeattrs(newslot, op->d.fetch.last_var);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_INNER_VAR)
		{
			int			attnum = op->d.var.attnum;

			/*
			 * Since we already extracted all referenced columns from the
			 * tuple with a FETCHSOME step, we can just grab the value
			 * directly out of the slot's decomposed-data arrays.  But let's
			 * have an Assert to check that that did happen.
			 */
			Assert(attnum >= 0 && attnum < innerslot->tts_nvalid);
			*op->resvalue = innerslot->tts_values[attnum];
			*op->resnull = innerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_VAR)
		{
			int			attnum = op->d.var.attnum;

			/* See EEOP_INNER_VAR comments */

			Assert(attnum >= 0 && attnum < outerslot->tts_nvalid);
			*op->resvalue = outerslot->tts_values[attnum];
			*op->resnull = outerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_VAR)
		{
			int			attnum = op->d.var.attnum;

			/* See EEOP_INNER_VAR comments */

			Assert(attnum >= 0 && attnum < scanslot->tts_nvalid);
			*op->resvalue = scanslot->tts_values[attnum];
			*op->resnull = scanslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_OLD_VAR)
		{
			int			attnum = op->d.var.attnum;

			/* See EEOP_INNER_VAR comments */

			Assert(attnum >= 0 && attnum < oldslot->tts_nvalid);
			*op->resvalue = oldslot->tts_values[attnum];
			*op->resnull = oldslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NEW_VAR)
		{
			int			attnum = op->d.var.attnum;

			/* See EEOP_INNER_VAR comments */

			Assert(attnum >= 0 && attnum < newslot->tts_nvalid);
			*op->resvalue = newslot->tts_values[attnum];
			*op->resnull = newslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_INNER_SYSVAR)
		{
			ExecEvalSysVar(state, op, econtext, innerslot);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_OUTER_SYSVAR)
		{
			ExecEvalSysVar(state, op, econtext, outerslot);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCAN_SYSVAR)
		{
			ExecEvalSysVar(state, op, econtext, scanslot);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_OLD_SYSVAR)
		{
			ExecEvalSysVar(state, op, econtext, oldslot);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_NEW_SYSVAR)
		{
			ExecEvalSysVar(state, op, econtext, newslot);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_WHOLEROW)
		{
			/* too complex for an inline implementation */
			ExecEvalWholeRowVar(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_INNER_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < innerslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = innerslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = innerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_OUTER_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < outerslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = outerslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = outerslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_SCAN_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < scanslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = scanslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = scanslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_OLD_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < oldslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = oldslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = oldslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_NEW_VAR)
		{
			int			resultnum = op->d.assign_var.resultnum;
			int			attnum = op->d.assign_var.attnum;

			/*
			 * We do not need CheckVarSlotCompatibility here; that was taken
			 * care of at compilation time.  But see EEOP_INNER_VAR comments.
			 */
			Assert(attnum >= 0 && attnum < newslot->tts_nvalid);
			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = newslot->tts_values[attnum];
			resultslot->tts_isnull[resultnum] = newslot->tts_isnull[attnum];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_TMP)
		{
			int			resultnum = op->d.assign_tmp.resultnum;

			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_values[resultnum] = state->resvalue;
			resultslot->tts_isnull[resultnum] = state->resnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ASSIGN_TMP_MAKE_RO)
		{
			int			resultnum = op->d.assign_tmp.resultnum;

			Assert(resultnum >= 0 && resultnum < resultslot->tts_tupleDescriptor->natts);
			resultslot->tts_isnull[resultnum] = state->resnull;
			if (!resultslot->tts_isnull[resultnum])
				resultslot->tts_values[resultnum] =
					MakeExpandedObjectReadOnlyInternal(state->resvalue);
			else
				resultslot->tts_values[resultnum] = state->resvalue;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CONST)
		{
			*op->resnull = op->d.constval.isnull;
			*op->resvalue = op->d.constval.value;

			EEO_NEXT();
		}

		/*
		 * Function-call implementations. Arguments have previously been
		 * evaluated directly into fcinfo->args.
		 *
		 * As both STRICT checks and function-usage are noticeable performance
		 * wise, and function calls are a very hot-path (they also back
		 * operators!), it's worth having so many separate opcodes.
		 *
		 * Note: the reason for using a temporary variable "d", here and in
		 * other places, is that some compilers think "*op->resvalue = f();"
		 * requires them to evaluate op->resvalue into a register before
		 * calling f(), just in case f() is able to modify op->resvalue
		 * somehow.  The extra line of code can save a useless register spill
		 * and reload across the function call.
		 */
		EEO_CASE(EEOP_FUNCEXPR)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			Datum		d;

			fcinfo->isnull = false;
			d = op->d.func.fn_addr(fcinfo);
			*op->resvalue = d;
			*op->resnull = fcinfo->isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_STRICT)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			NullableDatum *args = fcinfo->args;
			int			nargs = op->d.func.nargs;
			Datum		d;

			/* strict function, so check for NULL args */
			for (int argno = 0; argno < nargs; argno++)
			{
				if (args[argno].isnull)
				{
					*op->resnull = true;
					goto strictfail;
				}
			}
			fcinfo->isnull = false;
			d = op->d.func.fn_addr(fcinfo);
			*op->resvalue = d;
			*op->resnull = fcinfo->isnull;

	strictfail:
			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_FUSAGE)
		{
			/* not common enough to inline */
			ExecEvalFuncExprFusage(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FUNCEXPR_STRICT_FUSAGE)
		{
			/* not common enough to inline */
			ExecEvalFuncExprStrictFusage(state, op, econtext);

			EEO_NEXT();
		}

		/*
		 * If any of its clauses is FALSE, an AND's result is FALSE regardless
		 * of the states of the rest of the clauses, so we can stop evaluating
		 * and return FALSE immediately.  If none are FALSE and one or more is
		 * NULL, we return NULL; otherwise we return TRUE.  This makes sense
		 * when you interpret NULL as "don't know": perhaps one of the "don't
		 * knows" would have been FALSE if we'd known its value.  Only when
		 * all the inputs are known to be TRUE can we state confidently that
		 * the AND's result is TRUE.
		 */
		EEO_CASE(EEOP_BOOL_AND_STEP_FIRST)
		{
			*op->d.boolexpr.anynull = false;

			/*
			 * EEOP_BOOL_AND_STEP_FIRST resets anynull, otherwise it's the
			 * same as EEOP_BOOL_AND_STEP - so fall through to that.
			 */

			/* FALL THROUGH */
		}

		EEO_CASE(EEOP_BOOL_AND_STEP)
		{
			if (*op->resnull)
			{
				*op->d.boolexpr.anynull = true;
			}
			else if (!DatumGetBool(*op->resvalue))
			{
				/* result is already set to FALSE, need not change it */
				/* bail out early */
				EEO_JUMP(op->d.boolexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_AND_STEP_LAST)
		{
			if (*op->resnull)
			{
				/* result is already set to NULL, need not change it */
			}
			else if (!DatumGetBool(*op->resvalue))
			{
				/* result is already set to FALSE, need not change it */

				/*
				 * No point jumping early to jumpdone - would be same target
				 * (as this is the last argument to the AND expression),
				 * except more expensive.
				 */
			}
			else if (*op->d.boolexpr.anynull)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}
			else
			{
				/* result is already set to TRUE, need not change it */
			}

			EEO_NEXT();
		}

		/*
		 * If any of its clauses is TRUE, an OR's result is TRUE regardless of
		 * the states of the rest of the clauses, so we can stop evaluating
		 * and return TRUE immediately.  If none are TRUE and one or more is
		 * NULL, we return NULL; otherwise we return FALSE.  This makes sense
		 * when you interpret NULL as "don't know": perhaps one of the "don't
		 * knows" would have been TRUE if we'd known its value.  Only when all
		 * the inputs are known to be FALSE can we state confidently that the
		 * OR's result is FALSE.
		 */
		EEO_CASE(EEOP_BOOL_OR_STEP_FIRST)
		{
			*op->d.boolexpr.anynull = false;

			/*
			 * EEOP_BOOL_OR_STEP_FIRST resets anynull, otherwise it's the same
			 * as EEOP_BOOL_OR_STEP - so fall through to that.
			 */

			/* FALL THROUGH */
		}

		EEO_CASE(EEOP_BOOL_OR_STEP)
		{
			if (*op->resnull)
			{
				*op->d.boolexpr.anynull = true;
			}
			else if (DatumGetBool(*op->resvalue))
			{
				/* result is already set to TRUE, need not change it */
				/* bail out early */
				EEO_JUMP(op->d.boolexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_OR_STEP_LAST)
		{
			if (*op->resnull)
			{
				/* result is already set to NULL, need not change it */
			}
			else if (DatumGetBool(*op->resvalue))
			{
				/* result is already set to TRUE, need not change it */

				/*
				 * No point jumping to jumpdone - would be same target (as
				 * this is the last argument to the AND expression), except
				 * more expensive.
				 */
			}
			else if (*op->d.boolexpr.anynull)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;
			}
			else
			{
				/* result is already set to FALSE, need not change it */
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOL_NOT_STEP)
		{
			/*
			 * Evaluation of 'not' is simple... if expr is false, then return
			 * 'true' and vice versa.  It's safe to do this even on a
			 * nominally null value, so we ignore resnull; that means that
			 * NULL in produces NULL out, which is what we want.
			 */
			*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_QUAL)
		{
			/* simplified version of BOOL_AND_STEP for use by ExecQual() */

			/* If argument (also result) is false or null ... */
			if (*op->resnull ||
				!DatumGetBool(*op->resvalue))
			{
				/* ... bail out early, returning FALSE */
				*op->resnull = false;
				*op->resvalue = BoolGetDatum(false);
				EEO_JUMP(op->d.qualexpr.jumpdone);
			}

			/*
			 * Otherwise, leave the TRUE value in place, in case this is the
			 * last qual.  Then, TRUE is the correct answer.
			 */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP)
		{
			/* Unconditionally jump to target step */
			EEO_JUMP(op->d.jump.jumpdone);
		}

		EEO_CASE(EEOP_JUMP_IF_NULL)
		{
			/* Transfer control if current result is null */
			if (*op->resnull)
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP_IF_NOT_NULL)
		{
			/* Transfer control if current result is non-null */
			if (!*op->resnull)
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JUMP_IF_NOT_TRUE)
		{
			/* Transfer control if current result is null or false */
			if (*op->resnull || !DatumGetBool(*op->resvalue))
				EEO_JUMP(op->d.jump.jumpdone);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ISNULL)
		{
			*op->resvalue = BoolGetDatum(*op->resnull);
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ISNOTNULL)
		{
			*op->resvalue = BoolGetDatum(!*op->resnull);
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ROWISNULL)
		{
			/* out of line implementation: too large */
			ExecEvalRowNull(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLTEST_ROWISNOTNULL)
		{
			/* out of line implementation: too large */
			ExecEvalRowNotNull(state, op, econtext);

			EEO_NEXT();
		}

		/* BooleanTest implementations for all booltesttypes */

		EEO_CASE(EEOP_BOOLTEST_IS_TRUE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			/* else, input value is the correct output as well */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_NOT_TRUE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else
				*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_FALSE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else
				*op->resvalue = BoolGetDatum(!DatumGetBool(*op->resvalue));

			EEO_NEXT();
		}

		EEO_CASE(EEOP_BOOLTEST_IS_NOT_FALSE)
		{
			if (*op->resnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			/* else, input value is the correct output as well */

			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_EXEC)
		{
			/* out of line implementation: too large */
			ExecEvalParamExec(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_EXTERN)
		{
			/* out of line implementation: too large */
			ExecEvalParamExtern(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_CALLBACK)
		{
			/* allow an extension module to supply a PARAM_EXTERN value */
			op->d.cparam.paramfunc(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_PARAM_SET)
		{
			/* out of line, unlikely to matter performance-wise */
			ExecEvalParamSet(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_CASE_TESTVAL)
		{
			/*
			 * Normally upper parts of the expression tree have setup the
			 * values to be returned here, but some parts of the system
			 * currently misuse {caseValue,domainValue}_{datum,isNull} to set
			 * run-time data.  So if no values have been set-up, use
			 * ExprContext's.  This isn't pretty, but also not *that* ugly,
			 * and this is unlikely to be performance sensitive enough to
			 * worry about an extra branch.
			 */
			if (op->d.casetest.value)
			{
				*op->resvalue = *op->d.casetest.value;
				*op->resnull = *op->d.casetest.isnull;
			}
			else
			{
				*op->resvalue = econtext->caseValue_datum;
				*op->resnull = econtext->caseValue_isNull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_TESTVAL)
		{
			/*
			 * See EEOP_CASE_TESTVAL comment.
			 */
			if (op->d.casetest.value)
			{
				*op->resvalue = *op->d.casetest.value;
				*op->resnull = *op->d.casetest.isnull;
			}
			else
			{
				*op->resvalue = econtext->domainValue_datum;
				*op->resnull = econtext->domainValue_isNull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MAKE_READONLY)
		{
			/*
			 * Force a varlena value that might be read multiple times to R/O
			 */
			if (!*op->d.make_readonly.isnull)
				*op->resvalue =
					MakeExpandedObjectReadOnlyInternal(*op->d.make_readonly.value);
			*op->resnull = *op->d.make_readonly.isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_IOCOERCE)
		{
			/*
			 * Evaluate a CoerceViaIO node.  This can be quite a hot path, so
			 * inline as much work as possible.  The source value is in our
			 * result variable.
			 *
			 * Also look at ExecEvalCoerceViaIOSafe() if you change anything
			 * here.
			 */
			char	   *str;

			/* call output function (similar to OutputFunctionCall) */
			if (*op->resnull)
			{
				/* output functions are not called on nulls */
				str = NULL;
			}
			else
			{
				FunctionCallInfo fcinfo_out;

				fcinfo_out = op->d.iocoerce.fcinfo_data_out;
				fcinfo_out->args[0].value = *op->resvalue;
				fcinfo_out->args[0].isnull = false;

				fcinfo_out->isnull = false;
				str = DatumGetCString(FunctionCallInvoke(fcinfo_out));

				/* OutputFunctionCall assumes result isn't null */
				Assert(!fcinfo_out->isnull);
			}

			/* call input function (similar to InputFunctionCall) */
			if (!op->d.iocoerce.finfo_in->fn_strict || str != NULL)
			{
				FunctionCallInfo fcinfo_in;
				Datum		d;

				fcinfo_in = op->d.iocoerce.fcinfo_data_in;
				fcinfo_in->args[0].value = PointerGetDatum(str);
				fcinfo_in->args[0].isnull = *op->resnull;
				/* second and third arguments are already set up */

				fcinfo_in->isnull = false;
				d = FunctionCallInvoke(fcinfo_in);
				*op->resvalue = d;

				/* Should get null result if and only if str is NULL */
				if (str == NULL)
				{
					Assert(*op->resnull);
					Assert(fcinfo_in->isnull);
				}
				else
				{
					Assert(!*op->resnull);
					Assert(!fcinfo_in->isnull);
				}
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_IOCOERCE_SAFE)
		{
			ExecEvalCoerceViaIOSafe(state, op);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_DISTINCT)
		{
			/*
			 * IS DISTINCT FROM must evaluate arguments (already done into
			 * fcinfo->args) to determine whether they are NULL; if either is
			 * NULL then the result is determined.  If neither is NULL, then
			 * proceed to evaluate the comparison function, which is just the
			 * type's standard equality operator.  We need not care whether
			 * that function is strict.  Because the handling of nulls is
			 * different, we can't just reuse EEOP_FUNCEXPR.
			 */
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

			/* check function arguments for NULLness */
			if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
			{
				/* Both NULL? Then is not distinct... */
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
			{
				/* Only one is NULL? Then is distinct... */
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else
			{
				/* Neither null, so apply the equality function */
				Datum		eqresult;

				fcinfo->isnull = false;
				eqresult = op->d.func.fn_addr(fcinfo);
				/* Must invert result of "="; safe to do even if null */
				*op->resvalue = BoolGetDatum(!DatumGetBool(eqresult));
				*op->resnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		/* see EEOP_DISTINCT for comments, this is just inverted */
		EEO_CASE(EEOP_NOT_DISTINCT)
		{
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;

			if (fcinfo->args[0].isnull && fcinfo->args[1].isnull)
			{
				*op->resvalue = BoolGetDatum(true);
				*op->resnull = false;
			}
			else if (fcinfo->args[0].isnull || fcinfo->args[1].isnull)
			{
				*op->resvalue = BoolGetDatum(false);
				*op->resnull = false;
			}
			else
			{
				Datum		eqresult;

				fcinfo->isnull = false;
				eqresult = op->d.func.fn_addr(fcinfo);
				*op->resvalue = eqresult;
				*op->resnull = fcinfo->isnull;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NULLIF)
		{
			/*
			 * The arguments are already evaluated into fcinfo->args.
			 */
			FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
			Datum		save_arg0 = fcinfo->args[0].value;

			/* if either argument is NULL they can't be equal */
			if (!fcinfo->args[0].isnull && !fcinfo->args[1].isnull)
			{
				Datum		result;

				/*
				 * If first argument is of varlena type, it might be an
				 * expanded datum.  We need to ensure that the value passed to
				 * the comparison function is a read-only pointer.  However,
				 * if we end by returning the first argument, that will be the
				 * original read-write pointer if it was read-write.
				 */
				if (op->d.func.make_ro)
					fcinfo->args[0].value =
						MakeExpandedObjectReadOnlyInternal(save_arg0);

				fcinfo->isnull = false;
				result = op->d.func.fn_addr(fcinfo);

				/* if the arguments are equal return null */
				if (!fcinfo->isnull && DatumGetBool(result))
				{
					*op->resvalue = (Datum) 0;
					*op->resnull = true;

					EEO_NEXT();
				}
			}

			/* Arguments aren't equal, so return the first one */
			*op->resvalue = save_arg0;
			*op->resnull = fcinfo->args[0].isnull;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SQLVALUEFUNCTION)
		{
			/*
			 * Doesn't seem worthwhile to have an inline implementation
			 * efficiency-wise.
			 */
			ExecEvalSQLValueFunction(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CURRENTOFEXPR)
		{
			/* error invocation uses space, and shouldn't ever occur */
			ExecEvalCurrentOfExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_NEXTVALUEEXPR)
		{
			/*
			 * Doesn't seem worthwhile to have an inline implementation
			 * efficiency-wise.
			 */
			ExecEvalNextValueExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_RETURNINGEXPR)
		{
			/*
			 * The next op actually evaluates the expression.  If the OLD/NEW
			 * row doesn't exist, skip that and return NULL.
			 */
			if (state->flags & op->d.returningexpr.nullflag)
			{
				*op->resvalue = (Datum) 0;
				*op->resnull = true;

				EEO_JUMP(op->d.returningexpr.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYEXPR)
		{
			/* too complex for an inline implementation */
			ExecEvalArrayExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ARRAYCOERCE)
		{
			/* too complex for an inline implementation */
			ExecEvalArrayCoerce(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROW)
		{
			/* too complex for an inline implementation */
			ExecEvalRow(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROWCOMPARE_STEP)
		{
			FunctionCallInfo fcinfo = op->d.rowcompare_step.fcinfo_data;
			Datum		d;

			/* force NULL result if strict fn and NULL input */
			if (op->d.rowcompare_step.finfo->fn_strict &&
				(fcinfo->args[0].isnull || fcinfo->args[1].isnull))
			{
				*op->resnull = true;
				EEO_JUMP(op->d.rowcompare_step.jumpnull);
			}

			/* Apply comparison function */
			fcinfo->isnull = false;
			d = op->d.rowcompare_step.fn_addr(fcinfo);
			*op->resvalue = d;

			/* force NULL result if NULL function result */
			if (fcinfo->isnull)
			{
				*op->resnull = true;
				EEO_JUMP(op->d.rowcompare_step.jumpnull);
			}
			*op->resnull = false;

			/* If unequal, no need to compare remaining columns */
			if (DatumGetInt32(*op->resvalue) != 0)
			{
				EEO_JUMP(op->d.rowcompare_step.jumpdone);
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_ROWCOMPARE_FINAL)
		{
			int32		cmpresult = DatumGetInt32(*op->resvalue);
			CompareType cmptype = op->d.rowcompare_final.cmptype;

			*op->resnull = false;
			switch (cmptype)
			{
					/* EQ and NE cases aren't allowed here */
				case COMPARE_LT:
					*op->resvalue = BoolGetDatum(cmpresult < 0);
					break;
				case COMPARE_LE:
					*op->resvalue = BoolGetDatum(cmpresult <= 0);
					break;
				case COMPARE_GE:
					*op->resvalue = BoolGetDatum(cmpresult >= 0);
					break;
				case COMPARE_GT:
					*op->resvalue = BoolGetDatum(cmpresult > 0);
					break;
				default:
					Assert(false);
					break;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MINMAX)
		{
			/* too complex for an inline implementation */
			ExecEvalMinMax(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSELECT)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldSelect(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSTORE_DEFORM)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldStoreDeForm(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_FIELDSTORE_FORM)
		{
			/* too complex for an inline implementation */
			ExecEvalFieldStoreForm(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SBSREF_SUBSCRIPTS)
		{
			/* Precheck SubscriptingRef subscript(s) */
			if (op->d.sbsref_subscript.subscriptfunc(state, op, econtext))
			{
				EEO_NEXT();
			}
			else
			{
				/* Subscript is null, short-circuit SubscriptingRef to NULL */
				EEO_JUMP(op->d.sbsref_subscript.jumpdone);
			}
		}

		EEO_CASE(EEOP_SBSREF_OLD)
			EEO_CASE(EEOP_SBSREF_ASSIGN)
			EEO_CASE(EEOP_SBSREF_FETCH)
		{
			/* Perform a SubscriptingRef fetch or assignment */
			op->d.sbsref.subscriptfunc(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_CONVERT_ROWTYPE)
		{
			/* too complex for an inline implementation */
			ExecEvalConvertRowtype(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SCALARARRAYOP)
		{
			/* too complex for an inline implementation */
			ExecEvalScalarArrayOp(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_HASHED_SCALARARRAYOP)
		{
			/* too complex for an inline implementation */
			ExecEvalHashedScalarArrayOp(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_NOTNULL)
		{
			/* too complex for an inline implementation */
			ExecEvalConstraintNotNull(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_DOMAIN_CHECK)
		{
			/* too complex for an inline implementation */
			ExecEvalConstraintCheck(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_HASHDATUM_SET_INITVAL)
		{
			*op->resvalue = op->d.hashdatum_initvalue.init_value;
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_HASHDATUM_FIRST)
		{
			FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;

			/*
			 * Save the Datum on non-null inputs, otherwise store 0 so that
			 * subsequent NEXT32 operations combine with an initialized value.
			 */
			if (!fcinfo->args[0].isnull)
				*op->resvalue = op->d.hashdatum.fn_addr(fcinfo);
			else
				*op->resvalue = (Datum) 0;

			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_HASHDATUM_FIRST_STRICT)
		{
			FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;

			if (fcinfo->args[0].isnull)
			{
				/*
				 * With strict we have the expression return NULL instead of
				 * ignoring NULL input values.  We've nothing more to do after
				 * finding a NULL.
				 */
				*op->resnull = true;
				*op->resvalue = (Datum) 0;
				EEO_JUMP(op->d.hashdatum.jumpdone);
			}

			/* execute the hash function and save the resulting value */
			*op->resvalue = op->d.hashdatum.fn_addr(fcinfo);
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_HASHDATUM_NEXT32)
		{
			FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;
			uint32		existinghash;

			existinghash = DatumGetUInt32(op->d.hashdatum.iresult->value);
			/* combine successive hash values by rotating */
			existinghash = pg_rotate_left32(existinghash, 1);

			/* leave the hash value alone on NULL inputs */
			if (!fcinfo->args[0].isnull)
			{
				uint32		hashvalue;

				/* execute hash func and combine with previous hash value */
				hashvalue = DatumGetUInt32(op->d.hashdatum.fn_addr(fcinfo));
				existinghash = existinghash ^ hashvalue;
			}

			*op->resvalue = UInt32GetDatum(existinghash);
			*op->resnull = false;

			EEO_NEXT();
		}

		EEO_CASE(EEOP_HASHDATUM_NEXT32_STRICT)
		{
			FunctionCallInfo fcinfo = op->d.hashdatum.fcinfo_data;

			if (fcinfo->args[0].isnull)
			{
				/*
				 * With strict we have the expression return NULL instead of
				 * ignoring NULL input values.  We've nothing more to do after
				 * finding a NULL.
				 */
				*op->resnull = true;
				*op->resvalue = (Datum) 0;
				EEO_JUMP(op->d.hashdatum.jumpdone);
			}
			else
			{
				uint32		existinghash;
				uint32		hashvalue;

				existinghash = DatumGetUInt32(op->d.hashdatum.iresult->value);
				/* combine successive hash values by rotating */
				existinghash = pg_rotate_left32(existinghash, 1);

				/* execute hash func and combine with previous hash value */
				hashvalue = DatumGetUInt32(op->d.hashdatum.fn_addr(fcinfo));
				*op->resvalue = UInt32GetDatum(existinghash ^ hashvalue);
				*op->resnull = false;
			}

			EEO_NEXT();
		}

		EEO_CASE(EEOP_XMLEXPR)
		{
			/* too complex for an inline implementation */
			ExecEvalXmlExpr(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JSON_CONSTRUCTOR)
		{
			/* too complex for an inline implementation */
			ExecEvalJsonConstructor(state, op, econtext);
			EEO_NEXT();
		}

		EEO_CASE(EEOP_IS_JSON)
		{
			/* too complex for an inline implementation */
			ExecEvalJsonIsPredicate(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JSONEXPR_PATH)
		{
			/* too complex for an inline implementation */
			EEO_JUMP(ExecEvalJsonExprPath(state, op, econtext));
		}

		EEO_CASE(EEOP_JSONEXPR_COERCION)
		{
			/* too complex for an inline implementation */
			ExecEvalJsonCoercion(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_JSONEXPR_COERCION_FINISH)
		{
			/* too complex for an inline implementation */
			ExecEvalJsonCoercionFinish(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGGREF)
		{
			/*
			 * Returns a Datum whose value is the precomputed aggregate value
			 * found in the given expression context.
			 */
			int			aggno = op->d.aggref.aggno;

			Assert(econtext->ecxt_aggvalues != NULL);

			*op->resvalue = econtext->ecxt_aggvalues[aggno];
			*op->resnull = econtext->ecxt_aggnulls[aggno];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_GROUPING_FUNC)
		{
			/* too complex/uncommon for an inline implementation */
			ExecEvalGroupingFunc(state, op);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_WINDOW_FUNC)
		{
			/*
			 * Like Aggref, just return a precomputed value from the econtext.
			 */
			WindowFuncExprState *wfunc = op->d.window_func.wfstate;

			Assert(econtext->ecxt_aggvalues != NULL);

			*op->resvalue = econtext->ecxt_aggvalues[wfunc->wfuncno];
			*op->resnull = econtext->ecxt_aggnulls[wfunc->wfuncno];

			EEO_NEXT();
		}

		EEO_CASE(EEOP_MERGE_SUPPORT_FUNC)
		{
			/* too complex/uncommon for an inline implementation */
			ExecEvalMergeSupportFunc(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_SUBPLAN)
		{
			/* too complex for an inline implementation */
			ExecEvalSubPlan(state, op, econtext);

			EEO_NEXT();
		}

		/* evaluate a strict aggregate deserialization function */
		EEO_CASE(EEOP_AGG_STRICT_DESERIALIZE)
		{
			/* Don't call a strict deserialization function with NULL input */
			if (op->d.agg_deserialize.fcinfo_data->args[0].isnull)
				EEO_JUMP(op->d.agg_deserialize.jumpnull);

			/* fallthrough */
		}

		/* evaluate aggregate deserialization function (non-strict portion) */
		EEO_CASE(EEOP_AGG_DESERIALIZE)
		{
			FunctionCallInfo fcinfo = op->d.agg_deserialize.fcinfo_data;
			AggState   *aggstate = castNode(AggState, state->parent);
			MemoryContext oldContext;

			/*
			 * We run the deserialization functions in per-input-tuple memory
			 * context.
			 */
			oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);
			fcinfo->isnull = false;
			*op->resvalue = FunctionCallInvoke(fcinfo);
			*op->resnull = fcinfo->isnull;
			MemoryContextSwitchTo(oldContext);

			EEO_NEXT();
		}

		/*
		 * Check that a strict aggregate transition / combination function's
		 * input is not NULL.
		 */

		EEO_CASE(EEOP_AGG_STRICT_INPUT_CHECK_ARGS)
		{
			NullableDatum *args = op->d.agg_strict_input_check.args;
			int			nargs = op->d.agg_strict_input_check.nargs;

			for (int argno = 0; argno < nargs; argno++)
			{
				if (args[argno].isnull)
					EEO_JUMP(op->d.agg_strict_input_check.jumpnull);
			}
			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGG_STRICT_INPUT_CHECK_NULLS)
		{
			bool	   *nulls = op->d.agg_strict_input_check.nulls;
			int			nargs = op->d.agg_strict_input_check.nargs;

			for (int argno = 0; argno < nargs; argno++)
			{
				if (nulls[argno])
					EEO_JUMP(op->d.agg_strict_input_check.jumpnull);
			}
			EEO_NEXT();
		}

		/*
		 * Check for a NULL pointer to the per-group states.
		 */

		EEO_CASE(EEOP_AGG_PLAIN_PERGROUP_NULLCHECK)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerGroup pergroup_allaggs =
				aggstate->all_pergroups[op->d.agg_plain_pergroup_nullcheck.setoff];

			if (pergroup_allaggs == NULL)
				EEO_JUMP(op->d.agg_plain_pergroup_nullcheck.jumpnull);

			EEO_NEXT();
		}

		/*
		 * Different types of aggregate transition functions are implemented
		 * as different types of steps, to avoid incurring unnecessary
		 * overhead.  There's a step type for each valid combination of having
		 * a by value / by reference transition type, [not] needing to the
		 * initialize the transition value for the first row in a group from
		 * input, and [not] strict transition function.
		 *
		 * Could optimize further by splitting off by-reference for
		 * fixed-length types, but currently that doesn't seem worth it.
		 */

		EEO_CASE(EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(pertrans->transtypeByVal);

			if (pergroup->noTransValue)
			{
				/* If transValue has not yet been initialized, do so now. */
				ExecAggInitGroup(aggstate, pertrans, pergroup,
								 op->d.agg_trans.aggcontext);
				/* copied trans value from input, done this round */
			}
			else if (likely(!pergroup->transValueIsNull))
			{
				/* invoke transition function, unless prevented by strictness */
				ExecAggPlainTransByVal(aggstate, pertrans, pergroup,
									   op->d.agg_trans.aggcontext,
									   op->d.agg_trans.setno);
			}

			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(pertrans->transtypeByVal);

			if (likely(!pergroup->transValueIsNull))
				ExecAggPlainTransByVal(aggstate, pertrans, pergroup,
									   op->d.agg_trans.aggcontext,
									   op->d.agg_trans.setno);

			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_BYVAL)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(pertrans->transtypeByVal);

			ExecAggPlainTransByVal(aggstate, pertrans, pergroup,
								   op->d.agg_trans.aggcontext,
								   op->d.agg_trans.setno);

			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(!pertrans->transtypeByVal);

			if (pergroup->noTransValue)
				ExecAggInitGroup(aggstate, pertrans, pergroup,
								 op->d.agg_trans.aggcontext);
			else if (likely(!pergroup->transValueIsNull))
				ExecAggPlainTransByRef(aggstate, pertrans, pergroup,
									   op->d.agg_trans.aggcontext,
									   op->d.agg_trans.setno);

			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_STRICT_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(!pertrans->transtypeByVal);

			if (likely(!pergroup->transValueIsNull))
				ExecAggPlainTransByRef(aggstate, pertrans, pergroup,
									   op->d.agg_trans.aggcontext,
									   op->d.agg_trans.setno);
			EEO_NEXT();
		}

		/* see comments above EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL */
		EEO_CASE(EEOP_AGG_PLAIN_TRANS_BYREF)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
			AggStatePerGroup pergroup =
				&aggstate->all_pergroups[op->d.agg_trans.setoff][op->d.agg_trans.transno];

			Assert(!pertrans->transtypeByVal);

			ExecAggPlainTransByRef(aggstate, pertrans, pergroup,
								   op->d.agg_trans.aggcontext,
								   op->d.agg_trans.setno);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_AGG_PRESORTED_DISTINCT_SINGLE)
		{
			AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;
			AggState   *aggstate = castNode(AggState, state->parent);

			if (ExecEvalPreOrderedDistinctSingle(aggstate, pertrans))
				EEO_NEXT();
			else
				EEO_JUMP(op->d.agg_presorted_distinctcheck.jumpdistinct);
		}

		EEO_CASE(EEOP_AGG_PRESORTED_DISTINCT_MULTI)
		{
			AggState   *aggstate = castNode(AggState, state->parent);
			AggStatePerTrans pertrans = op->d.agg_presorted_distinctcheck.pertrans;

			if (ExecEvalPreOrderedDistinctMulti(aggstate, pertrans))
				EEO_NEXT();
			else
				EEO_JUMP(op->d.agg_presorted_distinctcheck.jumpdistinct);
		}

		/* process single-column ordered aggregate datum */
		EEO_CASE(EEOP_AGG_ORDERED_TRANS_DATUM)
		{
			/* too complex for an inline implementation */
			ExecEvalAggOrderedTransDatum(state, op, econtext);

			EEO_NEXT();
		}

		/* process multi-column ordered aggregate tuple */
		EEO_CASE(EEOP_AGG_ORDERED_TRANS_TUPLE)
		{
			/* too complex for an inline implementation */
			ExecEvalAggOrderedTransTuple(state, op, econtext);

			EEO_NEXT();
		}

		EEO_CASE(EEOP_LAST)
		{
			/* unreachable */
			Assert(false);
			goto out;
		}
	}

out:
	*isnull = state->resnull;
	return state->resvalue;
}

/*
 * Expression evaluation callback that performs extra checks before executing
 * the expression. Declared extern so other methods of execution can use it
 * too.
 */
Datum
ExecInterpExprStillValid(ExprState *state, ExprContext *econtext, bool *isNull)
{
	/*
	 * First time through, check whether attribute matches Var.  Might not be
	 * ok anymore, due to schema changes.
	 */
	CheckExprStillValid(state, econtext);

	/* skip the check during further executions */
	state->evalfunc = (ExprStateEvalFunc) state->evalfunc_private;

	/* and actually execute */
	return state->evalfunc(state, econtext, isNull);
}

/*
 * Check that an expression is still valid in the face of potential schema
 * changes since the plan has been created.
 */
void
CheckExprStillValid(ExprState *state, ExprContext *econtext)
{
	TupleTableSlot *innerslot;
	TupleTableSlot *outerslot;
	TupleTableSlot *scanslot;
	TupleTableSlot *oldslot;
	TupleTableSlot *newslot;

	innerslot = econtext->ecxt_innertuple;
	outerslot = econtext->ecxt_outertuple;
	scanslot = econtext->ecxt_scantuple;
	oldslot = econtext->ecxt_oldtuple;
	newslot = econtext->ecxt_newtuple;

	for (int i = 0; i < state->steps_len; i++)
	{
		ExprEvalStep *op = &state->steps[i];

		switch (ExecEvalStepOp(state, op))
		{
			case EEOP_INNER_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(innerslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_OUTER_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(outerslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_SCAN_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(scanslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_OLD_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(oldslot, attnum + 1, op->d.var.vartype);
					break;
				}

			case EEOP_NEW_VAR:
				{
					int			attnum = op->d.var.attnum;

					CheckVarSlotCompatibility(newslot, attnum + 1, op->d.var.vartype);
					break;
				}
			default:
				break;
		}
	}
}

/*
 * Check whether a user attribute in a slot can be referenced by a Var
 * expression.  This should succeed unless there have been schema changes
 * since the expression tree has been created.
 */
static void
CheckVarSlotCompatibility(TupleTableSlot *slot, int attnum, Oid vartype)
{
	/*
	 * What we have to check for here is the possibility of an attribute
	 * having been dropped or changed in type since the plan tree was created.
	 * Ideally the plan will get invalidated and not re-used, but just in
	 * case, we keep these defenses.  Fortunately it's sufficient to check
	 * once on the first time through.
	 *
	 * Note: ideally we'd check typmod as well as typid, but that seems
	 * impractical at the moment: in many cases the tupdesc will have been
	 * generated by ExecTypeFromTL(), and that can't guarantee to generate an
	 * accurate typmod in all cases, because some expression node types don't
	 * carry typmod.  Fortunately, for precisely that reason, there should be
	 * no places with a critical dependency on the typmod of a value.
	 *
	 * System attributes don't require checking since their types never
	 * change.
	 */
	if (attnum > 0)
	{
		TupleDesc	slot_tupdesc = slot->tts_tupleDescriptor;
		Form_pg_attribute attr;

		if (attnum > slot_tupdesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 attnum, slot_tupdesc->natts);

		attr = TupleDescAttr(slot_tupdesc, attnum - 1);

		if (attr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("attribute %d of type %s has been dropped",
							attnum, format_type_be(slot_tupdesc->tdtypeid))));

		if (vartype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d of type %s has wrong type",
							attnum, format_type_be(slot_tupdesc->tdtypeid)),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(vartype))));
	}
}

/*
 * Verify that the slot is compatible with a EEOP_*_FETCHSOME operation.
 */
static void
CheckOpSlotCompatibility(ExprEvalStep *op, TupleTableSlot *slot)
{
#ifdef USE_ASSERT_CHECKING
	/* there's nothing to check */
	if (!op->d.fetch.fixed)
		return;

	/*
	 * Should probably fixed at some point, but for now it's easier to allow
	 * buffer and heap tuples to be used interchangeably.
	 */
	if (slot->tts_ops == &TTSOpsBufferHeapTuple &&
		op->d.fetch.kind == &TTSOpsHeapTuple)
		return;
	if (slot->tts_ops == &TTSOpsHeapTuple &&
		op->d.fetch.kind == &TTSOpsBufferHeapTuple)
		return;

	/*
	 * At the moment we consider it OK if a virtual slot is used instead of a
	 * specific type of slot, as a virtual slot never needs to be deformed.
	 */
	if (slot->tts_ops == &TTSOpsVirtual)
		return;

	Assert(op->d.fetch.kind == slot->tts_ops);
#endif
}

/*
 * get_cached_rowtype: utility function to lookup a rowtype tupdesc
 *
 * type_id, typmod: identity of the rowtype
 * rowcache: space for caching identity info
 *		(rowcache->cacheptr must be initialized to NULL)
 * changed: if not NULL, *changed is set to true on any update
 *
 * The returned TupleDesc is not guaranteed pinned; caller must pin it
 * to use it across any operation that might incur cache invalidation,
 * including for example detoasting of input tuples.
 * (The TupleDesc is always refcounted, so just use IncrTupleDescRefCount.)
 *
 * NOTE: because composite types can change contents, we must be prepared
 * to re-do this during any node execution; cannot call just once during
 * expression initialization.
 */
static TupleDesc
get_cached_rowtype(Oid type_id, int32 typmod,
				   ExprEvalRowtypeCache *rowcache,
				   bool *changed)
{
	if (type_id != RECORDOID)
	{
		/*
		 * It's a named composite type, so use the regular typcache.  Do a
		 * lookup first time through, or if the composite type changed.  Note:
		 * "tupdesc_id == 0" may look redundant, but it protects against the
		 * admittedly-theoretical possibility that type_id was RECORDOID the
		 * last time through, so that the cacheptr isn't TypeCacheEntry *.
		 */
		TypeCacheEntry *typentry = (TypeCacheEntry *) rowcache->cacheptr;

		if (unlikely(typentry == NULL ||
					 rowcache->tupdesc_id == 0 ||
					 typentry->tupDesc_identifier != rowcache->tupdesc_id))
		{
			typentry = lookup_type_cache(type_id, TYPECACHE_TUPDESC);
			if (typentry->tupDesc == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_WRONG_OBJECT_TYPE),
						 errmsg("type %s is not composite",
								format_type_be(type_id))));
			rowcache->cacheptr = typentry;
			rowcache->tupdesc_id = typentry->tupDesc_identifier;
			if (changed)
				*changed = true;
		}
		return typentry->tupDesc;
	}
	else
	{
		/*
		 * A RECORD type, once registered, doesn't change for the life of the
		 * backend.  So we don't need a typcache entry as such, which is good
		 * because there isn't one.  It's possible that the caller is asking
		 * about a different type than before, though.
		 */
		TupleDesc	tupDesc = (TupleDesc) rowcache->cacheptr;

		if (unlikely(tupDesc == NULL ||
					 rowcache->tupdesc_id != 0 ||
					 type_id != tupDesc->tdtypeid ||
					 typmod != tupDesc->tdtypmod))
		{
			tupDesc = lookup_rowtype_tupdesc(type_id, typmod);
			/* Drop pin acquired by lookup_rowtype_tupdesc */
			ReleaseTupleDesc(tupDesc);
			rowcache->cacheptr = tupDesc;
			rowcache->tupdesc_id = 0;	/* not a valid value for non-RECORD */
			if (changed)
				*changed = true;
		}
		return tupDesc;
	}
}


/*
 * Fast-path functions, for very simple expressions
 */

/* implementation of ExecJust(Inner|Outer|Scan)Var */
static pg_attribute_always_inline Datum
ExecJustVarImpl(ExprState *state, TupleTableSlot *slot, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.var.attnum + 1;

	CheckOpSlotCompatibility(&state->steps[0], slot);

	/*
	 * Since we use slot_getattr(), we don't need to implement the FETCHSOME
	 * step explicitly, and we also needn't Assert that the attnum is in range
	 * --- slot_getattr() will take care of any problems.
	 */
	return slot_getattr(slot, attnum, isnull);
}

/* Simple reference to inner Var */
static Datum
ExecJustInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Simple reference to outer Var */
static Datum
ExecJustOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_outertuple, isnull);
}

/* Simple reference to scan Var */
static Datum
ExecJustScanVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarImpl(state, econtext->ecxt_scantuple, isnull);
}

/* implementation of ExecJustAssign(Inner|Outer|Scan)Var */
static pg_attribute_always_inline Datum
ExecJustAssignVarImpl(ExprState *state, TupleTableSlot *inslot, bool *isnull)
{
	ExprEvalStep *op = &state->steps[1];
	int			attnum = op->d.assign_var.attnum + 1;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *outslot = state->resultslot;

	CheckOpSlotCompatibility(&state->steps[0], inslot);

	/*
	 * We do not need CheckVarSlotCompatibility here; that was taken care of
	 * at compilation time.
	 *
	 * Since we use slot_getattr(), we don't need to implement the FETCHSOME
	 * step explicitly, and we also needn't Assert that the attnum is in range
	 * --- slot_getattr() will take care of any problems.  Nonetheless, check
	 * that resultnum is in range.
	 */
	Assert(resultnum >= 0 && resultnum < outslot->tts_tupleDescriptor->natts);
	outslot->tts_values[resultnum] =
		slot_getattr(inslot, attnum, &outslot->tts_isnull[resultnum]);
	return 0;
}

/* Evaluate inner Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Evaluate outer Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_outertuple, isnull);
}

/* Evaluate scan Var and assign to appropriate column of result tuple */
static Datum
ExecJustAssignScanVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarImpl(state, econtext->ecxt_scantuple, isnull);
}

/* Evaluate CASE_TESTVAL and apply a strict function to it */
static Datum
ExecJustApplyFuncToCase(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];
	FunctionCallInfo fcinfo;
	NullableDatum *args;
	int			nargs;
	Datum		d;

	/*
	 * XXX with some redesign of the CaseTestExpr mechanism, maybe we could
	 * get rid of this data shuffling?
	 */
	*op->resvalue = *op->d.casetest.value;
	*op->resnull = *op->d.casetest.isnull;

	op++;

	nargs = op->d.func.nargs;
	fcinfo = op->d.func.fcinfo_data;
	args = fcinfo->args;

	/* strict function, so check for NULL args */
	for (int argno = 0; argno < nargs; argno++)
	{
		if (args[argno].isnull)
		{
			*isnull = true;
			return (Datum) 0;
		}
	}
	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*isnull = fcinfo->isnull;
	return d;
}

/* Simple Const expression */
static Datum
ExecJustConst(ExprState *state, ExprContext *econtext, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];

	*isnull = op->d.constval.isnull;
	return op->d.constval.value;
}

/* implementation of ExecJust(Inner|Outer|Scan)VarVirt */
static pg_attribute_always_inline Datum
ExecJustVarVirtImpl(ExprState *state, TupleTableSlot *slot, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];
	int			attnum = op->d.var.attnum;

	/*
	 * As it is guaranteed that a virtual slot is used, there never is a need
	 * to perform tuple deforming (nor would it be possible). Therefore
	 * execExpr.c has not emitted an EEOP_*_FETCHSOME step. Verify, as much as
	 * possible, that that determination was accurate.
	 */
	Assert(TTS_IS_VIRTUAL(slot));
	Assert(TTS_FIXED(slot));
	Assert(attnum >= 0 && attnum < slot->tts_nvalid);

	*isnull = slot->tts_isnull[attnum];

	return slot->tts_values[attnum];
}

/* Like ExecJustInnerVar, optimized for virtual slots */
static Datum
ExecJustInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Like ExecJustOuterVar, optimized for virtual slots */
static Datum
ExecJustOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_outertuple, isnull);
}

/* Like ExecJustScanVar, optimized for virtual slots */
static Datum
ExecJustScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustVarVirtImpl(state, econtext->ecxt_scantuple, isnull);
}

/* implementation of ExecJustAssign(Inner|Outer|Scan)VarVirt */
static pg_attribute_always_inline Datum
ExecJustAssignVarVirtImpl(ExprState *state, TupleTableSlot *inslot, bool *isnull)
{
	ExprEvalStep *op = &state->steps[0];
	int			attnum = op->d.assign_var.attnum;
	int			resultnum = op->d.assign_var.resultnum;
	TupleTableSlot *outslot = state->resultslot;

	/* see ExecJustVarVirtImpl for comments */

	Assert(TTS_IS_VIRTUAL(inslot));
	Assert(TTS_FIXED(inslot));
	Assert(attnum >= 0 && attnum < inslot->tts_nvalid);
	Assert(resultnum >= 0 && resultnum < outslot->tts_tupleDescriptor->natts);

	outslot->tts_values[resultnum] = inslot->tts_values[attnum];
	outslot->tts_isnull[resultnum] = inslot->tts_isnull[attnum];

	return 0;
}

/* Like ExecJustAssignInnerVar, optimized for virtual slots */
static Datum
ExecJustAssignInnerVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Like ExecJustAssignOuterVar, optimized for virtual slots */
static Datum
ExecJustAssignOuterVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_outertuple, isnull);
}

/* Like ExecJustAssignScanVar, optimized for virtual slots */
static Datum
ExecJustAssignScanVarVirt(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustAssignVarVirtImpl(state, econtext->ecxt_scantuple, isnull);
}

/*
 * implementation for hashing an inner Var, seeding with an initial value.
 */
static Datum
ExecJustHashInnerVarWithIV(ExprState *state, ExprContext *econtext,
						   bool *isnull)
{
	ExprEvalStep *fetchop = &state->steps[0];
	ExprEvalStep *setivop = &state->steps[1];
	ExprEvalStep *innervar = &state->steps[2];
	ExprEvalStep *hashop = &state->steps[3];
	FunctionCallInfo fcinfo = hashop->d.hashdatum.fcinfo_data;
	int			attnum = innervar->d.var.attnum;
	uint32		hashkey;

	CheckOpSlotCompatibility(fetchop, econtext->ecxt_innertuple);
	slot_getsomeattrs(econtext->ecxt_innertuple, fetchop->d.fetch.last_var);

	fcinfo->args[0].value = econtext->ecxt_innertuple->tts_values[attnum];
	fcinfo->args[0].isnull = econtext->ecxt_innertuple->tts_isnull[attnum];

	hashkey = DatumGetUInt32(setivop->d.hashdatum_initvalue.init_value);
	hashkey = pg_rotate_left32(hashkey, 1);

	if (!fcinfo->args[0].isnull)
	{
		uint32		hashvalue;

		hashvalue = DatumGetUInt32(hashop->d.hashdatum.fn_addr(fcinfo));
		hashkey = hashkey ^ hashvalue;
	}

	*isnull = false;
	return UInt32GetDatum(hashkey);
}

/* implementation of ExecJustHash(Inner|Outer)Var */
static pg_attribute_always_inline Datum
ExecJustHashVarImpl(ExprState *state, TupleTableSlot *slot, bool *isnull)
{
	ExprEvalStep *fetchop = &state->steps[0];
	ExprEvalStep *var = &state->steps[1];
	ExprEvalStep *hashop = &state->steps[2];
	FunctionCallInfo fcinfo = hashop->d.hashdatum.fcinfo_data;
	int			attnum = var->d.var.attnum;

	CheckOpSlotCompatibility(fetchop, slot);
	slot_getsomeattrs(slot, fetchop->d.fetch.last_var);

	fcinfo->args[0].value = slot->tts_values[attnum];
	fcinfo->args[0].isnull = slot->tts_isnull[attnum];

	*isnull = false;

	if (!fcinfo->args[0].isnull)
		return DatumGetUInt32(hashop->d.hashdatum.fn_addr(fcinfo));
	else
		return (Datum) 0;
}

/* implementation for hashing an outer Var */
static Datum
ExecJustHashOuterVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustHashVarImpl(state, econtext->ecxt_outertuple, isnull);
}

/* implementation for hashing an inner Var */
static Datum
ExecJustHashInnerVar(ExprState *state, ExprContext *econtext, bool *isnull)
{
	return ExecJustHashVarImpl(state, econtext->ecxt_innertuple, isnull);
}

/* implementation of ExecJustHash(Inner|Outer)VarVirt */
static pg_attribute_always_inline Datum
ExecJustHashVarVirtImpl(ExprState *state, TupleTableSlot *slot, bool *isnull)
{
	ExprEvalStep *var = &state->steps[0];
	ExprEvalStep *hashop = &state->steps[1];
	FunctionCallInfo fcinfo = hashop->d.hashdatum.fcinfo_data;
	int			attnum = var->d.var.attnum;

	fcinfo->args[0].value = slot->tts_values[attnum];
	fcinfo->args[0].isnull = slot->tts_isnull[attnum];

	*isnull = false;

	if (!fcinfo->args[0].isnull)
		return DatumGetUInt32(hashop->d.hashdatum.fn_addr(fcinfo));
	else
		return (Datum) 0;
}

/* Like ExecJustHashInnerVar, optimized for virtual slots */
static Datum
ExecJustHashInnerVarVirt(ExprState *state, ExprContext *econtext,
						 bool *isnull)
{
	return ExecJustHashVarVirtImpl(state, econtext->ecxt_innertuple, isnull);
}

/* Like ExecJustHashOuterVar, optimized for virtual slots */
static Datum
ExecJustHashOuterVarVirt(ExprState *state, ExprContext *econtext,
						 bool *isnull)
{
	return ExecJustHashVarVirtImpl(state, econtext->ecxt_outertuple, isnull);
}

/*
 * implementation for hashing an outer Var.  Returns NULL on NULL input.
 */
static Datum
ExecJustHashOuterVarStrict(ExprState *state, ExprContext *econtext,
						   bool *isnull)
{
	ExprEvalStep *fetchop = &state->steps[0];
	ExprEvalStep *var = &state->steps[1];
	ExprEvalStep *hashop = &state->steps[2];
	FunctionCallInfo fcinfo = hashop->d.hashdatum.fcinfo_data;
	int			attnum = var->d.var.attnum;

	CheckOpSlotCompatibility(fetchop, econtext->ecxt_outertuple);
	slot_getsomeattrs(econtext->ecxt_outertuple, fetchop->d.fetch.last_var);

	fcinfo->args[0].value = econtext->ecxt_outertuple->tts_values[attnum];
	fcinfo->args[0].isnull = econtext->ecxt_outertuple->tts_isnull[attnum];

	if (!fcinfo->args[0].isnull)
	{
		*isnull = false;
		return DatumGetUInt32(hashop->d.hashdatum.fn_addr(fcinfo));
	}
	else
	{
		/* return NULL on NULL input */
		*isnull = true;
		return (Datum) 0;
	}
}

#if defined(EEO_USE_COMPUTED_GOTO)
/*
 * Comparator used when building address->opcode lookup table for
 * ExecEvalStepOp() in the threaded dispatch case.
 */
static int
dispatch_compare_ptr(const void *a, const void *b)
{
	const ExprEvalOpLookup *la = (const ExprEvalOpLookup *) a;
	const ExprEvalOpLookup *lb = (const ExprEvalOpLookup *) b;

	if (la->opcode < lb->opcode)
		return -1;
	else if (la->opcode > lb->opcode)
		return 1;
	return 0;
}
#endif

/*
 * Do one-time initialization of interpretation machinery.
 */
static void
ExecInitInterpreter(void)
{
#if defined(EEO_USE_COMPUTED_GOTO)
	/* Set up externally-visible pointer to dispatch table */
	if (dispatch_table == NULL)
	{
		dispatch_table = (const void **)
			DatumGetPointer(ExecInterpExpr(NULL, NULL, NULL));

		/* build reverse lookup table */
		for (int i = 0; i < EEOP_LAST; i++)
		{
			reverse_dispatch_table[i].opcode = dispatch_table[i];
			reverse_dispatch_table[i].op = (ExprEvalOp) i;
		}

		/* make it bsearch()able */
		qsort(reverse_dispatch_table,
			  EEOP_LAST /* nmembers */ ,
			  sizeof(ExprEvalOpLookup),
			  dispatch_compare_ptr);
	}
#endif
}

/*
 * Function to return the opcode of an expression step.
 *
 * When direct-threading is in use, ExprState->opcode isn't easily
 * decipherable. This function returns the appropriate enum member.
 */
ExprEvalOp
ExecEvalStepOp(ExprState *state, ExprEvalStep *op)
{
#if defined(EEO_USE_COMPUTED_GOTO)
	if (state->flags & EEO_FLAG_DIRECT_THREADED)
	{
		ExprEvalOpLookup key;
		ExprEvalOpLookup *res;

		key.opcode = (void *) op->opcode;
		res = bsearch(&key,
					  reverse_dispatch_table,
					  EEOP_LAST /* nmembers */ ,
					  sizeof(ExprEvalOpLookup),
					  dispatch_compare_ptr);
		Assert(res);			/* unknown ops shouldn't get looked up */
		return res->op;
	}
#endif
	return (ExprEvalOp) op->opcode;
}


/*
 * Out-of-line helper functions for complex instructions.
 */

/*
 * Evaluate EEOP_FUNCEXPR_FUSAGE
 */
void
ExecEvalFuncExprFusage(ExprState *state, ExprEvalStep *op,
					   ExprContext *econtext)
{
	FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
	PgStat_FunctionCallUsage fcusage;
	Datum		d;

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*op->resvalue = d;
	*op->resnull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);
}

/*
 * Evaluate EEOP_FUNCEXPR_STRICT_FUSAGE
 */
void
ExecEvalFuncExprStrictFusage(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{

	FunctionCallInfo fcinfo = op->d.func.fcinfo_data;
	PgStat_FunctionCallUsage fcusage;
	NullableDatum *args = fcinfo->args;
	int			nargs = op->d.func.nargs;
	Datum		d;

	/* strict function, so check for NULL args */
	for (int argno = 0; argno < nargs; argno++)
	{
		if (args[argno].isnull)
		{
			*op->resnull = true;
			return;
		}
	}

	pgstat_init_function_usage(fcinfo, &fcusage);

	fcinfo->isnull = false;
	d = op->d.func.fn_addr(fcinfo);
	*op->resvalue = d;
	*op->resnull = fcinfo->isnull;

	pgstat_end_function_usage(&fcusage, true);
}

/*
 * Evaluate a PARAM_EXEC parameter.
 *
 * PARAM_EXEC params (internal executor parameters) are stored in the
 * ecxt_param_exec_vals array, and can be accessed by array index.
 */
void
ExecEvalParamExec(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ParamExecData *prm;

	prm = &(econtext->ecxt_param_exec_vals[op->d.param.paramid]);
	if (unlikely(prm->execPlan != NULL))
	{
		/* Parameter not evaluated yet, so go do it */
		ExecSetParamPlan(prm->execPlan, econtext);
		/* ExecSetParamPlan should have processed this param... */
		Assert(prm->execPlan == NULL);
	}
	*op->resvalue = prm->value;
	*op->resnull = prm->isnull;
}

/*
 * Evaluate a PARAM_EXTERN parameter.
 *
 * PARAM_EXTERN parameters must be sought in ecxt_param_list_info.
 */
void
ExecEvalParamExtern(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ParamListInfo paramInfo = econtext->ecxt_param_list_info;
	int			paramId = op->d.param.paramid;

	if (likely(paramInfo &&
			   paramId > 0 && paramId <= paramInfo->numParams))
	{
		ParamExternData *prm;
		ParamExternData prmdata;

		/* give hook a chance in case parameter is dynamic */
		if (paramInfo->paramFetch != NULL)
			prm = paramInfo->paramFetch(paramInfo, paramId, false, &prmdata);
		else
			prm = &paramInfo->params[paramId - 1];

		if (likely(OidIsValid(prm->ptype)))
		{
			/* safety check in case hook did something unexpected */
			if (unlikely(prm->ptype != op->d.param.paramtype))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("type of parameter %d (%s) does not match that when preparing the plan (%s)",
								paramId,
								format_type_be(prm->ptype),
								format_type_be(op->d.param.paramtype))));
			*op->resvalue = prm->value;
			*op->resnull = prm->isnull;
			return;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("no value found for parameter %d", paramId)));
}

/*
 * Set value of a param (currently always PARAM_EXEC) from
 * state->res{value,null}.
 */
void
ExecEvalParamSet(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ParamExecData *prm;

	prm = &(econtext->ecxt_param_exec_vals[op->d.param.paramid]);

	/* Shouldn't have a pending evaluation anymore */
	Assert(prm->execPlan == NULL);

	prm->value = state->resvalue;
	prm->isnull = state->resnull;
}

/*
 * Evaluate a CoerceViaIO node in soft-error mode.
 *
 * The source value is in op's result variable.
 *
 * Note: This implements EEOP_IOCOERCE_SAFE. If you change anything here,
 * also look at the inline code for EEOP_IOCOERCE.
 */
void
ExecEvalCoerceViaIOSafe(ExprState *state, ExprEvalStep *op)
{
	char	   *str;

	/* call output function (similar to OutputFunctionCall) */
	if (*op->resnull)
	{
		/* output functions are not called on nulls */
		str = NULL;
	}
	else
	{
		FunctionCallInfo fcinfo_out;

		fcinfo_out = op->d.iocoerce.fcinfo_data_out;
		fcinfo_out->args[0].value = *op->resvalue;
		fcinfo_out->args[0].isnull = false;

		fcinfo_out->isnull = false;
		str = DatumGetCString(FunctionCallInvoke(fcinfo_out));

		/* OutputFunctionCall assumes result isn't null */
		Assert(!fcinfo_out->isnull);
	}

	/* call input function (similar to InputFunctionCallSafe) */
	if (!op->d.iocoerce.finfo_in->fn_strict || str != NULL)
	{
		FunctionCallInfo fcinfo_in;

		fcinfo_in = op->d.iocoerce.fcinfo_data_in;
		fcinfo_in->args[0].value = PointerGetDatum(str);
		fcinfo_in->args[0].isnull = *op->resnull;
		/* second and third arguments are already set up */

		/* ErrorSaveContext must be present. */
		Assert(IsA(fcinfo_in->context, ErrorSaveContext));

		fcinfo_in->isnull = false;
		*op->resvalue = FunctionCallInvoke(fcinfo_in);

		if (SOFT_ERROR_OCCURRED(fcinfo_in->context))
		{
			*op->resnull = true;
			*op->resvalue = (Datum) 0;
			return;
		}

		/* Should get null result if and only if str is NULL */
		if (str == NULL)
			Assert(*op->resnull);
		else
			Assert(!*op->resnull);
	}
}

/*
 * Evaluate a SQLValueFunction expression.
 */
void
ExecEvalSQLValueFunction(ExprState *state, ExprEvalStep *op)
{
	LOCAL_FCINFO(fcinfo, 0);
	SQLValueFunction *svf = op->d.sqlvaluefunction.svf;

	*op->resnull = false;

	/*
	 * Note: current_schema() can return NULL.  current_user() etc currently
	 * cannot, but might as well code those cases the same way for safety.
	 */
	switch (svf->op)
	{
		case SVFOP_CURRENT_DATE:
			*op->resvalue = DateADTGetDatum(GetSQLCurrentDate());
			break;
		case SVFOP_CURRENT_TIME:
		case SVFOP_CURRENT_TIME_N:
			*op->resvalue = TimeTzADTPGetDatum(GetSQLCurrentTime(svf->typmod));
			break;
		case SVFOP_CURRENT_TIMESTAMP:
		case SVFOP_CURRENT_TIMESTAMP_N:
			*op->resvalue = TimestampTzGetDatum(GetSQLCurrentTimestamp(svf->typmod));
			break;
		case SVFOP_LOCALTIME:
		case SVFOP_LOCALTIME_N:
			*op->resvalue = TimeADTGetDatum(GetSQLLocalTime(svf->typmod));
			break;
		case SVFOP_LOCALTIMESTAMP:
		case SVFOP_LOCALTIMESTAMP_N:
			*op->resvalue = TimestampGetDatum(GetSQLLocalTimestamp(svf->typmod));
			break;
		case SVFOP_CURRENT_ROLE:
		case SVFOP_CURRENT_USER:
		case SVFOP_USER:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_user(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
		case SVFOP_SESSION_USER:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = session_user(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
		case SVFOP_CURRENT_CATALOG:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_database(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
		case SVFOP_CURRENT_SCHEMA:
			InitFunctionCallInfoData(*fcinfo, NULL, 0, InvalidOid, NULL, NULL);
			*op->resvalue = current_schema(fcinfo);
			*op->resnull = fcinfo->isnull;
			break;
	}
}

/*
 * Raise error if a CURRENT OF expression is evaluated.
 *
 * The planner should convert CURRENT OF into a TidScan qualification, or some
 * other special handling in a ForeignScan node.  So we have to be able to do
 * ExecInitExpr on a CurrentOfExpr, but we shouldn't ever actually execute it.
 * If we get here, we suppose we must be dealing with CURRENT OF on a foreign
 * table whose FDW doesn't handle it, and complain accordingly.
 */
void
ExecEvalCurrentOfExpr(ExprState *state, ExprEvalStep *op)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("WHERE CURRENT OF is not supported for this table type")));
}

/*
 * Evaluate NextValueExpr.
 */
void
ExecEvalNextValueExpr(ExprState *state, ExprEvalStep *op)
{
	int64		newval = nextval_internal(op->d.nextvalueexpr.seqid, false);

	switch (op->d.nextvalueexpr.seqtypid)
	{
		case INT2OID:
			*op->resvalue = Int16GetDatum((int16) newval);
			break;
		case INT4OID:
			*op->resvalue = Int32GetDatum((int32) newval);
			break;
		case INT8OID:
			*op->resvalue = Int64GetDatum((int64) newval);
			break;
		default:
			elog(ERROR, "unsupported sequence type %u",
				 op->d.nextvalueexpr.seqtypid);
	}
	*op->resnull = false;
}

/*
 * Evaluate NullTest / IS NULL for rows.
 */
void
ExecEvalRowNull(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ExecEvalRowNullInt(state, op, econtext, true);
}

/*
 * Evaluate NullTest / IS NOT NULL for rows.
 */
void
ExecEvalRowNotNull(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ExecEvalRowNullInt(state, op, econtext, false);
}

/* Common code for IS [NOT] NULL on a row value */
static void
ExecEvalRowNullInt(ExprState *state, ExprEvalStep *op,
				   ExprContext *econtext, bool checkisnull)
{
	Datum		value = *op->resvalue;
	bool		isnull = *op->resnull;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	HeapTupleData tmptup;

	*op->resnull = false;

	/* NULL row variables are treated just as NULL scalar columns */
	if (isnull)
	{
		*op->resvalue = BoolGetDatum(checkisnull);
		return;
	}

	/*
	 * The SQL standard defines IS [NOT] NULL for a non-null rowtype argument
	 * as:
	 *
	 * "R IS NULL" is true if every field is the null value.
	 *
	 * "R IS NOT NULL" is true if no field is the null value.
	 *
	 * This definition is (apparently intentionally) not recursive; so our
	 * tests on the fields are primitive attisnull tests, not recursive checks
	 * to see if they are all-nulls or no-nulls rowtypes.
	 *
	 * The standard does not consider the possibility of zero-field rows, but
	 * here we consider them to vacuously satisfy both predicates.
	 */

	tuple = DatumGetHeapTupleHeader(value);

	tupType = HeapTupleHeaderGetTypeId(tuple);
	tupTypmod = HeapTupleHeaderGetTypMod(tuple);

	/* Lookup tupdesc if first time through or if type changes */
	tupDesc = get_cached_rowtype(tupType, tupTypmod,
								 &op->d.nulltest_row.rowcache, NULL);

	/*
	 * heap_attisnull needs a HeapTuple not a bare HeapTupleHeader.
	 */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	for (int att = 1; att <= tupDesc->natts; att++)
	{
		/* ignore dropped columns */
		if (TupleDescCompactAttr(tupDesc, att - 1)->attisdropped)
			continue;
		if (heap_attisnull(&tmptup, att, tupDesc))
		{
			/* null field disproves IS NOT NULL */
			if (!checkisnull)
			{
				*op->resvalue = BoolGetDatum(false);
				return;
			}
		}
		else
		{
			/* non-null field disproves IS NULL */
			if (checkisnull)
			{
				*op->resvalue = BoolGetDatum(false);
				return;
			}
		}
	}

	*op->resvalue = BoolGetDatum(true);
}

/*
 * Evaluate an ARRAY[] expression.
 *
 * The individual array elements (or subarrays) have already been evaluated
 * into op->d.arrayexpr.elemvalues[]/elemnulls[].
 */
void
ExecEvalArrayExpr(ExprState *state, ExprEvalStep *op)
{
	ArrayType  *result;
	Oid			element_type = op->d.arrayexpr.elemtype;
	int			nelems = op->d.arrayexpr.nelems;
	int			ndims = 0;
	int			dims[MAXDIM];
	int			lbs[MAXDIM];

	/* Set non-null as default */
	*op->resnull = false;

	if (!op->d.arrayexpr.multidims)
	{
		/* Elements are presumably of scalar type */
		Datum	   *dvalues = op->d.arrayexpr.elemvalues;
		bool	   *dnulls = op->d.arrayexpr.elemnulls;

		/* setup for 1-D array of the given length */
		ndims = 1;
		dims[0] = nelems;
		lbs[0] = 1;

		result = construct_md_array(dvalues, dnulls, ndims, dims, lbs,
									element_type,
									op->d.arrayexpr.elemlength,
									op->d.arrayexpr.elembyval,
									op->d.arrayexpr.elemalign);
	}
	else
	{
		/* Must be nested array expressions */
		int			nbytes = 0;
		int			nitems;
		int			outer_nelems = 0;
		int			elem_ndims = 0;
		int		   *elem_dims = NULL;
		int		   *elem_lbs = NULL;
		bool		firstone = true;
		bool		havenulls = false;
		bool		haveempty = false;
		char	  **subdata;
		bits8	  **subbitmaps;
		int		   *subbytes;
		int		   *subnitems;
		int32		dataoffset;
		char	   *dat;
		int			iitem;

		subdata = (char **) palloc(nelems * sizeof(char *));
		subbitmaps = (bits8 **) palloc(nelems * sizeof(bits8 *));
		subbytes = (int *) palloc(nelems * sizeof(int));
		subnitems = (int *) palloc(nelems * sizeof(int));

		/* loop through and get data area from each element */
		for (int elemoff = 0; elemoff < nelems; elemoff++)
		{
			Datum		arraydatum;
			bool		eisnull;
			ArrayType  *array;
			int			this_ndims;

			arraydatum = op->d.arrayexpr.elemvalues[elemoff];
			eisnull = op->d.arrayexpr.elemnulls[elemoff];

			/* temporarily ignore null subarrays */
			if (eisnull)
			{
				haveempty = true;
				continue;
			}

			array = DatumGetArrayTypeP(arraydatum);

			/* run-time double-check on element type */
			if (element_type != ARR_ELEMTYPE(array))
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot merge incompatible arrays"),
						 errdetail("Array with element type %s cannot be "
								   "included in ARRAY construct with element type %s.",
								   format_type_be(ARR_ELEMTYPE(array)),
								   format_type_be(element_type))));

			this_ndims = ARR_NDIM(array);
			/* temporarily ignore zero-dimensional subarrays */
			if (this_ndims <= 0)
			{
				haveempty = true;
				continue;
			}

			if (firstone)
			{
				/* Get sub-array details from first member */
				elem_ndims = this_ndims;
				ndims = elem_ndims + 1;
				if (ndims <= 0 || ndims > MAXDIM)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("number of array dimensions (%d) exceeds the maximum allowed (%d)",
									ndims, MAXDIM)));

				elem_dims = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_dims, ARR_DIMS(array), elem_ndims * sizeof(int));
				elem_lbs = (int *) palloc(elem_ndims * sizeof(int));
				memcpy(elem_lbs, ARR_LBOUND(array), elem_ndims * sizeof(int));

				firstone = false;
			}
			else
			{
				/* Check other sub-arrays are compatible */
				if (elem_ndims != this_ndims ||
					memcmp(elem_dims, ARR_DIMS(array),
						   elem_ndims * sizeof(int)) != 0 ||
					memcmp(elem_lbs, ARR_LBOUND(array),
						   elem_ndims * sizeof(int)) != 0)
					ereport(ERROR,
							(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
							 errmsg("multidimensional arrays must have array "
									"expressions with matching dimensions")));
			}

			subdata[outer_nelems] = ARR_DATA_PTR(array);
			subbitmaps[outer_nelems] = ARR_NULLBITMAP(array);
			subbytes[outer_nelems] = ARR_SIZE(array) - ARR_DATA_OFFSET(array);
			nbytes += subbytes[outer_nelems];
			/* check for overflow of total request */
			if (!AllocSizeIsValid(nbytes))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("array size exceeds the maximum allowed (%d)",
								(int) MaxAllocSize)));
			subnitems[outer_nelems] = ArrayGetNItems(this_ndims,
													 ARR_DIMS(array));
			havenulls |= ARR_HASNULL(array);
			outer_nelems++;
		}

		/*
		 * If all items were null or empty arrays, return an empty array;
		 * otherwise, if some were and some weren't, raise error.  (Note: we
		 * must special-case this somehow to avoid trying to generate a 1-D
		 * array formed from empty arrays.  It's not ideal...)
		 */
		if (haveempty)
		{
			if (ndims == 0)		/* didn't find any nonempty array */
			{
				*op->resvalue = PointerGetDatum(construct_empty_array(element_type));
				return;
			}
			ereport(ERROR,
					(errcode(ERRCODE_ARRAY_SUBSCRIPT_ERROR),
					 errmsg("multidimensional arrays must have array "
							"expressions with matching dimensions")));
		}

		/* setup for multi-D array */
		dims[0] = outer_nelems;
		lbs[0] = 1;
		for (int i = 1; i < ndims; i++)
		{
			dims[i] = elem_dims[i - 1];
			lbs[i] = elem_lbs[i - 1];
		}

		/* check for subscript overflow */
		nitems = ArrayGetNItems(ndims, dims);
		ArrayCheckBounds(ndims, dims, lbs);

		if (havenulls)
		{
			dataoffset = ARR_OVERHEAD_WITHNULLS(ndims, nitems);
			nbytes += dataoffset;
		}
		else
		{
			dataoffset = 0;		/* marker for no null bitmap */
			nbytes += ARR_OVERHEAD_NONULLS(ndims);
		}

		result = (ArrayType *) palloc0(nbytes);
		SET_VARSIZE(result, nbytes);
		result->ndim = ndims;
		result->dataoffset = dataoffset;
		result->elemtype = element_type;
		memcpy(ARR_DIMS(result), dims, ndims * sizeof(int));
		memcpy(ARR_LBOUND(result), lbs, ndims * sizeof(int));

		dat = ARR_DATA_PTR(result);
		iitem = 0;
		for (int i = 0; i < outer_nelems; i++)
		{
			memcpy(dat, subdata[i], subbytes[i]);
			dat += subbytes[i];
			if (havenulls)
				array_bitmap_copy(ARR_NULLBITMAP(result), iitem,
								  subbitmaps[i], 0,
								  subnitems[i]);
			iitem += subnitems[i];
		}
	}

	*op->resvalue = PointerGetDatum(result);
}

/*
 * Evaluate an ArrayCoerceExpr expression.
 *
 * Source array is in step's result variable.
 */
void
ExecEvalArrayCoerce(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	Datum		arraydatum;

	/* NULL array -> NULL result */
	if (*op->resnull)
		return;

	arraydatum = *op->resvalue;

	/*
	 * If it's binary-compatible, modify the element type in the array header,
	 * but otherwise leave the array as we received it.
	 */
	if (op->d.arraycoerce.elemexprstate == NULL)
	{
		/* Detoast input array if necessary, and copy in any case */
		ArrayType  *array = DatumGetArrayTypePCopy(arraydatum);

		ARR_ELEMTYPE(array) = op->d.arraycoerce.resultelemtype;
		*op->resvalue = PointerGetDatum(array);
		return;
	}

	/*
	 * Use array_map to apply the sub-expression to each array element.
	 */
	*op->resvalue = array_map(arraydatum,
							  op->d.arraycoerce.elemexprstate,
							  econtext,
							  op->d.arraycoerce.resultelemtype,
							  op->d.arraycoerce.amstate);
}

/*
 * Evaluate a ROW() expression.
 *
 * The individual columns have already been evaluated into
 * op->d.row.elemvalues[]/elemnulls[].
 */
void
ExecEvalRow(ExprState *state, ExprEvalStep *op)
{
	HeapTuple	tuple;

	/* build tuple from evaluated field values */
	tuple = heap_form_tuple(op->d.row.tupdesc,
							op->d.row.elemvalues,
							op->d.row.elemnulls);

	*op->resvalue = HeapTupleGetDatum(tuple);
	*op->resnull = false;
}

/*
 * Evaluate GREATEST() or LEAST() expression (note this is *not* MIN()/MAX()).
 *
 * All of the to-be-compared expressions have already been evaluated into
 * op->d.minmax.values[]/nulls[].
 */
void
ExecEvalMinMax(ExprState *state, ExprEvalStep *op)
{
	Datum	   *values = op->d.minmax.values;
	bool	   *nulls = op->d.minmax.nulls;
	FunctionCallInfo fcinfo = op->d.minmax.fcinfo_data;
	MinMaxOp	operator = op->d.minmax.op;

	/* set at initialization */
	Assert(fcinfo->args[0].isnull == false);
	Assert(fcinfo->args[1].isnull == false);

	/* default to null result */
	*op->resnull = true;

	for (int off = 0; off < op->d.minmax.nelems; off++)
	{
		/* ignore NULL inputs */
		if (nulls[off])
			continue;

		if (*op->resnull)
		{
			/* first nonnull input, adopt value */
			*op->resvalue = values[off];
			*op->resnull = false;
		}
		else
		{
			int			cmpresult;

			/* apply comparison function */
			fcinfo->args[0].value = *op->resvalue;
			fcinfo->args[1].value = values[off];

			fcinfo->isnull = false;
			cmpresult = DatumGetInt32(FunctionCallInvoke(fcinfo));
			if (fcinfo->isnull) /* probably should not happen */
				continue;

			if (cmpresult > 0 && operator == IS_LEAST)
				*op->resvalue = values[off];
			else if (cmpresult < 0 && operator == IS_GREATEST)
				*op->resvalue = values[off];
		}
	}
}

/*
 * Evaluate a FieldSelect node.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalFieldSelect(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	AttrNumber	fieldnum = op->d.fieldselect.fieldnum;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	Oid			tupType;
	int32		tupTypmod;
	TupleDesc	tupDesc;
	Form_pg_attribute attr;
	HeapTupleData tmptup;

	/* NULL record -> NULL result */
	if (*op->resnull)
		return;

	tupDatum = *op->resvalue;

	/* We can special-case expanded records for speed */
	if (VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(tupDatum)))
	{
		ExpandedRecordHeader *erh = (ExpandedRecordHeader *) DatumGetEOHP(tupDatum);

		Assert(erh->er_magic == ER_MAGIC);

		/* Extract record's TupleDesc */
		tupDesc = expanded_record_get_tupdesc(erh);

		/*
		 * Find field's attr record.  Note we don't support system columns
		 * here: a datum tuple doesn't have valid values for most of the
		 * interesting system columns anyway.
		 */
		if (fieldnum <= 0)		/* should never happen */
			elog(ERROR, "unsupported reference to system column %d in FieldSelect",
				 fieldnum);
		if (fieldnum > tupDesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 fieldnum, tupDesc->natts);
		attr = TupleDescAttr(tupDesc, fieldnum - 1);

		/* Check for dropped column, and force a NULL result if so */
		if (attr->attisdropped)
		{
			*op->resnull = true;
			return;
		}

		/* Check for type mismatch --- possible after ALTER COLUMN TYPE? */
		/* As in CheckVarSlotCompatibility, we should but can't check typmod */
		if (op->d.fieldselect.resulttype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d has wrong type", fieldnum),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(op->d.fieldselect.resulttype))));

		/* extract the field */
		*op->resvalue = expanded_record_get_field(erh, fieldnum,
												  op->resnull);
	}
	else
	{
		/* Get the composite datum and extract its type fields */
		tuple = DatumGetHeapTupleHeader(tupDatum);

		tupType = HeapTupleHeaderGetTypeId(tuple);
		tupTypmod = HeapTupleHeaderGetTypMod(tuple);

		/* Lookup tupdesc if first time through or if type changes */
		tupDesc = get_cached_rowtype(tupType, tupTypmod,
									 &op->d.fieldselect.rowcache, NULL);

		/*
		 * Find field's attr record.  Note we don't support system columns
		 * here: a datum tuple doesn't have valid values for most of the
		 * interesting system columns anyway.
		 */
		if (fieldnum <= 0)		/* should never happen */
			elog(ERROR, "unsupported reference to system column %d in FieldSelect",
				 fieldnum);
		if (fieldnum > tupDesc->natts)	/* should never happen */
			elog(ERROR, "attribute number %d exceeds number of columns %d",
				 fieldnum, tupDesc->natts);
		attr = TupleDescAttr(tupDesc, fieldnum - 1);

		/* Check for dropped column, and force a NULL result if so */
		if (attr->attisdropped)
		{
			*op->resnull = true;
			return;
		}

		/* Check for type mismatch --- possible after ALTER COLUMN TYPE? */
		/* As in CheckVarSlotCompatibility, we should but can't check typmod */
		if (op->d.fieldselect.resulttype != attr->atttypid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("attribute %d has wrong type", fieldnum),
					 errdetail("Table has type %s, but query expects %s.",
							   format_type_be(attr->atttypid),
							   format_type_be(op->d.fieldselect.resulttype))));

		/* heap_getattr needs a HeapTuple not a bare HeapTupleHeader */
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
		tmptup.t_data = tuple;

		/* extract the field */
		*op->resvalue = heap_getattr(&tmptup,
									 fieldnum,
									 tupDesc,
									 op->resnull);
	}
}

/*
 * Deform source tuple, filling in the step's values/nulls arrays, before
 * evaluating individual new values as part of a FieldStore expression.
 * Subsequent steps will overwrite individual elements of the values/nulls
 * arrays with the new field values, and then FIELDSTORE_FORM will build the
 * new tuple value.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalFieldStoreDeForm(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	if (*op->resnull)
	{
		/* Convert null input tuple into an all-nulls row */
		memset(op->d.fieldstore.nulls, true,
			   op->d.fieldstore.ncolumns * sizeof(bool));
	}
	else
	{
		/*
		 * heap_deform_tuple needs a HeapTuple not a bare HeapTupleHeader. We
		 * set all the fields in the struct just in case.
		 */
		Datum		tupDatum = *op->resvalue;
		HeapTupleHeader tuphdr;
		HeapTupleData tmptup;
		TupleDesc	tupDesc;

		tuphdr = DatumGetHeapTupleHeader(tupDatum);
		tmptup.t_len = HeapTupleHeaderGetDatumLength(tuphdr);
		ItemPointerSetInvalid(&(tmptup.t_self));
		tmptup.t_tableOid = InvalidOid;
		tmptup.t_data = tuphdr;

		/*
		 * Lookup tupdesc if first time through or if type changes.  Because
		 * we don't pin the tupdesc, we must not do this lookup until after
		 * doing DatumGetHeapTupleHeader: that could do database access while
		 * detoasting the datum.
		 */
		tupDesc = get_cached_rowtype(op->d.fieldstore.fstore->resulttype, -1,
									 op->d.fieldstore.rowcache, NULL);

		/* Check that current tupdesc doesn't have more fields than allocated */
		if (unlikely(tupDesc->natts > op->d.fieldstore.ncolumns))
			elog(ERROR, "too many columns in composite type %u",
				 op->d.fieldstore.fstore->resulttype);

		heap_deform_tuple(&tmptup, tupDesc,
						  op->d.fieldstore.values,
						  op->d.fieldstore.nulls);
	}
}

/*
 * Compute the new composite datum after each individual field value of a
 * FieldStore expression has been evaluated.
 */
void
ExecEvalFieldStoreForm(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	TupleDesc	tupDesc;
	HeapTuple	tuple;

	/* Lookup tupdesc (should be valid already) */
	tupDesc = get_cached_rowtype(op->d.fieldstore.fstore->resulttype, -1,
								 op->d.fieldstore.rowcache, NULL);

	tuple = heap_form_tuple(tupDesc,
							op->d.fieldstore.values,
							op->d.fieldstore.nulls);

	*op->resvalue = HeapTupleGetDatum(tuple);
	*op->resnull = false;
}

/*
 * Evaluate a rowtype coercion operation.
 * This may require rearranging field positions.
 *
 * Source record is in step's result variable.
 */
void
ExecEvalConvertRowtype(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	HeapTuple	result;
	Datum		tupDatum;
	HeapTupleHeader tuple;
	HeapTupleData tmptup;
	TupleDesc	indesc,
				outdesc;
	bool		changed = false;

	/* NULL in -> NULL out */
	if (*op->resnull)
		return;

	tupDatum = *op->resvalue;
	tuple = DatumGetHeapTupleHeader(tupDatum);

	/*
	 * Lookup tupdescs if first time through or if type changes.  We'd better
	 * pin them since type conversion functions could do catalog lookups and
	 * hence cause cache invalidation.
	 */
	indesc = get_cached_rowtype(op->d.convert_rowtype.inputtype, -1,
								op->d.convert_rowtype.incache,
								&changed);
	IncrTupleDescRefCount(indesc);
	outdesc = get_cached_rowtype(op->d.convert_rowtype.outputtype, -1,
								 op->d.convert_rowtype.outcache,
								 &changed);
	IncrTupleDescRefCount(outdesc);

	/*
	 * We used to be able to assert that incoming tuples are marked with
	 * exactly the rowtype of indesc.  However, now that ExecEvalWholeRowVar
	 * might change the tuples' marking to plain RECORD due to inserting
	 * aliases, we can only make this weak test:
	 */
	Assert(HeapTupleHeaderGetTypeId(tuple) == indesc->tdtypeid ||
		   HeapTupleHeaderGetTypeId(tuple) == RECORDOID);

	/* if first time through, or after change, initialize conversion map */
	if (changed)
	{
		MemoryContext old_cxt;

		/* allocate map in long-lived memory context */
		old_cxt = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		/* prepare map from old to new attribute numbers */
		op->d.convert_rowtype.map = convert_tuples_by_name(indesc, outdesc);

		MemoryContextSwitchTo(old_cxt);
	}

	/* Following steps need a HeapTuple not a bare HeapTupleHeader */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(tuple);
	tmptup.t_data = tuple;

	if (op->d.convert_rowtype.map != NULL)
	{
		/* Full conversion with attribute rearrangement needed */
		result = execute_attr_map_tuple(&tmptup, op->d.convert_rowtype.map);
		/* Result already has appropriate composite-datum header fields */
		*op->resvalue = HeapTupleGetDatum(result);
	}
	else
	{
		/*
		 * The tuple is physically compatible as-is, but we need to insert the
		 * destination rowtype OID in its composite-datum header field, so we
		 * have to copy it anyway.  heap_copy_tuple_as_datum() is convenient
		 * for this since it will both make the physical copy and insert the
		 * correct composite header fields.  Note that we aren't expecting to
		 * have to flatten any toasted fields: the input was a composite
		 * datum, so it shouldn't contain any.  So heap_copy_tuple_as_datum()
		 * is overkill here, but its check for external fields is cheap.
		 */
		*op->resvalue = heap_copy_tuple_as_datum(&tmptup, outdesc);
	}

	DecrTupleDescRefCount(indesc);
	DecrTupleDescRefCount(outdesc);
}

/*
 * Evaluate "scalar op ANY/ALL (array)".
 *
 * Source array is in our result area, scalar arg is already evaluated into
 * fcinfo->args[0].
 *
 * The operator always yields boolean, and we combine the results across all
 * array elements using OR and AND (for ANY and ALL respectively).  Of course
 * we short-circuit as soon as the result is known.
 */
void
ExecEvalScalarArrayOp(ExprState *state, ExprEvalStep *op)
{
	FunctionCallInfo fcinfo = op->d.scalararrayop.fcinfo_data;
	bool		useOr = op->d.scalararrayop.useOr;
	bool		strictfunc = op->d.scalararrayop.finfo->fn_strict;
	ArrayType  *arr;
	int			nitems;
	Datum		result;
	bool		resultnull;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char	   *s;
	bits8	   *bitmap;
	int			bitmask;

	/*
	 * If the array is NULL then we return NULL --- it's not very meaningful
	 * to do anything else, even if the operator isn't strict.
	 */
	if (*op->resnull)
		return;

	/* Else okay to fetch and detoast the array */
	arr = DatumGetArrayTypeP(*op->resvalue);

	/*
	 * If the array is empty, we return either FALSE or TRUE per the useOr
	 * flag.  This is correct even if the scalar is NULL; since we would
	 * evaluate the operator zero times, it matters not whether it would want
	 * to return NULL.
	 */
	nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));
	if (nitems <= 0)
	{
		*op->resvalue = BoolGetDatum(!useOr);
		*op->resnull = false;
		return;
	}

	/*
	 * If the scalar is NULL, and the function is strict, return NULL; no
	 * point in iterating the loop.
	 */
	if (fcinfo->args[0].isnull && strictfunc)
	{
		*op->resnull = true;
		return;
	}

	/*
	 * We arrange to look up info about the element type only once per series
	 * of calls, assuming the element type doesn't change underneath us.
	 */
	if (op->d.scalararrayop.element_type != ARR_ELEMTYPE(arr))
	{
		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &op->d.scalararrayop.typlen,
							 &op->d.scalararrayop.typbyval,
							 &op->d.scalararrayop.typalign);
		op->d.scalararrayop.element_type = ARR_ELEMTYPE(arr);
	}

	typlen = op->d.scalararrayop.typlen;
	typbyval = op->d.scalararrayop.typbyval;
	typalign = op->d.scalararrayop.typalign;

	/* Initialize result appropriately depending on useOr */
	result = BoolGetDatum(!useOr);
	resultnull = false;

	/* Loop over the array elements */
	s = (char *) ARR_DATA_PTR(arr);
	bitmap = ARR_NULLBITMAP(arr);
	bitmask = 1;

	for (int i = 0; i < nitems; i++)
	{
		Datum		elt;
		Datum		thisresult;

		/* Get array element, checking for NULL */
		if (bitmap && (*bitmap & bitmask) == 0)
		{
			fcinfo->args[1].value = (Datum) 0;
			fcinfo->args[1].isnull = true;
		}
		else
		{
			elt = fetch_att(s, typbyval, typlen);
			s = att_addlength_pointer(s, typlen, s);
			s = (char *) att_align_nominal(s, typalign);
			fcinfo->args[1].value = elt;
			fcinfo->args[1].isnull = false;
		}

		/* Call comparison function */
		if (fcinfo->args[1].isnull && strictfunc)
		{
			fcinfo->isnull = true;
			thisresult = (Datum) 0;
		}
		else
		{
			fcinfo->isnull = false;
			thisresult = op->d.scalararrayop.fn_addr(fcinfo);
		}

		/* Combine results per OR or AND semantics */
		if (fcinfo->isnull)
			resultnull = true;
		else if (useOr)
		{
			if (DatumGetBool(thisresult))
			{
				result = BoolGetDatum(true);
				resultnull = false;
				break;			/* needn't look at any more elements */
			}
		}
		else
		{
			if (!DatumGetBool(thisresult))
			{
				result = BoolGetDatum(false);
				resultnull = false;
				break;			/* needn't look at any more elements */
			}
		}

		/* advance bitmap pointer if any */
		if (bitmap)
		{
			bitmask <<= 1;
			if (bitmask == 0x100)
			{
				bitmap++;
				bitmask = 1;
			}
		}
	}

	*op->resvalue = result;
	*op->resnull = resultnull;
}

/*
 * Hash function for scalar array hash op elements.
 *
 * We use the element type's default hash opclass, and the column collation
 * if the type is collation-sensitive.
 */
static uint32
saop_element_hash(struct saophash_hash *tb, Datum key)
{
	ScalarArrayOpExprHashTable *elements_tab = (ScalarArrayOpExprHashTable *) tb->private_data;
	FunctionCallInfo fcinfo = &elements_tab->hash_fcinfo_data;
	Datum		hash;

	fcinfo->args[0].value = key;
	fcinfo->args[0].isnull = false;

	hash = elements_tab->hash_finfo.fn_addr(fcinfo);

	return DatumGetUInt32(hash);
}

/*
 * Matching function for scalar array hash op elements, to be used in hashtable
 * lookups.
 */
static bool
saop_hash_element_match(struct saophash_hash *tb, Datum key1, Datum key2)
{
	Datum		result;

	ScalarArrayOpExprHashTable *elements_tab = (ScalarArrayOpExprHashTable *) tb->private_data;
	FunctionCallInfo fcinfo = elements_tab->op->d.hashedscalararrayop.fcinfo_data;

	fcinfo->args[0].value = key1;
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = key2;
	fcinfo->args[1].isnull = false;

	result = elements_tab->op->d.hashedscalararrayop.finfo->fn_addr(fcinfo);

	return DatumGetBool(result);
}

/*
 * Evaluate "scalar op ANY (const array)".
 *
 * Similar to ExecEvalScalarArrayOp, but optimized for faster repeat lookups
 * by building a hashtable on the first lookup.  This hashtable will be reused
 * by subsequent lookups.  Unlike ExecEvalScalarArrayOp, this version only
 * supports OR semantics.
 *
 * Source array is in our result area, scalar arg is already evaluated into
 * fcinfo->args[0].
 *
 * The operator always yields boolean.
 */
void
ExecEvalHashedScalarArrayOp(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	ScalarArrayOpExprHashTable *elements_tab = op->d.hashedscalararrayop.elements_tab;
	FunctionCallInfo fcinfo = op->d.hashedscalararrayop.fcinfo_data;
	bool		inclause = op->d.hashedscalararrayop.inclause;
	bool		strictfunc = op->d.hashedscalararrayop.finfo->fn_strict;
	Datum		scalar = fcinfo->args[0].value;
	bool		scalar_isnull = fcinfo->args[0].isnull;
	Datum		result;
	bool		resultnull;
	bool		hashfound;

	/* We don't setup a hashed scalar array op if the array const is null. */
	Assert(!*op->resnull);

	/*
	 * If the scalar is NULL, and the function is strict, return NULL; no
	 * point in executing the search.
	 */
	if (fcinfo->args[0].isnull && strictfunc)
	{
		*op->resnull = true;
		return;
	}

	/* Build the hash table on first evaluation */
	if (elements_tab == NULL)
	{
		ScalarArrayOpExpr *saop;
		int16		typlen;
		bool		typbyval;
		char		typalign;
		int			nitems;
		bool		has_nulls = false;
		char	   *s;
		bits8	   *bitmap;
		int			bitmask;
		MemoryContext oldcontext;
		ArrayType  *arr;

		saop = op->d.hashedscalararrayop.saop;

		arr = DatumGetArrayTypeP(*op->resvalue);
		nitems = ArrayGetNItems(ARR_NDIM(arr), ARR_DIMS(arr));

		get_typlenbyvalalign(ARR_ELEMTYPE(arr),
							 &typlen,
							 &typbyval,
							 &typalign);

		oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);

		elements_tab = (ScalarArrayOpExprHashTable *)
			palloc0(offsetof(ScalarArrayOpExprHashTable, hash_fcinfo_data) +
					SizeForFunctionCallInfo(1));
		op->d.hashedscalararrayop.elements_tab = elements_tab;
		elements_tab->op = op;

		fmgr_info(saop->hashfuncid, &elements_tab->hash_finfo);
		fmgr_info_set_expr((Node *) saop, &elements_tab->hash_finfo);

		InitFunctionCallInfoData(elements_tab->hash_fcinfo_data,
								 &elements_tab->hash_finfo,
								 1,
								 saop->inputcollid,
								 NULL,
								 NULL);

		/*
		 * Create the hash table sizing it according to the number of elements
		 * in the array.  This does assume that the array has no duplicates.
		 * If the array happens to contain many duplicate values then it'll
		 * just mean that we sized the table a bit on the large side.
		 */
		elements_tab->hashtab = saophash_create(CurrentMemoryContext, nitems,
												elements_tab);

		MemoryContextSwitchTo(oldcontext);

		s = (char *) ARR_DATA_PTR(arr);
		bitmap = ARR_NULLBITMAP(arr);
		bitmask = 1;
		for (int i = 0; i < nitems; i++)
		{
			/* Get array element, checking for NULL. */
			if (bitmap && (*bitmap & bitmask) == 0)
			{
				has_nulls = true;
			}
			else
			{
				Datum		element;

				element = fetch_att(s, typbyval, typlen);
				s = att_addlength_pointer(s, typlen, s);
				s = (char *) att_align_nominal(s, typalign);

				saophash_insert(elements_tab->hashtab, element, &hashfound);
			}

			/* Advance bitmap pointer if any. */
			if (bitmap)
			{
				bitmask <<= 1;
				if (bitmask == 0x100)
				{
					bitmap++;
					bitmask = 1;
				}
			}
		}

		/*
		 * Remember if we had any nulls so that we know if we need to execute
		 * non-strict functions with a null lhs value if no match is found.
		 */
		op->d.hashedscalararrayop.has_nulls = has_nulls;
	}

	/* Check the hash to see if we have a match. */
	hashfound = NULL != saophash_lookup(elements_tab->hashtab, scalar);

	/* the result depends on if the clause is an IN or NOT IN clause */
	if (inclause)
		result = BoolGetDatum(hashfound);	/* IN */
	else
		result = BoolGetDatum(!hashfound);	/* NOT IN */

	resultnull = false;

	/*
	 * If we didn't find a match in the array, we still might need to handle
	 * the possibility of null values.  We didn't put any NULLs into the
	 * hashtable, but instead marked if we found any when building the table
	 * in has_nulls.
	 */
	if (!hashfound && op->d.hashedscalararrayop.has_nulls)
	{
		if (strictfunc)
		{

			/*
			 * We have nulls in the array so a non-null lhs and no match must
			 * yield NULL.
			 */
			result = (Datum) 0;
			resultnull = true;
		}
		else
		{
			/*
			 * Execute function will null rhs just once.
			 *
			 * The hash lookup path will have scribbled on the lhs argument so
			 * we need to set it up also (even though we entered this function
			 * with it already set).
			 */
			fcinfo->args[0].value = scalar;
			fcinfo->args[0].isnull = scalar_isnull;
			fcinfo->args[1].value = (Datum) 0;
			fcinfo->args[1].isnull = true;

			result = op->d.hashedscalararrayop.finfo->fn_addr(fcinfo);
			resultnull = fcinfo->isnull;

			/*
			 * Reverse the result for NOT IN clauses since the above function
			 * is the equality function and we need not-equals.
			 */
			if (!inclause)
				result = !result;
		}
	}

	*op->resvalue = result;
	*op->resnull = resultnull;
}

/*
 * Evaluate a NOT NULL domain constraint.
 */
void
ExecEvalConstraintNotNull(ExprState *state, ExprEvalStep *op)
{
	if (*op->resnull)
		errsave((Node *) op->d.domaincheck.escontext,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("domain %s does not allow null values",
						format_type_be(op->d.domaincheck.resulttype)),
				 errdatatype(op->d.domaincheck.resulttype)));
}

/*
 * Evaluate a CHECK domain constraint.
 */
void
ExecEvalConstraintCheck(ExprState *state, ExprEvalStep *op)
{
	if (!*op->d.domaincheck.checknull &&
		!DatumGetBool(*op->d.domaincheck.checkvalue))
		errsave((Node *) op->d.domaincheck.escontext,
				(errcode(ERRCODE_CHECK_VIOLATION),
				 errmsg("value for domain %s violates check constraint \"%s\"",
						format_type_be(op->d.domaincheck.resulttype),
						op->d.domaincheck.constraintname),
				 errdomainconstraint(op->d.domaincheck.resulttype,
									 op->d.domaincheck.constraintname)));
}

/*
 * Evaluate the various forms of XmlExpr.
 *
 * Arguments have been evaluated into named_argvalue/named_argnull
 * and/or argvalue/argnull arrays.
 */
void
ExecEvalXmlExpr(ExprState *state, ExprEvalStep *op)
{
	XmlExpr    *xexpr = op->d.xmlexpr.xexpr;
	Datum		value;

	*op->resnull = true;		/* until we get a result */
	*op->resvalue = (Datum) 0;

	switch (xexpr->op)
	{
		case IS_XMLCONCAT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				List	   *values = NIL;

				for (int i = 0; i < list_length(xexpr->args); i++)
				{
					if (!argnull[i])
						values = lappend(values, DatumGetPointer(argvalue[i]));
				}

				if (values != NIL)
				{
					*op->resvalue = PointerGetDatum(xmlconcat(values));
					*op->resnull = false;
				}
			}
			break;

		case IS_XMLFOREST:
			{
				Datum	   *argvalue = op->d.xmlexpr.named_argvalue;
				bool	   *argnull = op->d.xmlexpr.named_argnull;
				StringInfoData buf;
				ListCell   *lc;
				ListCell   *lc2;
				int			i;

				initStringInfo(&buf);

				i = 0;
				forboth(lc, xexpr->named_args, lc2, xexpr->arg_names)
				{
					Expr	   *e = (Expr *) lfirst(lc);
					char	   *argname = strVal(lfirst(lc2));

					if (!argnull[i])
					{
						value = argvalue[i];
						appendStringInfo(&buf, "<%s>%s</%s>",
										 argname,
										 map_sql_value_to_xml_value(value,
																	exprType((Node *) e), true),
										 argname);
						*op->resnull = false;
					}
					i++;
				}

				if (!*op->resnull)
				{
					text	   *result;

					result = cstring_to_text_with_len(buf.data, buf.len);
					*op->resvalue = PointerGetDatum(result);
				}

				pfree(buf.data);
			}
			break;

		case IS_XMLELEMENT:
			*op->resvalue = PointerGetDatum(xmlelement(xexpr,
													   op->d.xmlexpr.named_argvalue,
													   op->d.xmlexpr.named_argnull,
													   op->d.xmlexpr.argvalue,
													   op->d.xmlexpr.argnull));
			*op->resnull = false;
			break;

		case IS_XMLPARSE:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				text	   *data;
				bool		preserve_whitespace;

				/* arguments are known to be text, bool */
				Assert(list_length(xexpr->args) == 2);

				if (argnull[0])
					return;
				value = argvalue[0];
				data = DatumGetTextPP(value);

				if (argnull[1]) /* probably can't happen */
					return;
				value = argvalue[1];
				preserve_whitespace = DatumGetBool(value);

				*op->resvalue = PointerGetDatum(xmlparse(data,
														 xexpr->xmloption,
														 preserve_whitespace));
				*op->resnull = false;
			}
			break;

		case IS_XMLPI:
			{
				text	   *arg;
				bool		isnull;

				/* optional argument is known to be text */
				Assert(list_length(xexpr->args) <= 1);

				if (xexpr->args)
				{
					isnull = op->d.xmlexpr.argnull[0];
					if (isnull)
						arg = NULL;
					else
						arg = DatumGetTextPP(op->d.xmlexpr.argvalue[0]);
				}
				else
				{
					arg = NULL;
					isnull = false;
				}

				*op->resvalue = PointerGetDatum(xmlpi(xexpr->name,
													  arg,
													  isnull,
													  op->resnull));
			}
			break;

		case IS_XMLROOT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;
				xmltype    *data;
				text	   *version;
				int			standalone;

				/* arguments are known to be xml, text, int */
				Assert(list_length(xexpr->args) == 3);

				if (argnull[0])
					return;
				data = DatumGetXmlP(argvalue[0]);

				if (argnull[1])
					version = NULL;
				else
					version = DatumGetTextPP(argvalue[1]);

				Assert(!argnull[2]);	/* always present */
				standalone = DatumGetInt32(argvalue[2]);

				*op->resvalue = PointerGetDatum(xmlroot(data,
														version,
														standalone));
				*op->resnull = false;
			}
			break;

		case IS_XMLSERIALIZE:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;

				/* argument type is known to be xml */
				Assert(list_length(xexpr->args) == 1);

				if (argnull[0])
					return;
				value = argvalue[0];

				*op->resvalue =
					PointerGetDatum(xmltotext_with_options(DatumGetXmlP(value),
														   xexpr->xmloption,
														   xexpr->indent));
				*op->resnull = false;
			}
			break;

		case IS_DOCUMENT:
			{
				Datum	   *argvalue = op->d.xmlexpr.argvalue;
				bool	   *argnull = op->d.xmlexpr.argnull;

				/* optional argument is known to be xml */
				Assert(list_length(xexpr->args) == 1);

				if (argnull[0])
					return;
				value = argvalue[0];

				*op->resvalue =
					BoolGetDatum(xml_is_document(DatumGetXmlP(value)));
				*op->resnull = false;
			}
			break;

		default:
			elog(ERROR, "unrecognized XML operation");
			break;
	}
}

/*
 * Evaluate a JSON constructor expression.
 */
void
ExecEvalJsonConstructor(ExprState *state, ExprEvalStep *op,
						ExprContext *econtext)
{
	Datum		res;
	JsonConstructorExprState *jcstate = op->d.json_constructor.jcstate;
	JsonConstructorExpr *ctor = jcstate->constructor;
	bool		is_jsonb = ctor->returning->format->format_type == JS_FORMAT_JSONB;
	bool		isnull = false;

	if (ctor->type == JSCTOR_JSON_ARRAY)
		res = (is_jsonb ?
			   jsonb_build_array_worker :
			   json_build_array_worker) (jcstate->nargs,
										 jcstate->arg_values,
										 jcstate->arg_nulls,
										 jcstate->arg_types,
										 jcstate->constructor->absent_on_null);
	else if (ctor->type == JSCTOR_JSON_OBJECT)
		res = (is_jsonb ?
			   jsonb_build_object_worker :
			   json_build_object_worker) (jcstate->nargs,
										  jcstate->arg_values,
										  jcstate->arg_nulls,
										  jcstate->arg_types,
										  jcstate->constructor->absent_on_null,
										  jcstate->constructor->unique);
	else if (ctor->type == JSCTOR_JSON_SCALAR)
	{
		if (jcstate->arg_nulls[0])
		{
			res = (Datum) 0;
			isnull = true;
		}
		else
		{
			Datum		value = jcstate->arg_values[0];
			Oid			outfuncid = jcstate->arg_type_cache[0].outfuncid;
			JsonTypeCategory category = (JsonTypeCategory)
				jcstate->arg_type_cache[0].category;

			if (is_jsonb)
				res = datum_to_jsonb(value, category, outfuncid);
			else
				res = datum_to_json(value, category, outfuncid);
		}
	}
	else if (ctor->type == JSCTOR_JSON_PARSE)
	{
		if (jcstate->arg_nulls[0])
		{
			res = (Datum) 0;
			isnull = true;
		}
		else
		{
			Datum		value = jcstate->arg_values[0];
			text	   *js = DatumGetTextP(value);

			if (is_jsonb)
				res = jsonb_from_text(js, true);
			else
			{
				(void) json_validate(js, true, true);
				res = value;
			}
		}
	}
	else
		elog(ERROR, "invalid JsonConstructorExpr type %d", ctor->type);

	*op->resvalue = res;
	*op->resnull = isnull;
}

/*
 * Evaluate a IS JSON predicate.
 */
void
ExecEvalJsonIsPredicate(ExprState *state, ExprEvalStep *op)
{
	JsonIsPredicate *pred = op->d.is_json.pred;
	Datum		js = *op->resvalue;
	Oid			exprtype;
	bool		res;

	if (*op->resnull)
	{
		*op->resvalue = BoolGetDatum(false);
		return;
	}

	exprtype = exprType(pred->expr);

	if (exprtype == TEXTOID || exprtype == JSONOID)
	{
		text	   *json = DatumGetTextP(js);

		if (pred->item_type == JS_TYPE_ANY)
			res = true;
		else
		{
			switch (json_get_first_token(json, false))
			{
				case JSON_TOKEN_OBJECT_START:
					res = pred->item_type == JS_TYPE_OBJECT;
					break;
				case JSON_TOKEN_ARRAY_START:
					res = pred->item_type == JS_TYPE_ARRAY;
					break;
				case JSON_TOKEN_STRING:
				case JSON_TOKEN_NUMBER:
				case JSON_TOKEN_TRUE:
				case JSON_TOKEN_FALSE:
				case JSON_TOKEN_NULL:
					res = pred->item_type == JS_TYPE_SCALAR;
					break;
				default:
					res = false;
					break;
			}
		}

		/*
		 * Do full parsing pass only for uniqueness check or for JSON text
		 * validation.
		 */
		if (res && (pred->unique_keys || exprtype == TEXTOID))
			res = json_validate(json, pred->unique_keys, false);
	}
	else if (exprtype == JSONBOID)
	{
		if (pred->item_type == JS_TYPE_ANY)
			res = true;
		else
		{
			Jsonb	   *jb = DatumGetJsonbP(js);

			switch (pred->item_type)
			{
				case JS_TYPE_OBJECT:
					res = JB_ROOT_IS_OBJECT(jb);
					break;
				case JS_TYPE_ARRAY:
					res = JB_ROOT_IS_ARRAY(jb) && !JB_ROOT_IS_SCALAR(jb);
					break;
				case JS_TYPE_SCALAR:
					res = JB_ROOT_IS_ARRAY(jb) && JB_ROOT_IS_SCALAR(jb);
					break;
				default:
					res = false;
					break;
			}
		}

		/* Key uniqueness check is redundant for jsonb */
	}
	else
		res = false;

	*op->resvalue = BoolGetDatum(res);
}

/*
 * Evaluate a jsonpath against a document, both of which must have been
 * evaluated and their values saved in op->d.jsonexpr.jsestate.
 *
 * If an error occurs during JsonPath* evaluation or when coercing its result
 * to the RETURNING type, JsonExprState.error is set to true, provided the
 * ON ERROR behavior is not ERROR.  Similarly, if JsonPath{Query|Value}() found
 * no matching items, JsonExprState.empty is set to true, provided the ON EMPTY
 * behavior is not ERROR.  That is to signal to the subsequent steps that check
 * those flags to return the ON ERROR / ON EMPTY expression.
 *
 * Return value is the step address to be performed next.  It will be one of
 * jump_error, jump_empty, jump_eval_coercion, or jump_end, all given in
 * op->d.jsonexpr.jsestate.
 */
int
ExecEvalJsonExprPath(ExprState *state, ExprEvalStep *op,
					 ExprContext *econtext)
{
	JsonExprState *jsestate = op->d.jsonexpr.jsestate;
	JsonExpr   *jsexpr = jsestate->jsexpr;
	Datum		item;
	JsonPath   *path;
	bool		throw_error = jsexpr->on_error->btype == JSON_BEHAVIOR_ERROR;
	bool		error = false,
				empty = false;
	int			jump_eval_coercion = jsestate->jump_eval_coercion;
	char	   *val_string = NULL;

	item = jsestate->formatted_expr.value;
	path = DatumGetJsonPathP(jsestate->pathspec.value);

	/* Set error/empty to false. */
	memset(&jsestate->error, 0, sizeof(NullableDatum));
	memset(&jsestate->empty, 0, sizeof(NullableDatum));

	/* Also reset ErrorSaveContext contents for the next row. */
	if (jsestate->escontext.details_wanted)
	{
		jsestate->escontext.error_data = NULL;
		jsestate->escontext.details_wanted = false;
	}
	jsestate->escontext.error_occurred = false;

	switch (jsexpr->op)
	{
		case JSON_EXISTS_OP:
			{
				bool		exists = JsonPathExists(item, path,
													!throw_error ? &error : NULL,
													jsestate->args);

				if (!error)
				{
					*op->resnull = false;
					*op->resvalue = BoolGetDatum(exists);
				}
			}
			break;

		case JSON_QUERY_OP:
			*op->resvalue = JsonPathQuery(item, path, jsexpr->wrapper, &empty,
										  !throw_error ? &error : NULL,
										  jsestate->args,
										  jsexpr->column_name);

			*op->resnull = (DatumGetPointer(*op->resvalue) == NULL);
			break;

		case JSON_VALUE_OP:
			{
				JsonbValue *jbv = JsonPathValue(item, path, &empty,
												!throw_error ? &error : NULL,
												jsestate->args,
												jsexpr->column_name);

				if (jbv == NULL)
				{
					/* Will be coerced with json_populate_type(), if needed. */
					*op->resvalue = (Datum) 0;
					*op->resnull = true;
				}
				else if (!error && !empty)
				{
					if (jsexpr->returning->typid == JSONOID ||
						jsexpr->returning->typid == JSONBOID)
					{
						val_string = DatumGetCString(DirectFunctionCall1(jsonb_out,
																		 JsonbPGetDatum(JsonbValueToJsonb(jbv))));
					}
					else if (jsexpr->use_json_coercion)
					{
						*op->resvalue = JsonbPGetDatum(JsonbValueToJsonb(jbv));
						*op->resnull = false;
					}
					else
					{
						val_string = ExecGetJsonValueItemString(jbv, op->resnull);

						/*
						 * Simply convert to the default RETURNING type (text)
						 * if no coercion needed.
						 */
						if (!jsexpr->use_io_coercion)
							*op->resvalue = DirectFunctionCall1(textin,
																CStringGetDatum(val_string));
					}
				}
				break;
			}

			/* JSON_TABLE_OP can't happen here */

		default:
			elog(ERROR, "unrecognized SQL/JSON expression op %d",
				 (int) jsexpr->op);
			return false;
	}

	/*
	 * Coerce the result value to the RETURNING type by calling its input
	 * function.
	 */
	if (!*op->resnull && jsexpr->use_io_coercion)
	{
		FunctionCallInfo fcinfo;

		Assert(jump_eval_coercion == -1);
		fcinfo = jsestate->input_fcinfo;
		Assert(fcinfo != NULL);
		Assert(val_string != NULL);
		fcinfo->args[0].value = PointerGetDatum(val_string);
		fcinfo->args[0].isnull = *op->resnull;

		/*
		 * Second and third arguments are already set up in
		 * ExecInitJsonExpr().
		 */

		fcinfo->isnull = false;
		*op->resvalue = FunctionCallInvoke(fcinfo);
		if (SOFT_ERROR_OCCURRED(&jsestate->escontext))
			error = true;
	}

	/*
	 * When setting up the ErrorSaveContext (if needed) for capturing the
	 * errors that occur when coercing the JsonBehavior expression, set
	 * details_wanted to be able to show the actual error message as the
	 * DETAIL of the error message that tells that it is the JsonBehavior
	 * expression that caused the error; see ExecEvalJsonCoercionFinish().
	 */

	/* Handle ON EMPTY. */
	if (empty)
	{
		*op->resvalue = (Datum) 0;
		*op->resnull = true;
		if (jsexpr->on_empty)
		{
			if (jsexpr->on_empty->btype != JSON_BEHAVIOR_ERROR)
			{
				jsestate->empty.value = BoolGetDatum(true);
				/* Set up to catch coercion errors of the ON EMPTY value. */
				jsestate->escontext.error_occurred = false;
				jsestate->escontext.details_wanted = true;
				/* Jump to end if the ON EMPTY behavior is to return NULL */
				return jsestate->jump_empty >= 0 ? jsestate->jump_empty : jsestate->jump_end;
			}
		}
		else if (jsexpr->on_error->btype != JSON_BEHAVIOR_ERROR)
		{
			jsestate->error.value = BoolGetDatum(true);
			/* Set up to catch coercion errors of the ON ERROR value. */
			jsestate->escontext.error_occurred = false;
			jsestate->escontext.details_wanted = true;
			Assert(!throw_error);
			/* Jump to end if the ON ERROR behavior is to return NULL */
			return jsestate->jump_error >= 0 ? jsestate->jump_error : jsestate->jump_end;
		}

		if (jsexpr->column_name)
			ereport(ERROR,
					errcode(ERRCODE_NO_SQL_JSON_ITEM),
					errmsg("no SQL/JSON item found for specified path of column \"%s\"",
						   jsexpr->column_name));
		else
			ereport(ERROR,
					errcode(ERRCODE_NO_SQL_JSON_ITEM),
					errmsg("no SQL/JSON item found for specified path"));
	}

	/*
	 * ON ERROR. Wouldn't get here if the behavior is ERROR, because they
	 * would have already been thrown.
	 */
	if (error)
	{
		Assert(!throw_error);
		*op->resvalue = (Datum) 0;
		*op->resnull = true;
		jsestate->error.value = BoolGetDatum(true);
		/* Set up to catch coercion errors of the ON ERROR value. */
		jsestate->escontext.error_occurred = false;
		jsestate->escontext.details_wanted = true;
		/* Jump to end if the ON ERROR behavior is to return NULL */
		return jsestate->jump_error >= 0 ? jsestate->jump_error : jsestate->jump_end;
	}

	return jump_eval_coercion >= 0 ? jump_eval_coercion : jsestate->jump_end;
}

/*
 * Convert the given JsonbValue to its C string representation
 *
 * *resnull is set if the JsonbValue is a jbvNull.
 */
static char *
ExecGetJsonValueItemString(JsonbValue *item, bool *resnull)
{
	*resnull = false;

	/* get coercion state reference and datum of the corresponding SQL type */
	switch (item->type)
	{
		case jbvNull:
			*resnull = true;
			return NULL;

		case jbvString:
			{
				char	   *str = palloc(item->val.string.len + 1);

				memcpy(str, item->val.string.val, item->val.string.len);
				str[item->val.string.len] = '\0';
				return str;
			}

		case jbvNumeric:
			return DatumGetCString(DirectFunctionCall1(numeric_out,
													   NumericGetDatum(item->val.numeric)));

		case jbvBool:
			return DatumGetCString(DirectFunctionCall1(boolout,
													   BoolGetDatum(item->val.boolean)));

		case jbvDatetime:
			switch (item->val.datetime.typid)
			{
				case DATEOID:
					return DatumGetCString(DirectFunctionCall1(date_out,
															   item->val.datetime.value));
				case TIMEOID:
					return DatumGetCString(DirectFunctionCall1(time_out,
															   item->val.datetime.value));
				case TIMETZOID:
					return DatumGetCString(DirectFunctionCall1(timetz_out,
															   item->val.datetime.value));
				case TIMESTAMPOID:
					return DatumGetCString(DirectFunctionCall1(timestamp_out,
															   item->val.datetime.value));
				case TIMESTAMPTZOID:
					return DatumGetCString(DirectFunctionCall1(timestamptz_out,
															   item->val.datetime.value));
				default:
					elog(ERROR, "unexpected jsonb datetime type oid %u",
						 item->val.datetime.typid);
			}
			break;

		case jbvArray:
		case jbvObject:
		case jbvBinary:
			return DatumGetCString(DirectFunctionCall1(jsonb_out,
													   JsonbPGetDatum(JsonbValueToJsonb(item))));

		default:
			elog(ERROR, "unexpected jsonb value type %d", item->type);
	}

	Assert(false);
	*resnull = true;
	return NULL;
}

/*
 * Coerce a jsonb value produced by ExecEvalJsonExprPath() or an ON ERROR /
 * ON EMPTY behavior expression to the target type.
 *
 * Any soft errors that occur here will be checked by
 * EEOP_JSONEXPR_COERCION_FINISH that will run after this.
 */
void
ExecEvalJsonCoercion(ExprState *state, ExprEvalStep *op,
					 ExprContext *econtext)
{
	ErrorSaveContext *escontext = op->d.jsonexpr_coercion.escontext;

	/*
	 * Prepare to call json_populate_type() to coerce the boolean result of
	 * JSON_EXISTS_OP to the target type.  If the target type is integer or a
	 * domain over integer, call the boolean-to-integer cast function instead,
	 * because the integer's input function (which is what
	 * json_populate_type() calls to coerce to scalar target types) doesn't
	 * accept boolean literals as valid input.  We only have a special case
	 * for integer and domains thereof as it seems common to use those types
	 * for EXISTS columns in JSON_TABLE().
	 */
	if (op->d.jsonexpr_coercion.exists_coerce)
	{
		if (op->d.jsonexpr_coercion.exists_cast_to_int)
		{
			/* Check domain constraints if any. */
			if (op->d.jsonexpr_coercion.exists_check_domain &&
				!domain_check_safe(*op->resvalue, *op->resnull,
								   op->d.jsonexpr_coercion.targettype,
								   &op->d.jsonexpr_coercion.json_coercion_cache,
								   econtext->ecxt_per_query_memory,
								   (Node *) escontext))
			{
				*op->resnull = true;
				*op->resvalue = (Datum) 0;
			}
			else
				*op->resvalue = DirectFunctionCall1(bool_int4, *op->resvalue);
			return;
		}

		*op->resvalue = DirectFunctionCall1(jsonb_in,
											DatumGetBool(*op->resvalue) ?
											CStringGetDatum("true") :
											CStringGetDatum("false"));
	}

	*op->resvalue = json_populate_type(*op->resvalue, JSONBOID,
									   op->d.jsonexpr_coercion.targettype,
									   op->d.jsonexpr_coercion.targettypmod,
									   &op->d.jsonexpr_coercion.json_coercion_cache,
									   econtext->ecxt_per_query_memory,
									   op->resnull,
									   op->d.jsonexpr_coercion.omit_quotes,
									   (Node *) escontext);
}

static char *
GetJsonBehaviorValueString(JsonBehavior *behavior)
{
	/*
	 * The order of array elements must correspond to the order of
	 * JsonBehaviorType members.
	 */
	const char *behavior_names[] =
	{
		"NULL",
		"ERROR",
		"EMPTY",
		"TRUE",
		"FALSE",
		"UNKNOWN",
		"EMPTY ARRAY",
		"EMPTY OBJECT",
		"DEFAULT"
	};

	return pstrdup(behavior_names[behavior->btype]);
}

/*
 * Checks if an error occurred in ExecEvalJsonCoercion().  If so, this sets
 * JsonExprState.error to trigger the ON ERROR handling steps, unless the
 * error is thrown when coercing a JsonBehavior value.
 */
void
ExecEvalJsonCoercionFinish(ExprState *state, ExprEvalStep *op)
{
	JsonExprState *jsestate = op->d.jsonexpr.jsestate;

	if (SOFT_ERROR_OCCURRED(&jsestate->escontext))
	{
		/*
		 * jsestate->error or jsestate->empty being set means that the error
		 * occurred when coercing the JsonBehavior value.  Throw the error in
		 * that case with the actual coercion error message shown in the
		 * DETAIL part.
		 */
		if (DatumGetBool(jsestate->error.value))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			/*- translator: first %s is a SQL/JSON clause (e.g. ON ERROR) */
					 errmsg("could not coerce %s expression (%s) to the RETURNING type",
							"ON ERROR",
							GetJsonBehaviorValueString(jsestate->jsexpr->on_error)),
					 errdetail("%s", jsestate->escontext.error_data->message)));
		else if (DatumGetBool(jsestate->empty.value))
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
			/*- translator: first %s is a SQL/JSON clause (e.g. ON ERROR) */
					 errmsg("could not coerce %s expression (%s) to the RETURNING type",
							"ON EMPTY",
							GetJsonBehaviorValueString(jsestate->jsexpr->on_empty)),
					 errdetail("%s", jsestate->escontext.error_data->message)));

		*op->resvalue = (Datum) 0;
		*op->resnull = true;

		jsestate->error.value = BoolGetDatum(true);

		/*
		 * Reset for next use such as for catching errors when coercing a
		 * JsonBehavior expression.
		 */
		jsestate->escontext.error_occurred = false;
		jsestate->escontext.error_occurred = false;
		jsestate->escontext.details_wanted = true;
	}
}

/*
 * ExecEvalGroupingFunc
 *
 * Computes a bitmask with a bit for each (unevaluated) argument expression
 * (rightmost arg is least significant bit).
 *
 * A bit is set if the corresponding expression is NOT part of the set of
 * grouping expressions in the current grouping set.
 */
void
ExecEvalGroupingFunc(ExprState *state, ExprEvalStep *op)
{
	AggState   *aggstate = castNode(AggState, state->parent);
	int			result = 0;
	Bitmapset  *grouped_cols = aggstate->grouped_cols;
	ListCell   *lc;

	foreach(lc, op->d.grouping_func.clauses)
	{
		int			attnum = lfirst_int(lc);

		result <<= 1;

		if (!bms_is_member(attnum, grouped_cols))
			result |= 1;
	}

	*op->resvalue = Int32GetDatum(result);
	*op->resnull = false;
}

/*
 * ExecEvalMergeSupportFunc
 *
 * Returns information about the current MERGE action for its RETURNING list.
 */
void
ExecEvalMergeSupportFunc(ExprState *state, ExprEvalStep *op,
						 ExprContext *econtext)
{
	ModifyTableState *mtstate = castNode(ModifyTableState, state->parent);
	MergeActionState *relaction = mtstate->mt_merge_action;

	if (!relaction)
		elog(ERROR, "no merge action in progress");

	/* Return the MERGE action ("INSERT", "UPDATE", or "DELETE") */
	switch (relaction->mas_action->commandType)
	{
		case CMD_INSERT:
			*op->resvalue = PointerGetDatum(cstring_to_text_with_len("INSERT", 6));
			*op->resnull = false;
			break;
		case CMD_UPDATE:
			*op->resvalue = PointerGetDatum(cstring_to_text_with_len("UPDATE", 6));
			*op->resnull = false;
			break;
		case CMD_DELETE:
			*op->resvalue = PointerGetDatum(cstring_to_text_with_len("DELETE", 6));
			*op->resnull = false;
			break;
		case CMD_NOTHING:
			elog(ERROR, "unexpected merge action: DO NOTHING");
			break;
		default:
			elog(ERROR, "unrecognized commandType: %d",
				 (int) relaction->mas_action->commandType);
	}
}

/*
 * Hand off evaluation of a subplan to nodeSubplan.c
 */
void
ExecEvalSubPlan(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	SubPlanState *sstate = op->d.subplan.sstate;

	/* could potentially be nested, so make sure there's enough stack */
	check_stack_depth();

	*op->resvalue = ExecSubPlan(sstate, econtext, op->resnull);
}

/*
 * Evaluate a wholerow Var expression.
 *
 * Returns a Datum whose value is the value of a whole-row range variable
 * with respect to given expression context.
 */
void
ExecEvalWholeRowVar(ExprState *state, ExprEvalStep *op, ExprContext *econtext)
{
	Var		   *variable = op->d.wholerow.var;
	TupleTableSlot *slot = NULL;
	TupleDesc	output_tupdesc;
	MemoryContext oldcontext;
	HeapTupleHeader dtuple;
	HeapTuple	tuple;

	/* This was checked by ExecInitExpr */
	Assert(variable->varattno == InvalidAttrNumber);

	/* Get the input slot we want */
	switch (variable->varno)
	{
		case INNER_VAR:
			/* get the tuple from the inner node */
			slot = econtext->ecxt_innertuple;
			break;

		case OUTER_VAR:
			/* get the tuple from the outer node */
			slot = econtext->ecxt_outertuple;
			break;

			/* INDEX_VAR is handled by default case */

		default:

			/*
			 * Get the tuple from the relation being scanned.
			 *
			 * By default, this uses the "scan" tuple slot, but a wholerow Var
			 * in the RETURNING list may explicitly refer to OLD/NEW.  If the
			 * OLD/NEW row doesn't exist, we just return NULL.
			 */
			switch (variable->varreturningtype)
			{
				case VAR_RETURNING_DEFAULT:
					slot = econtext->ecxt_scantuple;
					break;

				case VAR_RETURNING_OLD:
					if (state->flags & EEO_FLAG_OLD_IS_NULL)
					{
						*op->resvalue = (Datum) 0;
						*op->resnull = true;
						return;
					}
					slot = econtext->ecxt_oldtuple;
					break;

				case VAR_RETURNING_NEW:
					if (state->flags & EEO_FLAG_NEW_IS_NULL)
					{
						*op->resvalue = (Datum) 0;
						*op->resnull = true;
						return;
					}
					slot = econtext->ecxt_newtuple;
					break;
			}
			break;
	}

	/* Apply the junkfilter if any */
	if (op->d.wholerow.junkFilter != NULL)
		slot = ExecFilterJunk(op->d.wholerow.junkFilter, slot);

	/*
	 * If first time through, obtain tuple descriptor and check compatibility.
	 *
	 * XXX: It'd be great if this could be moved to the expression
	 * initialization phase, but due to using slots that's currently not
	 * feasible.
	 */
	if (op->d.wholerow.first)
	{
		/* optimistically assume we don't need slow path */
		op->d.wholerow.slow = false;

		/*
		 * If the Var identifies a named composite type, we must check that
		 * the actual tuple type is compatible with it.
		 */
		if (variable->vartype != RECORDOID)
		{
			TupleDesc	var_tupdesc;
			TupleDesc	slot_tupdesc;

			/*
			 * We really only care about numbers of attributes and data types.
			 * Also, we can ignore type mismatch on columns that are dropped
			 * in the destination type, so long as (1) the physical storage
			 * matches or (2) the actual column value is NULL.  Case (1) is
			 * helpful in some cases involving out-of-date cached plans, while
			 * case (2) is expected behavior in situations such as an INSERT
			 * into a table with dropped columns (the planner typically
			 * generates an INT4 NULL regardless of the dropped column type).
			 * If we find a dropped column and cannot verify that case (1)
			 * holds, we have to use the slow path to check (2) for each row.
			 *
			 * If vartype is a domain over composite, just look through that
			 * to the base composite type.
			 */
			var_tupdesc = lookup_rowtype_tupdesc_domain(variable->vartype,
														-1, false);

			slot_tupdesc = slot->tts_tupleDescriptor;

			if (var_tupdesc->natts != slot_tupdesc->natts)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail_plural("Table row contains %d attribute, but query expects %d.",
										  "Table row contains %d attributes, but query expects %d.",
										  slot_tupdesc->natts,
										  slot_tupdesc->natts,
										  var_tupdesc->natts)));

			for (int i = 0; i < var_tupdesc->natts; i++)
			{
				Form_pg_attribute vattr = TupleDescAttr(var_tupdesc, i);
				Form_pg_attribute sattr = TupleDescAttr(slot_tupdesc, i);

				if (vattr->atttypid == sattr->atttypid)
					continue;	/* no worries */
				if (!vattr->attisdropped)
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("table row type and query-specified row type do not match"),
							 errdetail("Table has type %s at ordinal position %d, but query expects %s.",
									   format_type_be(sattr->atttypid),
									   i + 1,
									   format_type_be(vattr->atttypid))));

				if (vattr->attlen != sattr->attlen ||
					vattr->attalign != sattr->attalign)
					op->d.wholerow.slow = true; /* need to check for nulls */
			}

			/*
			 * Use the variable's declared rowtype as the descriptor for the
			 * output values.  In particular, we *must* absorb any
			 * attisdropped markings.
			 */
			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			output_tupdesc = CreateTupleDescCopy(var_tupdesc);
			MemoryContextSwitchTo(oldcontext);

			ReleaseTupleDesc(var_tupdesc);
		}
		else
		{
			/*
			 * In the RECORD case, we use the input slot's rowtype as the
			 * descriptor for the output values, modulo possibly assigning new
			 * column names below.
			 */
			oldcontext = MemoryContextSwitchTo(econtext->ecxt_per_query_memory);
			output_tupdesc = CreateTupleDescCopy(slot->tts_tupleDescriptor);
			MemoryContextSwitchTo(oldcontext);

			/*
			 * It's possible that the input slot is a relation scan slot and
			 * so is marked with that relation's rowtype.  But we're supposed
			 * to be returning RECORD, so reset to that.
			 */
			output_tupdesc->tdtypeid = RECORDOID;
			output_tupdesc->tdtypmod = -1;

			/*
			 * We already got the correct physical datatype info above, but
			 * now we should try to find the source RTE and adopt its column
			 * aliases, since it's unlikely that the input slot has the
			 * desired names.
			 *
			 * If we can't locate the RTE, assume the column names we've got
			 * are OK.  (As of this writing, the only cases where we can't
			 * locate the RTE are in execution of trigger WHEN clauses, and
			 * then the Var will have the trigger's relation's rowtype, so its
			 * names are fine.)  Also, if the creator of the RTE didn't bother
			 * to fill in an eref field, assume our column names are OK. (This
			 * happens in COPY, and perhaps other places.)
			 */
			if (econtext->ecxt_estate &&
				variable->varno <= econtext->ecxt_estate->es_range_table_size)
			{
				RangeTblEntry *rte = exec_rt_fetch(variable->varno,
												   econtext->ecxt_estate);

				if (rte->eref)
					ExecTypeSetColNames(output_tupdesc, rte->eref->colnames);
			}
		}

		/* Bless the tupdesc if needed, and save it in the execution state */
		op->d.wholerow.tupdesc = BlessTupleDesc(output_tupdesc);

		op->d.wholerow.first = false;
	}

	/*
	 * Make sure all columns of the slot are accessible in the slot's
	 * Datum/isnull arrays.
	 */
	slot_getallattrs(slot);

	if (op->d.wholerow.slow)
	{
		/* Check to see if any dropped attributes are non-null */
		TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
		TupleDesc	var_tupdesc = op->d.wholerow.tupdesc;

		Assert(var_tupdesc->natts == tupleDesc->natts);

		for (int i = 0; i < var_tupdesc->natts; i++)
		{
			CompactAttribute *vattr = TupleDescCompactAttr(var_tupdesc, i);
			CompactAttribute *sattr = TupleDescCompactAttr(tupleDesc, i);

			if (!vattr->attisdropped)
				continue;		/* already checked non-dropped cols */
			if (slot->tts_isnull[i])
				continue;		/* null is always okay */
			if (vattr->attlen != sattr->attlen ||
				vattr->attalignby != sattr->attalignby)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("table row type and query-specified row type do not match"),
						 errdetail("Physical storage mismatch on dropped attribute at ordinal position %d.",
								   i + 1)));
		}
	}

	/*
	 * Build a composite datum, making sure any toasted fields get detoasted.
	 *
	 * (Note: it is critical that we not change the slot's state here.)
	 */
	tuple = toast_build_flattened_tuple(slot->tts_tupleDescriptor,
										slot->tts_values,
										slot->tts_isnull);
	dtuple = tuple->t_data;

	/*
	 * Label the datum with the composite type info we identified before.
	 *
	 * (Note: we could skip doing this by passing op->d.wholerow.tupdesc to
	 * the tuple build step; but that seems a tad risky so let's not.)
	 */
	HeapTupleHeaderSetTypeId(dtuple, op->d.wholerow.tupdesc->tdtypeid);
	HeapTupleHeaderSetTypMod(dtuple, op->d.wholerow.tupdesc->tdtypmod);

	*op->resvalue = PointerGetDatum(dtuple);
	*op->resnull = false;
}

void
ExecEvalSysVar(ExprState *state, ExprEvalStep *op, ExprContext *econtext,
			   TupleTableSlot *slot)
{
	Datum		d;

	/* OLD/NEW system attribute is NULL if OLD/NEW row is NULL */
	if ((op->d.var.varreturningtype == VAR_RETURNING_OLD &&
		 state->flags & EEO_FLAG_OLD_IS_NULL) ||
		(op->d.var.varreturningtype == VAR_RETURNING_NEW &&
		 state->flags & EEO_FLAG_NEW_IS_NULL))
	{
		*op->resvalue = (Datum) 0;
		*op->resnull = true;
		return;
	}

	/* slot_getsysattr has sufficient defenses against bad attnums */
	d = slot_getsysattr(slot,
						op->d.var.attnum,
						op->resnull);
	*op->resvalue = d;
	/* this ought to be unreachable, but it's cheap enough to check */
	if (unlikely(*op->resnull))
		elog(ERROR, "failed to fetch attribute from slot");
}

/*
 * Transition value has not been initialized. This is the first non-NULL input
 * value for a group. We use it as the initial value for transValue.
 */
void
ExecAggInitGroup(AggState *aggstate, AggStatePerTrans pertrans, AggStatePerGroup pergroup,
				 ExprContext *aggcontext)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;

	/*
	 * We must copy the datum into aggcontext if it is pass-by-ref. We do not
	 * need to pfree the old transValue, since it's NULL.  (We already checked
	 * that the agg's input type is binary-compatible with its transtype, so
	 * straight copy here is OK.)
	 */
	oldContext = MemoryContextSwitchTo(aggcontext->ecxt_per_tuple_memory);
	pergroup->transValue = datumCopy(fcinfo->args[1].value,
									 pertrans->transtypeByVal,
									 pertrans->transtypeLen);
	pergroup->transValueIsNull = false;
	pergroup->noTransValue = false;
	MemoryContextSwitchTo(oldContext);
}

/*
 * Ensure that the new transition value is stored in the aggcontext,
 * rather than the per-tuple context.  This should be invoked only when
 * we know (a) the transition data type is pass-by-reference, and (b)
 * the newValue is distinct from the oldValue.
 *
 * NB: This can change the current memory context.
 *
 * We copy the presented newValue into the aggcontext, except when the datum
 * points to a R/W expanded object that is already a child of the aggcontext,
 * in which case we need not copy.  We then delete the oldValue, if not null.
 *
 * If the presented datum points to a R/W expanded object that is a child of
 * some other context, ideally we would just reparent it under the aggcontext.
 * Unfortunately, that doesn't work easily, and it wouldn't help anyway for
 * aggregate-aware transfns.  We expect that a transfn that deals in expanded
 * objects and is aware of the memory management conventions for aggregate
 * transition values will (1) on first call, return a R/W expanded object that
 * is already in the right context, allowing us to do nothing here, and (2) on
 * subsequent calls, modify and return that same object, so that control
 * doesn't even reach here.  However, if we have a generic transfn that
 * returns a new R/W expanded object (probably in the per-tuple context),
 * reparenting that result would cause problems.  We'd pass that R/W object to
 * the next invocation of the transfn, and then it would be at liberty to
 * change or delete that object, and if it deletes it then our own attempt to
 * delete the now-old transvalue afterwards would be a double free.  We avoid
 * this problem by forcing the stored transvalue to always be a flat
 * non-expanded object unless the transfn is visibly doing aggregate-aware
 * memory management.  This is somewhat inefficient, but the best answer to
 * that is to write a smarter transfn.
 */
Datum
ExecAggCopyTransValue(AggState *aggstate, AggStatePerTrans pertrans,
					  Datum newValue, bool newValueIsNull,
					  Datum oldValue, bool oldValueIsNull)
{
	Assert(newValue != oldValue);

	if (!newValueIsNull)
	{
		MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);
		if (DatumIsReadWriteExpandedObject(newValue,
										   false,
										   pertrans->transtypeLen) &&
			MemoryContextGetParent(DatumGetEOHP(newValue)->eoh_context) == CurrentMemoryContext)
			 /* do nothing */ ;
		else
			newValue = datumCopy(newValue,
								 pertrans->transtypeByVal,
								 pertrans->transtypeLen);
	}
	else
	{
		/*
		 * Ensure that AggStatePerGroup->transValue ends up being 0, so
		 * callers can safely compare newValue/oldValue without having to
		 * check their respective nullness.
		 */
		newValue = (Datum) 0;
	}

	if (!oldValueIsNull)
	{
		if (DatumIsReadWriteExpandedObject(oldValue,
										   false,
										   pertrans->transtypeLen))
			DeleteExpandedObject(oldValue);
		else
			pfree(DatumGetPointer(oldValue));
	}

	return newValue;
}

/*
 * ExecEvalPreOrderedDistinctSingle
 *		Returns true when the aggregate transition value Datum is distinct
 *		from the previous input Datum and returns false when the input Datum
 *		matches the previous input Datum.
 */
bool
ExecEvalPreOrderedDistinctSingle(AggState *aggstate, AggStatePerTrans pertrans)
{
	Datum		value = pertrans->transfn_fcinfo->args[1].value;
	bool		isnull = pertrans->transfn_fcinfo->args[1].isnull;

	if (!pertrans->haslast ||
		pertrans->lastisnull != isnull ||
		(!isnull && !DatumGetBool(FunctionCall2Coll(&pertrans->equalfnOne,
													pertrans->aggCollation,
													pertrans->lastdatum, value))))
	{
		if (pertrans->haslast && !pertrans->inputtypeByVal &&
			!pertrans->lastisnull)
			pfree(DatumGetPointer(pertrans->lastdatum));

		pertrans->haslast = true;
		if (!isnull)
		{
			MemoryContext oldContext;

			oldContext = MemoryContextSwitchTo(aggstate->curaggcontext->ecxt_per_tuple_memory);

			pertrans->lastdatum = datumCopy(value, pertrans->inputtypeByVal,
											pertrans->inputtypeLen);

			MemoryContextSwitchTo(oldContext);
		}
		else
			pertrans->lastdatum = (Datum) 0;
		pertrans->lastisnull = isnull;
		return true;
	}

	return false;
}

/*
 * ExecEvalPreOrderedDistinctMulti
 *		Returns true when the aggregate input is distinct from the previous
 *		input and returns false when the input matches the previous input, or
 *		when there was no previous input.
 */
bool
ExecEvalPreOrderedDistinctMulti(AggState *aggstate, AggStatePerTrans pertrans)
{
	ExprContext *tmpcontext = aggstate->tmpcontext;
	bool		isdistinct = false; /* for now */
	TupleTableSlot *save_outer;
	TupleTableSlot *save_inner;

	for (int i = 0; i < pertrans->numTransInputs; i++)
	{
		pertrans->sortslot->tts_values[i] = pertrans->transfn_fcinfo->args[i + 1].value;
		pertrans->sortslot->tts_isnull[i] = pertrans->transfn_fcinfo->args[i + 1].isnull;
	}

	ExecClearTuple(pertrans->sortslot);
	pertrans->sortslot->tts_nvalid = pertrans->numInputs;
	ExecStoreVirtualTuple(pertrans->sortslot);

	/* save the previous slots before we overwrite them */
	save_outer = tmpcontext->ecxt_outertuple;
	save_inner = tmpcontext->ecxt_innertuple;

	tmpcontext->ecxt_outertuple = pertrans->sortslot;
	tmpcontext->ecxt_innertuple = pertrans->uniqslot;

	if (!pertrans->haslast ||
		!ExecQual(pertrans->equalfnMulti, tmpcontext))
	{
		if (pertrans->haslast)
			ExecClearTuple(pertrans->uniqslot);

		pertrans->haslast = true;
		ExecCopySlot(pertrans->uniqslot, pertrans->sortslot);

		isdistinct = true;
	}

	/* restore the original slots */
	tmpcontext->ecxt_outertuple = save_outer;
	tmpcontext->ecxt_innertuple = save_inner;

	return isdistinct;
}

/*
 * Invoke ordered transition function, with a datum argument.
 */
void
ExecEvalAggOrderedTransDatum(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	int			setno = op->d.agg_trans.setno;

	tuplesort_putdatum(pertrans->sortstates[setno],
					   *op->resvalue, *op->resnull);
}

/*
 * Invoke ordered transition function, with a tuple argument.
 */
void
ExecEvalAggOrderedTransTuple(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext)
{
	AggStatePerTrans pertrans = op->d.agg_trans.pertrans;
	int			setno = op->d.agg_trans.setno;

	ExecClearTuple(pertrans->sortslot);
	pertrans->sortslot->tts_nvalid = pertrans->numInputs;
	ExecStoreVirtualTuple(pertrans->sortslot);
	tuplesort_puttupleslot(pertrans->sortstates[setno], pertrans->sortslot);
}

/* implementation of transition function invocation for byval types */
static pg_attribute_always_inline void
ExecAggPlainTransByVal(AggState *aggstate, AggStatePerTrans pertrans,
					   AggStatePerGroup pergroup,
					   ExprContext *aggcontext, int setno)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;
	Datum		newVal;

	/* cf. select_current_set() */
	aggstate->curaggcontext = aggcontext;
	aggstate->current_set = setno;

	/* set up aggstate->curpertrans for AggGetAggref() */
	aggstate->curpertrans = pertrans;

	/* invoke transition function in per-tuple context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	fcinfo->args[0].value = pergroup->transValue;
	fcinfo->args[0].isnull = pergroup->transValueIsNull;
	fcinfo->isnull = false;		/* just in case transfn doesn't set it */

	newVal = FunctionCallInvoke(fcinfo);

	pergroup->transValue = newVal;
	pergroup->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}

/* implementation of transition function invocation for byref types */
static pg_attribute_always_inline void
ExecAggPlainTransByRef(AggState *aggstate, AggStatePerTrans pertrans,
					   AggStatePerGroup pergroup,
					   ExprContext *aggcontext, int setno)
{
	FunctionCallInfo fcinfo = pertrans->transfn_fcinfo;
	MemoryContext oldContext;
	Datum		newVal;

	/* cf. select_current_set() */
	aggstate->curaggcontext = aggcontext;
	aggstate->current_set = setno;

	/* set up aggstate->curpertrans for AggGetAggref() */
	aggstate->curpertrans = pertrans;

	/* invoke transition function in per-tuple context */
	oldContext = MemoryContextSwitchTo(aggstate->tmpcontext->ecxt_per_tuple_memory);

	fcinfo->args[0].value = pergroup->transValue;
	fcinfo->args[0].isnull = pergroup->transValueIsNull;
	fcinfo->isnull = false;		/* just in case transfn doesn't set it */

	newVal = FunctionCallInvoke(fcinfo);

	/*
	 * For pass-by-ref datatype, must copy the new value into aggcontext and
	 * free the prior transValue.  But if transfn returned a pointer to its
	 * first input, we don't need to do anything.
	 *
	 * It's safe to compare newVal with pergroup->transValue without regard
	 * for either being NULL, because ExecAggCopyTransValue takes care to set
	 * transValue to 0 when NULL. Otherwise we could end up accidentally not
	 * reparenting, when the transValue has the same numerical value as
	 * newValue, despite being NULL.  This is a somewhat hot path, making it
	 * undesirable to instead solve this with another branch for the common
	 * case of the transition function returning its (modified) input
	 * argument.
	 */
	if (DatumGetPointer(newVal) != DatumGetPointer(pergroup->transValue))
		newVal = ExecAggCopyTransValue(aggstate, pertrans,
									   newVal, fcinfo->isnull,
									   pergroup->transValue,
									   pergroup->transValueIsNull);

	pergroup->transValue = newVal;
	pergroup->transValueIsNull = fcinfo->isnull;

	MemoryContextSwitchTo(oldContext);
}
