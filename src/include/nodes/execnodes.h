/*-------------------------------------------------------------------------
 *
 * execnodes.h--
 *	  definitions for executor state nodes
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execnodes.h,v 1.17 1998/08/03 19:41:31 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECNODES_H
#define EXECNODES_H

#include <nodes/memnodes.h>
#include <nodes/primnodes.h>
#include <executor/hashjoin.h>
#include <access/relscan.h>
#include <access/sdir.h>
#include <nodes/params.h>
#include <executor/tuptable.h>
#include <access/funcindex.h>

/* ----------------
 *	  IndexInfo information
 *
 *		this class holds the information saying what attributes
 *		are the key attributes for this index. -cim 10/15/89
 *
 *		NumKeyAttributes		number of key attributes for this index
 *		KeyAttributeNumbers		array of attribute numbers used as keys
 *		Predicate				partial-index predicate for this index
 * ----------------
 */
typedef struct IndexInfo
{
	NodeTag		type;
	int			ii_NumKeyAttributes;
	AttrNumber *ii_KeyAttributeNumbers;
	FuncIndexInfoPtr ii_FuncIndexInfo;
	Node	   *ii_Predicate;
} IndexInfo;

/* ----------------
 *	  RelationInfo information
 *
 *		whenever we update an existing relation, we have to
 *		update indices on the relation.  The RelationInfo class
 *		is used to hold all the information on result relations,
 *		including indices.. -cim 10/15/89
 *
 *		RangeTableIndex			result relation's range table index
 *		RelationDesc			relation descriptor for result relation
 *		NumIndices				number indices existing on result relation
 *		IndexRelationDescs		array of relation descriptors for indices
 *		IndexRelationInfo		array of key/attr info for indices
 * ----------------
 */
typedef struct RelationInfo
{
	NodeTag		type;
	Index		ri_RangeTableIndex;
	Relation	ri_RelationDesc;
	int			ri_NumIndices;
	RelationPtr ri_IndexRelationDescs;
	IndexInfo **ri_IndexRelationInfo;
} RelationInfo;

/* ----------------
 *	  ExprContext
 *
 *		This class holds the "current context" information
 *		needed to evaluate expressions for doing tuple qualifications
 *		and tuple projections.	For example, if an expression refers
 *		to an attribute in the current inner tuple then we need to know
 *		what the current inner tuple is and so we look at the expression
 *		context.
 * ----------------
 */
typedef struct ExprContext
{
	NodeTag		type;
	TupleTableSlot *ecxt_scantuple;
	TupleTableSlot *ecxt_innertuple;
	TupleTableSlot *ecxt_outertuple;
	Relation	ecxt_relation;
	Index		ecxt_relid;
	ParamListInfo ecxt_param_list_info;
	ParamExecData *ecxt_param_exec_vals;		/* this is for subselects */
	List	   *ecxt_range_table;
	Datum	   *ecxt_values;	/* precomputed values for aggreg */
	char	   *ecxt_nulls;		/* null flags for aggreg  values */
} ExprContext;

