/*-------------------------------------------------------------------------
 *
 * execExpr.h
 *	  Low level infrastructure related to expression evaluation
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/execExpr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXEC_EXPR_H
#define EXEC_EXPR_H

#include "executor/nodeAgg.h"
#include "nodes/execnodes.h"
#include "nodes/miscnodes.h"

/* forward references to avoid circularity */
struct ExprEvalStep;
struct SubscriptingRefState;
struct ScalarArrayOpExprHashTable;
struct JsonConstructorExprState;

/* Bits in ExprState->flags (see also execnodes.h for public flag bits): */
/* expression's interpreter has been initialized */
#define EEO_FLAG_INTERPRETER_INITIALIZED	(1 << 1)
/* jump-threading is in use */
#define EEO_FLAG_DIRECT_THREADED			(1 << 2)

/* Typical API for out-of-line evaluation subroutines */
typedef void (*ExecEvalSubroutine) (ExprState *state,
									struct ExprEvalStep *op,
									ExprContext *econtext);

/* API for out-of-line evaluation subroutines returning bool */
typedef bool (*ExecEvalBoolSubroutine) (ExprState *state,
										struct ExprEvalStep *op,
										ExprContext *econtext);

/* ExprEvalSteps that cache a composite type's tupdesc need one of these */
/* (it fits in-line in some step types, otherwise allocate out-of-line) */
typedef struct ExprEvalRowtypeCache
{
	/*
	 * cacheptr points to composite type's TypeCacheEntry if tupdesc_id is not
	 * 0; or for an anonymous RECORD type, it points directly at the cached
	 * tupdesc for the type, and tupdesc_id is 0.  (We'd use separate fields
	 * if space were not at a premium.)  Initial state is cacheptr == NULL.
	 */
	void	   *cacheptr;
	uint64		tupdesc_id;		/* last-seen tupdesc identifier, or 0 */
} ExprEvalRowtypeCache;

/*
 * Discriminator for ExprEvalSteps.
 *
 * Identifies the operation to be executed and which member in the
 * ExprEvalStep->d union is valid.
 *
 * The order of entries needs to be kept in sync with the dispatch_table[]
 * array in execExprInterp.c:ExecInterpExpr().
 */
