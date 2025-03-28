/*-------------------------------------------------------------------------
 *
 * execnodes.h
 *	  definitions for executor state nodes
 *
 * Most plan node types declared in plannodes.h have a corresponding
 * execution-state node type declared here.  An exception is that
 * expression nodes (subtypes of Expr) are usually represented by steps
 * of an ExprState, and fully handled within execExpr* - but sometimes
 * their state needs to be shared with other parts of the executor, as
 * for example with SubPlanState, which nodeSubplan.c has to modify.
 *
 * Node types declared in this file do not have any copy/equal/out/read
 * support.  (That is currently hard-wired in gen_node_support.pl, rather
 * than being explicitly represented by pg_node_attr decorations here.)
 * There is no need for copy, equal, or read support for executor trees.
 * Output support could be useful for debugging; but there are a lot of
 * specialized fields that would require custom code, so for now it's
 * not provided.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/execnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECNODES_H
#define EXECNODES_H

#include "access/tupconvert.h"
#include "executor/instrument.h"
#include "fmgr.h"
#include "lib/ilist.h"
#include "lib/pairingheap.h"
#include "nodes/miscnodes.h"
#include "nodes/params.h"
#include "nodes/plannodes.h"
#include "nodes/tidbitmap.h"
#include "partitioning/partdefs.h"
#include "storage/condition_variable.h"
#include "utils/hsearch.h"
#include "utils/queryenvironment.h"
#include "utils/plancache.h"
#include "utils/reltrigger.h"
#include "utils/sharedtuplestore.h"
#include "utils/snapshot.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"
#include "utils/tuplestore.h"

struct PlanState;				/* forward references in this file */
struct ParallelHashJoinState;
struct ExecRowMark;
struct ExprState;
struct ExprContext;
struct RangeTblEntry;			/* avoid including parsenodes.h here */
struct ExprEvalStep;			/* avoid including execExpr.h everywhere */
struct CopyMultiInsertBuffer;
struct LogicalTapeSet;


/* ----------------
 *		ExprState node
 *
 * ExprState represents the evaluation state for a whole expression tree.
 * It contains instructions (in ->steps) to evaluate the expression.
 * ----------------
 */
typedef Datum (*ExprStateEvalFunc) (struct ExprState *expression,
									struct ExprContext *econtext,
									bool *isNull);

/* Bits in ExprState->flags (see also execExpr.h for private flag bits): */
/* expression is for use with ExecQual() */
#define EEO_FLAG_IS_QUAL					(1 << 0)
/* expression refers to OLD table columns */
#define EEO_FLAG_HAS_OLD					(1 << 1)
/* expression refers to NEW table columns */
#define EEO_FLAG_HAS_NEW					(1 << 2)
/* OLD table row is NULL in RETURNING list */
#define EEO_FLAG_OLD_IS_NULL				(1 << 3)
/* NEW table row is NULL in RETURNING list */
#define EEO_FLAG_NEW_IS_NULL				(1 << 4)

typedef struct ExprState
{
	NodeTag		type;

#define FIELDNO_EXPRSTATE_FLAGS 1
	uint8		flags;			/* bitmask of EEO_FLAG_* bits, see above */

	/*
	 * Storage for result value of a scalar expression, or for individual
	 * column results within expressions built by ExecBuildProjectionInfo().
	 */
#define FIELDNO_EXPRSTATE_RESNULL 2
	bool		resnull;
#define FIELDNO_EXPRSTATE_RESVALUE 3
	Datum		resvalue;

	/*
	 * If projecting a tuple result, this slot holds the result; else NULL.
	 */
#define FIELDNO_EXPRSTATE_RESULTSLOT 4
	TupleTableSlot *resultslot;

	/*
	 * Instructions to compute expression's return value.
	 */
	struct ExprEvalStep *steps;

	/*
	 * Function that actually evaluates the expression.  This can be set to
	 * different values depending on the complexity of the expression.
	 */
	ExprStateEvalFunc evalfunc;

	/* original expression tree, for debugging only */
	Expr	   *expr;

	/* private state for an evalfunc */
	void	   *evalfunc_private;

	/*
	 * XXX: following fields only needed during "compilation" (ExecInitExpr);
	 * could be thrown away afterwards.
	 */

	int			steps_len;		/* number of steps currently */
	int			steps_alloc;	/* allocated length of steps array */

#define FIELDNO_EXPRSTATE_PARENT 11
	struct PlanState *parent;	/* parent PlanState node, if any */
	ParamListInfo ext_params;	/* for compiling PARAM_EXTERN nodes */

	Datum	   *innermost_caseval;
	bool	   *innermost_casenull;

	Datum	   *innermost_domainval;
	bool	   *innermost_domainnull;

	/*
	 * For expression nodes that support soft errors. Should be set to NULL if
	 * the caller wants errors to be thrown. Callers that do not want errors
	 * thrown should set it to a valid ErrorSaveContext before calling
	 * ExecInitExprRec().
	 */
	ErrorSaveContext *escontext;
} ExprState;


/* ----------------
 *	  IndexInfo information
 *
 *		this struct holds the information needed to construct new index
 *		entries for a particular index.  Used for both index_build and
 *		retail creation of index entries.
 *
 *		NumIndexAttrs		total number of columns in this index
 *		NumIndexKeyAttrs	number of key columns in index
 *		IndexAttrNumbers	underlying-rel attribute numbers used as keys
 *							(zeroes indicate expressions). It also contains
 * 							info about included columns.
 *		Expressions			expr trees for expression entries, or NIL if none
 *		ExpressionsState	exec state for expressions, or NIL if none
 *		Predicate			partial-index predicate, or NIL if none
 *		PredicateState		exec state for predicate, or NIL if none
 *		ExclusionOps		Per-column exclusion operators, or NULL if none
 *		ExclusionProcs		Underlying function OIDs for ExclusionOps
 *		ExclusionStrats		Opclass strategy numbers for ExclusionOps
 *		UniqueOps			These are like Exclusion*, but for unique indexes
 *		UniqueProcs
 *		UniqueStrats
 *		Unique				is it a unique index?
 *		OpclassOptions		opclass-specific options, or NULL if none
 *		ReadyForInserts		is it valid for inserts?
 *		CheckedUnchanged	IndexUnchanged status determined yet?
 *		IndexUnchanged		aminsert hint, cached for retail inserts
 *		Concurrent			are we doing a concurrent index build?
 *		BrokenHotChain		did we detect any broken HOT chains?
 *		Summarizing			is it a summarizing index?
 *		ParallelWorkers		# of workers requested (excludes leader)
 *		Am					Oid of index AM
 *		AmCache				private cache area for index AM
 *		Context				memory context holding this IndexInfo
 *
 * ii_Concurrent, ii_BrokenHotChain, and ii_ParallelWorkers are used only
 * during index build; they're conventionally zeroed otherwise.
 * ----------------
 */
typedef struct IndexInfo
{
	NodeTag		type;
	int			ii_NumIndexAttrs;	/* total number of columns in index */
	int			ii_NumIndexKeyAttrs;	/* number of key columns in index */
	AttrNumber	ii_IndexAttrNumbers[INDEX_MAX_KEYS];
	List	   *ii_Expressions; /* list of Expr */
	List	   *ii_ExpressionsState;	/* list of ExprState */
	List	   *ii_Predicate;	/* list of Expr */
	ExprState  *ii_PredicateState;
	Oid		   *ii_ExclusionOps;	/* array with one entry per column */
	Oid		   *ii_ExclusionProcs;	/* array with one entry per column */
	uint16	   *ii_ExclusionStrats; /* array with one entry per column */
	Oid		   *ii_UniqueOps;	/* array with one entry per column */
	Oid		   *ii_UniqueProcs; /* array with one entry per column */
	uint16	   *ii_UniqueStrats;	/* array with one entry per column */
	bool		ii_Unique;
	bool		ii_NullsNotDistinct;
	bool		ii_ReadyForInserts;
	bool		ii_CheckedUnchanged;
	bool		ii_IndexUnchanged;
	bool		ii_Concurrent;
	bool		ii_BrokenHotChain;
	bool		ii_Summarizing;
	bool		ii_WithoutOverlaps;
	int			ii_ParallelWorkers;
	Oid			ii_Am;
	void	   *ii_AmCache;
	MemoryContext ii_Context;
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
 *		and tuple projections.  For example, if an expression refers
 *		to an attribute in the current inner tuple then we need to know
 *		what the current inner tuple is and so we look at the expression
 *		context.
 *
 *	There are two memory contexts associated with an ExprContext:
 *	* ecxt_per_query_memory is a query-lifespan context, typically the same
 *	  context the ExprContext node itself is allocated in.  This context
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
#define FIELDNO_EXPRCONTEXT_SCANTUPLE 1
	TupleTableSlot *ecxt_scantuple;
#define FIELDNO_EXPRCONTEXT_INNERTUPLE 2
	TupleTableSlot *ecxt_innertuple;
#define FIELDNO_EXPRCONTEXT_OUTERTUPLE 3
	TupleTableSlot *ecxt_outertuple;

	/* Memory contexts for expression evaluation --- see notes above */
	MemoryContext ecxt_per_query_memory;
	MemoryContext ecxt_per_tuple_memory;

	/* Values to substitute for Param nodes in expression */
	ParamExecData *ecxt_param_exec_vals;	/* for PARAM_EXEC params */
	ParamListInfo ecxt_param_list_info; /* for other param types */

	/*
	 * Values to substitute for Aggref nodes in the expressions of an Agg
	 * node, or for WindowFunc nodes within a WindowAgg node.
	 */
#define FIELDNO_EXPRCONTEXT_AGGVALUES 8
	Datum	   *ecxt_aggvalues; /* precomputed values for aggs/windowfuncs */
#define FIELDNO_EXPRCONTEXT_AGGNULLS 9
	bool	   *ecxt_aggnulls;	/* null flags for aggs/windowfuncs */

	/* Value to substitute for CaseTestExpr nodes in expression */
#define FIELDNO_EXPRCONTEXT_CASEDATUM 10
	Datum		caseValue_datum;
#define FIELDNO_EXPRCONTEXT_CASENULL 11
	bool		caseValue_isNull;

	/* Value to substitute for CoerceToDomainValue nodes in expression */
#define FIELDNO_EXPRCONTEXT_DOMAINDATUM 12
	Datum		domainValue_datum;
#define FIELDNO_EXPRCONTEXT_DOMAINNULL 13
	bool		domainValue_isNull;

	/* Tuples that OLD/NEW Var nodes in RETURNING may refer to */
#define FIELDNO_EXPRCONTEXT_OLDTUPLE 14
	TupleTableSlot *ecxt_oldtuple;
#define FIELDNO_EXPRCONTEXT_NEWTUPLE 15
	TupleTableSlot *ecxt_newtuple;

	/* Link to containing EState (NULL if a standalone ExprContext) */
	struct EState *ecxt_estate;

	/* Functions to call back when ExprContext is shut down or rescanned */
	ExprContext_CB *ecxt_callbacks;
} ExprContext;

/*
 * Set-result status used when evaluating functions potentially returning a
 * set.
 */
typedef enum
{
	ExprSingleResult,			/* expression does not return a set */
	ExprMultipleResult,			/* this result is an element of a set */
	ExprEndResult,				/* there are no more elements in the set */
} ExprDoneCond;

/*
 * Return modes for functions returning sets.  Note values must be chosen
 * as separate bits so that a bitmask can be formed to indicate supported
 * modes.  SFRM_Materialize_Random and SFRM_Materialize_Preferred are
 * auxiliary flags about SFRM_Materialize mode, rather than separate modes.
 */
