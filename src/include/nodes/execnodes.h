/*-------------------------------------------------------------------------
 *
 * execnodes.h
 *	  definitions for executor state nodes
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execnodes.h,v 1.75.2.1 2003/01/23 05:10:57 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECNODES_H
#define EXECNODES_H

#include "access/relscan.h"
#include "access/sdir.h"
#include "executor/hashjoin.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "nodes/params.h"
#include "nodes/primnodes.h"
#include "utils/tuplestore.h"


/* ----------------
 *	  IndexInfo information
 *
 *		this class holds the information needed to construct new index
 *		entries for a particular index.  Used for both index_build and
 *		retail creation of index entries.
 *
 *		NumIndexAttrs		number of columns in this index
 *							(1 if a func. index, else same as NumKeyAttrs)
 *		NumKeyAttrs			number of key attributes for this index
 *							(ie, number of attrs from underlying relation)
 *		KeyAttrNumbers		underlying-rel attribute numbers used as keys
 *		Predicate			partial-index predicate, or NIL if none
 *		FuncOid				OID of function, or InvalidOid if not f. index
 *		FuncInfo			fmgr lookup data for function, if FuncOid valid
 *		Unique				is it a unique index?
 * ----------------
 */
typedef struct IndexInfo
{
	NodeTag		type;
	int			ii_NumIndexAttrs;
	int			ii_NumKeyAttrs;
	AttrNumber	ii_KeyAttrNumbers[INDEX_MAX_KEYS];
	List	   *ii_Predicate;
	Oid			ii_FuncOid;
	FmgrInfo	ii_FuncInfo;
	bool		ii_Unique;
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
 *	* ecxt_per_query_memory is a relatively long-lived context (such as
 *	  TransactionCommandContext); typically it's the same context the
 *	  ExprContext node itself is allocated in.	This context can be
 *	  used for purposes such as storing operator/function fcache nodes.
 *	* ecxt_per_tuple_memory is a short-term context for expression results.
 *	  As the name suggests, it will typically be reset once per tuple,
 *	  before we begin to evaluate expressions for that tuple.  Each
 *	  ExprContext normally has its very own per-tuple memory context.
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
 *		This is all the information needed to perform projections
 *		on a tuple.  Nodes which need to do projections create one
 *		of these.  In theory, when a node wants to perform a projection
 *		it should just update this information as necessary and then
 *		call ExecProject().  -cim 6/3/91
 *
 *		targetlist		target list for projection
 *		len				length of target list
 *		tupValue		array of pointers to projection results
 *		exprContext		expression context for ExecTargetList
 *		slot			slot to place projection result in
 * ----------------
 */
typedef struct ProjectionInfo
{
	NodeTag		type;
	List	   *pi_targetlist;
	int			pi_len;
	Datum	   *pi_tupValue;
	ExprContext *pi_exprContext;
	TupleTableSlot *pi_slot;
} ProjectionInfo;

/* ----------------
 *	  JunkFilter
 *
 *	  This class is used to store information regarding junk attributes.
 *	  A junk attribute is an attribute in a tuple that is needed only for
 *	  storing intermediate information in the executor, and does not belong
 *	  in emitted tuples.	For example, when we do an UPDATE query,
 *	  the planner adds a "junk" entry to the targetlist so that the tuples
 *	  returned to ExecutePlan() contain an extra attribute: the ctid of
 *	  the tuple to be updated.	This is needed to do the update, but we
 *	  don't want the ctid to be part of the stored new tuple!  So, we
 *	  apply a "junk filter" to remove the junk attributes and form the
 *	  real output tuple.
 *
 *	  targetList:		the original target list (including junk attributes).
 *	  length:			the length of 'targetList'.
 *	  tupType:			the tuple descriptor for the "original" tuple
 *						(including the junk attributes).
 *	  cleanTargetList:	the "clean" target list (junk attributes removed).
 *	  cleanLength:		the length of 'cleanTargetList'
 *	  cleanTupType:		the tuple descriptor of the "clean" tuple (with
 *						junk attributes removed).
 *	  cleanMap:			A map with the correspondence between the non-junk
 *						attribute numbers of the "original" tuple and the
 *						attribute numbers of the "clean" tuple.
 *	  junkContext:		memory context holding the JunkFilter node and all
 *						its subsidiary data structures.
 *	  resultSlot:		tuple slot that can be used to hold cleaned tuple.
 *
 * NOTE: the original targetList and tupType are passed to ExecInitJunkFilter,
 * as is the resultSlot.  These items do not belong to the JunkFilter.	All
 * the other subsidiary structures are created during ExecInitJunkFilter,
 * and all of them can be freed by deleting the memory context junkContext.
 * This would not be needed if we had a cleaner approach to managing
 * query-lifetime data structures...
 * ----------------
 */
typedef struct JunkFilter
{
	NodeTag		type;
	List	   *jf_targetList;
	int			jf_length;
	TupleDesc	jf_tupType;
	List	   *jf_cleanTargetList;
	int			jf_cleanLength;
	TupleDesc	jf_cleanTupType;
	AttrNumber *jf_cleanMap;
	MemoryContext jf_junkContext;
	TupleTableSlot *jf_resultSlot;
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
 *		ConstraintExprs			array of constraint-checking expressions
 *		junkFilter				for removing junk attributes from tuples
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
	List	  **ri_ConstraintExprs;
	JunkFilter *ri_junkFilter;
} ResultRelInfo;

/* ----------------
 *	  EState information
 *
 *		direction						direction of the scan
 *
 *		range_table						array of scan relation information
 *
 *		result_relation information		for insert/update/delete queries
 *
 *		into_relation_descriptor		relation being retrieved "into"
 *
 *		param_list_info					information needed to transform
 *										Param nodes into Const nodes
 *
 *		tupleTable						this is a pointer to an array
 *										of pointers to tuples used by
 *										the executor at any given moment.
 * ----------------
 */
typedef struct EState
{
	NodeTag		type;
	ScanDirection es_direction;
	Snapshot	es_snapshot;
	List	   *es_range_table;
	ResultRelInfo *es_result_relations; /* array of ResultRelInfos */
	int			es_num_result_relations;		/* length of array */
	ResultRelInfo *es_result_relation_info;		/* currently active array
												 * elt */
	JunkFilter *es_junkFilter;	/* currently active junk filter */
	Relation	es_into_relation_descriptor;
	ParamListInfo es_param_list_info;
	ParamExecData *es_param_exec_vals;	/* this is for subselects */
	TupleTable	es_tupleTable;
	uint32		es_processed;	/* # of tuples processed */
	Oid			es_lastoid;		/* last oid processed (by INSERT) */
	List	   *es_rowMark;		/* not good place, but there is no other */
	bool		es_force_oids;	/* true forces result tuples to have (space
								 * for) OIDs --- used for SELECT INTO */
	MemoryContext es_query_cxt; /* per-query context in which EState lives */

	/*
	 * this ExprContext is for per-output-tuple operations, such as
	 * constraint checks and index-value computations.	It will be reset
	 * for each output tuple.  Note that it will be created only if
	 * needed.
	 */
	ExprContext *es_per_tuple_exprcontext;
	/* Below is to re-evaluate plan qual in READ COMMITTED mode */
	struct Plan *es_origPlan;
	Pointer		es_evalPlanQual;
	bool	   *es_evTupleNull;
	HeapTuple  *es_evTuple;
	bool		es_useEvalPlan;
} EState;

/* ----------------
 *		Executor Type information needed by plannodes.h
 *
 *|		Note: the bogus classes CommonState and CommonScanState exist only
 *|			  because our inheritance system only allows single inheritance
 *|			  and we have to have unique slot names.  Hence two or more
 *|			  classes which want to have a common slot must ALL inherit
 *|			  the slot from some other class.  (This is a big hack to
 *|			  allow our classes to share slot names..)
 *|
 *|		Example:
 *|			  the class Result and the class NestLoop nodes both want
 *|			  a slot called "OuterTuple" so they both have to inherit
 *|			  it from some other class.  In this case they inherit
 *|			  it from CommonState.	"CommonState" and "CommonScanState" are
 *|			  the best names I could come up with for this sort of
 *|			  stuff.
 *|
 *|			  As a result, many classes have extra slots which they
 *|			  don't use.  These slots are denoted (unused) in the
 *|			  comment preceding the class definition.	If you
 *|			  comes up with a better idea of a way of doing things
 *|			  along these lines, then feel free to make your idea
 *|			  known to me.. -cim 10/15/89
 * ----------------
 */

/* ----------------------------------------------------------------
 *				 Common Executor State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 CommonState information
 *
 *		Superclass for all executor node-state object types.
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's expression-evaluation context
 *		ProjInfo		   info this node uses to form tuple projections
 *		TupFromTlist	   state flag used by some node types (why kept here?)
 * ----------------
 */
typedef struct CommonState
{
	NodeTag		type;			/* its first field is NodeTag */
	TupleTableSlot *cs_OuterTupleSlot;
	TupleTableSlot *cs_ResultTupleSlot;
	ExprContext *cs_ExprContext;
	ProjectionInfo *cs_ProjInfo;
	bool		cs_TupFromTlist;
} CommonState;


/* ----------------------------------------------------------------
 *				 Control Node State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 ResultState information
 *
 *		done			   flag which tells us to quit when we
 *						   have already returned a constant tuple.
 * ----------------
 */
typedef struct ResultState
{
	CommonState cstate;			/* its first field is NodeTag */
	bool		rs_done;
	bool		rs_checkqual;
} ResultState;

/* ----------------
 *	 AppendState information
 *
 *		whichplan		which plan is being executed (0 .. n-1)
 *		firstplan		first plan to execute (usually 0)
 *		lastplan		last plan to execute (usually n-1)
 *		nplans			how many plans are in the list
 *		initialized		array of ExecInitNode() results
 * ----------------
 */
typedef struct AppendState
{
	CommonState cstate;			/* its first field is NodeTag */
	int			as_whichplan;
	int			as_firstplan;
	int			as_lastplan;
	int			as_nplans;
	bool	   *as_initialized;
} AppendState;

/* ----------------------------------------------------------------
 *				 Scan State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 CommonScanState information
 *
 *		CommonScanState extends CommonState for node types that represent
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
typedef struct CommonScanState
{
	CommonState cstate;			/* its first field is NodeTag */
	Relation	css_currentRelation;
	HeapScanDesc css_currentScanDesc;
	TupleTableSlot *css_ScanTupleSlot;
} CommonScanState;

