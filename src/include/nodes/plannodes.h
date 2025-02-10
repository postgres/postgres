/*-------------------------------------------------------------------------
 *
 * plannodes.h
 *	  definitions for query plan nodes
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/plannodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANNODES_H
#define PLANNODES_H

#include "access/sdir.h"
#include "access/stratnum.h"
#include "common/relpath.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/lockoptions.h"
#include "nodes/primnodes.h"


/* ----------------------------------------------------------------
 *						node definitions
 * ----------------------------------------------------------------
 */

/* ----------------
 *		PlannedStmt node
 *
 * The output of the planner is a Plan tree headed by a PlannedStmt node.
 * PlannedStmt holds the "one time" information needed by the executor.
 *
 * For simplicity in APIs, we also wrap utility statements in PlannedStmt
 * nodes; in such cases, commandType == CMD_UTILITY, the statement itself
 * is in the utilityStmt field, and the rest of the struct is mostly dummy.
 * (We do use canSetTag, stmt_location, stmt_len, and possibly queryId.)
 *
 * PlannedStmt, as well as all varieties of Plan, do not support equal(),
 * not because it's not sensible but because we currently have no need.
 * ----------------
 */
typedef struct PlannedStmt
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;

	/* select|insert|update|delete|merge|utility */
	CmdType		commandType;

	/* query identifier (copied from Query) */
	uint64		queryId;

	/* is it insert|update|delete|merge RETURNING? */
	bool		hasReturning;

	/* has insert|update|delete|merge in WITH? */
	bool		hasModifyingCTE;

	/* do I set the command result tag? */
	bool		canSetTag;

	/* redo plan when TransactionXmin changes? */
	bool		transientPlan;

	/* is plan specific to current role? */
	bool		dependsOnRole;

	/* parallel mode required to execute? */
	bool		parallelModeNeeded;

	/* which forms of JIT should be performed */
	int			jitFlags;

	/* tree of Plan nodes */
	struct Plan *planTree;

	/*
	 * List of PartitionPruneInfo contained in the plan
	 */
	List	   *partPruneInfos;

	/* list of RangeTblEntry nodes */
	List	   *rtable;

	/*
	 * RT indexes of relations that are not subject to runtime pruning or are
	 * needed to perform runtime pruning
	 */
	Bitmapset  *unprunableRelids;

	/*
	 * list of RTEPermissionInfo nodes for rtable entries needing one
	 */
	List	   *permInfos;

	/* rtable indexes of target relations for INSERT/UPDATE/DELETE/MERGE */
	/* integer list of RT indexes, or NIL */
	List	   *resultRelations;

	/* list of AppendRelInfo nodes */
	List	   *appendRelations;

	/*
	 * Plan trees for SubPlan expressions; note that some could be NULL
	 */
	List	   *subplans;

	/* indices of subplans that require REWIND */
	Bitmapset  *rewindPlanIDs;

	/* a list of PlanRowMark's */
	List	   *rowMarks;

	/* OIDs of relations the plan depends on */
	List	   *relationOids;

	/* other dependencies, as PlanInvalItems */
	List	   *invalItems;

	/* type OIDs for PARAM_EXEC Params */
	List	   *paramExecTypes;

	/* non-null if this is utility stmt */
	Node	   *utilityStmt;

	/* statement location in source string (copied from Query) */
	/* start location, or -1 if unknown */
	ParseLoc	stmt_location;
	/* length in bytes; 0 means "rest of string" */
	ParseLoc	stmt_len;
} PlannedStmt;

/* macro for fetching the Plan associated with a SubPlan node */
#define exec_subplan_get_plan(plannedstmt, subplan) \
	((Plan *) list_nth((plannedstmt)->subplans, (subplan)->plan_id - 1))


/* ----------------
 *		Plan node
 *
 * All plan nodes "derive" from the Plan structure by having the
 * Plan structure as the first field.  This ensures that everything works
 * when nodes are cast to Plan's.  (node pointers are frequently cast to Plan*
 * when passed around generically in the executor)
 *
 * We never actually instantiate any Plan nodes; this is just the common
 * abstract superclass for all Plan-type nodes.
 * ----------------
 */
typedef struct Plan
{
	pg_node_attr(abstract, no_equal, no_query_jumble)

	NodeTag		type;

	/*
	 * estimated execution costs for plan (see costsize.c for more info)
	 */
	/* count of disabled nodes */
	int			disabled_nodes;
	/* cost expended before fetching any tuples */
	Cost		startup_cost;
	/* total cost (assuming all tuples fetched) */
	Cost		total_cost;

	/*
	 * planner's estimate of result size of this plan step
	 */
	/* number of rows plan is expected to emit */
	Cardinality plan_rows;
	/* average row width in bytes */
	int			plan_width;

	/*
	 * information needed for parallel query
	 */
	/* engage parallel-aware logic? */
	bool		parallel_aware;
	/* OK to use as part of parallel plan? */
	bool		parallel_safe;

	/*
	 * information needed for asynchronous execution
	 */
	/* engage asynchronous-capable logic? */
	bool		async_capable;

	/*
	 * Common structural data for all Plan types.
	 */
	/* unique across entire final plan tree */
	int			plan_node_id;
	/* target list to be computed at this node */
	List	   *targetlist;
	/* implicitly-ANDed qual conditions */
	List	   *qual;
	/* input plan tree(s) */
	struct Plan *lefttree;
	struct Plan *righttree;
	/* Init Plan nodes (un-correlated expr subselects) */
	List	   *initPlan;

	/*
	 * Information for management of parameter-change-driven rescanning
	 *
	 * extParam includes the paramIDs of all external PARAM_EXEC params
	 * affecting this plan node or its children.  setParam params from the
	 * node's initPlans are not included, but their extParams are.
	 *
	 * allParam includes all the extParam paramIDs, plus the IDs of local
	 * params that affect the node (i.e., the setParams of its initplans).
	 * These are _all_ the PARAM_EXEC params that affect this node.
	 */
	Bitmapset  *extParam;
	Bitmapset  *allParam;
} Plan;

/* ----------------
 *	these are defined to avoid confusion problems with "left"
 *	and "right" and "inner" and "outer".  The convention is that
 *	the "left" plan is the "outer" plan and the "right" plan is
 *	the inner plan, but these make the code more readable.
 * ----------------
 */
#define innerPlan(node)			(((Plan *)(node))->righttree)
#define outerPlan(node)			(((Plan *)(node))->lefttree)


/* ----------------
 *	 Result node -
 *		If no outer plan, evaluate a variable-free targetlist.
 *		If outer plan, return tuples from outer plan (after a level of
 *		projection as shown by targetlist).
 *
 * If resconstantqual isn't NULL, it represents a one-time qualification
 * test (i.e., one that doesn't depend on any variables from the outer plan,
 * so needs to be evaluated only once).
 * ----------------
 */
typedef struct Result
{
	Plan		plan;
	Node	   *resconstantqual;
} Result;