typedef enum
{
	SFRM_ValuePerCall = 0x01,	/* one value returned per call */
	SFRM_Materialize = 0x02,	/* result set instantiated in Tuplestore */
	SFRM_Materialize_Random = 0x04, /* Tuplestore needs randomAccess */
	SFRM_Materialize_Preferred = 0x08,	/* caller prefers Tuplestore */
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
 *		The target tuple slot is kept in ProjectionInfo->pi_state.resultslot.
 *		ExecProject() evaluates the tlist, forms a tuple, and stores it
 *		in the given slot.  Note that the result will be a "virtual" tuple
 *		unless ExecMaterializeSlot() is then called to force it to be
 *		converted to a physical tuple.  The slot must have a tupledesc
 *		that matches the output of the tlist!
 * ----------------
 */
typedef struct ProjectionInfo
{
	NodeTag		type;
	/* instructions to evaluate projection */
	ExprState	pi_state;
	/* expression context in which to evaluate expression */
	ExprContext *pi_exprContext;
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
 *	  the tuple to be updated.  This is needed to do the update, but we
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
 * ----------------
 */
typedef struct JunkFilter
{
	NodeTag		type;
	List	   *jf_targetList;
	TupleDesc	jf_cleanTupType;
	AttrNumber *jf_cleanMap;
	TupleTableSlot *jf_resultSlot;
} JunkFilter;

/*
 * OnConflictSetState
 *
 * Executor state of an ON CONFLICT DO UPDATE operation.
 */
typedef struct OnConflictSetState
{
	NodeTag		type;

	TupleTableSlot *oc_Existing;	/* slot to store existing target tuple in */
	TupleTableSlot *oc_ProjSlot;	/* CONFLICT ... SET ... projection target */
	ProjectionInfo *oc_ProjInfo;	/* for ON CONFLICT DO UPDATE SET */
	ExprState  *oc_WhereClause; /* state for the WHERE clause */
} OnConflictSetState;

/* ----------------
 *	 MergeActionState information
 *
 *	Executor state for a MERGE action.
 * ----------------
 */
typedef struct MergeActionState
{
	NodeTag		type;

	MergeAction *mas_action;	/* associated MergeAction node */
	ProjectionInfo *mas_proj;	/* projection of the action's targetlist for
								 * this rel */
	ExprState  *mas_whenqual;	/* WHEN [NOT] MATCHED AND conditions */
} MergeActionState;

/*
 * ResultRelInfo
 *
 * Whenever we update an existing relation, we have to update indexes on the
 * relation, and perhaps also fire triggers.  ResultRelInfo holds all the
 * information needed about a result relation, including indexes.
 *
 * Normally, a ResultRelInfo refers to a table that is in the query's range
 * table; then ri_RangeTableIndex is the RT index and ri_RelationDesc is
 * just a copy of the relevant es_relations[] entry.  However, in some
 * situations we create ResultRelInfos for relations that are not in the
 * range table, namely for targets of tuple routing in a partitioned table,
 * and when firing triggers in tables other than the target tables (See
 * ExecGetTriggerResultRel).  In these situations, ri_RangeTableIndex is 0
 * and ri_RelationDesc is a separately-opened relcache pointer that needs to
 * be separately closed.
 */
typedef struct ResultRelInfo
{
	NodeTag		type;

	/* result relation's range table index, or 0 if not in range table */
	Index		ri_RangeTableIndex;

	/* relation descriptor for result relation */
	Relation	ri_RelationDesc;

	/* # of indices existing on result relation */
	int			ri_NumIndices;

	/* array of relation descriptors for indices */
	RelationPtr ri_IndexRelationDescs;

	/* array of key/attr info for indices */
	IndexInfo **ri_IndexRelationInfo;

	/*
	 * For UPDATE/DELETE/MERGE result relations, the attribute number of the
	 * row identity junk attribute in the source plan's output tuples
	 */
	AttrNumber	ri_RowIdAttNo;

	/* For UPDATE, attnums of generated columns to be computed */
	Bitmapset  *ri_extraUpdatedCols;
	/* true if the above has been computed */
	bool		ri_extraUpdatedCols_valid;

	/* Projection to generate new tuple in an INSERT/UPDATE */
	ProjectionInfo *ri_projectNew;
	/* Slot to hold that tuple */
	TupleTableSlot *ri_newTupleSlot;
	/* Slot to hold the old tuple being updated */
	TupleTableSlot *ri_oldTupleSlot;
	/* Have the projection and the slots above been initialized? */
	bool		ri_projectNewInfoValid;

	/* updates do LockTuple() before oldtup read; see README.tuplock */
	bool		ri_needLockTagTuple;

	/* triggers to be fired, if any */
	TriggerDesc *ri_TrigDesc;

	/* cached lookup info for trigger functions */
	FmgrInfo   *ri_TrigFunctions;

	/* array of trigger WHEN expr states */
	ExprState **ri_TrigWhenExprs;

	/* optional runtime measurements for triggers */
	Instrumentation *ri_TrigInstrument;

	/* On-demand created slots for triggers / returning processing */
	TupleTableSlot *ri_ReturningSlot;	/* for trigger output tuples */
	TupleTableSlot *ri_TrigOldSlot; /* for a trigger's old tuple */
	TupleTableSlot *ri_TrigNewSlot; /* for a trigger's new tuple */
	TupleTableSlot *ri_AllNullSlot; /* for RETURNING OLD/NEW */

	/* FDW callback functions, if foreign table */
	struct FdwRoutine *ri_FdwRoutine;

	/* available to save private state of FDW */
	void	   *ri_FdwState;

	/* true when modifying foreign table directly */
	bool		ri_usesFdwDirectModify;

	/* batch insert stuff */
	int			ri_NumSlots;	/* number of slots in the array */
	int			ri_NumSlotsInitialized; /* number of initialized slots */
	int			ri_BatchSize;	/* max slots inserted in a single batch */
	TupleTableSlot **ri_Slots;	/* input tuples for batch insert */
	TupleTableSlot **ri_PlanSlots;

	/* list of WithCheckOption's to be checked */
	List	   *ri_WithCheckOptions;

	/* list of WithCheckOption expr states */
	List	   *ri_WithCheckOptionExprs;

	/* array of expr states for checking check constraints */
	ExprState **ri_CheckConstraintExprs;

	/*
	 * array of expr states for checking not-null constraints on virtual
	 * generated columns
	 */
	ExprState **ri_GenVirtualNotNullConstraintExprs;

	/*
	 * Arrays of stored generated columns ExprStates for INSERT/UPDATE/MERGE.
	 */
	ExprState **ri_GeneratedExprsI;
	ExprState **ri_GeneratedExprsU;

	/* number of stored generated columns we need to compute */
	int			ri_NumGeneratedNeededI;
	int			ri_NumGeneratedNeededU;

	/* list of RETURNING expressions */
	List	   *ri_returningList;

	/* for computing a RETURNING list */
	ProjectionInfo *ri_projectReturning;

	/* list of arbiter indexes to use to check conflicts */
	List	   *ri_onConflictArbiterIndexes;

	/* ON CONFLICT evaluation state */
	OnConflictSetState *ri_onConflict;

	/* for MERGE, lists of MergeActionState (one per MergeMatchKind) */
	List	   *ri_MergeActions[NUM_MERGE_MATCH_KINDS];

	/* for MERGE, expr state for checking the join condition */
	ExprState  *ri_MergeJoinCondition;

	/* partition check expression state (NULL if not set up yet) */
	ExprState  *ri_PartitionCheckExpr;

	/*
	 * Map to convert child result relation tuples to the format of the table
	 * actually mentioned in the query (called "root").  Computed only if
	 * needed.  A NULL map value indicates that no conversion is needed, so we
	 * must have a separate flag to show if the map has been computed.
	 */
	TupleConversionMap *ri_ChildToRootMap;
	bool		ri_ChildToRootMapValid;

	/*
	 * As above, but in the other direction.
	 */
	TupleConversionMap *ri_RootToChildMap;
	bool		ri_RootToChildMapValid;

	/*
	 * Information needed by tuple routing target relations
	 *
	 * RootResultRelInfo gives the target relation mentioned in the query, if
	 * it's a partitioned table. It is not set if the target relation
	 * mentioned in the query is an inherited table, nor when tuple routing is
	 * not needed.
	 *
	 * PartitionTupleSlot is non-NULL if RootToChild conversion is needed and
	 * the relation is a partition.
	 */
	struct ResultRelInfo *ri_RootResultRelInfo;
	TupleTableSlot *ri_PartitionTupleSlot;

	/* for use by copyfrom.c when performing multi-inserts */
	struct CopyMultiInsertBuffer *ri_CopyMultiInsertBuffer;

	/*
	 * Used when a leaf partition is involved in a cross-partition update of
	 * one of its ancestors; see ExecCrossPartitionUpdateForeignKey().
	 */
	List	   *ri_ancestorResultRels;
} ResultRelInfo;

/* ----------------
 *	  AsyncRequest
 *
 * State for an asynchronous tuple request.
 * ----------------
 */
typedef struct AsyncRequest
{
	struct PlanState *requestor;	/* Node that wants a tuple */
	struct PlanState *requestee;	/* Node from which a tuple is wanted */
	int			request_index;	/* Scratch space for requestor */
	bool		callback_pending;	/* Callback is needed */
	bool		request_complete;	/* Request complete, result valid */
	TupleTableSlot *result;		/* Result (NULL or an empty slot if no more
								 * tuples) */
} AsyncRequest;

/* ----------------
 *	  EState information
 *
 * Working state for an Executor invocation
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
	Index		es_range_table_size;	/* size of the range table arrays */
	Relation   *es_relations;	/* Array of per-range-table-entry Relation
								 * pointers, or NULL if not yet opened */
	struct ExecRowMark **es_rowmarks;	/* Array of per-range-table-entry
										 * ExecRowMarks, or NULL if none */
	List	   *es_rteperminfos;	/* List of RTEPermissionInfo */
	PlannedStmt *es_plannedstmt;	/* link to top of plan tree */
	CachedPlan *es_cachedplan;	/* CachedPlan providing the plan tree */
	List	   *es_part_prune_infos;	/* List of PartitionPruneInfo */
	List	   *es_part_prune_states;	/* List of PartitionPruneState */
	List	   *es_part_prune_results;	/* List of Bitmapset */
	Bitmapset  *es_unpruned_relids; /* PlannedStmt.unprunableRelids + RT
									 * indexes of leaf partitions that survive
									 * initial pruning; see
									 * ExecDoInitialPruning() */
	const char *es_sourceText;	/* Source text from QueryDesc */

	JunkFilter *es_junkFilter;	/* top-level junk filter, if any */

	/* If query can insert/delete tuples, the command ID to mark them with */
	CommandId	es_output_cid;

	/* Info about target table(s) for insert/update/delete queries: */
	ResultRelInfo **es_result_relations;	/* Array of per-range-table-entry
											 * ResultRelInfo pointers, or NULL
											 * if not a target table */
	List	   *es_opened_result_relations; /* List of non-NULL entries in
											 * es_result_relations in no
											 * specific order */

	PartitionDirectory es_partition_directory;	/* for PartitionDesc lookup */

	/*
	 * The following list contains ResultRelInfos created by the tuple routing
	 * code for partitions that aren't found in the es_result_relations array.
	 */
	List	   *es_tuple_routing_result_relations;

	/* Stuff used for firing triggers: */
	List	   *es_trig_target_relations;	/* trigger-only ResultRelInfos */

	/* Parameter info: */
	ParamListInfo es_param_list_info;	/* values of external params */
	ParamExecData *es_param_exec_vals;	/* values of internal params */

	QueryEnvironment *es_queryEnv;	/* query environment */

	/* Other working state: */
	MemoryContext es_query_cxt; /* per-query context in which EState lives */

	List	   *es_tupleTable;	/* List of TupleTableSlots */

	uint64		es_processed;	/* # of tuples processed during one
								 * ExecutorRun() call. */
	uint64		es_total_processed; /* total # of tuples aggregated across all
									 * ExecutorRun() calls. */

	int			es_top_eflags;	/* eflags passed to ExecutorStart */
	int			es_instrument;	/* OR of InstrumentOption flags */
	bool		es_finished;	/* true when ExecutorFinish is done */
	bool		es_aborted;		/* true when execution was aborted */

	List	   *es_exprcontexts;	/* List of ExprContexts within EState */

	List	   *es_subplanstates;	/* List of PlanState for SubPlans */

	List	   *es_auxmodifytables; /* List of secondary ModifyTableStates */

	/*
	 * this ExprContext is for per-output-tuple operations, such as constraint
	 * checks and index-value computations.  It will be reset for each output
	 * tuple.  Note that it will be created only if needed.
	 */
	ExprContext *es_per_tuple_exprcontext;

	/*
	 * If not NULL, this is an EPQState's EState. This is a field in EState
	 * both to allow EvalPlanQual aware executor nodes to detect that they
	 * need to perform EPQ related work, and to provide necessary information
	 * to do so.
	 */
	struct EPQState *es_epq_active;

	bool		es_use_parallel_mode;	/* can we use parallel workers? */

	int			es_parallel_workers_to_launch;	/* number of workers to
												 * launch. */
	int			es_parallel_workers_launched;	/* number of workers actually
												 * launched. */

	/* The per-query shared memory area to use for parallel execution. */
	struct dsa_area *es_query_dsa;

	/*
	 * JIT information. es_jit_flags indicates whether JIT should be performed
	 * and with which options.  es_jit is created on-demand when JITing is
	 * performed.
	 *
	 * es_jit_worker_instr is the combined, on demand allocated,
	 * instrumentation from all workers. The leader's instrumentation is kept
	 * separate, and is combined on demand by ExplainPrintJITSummary().
	 */
	int			es_jit_flags;
	struct JitContext *es_jit;
	struct JitInstrumentation *es_jit_worker_instr;

	/*
	 * Lists of ResultRelInfos for foreign tables on which batch-inserts are
	 * to be executed and owning ModifyTableStates, stored in the same order.
	 */
	List	   *es_insert_pending_result_relations;
	List	   *es_insert_pending_modifytables;
} EState;


/*
 * ExecRowMark -
 *	   runtime representation of FOR [KEY] UPDATE/SHARE clauses
 *
 * When doing UPDATE/DELETE/MERGE/SELECT FOR [KEY] UPDATE/SHARE, we will have
 * an ExecRowMark for each non-target relation in the query (except inheritance
 * parent RTEs, which can be ignored at runtime).  Virtual relations such as
 * subqueries-in-FROM will have an ExecRowMark with relation == NULL.  See
 * PlanRowMark for details about most of the fields.  In addition to fields
 * directly derived from PlanRowMark, we store an activity flag (to denote
 * inactive children of inheritance trees), curCtid, which is used by the
 * WHERE CURRENT OF code, and ermExtra, which is available for use by the plan
 * node that sources the relation (e.g., for a foreign table the FDW can use
 * ermExtra to hold information).
 *
 * EState->es_rowmarks is an array of these structs, indexed by RT index,
 * with NULLs for irrelevant RT indexes.  es_rowmarks itself is NULL if
 * there are no rowmarks.
 */
typedef struct ExecRowMark
{
	Relation	relation;		/* opened and suitably locked relation */
	Oid			relid;			/* its OID (or InvalidOid, if subquery) */
	Index		rti;			/* its range table index */
	Index		prti;			/* parent range table index, if child */
	Index		rowmarkId;		/* unique identifier for resjunk columns */
	RowMarkType markType;		/* see enum in nodes/plannodes.h */
	LockClauseStrength strength;	/* LockingClause's strength, or LCS_NONE */
	LockWaitPolicy waitPolicy;	/* NOWAIT and SKIP LOCKED */
	bool		ermActive;		/* is this mark relevant for current tuple? */
	ItemPointerData curCtid;	/* ctid of currently locked tuple, if any */
	void	   *ermExtra;		/* available for use by relation source node */
} ExecRowMark;

/*
 * ExecAuxRowMark -
 *	   additional runtime representation of FOR [KEY] UPDATE/SHARE clauses
 *
 * Each LockRows and ModifyTable node keeps a list of the rowmarks it needs to
 * deal with.  In addition to a pointer to the related entry in es_rowmarks,
 * this struct carries the column number(s) of the resjunk columns associated
 * with the rowmark (see comments for PlanRowMark for more detail).
 */
typedef struct ExecAuxRowMark
{
	ExecRowMark *rowmark;		/* related entry in es_rowmarks */
	AttrNumber	ctidAttNo;		/* resno of ctid junk attribute, if any */
	AttrNumber	toidAttNo;		/* resno of tableoid junk attribute, if any */
	AttrNumber	wholeAttNo;		/* resno of whole-row junk attribute, if any */
} ExecAuxRowMark;


/* ----------------------------------------------------------------
 *				 Tuple Hash Tables
 *
 * All-in-memory tuple hash tables are used for a number of purposes.
 *
 * Note: tab_hash_expr is for hashing the key datatype(s) stored in the table,
 * and tab_eq_func is a non-cross-type ExprState for equality checks on those
 * types.  Normally these are the only ExprStates used, but
 * FindTupleHashEntry() supports searching a hashtable using cross-data-type
 * hashing.  For that, the caller must supply an ExprState to hash the LHS
 * datatype as well as the cross-type equality ExprState to use.  in_hash_expr
 * and cur_eq_func are set to point to the caller's hash and equality
 * ExprStates while doing such a search.  During LookupTupleHashEntry(), they
 * point to tab_hash_expr and tab_eq_func respectively.
 * ----------------------------------------------------------------
 */
typedef struct TupleHashEntryData *TupleHashEntry;
typedef struct TupleHashTableData *TupleHashTable;

typedef struct TupleHashEntryData
{
	MinimalTuple firstTuple;	/* copy of first tuple in this group */
	uint32		status;			/* hash status */
	uint32		hash;			/* hash value (cached) */
} TupleHashEntryData;

/* define parameters necessary to generate the tuple hash table interface */
#define SH_PREFIX tuplehash
#define SH_ELEMENT_TYPE TupleHashEntryData
#define SH_KEY_TYPE MinimalTuple
#define SH_SCOPE extern
#define SH_DECLARE
#include "lib/simplehash.h"

typedef struct TupleHashTableData
{
	tuplehash_hash *hashtab;	/* underlying hash table */
	int			numCols;		/* number of columns in lookup key */
	AttrNumber *keyColIdx;		/* attr numbers of key columns */
	ExprState  *tab_hash_expr;	/* ExprState for hashing table datatype(s) */
	ExprState  *tab_eq_func;	/* comparator for table datatype(s) */
	Oid		   *tab_collations; /* collations for hash and comparison */
	MemoryContext tablecxt;		/* memory context containing table */
	MemoryContext tempcxt;		/* context for function evaluations */
	Size		additionalsize; /* size of additional data */
	TupleTableSlot *tableslot;	/* slot for referencing table entries */
	/* The following fields are set transiently for each table search: */
	TupleTableSlot *inputslot;	/* current input tuple's slot */
	ExprState  *in_hash_expr;	/* ExprState for hashing input datatype(s) */
	ExprState  *cur_eq_func;	/* comparator for input vs. table */
	ExprContext *exprcontext;	/* expression context */
}			TupleHashTableData;

typedef tuplehash_iterator TupleHashIterator;

/*
 * Use InitTupleHashIterator/TermTupleHashIterator for a read/write scan.
 * Use ResetTupleHashIterator if the table can be frozen (in this case no
 * explicit scan termination is needed).
 */
#define InitTupleHashIterator(htable, iter) \
	tuplehash_start_iterate(htable->hashtab, iter)
#define TermTupleHashIterator(iter) \
	((void) 0)
#define ResetTupleHashIterator(htable, iter) \
	InitTupleHashIterator(htable, iter)
#define ScanTupleHashTable(htable, iter) \
	tuplehash_iterate(htable->hashtab, iter)


/* ----------------------------------------------------------------
 *				 Expression State Nodes
 *
 * Formerly, there was a separate executor expression state node corresponding
 * to each node in a planned expression tree.  That's no longer the case; for
 * common expression node types, all the execution info is embedded into
 * step(s) in a single ExprState node.  But we still have a few executor state
 * node types for selected expression node types, mostly those in which info
 * has to be shared with other parts of the execution state tree.
 * ----------------------------------------------------------------
 */

/* ----------------
 *		WindowFuncExprState node
 * ----------------
 */
typedef struct WindowFuncExprState
{
	NodeTag		type;
	WindowFunc *wfunc;			/* expression plan node */
	List	   *args;			/* ExprStates for argument expressions */
	ExprState  *aggfilter;		/* FILTER expression */
	int			wfuncno;		/* ID number for wfunc within its plan node */
} WindowFuncExprState;


/* ----------------
 *		SetExprState node
 *
 * State for evaluating a potentially set-returning expression (like FuncExpr
 * or OpExpr).  In some cases, like some of the expressions in ROWS FROM(...)
 * the expression might not be a SRF, but nonetheless it uses the same
 * machinery as SRFs; it will be treated as a SRF returning a single row.
 * ----------------
 */
typedef struct SetExprState
{
	NodeTag		type;
	Expr	   *expr;			/* expression plan node */
	List	   *args;			/* ExprStates for argument expressions */

	/*
	 * In ROWS FROM, functions can be inlined, removing the FuncExpr normally
	 * inside.  In such a case this is the compiled expression (which cannot
	 * return a set), which'll be evaluated using regular ExecEvalExpr().
	 */
	ExprState  *elidedFuncState;

	/*
	 * Function manager's lookup info for the target function.  If func.fn_oid
	 * is InvalidOid, we haven't initialized it yet (nor any of the following
	 * fields, except funcReturnsSet).
	 */
	FmgrInfo	func;

	/*
	 * For a set-returning function (SRF) that returns a tuplestore, we keep
	 * the tuplestore here and dole out the result rows one at a time. The
	 * slot holds the row currently being returned.
	 */
	Tuplestorestate *funcResultStore;
	TupleTableSlot *funcResultSlot;

	/*
	 * In some cases we need to compute a tuple descriptor for the function's
	 * output.  If so, it's stored here.
	 */
	TupleDesc	funcResultDesc;
	bool		funcReturnsTuple;	/* valid when funcResultDesc isn't NULL */

	/*
	 * Remember whether the function is declared to return a set.  This is set
	 * by ExecInitExpr, and is valid even before the FmgrInfo is set up.
	 */
	bool		funcReturnsSet;

	/*
	 * setArgsValid is true when we are evaluating a set-returning function
	 * that uses value-per-call mode and we are in the middle of a call
	 * series; we want to pass the same argument values to the function again
	 * (and again, until it returns ExprEndResult).  This indicates that
	 * fcinfo_data already contains valid argument data.
	 */
	bool		setArgsValid;

	/*
	 * Flag to remember whether we have registered a shutdown callback for
	 * this SetExprState.  We do so only if funcResultStore or setArgsValid
	 * has been set at least once (since all the callback is for is to release
	 * the tuplestore or clear setArgsValid).
	 */
	bool		shutdown_reg;	/* a shutdown callback is registered */

	/*
	 * Call parameter structure for the function.  This has been initialized
	 * (by InitFunctionCallInfoData) if func.fn_oid is valid.  It also saves
	 * argument values between calls, when setArgsValid is true.
	 */
	FunctionCallInfo fcinfo;
} SetExprState;

/* ----------------
 *		SubPlanState node
 * ----------------
 */
typedef struct SubPlanState
{
	NodeTag		type;
	SubPlan    *subplan;		/* expression plan node */
	struct PlanState *planstate;	/* subselect plan's state tree */
	struct PlanState *parent;	/* parent plan node's state tree */
	ExprState  *testexpr;		/* state of combining expression */
	HeapTuple	curTuple;		/* copy of most recent tuple from subplan */
	Datum		curArray;		/* most recent array from ARRAY() subplan */
	/* these are used when hashing the subselect's output: */
	TupleDesc	descRight;		/* subselect desc after projection */
	ProjectionInfo *projLeft;	/* for projecting lefthand exprs */
	ProjectionInfo *projRight;	/* for projecting subselect output */
	TupleHashTable hashtable;	/* hash table for no-nulls subselect rows */
	TupleHashTable hashnulls;	/* hash table for rows with null(s) */
	bool		havehashrows;	/* true if hashtable is not empty */
	bool		havenullrows;	/* true if hashnulls is not empty */
	MemoryContext hashtablecxt; /* memory context containing hash tables */
	MemoryContext hashtempcxt;	/* temp memory context for hash tables */
	ExprContext *innerecontext; /* econtext for computing inner tuples */
	int			numCols;		/* number of columns being hashed */
	/* each of the remaining fields is an array of length numCols: */
	AttrNumber *keyColIdx;		/* control data for hash tables */
	Oid		   *tab_eq_funcoids;	/* equality func oids for table
									 * datatype(s) */
	Oid		   *tab_collations; /* collations for hash and comparison */
	FmgrInfo   *tab_hash_funcs; /* hash functions for table datatype(s) */
	ExprState  *lhs_hash_expr;	/* hash expr for lefthand datatype(s) */
	FmgrInfo   *cur_eq_funcs;	/* equality functions for LHS vs. table */
	ExprState  *cur_eq_comp;	/* equality comparator for LHS vs. table */
} SubPlanState;

/*
 * DomainConstraintState - one item to check during CoerceToDomain
 *
 * Note: we consider this to be part of an ExprState tree, so we give it
 * a name following the xxxState convention.  But there's no directly
 * associated plan-tree node.
 */
typedef enum DomainConstraintType
{
	DOM_CONSTRAINT_NOTNULL,
	DOM_CONSTRAINT_CHECK,
} DomainConstraintType;

typedef struct DomainConstraintState
{
	NodeTag		type;
	DomainConstraintType constrainttype;	/* constraint type */
	char	   *name;			/* name of constraint (for error msgs) */
	Expr	   *check_expr;		/* for CHECK, a boolean expression */
	ExprState  *check_exprstate;	/* check_expr's eval state, or NULL */
} DomainConstraintState;

/*
 * State for JsonExpr evaluation, too big to inline.
 *
 * This contains the information going into and coming out of the
 * EEOP_JSONEXPR_PATH eval step.
 */
typedef struct JsonExprState
{
	/* original expression node */
	JsonExpr   *jsexpr;

	/* value/isnull for formatted_expr */
	NullableDatum formatted_expr;

	/* value/isnull for pathspec */
	NullableDatum pathspec;

	/* JsonPathVariable entries for passing_values */
	List	   *args;

	/*
	 * Output variables that drive the EEOP_JUMP_IF_NOT_TRUE steps that are
	 * added for ON ERROR and ON EMPTY expressions, if any.
	 *
	 * Reset for each evaluation of EEOP_JSONEXPR_PATH.
	 */

	/* Set to true if jsonpath evaluation cause an error.  */
	NullableDatum error;

	/* Set to true if the jsonpath evaluation returned 0 items. */
	NullableDatum empty;

	/*
	 * Addresses of steps that implement the non-ERROR variant of ON EMPTY and
	 * ON ERROR behaviors, respectively.
	 */
	int			jump_empty;
	int			jump_error;

	/*
	 * Address of the step to coerce the result value of jsonpath evaluation
	 * to the RETURNING type.  -1 if no coercion if JsonExpr.use_io_coercion
	 * is true.
	 */
	int			jump_eval_coercion;

	/*
	 * Address to jump to when skipping all the steps after performing
	 * ExecEvalJsonExprPath() so as to return whatever the JsonPath* function
	 * returned as is, that is, in the cases where there's no error and no
	 * coercion is necessary.
	 */
	int			jump_end;

	/*
	 * RETURNING type input function invocation info when
	 * JsonExpr.use_io_coercion is true.
	 */
	FunctionCallInfo input_fcinfo;

	/*
	 * For error-safe evaluation of coercions.  When the ON ERROR behavior is
	 * not ERROR, a pointer to this is passed to ExecInitExprRec() when
	 * initializing the coercion expressions or to ExecInitJsonCoercion().
	 *
	 * Reset for each evaluation of EEOP_JSONEXPR_PATH.
	 */
	ErrorSaveContext escontext;
} JsonExprState;


/* ----------------------------------------------------------------
 *				 Executor State Trees
 *
 * An executing query has a PlanState tree paralleling the Plan tree
 * that describes the plan.
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 ExecProcNodeMtd
 *
 * This is the method called by ExecProcNode to return the next tuple
 * from an executor node.  It returns NULL, or an empty TupleTableSlot,
 * if no more tuples are available.
 * ----------------
 */
typedef TupleTableSlot *(*ExecProcNodeMtd) (struct PlanState *pstate);

/* ----------------
 *		PlanState node
 *
 * We never actually instantiate any PlanState nodes; this is just the common
 * abstract superclass for all PlanState-type nodes.
 * ----------------
 */
typedef struct PlanState
{
	pg_node_attr(abstract)

	NodeTag		type;

	Plan	   *plan;			/* associated Plan node */

	EState	   *state;			/* at execution time, states of individual
								 * nodes point to one EState for the whole
								 * top-level plan */

	ExecProcNodeMtd ExecProcNode;	/* function to return next tuple */
	ExecProcNodeMtd ExecProcNodeReal;	/* actual function, if above is a
										 * wrapper */

	Instrumentation *instrument;	/* Optional runtime stats for this node */
	WorkerInstrumentation *worker_instrument;	/* per-worker instrumentation */

	/* Per-worker JIT instrumentation */
	struct SharedJitInstrumentation *worker_jit_instrument;

	/*
	 * Common structural data for all Plan types.  These links to subsidiary
	 * state trees parallel links in the associated plan tree (except for the
	 * subPlan list, which does not exist in the plan tree).
	 */
	ExprState  *qual;			/* boolean qual condition */
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
	TupleDesc	ps_ResultTupleDesc; /* node's return type */
	TupleTableSlot *ps_ResultTupleSlot; /* slot for my result tuples */
	ExprContext *ps_ExprContext;	/* node's expression-evaluation context */
	ProjectionInfo *ps_ProjInfo;	/* info for doing tuple projection */

	bool		async_capable;	/* true if node is async-capable */

	/*
	 * Scanslot's descriptor if known. This is a bit of a hack, but otherwise
	 * it's hard for expression compilation to optimize based on the
	 * descriptor, without encoding knowledge about all executor nodes.
	 */
	TupleDesc	scandesc;

	/*
	 * Define the slot types for inner, outer and scanslots for expression
	 * contexts with this state as a parent.  If *opsset is set, then
	 * *opsfixed indicates whether *ops is guaranteed to be the type of slot
	 * used. That means that every slot in the corresponding
	 * ExprContext.ecxt_*tuple will point to a slot of that type, while
	 * evaluating the expression.  If *opsfixed is false, but *ops is set,
	 * that indicates the most likely type of slot.
	 *
	 * The scan* fields are set by ExecInitScanTupleSlot(). If that's not
	 * called, nodes can initialize the fields themselves.
	 *
	 * If outer/inneropsset is false, the information is inferred on-demand
	 * using ExecGetResultSlotOps() on ->righttree/lefttree, using the
	 * corresponding node's resultops* fields.
	 *
	 * The result* fields are automatically set when ExecInitResultSlot is
	 * used (be it directly or when the slot is created by
	 * ExecAssignScanProjectionInfo() /
	 * ExecConditionalAssignProjectionInfo()).  If no projection is necessary
	 * ExecConditionalAssignProjectionInfo() defaults those fields to the scan
	 * operations.
	 */
	const TupleTableSlotOps *scanops;
	const TupleTableSlotOps *outerops;
	const TupleTableSlotOps *innerops;
	const TupleTableSlotOps *resultops;
	bool		scanopsfixed;
	bool		outeropsfixed;
	bool		inneropsfixed;
	bool		resultopsfixed;
	bool		scanopsset;
	bool		outeropsset;
	bool		inneropsset;
	bool		resultopsset;
} PlanState;

/* ----------------
 *	these are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * ----------------
 */
#define innerPlanState(node)		(((PlanState *)(node))->righttree)
#define outerPlanState(node)		(((PlanState *)(node))->lefttree)

/* Macros for inline access to certain instrumentation counters */
#define InstrCountTuples2(node, delta) \
	do { \
		if (((PlanState *)(node))->instrument) \
			((PlanState *)(node))->instrument->ntuples2 += (delta); \
	} while (0)
#define InstrCountFiltered1(node, delta) \
	do { \
		if (((PlanState *)(node))->instrument) \
			((PlanState *)(node))->instrument->nfiltered1 += (delta); \
	} while(0)
#define InstrCountFiltered2(node, delta) \
	do { \
		if (((PlanState *)(node))->instrument) \
			((PlanState *)(node))->instrument->nfiltered2 += (delta); \
	} while(0)

/*
 * EPQState is state for executing an EvalPlanQual recheck on a candidate
 * tuples e.g. in ModifyTable or LockRows.
 *
 * To execute EPQ a separate EState is created (stored in ->recheckestate),
 * which shares some resources, like the rangetable, with the main query's
 * EState (stored in ->parentestate). The (sub-)tree of the plan that needs to
 * be rechecked (in ->plan), is separately initialized (into
 * ->recheckplanstate), but shares plan nodes with the corresponding nodes in
 * the main query. The scan nodes in that separate executor tree are changed
 * to return only the current tuple of interest for the respective
 * table. Those tuples are either provided by the caller (using
 * EvalPlanQualSlot), and/or found using the rowmark mechanism (non-locking
 * rowmarks by the EPQ machinery itself, locking ones by the caller).
 *
 * While the plan to be checked may be changed using EvalPlanQualSetPlan(),
 * all such plans need to share the same EState.
 */
typedef struct EPQState
{
	/* These are initialized by EvalPlanQualInit() and do not change later: */
	EState	   *parentestate;	/* main query's EState */
	int			epqParam;		/* ID of Param to force scan node re-eval */
	List	   *resultRelations;	/* integer list of RT indexes, or NIL */

	/*
	 * relsubs_slot[scanrelid - 1] holds the EPQ test tuple to be returned by
	 * the scan node for the scanrelid'th RT index, in place of performing an
	 * actual table scan.  Callers should use EvalPlanQualSlot() to fetch
	 * these slots.
	 */
	List	   *tuple_table;	/* tuple table for relsubs_slot */
	TupleTableSlot **relsubs_slot;

	/*
	 * Initialized by EvalPlanQualInit(), may be changed later with
	 * EvalPlanQualSetPlan():
	 */

	Plan	   *plan;			/* plan tree to be executed */
	List	   *arowMarks;		/* ExecAuxRowMarks (non-locking only) */


	/*
	 * The original output tuple to be rechecked.  Set by
	 * EvalPlanQualSetSlot(), before EvalPlanQualNext() or EvalPlanQual() may
	 * be called.
	 */
	TupleTableSlot *origslot;


	/* Initialized or reset by EvalPlanQualBegin(): */

	EState	   *recheckestate;	/* EState for EPQ execution, see above */

	/*
	 * Rowmarks that can be fetched on-demand using
	 * EvalPlanQualFetchRowMark(), indexed by scanrelid - 1. Only non-locking
	 * rowmarks.
	 */
	ExecAuxRowMark **relsubs_rowmark;

	/*
	 * relsubs_done[scanrelid - 1] is true if there is no EPQ tuple for this
	 * target relation or it has already been fetched in the current scan of
	 * this target relation within the current EvalPlanQual test.
	 */
	bool	   *relsubs_done;

	/*
	 * relsubs_blocked[scanrelid - 1] is true if there is no EPQ tuple for
	 * this target relation during the current EvalPlanQual test.  We keep
	 * these flags set for all relids listed in resultRelations, but
	 * transiently clear the one for the relation whose tuple is actually
	 * passed to EvalPlanQual().
	 */
	bool	   *relsubs_blocked;

	PlanState  *recheckplanstate;	/* EPQ specific exec nodes, for ->plan */
} EPQState;


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
 *	 ProjectSetState information
 *
 * Note: at least one of the "elems" will be a SetExprState; the rest are
 * regular ExprStates.
 * ----------------
 */
typedef struct ProjectSetState
{
	PlanState	ps;				/* its first field is NodeTag */
	Node	  **elems;			/* array of expression states */
	ExprDoneCond *elemdone;		/* array of per-SRF is-done states */
	int			nelems;			/* length of elemdone[] array */
	bool		pending_srf_tuples; /* still evaluating srfs in tlist? */
	MemoryContext argcontext;	/* context for SRF arguments */
} ProjectSetState;


/* flags for mt_merge_subcommands */
#define MERGE_INSERT	0x01
#define MERGE_UPDATE	0x02
#define MERGE_DELETE	0x04

/* ----------------
 *	 ModifyTableState information
 * ----------------
 */
typedef struct ModifyTableState
{
	PlanState	ps;				/* its first field is NodeTag */
	CmdType		operation;		/* INSERT, UPDATE, DELETE, or MERGE */
	bool		canSetTag;		/* do we set the command tag/es_processed? */
	bool		mt_done;		/* are we done? */
	int			mt_nrels;		/* number of entries in resultRelInfo[] */
	ResultRelInfo *resultRelInfo;	/* info about target relation(s) */

	/*
	 * Target relation mentioned in the original statement, used to fire
	 * statement-level triggers and as the root for tuple routing.  (This
	 * might point to one of the resultRelInfo[] entries, but it can also be a
	 * distinct struct.)
	 */
	ResultRelInfo *rootResultRelInfo;

	EPQState	mt_epqstate;	/* for evaluating EvalPlanQual rechecks */
	bool		fireBSTriggers; /* do we need to fire stmt triggers? */

	/*
	 * These fields are used for inherited UPDATE and DELETE, to track which
	 * target relation a given tuple is from.  If there are a lot of target
	 * relations, we use a hash table to translate table OIDs to
	 * resultRelInfo[] indexes; otherwise mt_resultOidHash is NULL.
	 */
	int			mt_resultOidAttno;	/* resno of "tableoid" junk attr */
	Oid			mt_lastResultOid;	/* last-seen value of tableoid */
	int			mt_lastResultIndex; /* corresponding index in resultRelInfo[] */
	HTAB	   *mt_resultOidHash;	/* optional hash table to speed lookups */

	/*
	 * Slot for storing tuples in the root partitioned table's rowtype during
	 * an UPDATE of a partitioned table.
	 */
	TupleTableSlot *mt_root_tuple_slot;

	/* Tuple-routing support info */
	struct PartitionTupleRouting *mt_partition_tuple_routing;

	/* controls transition table population for specified operation */
	struct TransitionCaptureState *mt_transition_capture;

	/* controls transition table population for INSERT...ON CONFLICT UPDATE */
	struct TransitionCaptureState *mt_oc_transition_capture;

	/* Flags showing which subcommands are present INS/UPD/DEL/DO NOTHING */
	int			mt_merge_subcommands;

	/* For MERGE, the action currently being executed */
	MergeActionState *mt_merge_action;

	/*
	 * For MERGE, if there is a pending NOT MATCHED [BY TARGET] action to be
	 * performed, this will be the last tuple read from the subplan; otherwise
	 * it will be NULL --- see the comments in ExecMerge().
	 */
	TupleTableSlot *mt_merge_pending_not_matched;

	/* tuple counters for MERGE */
	double		mt_merge_inserted;
	double		mt_merge_updated;
	double		mt_merge_deleted;

	/*
	 * Lists of valid updateColnosLists, mergeActionLists, and
	 * mergeJoinConditions.  These contain only entries for unpruned
	 * relations, filtered from the corresponding lists in ModifyTable.
	 */
	List	   *mt_updateColnosLists;
	List	   *mt_mergeActionLists;
	List	   *mt_mergeJoinConditions;
} ModifyTableState;

/* ----------------
 *	 AppendState information
 *
 *		nplans				how many plans are in the array
 *		whichplan			which synchronous plan is being executed (0 .. n-1)
 *							or a special negative value. See nodeAppend.c.
 *		prune_state			details required to allow partitions to be
 *							eliminated from the scan, or NULL if not possible.
 *		valid_subplans		for runtime pruning, valid synchronous appendplans
 *							indexes to scan.
 * ----------------
 */

struct AppendState;
typedef struct AppendState AppendState;
struct ParallelAppendState;
typedef struct ParallelAppendState ParallelAppendState;
struct PartitionPruneState;

struct AppendState
{
	PlanState	ps;				/* its first field is NodeTag */
	PlanState **appendplans;	/* array of PlanStates for my inputs */
	int			as_nplans;
	int			as_whichplan;
	bool		as_begun;		/* false means need to initialize */
	Bitmapset  *as_asyncplans;	/* asynchronous plans indexes */
	int			as_nasyncplans; /* # of asynchronous plans */
	AsyncRequest **as_asyncrequests;	/* array of AsyncRequests */
	TupleTableSlot **as_asyncresults;	/* unreturned results of async plans */
	int			as_nasyncresults;	/* # of valid entries in as_asyncresults */
	bool		as_syncdone;	/* true if all synchronous plans done in
								 * asynchronous mode, else false */
	int			as_nasyncremain;	/* # of remaining asynchronous plans */
	Bitmapset  *as_needrequest; /* asynchronous plans needing a new request */
	struct WaitEventSet *as_eventset;	/* WaitEventSet used to configure file
										 * descriptor wait events */
	int			as_first_partial_plan;	/* Index of 'appendplans' containing
										 * the first partial plan */
	ParallelAppendState *as_pstate; /* parallel coordination info */
	Size		pstate_len;		/* size of parallel coordination info */
	struct PartitionPruneState *as_prune_state;
	bool		as_valid_subplans_identified;	/* is as_valid_subplans valid? */
	Bitmapset  *as_valid_subplans;
	Bitmapset  *as_valid_asyncplans;	/* valid asynchronous plans indexes */
	bool		(*choose_next_subplan) (AppendState *);
};

/* ----------------
 *	 MergeAppendState information
 *
 *		nplans			how many plans are in the array
 *		nkeys			number of sort key columns
 *		sortkeys		sort keys in SortSupport representation
 *		slots			current output tuple of each subplan
 *		heap			heap of active tuples
 *		initialized		true if we have fetched first tuple from each subplan
 *		prune_state		details required to allow partitions to be
 *						eliminated from the scan, or NULL if not possible.
 *		valid_subplans	for runtime pruning, valid mergeplans indexes to
 *						scan.
 * ----------------
 */
typedef struct MergeAppendState
{
	PlanState	ps;				/* its first field is NodeTag */
	PlanState **mergeplans;		/* array of PlanStates for my inputs */
	int			ms_nplans;
	int			ms_nkeys;
	SortSupport ms_sortkeys;	/* array of length ms_nkeys */
	TupleTableSlot **ms_slots;	/* array of length ms_nplans */
	struct binaryheap *ms_heap; /* binary heap of slot indices */
	bool		ms_initialized; /* are subplans started? */
	struct PartitionPruneState *ms_prune_state;
	Bitmapset  *ms_valid_subplans;
} MergeAppendState;

/* ----------------
 *	 RecursiveUnionState information
 *
 *		RecursiveUnionState is used for performing a recursive union.
 *
 *		recursing			T when we're done scanning the non-recursive term
 *		intermediate_empty	T if intermediate_table is currently empty
 *		working_table		working table (to be scanned by recursive term)
 *		intermediate_table	current recursive output (next generation of WT)
 * ----------------
 */
typedef struct RecursiveUnionState
{
	PlanState	ps;				/* its first field is NodeTag */
	bool		recursing;
	bool		intermediate_empty;
	Tuplestorestate *working_table;
	Tuplestorestate *intermediate_table;
	/* Remaining fields are unused in UNION ALL case */
	Oid		   *eqfuncoids;		/* per-grouping-field equality fns */
	FmgrInfo   *hashfunctions;	/* per-grouping-field hash fns */
	MemoryContext tempContext;	/* short-term context for comparisons */
	TupleHashTable hashtable;	/* hash table for tuples already seen */
	MemoryContext tableContext; /* memory context containing hash table */
} RecursiveUnionState;

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
	struct TableScanDescData *ss_currentScanDesc;
	TupleTableSlot *ss_ScanTupleSlot;
} ScanState;

