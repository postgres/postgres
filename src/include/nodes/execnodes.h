/*-------------------------------------------------------------------------
 *
 * execnodes.h
 *	  definitions for executor state nodes
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/nodes/execnodes.h,v 1.183 2008/01/01 19:45:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECNODES_H
#define EXECNODES_H

#include "access/relscan.h"
#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "nodes/tidbitmap.h"
#include "utils/hsearch.h"
#include "utils/tuplestore.h"


/* ----------------
 *	  IndexInfo information
 *
 *		this struct holds the information needed to construct new index
 *		entries for a particular index.  Used for both index_build and
 *		retail creation of index entries.
 *
 *		NumIndexAttrs		number of columns in this index
 *		KeyAttrNumbers		underlying-rel attribute numbers used as keys
 *							(zeroes indicate expressions)
 *		Expressions			expr trees for expression entries, or NIL if none
 *		ExpressionsState	exec state for expressions, or NIL if none
 *		Predicate			partial-index predicate, or NIL if none
 *		PredicateState		exec state for predicate, or NIL if none
 *		Unique				is it a unique index?
 *		ReadyForInserts		is it valid for inserts?
 *		Concurrent			are we doing a concurrent index build?
 *		BrokenHotChain		did we detect any broken HOT chains?
 *
 * ii_Concurrent and ii_BrokenHotChain are used only during index build;
 * they're conventionally set to false otherwise.
 * ----------------
 */
typedef struct IndexInfo
{
	NodeTag		type;
	int			ii_NumIndexAttrs;
	AttrNumber	ii_KeyAttrNumbers[INDEX_MAX_KEYS];
	List	   *ii_Expressions; /* list of Expr */
	List	   *ii_ExpressionsState;	/* list of ExprState */
	List	   *ii_Predicate;	/* list of Expr */
	List	   *ii_PredicateState;		/* list of ExprState */
	bool		ii_Unique;
	bool		ii_ReadyForInserts;
	bool		ii_Concurrent;
	bool		ii_BrokenHotChain;
} IndexInfo;

/* ----------------
 *	  ExprContext_CB
 *
 *		List of callbacks to be called at ExprContext shutdown.
 * ----------------
 */
typedef void (*ExprContextCallbackFunction) (Datum arg);

typedef struct ExprContext_CB
{
	struct ExprContext_CB *next;
	ExprContextCallbackFunction function;
	Datum		arg;
} ExprContext_CB;

/* ----------------
 *	  ExprContext
 *
 *		This class holds the "current context" information
 *		needed to evaluate expressions for doing tuple qualifications
 *		and tuple projections.	For example, if an expression refers
 *		to an attribute in the current inner tuple then we need to know
 *		what the current inner tuple is and so we look at the expression
 *		context.
 *
 *	There are two memory contexts associated with an ExprContext:
 *	* ecxt_per_query_memory is a query-lifespan context, typically the same
 *	  context the ExprContext node itself is allocated in.	This context
 *	  can be used for purposes such as storing function call cache info.
 *	* ecxt_per_tuple_memory is a short-term context for expression results.
 *	  As the name suggests, it will typically be reset once per tuple,
 *	  before we begin to evaluate expressions for that tuple.  Each
 *	  ExprContext normally has its very own per-tuple memory context.
 *
 *	CurrentMemoryContext should be set to ecxt_per_tuple_memory before
 *	calling ExecEvalExpr() --- see ExecEvalExprSwitchContext().
 * ----------------
 */
typedef struct ExprContext
{
	NodeTag		type;

	/* Tuples that Var nodes in expression may refer to */
	TupleTableSlot *ecxt_scantuple;
	TupleTableSlot *ecxt_innertuple;
	TupleTableSlot *ecxt_outertuple;

	/* Memory contexts for expression evaluation --- see notes above */
	MemoryContext ecxt_per_query_memory;
	MemoryContext ecxt_per_tuple_memory;

	/* Values to substitute for Param nodes in expression */
	ParamExecData *ecxt_param_exec_vals;		/* for PARAM_EXEC params */
	ParamListInfo ecxt_param_list_info; /* for other param types */

	/* Values to substitute for Aggref nodes in expression */
	Datum	   *ecxt_aggvalues; /* precomputed values for Aggref nodes */
	bool	   *ecxt_aggnulls;	/* null flags for Aggref nodes */

	/* Value to substitute for CaseTestExpr nodes in expression */
	Datum		caseValue_datum;
	bool		caseValue_isNull;

	/* Value to substitute for CoerceToDomainValue nodes in expression */
	Datum		domainValue_datum;
	bool		domainValue_isNull;

	/* Link to containing EState (NULL if a standalone ExprContext) */
	struct EState *ecxt_estate;

	/* Functions to call back when ExprContext is shut down */
	ExprContext_CB *ecxt_callbacks;
} ExprContext;

/*
 * Set-result status returned by ExecEvalExpr()
 */
typedef enum
{
	ExprSingleResult,			/* expression does not return a set */
	ExprMultipleResult,			/* this result is an element of a set */
	ExprEndResult				/* there are no more elements in the set */
} ExprDoneCond;

/*
 * Return modes for functions returning sets.  Note values must be chosen
 * as separate bits so that a bitmask can be formed to indicate supported
 * modes.
 */
typedef enum
{
	SFRM_ValuePerCall = 0x01,	/* one value returned per call */
	SFRM_Materialize = 0x02		/* result set instantiated in Tuplestore */
} SetFunctionReturnMode;

/*
 * When calling a function that might return a set (multiple rows),
 * a node of this type is passed as fcinfo->resultinfo to allow
 * return status to be passed back.  A function returning set should
 * raise an error if no such resultinfo is provided.
 */
typedef struct ReturnSetInfo
{
	NodeTag		type;
	/* values set by caller: */
	ExprContext *econtext;		/* context function is being called in */
	TupleDesc	expectedDesc;	/* tuple descriptor expected by caller */
	int			allowedModes;	/* bitmask: return modes caller can handle */
	/* result status from function (but pre-initialized by caller): */
	SetFunctionReturnMode returnMode;	/* actual return mode */
	ExprDoneCond isDone;		/* status for ValuePerCall mode */
	/* fields filled by function in Materialize return mode: */
	Tuplestorestate *setResult; /* holds the complete returned tuple set */
	TupleDesc	setDesc;		/* actual descriptor for returned tuples */
} ReturnSetInfo;