typedef enum ExprEvalOp
{
	/* entire expression has been evaluated completely, return */
	EEOP_DONE,

	/* apply slot_getsomeattrs on corresponding tuple slot */
	EEOP_INNER_FETCHSOME,
	EEOP_OUTER_FETCHSOME,
	EEOP_SCAN_FETCHSOME,

	/* compute non-system Var value */
	EEOP_INNER_VAR,
	EEOP_OUTER_VAR,
	EEOP_SCAN_VAR,

	/* compute system Var value */
	EEOP_INNER_SYSVAR,
	EEOP_OUTER_SYSVAR,
	EEOP_SCAN_SYSVAR,

	/* compute wholerow Var */
	EEOP_WHOLEROW,

	/*
	 * Compute non-system Var value, assign it into ExprState's resultslot.
	 * These are not used if a CheckVarSlotCompatibility() check would be
	 * needed.
	 */
	EEOP_ASSIGN_INNER_VAR,
	EEOP_ASSIGN_OUTER_VAR,
	EEOP_ASSIGN_SCAN_VAR,

	/* assign ExprState's resvalue/resnull to a column of its resultslot */
	EEOP_ASSIGN_TMP,
	/* ditto, applying MakeExpandedObjectReadOnly() */
	EEOP_ASSIGN_TMP_MAKE_RO,

	/* evaluate Const value */
	EEOP_CONST,

	/*
	 * Evaluate function call (including OpExprs etc).  For speed, we
	 * distinguish in the opcode whether the function is strict and/or
	 * requires usage stats tracking.
	 */
	EEOP_FUNCEXPR,
	EEOP_FUNCEXPR_STRICT,
	EEOP_FUNCEXPR_FUSAGE,
	EEOP_FUNCEXPR_STRICT_FUSAGE,

	/*
	 * Evaluate boolean AND expression, one step per subexpression. FIRST/LAST
	 * subexpressions are special-cased for performance.  Since AND always has
	 * at least two subexpressions, FIRST and LAST never apply to the same
	 * subexpression.
	 */
	EEOP_BOOL_AND_STEP_FIRST,
	EEOP_BOOL_AND_STEP,
	EEOP_BOOL_AND_STEP_LAST,

	/* similarly for boolean OR expression */
	EEOP_BOOL_OR_STEP_FIRST,
	EEOP_BOOL_OR_STEP,
	EEOP_BOOL_OR_STEP_LAST,

	/* evaluate boolean NOT expression */
	EEOP_BOOL_NOT_STEP,

	/* simplified version of BOOL_AND_STEP for use by ExecQual() */
	EEOP_QUAL,

	/* unconditional jump to another step */
	EEOP_JUMP,

	/* conditional jumps based on current result value */
	EEOP_JUMP_IF_NULL,
	EEOP_JUMP_IF_NOT_NULL,
	EEOP_JUMP_IF_NOT_TRUE,

	/* perform NULL tests for scalar values */
	EEOP_NULLTEST_ISNULL,
	EEOP_NULLTEST_ISNOTNULL,

	/* perform NULL tests for row values */
	EEOP_NULLTEST_ROWISNULL,
	EEOP_NULLTEST_ROWISNOTNULL,

	/* evaluate a BooleanTest expression */
	EEOP_BOOLTEST_IS_TRUE,
	EEOP_BOOLTEST_IS_NOT_TRUE,
	EEOP_BOOLTEST_IS_FALSE,
	EEOP_BOOLTEST_IS_NOT_FALSE,

	/* evaluate PARAM_EXEC/EXTERN parameters */
	EEOP_PARAM_EXEC,
	EEOP_PARAM_EXTERN,
	EEOP_PARAM_CALLBACK,
	/* set PARAM_EXEC value */
	EEOP_PARAM_SET,

	/* return CaseTestExpr value */
	EEOP_CASE_TESTVAL,

	/* apply MakeExpandedObjectReadOnly() to target value */
	EEOP_MAKE_READONLY,

	/* evaluate assorted special-purpose expression types */
	EEOP_IOCOERCE,
	EEOP_IOCOERCE_SAFE,
	EEOP_DISTINCT,
	EEOP_NOT_DISTINCT,
	EEOP_NULLIF,
	EEOP_SQLVALUEFUNCTION,
	EEOP_CURRENTOFEXPR,
	EEOP_NEXTVALUEEXPR,
	EEOP_ARRAYEXPR,
	EEOP_ARRAYCOERCE,
	EEOP_ROW,

	/*
	 * Compare two individual elements of each of two compared ROW()
	 * expressions.  Skip to ROWCOMPARE_FINAL if elements are not equal.
	 */
	EEOP_ROWCOMPARE_STEP,

	/* evaluate boolean value based on previous ROWCOMPARE_STEP operations */
	EEOP_ROWCOMPARE_FINAL,

	/* evaluate GREATEST() or LEAST() */
	EEOP_MINMAX,

	/* evaluate FieldSelect expression */
	EEOP_FIELDSELECT,

	/*
	 * Deform tuple before evaluating new values for individual fields in a
	 * FieldStore expression.
	 */
	EEOP_FIELDSTORE_DEFORM,

	/*
	 * Form the new tuple for a FieldStore expression.  Individual fields will
	 * have been evaluated into columns of the tuple deformed by the preceding
	 * DEFORM step.
	 */
	EEOP_FIELDSTORE_FORM,

	/* Process container subscripts; possibly short-circuit result to NULL */
	EEOP_SBSREF_SUBSCRIPTS,

	/*
	 * Compute old container element/slice when a SubscriptingRef assignment
	 * expression contains SubscriptingRef/FieldStore subexpressions. Value is
	 * accessed using the CaseTest mechanism.
	 */
	EEOP_SBSREF_OLD,

	/* compute new value for SubscriptingRef assignment expression */
	EEOP_SBSREF_ASSIGN,

	/* compute element/slice for SubscriptingRef fetch expression */
	EEOP_SBSREF_FETCH,

	/* evaluate value for CoerceToDomainValue */
	EEOP_DOMAIN_TESTVAL,

	/* evaluate a domain's NOT NULL constraint */
	EEOP_DOMAIN_NOTNULL,

	/* evaluate a single domain CHECK constraint */
	EEOP_DOMAIN_CHECK,

	/* evaluation steps for hashing */
	EEOP_HASHDATUM_SET_INITVAL,
	EEOP_HASHDATUM_FIRST,
	EEOP_HASHDATUM_FIRST_STRICT,
	EEOP_HASHDATUM_NEXT32,
	EEOP_HASHDATUM_NEXT32_STRICT,

	/* evaluate assorted special-purpose expression types */
	EEOP_CONVERT_ROWTYPE,
	EEOP_SCALARARRAYOP,
	EEOP_HASHED_SCALARARRAYOP,
	EEOP_XMLEXPR,
	EEOP_JSON_CONSTRUCTOR,
	EEOP_IS_JSON,
	EEOP_JSONEXPR_PATH,
	EEOP_JSONEXPR_COERCION,
	EEOP_JSONEXPR_COERCION_FINISH,
	EEOP_AGGREF,
	EEOP_GROUPING_FUNC,
	EEOP_WINDOW_FUNC,
	EEOP_MERGE_SUPPORT_FUNC,
	EEOP_SUBPLAN,

	/* aggregation related nodes */
	EEOP_AGG_STRICT_DESERIALIZE,
	EEOP_AGG_DESERIALIZE,
	EEOP_AGG_STRICT_INPUT_CHECK_ARGS,
	EEOP_AGG_STRICT_INPUT_CHECK_NULLS,
	EEOP_AGG_PLAIN_PERGROUP_NULLCHECK,
	EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYVAL,
	EEOP_AGG_PLAIN_TRANS_STRICT_BYVAL,
	EEOP_AGG_PLAIN_TRANS_BYVAL,
	EEOP_AGG_PLAIN_TRANS_INIT_STRICT_BYREF,
	EEOP_AGG_PLAIN_TRANS_STRICT_BYREF,
	EEOP_AGG_PLAIN_TRANS_BYREF,
	EEOP_AGG_PRESORTED_DISTINCT_SINGLE,
	EEOP_AGG_PRESORTED_DISTINCT_MULTI,
	EEOP_AGG_ORDERED_TRANS_DATUM,
	EEOP_AGG_ORDERED_TRANS_TUPLE,

	/* non-existent operation, used e.g. to check array lengths */
	EEOP_LAST
} ExprEvalOp;