/* ----------------
 *	 ProjectSet node -
 *		Apply a projection that includes set-returning functions to the
 *		output tuples of the outer plan.
 * ----------------
 */
typedef struct ProjectSet
{
	Plan		plan;
} ProjectSet;

/* ----------------
 *	 ModifyTable node -
 *		Apply rows produced by outer plan to result table(s),
 *		by inserting, updating, or deleting.
 *
 * If the originally named target table is a partitioned table or inheritance
 * tree, both nominalRelation and rootRelation contain the RT index of the
 * partition root or appendrel RTE, which is not otherwise mentioned in the
 * plan.  Otherwise rootRelation is zero.  However, nominalRelation will
 * always be set, as it's the rel that EXPLAIN should claim is the
 * INSERT/UPDATE/DELETE/MERGE target.
 *
 * Note that rowMarks and epqParam are presumed to be valid for all the
 * table(s); they can't contain any info that varies across tables.
 * ----------------
 */
typedef struct ModifyTable
{
	Plan		plan;
	/* INSERT, UPDATE, DELETE, or MERGE */
	CmdType		operation;
	/* do we set the command tag/es_processed? */
	bool		canSetTag;
	/* Parent RT index for use of EXPLAIN */
	Index		nominalRelation;
	/* Root RT index, if partitioned/inherited */
	Index		rootRelation;
	/* some part key in hierarchy updated? */
	bool		partColsUpdated;
	/* integer list of RT indexes */
	List	   *resultRelations;
	/* per-target-table update_colnos lists */
	List	   *updateColnosLists;
	/* per-target-table WCO lists */
	List	   *withCheckOptionLists;
	/* alias for OLD in RETURNING lists */
	char	   *returningOldAlias;
	/* alias for NEW in RETURNING lists */
	char	   *returningNewAlias;
	/* per-target-table RETURNING tlists */
	List	   *returningLists;
	/* per-target-table FDW private data lists */
	List	   *fdwPrivLists;
	/* indices of FDW DM plans */
	Bitmapset  *fdwDirectModifyPlans;
	/* PlanRowMarks (non-locking only) */
	List	   *rowMarks;
	/* ID of Param for EvalPlanQual re-eval */
	int			epqParam;
	/* ON CONFLICT action */
	OnConflictAction onConflictAction;
	/* List of ON CONFLICT arbiter index OIDs  */
	List	   *arbiterIndexes;
	/* INSERT ON CONFLICT DO UPDATE targetlist */
	List	   *onConflictSet;
	/* target column numbers for onConflictSet */
	List	   *onConflictCols;
	/* WHERE for ON CONFLICT UPDATE */
	Node	   *onConflictWhere;
	/* RTI of the EXCLUDED pseudo relation */
	Index		exclRelRTI;
	/* tlist of the EXCLUDED pseudo relation */
	List	   *exclRelTlist;
	/* per-target-table lists of actions for MERGE */
	List	   *mergeActionLists;
	/* per-target-table join conditions for MERGE */
	List	   *mergeJoinConditions;
} ModifyTable;

struct PartitionPruneInfo;		/* forward reference to struct below */

/* ----------------
 *	 Append node -
 *		Generate the concatenation of the results of sub-plans.
 * ----------------
 */
typedef struct Append
{
	Plan		plan;
	/* RTIs of appendrel(s) formed by this node */
	Bitmapset  *apprelids;
	List	   *appendplans;
	/* # of asynchronous plans */
	int			nasyncplans;

	/*
	 * All 'appendplans' preceding this index are non-partial plans. All
	 * 'appendplans' from this index onwards are partial plans.
	 */
	int			first_partial_plan;

	/*
	 * Index into PlannedStmt.partPruneInfos and parallel lists in EState:
	 * es_part_prune_states and es_part_prune_results. Set to -1 if no
	 * run-time pruning is used.
	 */
	int			part_prune_index;
} Append;

/* ----------------
 *	 MergeAppend node -
 *		Merge the results of pre-sorted sub-plans to preserve the ordering.
 * ----------------
 */
typedef struct MergeAppend
{
	Plan		plan;

	/* RTIs of appendrel(s) formed by this node */
	Bitmapset  *apprelids;

	List	   *mergeplans;

	/* these fields are just like the sort-key info in struct Sort: */

	/* number of sort-key columns */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* OIDs of operators to sort them by */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* OIDs of collations */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST directions */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));

	/*
	 * Index into PlannedStmt.partPruneInfos and parallel lists in EState:
	 * es_part_prune_states and es_part_prune_results. Set to -1 if no
	 * run-time pruning is used.
	 */
	int			part_prune_index;
} MergeAppend;

/* ----------------
 *	RecursiveUnion node -
 *		Generate a recursive union of two subplans.
 *
 * The "outer" subplan is always the non-recursive term, and the "inner"
 * subplan is the recursive term.
 * ----------------
 */
typedef struct RecursiveUnion
{
	Plan		plan;

	/* ID of Param representing work table */
	int			wtParam;

	/* Remaining fields are zero/null in UNION ALL case */

	/* number of columns to check for duplicate-ness */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *dupColIdx pg_node_attr(array_size(numCols));

	/* equality operators to compare with */
	Oid		   *dupOperators pg_node_attr(array_size(numCols));
	Oid		   *dupCollations pg_node_attr(array_size(numCols));

	/* estimated number of groups in input */
	long		numGroups;
} RecursiveUnion;

/* ----------------
 *	 BitmapAnd node -
 *		Generate the intersection of the results of sub-plans.
 *
 * The subplans must be of types that yield tuple bitmaps.  The targetlist
 * and qual fields of the plan are unused and are always NIL.
 * ----------------
 */
typedef struct BitmapAnd
{
	Plan		plan;
	List	   *bitmapplans;
} BitmapAnd;

/* ----------------
 *	 BitmapOr node -
 *		Generate the union of the results of sub-plans.
 *
 * The subplans must be of types that yield tuple bitmaps.  The targetlist
 * and qual fields of the plan are unused and are always NIL.
 * ----------------
 */
typedef struct BitmapOr
{
	Plan		plan;
	bool		isshared;
	List	   *bitmapplans;
} BitmapOr;

/*
 * ==========
 * Scan nodes
 *
 * Scan is an abstract type that all relation scan plan types inherit from.
 * ==========
 */
typedef struct Scan
{
	pg_node_attr(abstract)

	Plan		plan;
	/* relid is index into the range table */
	Index		scanrelid;
} Scan;

/* ----------------
 *		sequential scan node
 * ----------------
 */
typedef struct SeqScan
{
	Scan		scan;
} SeqScan;

/* ----------------
 *		table sample scan node
 * ----------------
 */
typedef struct SampleScan
{
	Scan		scan;
	/* use struct pointer to avoid including parsenodes.h here */
	struct TableSampleClause *tablesample;
} SampleScan;