/* ----------------
 *		ProjectionInfo node information
 *
 *		This is all the information needed to perform projections ---
 *		that is, form new tuples by evaluation of targetlist expressions.
 *		Nodes which need to do projections create one of these.
 *
 *		ExecProject() evaluates the tlist, forms a tuple, and stores it
 *		in the given slot.	Note that the result will be a "virtual" tuple
 *		unless ExecMaterializeSlot() is then called to force it to be
 *		converted to a physical tuple.	The slot must have a tupledesc
 *		that matches the output of the tlist!
 *
 *		The planner very often produces tlists that consist entirely of
 *		simple Var references (lower levels of a plan tree almost always
 *		look like that).  So we have an optimization to handle that case
 *		with minimum overhead.
 *
 *		targetlist		target list for projection
 *		exprContext		expression context in which to evaluate targetlist
 *		slot			slot to place projection result in
 *		itemIsDone		workspace for ExecProject
 *		isVarList		TRUE if simple-Var-list optimization applies
 *		varSlotOffsets	array indicating which slot each simple Var is from
 *		varNumbers		array indicating attr numbers of simple Vars
 *		lastInnerVar	highest attnum from inner tuple slot (0 if none)
 *		lastOuterVar	highest attnum from outer tuple slot (0 if none)
 *		lastScanVar		highest attnum from scan tuple slot (0 if none)
 * ----------------
 */
typedef struct ProjectionInfo
{
	NodeTag		type;
	List	   *pi_targetlist;
	ExprContext *pi_exprContext;
	TupleTableSlot *pi_slot;
	ExprDoneCond *pi_itemIsDone;
	bool		pi_isVarList;
	int		   *pi_varSlotOffsets;
	int		   *pi_varNumbers;
	int			pi_lastInnerVar;
	int			pi_lastOuterVar;
	int			pi_lastScanVar;
} ProjectionInfo;

/* ----------------
 *	  JunkFilter
 *
 *	  This class is used to store information regarding junk attributes.
 *	  A junk attribute is an attribute in a tuple that is needed only for
 *	  storing intermediate information in the executor, and does not belong
 *	  in emitted tuples.  For example, when we do an UPDATE query,
 *	  the planner adds a "junk" entry to the targetlist so that the tuples
 *	  returned to ExecutePlan() contain an extra attribute: the ctid of
 *	  the tuple to be updated.	This is needed to do the update, but we
 *	  don't want the ctid to be part of the stored new tuple!  So, we
 *	  apply a "junk filter" to remove the junk attributes and form the
 *	  real output tuple.  The junkfilter code also provides routines to
 *	  extract the values of the junk attribute(s) from the input tuple.
 *
 *	  targetList:		the original target list (including junk attributes).
 *	  cleanTupType:		the tuple descriptor for the "clean" tuple (with
 *						junk attributes removed).
 *	  cleanMap:			A map with the correspondence between the non-junk
 *						attribute numbers of the "original" tuple and the
 *						attribute numbers of the "clean" tuple.
 *	  resultSlot:		tuple slot used to hold cleaned tuple.
 *	  junkAttNo:		not used by junkfilter code.  Can be used by caller
 *						to remember the attno of a specific junk attribute
 *						(execMain.c stores the "ctid" attno here).
 * ----------------
 */
typedef struct JunkFilter
{
	NodeTag		type;
	List	   *jf_targetList;
	TupleDesc	jf_cleanTupType;
	AttrNumber *jf_cleanMap;
	TupleTableSlot *jf_resultSlot;
	AttrNumber	jf_junkAttNo;
} JunkFilter;

/* ----------------
 *	  ResultRelInfo information
 *
 *		Whenever we update an existing relation, we have to
 *		update indices on the relation, and perhaps also fire triggers.
 *		The ResultRelInfo class is used to hold all the information needed
 *		about a result relation, including indices.. -cim 10/15/89
 *
 *		RangeTableIndex			result relation's range table index
 *		RelationDesc			relation descriptor for result relation
 *		NumIndices				# of indices existing on result relation
 *		IndexRelationDescs		array of relation descriptors for indices
 *		IndexRelationInfo		array of key/attr info for indices
 *		TrigDesc				triggers to be fired, if any
 *		TrigFunctions			cached lookup info for trigger functions
 *		TrigInstrument			optional runtime measurements for triggers
 *		ConstraintExprs			array of constraint-checking expr states
 *		junkFilter				for removing junk attributes from tuples
 *		projectReturning		for computing a RETURNING list
 * ----------------
 */
typedef struct ResultRelInfo
{
	NodeTag		type;
	Index		ri_RangeTableIndex;
	Relation	ri_RelationDesc;
	int			ri_NumIndices;
	RelationPtr ri_IndexRelationDescs;
	IndexInfo **ri_IndexRelationInfo;
	TriggerDesc *ri_TrigDesc;
	FmgrInfo   *ri_TrigFunctions;
	struct Instrumentation *ri_TrigInstrument;
	List	  **ri_ConstraintExprs;
	JunkFilter *ri_junkFilter;
	ProjectionInfo *ri_projectReturning;
} ResultRelInfo;

/* ----------------
 *	  EState information
 *
 * Master working state for an Executor invocation
 * ----------------
 */