typedef struct ExprEvalStep
{
	/*
	 * Instruction to be executed.  During instruction preparation this is an
	 * enum ExprEvalOp, but later it can be changed to some other type, e.g. a
	 * pointer for computed goto (that's why it's an intptr_t).
	 */
	intptr_t	opcode;

	/* where to store the result of this step */
	Datum	   *resvalue;
	bool	   *resnull;

	/*
	 * Inline data for the operation.  Inline data is faster to access, but
	 * also bloats the size of all instructions.  The union should be kept to
	 * no more than 40 bytes on 64-bit systems (so that the entire struct is
	 * no more than 64 bytes, a single cacheline on common systems).
	 */
	union
	{
		/* for EEOP_INNER/OUTER/SCAN_FETCHSOME */
		struct
		{
			/* attribute number up to which to fetch (inclusive) */
			int			last_var;
			/* will the type of slot be the same for every invocation */
			bool		fixed;
			/* tuple descriptor, if known */
			TupleDesc	known_desc;
			/* type of slot, can only be relied upon if fixed is set */
			const TupleTableSlotOps *kind;
		}			fetch;

		/* for EEOP_INNER/OUTER/SCAN_[SYS]VAR[_FIRST] */
		struct
		{
			/* attnum is attr number - 1 for regular VAR ... */
			/* but it's just the normal (negative) attr number for SYSVAR */
			int			attnum;
			Oid			vartype;	/* type OID of variable */
		}			var;

		/* for EEOP_WHOLEROW */
		struct
		{
			Var		   *var;	/* original Var node in plan tree */
			bool		first;	/* first time through, need to initialize? */
			bool		slow;	/* need runtime check for nulls? */
			TupleDesc	tupdesc;	/* descriptor for resulting tuples */
			JunkFilter *junkFilter; /* JunkFilter to remove resjunk cols */
		}			wholerow;

		/* for EEOP_ASSIGN_*_VAR */
		struct
		{
			/* target index in ExprState->resultslot->tts_values/nulls */
			int			resultnum;
			/* source attribute number - 1 */
			int			attnum;
		}			assign_var;

		/* for EEOP_ASSIGN_TMP[_MAKE_RO] */
		struct
		{
			/* target index in ExprState->resultslot->tts_values/nulls */
			int			resultnum;
		}			assign_tmp;

		/* for EEOP_CONST */
		struct
		{
			/* constant's value */
			Datum		value;
			bool		isnull;
		}			constval;

		/* for EEOP_FUNCEXPR_* / NULLIF / DISTINCT */
		struct
		{
			FmgrInfo   *finfo;	/* function's lookup data */
			FunctionCallInfo fcinfo_data;	/* arguments etc */
			/* faster to access without additional indirection: */
			PGFunction	fn_addr;	/* actual call address */
			int			nargs;	/* number of arguments */
		}			func;

		/* for EEOP_BOOL_*_STEP */
		struct
		{
			bool	   *anynull;	/* track if any input was NULL */
			int			jumpdone;	/* jump here if result determined */
		}			boolexpr;

		/* for EEOP_QUAL */
		struct
		{
			int			jumpdone;	/* jump here on false or null */
		}			qualexpr;

		/* for EEOP_JUMP[_CONDITION] */
		struct
		{
			int			jumpdone;	/* target instruction's index */
		}			jump;

		/* for EEOP_NULLTEST_ROWIS[NOT]NULL */
		struct
		{
			/* cached descriptor for composite type - filled at runtime */
			ExprEvalRowtypeCache rowcache;
		}			nulltest_row;

		/* for EEOP_PARAM_EXEC/EXTERN and EEOP_PARAM_SET */
		struct
		{
			int			paramid;	/* numeric ID for parameter */
			Oid			paramtype;	/* OID of parameter's datatype */
		}			param;

		/* for EEOP_PARAM_CALLBACK */
		struct
		{
			ExecEvalSubroutine paramfunc;	/* add-on evaluation subroutine */
			void	   *paramarg;	/* private data for same */
			int			paramid;	/* numeric ID for parameter */
			Oid			paramtype;	/* OID of parameter's datatype */
		}			cparam;

		/* for EEOP_CASE_TESTVAL/DOMAIN_TESTVAL */
		struct
		{
			Datum	   *value;	/* value to return */
			bool	   *isnull;
		}			casetest;

		/* for EEOP_MAKE_READONLY */
		struct
		{
			Datum	   *value;	/* value to coerce to read-only */
			bool	   *isnull;
		}			make_readonly;

		/* for EEOP_IOCOERCE */
		struct
		{
			/* lookup and call info for source type's output function */
			FmgrInfo   *finfo_out;
			FunctionCallInfo fcinfo_data_out;
			/* lookup and call info for result type's input function */
			FmgrInfo   *finfo_in;
			FunctionCallInfo fcinfo_data_in;
		}			iocoerce;

		/* for EEOP_SQLVALUEFUNCTION */
		struct
		{
			SQLValueFunction *svf;
		}			sqlvaluefunction;

		/* for EEOP_NEXTVALUEEXPR */
		struct
		{
			Oid			seqid;
			Oid			seqtypid;
		}			nextvalueexpr;

		/* for EEOP_ARRAYEXPR */
		struct
		{
			Datum	   *elemvalues; /* element values get stored here */
			bool	   *elemnulls;
			int			nelems; /* length of the above arrays */
			Oid			elemtype;	/* array element type */
			int16		elemlength; /* typlen of the array element type */
			bool		elembyval;	/* is the element type pass-by-value? */
			char		elemalign;	/* typalign of the element type */
			bool		multidims;	/* is array expression multi-D? */
		}			arrayexpr;

		/* for EEOP_ARRAYCOERCE */
		struct
		{
			ExprState  *elemexprstate;	/* null if no per-element work */
			Oid			resultelemtype; /* element type of result array */
			struct ArrayMapState *amstate;	/* workspace for array_map */
		}			arraycoerce;

		/* for EEOP_ROW */
		struct
		{
			TupleDesc	tupdesc;	/* descriptor for result tuples */
			/* workspace for the values constituting the row: */
			Datum	   *elemvalues;
			bool	   *elemnulls;
		}			row;

		/* for EEOP_ROWCOMPARE_STEP */
		struct
		{
			/* lookup and call data for column comparison function */
			FmgrInfo   *finfo;
			FunctionCallInfo fcinfo_data;
			PGFunction	fn_addr;
			/* target for comparison resulting in NULL */
			int			jumpnull;
			/* target for comparison yielding inequality */
			int			jumpdone;
		}			rowcompare_step;

		/* for EEOP_ROWCOMPARE_FINAL */
		struct
		{
			RowCompareType rctype;
		}			rowcompare_final;

		/* for EEOP_MINMAX */
		struct
		{
			/* workspace for argument values */
			Datum	   *values;
			bool	   *nulls;
			int			nelems;
			/* is it GREATEST or LEAST? */
			MinMaxOp	op;
			/* lookup and call data for comparison function */
			FmgrInfo   *finfo;
			FunctionCallInfo fcinfo_data;
		}			minmax;

		/* for EEOP_FIELDSELECT */
		struct
		{
			AttrNumber	fieldnum;	/* field number to extract */
			Oid			resulttype; /* field's type */
			/* cached descriptor for composite type - filled at runtime */
			ExprEvalRowtypeCache rowcache;
		}			fieldselect;

		/* for EEOP_FIELDSTORE_DEFORM / FIELDSTORE_FORM */
		struct
		{
			/* original expression node */
			FieldStore *fstore;

			/* cached descriptor for composite type - filled at runtime */
			/* note that a DEFORM and FORM pair share the same cache */
			ExprEvalRowtypeCache *rowcache;

			/* workspace for column values */
			Datum	   *values;
			bool	   *nulls;
			int			ncolumns;
		}			fieldstore;

		/* for EEOP_SBSREF_SUBSCRIPTS */
		struct
		{
			ExecEvalBoolSubroutine subscriptfunc;	/* evaluation subroutine */
			/* too big to have inline */
			struct SubscriptingRefState *state;
			int			jumpdone;	/* jump here on null */
		}			sbsref_subscript;

		/* for EEOP_SBSREF_OLD / ASSIGN / FETCH */
		struct
		{
			ExecEvalSubroutine subscriptfunc;	/* evaluation subroutine */
			/* too big to have inline */
			struct SubscriptingRefState *state;
		}			sbsref;

		/* for EEOP_DOMAIN_NOTNULL / DOMAIN_CHECK */
		struct
		{
			/* name of constraint */
			char	   *constraintname;
			/* where the result of a CHECK constraint will be stored */
			Datum	   *checkvalue;
			bool	   *checknull;
			/* OID of domain type */
			Oid			resulttype;
			ErrorSaveContext *escontext;
		}			domaincheck;

		/* for EEOP_HASH_SET_INITVAL */
		struct
		{
			Datum		init_value;

		}			hashdatum_initvalue;

		/* for EEOP_HASHDATUM_(FIRST|NEXT32)[_STRICT] */
		struct
		{
			FmgrInfo   *finfo;	/* function's lookup data */
			FunctionCallInfo fcinfo_data;	/* arguments etc */
			/* faster to access without additional indirection: */
			PGFunction	fn_addr;	/* actual call address */
			int			jumpdone;	/* jump here on null */
			NullableDatum *iresult; /* intermediate hash result */
		}			hashdatum;

		/* for EEOP_CONVERT_ROWTYPE */
		struct
		{
			Oid			inputtype;	/* input composite type */
			Oid			outputtype; /* output composite type */
			/* these three fields are filled at runtime: */
			ExprEvalRowtypeCache *incache;	/* cache for input type */
			ExprEvalRowtypeCache *outcache; /* cache for output type */
			TupleConversionMap *map;	/* column mapping */
		}			convert_rowtype;

		/* for EEOP_SCALARARRAYOP */
		struct
		{
			/* element_type/typlen/typbyval/typalign are filled at runtime */
			Oid			element_type;	/* InvalidOid if not yet filled */
			bool		useOr;	/* use OR or AND semantics? */
			int16		typlen; /* array element type storage info */
			bool		typbyval;
			char		typalign;
			FmgrInfo   *finfo;	/* function's lookup data */
			FunctionCallInfo fcinfo_data;	/* arguments etc */
			/* faster to access without additional indirection: */
			PGFunction	fn_addr;	/* actual call address */
		}			scalararrayop;

		/* for EEOP_HASHED_SCALARARRAYOP */
		struct
		{
			bool		has_nulls;
			bool		inclause;	/* true for IN and false for NOT IN */
			struct ScalarArrayOpExprHashTable *elements_tab;
			FmgrInfo   *finfo;	/* function's lookup data */
			FunctionCallInfo fcinfo_data;	/* arguments etc */
			ScalarArrayOpExpr *saop;
		}			hashedscalararrayop;

		/* for EEOP_XMLEXPR */
		struct
		{
			XmlExpr    *xexpr;	/* original expression node */
			/* workspace for evaluating named args, if any */
			Datum	   *named_argvalue;
			bool	   *named_argnull;
			/* workspace for evaluating unnamed args, if any */
			Datum	   *argvalue;
			bool	   *argnull;
		}			xmlexpr;

		/* for EEOP_JSON_CONSTRUCTOR */
		struct
		{
			struct JsonConstructorExprState *jcstate;
		}			json_constructor;

		/* for EEOP_AGGREF */
		struct
		{
			int			aggno;
		}			aggref;

		/* for EEOP_GROUPING_FUNC */
		struct
		{
			List	   *clauses;	/* integer list of column numbers */
		}			grouping_func;

		/* for EEOP_WINDOW_FUNC */
		struct
		{
			/* out-of-line state, modified by nodeWindowAgg.c */
			WindowFuncExprState *wfstate;
		}			window_func;

		/* for EEOP_SUBPLAN */
		struct
		{
			/* out-of-line state, created by nodeSubplan.c */
			SubPlanState *sstate;
		}			subplan;

		/* for EEOP_AGG_*DESERIALIZE */
		struct
		{
			FunctionCallInfo fcinfo_data;
			int			jumpnull;
		}			agg_deserialize;

		/* for EEOP_AGG_STRICT_INPUT_CHECK_NULLS / STRICT_INPUT_CHECK_ARGS */
		struct
		{
			/*
			 * For EEOP_AGG_STRICT_INPUT_CHECK_ARGS args contains pointers to
			 * the NullableDatums that need to be checked for NULLs.
			 *
			 * For EEOP_AGG_STRICT_INPUT_CHECK_NULLS nulls contains pointers
			 * to booleans that need to be checked for NULLs.
			 *
			 * Both cases currently need to exist because sometimes the
			 * to-be-checked nulls are in TupleTableSlot.isnull array, and
			 * sometimes in FunctionCallInfoBaseData.args[i].isnull.
			 */
			NullableDatum *args;
			bool	   *nulls;
			int			nargs;
			int			jumpnull;
		}			agg_strict_input_check;

		/* for EEOP_AGG_PLAIN_PERGROUP_NULLCHECK */
		struct
		{
			int			setoff;
			int			jumpnull;
		}			agg_plain_pergroup_nullcheck;

		/* for EEOP_AGG_PRESORTED_DISTINCT_{SINGLE,MULTI} */
		struct
		{
			AggStatePerTrans pertrans;
			ExprContext *aggcontext;
			int			jumpdistinct;
		}			agg_presorted_distinctcheck;

		/* for EEOP_AGG_PLAIN_TRANS_[INIT_][STRICT_]{BYVAL,BYREF} */
		/* for EEOP_AGG_ORDERED_TRANS_{DATUM,TUPLE} */
		struct
		{
			AggStatePerTrans pertrans;
			ExprContext *aggcontext;
			int			setno;
			int			transno;
			int			setoff;
		}			agg_trans;

		/* for EEOP_IS_JSON */
		struct
		{
			JsonIsPredicate *pred;	/* original expression node */
		}			is_json;

		/* for EEOP_JSONEXPR_PATH */
		struct
		{
			struct JsonExprState *jsestate;
		}			jsonexpr;

		/* for EEOP_JSONEXPR_COERCION */
		struct
		{
			Oid			targettype;
			int32		targettypmod;
			bool		omit_quotes;
			/* exists_* fields only relevant for JSON_EXISTS_OP. */
			bool		exists_coerce;
			bool		exists_cast_to_int;
			bool		exists_check_domain;
			void	   *json_coercion_cache;
			ErrorSaveContext *escontext;
		}			jsonexpr_coercion;
	}			d;
} ExprEvalStep;