/*
 * SeqScan uses a bare CommonScanState as its state item, since it needs
 * no additional fields.
 */

/* ----------------
 *	 IndexScanState information
 *
 *		Note that an IndexScan node *also* has a CommonScanState state item.
 *		IndexScanState stores the info needed specifically for indexing.
 *		There's probably no good reason why this is a separate node type
 *		rather than an extension of CommonScanState.
 *
 *		NumIndices		   number of indices in this scan
 *		IndexPtr		   current index in use
 *		ScanKeys		   Skey structures to scan index rels
 *		NumScanKeys		   array of no of keys in each Skey struct
 *		RuntimeKeyInfo	   array of array of flags for Skeys evaled at runtime
 *		RuntimeContext	   expr context for evaling runtime Skeys
 *		RuntimeKeysReady   true if runtime Skeys have been computed
 *		RelationDescs	   ptr to array of relation descriptors
 *		ScanDescs		   ptr to array of scan descriptors
 * ----------------
 */
typedef struct IndexScanState
{
	NodeTag		type;
	int			iss_NumIndices;
	int			iss_IndexPtr;
	int			iss_MarkIndexPtr;
	ScanKey    *iss_ScanKeys;
	int		   *iss_NumScanKeys;
	int		  **iss_RuntimeKeyInfo;
	ExprContext *iss_RuntimeContext;
	bool		iss_RuntimeKeysReady;
	RelationPtr iss_RelationDescs;
	IndexScanDescPtr iss_ScanDescs;
} IndexScanState;