typedef struct EState
{
	NodeTag		type;

	/* Basic state for all query types: */
	ScanDirection es_direction; /* current scan direction */
	Snapshot	es_snapshot;	/* time qual to use */
	Snapshot	es_crosscheck_snapshot; /* crosscheck time qual for RI */
	List	   *es_range_table; /* List of RangeTblEntry */

	/* If query can insert/delete tuples, the command ID to mark them with */
	CommandId	es_output_cid;

	/* Info about target table for insert/update/delete queries: */
	ResultRelInfo *es_result_relations; /* array of ResultRelInfos */
	int			es_num_result_relations;		/* length of array */
	ResultRelInfo *es_result_relation_info;		/* currently active array elt */
	JunkFilter *es_junkFilter;	/* currently active junk filter */

	/* Stuff used for firing triggers: */
	List	   *es_trig_target_relations;		/* trigger-only ResultRelInfos */
	TupleTableSlot *es_trig_tuple_slot; /* for trigger output tuples */

	/* Stuff used for SELECT INTO: */
	Relation	es_into_relation_descriptor;
	bool		es_into_relation_use_wal;

	/* Parameter info: */
	ParamListInfo es_param_list_info;	/* values of external params */
	ParamExecData *es_param_exec_vals;	/* values of internal params */

	/* Other working state: */
	MemoryContext es_query_cxt; /* per-query context in which EState lives */

	TupleTable	es_tupleTable;	/* Array of TupleTableSlots */

	uint32		es_processed;	/* # of tuples processed */
	Oid			es_lastoid;		/* last oid processed (by INSERT) */
	List	   *es_rowMarks;	/* not good place, but there is no other */

	bool		es_instrument;	/* true requests runtime instrumentation */
	bool		es_select_into; /* true if doing SELECT INTO */
	bool		es_into_oids;	/* true to generate OIDs in SELECT INTO */

	List	   *es_exprcontexts;	/* List of ExprContexts within EState */

	List	   *es_subplanstates;		/* List of PlanState for SubPlans */

	/*
	 * this ExprContext is for per-output-tuple operations, such as constraint
	 * checks and index-value computations.  It will be reset for each output
	 * tuple.  Note that it will be created only if needed.
	 */
	ExprContext *es_per_tuple_exprcontext;

	/* Below is to re-evaluate plan qual in READ COMMITTED mode */
	PlannedStmt *es_plannedstmt;	/* link to top of plan tree */
	struct evalPlanQual *es_evalPlanQual;		/* chain of PlanQual states */
	bool	   *es_evTupleNull; /* local array of EPQ status */
	HeapTuple  *es_evTuple;		/* shared array of EPQ substitute tuples */
	bool		es_useEvalPlan; /* evaluating EPQ tuples? */
} EState;


/* es_rowMarks is a list of these structs: */
typedef struct ExecRowMark
{
	Relation	relation;		/* opened and RowShareLock'd relation */
	Index		rti;			/* its range table index */
	bool		forUpdate;		/* true = FOR UPDATE, false = FOR SHARE */
	bool		noWait;			/* NOWAIT option */
	AttrNumber	ctidAttNo;		/* resno of its ctid junk attribute */
} ExecRowMark;


/* ----------------------------------------------------------------
 *				 Tuple Hash Tables
 *
 * All-in-memory tuple hash tables are used for a number of purposes.
 *
 * Note: tab_hash_funcs are for the key datatype(s) stored in the table,
 * and tab_eq_funcs are non-cross-type equality operators for those types.
 * Normally these are the only functions used, but FindTupleHashEntry()
 * supports searching a hashtable using cross-data-type hashing.  For that,
 * the caller must supply hash functions for the LHS datatype as well as
 * the cross-type equality operators to use.  in_hash_funcs and cur_eq_funcs
 * are set to point to the caller's function arrays while doing such a search.
 * During LookupTupleHashEntry(), they point to tab_hash_funcs and
 * tab_eq_funcs respectively.
 * ----------------------------------------------------------------
 */
typedef struct TupleHashEntryData *TupleHashEntry;
typedef struct TupleHashTableData *TupleHashTable;

typedef struct TupleHashEntryData
{
	/* firstTuple must be the first field in this struct! */
	MinimalTuple firstTuple;	/* copy of first tuple in this group */
	/* there may be additional data beyond the end of this struct */
} TupleHashEntryData;			/* VARIABLE LENGTH STRUCT */

typedef struct TupleHashTableData
{
	HTAB	   *hashtab;		/* underlying dynahash table */
	int			numCols;		/* number of columns in lookup key */
	AttrNumber *keyColIdx;		/* attr numbers of key columns */
	FmgrInfo   *tab_hash_funcs; /* hash functions for table datatype(s) */
	FmgrInfo   *tab_eq_funcs;	/* equality functions for table datatype(s) */
	MemoryContext tablecxt;		/* memory context containing table */
	MemoryContext tempcxt;		/* context for function evaluations */
	Size		entrysize;		/* actual size to make each hash entry */
	TupleTableSlot *tableslot;	/* slot for referencing table entries */
	/* The following fields are set transiently for each table search: */
	TupleTableSlot *inputslot;	/* current input tuple's slot */
	FmgrInfo   *in_hash_funcs;	/* hash functions for input datatype(s) */
	FmgrInfo   *cur_eq_funcs;	/* equality functions for input vs. table */
} TupleHashTableData;

typedef HASH_SEQ_STATUS TupleHashIterator;

/*
 * Use InitTupleHashIterator/TermTupleHashIterator for a read/write scan.
 * Use ResetTupleHashIterator if the table can be frozen (in this case no
 * explicit scan termination is needed).
 */
#define InitTupleHashIterator(htable, iter) \
	hash_seq_init(iter, (htable)->hashtab)
#define TermTupleHashIterator(iter) \
	hash_seq_term(iter)
#define ResetTupleHashIterator(htable, iter) \
	do { \
		hash_freeze((htable)->hashtab); \
		hash_seq_init(iter, (htable)->hashtab); \
	} while (0)
#define ScanTupleHashTable(iter) \
	((TupleHashEntry) hash_seq_search(iter))


/* ----------------------------------------------------------------
 *				 Expression State Trees
 *
 * Each executable expression tree has a parallel ExprState tree.
 *
 * Unlike PlanState, there is not an exact one-for-one correspondence between
 * ExprState node types and Expr node types.  Many Expr node types have no
 * need for node-type-specific run-time state, and so they can use plain
 * ExprState or GenericExprState as their associated ExprState node type.
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ExprState node
 *
 * ExprState is the common superclass for all ExprState-type nodes.
 *
 * It can also be instantiated directly for leaf Expr nodes that need no
 * local run-time state (such as Var, Const, or Param).
 *
 * To save on dispatch overhead, each ExprState node contains a function
 * pointer to the routine to execute to evaluate the node.
 * ----------------
 */

typedef struct ExprState ExprState;

typedef Datum (*ExprStateEvalFunc) (ExprState *expression,
												ExprContext *econtext,
												bool *isNull,
												ExprDoneCond *isDone);

struct ExprState
{
	NodeTag		type;
	Expr	   *expr;			/* associated Expr node */
	ExprStateEvalFunc evalfunc; /* routine to run to execute node */
};

/* ----------------
 *		GenericExprState node
 *
 * This is used for Expr node types that need no local run-time state,
 * but have one child Expr node.
 * ----------------
 */
typedef struct GenericExprState
{
	ExprState	xprstate;
	ExprState  *arg;			/* state of my child node */
} GenericExprState;

/* ----------------
 *		AggrefExprState node
 * ----------------
 */