/* ----------------
 *		index scan node
 *
 * indexqualorig is an implicitly-ANDed list of index qual expressions, each
 * in the same form it appeared in the query WHERE condition.  Each should
 * be of the form (indexkey OP comparisonval) or (comparisonval OP indexkey).
 * The indexkey is a Var or expression referencing column(s) of the index's
 * base table.  The comparisonval might be any expression, but it won't use
 * any columns of the base table.  The expressions are ordered by index
 * column position (but items referencing the same index column can appear
 * in any order).  indexqualorig is used at runtime only if we have to recheck
 * a lossy indexqual.
 *
 * indexqual has the same form, but the expressions have been commuted if
 * necessary to put the indexkeys on the left, and the indexkeys are replaced
 * by Var nodes identifying the index columns (their varno is INDEX_VAR and
 * their varattno is the index column number).
 *
 * indexorderbyorig is similarly the original form of any ORDER BY expressions
 * that are being implemented by the index, while indexorderby is modified to
 * have index column Vars on the left-hand side.  Here, multiple expressions
 * must appear in exactly the ORDER BY order, and this is not necessarily the
 * index column order.  Only the expressions are provided, not the auxiliary
 * sort-order information from the ORDER BY SortGroupClauses; it's assumed
 * that the sort ordering is fully determinable from the top-level operators.
 * indexorderbyorig is used at runtime to recheck the ordering, if the index
 * cannot calculate an accurate ordering.  It is also needed for EXPLAIN.
 *
 * indexorderbyops is a list of the OIDs of the operators used to sort the
 * ORDER BY expressions.  This is used together with indexorderbyorig to
 * recheck ordering at run time.  (Note that indexorderby, indexorderbyorig,
 * and indexorderbyops are used for amcanorderbyop cases, not amcanorder.)
 *
 * indexorderdir specifies the scan ordering, for indexscans on amcanorder
 * indexes (for other indexes it should be "don't care").
 * ----------------
 */
typedef struct IndexScan
{
	Scan		scan;
	/* OID of index to scan */
	Oid			indexid;
	/* list of index quals (usually OpExprs) */
	List	   *indexqual;
	/* the same in original form */
	List	   *indexqualorig;
	/* list of index ORDER BY exprs */
	List	   *indexorderby;
	/* the same in original form */
	List	   *indexorderbyorig;
	/* OIDs of sort ops for ORDER BY exprs */
	List	   *indexorderbyops;
	/* forward or backward or don't care */
	ScanDirection indexorderdir;
} IndexScan;

/* ----------------
 *		index-only scan node
 *
 * IndexOnlyScan is very similar to IndexScan, but it specifies an
 * index-only scan, in which the data comes from the index not the heap.
 * Because of this, *all* Vars in the plan node's targetlist, qual, and
 * index expressions reference index columns and have varno = INDEX_VAR.
 *
 * We could almost use indexqual directly against the index's output tuple
 * when rechecking lossy index operators, but that won't work for quals on
 * index columns that are not retrievable.  Hence, recheckqual is needed
 * for rechecks: it expresses the same condition as indexqual, but using
 * only index columns that are retrievable.  (We will not generate an
 * index-only scan if this is not possible.  An example is that if an
 * index has table column "x" in a retrievable index column "ind1", plus
 * an expression f(x) in a non-retrievable column "ind2", an indexable
 * query on f(x) will use "ind2" in indexqual and f(ind1) in recheckqual.
 * Without the "ind1" column, an index-only scan would be disallowed.)
 *
 * We don't currently need a recheckable equivalent of indexorderby,
 * because we don't support lossy operators in index ORDER BY.
 *
 * To help EXPLAIN interpret the index Vars for display, we provide
 * indextlist, which represents the contents of the index as a targetlist
 * with one TLE per index column.  Vars appearing in this list reference
 * the base table, and this is the only field in the plan node that may
 * contain such Vars.  Also, for the convenience of setrefs.c, TLEs in
 * indextlist are marked as resjunk if they correspond to columns that
 * the index AM cannot reconstruct.
 * ----------------
 */
typedef struct IndexOnlyScan
{
	Scan		scan;
	/* OID of index to scan */
	Oid			indexid;
	/* list of index quals (usually OpExprs) */
	List	   *indexqual;
	/* index quals in recheckable form */
	List	   *recheckqual;
	/* list of index ORDER BY exprs */
	List	   *indexorderby;
	/* TargetEntry list describing index's cols */
	List	   *indextlist;
	/* forward or backward or don't care */
	ScanDirection indexorderdir;
} IndexOnlyScan;

/* ----------------
 *		bitmap index scan node
 *
 * BitmapIndexScan delivers a bitmap of potential tuple locations;
 * it does not access the heap itself.  The bitmap is used by an
 * ancestor BitmapHeapScan node, possibly after passing through
 * intermediate BitmapAnd and/or BitmapOr nodes to combine it with
 * the results of other BitmapIndexScans.
 *
 * The fields have the same meanings as for IndexScan, except we don't
 * store a direction flag because direction is uninteresting.
 *
 * In a BitmapIndexScan plan node, the targetlist and qual fields are
 * not used and are always NIL.  The indexqualorig field is unused at
 * run time too, but is saved for the benefit of EXPLAIN.
 * ----------------
 */
typedef struct BitmapIndexScan
{
	Scan		scan;
	/* OID of index to scan */
	Oid			indexid;
	/* Create shared bitmap if set */
	bool		isshared;
	/* list of index quals (OpExprs) */
	List	   *indexqual;
	/* the same in original form */
	List	   *indexqualorig;
} BitmapIndexScan;

/* ----------------
 *		bitmap sequential scan node
 *
 * This needs a copy of the qual conditions being used by the input index
 * scans because there are various cases where we need to recheck the quals;
 * for example, when the bitmap is lossy about the specific rows on a page
 * that meet the index condition.
 * ----------------
 */
typedef struct BitmapHeapScan
{
	Scan		scan;
	/* index quals, in standard expr form */
	List	   *bitmapqualorig;
} BitmapHeapScan;

/* ----------------
 *		tid scan node
 *
 * tidquals is an implicitly OR'ed list of qual expressions of the form
 * "CTID = pseudoconstant", or "CTID = ANY(pseudoconstant_array)",
 * or a CurrentOfExpr for the relation.
 * ----------------
 */
typedef struct TidScan
{
	Scan		scan;
	/* qual(s) involving CTID = something */
	List	   *tidquals;
} TidScan;

/* ----------------
 *		tid range scan node
 *
 * tidrangequals is an implicitly AND'ed list of qual expressions of the form
 * "CTID relop pseudoconstant", where relop is one of >,>=,<,<=.
 * ----------------
 */
typedef struct TidRangeScan
{
	Scan		scan;
	/* qual(s) involving CTID op something */
	List	   *tidrangequals;
} TidRangeScan;

