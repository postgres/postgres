/*-------------------------------------------------------------------------
 *
 * plannodes.h
 *	  definitions for query plan nodes
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: plannodes.h,v 1.46 2000/11/12 00:37:01 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNODES_H
#define PLANNODES_H

#include "nodes/execnodes.h"

/* ----------------------------------------------------------------
 *	Executor State types are used in the plannode structures
 *	so we have to include their definitions too.
 *
 *		Node Type				node information used by executor
 *
 * control nodes
 *
 *		Result					ResultState				resstate;
 *		Append					AppendState				appendstate;
 *
 * scan nodes
 *
 *		Scan ***				CommonScanState			scanstate;
 *		IndexScan				IndexScanState			indxstate;
 *		SubqueryScan			SubqueryScanState		subquerystate;
 *
 *		  (*** nodes which inherit Scan also inherit scanstate)
 *
 * join nodes
 *
 *		NestLoop				NestLoopState			nlstate;
 *		MergeJoin				MergeJoinState			mergestate;
 *		HashJoin				HashJoinState			hashjoinstate;
 *
 * materialize nodes
 *
 *		Material				MaterialState			matstate;
 *		Sort					SortState				sortstate;
 *		Unique					UniqueState				uniquestate;
 *		SetOp					SetOpState				setopstate;
 *		Limit					LimitState				limitstate;
 *		Hash					HashState				hashstate;
 *
 * ----------------------------------------------------------------
 */


/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		Plan node
 * ----------------
 */

typedef struct Plan
{
	NodeTag		type;

	/* estimated execution costs for plan (see costsize.c for more info) */
	Cost		startup_cost;	/* cost expended before fetching any
								 * tuples */
	Cost		total_cost;		/* total cost (assuming all tuples
								 * fetched) */

	/*
	 * planner's estimate of result size (note: LIMIT, if any, is not
	 * considered in setting plan_rows)
	 */
	double		plan_rows;		/* number of rows plan is expected to emit */
	int			plan_width;		/* average row width in bytes */

	EState	   *state;			/* at execution time, state's of
								 * individual nodes point to one EState
								 * for the whole top-level plan */
	List	   *targetlist;
	List	   *qual;			/* implicitly-ANDed qual conditions */
	struct Plan *lefttree;
	struct Plan *righttree;
	List	   *extParam;		/* indices of _all_ _external_ PARAM_EXEC
								 * for this plan in global
								 * es_param_exec_vals. Params from
								 * setParam from initPlan-s are not
								 * included, but their execParam-s are
								 * here!!! */
	List	   *locParam;		/* someones from setParam-s */
	List	   *chgParam;		/* list of changed ones from the above */
	List	   *initPlan;		/* Init Plan nodes (un-correlated expr
								 * subselects) */
	List	   *subPlan;		/* Other SubPlan nodes */

	/*
	 * We really need in some TopPlan node to store range table and
	 * resultRelation from Query there and get rid of Query itself from
	 * Executor. Some other stuff like below could be put there, too.
	 */
	int			nParamExec;		/* Number of them in entire query. This is
								 * to get Executor know about how many
								 * param_exec there are in query plan. */
} Plan;

/* ----------------
 *	these are are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * ----------------
 */
#define innerPlan(node)			(((Plan *)(node))->righttree)
#define outerPlan(node)			(((Plan *)(node))->lefttree)


/*
 * ===============
 * Top-level nodes
 * ===============
 */

/* all plan nodes "derive" from the Plan structure by having the
   Plan structure as the first field.  This ensures that everything works
   when nodes are cast to Plan's.  (node pointers are frequently cast to Plan*
   when passed around generically in the executor */


/* ----------------
 *	 Result node -
 *		If no outer plan, evaluate a variable-free targetlist.
 *		If outer plan, return tuples from outer plan that satisfy
 *		given quals (we can also do a level of projection)
 * ----------------
 */
typedef struct Result
{
	Plan		plan;
	Node	   *resconstantqual;
	ResultState *resstate;
} Result;