typedef struct AggrefExprState
{
	ExprState	xprstate;
	List	   *args;			/* states of argument expressions */
	int			aggno;			/* ID number for agg within its plan node */
} AggrefExprState;

/* ----------------
 *		ArrayRefExprState node
 *
 * Note: array types can be fixed-length (typlen > 0), but only when the
 * element type is itself fixed-length.  Otherwise they are varlena structures
 * and have typlen = -1.  In any case, an array type is never pass-by-value.
 * ----------------
 */
typedef struct ArrayRefExprState
{
	ExprState	xprstate;
	List	   *refupperindexpr;	/* states for child nodes */
	List	   *reflowerindexpr;
	ExprState  *refexpr;
	ExprState  *refassgnexpr;
	int16		refattrlength;	/* typlen of array type */
	int16		refelemlength;	/* typlen of the array element type */
	bool		refelembyval;	/* is the element type pass-by-value? */
	char		refelemalign;	/* typalign of the element type */
} ArrayRefExprState;

/* ----------------
 *		FuncExprState node
 *
 * Although named for FuncExpr, this is also used for OpExpr, DistinctExpr,
 * and NullIf nodes; be careful to check what xprstate.expr is actually
 * pointing at!
 * ----------------
 */
typedef struct FuncExprState
{
	ExprState	xprstate;
	List	   *args;			/* states of argument expressions */

	/*
	 * Function manager's lookup info for the target function.  If func.fn_oid
	 * is InvalidOid, we haven't initialized it yet.
	 */
	FmgrInfo	func;

	/*
	 * We also need to store argument values across calls when evaluating a
	 * function-returning-set.
	 *
	 * setArgsValid is true when we are evaluating a set-valued function and
	 * we are in the middle of a call series; we want to pass the same
	 * argument values to the function again (and again, until it returns
	 * ExprEndResult).
	 */
	bool		setArgsValid;

	/*
	 * Flag to remember whether we found a set-valued argument to the
	 * function. This causes the function result to be a set as well. Valid
	 * only when setArgsValid is true.
	 */
	bool		setHasSetArg;	/* some argument returns a set */

	/*
	 * Flag to remember whether we have registered a shutdown callback for
	 * this FuncExprState.	We do so only if setArgsValid has been true at
	 * least once (since all the callback is for is to clear setArgsValid).
	 */
	bool		shutdown_reg;	/* a shutdown callback is registered */

	/*
	 * Current argument data for a set-valued function; contains valid data
	 * only if setArgsValid is true.
	 */
	FunctionCallInfoData setArgs;
} FuncExprState;

/* ----------------
 *		ScalarArrayOpExprState node
 *
 * This is a FuncExprState plus some additional data.
 * ----------------
 */
typedef struct ScalarArrayOpExprState
{
	FuncExprState fxprstate;
	/* Cached info about array element type */
	Oid			element_type;
	int16		typlen;
	bool		typbyval;
	char		typalign;
} ScalarArrayOpExprState;

/* ----------------
 *		BoolExprState node
 * ----------------
 */
typedef struct BoolExprState
{
	ExprState	xprstate;
	List	   *args;			/* states of argument expression(s) */
} BoolExprState;

/* ----------------
 *		SubPlanState node
 * ----------------
 */
typedef struct SubPlanState
{
	ExprState	xprstate;
	struct PlanState *planstate;	/* subselect plan's state tree */
	ExprState  *testexpr;		/* state of combining expression */
	List	   *args;			/* states of argument expression(s) */
	HeapTuple	curTuple;		/* copy of most recent tuple from subplan */
	/* these are used when hashing the subselect's output: */
	ProjectionInfo *projLeft;	/* for projecting lefthand exprs */
	ProjectionInfo *projRight;	/* for projecting subselect output */
	TupleHashTable hashtable;	/* hash table for no-nulls subselect rows */
	TupleHashTable hashnulls;	/* hash table for rows with null(s) */
	bool		havehashrows;	/* TRUE if hashtable is not empty */
	bool		havenullrows;	/* TRUE if hashnulls is not empty */
	MemoryContext tablecxt;		/* memory context containing tables */
	ExprContext *innerecontext; /* working context for comparisons */
	AttrNumber *keyColIdx;		/* control data for hash tables */
	FmgrInfo   *tab_hash_funcs; /* hash functions for table datatype(s) */
	FmgrInfo   *tab_eq_funcs;	/* equality functions for table datatype(s) */
	FmgrInfo   *lhs_hash_funcs; /* hash functions for lefthand datatype(s) */
	FmgrInfo   *cur_eq_funcs;	/* equality functions for LHS vs. table */
} SubPlanState;

/* ----------------
 *		FieldSelectState node
 * ----------------
 */
typedef struct FieldSelectState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input expression */
	TupleDesc	argdesc;		/* tupdesc for most recent input */
} FieldSelectState;

/* ----------------
 *		FieldStoreState node
 * ----------------
 */
typedef struct FieldStoreState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input tuple value */
	List	   *newvals;		/* new value(s) for field(s) */
	TupleDesc	argdesc;		/* tupdesc for most recent input */
} FieldStoreState;

/* ----------------
 *		CoerceViaIOState node
 * ----------------
 */
typedef struct CoerceViaIOState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input expression */
	FmgrInfo	outfunc;		/* lookup info for source output function */
	FmgrInfo	infunc;			/* lookup info for result input function */
	Oid			intypioparam;	/* argument needed for input function */
} CoerceViaIOState;

/* ----------------
 *		ArrayCoerceExprState node
 * ----------------
 */
typedef struct ArrayCoerceExprState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input array value */
	Oid			resultelemtype; /* element type of result array */
	FmgrInfo	elemfunc;		/* lookup info for element coercion function */
	/* use struct pointer to avoid including array.h here */
	struct ArrayMapState *amstate;		/* workspace for array_map */
} ArrayCoerceExprState;

/* ----------------
 *		ConvertRowtypeExprState node
 * ----------------
 */
typedef struct ConvertRowtypeExprState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input tuple value */
	TupleDesc	indesc;			/* tupdesc for source rowtype */
	TupleDesc	outdesc;		/* tupdesc for result rowtype */
	AttrNumber *attrMap;		/* indexes of input fields, or 0 for null */
	Datum	   *invalues;		/* workspace for deconstructing source */
	bool	   *inisnull;
	Datum	   *outvalues;		/* workspace for constructing result */
	bool	   *outisnull;
} ConvertRowtypeExprState;

/* ----------------
 *		CaseExprState node
 * ----------------
 */