/* Enforce the size rule given in the comment above */
StaticAssertDecl(sizeof(ExprEvalStep) <= 64,
				 "size of ExprEvalStep exceeds 64 bytes");


/* Non-inline data for container operations */
typedef struct SubscriptingRefState
{
	bool		isassignment;	/* is it assignment, or just fetch? */

	/* workspace for type-specific subscripting code */
	void	   *workspace;

	/* numupper and upperprovided[] are filled at expression compile time */
	/* at runtime, subscripts are computed in upperindex[]/upperindexnull[] */
	int			numupper;
	bool	   *upperprovided;	/* indicates if this position is supplied */
	Datum	   *upperindex;
	bool	   *upperindexnull;

	/* similarly for lower indexes, if any */
	int			numlower;
	bool	   *lowerprovided;
	Datum	   *lowerindex;
	bool	   *lowerindexnull;

	/* for assignment, new value to assign is evaluated into here */
	Datum		replacevalue;
	bool		replacenull;

	/* if we have a nested assignment, sbs_fetch_old puts old value here */
	Datum		prevvalue;
	bool		prevnull;
} SubscriptingRefState;

/* Execution step methods used for SubscriptingRef */
typedef struct SubscriptExecSteps
{
	/* See nodes/subscripting.h for more detail about these */
	ExecEvalBoolSubroutine sbs_check_subscripts;	/* process subscripts */
	ExecEvalSubroutine sbs_fetch;	/* fetch an element */
	ExecEvalSubroutine sbs_assign;	/* assign to an element */
	ExecEvalSubroutine sbs_fetch_old;	/* fetch old value for assignment */
} SubscriptExecSteps;