/* ----------------
 *		subquery scan node
 *
 * SubqueryScan is for scanning the output of a sub-query in the range table.
 * We often need an extra plan node above the sub-query's plan to perform
 * expression evaluations (which we can't push into the sub-query without
 * risking changing its semantics).  Although we are not scanning a physical
 * relation, we make this a descendant of Scan anyway for code-sharing
 * purposes.
 *
 * SubqueryScanStatus caches the trivial_subqueryscan property of the node.
 * SUBQUERY_SCAN_UNKNOWN means not yet determined.  This is only used during
 * planning.
 *
 * Note: we store the sub-plan in the type-specific subplan field, not in
 * the generic lefttree field as you might expect.  This is because we do
 * not want plan-tree-traversal routines to recurse into the subplan without
 * knowing that they are changing Query contexts.
 * ----------------
 */
typedef enum SubqueryScanStatus
{
	SUBQUERY_SCAN_UNKNOWN,
	SUBQUERY_SCAN_TRIVIAL,
	SUBQUERY_SCAN_NONTRIVIAL,
} SubqueryScanStatus;

typedef struct SubqueryScan
{
	Scan		scan;
	Plan	   *subplan;
	SubqueryScanStatus scanstatus;
} SubqueryScan;

/* ----------------
 *		FunctionScan node
 * ----------------
 */
typedef struct FunctionScan
{
	Scan		scan;
	/* list of RangeTblFunction nodes */
	List	   *functions;
	/* WITH ORDINALITY */
	bool		funcordinality;
} FunctionScan;

/* ----------------
 *		ValuesScan node
 * ----------------
 */
typedef struct ValuesScan
{
	Scan		scan;
	/* list of expression lists */
	List	   *values_lists;
} ValuesScan;

/* ----------------
 *		TableFunc scan node
 * ----------------
 */
typedef struct TableFuncScan
{
	Scan		scan;
	/* table function node */
	TableFunc  *tablefunc;
} TableFuncScan;

/* ----------------
 *		CteScan node
 * ----------------
 */
typedef struct CteScan
{
	Scan		scan;
	/* ID of init SubPlan for CTE */
	int			ctePlanId;
	/* ID of Param representing CTE output */
	int			cteParam;
} CteScan;

/* ----------------
 *		NamedTuplestoreScan node
 * ----------------
 */
typedef struct NamedTuplestoreScan
{
	Scan		scan;
	/* Name given to Ephemeral Named Relation */
	char	   *enrname;
} NamedTuplestoreScan;

/* ----------------
 *		WorkTableScan node
 * ----------------
 */
typedef struct WorkTableScan
{
	Scan		scan;
	/* ID of Param representing work table */
	int			wtParam;
} WorkTableScan;

/* ----------------
 *		ForeignScan node
 *
 * fdw_exprs and fdw_private are both under the control of the foreign-data
 * wrapper, but fdw_exprs is presumed to contain expression trees and will
 * be post-processed accordingly by the planner; fdw_private won't be.
 * Note that everything in both lists must be copiable by copyObject().
 * One way to store an arbitrary blob of bytes is to represent it as a bytea
 * Const.  Usually, though, you'll be better off choosing a representation
 * that can be dumped usefully by nodeToString().
 *
 * fdw_scan_tlist is a targetlist describing the contents of the scan tuple
 * returned by the FDW; it can be NIL if the scan tuple matches the declared
 * rowtype of the foreign table, which is the normal case for a simple foreign
 * table scan.  (If the plan node represents a foreign join, fdw_scan_tlist
 * is required since there is no rowtype available from the system catalogs.)
 * When fdw_scan_tlist is provided, Vars in the node's tlist and quals must
 * have varno INDEX_VAR, and their varattnos correspond to resnos in the
 * fdw_scan_tlist (which are also column numbers in the actual scan tuple).
 * fdw_scan_tlist is never actually executed; it just holds expression trees
 * describing what is in the scan tuple's columns.
 *
 * fdw_recheck_quals should contain any quals which the core system passed to
 * the FDW but which were not added to scan.plan.qual; that is, it should
 * contain the quals being checked remotely.  This is needed for correct
 * behavior during EvalPlanQual rechecks.
 *
 * When the plan node represents a foreign join, scan.scanrelid is zero and
 * fs_relids must be consulted to identify the join relation.  (fs_relids
 * is valid for simple scans as well, but will always match scan.scanrelid.)
 * fs_relids includes outer joins; fs_base_relids does not.
 *
 * If the FDW's PlanDirectModify() callback decides to repurpose a ForeignScan
 * node to perform the UPDATE or DELETE operation directly in the remote
 * server, it sets 'operation' and 'resultRelation' to identify the operation
 * type and target relation.  Note that these fields are only set if the
 * modification is performed *fully* remotely; otherwise, the modification is
 * driven by a local ModifyTable node and 'operation' is left to CMD_SELECT.
 * ----------------
 */
typedef struct ForeignScan
{
	Scan		scan;
	/* SELECT/INSERT/UPDATE/DELETE */
	CmdType		operation;
	/* direct modification target's RT index */
	Index		resultRelation;
	/* user to perform the scan as; 0 means to check as current user */
	Oid			checkAsUser;
	/* OID of foreign server */
	Oid			fs_server;
	/* expressions that FDW may evaluate */
	List	   *fdw_exprs;
	/* private data for FDW */
	List	   *fdw_private;
	/* optional tlist describing scan tuple */
	List	   *fdw_scan_tlist;
	/* original quals not in scan.plan.qual */
	List	   *fdw_recheck_quals;
	/* base+OJ RTIs generated by this scan */
	Bitmapset  *fs_relids;
	/* base RTIs generated by this scan */
	Bitmapset  *fs_base_relids;
	/* true if any "system column" is needed */
	bool		fsSystemCol;
} ForeignScan;

/* ----------------
 *	   CustomScan node
 *
 * The comments for ForeignScan's fdw_exprs, fdw_private, fdw_scan_tlist,
 * and fs_relids fields apply equally to CustomScan's custom_exprs,
 * custom_private, custom_scan_tlist, and custom_relids fields.  The
 * convention of setting scan.scanrelid to zero for joins applies as well.
 *
 * Note that since Plan trees can be copied, custom scan providers *must*
 * fit all plan data they need into those fields; embedding CustomScan in
 * a larger struct will not work.
 * ----------------
 */
struct CustomScanMethods;

typedef struct CustomScan
{
	Scan		scan;
	/* mask of CUSTOMPATH_* flags, see nodes/extensible.h */
	uint32		flags;
	/* list of Plan nodes, if any */
	List	   *custom_plans;
	/* expressions that custom code may evaluate */
	List	   *custom_exprs;
	/* private data for custom code */
	List	   *custom_private;
	/* optional tlist describing scan tuple */
	List	   *custom_scan_tlist;
	/* RTIs generated by this scan */
	Bitmapset  *custom_relids;

	/*
	 * NOTE: The method field of CustomScan is required to be a pointer to a
	 * static table of callback functions.  So we don't copy the table itself,
	 * just reference the original one.
	 */
	const struct CustomScanMethods *methods;
} CustomScan;