typedef struct CaseExprState
{
	ExprState	xprstate;
	ExprState  *arg;			/* implicit equality comparison argument */
	List	   *args;			/* the arguments (list of WHEN clauses) */
	ExprState  *defresult;		/* the default result (ELSE clause) */
} CaseExprState;

/* ----------------
 *		CaseWhenState node
 * ----------------
 */
typedef struct CaseWhenState
{
	ExprState	xprstate;
	ExprState  *expr;			/* condition expression */
	ExprState  *result;			/* substitution result */
} CaseWhenState;

/* ----------------
 *		ArrayExprState node
 *
 * Note: ARRAY[] expressions always produce varlena arrays, never fixed-length
 * arrays.
 * ----------------
 */
typedef struct ArrayExprState
{
	ExprState	xprstate;
	List	   *elements;		/* states for child nodes */
	int16		elemlength;		/* typlen of the array element type */
	bool		elembyval;		/* is the element type pass-by-value? */
	char		elemalign;		/* typalign of the element type */
} ArrayExprState;

/* ----------------
 *		RowExprState node
 * ----------------
 */
typedef struct RowExprState
{
	ExprState	xprstate;
	List	   *args;			/* the arguments */
	TupleDesc	tupdesc;		/* descriptor for result tuples */
} RowExprState;

/* ----------------
 *		RowCompareExprState node
 * ----------------
 */
typedef struct RowCompareExprState
{
	ExprState	xprstate;
	List	   *largs;			/* the left-hand input arguments */
	List	   *rargs;			/* the right-hand input arguments */
	FmgrInfo   *funcs;			/* array of comparison function info */
} RowCompareExprState;

/* ----------------
 *		CoalesceExprState node
 * ----------------
 */
typedef struct CoalesceExprState
{
	ExprState	xprstate;
	List	   *args;			/* the arguments */
} CoalesceExprState;

/* ----------------
 *		MinMaxExprState node
 * ----------------
 */
typedef struct MinMaxExprState
{
	ExprState	xprstate;
	List	   *args;			/* the arguments */
	FmgrInfo	cfunc;			/* lookup info for comparison func */
} MinMaxExprState;

/* ----------------
 *		XmlExprState node
 * ----------------
 */
typedef struct XmlExprState
{
	ExprState	xprstate;
	List	   *named_args;		/* ExprStates for named arguments */
	FmgrInfo   *named_outfuncs; /* array of output fns for named arguments */
	List	   *args;			/* ExprStates for other arguments */
} XmlExprState;

/* ----------------
 *		NullTestState node
 * ----------------
 */
typedef struct NullTestState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input expression */
	bool		argisrow;		/* T if input is of a composite type */
	/* used only if argisrow: */
	TupleDesc	argdesc;		/* tupdesc for most recent input */
} NullTestState;

/* ----------------
 *		CoerceToDomainState node
 * ----------------
 */
typedef struct CoerceToDomainState
{
	ExprState	xprstate;
	ExprState  *arg;			/* input expression */
	/* Cached list of constraints that need to be checked */
	List	   *constraints;	/* list of DomainConstraintState nodes */
} CoerceToDomainState;

/*
 * DomainConstraintState - one item to check during CoerceToDomain
 *
 * Note: this is just a Node, and not an ExprState, because it has no
 * corresponding Expr to link to.  Nonetheless it is part of an ExprState
 * tree, so we give it a name following the xxxState convention.
 */
typedef enum DomainConstraintType
{
	DOM_CONSTRAINT_NOTNULL,
	DOM_CONSTRAINT_CHECK
} DomainConstraintType;

typedef struct DomainConstraintState
{
	NodeTag		type;
	DomainConstraintType constrainttype;		/* constraint type */
	char	   *name;			/* name of constraint (for error msgs) */
	ExprState  *check_expr;		/* for CHECK, a boolean expression */
} DomainConstraintState;


/* ----------------------------------------------------------------
 *				 Executor State Trees
 *
 * An executing query has a PlanState tree paralleling the Plan tree
 * that describes the plan.
 * ----------------------------------------------------------------
 */

/* ----------------
 *		PlanState node
 *
 * We never actually instantiate any PlanState nodes; this is just the common
 * abstract superclass for all PlanState-type nodes.
 * ----------------
 */
typedef struct PlanState
{
	NodeTag		type;

	Plan	   *plan;			/* associated Plan node */

	EState	   *state;			/* at execution time, state's of individual
								 * nodes point to one EState for the whole
								 * top-level plan */

	struct Instrumentation *instrument; /* Optional runtime stats for this
										 * plan node */

	/*
	 * Common structural data for all Plan types.  These links to subsidiary
	 * state trees parallel links in the associated plan tree (except for the
	 * subPlan list, which does not exist in the plan tree).
	 */
	List	   *targetlist;		/* target list to be computed at this node */
	List	   *qual;			/* implicitly-ANDed qual conditions */
	struct PlanState *lefttree; /* input plan tree(s) */
	struct PlanState *righttree;
	List	   *initPlan;		/* Init SubPlanState nodes (un-correlated expr
								 * subselects) */
	List	   *subPlan;		/* SubPlanState nodes in my expressions */

	/*
	 * State for management of parameter-change-driven rescanning
	 */
	Bitmapset  *chgParam;		/* set of IDs of changed Params */

	/*
	 * Other run-time state needed by most if not all node types.
	 */
	TupleTableSlot *ps_OuterTupleSlot;	/* slot for current "outer" tuple */
	TupleTableSlot *ps_ResultTupleSlot; /* slot for my result tuples */
	ExprContext *ps_ExprContext;	/* node's expression-evaluation context */
	ProjectionInfo *ps_ProjInfo;	/* info for doing tuple projection */
	bool		ps_TupFromTlist;/* state flag for processing set-valued
								 * functions in targetlist */
} PlanState;

/* ----------------
 *	these are are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * ----------------
 */
#define innerPlanState(node)		(((PlanState *)(node))->righttree)
#define outerPlanState(node)		(((PlanState *)(node))->lefttree)


/* ----------------
 *	 ResultState information
 * ----------------
 */
typedef struct ResultState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *resconstantqual;
	bool		rs_done;		/* are we done? */
	bool		rs_checkqual;	/* do we need to check the qual? */
} ResultState;

/* ----------------
 *	 AppendState information
 *
 *		nplans			how many plans are in the list
 *		whichplan		which plan is being executed (0 .. n-1)
 *		firstplan		first plan to execute (usually 0)
 *		lastplan		last plan to execute (usually n-1)
 * ----------------
 */
