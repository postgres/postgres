/*-------------------------------------------------------------------------
 *
 * plannodes.h
 *	  definitions for query plan nodes
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: plannodes.h,v 1.22 1999/02/13 23:21:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNODES_H
#define PLANNODES_H

#include <nodes/execnodes.h>

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
	Cost		cost;
	int			plan_size;
	int			plan_width;
	int			plan_tupperpage;
	EState	   *state;			/* at execution time, state's of
								 * individual nodes point to one EState
								 * for the whole top-level plan */
	List	   *targetlist;
	List	   *qual;			/* Node* or List* ?? */
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
 *	 result node -
 *		returns tuples from outer plan that satisfy the qualifications
 * ----------------
 */
typedef struct Result
{
	Plan		plan;
	Node	   *resconstantqual;
	ResultState *resstate;
} Result;

/* ----------------
 *		append node
 * ----------------
 */
typedef struct Append
{
	Plan		plan;
	List	   *appendplans;
	List	   *unionrtables;	/* List of range tables, one for each
								 * union query. */
	Index		inheritrelid;	/* The range table has to be changed for
								 * inheritance. */
	List	   *inheritrtable;
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
	IndexScanState *indxstate;
} IndexScan;

/*
 * ==========
 * Join nodes
 * ==========
 */

/* ----------------
 *		Join node
 * ----------------
 */
typedef Plan Join;

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
	Oid			mergejoinop;
	Oid		   *mergerightorder;/* inner sort operator */
	Oid		   *mergeleftorder; /* outer sort operator */
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
	HashJoinTable hashjointable;
	IpcMemoryKey hashjointablekey;
	int			hashjointablesize;
	bool		hashdone;
} HashJoin;

/* ---------------
 *		aggregate node
 * ---------------
 */
typedef struct Agg
{
	Plan		plan;
	List	   *aggs;
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
	AttrNumber *grpColIdx;		/* index into the target list */
	GroupState *grpstate;
} Group;

/*
 * ==========
 * Noname nodes
 * ==========
 */
typedef struct Noname
{
	Plan		plan;
	Oid			nonameid;
	int			keycount;
} Noname;

/* ----------------
 *		materialization node
 * ----------------
 */
typedef struct Material
{
	Plan		plan;			/* noname node flattened out */
	Oid			nonameid;
	int			keycount;
	MaterialState *matstate;
} Material;

/* ----------------
 *		sort node
 * ----------------
 */
typedef struct Sort
{
	Plan		plan;			/* noname node flattened out */
	Oid			nonameid;
	int			keycount;
	SortState  *sortstate;
	void	   *psortstate;
	bool		cleaned;
} Sort;

/* ----------------
 *		unique node
 * ----------------
 */
typedef struct Unique
{
	Plan		plan;			/* noname node flattened out */
	Oid			nonameid;
	int			keycount;
	char	   *uniqueAttr;		/* NULL if all attrs, or unique attribute
								 * name */
	AttrNumber	uniqueAttrNum;	/* attribute number of attribute to select
								 * distinct on */
	UniqueState *uniquestate;
} Unique;

/* ----------------
 *		hash build node
 * ----------------
 */
typedef struct Hash
{
	Plan		plan;
	Var		   *hashkey;
	HashState  *hashstate;
	HashJoinTable hashtable;
	IpcMemoryKey hashtablekey;
	int			hashtablesize;
} Hash;

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
} Tee;

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
	List	   *rtable;			/* range table */
	List	   *setParam;		/* non-correlated EXPR & EXISTS subqueries
								 * have to set some Params for paren Plan */
	List	   *parParam;		/* indices of corr. Vars from parent plan */
	SubLink    *sublink;		/* SubLink node for subselects in WHERE
								 * and HAVING */
	bool		shutdown;		/* shutdown plan if TRUE */
} SubPlan;

#endif	 /* PLANNODES_H */