/*
 * ==========
 * Join nodes
 * ==========
 */

/* ----------------
 *		Join node
 *
 * jointype:	rule for joining tuples from left and right subtrees
 * inner_unique each outer tuple can match to no more than one inner tuple
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
 *
 * inner_unique is set if the joinquals are such that no more than one inner
 * tuple could match any given outer tuple.  This allows the executor to
 * skip searching for additional matches.  (This must be provable from just
 * the joinquals, ignoring plan.qual, due to where the executor tests it.)
 * ----------------
 */
typedef struct Join
{
	pg_node_attr(abstract)

	Plan		plan;
	JoinType	jointype;
	bool		inner_unique;
	/* JOIN quals (in addition to plan.qual) */
	List	   *joinqual;
} Join;

/* ----------------
 *		nest loop join node
 *
 * The nestParams list identifies any executor Params that must be passed
 * into execution of the inner subplan carrying values from the current row
 * of the outer subplan.  Currently we restrict these values to be simple
 * Vars, but perhaps someday that'd be worth relaxing.  (Note: during plan
 * creation, the paramval can actually be a PlaceHolderVar expression; but it
 * must be a Var with varno OUTER_VAR by the time it gets to the executor.)
 * ----------------
 */
typedef struct NestLoop
{
	Join		join;
	/* list of NestLoopParam nodes */
	List	   *nestParams;
} NestLoop;

typedef struct NestLoopParam
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	/* number of the PARAM_EXEC Param to set */
	int			paramno;
	/* outer-relation Var to assign to Param */
	Var		   *paramval;
} NestLoopParam;

/* ----------------
 *		merge join node
 *
 * The expected ordering of each mergeable column is described by a btree
 * opfamily OID, a collation OID, a direction (BTLessStrategyNumber or
 * BTGreaterStrategyNumber) and a nulls-first flag.  Note that the two sides
 * of each mergeclause may be of different datatypes, but they are ordered the
 * same way according to the common opfamily and collation.  The operator in
 * each mergeclause must be an equality operator of the indicated opfamily.
 * ----------------
 */
typedef struct MergeJoin
{
	Join		join;

	/* Can we skip mark/restore calls? */
	bool		skip_mark_restore;

	/* mergeclauses as expression trees */
	List	   *mergeclauses;

	/* these are arrays, but have the same length as the mergeclauses list: */

	/* per-clause OIDs of btree opfamilies */
	Oid		   *mergeFamilies pg_node_attr(array_size(mergeclauses));

	/* per-clause OIDs of collations */
	Oid		   *mergeCollations pg_node_attr(array_size(mergeclauses));

	/* per-clause ordering (ASC or DESC) */
	bool	   *mergeReversals pg_node_attr(array_size(mergeclauses));

	/* per-clause nulls ordering */
	bool	   *mergeNullsFirst pg_node_attr(array_size(mergeclauses));
} MergeJoin;

/* ----------------
 *		hash join node
 * ----------------
 */
typedef struct HashJoin
{
	Join		join;
	List	   *hashclauses;
	List	   *hashoperators;
	List	   *hashcollations;

	/*
	 * List of expressions to be hashed for tuples from the outer plan, to
	 * perform lookups in the hashtable over the inner plan.
	 */
	List	   *hashkeys;
} HashJoin;

/* ----------------
 *		materialization node
 * ----------------
 */
typedef struct Material
{
	Plan		plan;
} Material;

/* ----------------
 *		memoize node
 * ----------------
 */
typedef struct Memoize
{
	Plan		plan;

	/* size of the two arrays below */
	int			numKeys;

	/* hash operators for each key */
	Oid		   *hashOperators pg_node_attr(array_size(numKeys));

	/* collations for each key */
	Oid		   *collations pg_node_attr(array_size(numKeys));

	/* cache keys in the form of exprs containing parameters */
	List	   *param_exprs;

	/*
	 * true if the cache entry should be marked as complete after we store the
	 * first tuple in it.
	 */
	bool		singlerow;

	/*
	 * true when cache key should be compared bit by bit, false when using
	 * hash equality ops
	 */
	bool		binary_mode;

	/*
	 * The maximum number of entries that the planner expects will fit in the
	 * cache, or 0 if unknown
	 */
	uint32		est_entries;

	/* paramids from param_exprs */
	Bitmapset  *keyparamids;
} Memoize;

/* ----------------
 *		sort node
 * ----------------
 */
typedef struct Sort
{
	Plan		plan;

	/* number of sort-key columns */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* OIDs of operators to sort them by */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* OIDs of collations */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST directions */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));
} Sort;

/* ----------------
 *		incremental sort node
 * ----------------
 */
typedef struct IncrementalSort
{
	Sort		sort;
	/* number of presorted columns */
	int			nPresortedCols;
} IncrementalSort;

/* ---------------
 *	 group node -
 *		Used for queries with GROUP BY (but no aggregates) specified.
 *		The input must be presorted according to the grouping columns.
 * ---------------
 */
typedef struct Group
{
	Plan		plan;

	/* number of grouping columns */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *grpColIdx pg_node_attr(array_size(numCols));

	/* equality operators to compare with */
	Oid		   *grpOperators pg_node_attr(array_size(numCols));
	Oid		   *grpCollations pg_node_attr(array_size(numCols));
} Group;

/* ---------------
 *		aggregate node
 *
 * An Agg node implements plain or grouped aggregation.  For grouped
 * aggregation, we can work with presorted input or unsorted input;
 * the latter strategy uses an internal hashtable.
 *
 * Notice the lack of any direct info about the aggregate functions to be
 * computed.  They are found by scanning the node's tlist and quals during
 * executor startup.  (It is possible that there are no aggregate functions;
 * this could happen if they get optimized away by constant-folding, or if
 * we are using the Agg node to implement hash-based grouping.)
 * ---------------
 */
typedef struct Agg
{
	Plan		plan;

	/* basic strategy, see nodes.h */
	AggStrategy aggstrategy;

	/* agg-splitting mode, see nodes.h */
	AggSplit	aggsplit;

	/* number of grouping columns */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *grpColIdx pg_node_attr(array_size(numCols));

	/* equality operators to compare with */
	Oid		   *grpOperators pg_node_attr(array_size(numCols));
	Oid		   *grpCollations pg_node_attr(array_size(numCols));

	/* estimated number of groups in input */
	long		numGroups;

	/* for pass-by-ref transition data */
	uint64		transitionSpace;

	/* IDs of Params used in Aggref inputs */
	Bitmapset  *aggParams;

	/* Note: planner provides numGroups & aggParams only in HASHED/MIXED case */

	/* grouping sets to use */
	List	   *groupingSets;

	/* chained Agg/Sort nodes */
	List	   *chain;
} Agg;