typedef struct AppendState
{
	PlanState	ps;				/* its first field is NodeTag */
	PlanState **appendplans;	/* array of PlanStates for my inputs */
	int			as_nplans;
	int			as_whichplan;
	int			as_firstplan;
	int			as_lastplan;
} AppendState;

/* ----------------
 *	 BitmapAndState information
 * ----------------
 */
typedef struct BitmapAndState
{
	PlanState	ps;				/* its first field is NodeTag */
	PlanState **bitmapplans;	/* array of PlanStates for my inputs */
	int			nplans;			/* number of input plans */
} BitmapAndState;

/* ----------------
 *	 BitmapOrState information
 * ----------------
 */
typedef struct BitmapOrState
{
	PlanState	ps;				/* its first field is NodeTag */
	PlanState **bitmapplans;	/* array of PlanStates for my inputs */
	int			nplans;			/* number of input plans */
} BitmapOrState;

/* ----------------------------------------------------------------
 *				 Scan State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 ScanState information
 *
 *		ScanState extends PlanState for node types that represent
 *		scans of an underlying relation.  It can also be used for nodes
 *		that scan the output of an underlying plan node --- in that case,
 *		only ScanTupleSlot is actually useful, and it refers to the tuple
 *		retrieved from the subplan.
 *
 *		currentRelation    relation being scanned (NULL if none)
 *		currentScanDesc    current scan descriptor for scan (NULL if none)
 *		ScanTupleSlot	   pointer to slot in tuple table holding scan tuple
 * ----------------
 */
typedef struct ScanState
{
	PlanState	ps;				/* its first field is NodeTag */
	Relation	ss_currentRelation;
	HeapScanDesc ss_currentScanDesc;
	TupleTableSlot *ss_ScanTupleSlot;
} ScanState;

/*
 * SeqScan uses a bare ScanState as its state node, since it needs
 * no additional fields.
 */
typedef ScanState SeqScanState;

/*
 * These structs store information about index quals that don't have simple
 * constant right-hand sides.  See comments for ExecIndexBuildScanKeys()
 * for discussion.
 */
typedef struct
{
	ScanKey		scan_key;		/* scankey to put value into */
	ExprState  *key_expr;		/* expr to evaluate to get value */
} IndexRuntimeKeyInfo;

typedef struct
{
	ScanKey		scan_key;		/* scankey to put value into */
	ExprState  *array_expr;		/* expr to evaluate to get array value */
	int			next_elem;		/* next array element to use */
	int			num_elems;		/* number of elems in current array value */
	Datum	   *elem_values;	/* array of num_elems Datums */
	bool	   *elem_nulls;		/* array of num_elems is-null flags */
} IndexArrayKeyInfo;

/* ----------------
 *	 IndexScanState information
 *
 *		indexqualorig	   execution state for indexqualorig expressions
 *		ScanKeys		   Skey structures to scan index rel
 *		NumScanKeys		   number of Skey structs
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys structs
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 * ----------------
 */
typedef struct IndexScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *indexqualorig;
	ScanKey		iss_ScanKeys;
	int			iss_NumScanKeys;
	IndexRuntimeKeyInfo *iss_RuntimeKeys;
	int			iss_NumRuntimeKeys;
	bool		iss_RuntimeKeysReady;
	ExprContext *iss_RuntimeContext;
	Relation	iss_RelationDesc;
	IndexScanDesc iss_ScanDesc;
} IndexScanState;

/* ----------------
 *	 BitmapIndexScanState information
 *
 *		result			   bitmap to return output into, or NULL
 *		ScanKeys		   Skey structures to scan index rel
 *		NumScanKeys		   number of Skey structs
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys structs
 *		ArrayKeys		   info about Skeys that come from ScalarArrayOpExprs
 *		NumArrayKeys	   number of ArrayKeys structs
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 * ----------------
 */
typedef struct BitmapIndexScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	TIDBitmap  *biss_result;
	ScanKey		biss_ScanKeys;
	int			biss_NumScanKeys;
	IndexRuntimeKeyInfo *biss_RuntimeKeys;
	int			biss_NumRuntimeKeys;
	IndexArrayKeyInfo *biss_ArrayKeys;
	int			biss_NumArrayKeys;
	bool		biss_RuntimeKeysReady;
	ExprContext *biss_RuntimeContext;
	Relation	biss_RelationDesc;
	IndexScanDesc biss_ScanDesc;
} BitmapIndexScanState;

/* ----------------
 *	 BitmapHeapScanState information
 *
 *		bitmapqualorig	   execution state for bitmapqualorig expressions
 *		tbm				   bitmap obtained from child index scan(s)
 *		tbmres			   current-page data
 * ----------------
 */
typedef struct BitmapHeapScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *bitmapqualorig;
	TIDBitmap  *tbm;
	TBMIterateResult *tbmres;
} BitmapHeapScanState;

/* ----------------
 *	 TidScanState information
 *
 *		isCurrentOf    scan has a CurrentOfExpr qual
 *		NumTids		   number of tids in this scan
 *		TidPtr		   index of currently fetched tid
 *		TidList		   evaluated item pointers (array of size NumTids)
 * ----------------
 */
typedef struct TidScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *tss_tidquals;	/* list of ExprState nodes */
	bool		tss_isCurrentOf;
	int			tss_NumTids;
	int			tss_TidPtr;
	int			tss_MarkTidPtr;
	ItemPointerData *tss_TidList;
	HeapTupleData tss_htup;
} TidScanState;

/* ----------------
 *	 SubqueryScanState information
 *
 *		SubqueryScanState is used for scanning a sub-query in the range table.
 *		ScanTupleSlot references the current output tuple of the sub-query.
 * ----------------
 */
typedef struct SubqueryScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	PlanState  *subplan;
} SubqueryScanState;

/* ----------------
 *	 FunctionScanState information
 *
 *		Function nodes are used to scan the results of a
 *		function appearing in FROM (typically a function returning set).
 *
 *		tupdesc				expected return tuple description
 *		tuplestorestate		private state of tuplestore.c
 *		funcexpr			state for function expression being evaluated
 * ----------------
 */
typedef struct FunctionScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	TupleDesc	tupdesc;
	Tuplestorestate *tuplestorestate;
	ExprState  *funcexpr;
} FunctionScanState;