/* ----------------
 *		ProjectionInfo node information
 *
 *		This is all the information needed to preform projections
 *		on a tuple.  Nodes which need to do projections create one
 *		of these.  In theory, when a node wants to preform a projection
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
 *	  this class is used to store information regarding junk attributes.
 *	  A junk attribute is an attribute in a tuple that is needed only for
 *	  storing intermediate information in the executor, and does not belong
 *	  in the tuple proper.	For example, when we do a delete or replace
 *	  query, the planner adds an entry to the targetlist so that the tuples
 *	  returned to ExecutePlan() contain an extra attribute: the t_ctid of
 *	  the tuple to be deleted/replaced.  This is needed for amdelete() and
 *	  amreplace().	In doing a delete this does not make much of a
 *	  difference, but in doing a replace we have to make sure we disgard
 *	  all the junk in a tuple before calling amreplace().  Otherwise the
 *	  inserted tuple will not have the correct schema.	This solves a
 *	  problem with hash-join and merge-sort replace plans.	-cim 10/10/90
 *
 *	  targetList:		the original target list (including junk attributes).
 *	  length:			the length of 'targetList'.
 *	  tupType:			the tuple descriptor for the "original" tuple
 *						(including the junk attributes).
 *	  cleanTargetList:	the "clean" target list (junk attributes removed).
 *	  cleanLength:		the length of 'cleanTargetList'
 *	  cleanTupTyp:		the tuple descriptor of the "clean" tuple (with
 *						junk attributes removed).
 *	  cleanMap:			A map with the correspondance between the non junk
 *						attributes of the "original" tuple and the
 *						attributes of the "clean" tuple.
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
} JunkFilter;

/* ----------------
 *	  EState information
 *
 *		direction						direction of the scan
 *
 *		range_table						array of scan relation information
 *
 *		result_relation_information		for update queries
 *
 *		into_relation_descriptor		relation being retrieved "into"
 *
 *		param_list_info					information needed to transform
 *										Param nodes into Const nodes
 *
 *		BaseId							during InitPlan(), each node is
 *										given a number.  this is the next
 *										number to be assigned.
 *
 *		tupleTable						this is a pointer to an array
 *										of pointers to tuples used by
 *										the executor at any given moment.
 *
 *		junkFilter						contains information used to
 *										extract junk attributes from a tuple.
 *										(see JunkFilter above)
 *
 *		refcount						local buffer refcounts used in
 *										an ExecMain cycle.	this is introduced
 *										to avoid ExecStart's unpinning each
 *										other's buffers when called recursively
 * ----------------
 */
typedef struct EState
{
	NodeTag			type;
	ScanDirection	es_direction;
	Snapshot		es_snapshot;
	List		   *es_range_table;
	RelationInfo   *es_result_relation_info;
	Relation		es_into_relation_descriptor;
	ParamListInfo	es_param_list_info;
	ParamExecData  *es_param_exec_vals;	/* this is for subselects */
	int				es_BaseId;
	TupleTable		es_tupleTable;
	JunkFilter	   *es_junkFilter;
	int			   *es_refcount;
	uint32			es_processed;	/* # of tuples processed */
	Oid				es_lastoid;		/* last oid processed (by INSERT) */
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
 *|			  comment preceeding the class definition.	If you
 *|			  comes up with a better idea of a way of doing things
 *|			  along these lines, then feel free to make your idea
 *|			  known to me.. -cim 10/15/89
 * ----------------
 */

/* ----------------------------------------------------------------
 *				 Common Executor State Information
 * ----------------------------------------------------------------
 */

/* BaseNode removed -- base_id moved into CommonState		- jolly */

/* ----------------
 *	 CommonState information
 *
 *|		this is a bogus class used to hold slots so other
 *|		nodes can inherit them...
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 *
 * ----------------
 */
typedef struct CommonState
{
	NodeTag		type;			/* its first field is NodeTag */
	int			cs_base_id;
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
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
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
 *		append nodes have this field "unionplans" which is this
 *		list of plans to execute in sequence..	these variables
 *		keep track of things..
 *
 *		whichplan		which plan is being executed
 *		nplans			how many plans are in the list
 *		initialized		array of ExecInitNode() results
 *		rtentries		range table for the current plan
 *		result_relation_info_list  array of each subplan's result relation info
 *		junkFilter_list  array of each subplan's junk filter
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct AppendState
{
	CommonState cstate;			/* its first field is NodeTag */
	int			as_whichplan;
	int			as_nplans;
	bool	   *as_initialized;
	List	   *as_rtentries;
	List	   *as_result_relation_info_list;
	List	   *as_junkFilter_list;
} AppendState;

/* ----------------------------------------------------------------
 *				 Scan State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 CommonScanState information
 *
 *		CommonScanState is a class like CommonState, but is used more
 *		by the nodes like SeqScan and Sort which want to
 *		keep track of an underlying relation.
 *
 *		currentRelation    relation being scanned
 *		currentScanDesc    current scan descriptor for scan
 *		ScanTupleSlot	   pointer to slot in tuple table holding scan tuple
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct CommonScanState
{
	CommonState cstate;			/* its first field is NodeTag */
	Relation	css_currentRelation;
	HeapScanDesc css_currentScanDesc;
	TupleTableSlot *css_ScanTupleSlot;
} CommonScanState;

/* ----------------
 *	 IndexScanState information
 *
 *|		index scans don't use CommonScanState because
 *|		the underlying AM abstractions for heap scans and
 *|		index scans are too different..  It would be nice
 *|		if the current abstraction was more useful but ... -cim 10/15/89
 *
 *		IndexPtr		   current index in use
 *		NumIndices		   number of indices in this scan
 *		ScanKeys		   Skey structures to scan index rels
 *		NumScanKeys		   array of no of keys in each Skey struct
 *		RuntimeKeyInfo	   array of array of flags for Skeys evaled at runtime
 *		RelationDescs	   ptr to array of relation descriptors
 *		ScanDescs		   ptr to array of scan descriptors
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct IndexScanState
{
	CommonState cstate;			/* its first field is NodeTag */
	int			iss_NumIndices;
	int			iss_IndexPtr;
	int			iss_MarkIndexPtr;
	ScanKey    *iss_ScanKeys;
	int		   *iss_NumScanKeys;
	Pointer		iss_RuntimeKeyInfo;
	RelationPtr iss_RelationDescs;
	IndexScanDescPtr iss_ScanDescs;
} IndexScanState;