/* EEOP_JSON_CONSTRUCTOR state, too big to inline */
typedef struct JsonConstructorExprState
{
	JsonConstructorExpr *constructor;
	Datum	   *arg_values;
	bool	   *arg_nulls;
	Oid		   *arg_types;
	struct
	{
		int			category;
		Oid			outfuncid;
	}		   *arg_type_cache; /* cache for datum_to_json[b]() */
	int			nargs;
} JsonConstructorExprState;


/* functions in execExpr.c */
extern void ExprEvalPushStep(ExprState *es, const ExprEvalStep *s);

/* functions in execExprInterp.c */
extern void ExecReadyInterpretedExpr(ExprState *state);
extern ExprEvalOp ExecEvalStepOp(ExprState *state, ExprEvalStep *op);

extern Datum ExecInterpExprStillValid(ExprState *state, ExprContext *econtext, bool *isNull);
extern void CheckExprStillValid(ExprState *state, ExprContext *econtext);

/*
 * Non fast-path execution functions. These are externs instead of statics in
 * execExprInterp.c, because that allows them to be used by other methods of
 * expression evaluation, reducing code duplication.
 */
extern void ExecEvalFuncExprFusage(ExprState *state, ExprEvalStep *op,
								   ExprContext *econtext);
extern void ExecEvalFuncExprStrictFusage(ExprState *state, ExprEvalStep *op,
										 ExprContext *econtext);