/* ----------------
 *	 ValuesScanState information
 *
 *		ValuesScan nodes are used to scan the results of a VALUES list
 *
 *		rowcontext			per-expression-list context
 *		exprlists			array of expression lists being evaluated
 *		array_len			size of array
 *		curr_idx			current array index (0-based)
 *		marked_idx			marked position (for mark/restore)
 *
 *	Note: ss.ps.ps_ExprContext is used to evaluate any qual or projection
 *	expressions attached to the node.  We create a second ExprContext,
 *	rowcontext, in which to build the executor expression state for each
 *	Values sublist.  Resetting this context lets us get rid of expression
 *	state for each row, avoiding major memory leakage over a long values list.
 * ----------------
 */
typedef struct ValuesScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprContext *rowcontext;
	List	  **exprlists;
	int			array_len;
	int			curr_idx;
	int			marked_idx;
} ValuesScanState;

/* ----------------------------------------------------------------
 *				 Join State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 JoinState information
 *
 *		Superclass for state nodes of join plans.
 * ----------------
 */
typedef struct JoinState
{
	PlanState	ps;
	JoinType	jointype;
	List	   *joinqual;		/* JOIN quals (in addition to ps.qual) */
} JoinState;

/* ----------------
 *	 NestLoopState information
 *
 *		NeedNewOuter	   true if need new outer tuple on next call
 *		MatchedOuter	   true if found a join match for current outer tuple
 *		NullInnerTupleSlot prepared null tuple for left outer joins
 * ----------------
 */
typedef struct NestLoopState
{
	JoinState	js;				/* its first field is NodeTag */
	bool		nl_NeedNewOuter;
	bool		nl_MatchedOuter;
	TupleTableSlot *nl_NullInnerTupleSlot;
} NestLoopState;

/* ----------------
 *	 MergeJoinState information
 *
 *		NumClauses		   number of mergejoinable join clauses
 *		Clauses			   info for each mergejoinable clause
 *		JoinState		   current "state" of join.  see execdefs.h
 *		ExtraMarks		   true to issue extra Mark operations on inner scan
 *		FillOuter		   true if should emit unjoined outer tuples anyway
 *		FillInner		   true if should emit unjoined inner tuples anyway
 *		MatchedOuter	   true if found a join match for current outer tuple
 *		MatchedInner	   true if found a join match for current inner tuple
 *		OuterTupleSlot	   slot in tuple table for cur outer tuple
 *		InnerTupleSlot	   slot in tuple table for cur inner tuple
 *		MarkedTupleSlot    slot in tuple table for marked tuple
 *		NullOuterTupleSlot prepared null tuple for right outer joins
 *		NullInnerTupleSlot prepared null tuple for left outer joins
 *		OuterEContext	   workspace for computing outer tuple's join values
 *		InnerEContext	   workspace for computing inner tuple's join values
 * ----------------
 */
/* private in nodeMergejoin.c: */
typedef struct MergeJoinClauseData *MergeJoinClause;

typedef struct MergeJoinState
{
	JoinState	js;				/* its first field is NodeTag */
	int			mj_NumClauses;
	MergeJoinClause mj_Clauses; /* array of length mj_NumClauses */
	int			mj_JoinState;
	bool		mj_ExtraMarks;
	bool		mj_FillOuter;
	bool		mj_FillInner;
	bool		mj_MatchedOuter;
	bool		mj_MatchedInner;
	TupleTableSlot *mj_OuterTupleSlot;
	TupleTableSlot *mj_InnerTupleSlot;
	TupleTableSlot *mj_MarkedTupleSlot;
	TupleTableSlot *mj_NullOuterTupleSlot;
	TupleTableSlot *mj_NullInnerTupleSlot;
	ExprContext *mj_OuterEContext;
	ExprContext *mj_InnerEContext;
} MergeJoinState;

/* ----------------
 *	 HashJoinState information
 *
 *		hj_HashTable			hash table for the hashjoin
 *								(NULL if table not built yet)
 *		hj_CurHashValue			hash value for current outer tuple
 *		hj_CurBucketNo			bucket# for current outer tuple
 *		hj_CurTuple				last inner tuple matched to current outer
 *								tuple, or NULL if starting search
 *								(CurHashValue, CurBucketNo and CurTuple are
 *								 undefined if OuterTupleSlot is empty!)
 *		hj_OuterHashKeys		the outer hash keys in the hashjoin condition
 *		hj_InnerHashKeys		the inner hash keys in the hashjoin condition
 *		hj_HashOperators		the join operators in the hashjoin condition
 *		hj_OuterTupleSlot		tuple slot for outer tuples
 *		hj_HashTupleSlot		tuple slot for hashed tuples
 *		hj_NullInnerTupleSlot	prepared null tuple for left outer joins
 *		hj_FirstOuterTupleSlot	first tuple retrieved from outer plan
 *		hj_NeedNewOuter			true if need new outer tuple on next call
 *		hj_MatchedOuter			true if found a join match for current outer
 *		hj_OuterNotEmpty		true if outer relation known not empty
 * ----------------
 */

/* these structs are defined in executor/hashjoin.h: */
typedef struct HashJoinTupleData *HashJoinTuple;
typedef struct HashJoinTableData *HashJoinTable;

typedef struct HashJoinState
{
	JoinState	js;				/* its first field is NodeTag */
	List	   *hashclauses;	/* list of ExprState nodes */
	HashJoinTable hj_HashTable;
	uint32		hj_CurHashValue;
	int			hj_CurBucketNo;
	HashJoinTuple hj_CurTuple;
	List	   *hj_OuterHashKeys;		/* list of ExprState nodes */
	List	   *hj_InnerHashKeys;		/* list of ExprState nodes */
	List	   *hj_HashOperators;		/* list of operator OIDs */
	TupleTableSlot *hj_OuterTupleSlot;
	TupleTableSlot *hj_HashTupleSlot;
	TupleTableSlot *hj_NullInnerTupleSlot;
	TupleTableSlot *hj_FirstOuterTupleSlot;
	bool		hj_NeedNewOuter;
	bool		hj_MatchedOuter;
	bool		hj_OuterNotEmpty;
} HashJoinState;


/* ----------------------------------------------------------------
 *				 Materialization State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 MaterialState information
 *
 *		materialize nodes are used to materialize the results
 *		of a subplan into a temporary file.
 *
 *		ss.ss_ScanTupleSlot refers to output of underlying plan.
 * ----------------
 */