/* ----------------------------------------------------------------
 *				 Join State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 JoinState information
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef CommonState JoinState;

/* ----------------
 *	 NestLoopState information
 *
 *		PortalFlag		   Set to enable portals to work.
 *
 *	 JoinState information
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct NestLoopState
{
	JoinState	jstate;			/* its first field is NodeTag */
	bool		nl_PortalFlag;
} NestLoopState;

/* ----------------
 *	 MergeJoinState information
 *
 *		OSortopI		   outerKey1 sortOp innerKey1 ...
 *		ISortopO		   innerkey1 sortOp outerkey1 ...
 *		JoinState		   current "state" of join. see executor.h
 *		MarkedTupleSlot    pointer to slot in tuple table for marked tuple
 *
 *	 JoinState information
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct MergeJoinState
{
	JoinState	jstate;			/* its first field is NodeTag */
	List	   *mj_OSortopI;
	List	   *mj_ISortopO;
	int			mj_JoinState;
	TupleTableSlot *mj_MarkedTupleSlot;
} MergeJoinState;

/* ----------------
 *	 HashJoinState information
 *
 *		hj_HashTable			address of the hash table for the hashjoin
 *		hj_HashTableShmId		shared memory id of hash table
 *		hj_CurBucket			the current hash bucket that we are searching
 *								for matches of the current outer tuple
 *		hj_CurTuple				the current matching inner tuple in the
 *								current hash bucket
 *		hj_CurOTuple			the current matching inner tuple in the
 *								current hash overflow chain
 *		hj_InnerHashKey			the inner hash key in the hashjoin condition
 *		hj_OuterBatches			file descriptors for outer batches
 *		hj_InnerBatches			file descriptors for inner batches
 *		hj_OuterReadPos			current read position of outer batch
 *		hj_OuterReadBlk			current read block of outer batch
 *		hj_OuterTupleSlot		tuple slot for outer tuples
 *		hj_HashTupleSlot		tuple slot for hashed tuples
 *
 *
 *
 *	 JoinState information
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct HashJoinState
{
	JoinState	jstate;			/* its first field is NodeTag */
	HashJoinTable hj_HashTable;
	IpcMemoryId hj_HashTableShmId;
	HashBucket	hj_CurBucket;
	HeapTuple	hj_CurTuple;
	OverflowTuple hj_CurOTuple;
	Var		   *hj_InnerHashKey;
	File	   *hj_OuterBatches;
	File	   *hj_InnerBatches;
	char	   *hj_OuterReadPos;
	int			hj_OuterReadBlk;
	TupleTableSlot *hj_OuterTupleSlot;
	TupleTableSlot *hj_HashTupleSlot;
} HashJoinState;