/* ----------------
 *	 TidScanState information
 *
 *		Note that a TidScan node *also* has a CommonScanState state item.
 *		There's probably no good reason why this is a separate node type
 *		rather than an extension of CommonScanState.
 *
 *		NumTids		   number of tids in this scan
 *		TidPtr		   current tid in use
 *		TidList		   evaluated item pointers
 * ----------------
 */
typedef struct TidScanState
{
	NodeTag		type;
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
 *		The sub-query will have its own EState, which we save here.
 *		ScanTupleSlot references the current output tuple of the sub-query.
 *
 *		SubEState		   exec state for sub-query
 * ----------------
 */
typedef struct SubqueryScanState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	EState	   *sss_SubEState;
} SubqueryScanState;

/* ----------------
 *	 FunctionScanState information
 *
 *		Function nodes are used to scan the results of a
 *		function appearing in FROM (typically a function returning set).
 *
 *		tupdesc				expected return tuple description
 *		tuplestorestate		private state of tuplestore.c
 *		funcexpr			function expression being evaluated
 * ----------------
 */
typedef struct FunctionScanState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	TupleDesc	tupdesc;
	Tuplestorestate *tuplestorestate;
	Node	   *funcexpr;
} FunctionScanState;

/* ----------------------------------------------------------------
 *				 Join State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 JoinState information
 *
 *		Superclass for state items of join nodes.
 *		Currently this is the same as CommonState.
 * ----------------
 */