/* ----------------
 *	 SeqScanState information
 * ----------------
 */
typedef struct SeqScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	Size		pscan_len;		/* size of parallel heap scan descriptor */
} SeqScanState;

/* ----------------
 *	 SampleScanState information
 * ----------------
 */
typedef struct SampleScanState
{
	ScanState	ss;
	List	   *args;			/* expr states for TABLESAMPLE params */
	ExprState  *repeatable;		/* expr state for REPEATABLE expr */
	/* use struct pointer to avoid including tsmapi.h here */
	struct TsmRoutine *tsmroutine;	/* descriptor for tablesample method */
	void	   *tsm_state;		/* tablesample method can keep state here */
	bool		use_bulkread;	/* use bulkread buffer access strategy? */
	bool		use_pagemode;	/* use page-at-a-time visibility checking? */
	bool		begun;			/* false means need to call BeginSampleScan */
	uint32		seed;			/* random seed */
	int64		donetuples;		/* number of tuples already returned */
	bool		haveblock;		/* has a block for sampling been determined */
	bool		done;			/* exhausted all tuples? */
} SampleScanState;

/*
 * These structs store information about index quals that don't have simple
 * constant right-hand sides.  See comments for ExecIndexBuildScanKeys()
 * for discussion.
 */
typedef struct
{
	struct ScanKeyData *scan_key;	/* scankey to put value into */
	ExprState  *key_expr;		/* expr to evaluate to get value */
	bool		key_toastable;	/* is expr's result a toastable datatype? */
} IndexRuntimeKeyInfo;