/* ----------------------------------------------------------------
 *				 Materialization State Information
 * ----------------------------------------------------------------
 */

/* ----------------
 *	 MaterialState information
 *
 *		materialize nodes are used to materialize the results
 *		of a subplan into a temporary relation.
 *
 *		Flag			indicated whether subplan has been materialized
 *		TempRelation	temporary relation containing result of executing
 *						the subplan.
 *
 *	 CommonScanState information
 *
 *		currentRelation    relation descriptor of sorted relation
 *		currentScanDesc    current scan descriptor for scan
 *		ScanTupleSlot	   pointer to slot in tuple table holding scan tuple
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct MaterialState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	bool		mat_Flag;
	Relation	mat_TempRelation;
} MaterialState;

/* ---------------------
 *	AggregateState information
 *
 *		done			indicated whether aggregate has been materialized
 * -------------------------
 */
typedef struct AggState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	bool		agg_done;
} AggState;

/* ---------------------
 *	GroupState information
 *
 * -------------------------
 */
typedef struct GroupState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	bool		grp_useFirstTuple;		/* first tuple not processed yet */
	bool		grp_done;
	HeapTuple	grp_firstTuple;
} GroupState;

/* ----------------
 *	 SortState information
 *
 *|		sort nodes are really just a kind of a scan since
 *|		we implement sorts by retrieveing the entire subplan
 *|		into a temp relation, sorting the temp relation into
 *|		another sorted relation, and then preforming a simple
 *|		unqualified sequential scan on the sorted relation..
 *|		-cim 10/15/89
 *
 *		Flag			indicated whether relation has been sorted
 *		Keys			scan key structures used to keep info on sort keys
 *		TempRelation	temporary relation containing result of executing
 *						the subplan.
 *
 *	 CommonScanState information
 *
 *		currentRelation    relation descriptor of sorted relation
 *		currentScanDesc    current scan descriptor for scan
 *		ScanTupleSlot	   pointer to slot in tuple table holding scan tuple
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct SortState
{
	CommonScanState csstate;	/* its first field is NodeTag */
	bool		sort_Flag;
	ScanKey		sort_Keys;
	bool		cleaned;
} SortState;

/* ----------------
 *	 UniqueState information
 *
 *		Unique nodes are used "on top of" sort nodes to discard
 *		duplicate tuples returned from the sort phase.	Basically
 *		all it does is compare the current tuple from the subplan
 *		with the previously fetched tuple stored in OuterTuple and
 *		if the two are identical, then we just fetch another tuple
 *		from the sort and try again.
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef CommonState UniqueState;


/* ----------------
 *	 HashState information
 *
 *		hashBatches		   file descriptors for the batches
 *
 *	 CommonState information
 *
 *		OuterTupleSlot	   pointer to slot containing current "outer" tuple
 *		ResultTupleSlot    pointer to slot in tuple table for projected tuple
 *		ExprContext		   node's current expression context
 *		ProjInfo		   info this node uses to form tuple projections
 *		NumScanAttributes  size of ScanAttributes array
 *		ScanAttributes	   attribute numbers of interest in this tuple
 * ----------------
 */
typedef struct HashState
{
	CommonState cstate;			/* its first field is NodeTag */
	File	   *hashBatches;
} HashState;

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
	int			tee_leftPlace;
	int			tee_rightPlace;
	int			tee_lastPlace;
	char	   *tee_bufferRelname;
	Relation	tee_bufferRel;
	MemoryContext tee_mcxt;
	HeapScanDesc tee_leftScanDesc;
	HeapScanDesc tee_rightScanDesc;
} TeeState;

#endif							/* EXECNODES_H */