/* ----------------
 *		window aggregate node
 * ----------------
 */
typedef struct WindowAgg
{
	Plan		plan;

	/* ID referenced by window functions */
	Index		winref;

	/* number of columns in partition clause */
	int			partNumCols;

	/* their indexes in the target list */
	AttrNumber *partColIdx pg_node_attr(array_size(partNumCols));

	/* equality operators for partition columns */
	Oid		   *partOperators pg_node_attr(array_size(partNumCols));

	/* collations for partition columns */
	Oid		   *partCollations pg_node_attr(array_size(partNumCols));

	/* number of columns in ordering clause */
	int			ordNumCols;

	/* their indexes in the target list */
	AttrNumber *ordColIdx pg_node_attr(array_size(ordNumCols));

	/* equality operators for ordering columns */
	Oid		   *ordOperators pg_node_attr(array_size(ordNumCols));

	/* collations for ordering columns */
	Oid		   *ordCollations pg_node_attr(array_size(ordNumCols));

	/* frame_clause options, see WindowDef */
	int			frameOptions;

	/* expression for starting bound, if any */
	Node	   *startOffset;

	/* expression for ending bound, if any */
	Node	   *endOffset;

	/* qual to help short-circuit execution */
	List	   *runCondition;

	/* runCondition for display in EXPLAIN */
	List	   *runConditionOrig;

	/* these fields are used with RANGE offset PRECEDING/FOLLOWING: */

	/* in_range function for startOffset */
	Oid			startInRangeFunc;

	/* in_range function for endOffset */
	Oid			endInRangeFunc;

	/* collation for in_range tests */
	Oid			inRangeColl;

	/* use ASC sort order for in_range tests? */
	bool		inRangeAsc;

	/* nulls sort first for in_range tests? */
	bool		inRangeNullsFirst;

	/*
	 * false for all apart from the WindowAgg that's closest to the root of
	 * the plan
	 */
	bool		topWindow;
} WindowAgg;

/* ----------------
 *		unique node
 * ----------------
 */
typedef struct Unique
{
	Plan		plan;

	/* number of columns to check for uniqueness */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *uniqColIdx pg_node_attr(array_size(numCols));

	/* equality operators to compare with */
	Oid		   *uniqOperators pg_node_attr(array_size(numCols));

	/* collations for equality comparisons */
	Oid		   *uniqCollations pg_node_attr(array_size(numCols));
} Unique;

/* ------------
 *		gather node
 *
 * Note: rescan_param is the ID of a PARAM_EXEC parameter slot.  That slot
 * will never actually contain a value, but the Gather node must flag it as
 * having changed whenever it is rescanned.  The child parallel-aware scan
 * nodes are marked as depending on that parameter, so that the rescan
 * machinery is aware that their output is likely to change across rescans.
 * In some cases we don't need a rescan Param, so rescan_param is set to -1.
 * ------------
 */
typedef struct Gather
{
	Plan		plan;
	/* planned number of worker processes */
	int			num_workers;
	/* ID of Param that signals a rescan, or -1 */
	int			rescan_param;
	/* don't execute plan more than once */
	bool		single_copy;
	/* suppress EXPLAIN display (for testing)? */
	bool		invisible;

	/*
	 * param id's of initplans which are referred at gather or one of its
	 * child nodes
	 */
	Bitmapset  *initParam;
} Gather;

/* ------------
 *		gather merge node
 * ------------
 */
typedef struct GatherMerge
{
	Plan		plan;

	/* planned number of worker processes */
	int			num_workers;

	/* ID of Param that signals a rescan, or -1 */
	int			rescan_param;

	/* remaining fields are just like the sort-key info in struct Sort */

	/* number of sort-key columns */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *sortColIdx pg_node_attr(array_size(numCols));

	/* OIDs of operators to sort them by */
	Oid		   *sortOperators pg_node_attr(array_size(numCols));

	/* OIDs of collations */
	Oid		   *collations pg_node_attr(array_size(numCols));

	/* NULLS FIRST/LAST directions */
	bool	   *nullsFirst pg_node_attr(array_size(numCols));

	/*
	 * param id's of initplans which are referred at gather merge or one of
	 * its child nodes
	 */
	Bitmapset  *initParam;
} GatherMerge;

/* ----------------
 *		hash build node
 *
 * If the executor is supposed to try to apply skew join optimization, then
 * skewTable/skewColumn/skewInherit identify the outer relation's join key
 * column, from which the relevant MCV statistics can be fetched.
 * ----------------
 */
typedef struct Hash
{
	Plan		plan;

	/*
	 * List of expressions to be hashed for tuples from Hash's outer plan,
	 * needed to put them into the hashtable.
	 */
	/* hash keys for the hashjoin condition */
	List	   *hashkeys;
	/* outer join key's table OID, or InvalidOid */
	Oid			skewTable;
	/* outer join key's column #, or zero */
	AttrNumber	skewColumn;
	/* is outer join rel an inheritance tree? */
	bool		skewInherit;
	/* all other info is in the parent HashJoin node */
	/* estimate total rows if parallel_aware */
	Cardinality rows_total;
} Hash;

/* ----------------
 *		setop node
 * ----------------
 */
typedef struct SetOp
{
	Plan		plan;

	/* what to do, see nodes.h */
	SetOpCmd	cmd;

	/* how to do it, see nodes.h */
	SetOpStrategy strategy;

	/* number of columns to compare */
	int			numCols;

	/* their indexes in the target list */
	AttrNumber *cmpColIdx pg_node_attr(array_size(numCols));

	/* comparison operators (either equality operators or sort operators) */
	Oid		   *cmpOperators pg_node_attr(array_size(numCols));
	Oid		   *cmpCollations pg_node_attr(array_size(numCols));

	/* nulls-first flags if sorting, otherwise not interesting */
	bool	   *cmpNullsFirst pg_node_attr(array_size(numCols));

	/* estimated number of groups in left input */
	long		numGroups;
} SetOp;

/* ----------------
 *		lock-rows node
 *
 * rowMarks identifies the rels to be locked by this node; it should be
 * a subset of the rowMarks listed in the top-level PlannedStmt.
 * epqParam is a Param that all scan nodes below this one must depend on.
 * It is used to force re-evaluation of the plan during EvalPlanQual.
 * ----------------
 */
typedef struct LockRows
{
	Plan		plan;
	/* a list of PlanRowMark's */
	List	   *rowMarks;
	/* ID of Param for EvalPlanQual re-eval */
	int			epqParam;
} LockRows;

/* ----------------
 *		limit node
 *
 * Note: as of Postgres 8.2, the offset and count expressions are expected
 * to yield int8, rather than int4 as before.
 * ----------------
 */