typedef struct
{
	struct ScanKeyData *scan_key;	/* scankey to put value into */
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
 *		indexorderbyorig   execution state for indexorderbyorig expressions
 *		ScanKeys		   Skey structures for index quals
 *		NumScanKeys		   number of ScanKeys
 *		OrderByKeys		   Skey structures for index ordering operators
 *		NumOrderByKeys	   number of OrderByKeys
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 *		Instrument		   local index scan instrumentation
 *		SharedInfo		   parallel worker instrumentation (no leader entry)
 *
 *		ReorderQueue	   tuples that need reordering due to re-check
 *		ReachedEnd		   have we fetched all tuples from index already?
 *		OrderByValues	   values of ORDER BY exprs of last fetched tuple
 *		OrderByNulls	   null flags for OrderByValues
 *		SortSupport		   for reordering ORDER BY exprs
 *		OrderByTypByVals   is the datatype of order by expression pass-by-value?
 *		OrderByTypLens	   typlens of the datatypes of order by expressions
 *		PscanLen		   size of parallel index scan descriptor
 * ----------------
 */
typedef struct IndexScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *indexqualorig;
	List	   *indexorderbyorig;
	struct ScanKeyData *iss_ScanKeys;
	int			iss_NumScanKeys;
	struct ScanKeyData *iss_OrderByKeys;
	int			iss_NumOrderByKeys;
	IndexRuntimeKeyInfo *iss_RuntimeKeys;
	int			iss_NumRuntimeKeys;
	bool		iss_RuntimeKeysReady;
	ExprContext *iss_RuntimeContext;
	Relation	iss_RelationDesc;
	struct IndexScanDescData *iss_ScanDesc;
	IndexScanInstrumentation iss_Instrument;
	SharedIndexScanInstrumentation *iss_SharedInfo;

	/* These are needed for re-checking ORDER BY expr ordering */
	pairingheap *iss_ReorderQueue;
	bool		iss_ReachedEnd;
	Datum	   *iss_OrderByValues;
	bool	   *iss_OrderByNulls;
	SortSupport iss_SortSupport;
	bool	   *iss_OrderByTypByVals;
	int16	   *iss_OrderByTypLens;
	Size		iss_PscanLen;
} IndexScanState;