typedef struct MaterialState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			eflags;			/* capability flags to pass to tuplestore */
	bool		eof_underlying; /* reached end of underlying plan? */
	void	   *tuplestorestate;	/* private state of tuplestore.c */
} MaterialState;

/* ----------------
 *	 SortState information
 * ----------------
 */
typedef struct SortState
{
	ScanState	ss;				/* its first field is NodeTag */
	bool		randomAccess;	/* need random access to sort output? */
	bool		bounded;		/* is the result set bounded? */
	int64		bound;			/* if bounded, how many tuples are needed */
	bool		sort_Done;		/* sort completed yet? */
	bool		bounded_Done;	/* value of bounded we did the sort with */
	int64		bound_Done;		/* value of bound we did the sort with */
	void	   *tuplesortstate; /* private state of tuplesort.c */
} SortState;

/* ---------------------
 *	GroupState information
 * -------------------------
 */
typedef struct GroupState
{
	ScanState	ss;				/* its first field is NodeTag */
	FmgrInfo   *eqfunctions;	/* per-field lookup data for equality fns */
	bool		grp_done;		/* indicates completion of Group scan */
} GroupState;

/* ---------------------
 *	AggState information
 *
 *	ss.ss_ScanTupleSlot refers to output of underlying plan.
 *
 *	Note: ss.ps.ps_ExprContext contains ecxt_aggvalues and
 *	ecxt_aggnulls arrays, which hold the computed agg values for the current
 *	input group during evaluation of an Agg node's output tuple(s).  We
 *	create a second ExprContext, tmpcontext, in which to evaluate input
 *	expressions and run the aggregate transition functions.
 * -------------------------
 */
/* these structs are private in nodeAgg.c: */
typedef struct AggStatePerAggData *AggStatePerAgg;
typedef struct AggStatePerGroupData *AggStatePerGroup;

typedef struct AggState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *aggs;			/* all Aggref nodes in targetlist & quals */
	int			numaggs;		/* length of list (could be zero!) */
	FmgrInfo   *eqfunctions;	/* per-grouping-field equality fns */
	FmgrInfo   *hashfunctions;	/* per-grouping-field hash fns */
	AggStatePerAgg peragg;		/* per-Aggref information */
	MemoryContext aggcontext;	/* memory context for long-lived data */
	ExprContext *tmpcontext;	/* econtext for input expressions */
	bool		agg_done;		/* indicates completion of Agg scan */
	/* these fields are used in AGG_PLAIN and AGG_SORTED modes: */
	AggStatePerGroup pergroup;	/* per-Aggref-per-group working state */
	HeapTuple	grp_firstTuple; /* copy of first tuple of current group */
	/* these fields are used in AGG_HASHED mode: */
	TupleHashTable hashtable;	/* hash table with one entry per group */
	TupleTableSlot *hashslot;	/* slot for loading hash table */
	List	   *hash_needed;	/* list of columns needed in hash table */
	bool		table_filled;	/* hash table filled yet? */
	TupleHashIterator hashiter; /* for iterating through hash table */
} AggState;

/* ----------------
 *	 UniqueState information
 *
 *		Unique nodes are used "on top of" sort nodes to discard
 *		duplicate tuples returned from the sort phase.	Basically
 *		all it does is compare the current tuple from the subplan
 *		with the previously fetched tuple (stored in its result slot).
 *		If the two are identical in all interesting fields, then
 *		we just fetch another tuple from the sort and try again.
 * ----------------
 */
typedef struct UniqueState
{
	PlanState	ps;				/* its first field is NodeTag */
	FmgrInfo   *eqfunctions;	/* per-field lookup data for equality fns */
	MemoryContext tempContext;	/* short-term context for comparisons */
} UniqueState;

/* ----------------
 *	 HashState information
 * ----------------
 */
typedef struct HashState
{
	PlanState	ps;				/* its first field is NodeTag */
	HashJoinTable hashtable;	/* hash table for the hashjoin */
	List	   *hashkeys;		/* list of ExprState nodes */
	/* hashkeys is same as parent's hj_InnerHashKeys */
} HashState;

/* ----------------
 *	 SetOpState information
 *
 *		SetOp nodes are used "on top of" sort nodes to discard
 *		duplicate tuples returned from the sort phase.	These are
 *		more complex than a simple Unique since we have to count
 *		how many duplicates to return.
 * ----------------
 */
typedef struct SetOpState
{
	PlanState	ps;				/* its first field is NodeTag */
	FmgrInfo   *eqfunctions;	/* per-field lookup data for equality fns */
	bool		subplan_done;	/* has subplan returned EOF? */
	long		numLeft;		/* number of left-input dups of cur group */
	long		numRight;		/* number of right-input dups of cur group */
	long		numOutput;		/* number of dups left to output */
	MemoryContext tempContext;	/* short-term context for comparisons */
} SetOpState;

/* ----------------
 *	 LimitState information
 *
 *		Limit nodes are used to enforce LIMIT/OFFSET clauses.
 *		They just select the desired subrange of their subplan's output.
 *
 * offset is the number of initial tuples to skip (0 does nothing).
 * count is the number of tuples to return after skipping the offset tuples.
 * If no limit count was specified, count is undefined and noCount is true.
 * When lstate == LIMIT_INITIAL, offset/count/noCount haven't been set yet.
 * ----------------
 */
typedef enum
{
	LIMIT_INITIAL,				/* initial state for LIMIT node */
	LIMIT_RESCAN,				/* rescan after recomputing parameters */
	LIMIT_EMPTY,				/* there are no returnable rows */
	LIMIT_INWINDOW,				/* have returned a row in the window */
	LIMIT_SUBPLANEOF,			/* at EOF of subplan (within window) */
	LIMIT_WINDOWEND,			/* stepped off end of window */
	LIMIT_WINDOWSTART			/* stepped off beginning of window */
} LimitStateCond;

typedef struct LimitState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *limitOffset;	/* OFFSET parameter, or NULL if none */
	ExprState  *limitCount;		/* COUNT parameter, or NULL if none */
	int64		offset;			/* current OFFSET value */
	int64		count;			/* current COUNT, if any */
	bool		noCount;		/* if true, ignore count */
	LimitStateCond lstate;		/* state machine status, as above */
	int64		position;		/* 1-based index of last tuple returned */
	TupleTableSlot *subSlot;	/* tuple last obtained from subplan */
} LimitState;

#endif   /* EXECNODES_H */