/* ----------------
 *	 Append node -
 *		Generate the concatenation of the results of sub-plans.
 *
 * Append nodes are sometimes used to switch between several result relations
 * (when the target of an UPDATE or DELETE is an inheritance set).  Such a
 * node will have isTarget true.  The Append executor is then responsible
 * for updating the executor state to point at the correct target relation
 * whenever it switches subplans.
 * ----------------
 */
typedef struct Append
{
	Plan		plan;
	List	   *appendplans;
	bool		isTarget;
	AppendState *appendstate;
} Append;

/*
 * ==========
 * Scan nodes
 * ==========
 */
typedef struct Scan
{
	Plan		plan;
	Index		scanrelid;		/* relid is index into the range table */
	CommonScanState *scanstate;
} Scan;

/* ----------------
 *		sequential scan node
 * ----------------
 */
typedef Scan SeqScan;

/* ----------------
 *		index scan node
 * ----------------
 */
typedef struct IndexScan
{
	Scan		scan;
	List	   *indxid;
	List	   *indxqual;
	List	   *indxqualorig;
	ScanDirection indxorderdir;
	IndexScanState *indxstate;
} IndexScan;

/* ----------------
 *				tid scan node
 * ----------------
 */
typedef struct TidScan
{
	Scan		scan;
	bool		needRescan;
	List	   *tideval;
	TidScanState *tidstate;
} TidScan;

/* ----------------
 *		subquery scan node
 *
 * SubqueryScan is for scanning the output of a sub-query in the range table.
 * We need a special plan node above the sub-query's plan as a place to switch
 * execution contexts.  Although we are not scanning a physical relation,
 * we make this a descendant of Scan anyway for code-sharing purposes.
 *
 * Note: we store the sub-plan in the type-specific subplan field, not in
 * the generic lefttree field as you might expect.  This is because we do
 * not want plan-tree-traversal routines to recurse into the subplan without
 * knowing that they are changing Query contexts.
 * ----------------
 */
typedef struct SubqueryScan
{
	Scan		scan;
	Plan	   *subplan;
} SubqueryScan;

/*
 * ==========
 * Join nodes
 * ==========
 */

/* ----------------
 *		Join node
 *
 * jointype:	rule for joining tuples from left and right subtrees
 * joinqual:	qual conditions that came from JOIN/ON or JOIN/USING
 *				(plan.qual contains conditions that came from WHERE)
 *
 * When jointype is INNER, joinqual and plan.qual are semantically
 * interchangeable.  For OUTER jointypes, the two are *not* interchangeable;
 * only joinqual is used to determine whether a match has been found for
 * the purpose of deciding whether to generate null-extended tuples.
 * (But plan.qual is still applied before actually returning a tuple.)
 * For an outer join, only joinquals are allowed to be used as the merge
 * or hash condition of a merge or hash join.
 * ----------------
 */
typedef struct Join
{
	Plan		plan;
	JoinType	jointype;
	List	   *joinqual;		/* JOIN quals (in addition to plan.qual) */
} Join;

/* ----------------
 *		nest loop join node
 * ----------------
 */
typedef struct NestLoop
{
	Join		join;
	NestLoopState *nlstate;
} NestLoop;

/* ----------------
 *		merge join node
 * ----------------
 */
typedef struct MergeJoin
{
	Join		join;
	List	   *mergeclauses;
	MergeJoinState *mergestate;
} MergeJoin;

/* ----------------
 *		hash join (probe) node
 * ----------------
 */
typedef struct HashJoin
{
	Join		join;
	List	   *hashclauses;
	Oid			hashjoinop;
	HashJoinState *hashjoinstate;
} HashJoin;

/* ---------------
 *		aggregate node
 * ---------------
 */
typedef struct Agg
{
	Plan		plan;
	AggState   *aggstate;
} Agg;