/* ----------------
 *	 IndexOnlyScanState information
 *
 *		recheckqual		   execution state for recheckqual expressions
 *		ScanKeys		   Skey structures for index quals
 *		NumScanKeys		   number of ScanKeys
 *		OrderByKeys		   Skey structures for index ordering operators
 *		NumOrderByKeys	   number of OrderByKeys
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 *		Instrument		   local index scan instrumentation
 *		SharedInfo		   parallel worker instrumentation (no leader entry)
 *		TableSlot		   slot for holding tuples fetched from the table
 *		VMBuffer		   buffer in use for visibility map testing, if any
 *		PscanLen		   size of parallel index-only scan descriptor
 *		NameCStringAttNums attnums of name typed columns to pad to NAMEDATALEN
 *		NameCStringCount   number of elements in the NameCStringAttNums array
 * ----------------
 */
typedef struct IndexOnlyScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *recheckqual;
	struct ScanKeyData *ioss_ScanKeys;
	int			ioss_NumScanKeys;
	struct ScanKeyData *ioss_OrderByKeys;
	int			ioss_NumOrderByKeys;
	IndexRuntimeKeyInfo *ioss_RuntimeKeys;
	int			ioss_NumRuntimeKeys;
	bool		ioss_RuntimeKeysReady;
	ExprContext *ioss_RuntimeContext;
	Relation	ioss_RelationDesc;
	struct IndexScanDescData *ioss_ScanDesc;
	IndexScanInstrumentation ioss_Instrument;
	SharedIndexScanInstrumentation *ioss_SharedInfo;
	TupleTableSlot *ioss_TableSlot;
	Buffer		ioss_VMBuffer;
	Size		ioss_PscanLen;
	AttrNumber *ioss_NameCStringAttNums;
	int			ioss_NameCStringCount;
} IndexOnlyScanState;

/* ----------------
 *	 BitmapIndexScanState information
 *
 *		result			   bitmap to return output into, or NULL
 *		ScanKeys		   Skey structures for index quals
 *		NumScanKeys		   number of ScanKeys
 *		RuntimeKeys		   info about Skeys that must be evaluated at runtime
 *		NumRuntimeKeys	   number of RuntimeKeys
 *		ArrayKeys		   info about Skeys that come from ScalarArrayOpExprs
 *		NumArrayKeys	   number of ArrayKeys
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RelationDesc	   index relation descriptor
 *		ScanDesc		   index scan descriptor
 *		Instrument		   local index scan instrumentation
 *		SharedInfo		   parallel worker instrumentation (no leader entry)
 * ----------------
 */
typedef struct BitmapIndexScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	TIDBitmap  *biss_result;
	struct ScanKeyData *biss_ScanKeys;
	int			biss_NumScanKeys;
	IndexRuntimeKeyInfo *biss_RuntimeKeys;
	int			biss_NumRuntimeKeys;
	IndexArrayKeyInfo *biss_ArrayKeys;
	int			biss_NumArrayKeys;
	bool		biss_RuntimeKeysReady;
	ExprContext *biss_RuntimeContext;
	Relation	biss_RelationDesc;
	struct IndexScanDescData *biss_ScanDesc;
	IndexScanInstrumentation biss_Instrument;
	SharedIndexScanInstrumentation *biss_SharedInfo;
} BitmapIndexScanState;