typedef CommonState JoinState;

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
	JoinState	jstate;			/* its first field is NodeTag */
	bool		nl_NeedNewOuter;
	bool		nl_MatchedOuter;
	TupleTableSlot *nl_NullInnerTupleSlot;
} NestLoopState;

/* ----------------
 *	 MergeJoinState information
 *
 *		OuterSkipQual	   outerKey1 < innerKey1 ...
 *		InnerSkipQual	   outerKey1 > innerKey1 ...
 *		JoinState		   current "state" of join. see executor.h
 *		MatchedOuter	   true if found a join match for current outer tuple
 *		MatchedInner	   true if found a join match for current inner tuple
 *		OuterTupleSlot	   pointer to slot in tuple table for cur outer tuple
 *		InnerTupleSlot	   pointer to slot in tuple table for cur inner tuple
 *		MarkedTupleSlot    pointer to slot in tuple table for marked tuple
 *		NullOuterTupleSlot prepared null tuple for right outer joins
 *		NullInnerTupleSlot prepared null tuple for left outer joins
 * ----------------
 */
typedef struct MergeJoinState
{
	JoinState	jstate;			/* its first field is NodeTag */
	List	   *mj_OuterSkipQual;
	List	   *mj_InnerSkipQual;
	int			mj_JoinState;
	bool		mj_MatchedOuter;
	bool		mj_MatchedInner;
	TupleTableSlot *mj_OuterTupleSlot;
	TupleTableSlot *mj_InnerTupleSlot;
	TupleTableSlot *mj_MarkedTupleSlot;
	TupleTableSlot *mj_NullOuterTupleSlot;
	TupleTableSlot *mj_NullInnerTupleSlot;
} MergeJoinState;

/* ----------------
 *	 HashJoinState information
 *
 *		hj_HashTable			hash table for the hashjoin
 *		hj_CurBucketNo			bucket# for current outer tuple
 *		hj_CurTuple				last inner tuple matched to current outer
 *								tuple, or NULL if starting search
 *								(CurBucketNo and CurTuple are meaningless
 *								 unless OuterTupleSlot is nonempty!)
 *		hj_InnerHashKey			the inner hash key in the hashjoin condition
 *		hj_OuterTupleSlot		tuple slot for outer tuples
 *		hj_HashTupleSlot		tuple slot for hashed tuples
 *		hj_NullInnerTupleSlot	prepared null tuple for left outer joins
 *		hj_NeedNewOuter			true if need new outer tuple on next call
 *		hj_MatchedOuter			true if found a join match for current outer
 *		hj_hashdone				true if hash-table-build phase is done
 * ----------------
 */
typedef struct HashJoinState
{
	JoinState	jstate;			/* its first field is NodeTag */
	HashJoinTable hj_HashTable;
	int			hj_CurBucketNo;
	HashJoinTuple hj_CurTuple;
	Node	   *hj_InnerHashKey;
	TupleTableSlot *hj_OuterTupleSlot;
	TupleTableSlot *hj_HashTupleSlot;
	TupleTableSlot *hj_NullInnerTupleSlot;
	bool		hj_NeedNewOuter;
	bool		hj_MatchedOuter;
	bool		hj_hashdone;
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
 *		csstate.css_ScanTupleSlot refers to output of underlying plan.
 *
 *		tuplestorestate		private state of tuplestore.c
 * ----------------
 */
typedef struct MaterialState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	void	   *tuplestorestate;
} MaterialState;