extern void ExecEvalParamExec(ExprState *state, ExprEvalStep *op,
							  ExprContext *econtext);
extern void ExecEvalParamSet(ExprState *state, ExprEvalStep *op,
							 ExprContext *econtext);
extern void ExecEvalParamExtern(ExprState *state, ExprEvalStep *op,
								ExprContext *econtext);
extern void ExecEvalCoerceViaIOSafe(ExprState *state, ExprEvalStep *op);
extern void ExecEvalSQLValueFunction(ExprState *state, ExprEvalStep *op);
extern void ExecEvalCurrentOfExpr(ExprState *state, ExprEvalStep *op);
extern void ExecEvalNextValueExpr(ExprState *state, ExprEvalStep *op);
extern void ExecEvalRowNull(ExprState *state, ExprEvalStep *op,
							ExprContext *econtext);
extern void ExecEvalRowNotNull(ExprState *state, ExprEvalStep *op,
							   ExprContext *econtext);
extern void ExecEvalArrayExpr(ExprState *state, ExprEvalStep *op);
extern void ExecEvalArrayCoerce(ExprState *state, ExprEvalStep *op,
								ExprContext *econtext);
extern void ExecEvalRow(ExprState *state, ExprEvalStep *op);
extern void ExecEvalMinMax(ExprState *state, ExprEvalStep *op);
extern void ExecEvalFieldSelect(ExprState *state, ExprEvalStep *op,
								ExprContext *econtext);