/* ----------------
 *	 BitmapHeapScanInstrumentation information
 *
 *		exact_pages		   total number of exact pages retrieved
 *		lossy_pages		   total number of lossy pages retrieved
 * ----------------
 */
typedef struct BitmapHeapScanInstrumentation
{
	uint64		exact_pages;
	uint64		lossy_pages;
} BitmapHeapScanInstrumentation;

/* ----------------
 *	 SharedBitmapState information
 *
 *		BM_INITIAL		TIDBitmap creation is not yet started, so first worker
 *						to see this state will set the state to BM_INPROGRESS
 *						and that process will be responsible for creating
 *						TIDBitmap.
 *		BM_INPROGRESS	TIDBitmap creation is in progress; workers need to
 *						sleep until it's finished.
 *		BM_FINISHED		TIDBitmap creation is done, so now all workers can
 *						proceed to iterate over TIDBitmap.
 * ----------------
 */
typedef enum
{
	BM_INITIAL,
	BM_INPROGRESS,
	BM_FINISHED,
} SharedBitmapState;

/* ----------------
 *	 ParallelBitmapHeapState information
 *		tbmiterator				iterator for scanning current pages
 *		mutex					mutual exclusion for state
 *		state					current state of the TIDBitmap
 *		cv						conditional wait variable
 * ----------------
 */
typedef struct ParallelBitmapHeapState
{
	dsa_pointer tbmiterator;
	slock_t		mutex;
	SharedBitmapState state;
	ConditionVariable cv;
} ParallelBitmapHeapState;

/* ----------------
 *	 Instrumentation data for a parallel bitmap heap scan.
 *
 * A shared memory struct that each parallel worker copies its
 * BitmapHeapScanInstrumentation information into at executor shutdown to
 * allow the leader to display the information in EXPLAIN ANALYZE.
 * ----------------
 */
typedef struct SharedBitmapHeapInstrumentation
{
	int			num_workers;
	BitmapHeapScanInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedBitmapHeapInstrumentation;

/* ----------------
 *	 BitmapHeapScanState information
 *
 *		bitmapqualorig	   execution state for bitmapqualorig expressions
 *		tbm				   bitmap obtained from child index scan(s)
 *		stats			   execution statistics
 *		initialized		   is node is ready to iterate
 *		pstate			   shared state for parallel bitmap scan
 *		sinstrument		   statistics for parallel workers
 *		recheck			   do current page's tuples need recheck
 * ----------------
 */
typedef struct BitmapHeapScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *bitmapqualorig;
	TIDBitmap  *tbm;
	BitmapHeapScanInstrumentation stats;
	bool		initialized;
	ParallelBitmapHeapState *pstate;
	SharedBitmapHeapInstrumentation *sinstrument;
	bool		recheck;
} BitmapHeapScanState;

/* ----------------
 *	 TidScanState information
 *
 *		tidexprs	   list of TidExpr structs (see nodeTidscan.c)
 *		isCurrentOf    scan has a CurrentOfExpr qual
 *		NumTids		   number of tids in this scan
 *		TidPtr		   index of currently fetched tid
 *		TidList		   evaluated item pointers (array of size NumTids)
 * ----------------
 */
typedef struct TidScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *tss_tidexprs;
	bool		tss_isCurrentOf;
	int			tss_NumTids;
	int			tss_TidPtr;
	ItemPointerData *tss_TidList;
} TidScanState;

/* ----------------
 *	 TidRangeScanState information
 *
 *		trss_tidexprs		list of TidOpExpr structs (see nodeTidrangescan.c)
 *		trss_mintid			the lowest TID in the scan range
 *		trss_maxtid			the highest TID in the scan range
 *		trss_inScan			is a scan currently in progress?
 * ----------------
 */
typedef struct TidRangeScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *trss_tidexprs;
	ItemPointerData trss_mintid;
	ItemPointerData trss_maxtid;
	bool		trss_inScan;
} TidRangeScanState;

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
 *		eflags				node's capability flags
 *		ordinality			is this scan WITH ORDINALITY?
 *		simple				true if we have 1 function and no ordinality
 *		ordinal				current ordinal column value
 *		nfuncs				number of functions being executed
 *		funcstates			per-function execution states (private in
 *							nodeFunctionscan.c)
 *		argcontext			memory context to evaluate function arguments in
 * ----------------
 */
struct FunctionScanPerFuncState;

typedef struct FunctionScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			eflags;
	bool		ordinality;
	bool		simple;
	int64		ordinal;
	int			nfuncs;
	struct FunctionScanPerFuncState *funcstates;	/* array of length nfuncs */
	MemoryContext argcontext;
} FunctionScanState;

/* ----------------
 *	 ValuesScanState information
 *
 *		ValuesScan nodes are used to scan the results of a VALUES list
 *
 *		rowcontext			per-expression-list context
 *		exprlists			array of expression lists being evaluated
 *		exprstatelists		array of expression state lists, for SubPlans only
 *		array_len			size of above arrays
 *		curr_idx			current array index (0-based)
 *
 *	Note: ss.ps.ps_ExprContext is used to evaluate any qual or projection
 *	expressions attached to the node.  We create a second ExprContext,
 *	rowcontext, in which to build the executor expression state for each
 *	Values sublist.  Resetting this context lets us get rid of expression
 *	state for each row, avoiding major memory leakage over a long values list.
 *	However, that doesn't work for sublists containing SubPlans, because a
 *	SubPlan has to be connected up to the outer plan tree to work properly.
 *	Therefore, for only those sublists containing SubPlans, we do expression
 *	state construction at executor start, and store those pointers in
 *	exprstatelists[].  NULL entries in that array correspond to simple
 *	subexpressions that are handled as described above.
 * ----------------
 */
typedef struct ValuesScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprContext *rowcontext;
	List	  **exprlists;
	List	  **exprstatelists;
	int			array_len;
	int			curr_idx;
} ValuesScanState;

/* ----------------
 *		TableFuncScanState node
 *
 * Used in table-expression functions like XMLTABLE.
 * ----------------
 */
typedef struct TableFuncScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *docexpr;		/* state for document expression */
	ExprState  *rowexpr;		/* state for row-generating expression */
	List	   *colexprs;		/* state for column-generating expression */
	List	   *coldefexprs;	/* state for column default expressions */
	List	   *colvalexprs;	/* state for column value expressions */
	List	   *passingvalexprs;	/* state for PASSING argument expressions */
	List	   *ns_names;		/* same as TableFunc.ns_names */
	List	   *ns_uris;		/* list of states of namespace URI exprs */
	Bitmapset  *notnulls;		/* nullability flag for each output column */
	void	   *opaque;			/* table builder private space */
	const struct TableFuncRoutine *routine; /* table builder methods */
	FmgrInfo   *in_functions;	/* input function for each column */
	Oid		   *typioparams;	/* typioparam for each column */
	int64		ordinal;		/* row number to be output next */
	MemoryContext perTableCxt;	/* per-table context */
	Tuplestorestate *tupstore;	/* output tuple store */
} TableFuncScanState;

/* ----------------
 *	 CteScanState information
 *
 *		CteScan nodes are used to scan a CommonTableExpr query.
 *
 * Multiple CteScan nodes can read out from the same CTE query.  We use
 * a tuplestore to hold rows that have been read from the CTE query but
 * not yet consumed by all readers.
 * ----------------
 */
typedef struct CteScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			eflags;			/* capability flags to pass to tuplestore */
	int			readptr;		/* index of my tuplestore read pointer */
	PlanState  *cteplanstate;	/* PlanState for the CTE query itself */
	/* Link to the "leader" CteScanState (possibly this same node) */
	struct CteScanState *leader;
	/* The remaining fields are only valid in the "leader" CteScanState */
	Tuplestorestate *cte_table; /* rows already read from the CTE query */
	bool		eof_cte;		/* reached end of CTE query? */
} CteScanState;

/* ----------------
 *	 NamedTuplestoreScanState information
 *
 *		NamedTuplestoreScan nodes are used to scan a Tuplestore created and
 *		named prior to execution of the query.  An example is a transition
 *		table for an AFTER trigger.
 *
 * Multiple NamedTuplestoreScan nodes can read out from the same Tuplestore.
 * ----------------
 */
typedef struct NamedTuplestoreScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			readptr;		/* index of my tuplestore read pointer */
	TupleDesc	tupdesc;		/* format of the tuples in the tuplestore */
	Tuplestorestate *relation;	/* the rows */
} NamedTuplestoreScanState;

/* ----------------
 *	 WorkTableScanState information
 *
 *		WorkTableScan nodes are used to scan the work table created by
 *		a RecursiveUnion node.  We locate the RecursiveUnion node
 *		during executor startup.
 * ----------------
 */
typedef struct WorkTableScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	RecursiveUnionState *rustate;
} WorkTableScanState;

/* ----------------
 *	 ForeignScanState information
 *
 *		ForeignScan nodes are used to scan foreign-data tables.
 * ----------------
 */
typedef struct ForeignScanState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *fdw_recheck_quals;	/* original quals not in ss.ps.qual */
	Size		pscan_len;		/* size of parallel coordination information */
	ResultRelInfo *resultRelInfo;	/* result rel info, if UPDATE or DELETE */
	/* use struct pointer to avoid including fdwapi.h here */
	struct FdwRoutine *fdwroutine;
	void	   *fdw_state;		/* foreign-data wrapper can keep state here */
} ForeignScanState;

/* ----------------
 *	 CustomScanState information
 *
 *		CustomScan nodes are used to execute custom code within executor.
 *
 * Core code must avoid assuming that the CustomScanState is only as large as
 * the structure declared here; providers are allowed to make it the first
 * element in a larger structure, and typically would need to do so.  The
 * struct is actually allocated by the CreateCustomScanState method associated
 * with the plan node.  Any additional fields can be initialized there, or in
 * the BeginCustomScan method.
 * ----------------
 */
struct CustomExecMethods;

typedef struct CustomScanState
{
	ScanState	ss;
	uint32		flags;			/* mask of CUSTOMPATH_* flags, see
								 * nodes/extensible.h */
	List	   *custom_ps;		/* list of child PlanState nodes, if any */
	Size		pscan_len;		/* size of parallel coordination information */
	const struct CustomExecMethods *methods;
	const struct TupleTableSlotOps *slotOps;
} CustomScanState;

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
	bool		single_match;	/* True if we should skip to next outer tuple
								 * after finding one inner match */
	ExprState  *joinqual;		/* JOIN quals (in addition to ps.qual) */
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
 *		JoinState		   current state of ExecMergeJoin state machine
 *		SkipMarkRestore    true if we may skip Mark and Restore operations
 *		ExtraMarks		   true to issue extra Mark operations on inner scan
 *		ConstFalseJoin	   true if we have a constant-false joinqual
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
	bool		mj_SkipMarkRestore;
	bool		mj_ExtraMarks;
	bool		mj_ConstFalseJoin;
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
 *		hashclauses				original form of the hashjoin condition
 *		hj_OuterHash			ExprState for hashing outer keys
 *		hj_HashTable			hash table for the hashjoin
 *								(NULL if table not built yet)
 *		hj_CurHashValue			hash value for current outer tuple
 *		hj_CurBucketNo			regular bucket# for current outer tuple
 *		hj_CurSkewBucketNo		skew bucket# for current outer tuple
 *		hj_CurTuple				last inner tuple matched to current outer
 *								tuple, or NULL if starting search
 *								(hj_CurXXX variables are undefined if
 *								OuterTupleSlot is empty!)
 *		hj_OuterTupleSlot		tuple slot for outer tuples
 *		hj_HashTupleSlot		tuple slot for inner (hashed) tuples
 *		hj_NullOuterTupleSlot	prepared null tuple for right/right-anti/full
 *								outer joins
 *		hj_NullInnerTupleSlot	prepared null tuple for left/full outer joins
 *		hj_FirstOuterTupleSlot	first tuple retrieved from outer plan
 *		hj_JoinState			current state of ExecHashJoin state machine
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
	ExprState  *hashclauses;
	ExprState  *hj_OuterHash;
	HashJoinTable hj_HashTable;
	uint32		hj_CurHashValue;
	int			hj_CurBucketNo;
	int			hj_CurSkewBucketNo;
	HashJoinTuple hj_CurTuple;
	TupleTableSlot *hj_OuterTupleSlot;
	TupleTableSlot *hj_HashTupleSlot;
	TupleTableSlot *hj_NullOuterTupleSlot;
	TupleTableSlot *hj_NullInnerTupleSlot;
	TupleTableSlot *hj_FirstOuterTupleSlot;
	int			hj_JoinState;
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
	Tuplestorestate *tuplestorestate;
} MaterialState;