typedef struct Limit
{
	Plan		plan;

	/* OFFSET parameter, or NULL if none */
	Node	   *limitOffset;

	/* COUNT parameter, or NULL if none */
	Node	   *limitCount;

	/* limit type */
	LimitOption limitOption;

	/* number of columns to check for similarity  */
	int			uniqNumCols;

	/* their indexes in the target list */
	AttrNumber *uniqColIdx pg_node_attr(array_size(uniqNumCols));

	/* equality operators to compare with */
	Oid		   *uniqOperators pg_node_attr(array_size(uniqNumCols));

	/* collations for equality comparisons */
	Oid		   *uniqCollations pg_node_attr(array_size(uniqNumCols));
} Limit;


/*
 * RowMarkType -
 *	  enums for types of row-marking operations
 *
 * The first four of these values represent different lock strengths that
 * we can take on tuples according to SELECT FOR [KEY] UPDATE/SHARE requests.
 * We support these on regular tables, as well as on foreign tables whose FDWs
 * report support for late locking.  For other foreign tables, any locking
 * that might be done for such requests must happen during the initial row
 * fetch; their FDWs provide no mechanism for going back to lock a row later.
 * This means that the semantics will be a bit different than for a local
 * table; in particular we are likely to lock more rows than would be locked
 * locally, since remote rows will be locked even if they then fail
 * locally-checked restriction or join quals.  However, the prospect of
 * doing a separate remote query to lock each selected row is usually pretty
 * unappealing, so early locking remains a credible design choice for FDWs.
 *
 * When doing UPDATE/DELETE/MERGE/SELECT FOR UPDATE/SHARE, we have to uniquely
 * identify all the source rows, not only those from the target relations, so
 * that we can perform EvalPlanQual rechecking at need.  For plain tables we
 * can just fetch the TID, much as for a target relation; this case is
 * represented by ROW_MARK_REFERENCE.  Otherwise (for example for VALUES or
 * FUNCTION scans) we have to copy the whole row value.  ROW_MARK_COPY is
 * pretty inefficient, since most of the time we'll never need the data; but
 * fortunately the overhead is usually not performance-critical in practice.
 * By default we use ROW_MARK_COPY for foreign tables, but if the FDW has
 * a concept of rowid it can request to use ROW_MARK_REFERENCE instead.
 * (Again, this probably doesn't make sense if a physical remote fetch is
 * needed, but for FDWs that map to local storage it might be credible.)
 */
typedef enum RowMarkType
{
	ROW_MARK_EXCLUSIVE,			/* obtain exclusive tuple lock */
	ROW_MARK_NOKEYEXCLUSIVE,	/* obtain no-key exclusive tuple lock */
	ROW_MARK_SHARE,				/* obtain shared tuple lock */
	ROW_MARK_KEYSHARE,			/* obtain keyshare tuple lock */
	ROW_MARK_REFERENCE,			/* just fetch the TID, don't lock it */
	ROW_MARK_COPY,				/* physically copy the row value */
} RowMarkType;

#define RowMarkRequiresRowShareLock(marktype)  ((marktype) <= ROW_MARK_KEYSHARE)

/*
 * PlanRowMark -
 *	   plan-time representation of FOR [KEY] UPDATE/SHARE clauses
 *
 * When doing UPDATE/DELETE/MERGE/SELECT FOR UPDATE/SHARE, we create a separate
 * PlanRowMark node for each non-target relation in the query.  Relations that
 * are not specified as FOR UPDATE/SHARE are marked ROW_MARK_REFERENCE (if
 * regular tables or supported foreign tables) or ROW_MARK_COPY (if not).
 *
 * Initially all PlanRowMarks have rti == prti and isParent == false.
 * When the planner discovers that a relation is the root of an inheritance
 * tree, it sets isParent true, and adds an additional PlanRowMark to the
 * list for each child relation (including the target rel itself in its role
 * as a child, if it is not a partitioned table).  Any non-leaf partitioned
 * child relations will also have entries with isParent = true.  The child
 * entries have rti == child rel's RT index and prti == top parent's RT index,
 * and can therefore be recognized as children by the fact that prti != rti.
 * The parent's allMarkTypes field gets the OR of (1<<markType) across all
 * its children (this definition allows children to use different markTypes).
 *
 * The planner also adds resjunk output columns to the plan that carry
 * information sufficient to identify the locked or fetched rows.  When
 * markType != ROW_MARK_COPY, these columns are named
 *		tableoid%u			OID of table
 *		ctid%u				TID of row
 * The tableoid column is only present for an inheritance hierarchy.
 * When markType == ROW_MARK_COPY, there is instead a single column named
 *		wholerow%u			whole-row value of relation
 * (An inheritance hierarchy could have all three resjunk output columns,
 * if some children use a different markType than others.)
 * In all three cases, %u represents the rowmark ID number (rowmarkId).
 * This number is unique within a plan tree, except that child relation
 * entries copy their parent's rowmarkId.  (Assigning unique numbers
 * means we needn't renumber rowmarkIds when flattening subqueries, which
 * would require finding and renaming the resjunk columns as well.)
 * Note this means that all tables in an inheritance hierarchy share the
 * same resjunk column names.
 */
typedef struct PlanRowMark
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	/* range table index of markable relation */
	Index		rti;
	/* range table index of parent relation */
	Index		prti;
	/* unique identifier for resjunk columns */
	Index		rowmarkId;
	/* see enum above */
	RowMarkType markType;
	/* OR of (1<<markType) for all children */
	int			allMarkTypes;
	/* LockingClause's strength, or LCS_NONE */
	LockClauseStrength strength;
	/* NOWAIT and SKIP LOCKED options */
	LockWaitPolicy waitPolicy;
	/* true if this is a "dummy" parent entry */
	bool		isParent;
} PlanRowMark;


/*
 * Node types to represent partition pruning information.
 */

/*
 * PartitionPruneInfo - Details required to allow the executor to prune
 * partitions.
 *
 * Here we store mapping details to allow translation of a partitioned table's
 * index as returned by the partition pruning code into subplan indexes for
 * plan types which support arbitrary numbers of subplans, such as Append.
 * We also store various details to tell the executor when it should be
 * performing partition pruning.
 *
 * Each PartitionedRelPruneInfo describes the partitioning rules for a single
 * partitioned table (a/k/a level of partitioning).  Since a partitioning
 * hierarchy could contain multiple levels, we represent it by a List of
 * PartitionedRelPruneInfos, where the first entry represents the topmost
 * partitioned table and additional entries represent non-leaf child
 * partitions, ordered such that parents appear before their children.
 * Then, since an Append-type node could have multiple partitioning
 * hierarchies among its children, we have an unordered List of those Lists.
 *
 * relids				RelOptInfo.relids of the parent plan node (e.g. Append
 *						or MergeAppend) to which this PartitionPruneInfo node
 *						belongs.  The pruning logic ensures that this matches
 *						the parent plan node's apprelids.
 * prune_infos			List of Lists containing PartitionedRelPruneInfo nodes,
 *						one sublist per run-time-prunable partition hierarchy
 *						appearing in the parent plan node's subplans.
 * other_subplans		Indexes of any subplans that are not accounted for
 *						by any of the PartitionedRelPruneInfo nodes in
 *						"prune_infos".  These subplans must not be pruned.
 */