extern void ExecEvalFieldStoreDeForm(ExprState *state, ExprEvalStep *op,
									 ExprContext *econtext);
extern void ExecEvalFieldStoreForm(ExprState *state, ExprEvalStep *op,
								   ExprContext *econtext);
extern void ExecEvalConvertRowtype(ExprState *state, ExprEvalStep *op,
								   ExprContext *econtext);
extern void ExecEvalScalarArrayOp(ExprState *state, ExprEvalStep *op);
extern void ExecEvalHashedScalarArrayOp(ExprState *state, ExprEvalStep *op,
										ExprContext *econtext);
extern void ExecEvalConstraintNotNull(ExprState *state, ExprEvalStep *op);
extern void ExecEvalConstraintCheck(ExprState *state, ExprEvalStep *op);
extern void ExecEvalXmlExpr(ExprState *state, ExprEvalStep *op);
extern void ExecEvalJsonConstructor(ExprState *state, ExprEvalStep *op,
									ExprContext *econtext);
extern void ExecEvalJsonIsPredicate(ExprState *state, ExprEvalStep *op);
extern int	ExecEvalJsonExprPath(ExprState *state, ExprEvalStep *op,
								 ExprContext *econtext);
extern void ExecEvalJsonCoercion(ExprState *state, ExprEvalStep *op,
								 ExprContext *econtext);