struct MemoizeEntry;
struct MemoizeTuple;
struct MemoizeKey;

typedef struct MemoizeInstrumentation
{
	uint64		cache_hits;		/* number of rescans where we've found the
								 * scan parameter values to be cached */
	uint64		cache_misses;	/* number of rescans where we've not found the
								 * scan parameter values to be cached. */
	uint64		cache_evictions;	/* number of cache entries removed due to
									 * the need to free memory */
	uint64		cache_overflows;	/* number of times we've had to bypass the
									 * cache when filling it due to not being
									 * able to free enough space to store the
									 * current scan's tuples. */
	uint64		mem_peak;		/* peak memory usage in bytes */
} MemoizeInstrumentation;

/* ----------------
 *	 Shared memory container for per-worker memoize information
 * ----------------
 */
typedef struct SharedMemoizeInfo
{
	int			num_workers;
	MemoizeInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedMemoizeInfo;

/* ----------------
 *	 MemoizeState information
 *
 *		memoize nodes are used to cache recent and commonly seen results from
 *		a parameterized scan.
 * ----------------
 */
typedef struct MemoizeState
{
	ScanState	ss;				/* its first field is NodeTag */
	int			mstatus;		/* value of ExecMemoize state machine */
	int			nkeys;			/* number of cache keys */
	struct memoize_hash *hashtable; /* hash table for cache entries */
	TupleDesc	hashkeydesc;	/* tuple descriptor for cache keys */
	TupleTableSlot *tableslot;	/* min tuple slot for existing cache entries */
	TupleTableSlot *probeslot;	/* virtual slot used for hash lookups */
	ExprState  *cache_eq_expr;	/* Compare exec params to hash key */
	ExprState **param_exprs;	/* exprs containing the parameters to this
								 * node */
	FmgrInfo   *hashfunctions;	/* lookup data for hash funcs nkeys in size */
	Oid		   *collations;		/* collation for comparisons nkeys in size */
	uint64		mem_used;		/* bytes of memory used by cache */
	uint64		mem_limit;		/* memory limit in bytes for the cache */
	MemoryContext tableContext; /* memory context to store cache data */
	dlist_head	lru_list;		/* least recently used entry list */
	struct MemoizeTuple *last_tuple;	/* Used to point to the last tuple
										 * returned during a cache hit and the
										 * tuple we last stored when
										 * populating the cache. */
	struct MemoizeEntry *entry; /* the entry that 'last_tuple' belongs to or
								 * NULL if 'last_tuple' is NULL. */
	bool		singlerow;		/* true if the cache entry is to be marked as
								 * complete after caching the first tuple. */
	bool		binary_mode;	/* true when cache key should be compared bit
								 * by bit, false when using hash equality ops */
	MemoizeInstrumentation stats;	/* execution statistics */
	SharedMemoizeInfo *shared_info; /* statistics for parallel workers */
	Bitmapset  *keyparamids;	/* Param->paramids of expressions belonging to
								 * param_exprs */
} MemoizeState;

/* ----------------
 *	 When performing sorting by multiple keys, it's possible that the input
 *	 dataset is already sorted on a prefix of those keys. We call these
 *	 "presorted keys".
 *	 PresortedKeyData represents information about one such key.
 * ----------------
 */
typedef struct PresortedKeyData
{
	FmgrInfo	flinfo;			/* comparison function info */
	FunctionCallInfo fcinfo;	/* comparison function call info */
	OffsetNumber attno;			/* attribute number in tuple */
} PresortedKeyData;

/* ----------------
 *	 Shared memory container for per-worker sort information
 * ----------------
 */
typedef struct SharedSortInfo
{
	int			num_workers;
	TuplesortInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedSortInfo;

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
	bool		am_worker;		/* are we a worker? */
	bool		datumSort;		/* Datum sort instead of tuple sort? */
	SharedSortInfo *shared_info;	/* one entry per worker */
} SortState;

/* ----------------
 *	 Instrumentation information for IncrementalSort
 * ----------------
 */
typedef struct IncrementalSortGroupInfo
{
	int64		groupCount;
	int64		maxDiskSpaceUsed;
	int64		totalDiskSpaceUsed;
	int64		maxMemorySpaceUsed;
	int64		totalMemorySpaceUsed;
	bits32		sortMethods;	/* bitmask of TuplesortMethod */
} IncrementalSortGroupInfo;

typedef struct IncrementalSortInfo
{
	IncrementalSortGroupInfo fullsortGroupInfo;
	IncrementalSortGroupInfo prefixsortGroupInfo;
} IncrementalSortInfo;

/* ----------------
 *	 Shared memory container for per-worker incremental sort information
 * ----------------
 */
typedef struct SharedIncrementalSortInfo
{
	int			num_workers;
	IncrementalSortInfo sinfo[FLEXIBLE_ARRAY_MEMBER];
} SharedIncrementalSortInfo;

/* ----------------
 *	 IncrementalSortState information
 * ----------------
 */
typedef enum
{
	INCSORT_LOADFULLSORT,
	INCSORT_LOADPREFIXSORT,
	INCSORT_READFULLSORT,
	INCSORT_READPREFIXSORT,
} IncrementalSortExecutionStatus;

typedef struct IncrementalSortState
{
	ScanState	ss;				/* its first field is NodeTag */
	bool		bounded;		/* is the result set bounded? */
	int64		bound;			/* if bounded, how many tuples are needed */
	bool		outerNodeDone;	/* finished fetching tuples from outer node */
	int64		bound_Done;		/* value of bound we did the sort with */
	IncrementalSortExecutionStatus execution_status;
	int64		n_fullsort_remaining;
	Tuplesortstate *fullsort_state; /* private state of tuplesort.c */
	Tuplesortstate *prefixsort_state;	/* private state of tuplesort.c */
	/* the keys by which the input path is already sorted */
	PresortedKeyData *presorted_keys;

	IncrementalSortInfo incsort_info;

	/* slot for pivot tuple defining values of presorted keys within group */
	TupleTableSlot *group_pivot;
	TupleTableSlot *transfer_tuple;
	bool		am_worker;		/* are we a worker? */
	SharedIncrementalSortInfo *shared_info; /* one entry per worker */
} IncrementalSortState;

/* ---------------------
 *	GroupState information
 * ---------------------
 */
typedef struct GroupState
{
	ScanState	ss;				/* its first field is NodeTag */
	ExprState  *eqfunction;		/* equality function */
	bool		grp_done;		/* indicates completion of Group scan */
} GroupState;

/* ---------------------
 *	per-worker aggregate information
 * ---------------------
 */
typedef struct AggregateInstrumentation
{
	Size		hash_mem_peak;	/* peak hash table memory usage */
	uint64		hash_disk_used; /* kB of disk space used */
	int			hash_batches_used;	/* batches used during entire execution */
} AggregateInstrumentation;

/* ----------------
 *	 Shared memory container for per-worker aggregate information
 * ----------------
 */
typedef struct SharedAggInfo
{
	int			num_workers;
	AggregateInstrumentation sinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedAggInfo;

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
 * ---------------------
 */
/* these structs are private in nodeAgg.c: */
typedef struct AggStatePerAggData *AggStatePerAgg;
typedef struct AggStatePerTransData *AggStatePerTrans;
typedef struct AggStatePerGroupData *AggStatePerGroup;
typedef struct AggStatePerPhaseData *AggStatePerPhase;
typedef struct AggStatePerHashData *AggStatePerHash;

typedef struct AggState
{
	ScanState	ss;				/* its first field is NodeTag */
	List	   *aggs;			/* all Aggref nodes in targetlist & quals */
	int			numaggs;		/* length of list (could be zero!) */
	int			numtrans;		/* number of pertrans items */
	AggStrategy aggstrategy;	/* strategy mode */
	AggSplit	aggsplit;		/* agg-splitting mode, see nodes.h */
	AggStatePerPhase phase;		/* pointer to current phase data */
	int			numphases;		/* number of phases (including phase 0) */
	int			current_phase;	/* current phase number */
	AggStatePerAgg peragg;		/* per-Aggref information */
	AggStatePerTrans pertrans;	/* per-Trans state information */
	ExprContext *hashcontext;	/* econtexts for long-lived data (hashtable) */
	ExprContext **aggcontexts;	/* econtexts for long-lived data (per GS) */
	ExprContext *tmpcontext;	/* econtext for input expressions */
#define FIELDNO_AGGSTATE_CURAGGCONTEXT 14
	ExprContext *curaggcontext; /* currently active aggcontext */
	AggStatePerAgg curperagg;	/* currently active aggregate, if any */
#define FIELDNO_AGGSTATE_CURPERTRANS 16
	AggStatePerTrans curpertrans;	/* currently active trans state, if any */
	bool		input_done;		/* indicates end of input */
	bool		agg_done;		/* indicates completion of Agg scan */
	int			projected_set;	/* The last projected grouping set */
#define FIELDNO_AGGSTATE_CURRENT_SET 20
	int			current_set;	/* The current grouping set being evaluated */
	Bitmapset  *grouped_cols;	/* grouped cols in current projection */
	List	   *all_grouped_cols;	/* list of all grouped cols in DESC order */
	Bitmapset  *colnos_needed;	/* all columns needed from the outer plan */
	int			max_colno_needed;	/* highest colno needed from outer plan */
	bool		all_cols_needed;	/* are all cols from outer plan needed? */
	/* These fields are for grouping set phase data */
	int			maxsets;		/* The max number of sets in any phase */
	AggStatePerPhase phases;	/* array of all phases */
	Tuplesortstate *sort_in;	/* sorted input to phases > 1 */
	Tuplesortstate *sort_out;	/* input is copied here for next phase */
	TupleTableSlot *sort_slot;	/* slot for sort results */
	/* these fields are used in AGG_PLAIN and AGG_SORTED modes: */
	AggStatePerGroup *pergroups;	/* grouping set indexed array of per-group
									 * pointers */
	HeapTuple	grp_firstTuple; /* copy of first tuple of current group */
	/* these fields are used in AGG_HASHED and AGG_MIXED modes: */
	bool		table_filled;	/* hash table filled yet? */
	int			num_hashes;
	MemoryContext hash_metacxt; /* memory for hash table bucket array */
	MemoryContext hash_tablecxt;	/* memory for hash table entries */
	struct LogicalTapeSet *hash_tapeset;	/* tape set for hash spill tapes */
	struct HashAggSpill *hash_spills;	/* HashAggSpill for each grouping set,
										 * exists only during first pass */
	TupleTableSlot *hash_spill_rslot;	/* for reading spill files */
	TupleTableSlot *hash_spill_wslot;	/* for writing spill files */
	List	   *hash_batches;	/* hash batches remaining to be processed */
	bool		hash_ever_spilled;	/* ever spilled during this execution? */
	bool		hash_spill_mode;	/* we hit a limit during the current batch
									 * and we must not create new groups */
	Size		hash_mem_limit; /* limit before spilling hash table */
	uint64		hash_ngroups_limit; /* limit before spilling hash table */
	int			hash_planned_partitions;	/* number of partitions planned
											 * for first pass */
	double		hashentrysize;	/* estimate revised during execution */
	Size		hash_mem_peak;	/* peak hash table memory usage */
	uint64		hash_ngroups_current;	/* number of groups currently in
										 * memory in all hash tables */
	uint64		hash_disk_used; /* kB of disk space used */
	int			hash_batches_used;	/* batches used during entire execution */

	AggStatePerHash perhash;	/* array of per-hashtable data */
	AggStatePerGroup *hash_pergroup;	/* grouping set indexed array of
										 * per-group pointers */

	/* support for evaluation of agg input expressions: */
#define FIELDNO_AGGSTATE_ALL_PERGROUPS 54
	AggStatePerGroup *all_pergroups;	/* array of first ->pergroups, than
										 * ->hash_pergroup */
	SharedAggInfo *shared_info; /* one entry per worker */
} AggState;

/* ----------------
 *	WindowAggState information
 * ----------------
 */
/* these structs are private in nodeWindowAgg.c: */
typedef struct WindowStatePerFuncData *WindowStatePerFunc;
typedef struct WindowStatePerAggData *WindowStatePerAgg;

/*
 * WindowAggStatus -- Used to track the status of WindowAggState
 */
typedef enum WindowAggStatus
{
	WINDOWAGG_DONE,				/* No more processing to do */
	WINDOWAGG_RUN,				/* Normal processing of window funcs */
	WINDOWAGG_PASSTHROUGH,		/* Don't eval window funcs */
	WINDOWAGG_PASSTHROUGH_STRICT,	/* Pass-through plus don't store new
									 * tuples during spool */
} WindowAggStatus;

typedef struct WindowAggState
{
	ScanState	ss;				/* its first field is NodeTag */

	/* these fields are filled in by ExecInitExpr: */
	List	   *funcs;			/* all WindowFunc nodes in targetlist */
	int			numfuncs;		/* total number of window functions */
	int			numaggs;		/* number that are plain aggregates */

	WindowStatePerFunc perfunc; /* per-window-function information */
	WindowStatePerAgg peragg;	/* per-plain-aggregate information */
	ExprState  *partEqfunction; /* equality funcs for partition columns */
	ExprState  *ordEqfunction;	/* equality funcs for ordering columns */
	Tuplestorestate *buffer;	/* stores rows of current partition */
	int			current_ptr;	/* read pointer # for current row */
	int			framehead_ptr;	/* read pointer # for frame head, if used */
	int			frametail_ptr;	/* read pointer # for frame tail, if used */
	int			grouptail_ptr;	/* read pointer # for group tail, if used */
	int64		spooled_rows;	/* total # of rows in buffer */
	int64		currentpos;		/* position of current row in partition */
	int64		frameheadpos;	/* current frame head position */
	int64		frametailpos;	/* current frame tail position (frame end+1) */
	/* use struct pointer to avoid including windowapi.h here */
	struct WindowObjectData *agg_winobj;	/* winobj for aggregate fetches */
	int64		aggregatedbase; /* start row for current aggregates */
	int64		aggregatedupto; /* rows before this one are aggregated */
	WindowAggStatus status;		/* run status of WindowAggState */

	int			frameOptions;	/* frame_clause options, see WindowDef */
	ExprState  *startOffset;	/* expression for starting bound offset */
	ExprState  *endOffset;		/* expression for ending bound offset */
	Datum		startOffsetValue;	/* result of startOffset evaluation */
	Datum		endOffsetValue; /* result of endOffset evaluation */

	/* these fields are used with RANGE offset PRECEDING/FOLLOWING: */
	FmgrInfo	startInRangeFunc;	/* in_range function for startOffset */
	FmgrInfo	endInRangeFunc; /* in_range function for endOffset */
	Oid			inRangeColl;	/* collation for in_range tests */
	bool		inRangeAsc;		/* use ASC sort order for in_range tests? */
	bool		inRangeNullsFirst;	/* nulls sort first for in_range tests? */

	/* fields relating to runconditions */
	bool		use_pass_through;	/* When false, stop execution when
									 * runcondition is no longer true.  Else
									 * just stop evaluating window funcs. */
	bool		top_window;		/* true if this is the top-most WindowAgg or
								 * the only WindowAgg in this query level */
	ExprState  *runcondition;	/* Condition which must remain true otherwise
								 * execution of the WindowAgg will finish or
								 * go into pass-through mode.  NULL when there
								 * is no such condition. */

	/* these fields are used in GROUPS mode: */
	int64		currentgroup;	/* peer group # of current row in partition */
	int64		frameheadgroup; /* peer group # of frame head row */
	int64		frametailgroup; /* peer group # of frame tail row */
	int64		groupheadpos;	/* current row's peer group head position */
	int64		grouptailpos;	/* " " " " tail position (group end+1) */

	MemoryContext partcontext;	/* context for partition-lifespan data */
	MemoryContext aggcontext;	/* shared context for aggregate working data */
	MemoryContext curaggcontext;	/* current aggregate's working data */
	ExprContext *tmpcontext;	/* short-term evaluation context */

	bool		all_first;		/* true if the scan is starting */
	bool		partition_spooled;	/* true if all tuples in current partition
									 * have been spooled into tuplestore */
	bool		next_partition; /* true if begin_partition needs to be called */
	bool		more_partitions;	/* true if there's more partitions after
									 * this one */
	bool		framehead_valid;	/* true if frameheadpos is known up to
									 * date for current row */
	bool		frametail_valid;	/* true if frametailpos is known up to
									 * date for current row */
	bool		grouptail_valid;	/* true if grouptailpos is known up to
									 * date for current row */

	TupleTableSlot *first_part_slot;	/* first tuple of current or next
										 * partition */
	TupleTableSlot *framehead_slot; /* first tuple of current frame */
	TupleTableSlot *frametail_slot; /* first tuple after current frame */

	/* temporary slots for tuples fetched back from tuplestore */
	TupleTableSlot *agg_row_slot;
	TupleTableSlot *temp_slot_1;
	TupleTableSlot *temp_slot_2;
} WindowAggState;

/* ----------------
 *	 UniqueState information
 *
 *		Unique nodes are used "on top of" sort nodes to discard
 *		duplicate tuples returned from the sort phase.  Basically
 *		all it does is compare the current tuple from the subplan
 *		with the previously fetched tuple (stored in its result slot).
 *		If the two are identical in all interesting fields, then
 *		we just fetch another tuple from the sort and try again.
 * ----------------
 */
typedef struct UniqueState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *eqfunction;		/* tuple equality qual */
} UniqueState;

/* ----------------
 * GatherState information
 *
 *		Gather nodes launch 1 or more parallel workers, run a subplan
 *		in those workers, and collect the results.
 * ----------------
 */
typedef struct GatherState
{
	PlanState	ps;				/* its first field is NodeTag */
	bool		initialized;	/* workers launched? */
	bool		need_to_scan_locally;	/* need to read from local plan? */
	int64		tuples_needed;	/* tuple bound, see ExecSetTupleBound */
	/* these fields are set up once: */
	TupleTableSlot *funnel_slot;
	struct ParallelExecutorInfo *pei;
	/* all remaining fields are reinitialized during a rescan: */
	int			nworkers_launched;	/* original number of workers */
	int			nreaders;		/* number of still-active workers */
	int			nextreader;		/* next one to try to read from */
	struct TupleQueueReader **reader;	/* array with nreaders active entries */
} GatherState;

/* ----------------
 * GatherMergeState information
 *
 *		Gather merge nodes launch 1 or more parallel workers, run a
 *		subplan which produces sorted output in each worker, and then
 *		merge the results into a single sorted stream.
 * ----------------
 */
struct GMReaderTupleBuffer;		/* private in nodeGatherMerge.c */

typedef struct GatherMergeState
{
	PlanState	ps;				/* its first field is NodeTag */
	bool		initialized;	/* workers launched? */
	bool		gm_initialized; /* gather_merge_init() done? */
	bool		need_to_scan_locally;	/* need to read from local plan? */
	int64		tuples_needed;	/* tuple bound, see ExecSetTupleBound */
	/* these fields are set up once: */
	TupleDesc	tupDesc;		/* descriptor for subplan result tuples */
	int			gm_nkeys;		/* number of sort columns */
	SortSupport gm_sortkeys;	/* array of length gm_nkeys */
	struct ParallelExecutorInfo *pei;
	/* all remaining fields are reinitialized during a rescan */
	/* (but the arrays are not reallocated, just cleared) */
	int			nworkers_launched;	/* original number of workers */
	int			nreaders;		/* number of active workers */
	TupleTableSlot **gm_slots;	/* array with nreaders+1 entries */
	struct TupleQueueReader **reader;	/* array with nreaders active entries */
	struct GMReaderTupleBuffer *gm_tuple_buffers;	/* nreaders tuple buffers */
	struct binaryheap *gm_heap; /* binary heap of slot indices */
} GatherMergeState;

/* ----------------
 *	 Values displayed by EXPLAIN ANALYZE
 * ----------------
 */
typedef struct HashInstrumentation
{
	int			nbuckets;		/* number of buckets at end of execution */
	int			nbuckets_original;	/* planned number of buckets */
	int			nbatch;			/* number of batches at end of execution */
	int			nbatch_original;	/* planned number of batches */
	Size		space_peak;		/* peak memory usage in bytes */
} HashInstrumentation;

/* ----------------
 *	 Shared memory container for per-worker hash information
 * ----------------
 */
typedef struct SharedHashInfo
{
	int			num_workers;
	HashInstrumentation hinstrument[FLEXIBLE_ARRAY_MEMBER];
} SharedHashInfo;

/* ----------------
 *	 HashState information
 * ----------------
 */
typedef struct HashState
{
	PlanState	ps;				/* its first field is NodeTag */
	HashJoinTable hashtable;	/* hash table for the hashjoin */
	ExprState  *hash_expr;		/* ExprState to get hash value */

	FmgrInfo   *skew_hashfunction;	/* lookup data for skew hash function */
	Oid			skew_collation; /* collation to call skew_hashfunction with */

	/*
	 * In a parallelized hash join, the leader retains a pointer to the
	 * shared-memory stats area in its shared_info field, and then copies the
	 * shared-memory info back to local storage before DSM shutdown.  The
	 * shared_info field remains NULL in workers, or in non-parallel joins.
	 */
	SharedHashInfo *shared_info;

	/*
	 * If we are collecting hash stats, this points to an initially-zeroed
	 * collection area, which could be either local storage or in shared
	 * memory; either way it's for just one process.
	 */
	HashInstrumentation *hinstrument;

	/* Parallel hash state. */
	struct ParallelHashJoinState *parallel_state;
} HashState;

/* ----------------
 *	 SetOpState information
 *
 *		SetOp nodes support either sorted or hashed de-duplication.
 *		The sorted mode is a bit like MergeJoin, the hashed mode like Agg.
 * ----------------
 */
typedef struct SetOpStatePerInput
{
	TupleTableSlot *firstTupleSlot; /* first tuple of current group */
	int64		numTuples;		/* number of tuples in current group */
	TupleTableSlot *nextTupleSlot;	/* next input tuple, if already read */
	bool		needGroup;		/* do we need to load a new group? */
} SetOpStatePerInput;

typedef struct SetOpState
{
	PlanState	ps;				/* its first field is NodeTag */
	bool		setop_done;		/* indicates completion of output scan */
	int64		numOutput;		/* number of dups left to output */
	int			numCols;		/* number of grouping columns */

	/* these fields are used in SETOP_SORTED mode: */
	SortSupport sortKeys;		/* per-grouping-field sort data */
	SetOpStatePerInput leftInput;	/* current outer-relation input state */
	SetOpStatePerInput rightInput;	/* current inner-relation input state */
	bool		need_init;		/* have we read the first tuples yet? */

	/* these fields are used in SETOP_HASHED mode: */
	Oid		   *eqfuncoids;		/* per-grouping-field equality fns */
	FmgrInfo   *hashfunctions;	/* per-grouping-field hash fns */
	TupleHashTable hashtable;	/* hash table with one entry per group */
	MemoryContext tableContext; /* memory context containing hash table */
	bool		table_filled;	/* hash table filled yet? */
	TupleHashIterator hashiter; /* for iterating through hash table */
} SetOpState;

/* ----------------
 *	 LockRowsState information
 *
 *		LockRows nodes are used to enforce FOR [KEY] UPDATE/SHARE locking.
 * ----------------
 */
typedef struct LockRowsState
{
	PlanState	ps;				/* its first field is NodeTag */
	List	   *lr_arowMarks;	/* List of ExecAuxRowMarks */
	EPQState	lr_epqstate;	/* for evaluating EvalPlanQual rechecks */
} LockRowsState;

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
	LIMIT_WINDOWEND_TIES,		/* have returned a tied row */
	LIMIT_SUBPLANEOF,			/* at EOF of subplan (within window) */
	LIMIT_WINDOWEND,			/* stepped off end of window */
	LIMIT_WINDOWSTART,			/* stepped off beginning of window */
} LimitStateCond;

typedef struct LimitState
{
	PlanState	ps;				/* its first field is NodeTag */
	ExprState  *limitOffset;	/* OFFSET parameter, or NULL if none */
	ExprState  *limitCount;		/* COUNT parameter, or NULL if none */
	LimitOption limitOption;	/* limit specification type */
	int64		offset;			/* current OFFSET value */
	int64		count;			/* current COUNT, if any */
	bool		noCount;		/* if true, ignore count */
	LimitStateCond lstate;		/* state machine status, as above */
	int64		position;		/* 1-based index of last tuple returned */
	TupleTableSlot *subSlot;	/* tuple last obtained from subplan */
	ExprState  *eqfunction;		/* tuple equality qual in case of WITH TIES
								 * option */
	TupleTableSlot *last_slot;	/* slot for evaluation of ties */
} LimitState;

#endif							/* EXECNODES_H */