/* ---------------
 *	 group node -
 *		use for queries with GROUP BY specified.
 *
 *		If tuplePerGroup is true, one tuple (with group columns only) is
 *		returned for each group and NULL is returned when there are no more
 *		groups. Otherwise, all the tuples of a group are returned with a
 *		NULL returned at the end of each group. (see nodeGroup.c for details)
 * ---------------
 */
typedef struct Group
{
	Plan		plan;
	bool		tuplePerGroup;	/* what tuples to return (see above) */
	int			numCols;		/* number of group columns */
	AttrNumber *grpColIdx;		/* indexes into the target list */
	GroupState *grpstate;
} Group;

/* ----------------
 *		materialization node
 * ----------------
 */
typedef struct Material
{
	Plan		plan;
	MaterialState *matstate;
} Material;

/* ----------------
 *		sort node
 * ----------------
 */
typedef struct Sort
{
	Plan		plan;
	int			keycount;
	SortState  *sortstate;
} Sort;

/* ----------------
 *		unique node
 * ----------------
 */
typedef struct Unique
{
	Plan		plan;
	int			numCols;		/* number of columns to check for
								 * uniqueness */
	AttrNumber *uniqColIdx;		/* indexes into the target list */
	UniqueState *uniquestate;
} Unique;

/* ----------------
 *		setop node
 * ----------------
 */
typedef enum SetOpCmd
{
	SETOPCMD_INTERSECT,
	SETOPCMD_INTERSECT_ALL,
	SETOPCMD_EXCEPT,
	SETOPCMD_EXCEPT_ALL
} SetOpCmd;

typedef struct SetOp
{
	Plan		plan;
	SetOpCmd	cmd;			/* what to do */
	int			numCols;		/* number of columns to check for
								 * duplicate-ness */
	AttrNumber *dupColIdx;		/* indexes into the target list */
	AttrNumber	flagColIdx;
	SetOpState *setopstate;
} SetOp;

/* ----------------
 *		limit node
 * ----------------
 */
typedef struct Limit
{
	Plan		plan;
	Node	   *limitOffset;	/* OFFSET parameter, or NULL if none */
	Node	   *limitCount;		/* COUNT parameter, or NULL if none */
	LimitState *limitstate;
} Limit;

/* ----------------
 *		hash build node
 * ----------------
 */
typedef struct Hash
{
	Plan		plan;
	Node	   *hashkey;
	HashState  *hashstate;
} Hash;

#ifdef NOT_USED
/* -------------------
 *		Tee node information
 *
 *	  leftParent :				the left parent of this node
 *	  rightParent:				the right parent of this node
 * -------------------
*/
typedef struct Tee
{
	Plan		plan;
	Plan	   *leftParent;
	Plan	   *rightParent;
	TeeState   *teestate;
	char	   *teeTableName;	/* the name of the table to materialize
								 * the tee into */
	List	   *rtentries;		/* the range table for the plan below the
								 * Tee may be different than the parent
								 * plans */
}			Tee;

#endif

/* ---------------------
 *		SubPlan node
 * ---------------------
 */
typedef struct SubPlan
{
	NodeTag		type;
	Plan	   *plan;			/* subselect plan itself */
	int			plan_id;		/* dummy thing because of we haven't equal
								 * funcs for plan nodes... actually, we
								 * could put *plan itself somewhere else
								 * (TopPlan node ?)... */
	List	   *rtable;			/* range table for subselect */
	/* setParam and parParam are lists of integers (param IDs) */
	List	   *setParam;		/* non-correlated EXPR & EXISTS subqueries
								 * have to set some Params for paren Plan */
	List	   *parParam;		/* indices of corr. Vars from parent plan */
	SubLink    *sublink;		/* SubLink node from parser; holds info
								 * about what to do with subselect's
								 * results */

	/*
	 * Remaining fields are working state for executor; not used in
	 * planning
	 */
	bool		needShutdown;	/* TRUE = need to shutdown subplan */
	HeapTuple	curTuple;		/* copy of most recent tuple from subplan */
} SubPlan;

#endif	 /* PLANNODES_H */