extern void ExecEvalJsonCoercionFinish(ExprState *state, ExprEvalStep *op);
extern void ExecEvalGroupingFunc(ExprState *state, ExprEvalStep *op);
extern void ExecEvalMergeSupportFunc(ExprState *state, ExprEvalStep *op,
									 ExprContext *econtext);
extern void ExecEvalSubPlan(ExprState *state, ExprEvalStep *op,
							ExprContext *econtext);
extern void ExecEvalWholeRowVar(ExprState *state, ExprEvalStep *op,
								ExprContext *econtext);
extern void ExecEvalSysVar(ExprState *state, ExprEvalStep *op,
						   ExprContext *econtext, TupleTableSlot *slot);

extern void ExecAggInitGroup(AggState *aggstate, AggStatePerTrans pertrans, AggStatePerGroup pergroup,
							 ExprContext *aggcontext);
extern Datum ExecAggCopyTransValue(AggState *aggstate, AggStatePerTrans pertrans,
								   Datum newValue, bool newValueIsNull,
								   Datum oldValue, bool oldValueIsNull);
extern bool ExecEvalPreOrderedDistinctSingle(AggState *aggstate,
											 AggStatePerTrans pertrans);
extern bool ExecEvalPreOrderedDistinctMulti(AggState *aggstate,
											AggStatePerTrans pertrans);
extern void ExecEvalAggOrderedTransDatum(ExprState *state, ExprEvalStep *op,
										 ExprContext *econtext);
extern void ExecEvalAggOrderedTransTuple(ExprState *state, ExprEvalStep *op,
										 ExprContext *econtext);

#endif							/* EXEC_EXPR_H */