typedef struct PartitionPruneInfo
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	Bitmapset  *relids;
	List	   *prune_infos;
	Bitmapset  *other_subplans;
} PartitionPruneInfo;

/*
 * PartitionedRelPruneInfo - Details required to allow the executor to prune
 * partitions for a single partitioned table.
 *
 * subplan_map[], subpart_map[], and leafpart_rti_map[] are indexed by partition
 * index of the partitioned table referenced by 'rtindex', the partition index
 * being the order that the partitions are defined in the table's
 * PartitionDesc.  For a leaf partition p, subplan_map[p] contains the
 * zero-based index of the partition's subplan in the parent plan's subplan
 * list; it is -1 if the partition is non-leaf or has been pruned.  For a
 * non-leaf partition p, subpart_map[p] contains the zero-based index of that
 * sub-partition's PartitionedRelPruneInfo in the hierarchy's
 * PartitionedRelPruneInfo list; it is -1 if the partition is a leaf or has
 * been pruned.  leafpart_rti_map[p] contains the RT index of a leaf partition
 * if its subplan is in the parent plan' subplan list; it is 0 either if the
 * partition is non-leaf or it is leaf but has been pruned during planning.
 * Note that subplan indexes, as stored in 'subplan_map', are global across the
 * parent plan node, but partition indexes are valid only within a particular
 * hierarchy.  relid_map[p] contains the partition's OID, or 0 if the partition
 * was pruned.
 */
typedef struct PartitionedRelPruneInfo
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;

	/* RT index of partition rel for this level */
	Index		rtindex;

	/* Indexes of all partitions which subplans or subparts are present for */
	Bitmapset  *present_parts;

	/* Length of the following arrays: */
	int			nparts;

	/* subplan index by partition index, or -1 */
	int		   *subplan_map pg_node_attr(array_size(nparts));

	/* subpart index by partition index, or -1 */
	int		   *subpart_map pg_node_attr(array_size(nparts));

	/* RT index by partition index, or 0 */
	int		   *leafpart_rti_map pg_node_attr(array_size(nparts));

	/* relation OID by partition index, or 0 */
	Oid		   *relid_map pg_node_attr(array_size(nparts));

	/*
	 * initial_pruning_steps shows how to prune during executor startup (i.e.,
	 * without use of any PARAM_EXEC Params); it is NIL if no startup pruning
	 * is required.  exec_pruning_steps shows how to prune with PARAM_EXEC
	 * Params; it is NIL if no per-scan pruning is required.
	 */
	/* List of PartitionPruneStep */
	List	   *initial_pruning_steps;
	/* List of PartitionPruneStep */
	List	   *exec_pruning_steps;

	/* All PARAM_EXEC Param IDs in exec_pruning_steps */
	Bitmapset  *execparamids;
} PartitionedRelPruneInfo;

/*
 * Abstract Node type for partition pruning steps (there are no concrete
 * Nodes of this type).
 *
 * step_id is the global identifier of the step within its pruning context.
 */
typedef struct PartitionPruneStep
{
	pg_node_attr(abstract, no_equal, no_query_jumble)

	NodeTag		type;
	int			step_id;
} PartitionPruneStep;

/*
 * PartitionPruneStepOp - Information to prune using a set of mutually ANDed
 *							OpExpr clauses
 *
 * This contains information extracted from up to partnatts OpExpr clauses,
 * where partnatts is the number of partition key columns.  'opstrategy' is the
 * strategy of the operator in the clause matched to the last partition key.
 * 'exprs' contains expressions which comprise the lookup key to be passed to
 * the partition bound search function.  'cmpfns' contains the OIDs of
 * comparison functions used to compare aforementioned expressions with
 * partition bounds.  Both 'exprs' and 'cmpfns' contain the same number of
 * items, up to partnatts items.
 *
 * Once we find the offset of a partition bound using the lookup key, we
 * determine which partitions to include in the result based on the value of
 * 'opstrategy'.  For example, if it were equality, we'd return just the
 * partition that would contain that key or a set of partitions if the key
 * didn't consist of all partitioning columns.  For non-equality strategies,
 * we'd need to include other partitions as appropriate.
 *
 * 'nullkeys' is the set containing the offset of the partition keys (0 to
 * partnatts - 1) that were matched to an IS NULL clause.  This is only
 * considered for hash partitioning as we need to pass which keys are null
 * to the hash partition bound search function.  It is never possible to
 * have an expression be present in 'exprs' for a given partition key and
 * the corresponding bit set in 'nullkeys'.
 */
typedef struct PartitionPruneStepOp
{
	PartitionPruneStep step;

	StrategyNumber opstrategy;
	List	   *exprs;
	List	   *cmpfns;
	Bitmapset  *nullkeys;
} PartitionPruneStepOp;

/*
 * PartitionPruneStepCombine - Information to prune using a BoolExpr clause
 *
 * For BoolExpr clauses, we combine the set of partitions determined for each
 * of the argument clauses.
 */
typedef enum PartitionPruneCombineOp
{
	PARTPRUNE_COMBINE_UNION,
	PARTPRUNE_COMBINE_INTERSECT,
} PartitionPruneCombineOp;

typedef struct PartitionPruneStepCombine
{
	PartitionPruneStep step;

	PartitionPruneCombineOp combineOp;
	List	   *source_stepids;
} PartitionPruneStepCombine;


/*
 * Plan invalidation info
 *
 * We track the objects on which a PlannedStmt depends in two ways:
 * relations are recorded as a simple list of OIDs, and everything else
 * is represented as a list of PlanInvalItems.  A PlanInvalItem is designed
 * to be used with the syscache invalidation mechanism, so it identifies a
 * system catalog entry by cache ID and hash value.
 */
typedef struct PlanInvalItem
{
	pg_node_attr(no_equal, no_query_jumble)

	NodeTag		type;
	/* a syscache ID, see utils/syscache.h */
	int			cacheId;
	/* hash value of object's cache lookup key */
	uint32		hashValue;
} PlanInvalItem;

/*
 * MonotonicFunction
 *
 * Allows the planner to track monotonic properties of functions.  A function
 * is monotonically increasing if a subsequent call cannot yield a lower value
 * than the previous call.  A monotonically decreasing function cannot yield a
 * higher value on subsequent calls, and a function which is both must return
 * the same value on each call.
 */
typedef enum MonotonicFunction
{
	MONOTONICFUNC_NONE = 0,
	MONOTONICFUNC_INCREASING = (1 << 0),
	MONOTONICFUNC_DECREASING = (1 << 1),
	MONOTONICFUNC_BOTH = MONOTONICFUNC_INCREASING | MONOTONICFUNC_DECREASING,
} MonotonicFunction;

#endif							/* PLANNODES_H */