/* ---------------------
 *	AggregateState information
 *
 *	csstate.css_ScanTupleSlot refers to output of underlying plan.
 *
 *	Note: the associated ExprContext contains ecxt_aggvalues and ecxt_aggnulls
 *	arrays, which hold the computed agg values for the current input group
 *	during evaluation of an Agg node's output tuple(s).
 * -------------------------
 */
typedef struct AggStatePerAggData *AggStatePerAgg;		/* private in nodeAgg.c */

typedef struct AggState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	List	   *aggs;			/* all Aggref nodes in targetlist & quals */
	int			numaggs;		/* length of list (could be zero!) */
	AggStatePerAgg peragg;		/* per-Aggref working state */
	MemoryContext tup_cxt;		/* context for per-output-tuple
								 * expressions */
	MemoryContext agg_cxt[2];	/* pair of expression eval memory contexts */
	int			which_cxt;		/* 0 or 1, indicates current agg_cxt */
	bool		agg_done;		/* indicates completion of Agg scan */
} AggState;

/* ---------------------
 *	GroupState information
 * -------------------------
 */
typedef struct GroupState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	FmgrInfo   *eqfunctions;	/* per-field lookup data for equality fns */
	bool		grp_useFirstTuple;		/* first tuple not processed yet */
	bool		grp_done;
	HeapTuple	grp_firstTuple;
} GroupState;

/* ----------------
 *	 SortState information
 *
 *		sort_Done		indicates whether sort has been performed yet
 *		tuplesortstate	private state of tuplesort.c
 * ----------------
 */
typedef struct SortState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	bool		sort_Done;
	void	   *tuplesortstate;
} SortState;

/* ----------------
 *	 UniqueState information
 *
 *		Unique nodes are used "on top of" sort nodes to discard
 *		duplicate tuples returned from the sort phase.	Basically
 *		all it does is compare the current tuple from the subplan
 *		with the previously fetched tuple stored in priorTuple.
 *		If the two are identical in all interesting fields, then
 *		we just fetch another tuple from the sort and try again.
 * ----------------
 */
typedef struct UniqueState
{
	CommonState cstate;			/* its first field is NodeTag */
	FmgrInfo   *eqfunctions;	/* per-field lookup data for equality fns */
	HeapTuple	priorTuple;		/* most recently returned tuple, or NULL */
	MemoryContext tempContext;	/* short-term context for comparisons */
} UniqueState;

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
	CommonState cstate;			/* its first field is NodeTag */
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
 * ----------------
 */
typedef struct LimitState
{
	CommonState cstate;			/* its first field is NodeTag */
	long		offset;			/* current OFFSET value */
	long		count;			/* current COUNT, if any */
	long		position;		/* 1-based index of last tuple fetched */
	bool		parmsSet;		/* have we calculated offset/limit yet? */
	bool		noCount;		/* if true, ignore count */
	bool		atEnd;			/* if true, we've reached EOF of subplan */
} LimitState;


/* ----------------
 *	 HashState information
 *
 *		hashtable			hash table for the hashjoin
 * ----------------
 */
typedef struct HashState
{
	CommonState cstate;			/* its first field is NodeTag */
	HashJoinTable hashtable;
} HashState;

#ifdef NOT_USED
/* -----------------------
 *	TeeState information
 *	  leftPlace  :	  next item in the queue unseen by the left parent
 *	  rightPlace :	  next item in the queue unseen by the right parent
 *	  lastPlace  :	  last item in the queue
 *	  bufferRelname :  name of the relation used as the buffer queue
 *	  bufferRel		:  the relation used as the buffer queue
 *	  mcxt			:  for now, tee's have their own memory context
 *					   may be cleaned up later if portals are cleaned up
 *
 * initially, a Tee starts with [left/right]Place variables set to	-1.
 * on cleanup, queue is free'd when both leftPlace and rightPlace = -1
 * -------------------------
*/
typedef struct TeeState
{
	CommonState cstate;			/* its first field is NodeTag */
	int			tee_leftPlace,
				tee_rightPlace,
				tee_lastPlace;
	char	   *tee_bufferRelname;
	Relation	tee_bufferRel;
	MemoryContext tee_mcxt;
	HeapScanDesc tee_leftScanDesc,
				tee_rightScanDesc;
}	TeeState;
#endif

#endif   /* EXECNODES_H */
