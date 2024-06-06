/*-------------------------------------------------------------------------
 *
 * pathnodes.h
 *	  Definitions for planner's internal data structures, especially Paths.
 *
 * We don't support copying RelOptInfo, IndexOptInfo, or Path nodes.
 * There are some subsidiary structs that are useful to copy, though.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/nodes/pathnodes.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PATHNODES_H
#define PATHNODES_H

#include "access/sdir.h"
#include "lib/stringinfo.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "storage/block.h"


/*
 * Relids
 *		Set of relation identifiers (indexes into the rangetable).
 */
typedef Bitmapset *Relids;

/*
 * When looking for a "cheapest path", this enum specifies whether we want
 * cheapest startup cost or cheapest total cost.
 */
typedef enum CostSelector
{
	STARTUP_COST, TOTAL_COST
} CostSelector;

/*
 * The cost estimate produced by cost_qual_eval() includes both a one-time
 * (startup) cost, and a per-tuple cost.
 */
typedef struct QualCost
{
	Cost		startup;		/* one-time cost */
	Cost		per_tuple;		/* per-evaluation cost */
} QualCost;

/*
 * Costing aggregate function execution requires these statistics about
 * the aggregates to be executed by a given Agg node.  Note that the costs
 * include the execution costs of the aggregates' argument expressions as
 * well as the aggregate functions themselves.  Also, the fields must be
 * defined so that initializing the struct to zeroes with memset is correct.
 */
typedef struct AggClauseCosts
{
	QualCost	transCost;		/* total per-input-row execution costs */
	QualCost	finalCost;		/* total per-aggregated-row costs */
	Size		transitionSpace;	/* space for pass-by-ref transition data */
} AggClauseCosts;

/*
 * This enum identifies the different types of "upper" (post-scan/join)
 * relations that we might deal with during planning.
 */
typedef enum UpperRelationKind
{
	UPPERREL_SETOP,				/* result of UNION/INTERSECT/EXCEPT, if any */
	UPPERREL_PARTIAL_GROUP_AGG, /* result of partial grouping/aggregation, if
								 * any */
	UPPERREL_GROUP_AGG,			/* result of grouping/aggregation, if any */
	UPPERREL_WINDOW,			/* result of window functions, if any */
	UPPERREL_PARTIAL_DISTINCT,	/* result of partial "SELECT DISTINCT", if any */
	UPPERREL_DISTINCT,			/* result of "SELECT DISTINCT", if any */
	UPPERREL_ORDERED,			/* result of ORDER BY, if any */
	UPPERREL_FINAL,				/* result of any remaining top-level actions */
	/* NB: UPPERREL_FINAL must be last enum entry; it's used to size arrays */
} UpperRelationKind;

/*----------
 * PlannerGlobal
 *		Global information for planning/optimization
 *
 * PlannerGlobal holds state for an entire planner invocation; this state
 * is shared across all levels of sub-Queries that exist in the command being
 * planned.
 *
 * Not all fields are printed.  (In some cases, there is no print support for
 * the field type; in others, doing so would lead to infinite recursion.)
 *----------
 */
typedef struct PlannerGlobal
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* Param values provided to planner() */
	ParamListInfo boundParams pg_node_attr(read_write_ignore);

	/* Plans for SubPlan nodes */
	List	   *subplans;

	/* Paths from which the SubPlan Plans were made */
	List	   *subpaths;

	/* PlannerInfos for SubPlan nodes */
	List	   *subroots pg_node_attr(read_write_ignore);

	/* indices of subplans that require REWIND */
	Bitmapset  *rewindPlanIDs;

	/* "flat" rangetable for executor */
	List	   *finalrtable;

	/* "flat" list of RTEPermissionInfos */
	List	   *finalrteperminfos;

	/* "flat" list of PlanRowMarks */
	List	   *finalrowmarks;

	/* "flat" list of integer RT indexes */
	List	   *resultRelations;

	/* "flat" list of AppendRelInfos */
	List	   *appendRelations;

	/* OIDs of relations the plan depends on */
	List	   *relationOids;

	/* other dependencies, as PlanInvalItems */
	List	   *invalItems;

	/* type OIDs for PARAM_EXEC Params */
	List	   *paramExecTypes;

	/* highest PlaceHolderVar ID assigned */
	Index		lastPHId;

	/* highest PlanRowMark ID assigned */
	Index		lastRowMarkId;

	/* highest plan node ID assigned */
	int			lastPlanNodeId;

	/* redo plan when TransactionXmin changes? */
	bool		transientPlan;

	/* is plan specific to current role? */
	bool		dependsOnRole;

	/* parallel mode potentially OK? */
	bool		parallelModeOK;

	/* parallel mode actually required? */
	bool		parallelModeNeeded;

	/* worst PROPARALLEL hazard level */
	char		maxParallelHazard;

	/* partition descriptors */
	PartitionDirectory partition_directory pg_node_attr(read_write_ignore);
} PlannerGlobal;

/* macro for fetching the Plan associated with a SubPlan node */
#define planner_subplan_get_plan(root, subplan) \
	((Plan *) list_nth((root)->glob->subplans, (subplan)->plan_id - 1))


/*----------
 * PlannerInfo
 *		Per-query information for planning/optimization
 *
 * This struct is conventionally called "root" in all the planner routines.
 * It holds links to all of the planner's working state, in addition to the
 * original Query.  Note that at present the planner extensively modifies
 * the passed-in Query data structure; someday that should stop.
 *
 * For reasons explained in optimizer/optimizer.h, we define the typedef
 * either here or in that header, whichever is read first.
 *
 * Not all fields are printed.  (In some cases, there is no print support for
 * the field type; in others, doing so would lead to infinite recursion or
 * bloat dump output more than seems useful.)
 *----------
 */
#ifndef HAVE_PLANNERINFO_TYPEDEF
typedef struct PlannerInfo PlannerInfo;
#define HAVE_PLANNERINFO_TYPEDEF 1
#endif

struct PlannerInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* the Query being planned */
	Query	   *parse;

	/* global info for current planner run */
	PlannerGlobal *glob;

	/* 1 at the outermost Query */
	Index		query_level;

	/* NULL at outermost Query */
	PlannerInfo *parent_root pg_node_attr(read_write_ignore);

	/*
	 * plan_params contains the expressions that this query level needs to
	 * make available to a lower query level that is currently being planned.
	 * outer_params contains the paramIds of PARAM_EXEC Params that outer
	 * query levels will make available to this query level.
	 */
	/* list of PlannerParamItems, see below */
	List	   *plan_params;
	Bitmapset  *outer_params;

	/*
	 * simple_rel_array holds pointers to "base rels" and "other rels" (see
	 * comments for RelOptInfo for more info).  It is indexed by rangetable
	 * index (so entry 0 is always wasted).  Entries can be NULL when an RTE
	 * does not correspond to a base relation, such as a join RTE or an
	 * unreferenced view RTE; or if the RelOptInfo hasn't been made yet.
	 */
	struct RelOptInfo **simple_rel_array pg_node_attr(array_size(simple_rel_array_size));
	/* allocated size of array */
	int			simple_rel_array_size;

	/*
	 * simple_rte_array is the same length as simple_rel_array and holds
	 * pointers to the associated rangetable entries.  Using this is a shade
	 * faster than using rt_fetch(), mostly due to fewer indirections.  (Not
	 * printed because it'd be redundant with parse->rtable.)
	 */
	RangeTblEntry **simple_rte_array pg_node_attr(read_write_ignore);

	/*
	 * append_rel_array is the same length as the above arrays, and holds
	 * pointers to the corresponding AppendRelInfo entry indexed by
	 * child_relid, or NULL if the rel is not an appendrel child.  The array
	 * itself is not allocated if append_rel_list is empty.  (Not printed
	 * because it'd be redundant with append_rel_list.)
	 */
	struct AppendRelInfo **append_rel_array pg_node_attr(read_write_ignore);

	/*
	 * all_baserels is a Relids set of all base relids (but not joins or
	 * "other" rels) in the query.  This is computed in deconstruct_jointree.
	 */
	Relids		all_baserels;

	/*
	 * outer_join_rels is a Relids set of all outer-join relids in the query.
	 * This is computed in deconstruct_jointree.
	 */
	Relids		outer_join_rels;

	/*
	 * all_query_rels is a Relids set of all base relids and outer join relids
	 * (but not "other" relids) in the query.  This is the Relids identifier
	 * of the final join we need to form.  This is computed in
	 * deconstruct_jointree.
	 */
	Relids		all_query_rels;

	/*
	 * join_rel_list is a list of all join-relation RelOptInfos we have
	 * considered in this planning run.  For small problems we just scan the
	 * list to do lookups, but when there are many join relations we build a
	 * hash table for faster lookups.  The hash table is present and valid
	 * when join_rel_hash is not NULL.  Note that we still maintain the list
	 * even when using the hash table for lookups; this simplifies life for
	 * GEQO.
	 */
	List	   *join_rel_list;
	struct HTAB *join_rel_hash pg_node_attr(read_write_ignore);

	/*
	 * When doing a dynamic-programming-style join search, join_rel_level[k]
	 * is a list of all join-relation RelOptInfos of level k, and
	 * join_cur_level is the current level.  New join-relation RelOptInfos are
	 * automatically added to the join_rel_level[join_cur_level] list.
	 * join_rel_level is NULL if not in use.
	 *
	 * Note: we've already printed all baserel and joinrel RelOptInfos above,
	 * so we don't dump join_rel_level or other lists of RelOptInfos.
	 */
	/* lists of join-relation RelOptInfos */
	List	  **join_rel_level pg_node_attr(read_write_ignore);
	/* index of list being extended */
	int			join_cur_level;

	/* init SubPlans for query */
	List	   *init_plans;

	/*
	 * per-CTE-item list of subplan IDs (or -1 if no subplan was made for that
	 * CTE)
	 */
	List	   *cte_plan_ids;

	/* List of Lists of Params for MULTIEXPR subquery outputs */
	List	   *multiexpr_params;

	/* list of JoinDomains used in the query (higher ones first) */
	List	   *join_domains;

	/* list of active EquivalenceClasses */
	List	   *eq_classes;

	/* set true once ECs are canonical */
	bool		ec_merging_done;

	/* list of "canonical" PathKeys */
	List	   *canon_pathkeys;

	/*
	 * list of OuterJoinClauseInfos for mergejoinable outer join clauses
	 * w/nonnullable var on left
	 */
	List	   *left_join_clauses;

	/*
	 * list of OuterJoinClauseInfos for mergejoinable outer join clauses
	 * w/nonnullable var on right
	 */
	List	   *right_join_clauses;

	/*
	 * list of OuterJoinClauseInfos for mergejoinable full join clauses
	 */
	List	   *full_join_clauses;

	/* list of SpecialJoinInfos */
	List	   *join_info_list;

	/* counter for assigning RestrictInfo serial numbers */
	int			last_rinfo_serial;

	/*
	 * all_result_relids is empty for SELECT, otherwise it contains at least
	 * parse->resultRelation.  For UPDATE/DELETE/MERGE across an inheritance
	 * or partitioning tree, the result rel's child relids are added.  When
	 * using multi-level partitioning, intermediate partitioned rels are
	 * included. leaf_result_relids is similar except that only actual result
	 * tables, not partitioned tables, are included in it.
	 */
	/* set of all result relids */
	Relids		all_result_relids;
	/* set of all leaf relids */
	Relids		leaf_result_relids;

	/*
	 * list of AppendRelInfos
	 *
	 * Note: for AppendRelInfos describing partitions of a partitioned table,
	 * we guarantee that partitions that come earlier in the partitioned
	 * table's PartitionDesc will appear earlier in append_rel_list.
	 */
	List	   *append_rel_list;

	/* list of RowIdentityVarInfos */
	List	   *row_identity_vars;

	/* list of PlanRowMarks */
	List	   *rowMarks;

	/* list of PlaceHolderInfos */
	List	   *placeholder_list;

	/* array of PlaceHolderInfos indexed by phid */
	struct PlaceHolderInfo **placeholder_array pg_node_attr(read_write_ignore, array_size(placeholder_array_size));
	/* allocated size of array */
	int			placeholder_array_size pg_node_attr(read_write_ignore);

	/* list of ForeignKeyOptInfos */
	List	   *fkey_list;

	/* desired pathkeys for query_planner() */
	List	   *query_pathkeys;

	/* groupClause pathkeys, if any */
	List	   *group_pathkeys;

	/*
	 * The number of elements in the group_pathkeys list which belong to the
	 * GROUP BY clause.  Additional ones belong to ORDER BY / DISTINCT
	 * aggregates.
	 */
	int			num_groupby_pathkeys;

	/* pathkeys of bottom window, if any */
	List	   *window_pathkeys;
	/* distinctClause pathkeys, if any */
	List	   *distinct_pathkeys;
	/* sortClause pathkeys, if any */
	List	   *sort_pathkeys;
	/* set operator pathkeys, if any */
	List	   *setop_pathkeys;

	/* Canonicalised partition schemes used in the query. */
	List	   *part_schemes pg_node_attr(read_write_ignore);

	/* RelOptInfos we are now trying to join */
	List	   *initial_rels pg_node_attr(read_write_ignore);

	/*
	 * Upper-rel RelOptInfos. Use fetch_upper_rel() to get any particular
	 * upper rel.
	 */
	List	   *upper_rels[UPPERREL_FINAL + 1] pg_node_attr(read_write_ignore);

	/* Result tlists chosen by grouping_planner for upper-stage processing */
	struct PathTarget *upper_targets[UPPERREL_FINAL + 1] pg_node_attr(read_write_ignore);

	/*
	 * The fully-processed groupClause is kept here.  It differs from
	 * parse->groupClause in that we remove any items that we can prove
	 * redundant, so that only the columns named here actually need to be
	 * compared to determine grouping.  Note that it's possible for *all* the
	 * items to be proven redundant, implying that there is only one group
	 * containing all the query's rows.  Hence, if you want to check whether
	 * GROUP BY was specified, test for nonempty parse->groupClause, not for
	 * nonempty processed_groupClause.  Optimizer chooses specific order of
	 * group-by clauses during the upper paths generation process, attempting
	 * to use different strategies to minimize number of sorts or engage
	 * incremental sort.  See preprocess_groupclause() and
	 * get_useful_group_keys_orderings() for details.
	 *
	 * Currently, when grouping sets are specified we do not attempt to
	 * optimize the groupClause, so that processed_groupClause will be
	 * identical to parse->groupClause.
	 */
	List	   *processed_groupClause;

	/*
	 * The fully-processed distinctClause is kept here.  It differs from
	 * parse->distinctClause in that we remove any items that we can prove
	 * redundant, so that only the columns named here actually need to be
	 * compared to determine uniqueness.  Note that it's possible for *all*
	 * the items to be proven redundant, implying that there should be only
	 * one output row.  Hence, if you want to check whether DISTINCT was
	 * specified, test for nonempty parse->distinctClause, not for nonempty
	 * processed_distinctClause.
	 */
	List	   *processed_distinctClause;

	/*
	 * The fully-processed targetlist is kept here.  It differs from
	 * parse->targetList in that (for INSERT) it's been reordered to match the
	 * target table, and defaults have been filled in.  Also, additional
	 * resjunk targets may be present.  preprocess_targetlist() does most of
	 * that work, but note that more resjunk targets can get added during
	 * appendrel expansion.  (Hence, upper_targets mustn't get set up till
	 * after that.)
	 */
	List	   *processed_tlist;

	/*
	 * For UPDATE, this list contains the target table's attribute numbers to
	 * which the first N entries of processed_tlist are to be assigned.  (Any
	 * additional entries in processed_tlist must be resjunk.)  DO NOT use the
	 * resnos in processed_tlist to identify the UPDATE target columns.
	 */
	List	   *update_colnos;

	/*
	 * Fields filled during create_plan() for use in setrefs.c
	 */
	/* for GroupingFunc fixup (can't print: array length not known here) */
	AttrNumber *grouping_map pg_node_attr(read_write_ignore);
	/* List of MinMaxAggInfos */
	List	   *minmax_aggs;

	/* context holding PlannerInfo */
	MemoryContext planner_cxt pg_node_attr(read_write_ignore);

	/* # of pages in all non-dummy tables of query */
	Cardinality total_table_pages;

	/* tuple_fraction passed to query_planner */
	Selectivity tuple_fraction;
	/* limit_tuples passed to query_planner */
	Cardinality limit_tuples;

	/*
	 * Minimum security_level for quals. Note: qual_security_level is zero if
	 * there are no securityQuals.
	 */
	Index		qual_security_level;

	/* true if any RTEs are RTE_JOIN kind */
	bool		hasJoinRTEs;
	/* true if any RTEs are marked LATERAL */
	bool		hasLateralRTEs;
	/* true if havingQual was non-null */
	bool		hasHavingQual;
	/* true if any RestrictInfo has pseudoconstant = true */
	bool		hasPseudoConstantQuals;
	/* true if we've made any of those */
	bool		hasAlternativeSubPlans;
	/* true once we're no longer allowed to add PlaceHolderInfos */
	bool		placeholdersFrozen;
	/* true if planning a recursive WITH item */
	bool		hasRecursion;

	/*
	 * Information about aggregates. Filled by preprocess_aggrefs().
	 */
	/* AggInfo structs */
	List	   *agginfos;
	/* AggTransInfo structs */
	List	   *aggtransinfos;
	/* number of aggs with DISTINCT/ORDER BY/WITHIN GROUP */
	int			numOrderedAggs;
	/* does any agg not support partial mode? */
	bool		hasNonPartialAggs;
	/* is any partial agg non-serializable? */
	bool		hasNonSerialAggs;

	/*
	 * These fields are used only when hasRecursion is true:
	 */
	/* PARAM_EXEC ID for the work table */
	int			wt_param_id;
	/* a path for non-recursive term */
	struct Path *non_recursive_path;

	/*
	 * These fields are workspace for createplan.c
	 */
	/* outer rels above current node */
	Relids		curOuterRels;
	/* not-yet-assigned NestLoopParams */
	List	   *curOuterParams;

	/*
	 * These fields are workspace for setrefs.c.  Each is an array
	 * corresponding to glob->subplans.  (We could probably teach
	 * gen_node_support.pl how to determine the array length, but it doesn't
	 * seem worth the trouble, so just mark them read_write_ignore.)
	 */
	bool	   *isAltSubplan pg_node_attr(read_write_ignore);
	bool	   *isUsedSubplan pg_node_attr(read_write_ignore);

	/* optional private data for join_search_hook, e.g., GEQO */
	void	   *join_search_private pg_node_attr(read_write_ignore);

	/* Does this query modify any partition key columns? */
	bool		partColsUpdated;
};


/*
 * In places where it's known that simple_rte_array[] must have been prepared
 * already, we just index into it to fetch RTEs.  In code that might be
 * executed before or after entering query_planner(), use this macro.
 */
#define planner_rt_fetch(rti, root) \
	((root)->simple_rte_array ? (root)->simple_rte_array[rti] : \
	 rt_fetch(rti, (root)->parse->rtable))

/*
 * If multiple relations are partitioned the same way, all such partitions
 * will have a pointer to the same PartitionScheme.  A list of PartitionScheme
 * objects is attached to the PlannerInfo.  By design, the partition scheme
 * incorporates only the general properties of the partition method (LIST vs.
 * RANGE, number of partitioning columns and the type information for each)
 * and not the specific bounds.
 *
 * We store the opclass-declared input data types instead of the partition key
 * datatypes since the former rather than the latter are used to compare
 * partition bounds. Since partition key data types and the opclass declared
 * input data types are expected to be binary compatible (per ResolveOpClass),
 * both of those should have same byval and length properties.
 */
typedef struct PartitionSchemeData
{
	char		strategy;		/* partition strategy */
	int16		partnatts;		/* number of partition attributes */
	Oid		   *partopfamily;	/* OIDs of operator families */
	Oid		   *partopcintype;	/* OIDs of opclass declared input data types */
	Oid		   *partcollation;	/* OIDs of partitioning collations */

	/* Cached information about partition key data types. */
	int16	   *parttyplen;
	bool	   *parttypbyval;

	/* Cached information about partition comparison functions. */
	struct FmgrInfo *partsupfunc;
}			PartitionSchemeData;

typedef struct PartitionSchemeData *PartitionScheme;

/*----------
 * RelOptInfo
 *		Per-relation information for planning/optimization
 *
 * For planning purposes, a "base rel" is either a plain relation (a table)
 * or the output of a sub-SELECT or function that appears in the range table.
 * In either case it is uniquely identified by an RT index.  A "joinrel"
 * is the joining of two or more base rels.  A joinrel is identified by
 * the set of RT indexes for its component baserels, along with RT indexes
 * for any outer joins it has computed.  We create RelOptInfo nodes for each
 * baserel and joinrel, and store them in the PlannerInfo's simple_rel_array
 * and join_rel_list respectively.
 *
 * Note that there is only one joinrel for any given set of component
 * baserels, no matter what order we assemble them in; so an unordered
 * set is the right datatype to identify it with.
 *
 * We also have "other rels", which are like base rels in that they refer to
 * single RT indexes; but they are not part of the join tree, and are given
 * a different RelOptKind to identify them.
 * Currently the only kind of otherrels are those made for member relations
 * of an "append relation", that is an inheritance set or UNION ALL subquery.
 * An append relation has a parent RTE that is a base rel, which represents
 * the entire append relation.  The member RTEs are otherrels.  The parent
 * is present in the query join tree but the members are not.  The member
 * RTEs and otherrels are used to plan the scans of the individual tables or
 * subqueries of the append set; then the parent baserel is given Append
 * and/or MergeAppend paths comprising the best paths for the individual
 * member rels.  (See comments for AppendRelInfo for more information.)
 *
 * At one time we also made otherrels to represent join RTEs, for use in
 * handling join alias Vars.  Currently this is not needed because all join
 * alias Vars are expanded to non-aliased form during preprocess_expression.
 *
 * We also have relations representing joins between child relations of
 * different partitioned tables. These relations are not added to
 * join_rel_level lists as they are not joined directly by the dynamic
 * programming algorithm.
 *
 * There is also a RelOptKind for "upper" relations, which are RelOptInfos
 * that describe post-scan/join processing steps, such as aggregation.
 * Many of the fields in these RelOptInfos are meaningless, but their Path
 * fields always hold Paths showing ways to do that processing step.
 *
 * Parts of this data structure are specific to various scan and join
 * mechanisms.  It didn't seem worth creating new node types for them.
 *
 *		relids - Set of relation identifiers (RT indexes).  This is a base
 *				 relation if there is just one, a join relation if more;
 *				 in the join case, RT indexes of any outer joins formed
 *				 at or below this join are included along with baserels
 *		rows - estimated number of tuples in the relation after restriction
 *			   clauses have been applied (ie, output rows of a plan for it)
 *		consider_startup - true if there is any value in keeping plain paths for
 *						   this rel on the basis of having cheap startup cost
 *		consider_param_startup - the same for parameterized paths
 *		reltarget - Default Path output tlist for this rel; normally contains
 *					Var and PlaceHolderVar nodes for the values we need to
 *					output from this relation.
 *					List is in no particular order, but all rels of an
 *					appendrel set must use corresponding orders.
 *					NOTE: in an appendrel child relation, may contain
 *					arbitrary expressions pulled up from a subquery!
 *		pathlist - List of Path nodes, one for each potentially useful
 *				   method of generating the relation
 *		ppilist - ParamPathInfo nodes for parameterized Paths, if any
 *		cheapest_startup_path - the pathlist member with lowest startup cost
 *			(regardless of ordering) among the unparameterized paths;
 *			or NULL if there is no unparameterized path
 *		cheapest_total_path - the pathlist member with lowest total cost
 *			(regardless of ordering) among the unparameterized paths;
 *			or if there is no unparameterized path, the path with lowest
 *			total cost among the paths with minimum parameterization
 *		cheapest_unique_path - for caching cheapest path to produce unique
 *			(no duplicates) output from relation; NULL if not yet requested
 *		cheapest_parameterized_paths - best paths for their parameterizations;
 *			always includes cheapest_total_path, even if that's unparameterized
 *		direct_lateral_relids - rels this rel has direct LATERAL references to
 *		lateral_relids - required outer rels for LATERAL, as a Relids set
 *			(includes both direct and indirect lateral references)
 *
 * If the relation is a base relation it will have these fields set:
 *
 *		relid - RTE index (this is redundant with the relids field, but
 *				is provided for convenience of access)
 *		rtekind - copy of RTE's rtekind field
 *		min_attr, max_attr - range of valid AttrNumbers for rel
 *		attr_needed - array of bitmapsets indicating the highest joinrel
 *				in which each attribute is needed; if bit 0 is set then
 *				the attribute is needed as part of final targetlist
 *		attr_widths - cache space for per-attribute width estimates;
 *					  zero means not computed yet
 *		nulling_relids - relids of outer joins that can null this rel
 *		lateral_vars - lateral cross-references of rel, if any (list of
 *					   Vars and PlaceHolderVars)
 *		lateral_referencers - relids of rels that reference this one laterally
 *				(includes both direct and indirect lateral references)
 *		indexlist - list of IndexOptInfo nodes for relation's indexes
 *					(always NIL if it's not a table or partitioned table)
 *		pages - number of disk pages in relation (zero if not a table)
 *		tuples - number of tuples in relation (not considering restrictions)
 *		allvisfrac - fraction of disk pages that are marked all-visible
 *		eclass_indexes - EquivalenceClasses that mention this rel (filled
 *						 only after EC merging is complete)
 *		subroot - PlannerInfo for subquery (NULL if it's not a subquery)
 *		subplan_params - list of PlannerParamItems to be passed to subquery
 *
 *		Note: for a subquery, tuples and subroot are not set immediately
 *		upon creation of the RelOptInfo object; they are filled in when
 *		set_subquery_pathlist processes the object.
 *
 *		For otherrels that are appendrel members, these fields are filled
 *		in just as for a baserel, except we don't bother with lateral_vars.
 *
 * If the relation is either a foreign table or a join of foreign tables that
 * all belong to the same foreign server and are assigned to the same user to
 * check access permissions as (cf checkAsUser), these fields will be set:
 *
 *		serverid - OID of foreign server, if foreign table (else InvalidOid)
 *		userid - OID of user to check access as (InvalidOid means current user)
 *		useridiscurrent - we've assumed that userid equals current user
 *		fdwroutine - function hooks for FDW, if foreign table (else NULL)
 *		fdw_private - private state for FDW, if foreign table (else NULL)
 *
 * Two fields are used to cache knowledge acquired during the join search
 * about whether this rel is provably unique when being joined to given other
 * relation(s), ie, it can have at most one row matching any given row from
 * that join relation.  Currently we only attempt such proofs, and thus only
 * populate these fields, for base rels; but someday they might be used for
 * join rels too:
 *
 *		unique_for_rels - list of Relid sets, each one being a set of other
 *					rels for which this one has been proven unique
 *		non_unique_for_rels - list of Relid sets, each one being a set of
 *					other rels for which we have tried and failed to prove
 *					this one unique
 *
 * The presence of the following fields depends on the restrictions
 * and joins that the relation participates in:
 *
 *		baserestrictinfo - List of RestrictInfo nodes, containing info about
 *					each non-join qualification clause in which this relation
 *					participates (only used for base rels)
 *		baserestrictcost - Estimated cost of evaluating the baserestrictinfo
 *					clauses at a single tuple (only used for base rels)
 *		baserestrict_min_security - Smallest security_level found among
 *					clauses in baserestrictinfo
 *		joininfo  - List of RestrictInfo nodes, containing info about each
 *					join clause in which this relation participates (but
 *					note this excludes clauses that might be derivable from
 *					EquivalenceClasses)
 *		has_eclass_joins - flag that EquivalenceClass joins are possible
 *
 * Note: Keeping a restrictinfo list in the RelOptInfo is useful only for
 * base rels, because for a join rel the set of clauses that are treated as
 * restrict clauses varies depending on which sub-relations we choose to join.
 * (For example, in a 3-base-rel join, a clause relating rels 1 and 2 must be
 * treated as a restrictclause if we join {1} and {2 3} to make {1 2 3}; but
 * if we join {1 2} and {3} then that clause will be a restrictclause in {1 2}
 * and should not be processed again at the level of {1 2 3}.)	Therefore,
 * the restrictinfo list in the join case appears in individual JoinPaths
 * (field joinrestrictinfo), not in the parent relation.  But it's OK for
 * the RelOptInfo to store the joininfo list, because that is the same
 * for a given rel no matter how we form it.
 *
 * We store baserestrictcost in the RelOptInfo (for base relations) because
 * we know we will need it at least once (to price the sequential scan)
 * and may need it multiple times to price index scans.
 *
 * A join relation is considered to be partitioned if it is formed from a
 * join of two relations that are partitioned, have matching partitioning
 * schemes, and are joined on an equijoin of the partitioning columns.
 * Under those conditions we can consider the join relation to be partitioned
 * by either relation's partitioning keys, though some care is needed if
 * either relation can be forced to null by outer-joining.  For example, an
 * outer join like (A LEFT JOIN B ON A.a = B.b) may produce rows with B.b
 * NULL.  These rows may not fit the partitioning conditions imposed on B.
 * Hence, strictly speaking, the join is not partitioned by B.b and thus
 * partition keys of an outer join should include partition key expressions
 * from the non-nullable side only.  However, if a subsequent join uses
 * strict comparison operators (and all commonly-used equijoin operators are
 * strict), the presence of nulls doesn't cause a problem: such rows couldn't
 * match anything on the other side and thus they don't create a need to do
 * any cross-partition sub-joins.  Hence we can treat such values as still
 * partitioning the join output for the purpose of additional partitionwise
 * joining, so long as a strict join operator is used by the next join.
 *
 * If the relation is partitioned, these fields will be set:
 *
 *		part_scheme - Partitioning scheme of the relation
 *		nparts - Number of partitions
 *		boundinfo - Partition bounds
 *		partbounds_merged - true if partition bounds are merged ones
 *		partition_qual - Partition constraint if not the root
 *		part_rels - RelOptInfos for each partition
 *		all_partrels - Relids set of all partition relids
 *		partexprs, nullable_partexprs - Partition key expressions
 *
 * The partexprs and nullable_partexprs arrays each contain
 * part_scheme->partnatts elements.  Each of the elements is a list of
 * partition key expressions.  For partitioned base relations, there is one
 * expression in each partexprs element, and nullable_partexprs is empty.
 * For partitioned join relations, each base relation within the join
 * contributes one partition key expression per partitioning column;
 * that expression goes in the partexprs[i] list if the base relation
 * is not nullable by this join or any lower outer join, or in the
 * nullable_partexprs[i] list if the base relation is nullable.
 * Furthermore, FULL JOINs add extra nullable_partexprs expressions
 * corresponding to COALESCE expressions of the left and right join columns,
 * to simplify matching join clauses to those lists.
 *
 * Not all fields are printed.  (In some cases, there is no print support for
 * the field type.)
 *----------
 */

/* Bitmask of flags supported by table AMs */
#define AMFLAG_HAS_TID_RANGE (1 << 0)

typedef enum RelOptKind
{
	RELOPT_BASEREL,
	RELOPT_JOINREL,
	RELOPT_OTHER_MEMBER_REL,
	RELOPT_OTHER_JOINREL,
	RELOPT_UPPER_REL,
	RELOPT_OTHER_UPPER_REL,
} RelOptKind;

/*
 * Is the given relation a simple relation i.e a base or "other" member
 * relation?
 */
#define IS_SIMPLE_REL(rel) \
	((rel)->reloptkind == RELOPT_BASEREL || \
	 (rel)->reloptkind == RELOPT_OTHER_MEMBER_REL)

/* Is the given relation a join relation? */
#define IS_JOIN_REL(rel)	\
	((rel)->reloptkind == RELOPT_JOINREL || \
	 (rel)->reloptkind == RELOPT_OTHER_JOINREL)

/* Is the given relation an upper relation? */
#define IS_UPPER_REL(rel)	\
	((rel)->reloptkind == RELOPT_UPPER_REL || \
	 (rel)->reloptkind == RELOPT_OTHER_UPPER_REL)

/* Is the given relation an "other" relation? */
#define IS_OTHER_REL(rel) \
	((rel)->reloptkind == RELOPT_OTHER_MEMBER_REL || \
	 (rel)->reloptkind == RELOPT_OTHER_JOINREL || \
	 (rel)->reloptkind == RELOPT_OTHER_UPPER_REL)

typedef struct RelOptInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	RelOptKind	reloptkind;

	/*
	 * all relations included in this RelOptInfo; set of base + OJ relids
	 * (rangetable indexes)
	 */
	Relids		relids;

	/*
	 * size estimates generated by planner
	 */
	/* estimated number of result tuples */
	Cardinality rows;

	/*
	 * per-relation planner control flags
	 */
	/* keep cheap-startup-cost paths? */
	bool		consider_startup;
	/* ditto, for parameterized paths? */
	bool		consider_param_startup;
	/* consider parallel paths? */
	bool		consider_parallel;

	/*
	 * default result targetlist for Paths scanning this relation; list of
	 * Vars/Exprs, cost, width
	 */
	struct PathTarget *reltarget;

	/*
	 * materialization information
	 */
	List	   *pathlist;		/* Path structures */
	List	   *ppilist;		/* ParamPathInfos used in pathlist */
	List	   *partial_pathlist;	/* partial Paths */
	struct Path *cheapest_startup_path;
	struct Path *cheapest_total_path;
	struct Path *cheapest_unique_path;
	List	   *cheapest_parameterized_paths;

	/*
	 * parameterization information needed for both base rels and join rels
	 * (see also lateral_vars and lateral_referencers)
	 */
	/* rels directly laterally referenced */
	Relids		direct_lateral_relids;
	/* minimum parameterization of rel */
	Relids		lateral_relids;

	/*
	 * information about a base rel (not set for join rels!)
	 */
	Index		relid;
	/* containing tablespace */
	Oid			reltablespace;
	/* RELATION, SUBQUERY, FUNCTION, etc */
	RTEKind		rtekind;
	/* smallest attrno of rel (often <0) */
	AttrNumber	min_attr;
	/* largest attrno of rel */
	AttrNumber	max_attr;
	/* array indexed [min_attr .. max_attr] */
	Relids	   *attr_needed pg_node_attr(read_write_ignore);
	/* array indexed [min_attr .. max_attr] */
	int32	   *attr_widths pg_node_attr(read_write_ignore);

	/*
	 * Zero-based set containing attnums of NOT NULL columns.  Not populated
	 * for rels corresponding to non-partitioned inh==true RTEs.
	 */
	Bitmapset  *notnullattnums;
	/* relids of outer joins that can null this baserel */
	Relids		nulling_relids;
	/* LATERAL Vars and PHVs referenced by rel */
	List	   *lateral_vars;
	/* rels that reference this baserel laterally */
	Relids		lateral_referencers;
	/* list of IndexOptInfo */
	List	   *indexlist;
	/* list of StatisticExtInfo */
	List	   *statlist;
	/* size estimates derived from pg_class */
	BlockNumber pages;
	Cardinality tuples;
	double		allvisfrac;
	/* indexes in PlannerInfo's eq_classes list of ECs that mention this rel */
	Bitmapset  *eclass_indexes;
	PlannerInfo *subroot;		/* if subquery */
	List	   *subplan_params; /* if subquery */
	/* wanted number of parallel workers */
	int			rel_parallel_workers;
	/* Bitmask of optional features supported by the table AM */
	uint32		amflags;

	/*
	 * Information about foreign tables and foreign joins
	 */
	/* identifies server for the table or join */
	Oid			serverid;
	/* identifies user to check access as; 0 means to check as current user */
	Oid			userid;
	/* join is only valid for current user */
	bool		useridiscurrent;
	/* use "struct FdwRoutine" to avoid including fdwapi.h here */
	struct FdwRoutine *fdwroutine pg_node_attr(read_write_ignore);
	void	   *fdw_private pg_node_attr(read_write_ignore);

	/*
	 * cache space for remembering if we have proven this relation unique
	 */
	/* known unique for these other relid set(s) */
	List	   *unique_for_rels;
	/* known not unique for these set(s) */
	List	   *non_unique_for_rels;

	/*
	 * used by various scans and joins:
	 */
	/* RestrictInfo structures (if base rel) */
	List	   *baserestrictinfo;
	/* cost of evaluating the above */
	QualCost	baserestrictcost;
	/* min security_level found in baserestrictinfo */
	Index		baserestrict_min_security;
	/* RestrictInfo structures for join clauses involving this rel */
	List	   *joininfo;
	/* T means joininfo is incomplete */
	bool		has_eclass_joins;

	/*
	 * used by partitionwise joins:
	 */
	/* consider partitionwise join paths? (if partitioned rel) */
	bool		consider_partitionwise_join;

	/*
	 * inheritance links, if this is an otherrel (otherwise NULL):
	 */
	/* Immediate parent relation (dumping it would be too verbose) */
	struct RelOptInfo *parent pg_node_attr(read_write_ignore);
	/* Topmost parent relation (dumping it would be too verbose) */
	struct RelOptInfo *top_parent pg_node_attr(read_write_ignore);
	/* Relids of topmost parent (redundant, but handy) */
	Relids		top_parent_relids;

	/*
	 * used for partitioned relations:
	 */
	/* Partitioning scheme */
	PartitionScheme part_scheme pg_node_attr(read_write_ignore);

	/*
	 * Number of partitions; -1 if not yet set; in case of a join relation 0
	 * means it's considered unpartitioned
	 */
	int			nparts;
	/* Partition bounds */
	struct PartitionBoundInfoData *boundinfo pg_node_attr(read_write_ignore);
	/* True if partition bounds were created by partition_bounds_merge() */
	bool		partbounds_merged;
	/* Partition constraint, if not the root */
	List	   *partition_qual;

	/*
	 * Array of RelOptInfos of partitions, stored in the same order as bounds
	 * (don't print, too bulky and duplicative)
	 */
	struct RelOptInfo **part_rels pg_node_attr(read_write_ignore);

	/*
	 * Bitmap with members acting as indexes into the part_rels[] array to
	 * indicate which partitions survived partition pruning.
	 */
	Bitmapset  *live_parts;
	/* Relids set of all partition relids */
	Relids		all_partrels;

	/*
	 * These arrays are of length partkey->partnatts, which we don't have at
	 * hand, so don't try to print
	 */

	/* Non-nullable partition key expressions */
	List	  **partexprs pg_node_attr(read_write_ignore);
	/* Nullable partition key expressions */
	List	  **nullable_partexprs pg_node_attr(read_write_ignore);
} RelOptInfo;

/*
 * Is given relation partitioned?
 *
 * It's not enough to test whether rel->part_scheme is set, because it might
 * be that the basic partitioning properties of the input relations matched
 * but the partition bounds did not.  Also, if we are able to prove a rel
 * dummy (empty), we should henceforth treat it as unpartitioned.
 */
#define IS_PARTITIONED_REL(rel) \
	((rel)->part_scheme && (rel)->boundinfo && (rel)->nparts > 0 && \
	 (rel)->part_rels && !IS_DUMMY_REL(rel))

/*
 * Convenience macro to make sure that a partitioned relation has all the
 * required members set.
 */
#define REL_HAS_ALL_PART_PROPS(rel)	\
	((rel)->part_scheme && (rel)->boundinfo && (rel)->nparts > 0 && \
	 (rel)->part_rels && (rel)->partexprs && (rel)->nullable_partexprs)

/*
 * IndexOptInfo
 *		Per-index information for planning/optimization
 *
 *		indexkeys[], indexcollations[] each have ncolumns entries.
 *		opfamily[], and opcintype[]	each have nkeycolumns entries. They do
 *		not contain any information about included attributes.
 *
 *		sortopfamily[], reverse_sort[], and nulls_first[] have
 *		nkeycolumns entries, if the index is ordered; but if it is unordered,
 *		those pointers are NULL.
 *
 *		Zeroes in the indexkeys[] array indicate index columns that are
 *		expressions; there is one element in indexprs for each such column.
 *
 *		For an ordered index, reverse_sort[] and nulls_first[] describe the
 *		sort ordering of a forward indexscan; we can also consider a backward
 *		indexscan, which will generate the reverse ordering.
 *
 *		The indexprs and indpred expressions have been run through
 *		prepqual.c and eval_const_expressions() for ease of matching to
 *		WHERE clauses. indpred is in implicit-AND form.
 *
 *		indextlist is a TargetEntry list representing the index columns.
 *		It provides an equivalent base-relation Var for each simple column,
 *		and links to the matching indexprs element for each expression column.
 *
 *		While most of these fields are filled when the IndexOptInfo is created
 *		(by plancat.c), indrestrictinfo and predOK are set later, in
 *		check_index_predicates().
 */
#ifndef HAVE_INDEXOPTINFO_TYPEDEF
typedef struct IndexOptInfo IndexOptInfo;
#define HAVE_INDEXOPTINFO_TYPEDEF 1
#endif

struct IndexOptInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* OID of the index relation */
	Oid			indexoid;
	/* tablespace of index (not table) */
	Oid			reltablespace;
	/* back-link to index's table; don't print, else infinite recursion */
	RelOptInfo *rel pg_node_attr(read_write_ignore);

	/*
	 * index-size statistics (from pg_class and elsewhere)
	 */
	/* number of disk pages in index */
	BlockNumber pages;
	/* number of index tuples in index */
	Cardinality tuples;
	/* index tree height, or -1 if unknown */
	int			tree_height;

	/*
	 * index descriptor information
	 */
	/* number of columns in index */
	int			ncolumns;
	/* number of key columns in index */
	int			nkeycolumns;

	/*
	 * table column numbers of index's columns (both key and included
	 * columns), or 0 for expression columns
	 */
	int		   *indexkeys pg_node_attr(array_size(ncolumns));
	/* OIDs of collations of index columns */
	Oid		   *indexcollations pg_node_attr(array_size(nkeycolumns));
	/* OIDs of operator families for columns */
	Oid		   *opfamily pg_node_attr(array_size(nkeycolumns));
	/* OIDs of opclass declared input data types */
	Oid		   *opcintype pg_node_attr(array_size(nkeycolumns));
	/* OIDs of btree opfamilies, if orderable.  NULL if partitioned index */
	Oid		   *sortopfamily pg_node_attr(array_size(nkeycolumns));
	/* is sort order descending? or NULL if partitioned index */
	bool	   *reverse_sort pg_node_attr(array_size(nkeycolumns));
	/* do NULLs come first in the sort order? or NULL if partitioned index */
	bool	   *nulls_first pg_node_attr(array_size(nkeycolumns));
	/* opclass-specific options for columns */
	bytea	  **opclassoptions pg_node_attr(read_write_ignore);
	/* which index cols can be returned in an index-only scan? */
	bool	   *canreturn pg_node_attr(array_size(ncolumns));
	/* OID of the access method (in pg_am) */
	Oid			relam;

	/*
	 * expressions for non-simple index columns; redundant to print since we
	 * print indextlist
	 */
	List	   *indexprs pg_node_attr(read_write_ignore);
	/* predicate if a partial index, else NIL */
	List	   *indpred;

	/* targetlist representing index columns */
	List	   *indextlist;

	/*
	 * parent relation's baserestrictinfo list, less any conditions implied by
	 * the index's predicate (unless it's a target rel, see comments in
	 * check_index_predicates())
	 */
	List	   *indrestrictinfo;

	/* true if index predicate matches query */
	bool		predOK;
	/* true if a unique index */
	bool		unique;
	/* is uniqueness enforced immediately? */
	bool		immediate;
	/* true if index doesn't really exist */
	bool		hypothetical;

	/*
	 * Remaining fields are copied from the index AM's API struct
	 * (IndexAmRoutine).  These fields are not set for partitioned indexes.
	 */
	bool		amcanorderbyop;
	bool		amoptionalkey;
	bool		amsearcharray;
	bool		amsearchnulls;
	/* does AM have amgettuple interface? */
	bool		amhasgettuple;
	/* does AM have amgetbitmap interface? */
	bool		amhasgetbitmap;
	bool		amcanparallel;
	/* does AM have ammarkpos interface? */
	bool		amcanmarkpos;
	/* AM's cost estimator */
	/* Rather than include amapi.h here, we declare amcostestimate like this */
	void		(*amcostestimate) () pg_node_attr(read_write_ignore);
};

/*
 * ForeignKeyOptInfo
 *		Per-foreign-key information for planning/optimization
 *
 * The per-FK-column arrays can be fixed-size because we allow at most
 * INDEX_MAX_KEYS columns in a foreign key constraint.  Each array has
 * nkeys valid entries.
 */
typedef struct ForeignKeyOptInfo
{
	pg_node_attr(custom_read_write, no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/*
	 * Basic data about the foreign key (fetched from catalogs):
	 */

	/* RT index of the referencing table */
	Index		con_relid;
	/* RT index of the referenced table */
	Index		ref_relid;
	/* number of columns in the foreign key */
	int			nkeys;
	/* cols in referencing table */
	AttrNumber	conkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* cols in referenced table */
	AttrNumber	confkey[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));
	/* PK = FK operator OIDs */
	Oid			conpfeqop[INDEX_MAX_KEYS] pg_node_attr(array_size(nkeys));

	/*
	 * Derived info about whether FK's equality conditions match the query:
	 */

	/* # of FK cols matched by ECs */
	int			nmatched_ec;
	/* # of these ECs that are ec_has_const */
	int			nconst_ec;
	/* # of FK cols matched by non-EC rinfos */
	int			nmatched_rcols;
	/* total # of non-EC rinfos matched to FK */
	int			nmatched_ri;
	/* Pointer to eclass matching each column's condition, if there is one */
	struct EquivalenceClass *eclass[INDEX_MAX_KEYS];
	/* Pointer to eclass member for the referencing Var, if there is one */
	struct EquivalenceMember *fk_eclass_member[INDEX_MAX_KEYS];
	/* List of non-EC RestrictInfos matching each column's condition */
	List	   *rinfos[INDEX_MAX_KEYS];
} ForeignKeyOptInfo;

/*
 * StatisticExtInfo
 *		Information about extended statistics for planning/optimization
 *
 * Each pg_statistic_ext row is represented by one or more nodes of this
 * type, or even zero if ANALYZE has not computed them.
 */
typedef struct StatisticExtInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* OID of the statistics row */
	Oid			statOid;

	/* includes child relations */
	bool		inherit;

	/* back-link to statistic's table; don't print, else infinite recursion */
	RelOptInfo *rel pg_node_attr(read_write_ignore);

	/* statistics kind of this entry */
	char		kind;

	/* attnums of the columns covered */
	Bitmapset  *keys;

	/* expressions */
	List	   *exprs;
} StatisticExtInfo;

/*
 * JoinDomains
 *
 * A "join domain" defines the scope of applicability of deductions made via
 * the EquivalenceClass mechanism.  Roughly speaking, a join domain is a set
 * of base+OJ relations that are inner-joined together.  More precisely, it is
 * the set of relations at which equalities deduced from an EquivalenceClass
 * can be enforced or should be expected to hold.  The topmost JoinDomain
 * covers the whole query (so its jd_relids should equal all_query_rels).
 * An outer join creates a new JoinDomain that includes all base+OJ relids
 * within its nullable side, but (by convention) not the OJ's own relid.
 * A FULL join creates two new JoinDomains, one for each side.
 *
 * Notice that a rel that is below outer join(s) will thus appear to belong
 * to multiple join domains.  However, any of its Vars that appear in
 * EquivalenceClasses belonging to higher join domains will have nullingrel
 * bits preventing them from being evaluated at the rel's scan level, so that
 * we will not be able to derive enforceable-at-the-rel-scan-level clauses
 * from such ECs.  We define the join domain relid sets this way so that
 * domains can be said to be "higher" or "lower" when one domain relid set
 * includes another.
 *
 * The JoinDomains for a query are computed in deconstruct_jointree.
 * We do not copy JoinDomain structs once made, so they can be compared
 * for equality by simple pointer equality.
 */
typedef struct JoinDomain
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Relids		jd_relids;		/* all relids contained within the domain */
} JoinDomain;

/*
 * EquivalenceClasses
 *
 * Whenever we identify a mergejoinable equality clause A = B that is
 * not an outer-join clause, we create an EquivalenceClass containing
 * the expressions A and B to record this knowledge.  If we later find another
 * equivalence B = C, we add C to the existing EquivalenceClass; this may
 * require merging two existing EquivalenceClasses.  At the end of the qual
 * distribution process, we have sets of values that are known all transitively
 * equal to each other, where "equal" is according to the rules of the btree
 * operator family(s) shown in ec_opfamilies, as well as the collation shown
 * by ec_collation.  (We restrict an EC to contain only equalities whose
 * operators belong to the same set of opfamilies.  This could probably be
 * relaxed, but for now it's not worth the trouble, since nearly all equality
 * operators belong to only one btree opclass anyway.  Similarly, we suppose
 * that all or none of the input datatypes are collatable, so that a single
 * collation value is sufficient.)
 *
 * Strictly speaking, deductions from an EquivalenceClass hold only within
 * a "join domain", that is a set of relations that are innerjoined together
 * (see JoinDomain above).  For the most part we don't need to account for
 * this explicitly, because equality clauses from different join domains
 * will contain Vars that are not equal() because they have different
 * nullingrel sets, and thus we will never falsely merge ECs from different
 * join domains.  But Var-free (pseudoconstant) expressions lack that safety
 * feature.  We handle that by marking "const" EC members with the JoinDomain
 * of the clause they came from; two nominally-equal const members will be
 * considered different if they came from different JoinDomains.  This ensures
 * no false EquivalenceClass merges will occur.
 *
 * We also use EquivalenceClasses as the base structure for PathKeys, letting
 * us represent knowledge about different sort orderings being equivalent.
 * Since every PathKey must reference an EquivalenceClass, we will end up
 * with single-member EquivalenceClasses whenever a sort key expression has
 * not been equivalenced to anything else.  It is also possible that such an
 * EquivalenceClass will contain a volatile expression ("ORDER BY random()"),
 * which is a case that can't arise otherwise since clauses containing
 * volatile functions are never considered mergejoinable.  We mark such
 * EquivalenceClasses specially to prevent them from being merged with
 * ordinary EquivalenceClasses.  Also, for volatile expressions we have
 * to be careful to match the EquivalenceClass to the correct targetlist
 * entry: consider SELECT random() AS a, random() AS b ... ORDER BY b,a.
 * So we record the SortGroupRef of the originating sort clause.
 *
 * NB: if ec_merged isn't NULL, this class has been merged into another, and
 * should be ignored in favor of using the pointed-to class.
 *
 * NB: EquivalenceClasses are never copied after creation.  Therefore,
 * copyObject() copies pointers to them as pointers, and equal() compares
 * pointers to EquivalenceClasses via pointer equality.  This is implemented
 * by putting copy_as_scalar and equal_as_scalar attributes on fields that
 * are pointers to EquivalenceClasses.  The same goes for EquivalenceMembers.
 */
typedef struct EquivalenceClass
{
	pg_node_attr(custom_read_write, no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	List	   *ec_opfamilies;	/* btree operator family OIDs */
	Oid			ec_collation;	/* collation, if datatypes are collatable */
	List	   *ec_members;		/* list of EquivalenceMembers */
	List	   *ec_sources;		/* list of generating RestrictInfos */
	List	   *ec_derives;		/* list of derived RestrictInfos */
	Relids		ec_relids;		/* all relids appearing in ec_members, except
								 * for child members (see below) */
	bool		ec_has_const;	/* any pseudoconstants in ec_members? */
	bool		ec_has_volatile;	/* the (sole) member is a volatile expr */
	bool		ec_broken;		/* failed to generate needed clauses? */
	Index		ec_sortref;		/* originating sortclause label, or 0 */
	Index		ec_min_security;	/* minimum security_level in ec_sources */
	Index		ec_max_security;	/* maximum security_level in ec_sources */
	struct EquivalenceClass *ec_merged; /* set if merged into another EC */
} EquivalenceClass;

/*
 * If an EC contains a constant, any PathKey depending on it must be
 * redundant, since there's only one possible value of the key.
 */
#define EC_MUST_BE_REDUNDANT(eclass)  \
	((eclass)->ec_has_const)

/*
 * EquivalenceMember - one member expression of an EquivalenceClass
 *
 * em_is_child signifies that this element was built by transposing a member
 * for an appendrel parent relation to represent the corresponding expression
 * for an appendrel child.  These members are used for determining the
 * pathkeys of scans on the child relation and for explicitly sorting the
 * child when necessary to build a MergeAppend path for the whole appendrel
 * tree.  An em_is_child member has no impact on the properties of the EC as a
 * whole; in particular the EC's ec_relids field does NOT include the child
 * relation.  An em_is_child member should never be marked em_is_const nor
 * cause ec_has_const or ec_has_volatile to be set, either.  Thus, em_is_child
 * members are not really full-fledged members of the EC, but just reflections
 * or doppelgangers of real members.  Most operations on EquivalenceClasses
 * should ignore em_is_child members, and those that don't should test
 * em_relids to make sure they only consider relevant members.
 *
 * em_datatype is usually the same as exprType(em_expr), but can be
 * different when dealing with a binary-compatible opfamily; in particular
 * anyarray_ops would never work without this.  Use em_datatype when
 * looking up a specific btree operator to work with this expression.
 */
typedef struct EquivalenceMember
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Expr	   *em_expr;		/* the expression represented */
	Relids		em_relids;		/* all relids appearing in em_expr */
	bool		em_is_const;	/* expression is pseudoconstant? */
	bool		em_is_child;	/* derived version for a child relation? */
	Oid			em_datatype;	/* the "nominal type" used by the opfamily */
	JoinDomain *em_jdomain;		/* join domain containing the source clause */
	/* if em_is_child is true, this links to corresponding EM for top parent */
	struct EquivalenceMember *em_parent pg_node_attr(read_write_ignore);
} EquivalenceMember;

/*
 * PathKeys
 *
 * The sort ordering of a path is represented by a list of PathKey nodes.
 * An empty list implies no known ordering.  Otherwise the first item
 * represents the primary sort key, the second the first secondary sort key,
 * etc.  The value being sorted is represented by linking to an
 * EquivalenceClass containing that value and including pk_opfamily among its
 * ec_opfamilies.  The EquivalenceClass tells which collation to use, too.
 * This is a convenient method because it makes it trivial to detect
 * equivalent and closely-related orderings. (See optimizer/README for more
 * information.)
 *
 * Note: pk_strategy is either BTLessStrategyNumber (for ASC) or
 * BTGreaterStrategyNumber (for DESC).  We assume that all ordering-capable
 * index types will use btree-compatible strategy numbers.
 */
typedef struct PathKey
{
	pg_node_attr(no_read, no_query_jumble)

	NodeTag		type;

	/* the value that is ordered */
	EquivalenceClass *pk_eclass pg_node_attr(copy_as_scalar, equal_as_scalar);
	Oid			pk_opfamily;	/* btree opfamily defining the ordering */
	int			pk_strategy;	/* sort direction (ASC or DESC) */
	bool		pk_nulls_first; /* do NULLs come before normal values? */
} PathKey;

/*
 * Contains an order of group-by clauses and the corresponding list of
 * pathkeys.
 *
 * The elements of 'clauses' list should have the same order as the head of
 * 'pathkeys' list.  The tleSortGroupRef of the clause should be equal to
 * ec_sortref of the pathkey equivalence class.  If there are redundant
 * clauses with the same tleSortGroupRef, they must be grouped together.
 */
typedef struct GroupByOrdering
{
	NodeTag		type;

	List	   *pathkeys;
	List	   *clauses;
} GroupByOrdering;

/*
 * VolatileFunctionStatus -- allows nodes to cache their
 * contain_volatile_functions properties. VOLATILITY_UNKNOWN means not yet
 * determined.
 */
typedef enum VolatileFunctionStatus
{
	VOLATILITY_UNKNOWN = 0,
	VOLATILITY_VOLATILE,
	VOLATILITY_NOVOLATILE,
} VolatileFunctionStatus;

/*
 * PathTarget
 *
 * This struct contains what we need to know during planning about the
 * targetlist (output columns) that a Path will compute.  Each RelOptInfo
 * includes a default PathTarget, which its individual Paths may simply
 * reference.  However, in some cases a Path may compute outputs different
 * from other Paths, and in that case we make a custom PathTarget for it.
 * For example, an indexscan might return index expressions that would
 * otherwise need to be explicitly calculated.  (Note also that "upper"
 * relations generally don't have useful default PathTargets.)
 *
 * exprs contains bare expressions; they do not have TargetEntry nodes on top,
 * though those will appear in finished Plans.
 *
 * sortgrouprefs[] is an array of the same length as exprs, containing the
 * corresponding sort/group refnos, or zeroes for expressions not referenced
 * by sort/group clauses.  If sortgrouprefs is NULL (which it generally is in
 * RelOptInfo.reltarget targets; only upper-level Paths contain this info),
 * we have not identified sort/group columns in this tlist.  This allows us to
 * deal with sort/group refnos when needed with less expense than including
 * TargetEntry nodes in the exprs list.
 */
typedef struct PathTarget
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* list of expressions to be computed */
	List	   *exprs;

	/* corresponding sort/group refnos, or 0 */
	Index	   *sortgrouprefs pg_node_attr(array_size(exprs));

	/* cost of evaluating the expressions */
	QualCost	cost;

	/* estimated avg width of result tuples */
	int			width;

	/* indicates if exprs contain any volatile functions */
	VolatileFunctionStatus has_volatile_expr;
} PathTarget;

/* Convenience macro to get a sort/group refno from a PathTarget */
#define get_pathtarget_sortgroupref(target, colno) \
	((target)->sortgrouprefs ? (target)->sortgrouprefs[colno] : (Index) 0)


/*
 * ParamPathInfo
 *
 * All parameterized paths for a given relation with given required outer rels
 * link to a single ParamPathInfo, which stores common information such as
 * the estimated rowcount for this parameterization.  We do this partly to
 * avoid recalculations, but mostly to ensure that the estimated rowcount
 * is in fact the same for every such path.
 *
 * Note: ppi_clauses is only used in ParamPathInfos for base relation paths;
 * in join cases it's NIL because the set of relevant clauses varies depending
 * on how the join is formed.  The relevant clauses will appear in each
 * parameterized join path's joinrestrictinfo list, instead.  ParamPathInfos
 * for append relations don't bother with this, either.
 *
 * ppi_serials is the set of rinfo_serial numbers for quals that are enforced
 * by this path.  As with ppi_clauses, it's only maintained for baserels.
 * (We could construct it on-the-fly from ppi_clauses, but it seems better
 * to materialize a copy.)
 */
typedef struct ParamPathInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Relids		ppi_req_outer;	/* rels supplying parameters used by path */
	Cardinality ppi_rows;		/* estimated number of result tuples */
	List	   *ppi_clauses;	/* join clauses available from outer rels */
	Bitmapset  *ppi_serials;	/* set of rinfo_serial for enforced quals */
} ParamPathInfo;


/*
 * Type "Path" is used as-is for sequential-scan paths, as well as some other
 * simple plan types that we don't need any extra information in the path for.
 * For other path types it is the first component of a larger struct.
 *
 * "pathtype" is the NodeTag of the Plan node we could build from this Path.
 * It is partially redundant with the Path's NodeTag, but allows us to use
 * the same Path type for multiple Plan types when there is no need to
 * distinguish the Plan type during path processing.
 *
 * "parent" identifies the relation this Path scans, and "pathtarget"
 * describes the precise set of output columns the Path would compute.
 * In simple cases all Paths for a given rel share the same targetlist,
 * which we represent by having path->pathtarget equal to parent->reltarget.
 *
 * "param_info", if not NULL, links to a ParamPathInfo that identifies outer
 * relation(s) that provide parameter values to each scan of this path.
 * That means this path can only be joined to those rels by means of nestloop
 * joins with this path on the inside.  Also note that a parameterized path
 * is responsible for testing all "movable" joinclauses involving this rel
 * and the specified outer rel(s).
 *
 * "rows" is the same as parent->rows in simple paths, but in parameterized
 * paths and UniquePaths it can be less than parent->rows, reflecting the
 * fact that we've filtered by extra join conditions or removed duplicates.
 *
 * "pathkeys" is a List of PathKey nodes (see above), describing the sort
 * ordering of the path's output rows.
 *
 * We do not support copying Path trees, mainly because the circular linkages
 * between RelOptInfo and Path nodes can't be handled easily in a simple
 * depth-first traversal.  We also don't have read support at the moment.
 */
typedef struct Path
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* tag identifying scan/join method */
	NodeTag		pathtype;

	/*
	 * the relation this path can build
	 *
	 * We do NOT print the parent, else we'd be in infinite recursion.  We can
	 * print the parent's relids for identification purposes, though.
	 */
	RelOptInfo *parent pg_node_attr(write_only_relids);

	/*
	 * list of Vars/Exprs, cost, width
	 *
	 * We print the pathtarget only if it's not the default one for the rel.
	 */
	PathTarget *pathtarget pg_node_attr(write_only_nondefault_pathtarget);

	/*
	 * parameterization info, or NULL if none
	 *
	 * We do not print the whole of param_info, since it's printed via
	 * RelOptInfo; it's sufficient and less cluttering to print just the
	 * required outer relids.
	 */
	ParamPathInfo *param_info pg_node_attr(write_only_req_outer);

	/* engage parallel-aware logic? */
	bool		parallel_aware;
	/* OK to use as part of parallel plan? */
	bool		parallel_safe;
	/* desired # of workers; 0 = not parallel */
	int			parallel_workers;

	/* estimated size/costs for path (see costsize.c for more info) */
	Cardinality rows;			/* estimated number of result tuples */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	/* sort ordering of path's output; a List of PathKey nodes; see above */
	List	   *pathkeys;
} Path;

/* Macro for extracting a path's parameterization relids; beware double eval */
#define PATH_REQ_OUTER(path)  \
	((path)->param_info ? (path)->param_info->ppi_req_outer : (Relids) NULL)

/*----------
 * IndexPath represents an index scan over a single index.
 *
 * This struct is used for both regular indexscans and index-only scans;
 * path.pathtype is T_IndexScan or T_IndexOnlyScan to show which is meant.
 *
 * 'indexinfo' is the index to be scanned.
 *
 * 'indexclauses' is a list of IndexClause nodes, each representing one
 * index-checkable restriction, with implicit AND semantics across the list.
 * An empty list implies a full index scan.
 *
 * 'indexorderbys', if not NIL, is a list of ORDER BY expressions that have
 * been found to be usable as ordering operators for an amcanorderbyop index.
 * The list must match the path's pathkeys, ie, one expression per pathkey
 * in the same order.  These are not RestrictInfos, just bare expressions,
 * since they generally won't yield booleans.  It's guaranteed that each
 * expression has the index key on the left side of the operator.
 *
 * 'indexorderbycols' is an integer list of index column numbers (zero-based)
 * of the same length as 'indexorderbys', showing which index column each
 * ORDER BY expression is meant to be used with.  (There is no restriction
 * on which index column each ORDER BY can be used with.)
 *
 * 'indexscandir' is one of:
 *		ForwardScanDirection: forward scan of an index
 *		BackwardScanDirection: backward scan of an ordered index
 * Unordered indexes will always have an indexscandir of ForwardScanDirection.
 *
 * 'indextotalcost' and 'indexselectivity' are saved in the IndexPath so that
 * we need not recompute them when considering using the same index in a
 * bitmap index/heap scan (see BitmapHeapPath).  The costs of the IndexPath
 * itself represent the costs of an IndexScan or IndexOnlyScan plan type.
 *----------
 */
typedef struct IndexPath
{
	Path		path;
	IndexOptInfo *indexinfo;
	List	   *indexclauses;
	List	   *indexorderbys;
	List	   *indexorderbycols;
	ScanDirection indexscandir;
	Cost		indextotalcost;
	Selectivity indexselectivity;
} IndexPath;

/*
 * Each IndexClause references a RestrictInfo node from the query's WHERE
 * or JOIN conditions, and shows how that restriction can be applied to
 * the particular index.  We support both indexclauses that are directly
 * usable by the index machinery, which are typically of the form
 * "indexcol OP pseudoconstant", and those from which an indexable qual
 * can be derived.  The simplest such transformation is that a clause
 * of the form "pseudoconstant OP indexcol" can be commuted to produce an
 * indexable qual (the index machinery expects the indexcol to be on the
 * left always).  Another example is that we might be able to extract an
 * indexable range condition from a LIKE condition, as in "x LIKE 'foo%bar'"
 * giving rise to "x >= 'foo' AND x < 'fop'".  Derivation of such lossy
 * conditions is done by a planner support function attached to the
 * indexclause's top-level function or operator.
 *
 * indexquals is a list of RestrictInfos for the directly-usable index
 * conditions associated with this IndexClause.  In the simplest case
 * it's a one-element list whose member is iclause->rinfo.  Otherwise,
 * it contains one or more directly-usable indexqual conditions extracted
 * from the given clause.  The 'lossy' flag indicates whether the
 * indexquals are semantically equivalent to the original clause, or
 * represent a weaker condition.
 *
 * Normally, indexcol is the index of the single index column the clause
 * works on, and indexcols is NIL.  But if the clause is a RowCompareExpr,
 * indexcol is the index of the leading column, and indexcols is a list of
 * all the affected columns.  (Note that indexcols matches up with the
 * columns of the actual indexable RowCompareExpr in indexquals, which
 * might be different from the original in rinfo.)
 *
 * An IndexPath's IndexClause list is required to be ordered by index
 * column, i.e. the indexcol values must form a nondecreasing sequence.
 * (The order of multiple clauses for the same index column is unspecified.)
 */
typedef struct IndexClause
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;
	struct RestrictInfo *rinfo; /* original restriction or join clause */
	List	   *indexquals;		/* indexqual(s) derived from it */
	bool		lossy;			/* are indexquals a lossy version of clause? */
	AttrNumber	indexcol;		/* index column the clause uses (zero-based) */
	List	   *indexcols;		/* multiple index columns, if RowCompare */
} IndexClause;

/*
 * BitmapHeapPath represents one or more indexscans that generate TID bitmaps
 * instead of directly accessing the heap, followed by AND/OR combinations
 * to produce a single bitmap, followed by a heap scan that uses the bitmap.
 * Note that the output is always considered unordered, since it will come
 * out in physical heap order no matter what the underlying indexes did.
 *
 * The individual indexscans are represented by IndexPath nodes, and any
 * logic on top of them is represented by a tree of BitmapAndPath and
 * BitmapOrPath nodes.  Notice that we can use the same IndexPath node both
 * to represent a regular (or index-only) index scan plan, and as the child
 * of a BitmapHeapPath that represents scanning the same index using a
 * BitmapIndexScan.  The startup_cost and total_cost figures of an IndexPath
 * always represent the costs to use it as a regular (or index-only)
 * IndexScan.  The costs of a BitmapIndexScan can be computed using the
 * IndexPath's indextotalcost and indexselectivity.
 */
typedef struct BitmapHeapPath
{
	Path		path;
	Path	   *bitmapqual;		/* IndexPath, BitmapAndPath, BitmapOrPath */
} BitmapHeapPath;

/*
 * BitmapAndPath represents a BitmapAnd plan node; it can only appear as
 * part of the substructure of a BitmapHeapPath.  The Path structure is
 * a bit more heavyweight than we really need for this, but for simplicity
 * we make it a derivative of Path anyway.
 */
typedef struct BitmapAndPath
{
	Path		path;
	List	   *bitmapquals;	/* IndexPaths and BitmapOrPaths */
	Selectivity bitmapselectivity;
} BitmapAndPath;

/*
 * BitmapOrPath represents a BitmapOr plan node; it can only appear as
 * part of the substructure of a BitmapHeapPath.  The Path structure is
 * a bit more heavyweight than we really need for this, but for simplicity
 * we make it a derivative of Path anyway.
 */
typedef struct BitmapOrPath
{
	Path		path;
	List	   *bitmapquals;	/* IndexPaths and BitmapAndPaths */
	Selectivity bitmapselectivity;
} BitmapOrPath;

/*
 * TidPath represents a scan by TID
 *
 * tidquals is an implicitly OR'ed list of qual expressions of the form
 * "CTID = pseudoconstant", or "CTID = ANY(pseudoconstant_array)",
 * or a CurrentOfExpr for the relation.
 */
typedef struct TidPath
{
	Path		path;
	List	   *tidquals;		/* qual(s) involving CTID = something */
} TidPath;

/*
 * TidRangePath represents a scan by a contiguous range of TIDs
 *
 * tidrangequals is an implicitly AND'ed list of qual expressions of the form
 * "CTID relop pseudoconstant", where relop is one of >,>=,<,<=.
 */
typedef struct TidRangePath
{
	Path		path;
	List	   *tidrangequals;
} TidRangePath;

/*
 * SubqueryScanPath represents a scan of an unflattened subquery-in-FROM
 *
 * Note that the subpath comes from a different planning domain; for example
 * RTE indexes within it mean something different from those known to the
 * SubqueryScanPath.  path.parent->subroot is the planning context needed to
 * interpret the subpath.
 */
typedef struct SubqueryScanPath
{
	Path		path;
	Path	   *subpath;		/* path representing subquery execution */
} SubqueryScanPath;

/*
 * ForeignPath represents a potential scan of a foreign table, foreign join
 * or foreign upper-relation.
 *
 * In the case of a foreign join, fdw_restrictinfo stores the RestrictInfos to
 * apply to the join, which are used by createplan.c to get pseudoconstant
 * clauses evaluated as one-time quals in a gating Result plan node.
 *
 * fdw_private stores FDW private data about the scan.  While fdw_private is
 * not actually touched by the core code during normal operations, it's
 * generally a good idea to use a representation that can be dumped by
 * nodeToString(), so that you can examine the structure during debugging
 * with tools like pprint().
 */
typedef struct ForeignPath
{
	Path		path;
	Path	   *fdw_outerpath;
	List	   *fdw_restrictinfo;
	List	   *fdw_private;
} ForeignPath;

/*
 * CustomPath represents a table scan or a table join done by some out-of-core
 * extension.
 *
 * We provide a set of hooks here - which the provider must take care to set
 * up correctly - to allow extensions to supply their own methods of scanning
 * a relation or join relations.  For example, a provider might provide GPU
 * acceleration, a cache-based scan, or some other kind of logic we haven't
 * dreamed up yet.
 *
 * CustomPaths can be injected into the planning process for a base or join
 * relation by set_rel_pathlist_hook or set_join_pathlist_hook functions,
 * respectively.
 *
 * In the case of a table join, custom_restrictinfo stores the RestrictInfos
 * to apply to the join, which are used by createplan.c to get pseudoconstant
 * clauses evaluated as one-time quals in a gating Result plan node.
 *
 * Core code must avoid assuming that the CustomPath is only as large as
 * the structure declared here; providers are allowed to make it the first
 * element in a larger structure.  (Since the planner never copies Paths,
 * this doesn't add any complication.)  However, for consistency with the
 * FDW case, we provide a "custom_private" field in CustomPath; providers
 * may prefer to use that rather than define another struct type.
 */

struct CustomPathMethods;

typedef struct CustomPath
{
	Path		path;
	uint32		flags;			/* mask of CUSTOMPATH_* flags, see
								 * nodes/extensible.h */
	List	   *custom_paths;	/* list of child Path nodes, if any */
	List	   *custom_restrictinfo;
	List	   *custom_private;
	const struct CustomPathMethods *methods;
} CustomPath;

/*
 * AppendPath represents an Append plan, ie, successive execution of
 * several member plans.
 *
 * For partial Append, 'subpaths' contains non-partial subpaths followed by
 * partial subpaths.
 *
 * Note: it is possible for "subpaths" to contain only one, or even no,
 * elements.  These cases are optimized during create_append_plan.
 * In particular, an AppendPath with no subpaths is a "dummy" path that
 * is created to represent the case that a relation is provably empty.
 * (This is a convenient representation because it means that when we build
 * an appendrel and find that all its children have been excluded, no extra
 * action is needed to recognize the relation as dummy.)
 */
typedef struct AppendPath
{
	Path		path;
	List	   *subpaths;		/* list of component Paths */
	/* Index of first partial path in subpaths; list_length(subpaths) if none */
	int			first_partial_path;
	Cardinality limit_tuples;	/* hard limit on output tuples, or -1 */
} AppendPath;

#define IS_DUMMY_APPEND(p) \
	(IsA((p), AppendPath) && ((AppendPath *) (p))->subpaths == NIL)

/*
 * A relation that's been proven empty will have one path that is dummy
 * (but might have projection paths on top).  For historical reasons,
 * this is provided as a macro that wraps is_dummy_rel().
 */
#define IS_DUMMY_REL(r) is_dummy_rel(r)
extern bool is_dummy_rel(RelOptInfo *rel);

/*
 * MergeAppendPath represents a MergeAppend plan, ie, the merging of sorted
 * results from several member plans to produce similarly-sorted output.
 */
typedef struct MergeAppendPath
{
	Path		path;
	List	   *subpaths;		/* list of component Paths */
	Cardinality limit_tuples;	/* hard limit on output tuples, or -1 */
} MergeAppendPath;

/*
 * GroupResultPath represents use of a Result plan node to compute the
 * output of a degenerate GROUP BY case, wherein we know we should produce
 * exactly one row, which might then be filtered by a HAVING qual.
 *
 * Note that quals is a list of bare clauses, not RestrictInfos.
 */
typedef struct GroupResultPath
{
	Path		path;
	List	   *quals;
} GroupResultPath;

/*
 * MaterialPath represents use of a Material plan node, i.e., caching of
 * the output of its subpath.  This is used when the subpath is expensive
 * and needs to be scanned repeatedly, or when we need mark/restore ability
 * and the subpath doesn't have it.
 */
typedef struct MaterialPath
{
	Path		path;
	Path	   *subpath;
} MaterialPath;

/*
 * MemoizePath represents a Memoize plan node, i.e., a cache that caches
 * tuples from parameterized paths to save the underlying node from having to
 * be rescanned for parameter values which are already cached.
 */
typedef struct MemoizePath
{
	Path		path;
	Path	   *subpath;		/* outerpath to cache tuples from */
	List	   *hash_operators; /* OIDs of hash equality ops for cache keys */
	List	   *param_exprs;	/* expressions that are cache keys */
	bool		singlerow;		/* true if the cache entry is to be marked as
								 * complete after caching the first record. */
	bool		binary_mode;	/* true when cache key should be compared bit
								 * by bit, false when using hash equality ops */
	Cardinality calls;			/* expected number of rescans */
	uint32		est_entries;	/* The maximum number of entries that the
								 * planner expects will fit in the cache, or 0
								 * if unknown */
} MemoizePath;

/*
 * UniquePath represents elimination of distinct rows from the output of
 * its subpath.
 *
 * This can represent significantly different plans: either hash-based or
 * sort-based implementation, or a no-op if the input path can be proven
 * distinct already.  The decision is sufficiently localized that it's not
 * worth having separate Path node types.  (Note: in the no-op case, we could
 * eliminate the UniquePath node entirely and just return the subpath; but
 * it's convenient to have a UniquePath in the path tree to signal upper-level
 * routines that the input is known distinct.)
 */
typedef enum UniquePathMethod
{
	UNIQUE_PATH_NOOP,			/* input is known unique already */
	UNIQUE_PATH_HASH,			/* use hashing */
	UNIQUE_PATH_SORT,			/* use sorting */
} UniquePathMethod;

typedef struct UniquePath
{
	Path		path;
	Path	   *subpath;
	UniquePathMethod umethod;
	List	   *in_operators;	/* equality operators of the IN clause */
	List	   *uniq_exprs;		/* expressions to be made unique */
} UniquePath;

/*
 * GatherPath runs several copies of a plan in parallel and collects the
 * results.  The parallel leader may also execute the plan, unless the
 * single_copy flag is set.
 */
typedef struct GatherPath
{
	Path		path;
	Path	   *subpath;		/* path for each worker */
	bool		single_copy;	/* don't execute path more than once */
	int			num_workers;	/* number of workers sought to help */
} GatherPath;

/*
 * GatherMergePath runs several copies of a plan in parallel and collects
 * the results, preserving their common sort order.
 */
typedef struct GatherMergePath
{
	Path		path;
	Path	   *subpath;		/* path for each worker */
	int			num_workers;	/* number of workers sought to help */
} GatherMergePath;


/*
 * All join-type paths share these fields.
 */

typedef struct JoinPath
{
	pg_node_attr(abstract)

	Path		path;

	JoinType	jointype;

	bool		inner_unique;	/* each outer tuple provably matches no more
								 * than one inner tuple */

	Path	   *outerjoinpath;	/* path for the outer side of the join */
	Path	   *innerjoinpath;	/* path for the inner side of the join */

	List	   *joinrestrictinfo;	/* RestrictInfos to apply to join */

	/*
	 * See the notes for RelOptInfo and ParamPathInfo to understand why
	 * joinrestrictinfo is needed in JoinPath, and can't be merged into the
	 * parent RelOptInfo.
	 */
} JoinPath;

/*
 * A nested-loop path needs no special fields.
 */

typedef struct NestPath
{
	JoinPath	jpath;
} NestPath;

/*
 * A mergejoin path has these fields.
 *
 * Unlike other path types, a MergePath node doesn't represent just a single
 * run-time plan node: it can represent up to four.  Aside from the MergeJoin
 * node itself, there can be a Sort node for the outer input, a Sort node
 * for the inner input, and/or a Material node for the inner input.  We could
 * represent these nodes by separate path nodes, but considering how many
 * different merge paths are investigated during a complex join problem,
 * it seems better to avoid unnecessary palloc overhead.
 *
 * path_mergeclauses lists the clauses (in the form of RestrictInfos)
 * that will be used in the merge.
 *
 * Note that the mergeclauses are a subset of the parent relation's
 * restriction-clause list.  Any join clauses that are not mergejoinable
 * appear only in the parent's restrict list, and must be checked by a
 * qpqual at execution time.
 *
 * outersortkeys (resp. innersortkeys) is NIL if the outer path
 * (resp. inner path) is already ordered appropriately for the
 * mergejoin.  If it is not NIL then it is a PathKeys list describing
 * the ordering that must be created by an explicit Sort node.
 *
 * skip_mark_restore is true if the executor need not do mark/restore calls.
 * Mark/restore overhead is usually required, but can be skipped if we know
 * that the executor need find only one match per outer tuple, and that the
 * mergeclauses are sufficient to identify a match.  In such cases the
 * executor can immediately advance the outer relation after processing a
 * match, and therefore it need never back up the inner relation.
 *
 * materialize_inner is true if a Material node should be placed atop the
 * inner input.  This may appear with or without an inner Sort step.
 */

typedef struct MergePath
{
	JoinPath	jpath;
	List	   *path_mergeclauses;	/* join clauses to be used for merge */
	List	   *outersortkeys;	/* keys for explicit sort, if any */
	List	   *innersortkeys;	/* keys for explicit sort, if any */
	bool		skip_mark_restore;	/* can executor skip mark/restore? */
	bool		materialize_inner;	/* add Materialize to inner? */
} MergePath;

/*
 * A hashjoin path has these fields.
 *
 * The remarks above for mergeclauses apply for hashclauses as well.
 *
 * Hashjoin does not care what order its inputs appear in, so we have
 * no need for sortkeys.
 */

typedef struct HashPath
{
	JoinPath	jpath;
	List	   *path_hashclauses;	/* join clauses used for hashing */
	int			num_batches;	/* number of batches expected */
	Cardinality inner_rows_total;	/* total inner rows expected */
} HashPath;

/*
 * ProjectionPath represents a projection (that is, targetlist computation)
 *
 * Nominally, this path node represents using a Result plan node to do a
 * projection step.  However, if the input plan node supports projection,
 * we can just modify its output targetlist to do the required calculations
 * directly, and not need a Result.  In some places in the planner we can just
 * jam the desired PathTarget into the input path node (and adjust its cost
 * accordingly), so we don't need a ProjectionPath.  But in other places
 * it's necessary to not modify the input path node, so we need a separate
 * ProjectionPath node, which is marked dummy to indicate that we intend to
 * assign the work to the input plan node.  The estimated cost for the
 * ProjectionPath node will account for whether a Result will be used or not.
 */
typedef struct ProjectionPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	bool		dummypp;		/* true if no separate Result is needed */
} ProjectionPath;

/*
 * ProjectSetPath represents evaluation of a targetlist that includes
 * set-returning function(s), which will need to be implemented by a
 * ProjectSet plan node.
 */
typedef struct ProjectSetPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
} ProjectSetPath;

/*
 * SortPath represents an explicit sort step
 *
 * The sort keys are, by definition, the same as path.pathkeys.
 *
 * Note: the Sort plan node cannot project, so path.pathtarget must be the
 * same as the input's pathtarget.
 */
typedef struct SortPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
} SortPath;

/*
 * IncrementalSortPath represents an incremental sort step
 *
 * This is like a regular sort, except some leading key columns are assumed
 * to be ordered already.
 */
typedef struct IncrementalSortPath
{
	SortPath	spath;
	int			nPresortedCols; /* number of presorted columns */
} IncrementalSortPath;

/*
 * GroupPath represents grouping (of presorted input)
 *
 * groupClause represents the columns to be grouped on; the input path
 * must be at least that well sorted.
 *
 * We can also apply a qual to the grouped rows (equivalent of HAVING)
 */
typedef struct GroupPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	List	   *groupClause;	/* a list of SortGroupClause's */
	List	   *qual;			/* quals (HAVING quals), if any */
} GroupPath;

/*
 * UpperUniquePath represents adjacent-duplicate removal (in presorted input)
 *
 * The columns to be compared are the first numkeys columns of the path's
 * pathkeys.  The input is presumed already sorted that way.
 */
typedef struct UpperUniquePath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	int			numkeys;		/* number of pathkey columns to compare */
} UpperUniquePath;

/*
 * AggPath represents generic computation of aggregate functions
 *
 * This may involve plain grouping (but not grouping sets), using either
 * sorted or hashed grouping; for the AGG_SORTED case, the input must be
 * appropriately presorted.
 */
typedef struct AggPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	AggStrategy aggstrategy;	/* basic strategy, see nodes.h */
	AggSplit	aggsplit;		/* agg-splitting mode, see nodes.h */
	Cardinality numGroups;		/* estimated number of groups in input */
	uint64		transitionSpace;	/* for pass-by-ref transition data */
	List	   *groupClause;	/* a list of SortGroupClause's */
	List	   *qual;			/* quals (HAVING quals), if any */
} AggPath;

/*
 * Various annotations used for grouping sets in the planner.
 */

typedef struct GroupingSetData
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;
	List	   *set;			/* grouping set as list of sortgrouprefs */
	Cardinality numGroups;		/* est. number of result groups */
} GroupingSetData;

typedef struct RollupData
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;
	List	   *groupClause;	/* applicable subset of parse->groupClause */
	List	   *gsets;			/* lists of integer indexes into groupClause */
	List	   *gsets_data;		/* list of GroupingSetData */
	Cardinality numGroups;		/* est. number of result groups */
	bool		hashable;		/* can be hashed */
	bool		is_hashed;		/* to be implemented as a hashagg */
} RollupData;

/*
 * GroupingSetsPath represents a GROUPING SETS aggregation
 */

typedef struct GroupingSetsPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	AggStrategy aggstrategy;	/* basic strategy */
	List	   *rollups;		/* list of RollupData */
	List	   *qual;			/* quals (HAVING quals), if any */
	uint64		transitionSpace;	/* for pass-by-ref transition data */
} GroupingSetsPath;

/*
 * MinMaxAggPath represents computation of MIN/MAX aggregates from indexes
 */
typedef struct MinMaxAggPath
{
	Path		path;
	List	   *mmaggregates;	/* list of MinMaxAggInfo */
	List	   *quals;			/* HAVING quals, if any */
} MinMaxAggPath;

/*
 * WindowAggPath represents generic computation of window functions
 */
typedef struct WindowAggPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	WindowClause *winclause;	/* WindowClause we'll be using */
	List	   *qual;			/* lower-level WindowAgg runconditions */
	List	   *runCondition;	/* OpExpr List to short-circuit execution */
	bool		topwindow;		/* false for all apart from the WindowAgg
								 * that's closest to the root of the plan */
} WindowAggPath;

/*
 * SetOpPath represents a set-operation, that is INTERSECT or EXCEPT
 */
typedef struct SetOpPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	SetOpCmd	cmd;			/* what to do, see nodes.h */
	SetOpStrategy strategy;		/* how to do it, see nodes.h */
	List	   *distinctList;	/* SortGroupClauses identifying target cols */
	AttrNumber	flagColIdx;		/* where is the flag column, if any */
	int			firstFlag;		/* flag value for first input relation */
	Cardinality numGroups;		/* estimated number of groups in input */
} SetOpPath;

/*
 * RecursiveUnionPath represents a recursive UNION node
 */
typedef struct RecursiveUnionPath
{
	Path		path;
	Path	   *leftpath;		/* paths representing input sources */
	Path	   *rightpath;
	List	   *distinctList;	/* SortGroupClauses identifying target cols */
	int			wtParam;		/* ID of Param representing work table */
	Cardinality numGroups;		/* estimated number of groups in input */
} RecursiveUnionPath;

/*
 * LockRowsPath represents acquiring row locks for SELECT FOR UPDATE/SHARE
 */
typedef struct LockRowsPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	List	   *rowMarks;		/* a list of PlanRowMark's */
	int			epqParam;		/* ID of Param for EvalPlanQual re-eval */
} LockRowsPath;

/*
 * ModifyTablePath represents performing INSERT/UPDATE/DELETE/MERGE
 *
 * We represent most things that will be in the ModifyTable plan node
 * literally, except we have a child Path not Plan.  But analysis of the
 * OnConflictExpr is deferred to createplan.c, as is collection of FDW data.
 */
typedef struct ModifyTablePath
{
	Path		path;
	Path	   *subpath;		/* Path producing source data */
	CmdType		operation;		/* INSERT, UPDATE, DELETE, or MERGE */
	bool		canSetTag;		/* do we set the command tag/es_processed? */
	Index		nominalRelation;	/* Parent RT index for use of EXPLAIN */
	Index		rootRelation;	/* Root RT index, if partitioned/inherited */
	bool		partColsUpdated;	/* some part key in hierarchy updated? */
	List	   *resultRelations;	/* integer list of RT indexes */
	List	   *updateColnosLists;	/* per-target-table update_colnos lists */
	List	   *withCheckOptionLists;	/* per-target-table WCO lists */
	List	   *returningLists; /* per-target-table RETURNING tlists */
	List	   *rowMarks;		/* PlanRowMarks (non-locking only) */
	OnConflictExpr *onconflict; /* ON CONFLICT clause, or NULL */
	int			epqParam;		/* ID of Param for EvalPlanQual re-eval */
	List	   *mergeActionLists;	/* per-target-table lists of actions for
									 * MERGE */
	List	   *mergeJoinConditions;	/* per-target-table join conditions
										 * for MERGE */
} ModifyTablePath;

/*
 * LimitPath represents applying LIMIT/OFFSET restrictions
 */
typedef struct LimitPath
{
	Path		path;
	Path	   *subpath;		/* path representing input source */
	Node	   *limitOffset;	/* OFFSET parameter, or NULL if none */
	Node	   *limitCount;		/* COUNT parameter, or NULL if none */
	LimitOption limitOption;	/* FETCH FIRST with ties or exact number */
} LimitPath;


/*
 * Restriction clause info.
 *
 * We create one of these for each AND sub-clause of a restriction condition
 * (WHERE or JOIN/ON clause).  Since the restriction clauses are logically
 * ANDed, we can use any one of them or any subset of them to filter out
 * tuples, without having to evaluate the rest.  The RestrictInfo node itself
 * stores data used by the optimizer while choosing the best query plan.
 *
 * If a restriction clause references a single base relation, it will appear
 * in the baserestrictinfo list of the RelOptInfo for that base rel.
 *
 * If a restriction clause references more than one base+OJ relation, it will
 * appear in the joininfo list of every RelOptInfo that describes a strict
 * subset of the relations mentioned in the clause.  The joininfo lists are
 * used to drive join tree building by selecting plausible join candidates.
 * The clause cannot actually be applied until we have built a join rel
 * containing all the relations it references, however.
 *
 * When we construct a join rel that includes all the relations referenced
 * in a multi-relation restriction clause, we place that clause into the
 * joinrestrictinfo lists of paths for the join rel, if neither left nor
 * right sub-path includes all relations referenced in the clause.  The clause
 * will be applied at that join level, and will not propagate any further up
 * the join tree.  (Note: the "predicate migration" code was once intended to
 * push restriction clauses up and down the plan tree based on evaluation
 * costs, but it's dead code and is unlikely to be resurrected in the
 * foreseeable future.)
 *
 * Note that in the presence of more than two rels, a multi-rel restriction
 * might reach different heights in the join tree depending on the join
 * sequence we use.  So, these clauses cannot be associated directly with
 * the join RelOptInfo, but must be kept track of on a per-join-path basis.
 *
 * RestrictInfos that represent equivalence conditions (i.e., mergejoinable
 * equalities that are not outerjoin-delayed) are handled a bit differently.
 * Initially we attach them to the EquivalenceClasses that are derived from
 * them.  When we construct a scan or join path, we look through all the
 * EquivalenceClasses and generate derived RestrictInfos representing the
 * minimal set of conditions that need to be checked for this particular scan
 * or join to enforce that all members of each EquivalenceClass are in fact
 * equal in all rows emitted by the scan or join.
 *
 * The clause_relids field lists the base plus outer-join RT indexes that
 * actually appear in the clause.  required_relids lists the minimum set of
 * relids needed to evaluate the clause; while this is often equal to
 * clause_relids, it can be more.  We will add relids to required_relids when
 * we need to force an outer join ON clause to be evaluated exactly at the
 * level of the outer join, which is true except when it is a "degenerate"
 * condition that references only Vars from the nullable side of the join.
 *
 * RestrictInfo nodes contain a flag to indicate whether a qual has been
 * pushed down to a lower level than its original syntactic placement in the
 * join tree would suggest.  If an outer join prevents us from pushing a qual
 * down to its "natural" semantic level (the level associated with just the
 * base rels used in the qual) then we mark the qual with a "required_relids"
 * value including more than just the base rels it actually uses.  By
 * pretending that the qual references all the rels required to form the outer
 * join, we prevent it from being evaluated below the outer join's joinrel.
 * When we do form the outer join's joinrel, we still need to distinguish
 * those quals that are actually in that join's JOIN/ON condition from those
 * that appeared elsewhere in the tree and were pushed down to the join rel
 * because they used no other rels.  That's what the is_pushed_down flag is
 * for; it tells us that a qual is not an OUTER JOIN qual for the set of base
 * rels listed in required_relids.  A clause that originally came from WHERE
 * or an INNER JOIN condition will *always* have its is_pushed_down flag set.
 * It's possible for an OUTER JOIN clause to be marked is_pushed_down too,
 * if we decide that it can be pushed down into the nullable side of the join.
 * In that case it acts as a plain filter qual for wherever it gets evaluated.
 * (In short, is_pushed_down is only false for non-degenerate outer join
 * conditions.  Possibly we should rename it to reflect that meaning?  But
 * see also the comments for RINFO_IS_PUSHED_DOWN, below.)
 *
 * There is also an incompatible_relids field, which is a set of outer-join
 * relids above which we cannot evaluate the clause (because they might null
 * Vars it uses that should not be nulled yet).  In principle this could be
 * filled in any RestrictInfo as the set of OJ relids that appear above the
 * clause and null Vars that it uses.  In practice we only bother to populate
 * it for "clone" clauses, as it's currently only needed to prevent multiple
 * clones of the same clause from being accepted for evaluation at the same
 * join level.
 *
 * There is also an outer_relids field, which is NULL except for outer join
 * clauses; for those, it is the set of relids on the outer side of the
 * clause's outer join.  (These are rels that the clause cannot be applied to
 * in parameterized scans, since pushing it into the join's outer side would
 * lead to wrong answers.)
 *
 * To handle security-barrier conditions efficiently, we mark RestrictInfo
 * nodes with a security_level field, in which higher values identify clauses
 * coming from less-trusted sources.  The exact semantics are that a clause
 * cannot be evaluated before another clause with a lower security_level value
 * unless the first clause is leakproof.  As with outer-join clauses, this
 * creates a reason for clauses to sometimes need to be evaluated higher in
 * the join tree than their contents would suggest; and even at a single plan
 * node, this rule constrains the order of application of clauses.
 *
 * In general, the referenced clause might be arbitrarily complex.  The
 * kinds of clauses we can handle as indexscan quals, mergejoin clauses,
 * or hashjoin clauses are limited (e.g., no volatile functions).  The code
 * for each kind of path is responsible for identifying the restrict clauses
 * it can use and ignoring the rest.  Clauses not implemented by an indexscan,
 * mergejoin, or hashjoin will be placed in the plan qual or joinqual field
 * of the finished Plan node, where they will be enforced by general-purpose
 * qual-expression-evaluation code.  (But we are still entitled to count
 * their selectivity when estimating the result tuple count, if we
 * can guess what it is...)
 *
 * When the referenced clause is an OR clause, we generate a modified copy
 * in which additional RestrictInfo nodes are inserted below the top-level
 * OR/AND structure.  This is a convenience for OR indexscan processing:
 * indexquals taken from either the top level or an OR subclause will have
 * associated RestrictInfo nodes.
 *
 * The can_join flag is set true if the clause looks potentially useful as
 * a merge or hash join clause, that is if it is a binary opclause with
 * nonoverlapping sets of relids referenced in the left and right sides.
 * (Whether the operator is actually merge or hash joinable isn't checked,
 * however.)
 *
 * The pseudoconstant flag is set true if the clause contains no Vars of
 * the current query level and no volatile functions.  Such a clause can be
 * pulled out and used as a one-time qual in a gating Result node.  We keep
 * pseudoconstant clauses in the same lists as other RestrictInfos so that
 * the regular clause-pushing machinery can assign them to the correct join
 * level, but they need to be treated specially for cost and selectivity
 * estimates.  Note that a pseudoconstant clause can never be an indexqual
 * or merge or hash join clause, so it's of no interest to large parts of
 * the planner.
 *
 * When we generate multiple versions of a clause so as to have versions
 * that will work after commuting some left joins per outer join identity 3,
 * we mark the one with the fewest nullingrels bits with has_clone = true,
 * and the rest with is_clone = true.  This allows proper filtering of
 * these redundant clauses, so that we apply only one version of them.
 *
 * When join clauses are generated from EquivalenceClasses, there may be
 * several equally valid ways to enforce join equivalence, of which we need
 * apply only one.  We mark clauses of this kind by setting parent_ec to
 * point to the generating EquivalenceClass.  Multiple clauses with the same
 * parent_ec in the same join are redundant.
 *
 * Most fields are ignored for equality, since they may not be set yet, and
 * should be derivable from the clause anyway.
 *
 * parent_ec, left_ec, right_ec are not printed, lest it lead to infinite
 * recursion in plan tree dump.
 */

typedef struct RestrictInfo
{
	pg_node_attr(no_read, no_query_jumble)

	NodeTag		type;

	/* the represented clause of WHERE or JOIN */
	Expr	   *clause;

	/* true if clause was pushed down in level */
	bool		is_pushed_down;

	/* see comment above */
	bool		can_join pg_node_attr(equal_ignore);

	/* see comment above */
	bool		pseudoconstant pg_node_attr(equal_ignore);

	/* see comment above */
	bool		has_clone;
	bool		is_clone;

	/* true if known to contain no leaked Vars */
	bool		leakproof pg_node_attr(equal_ignore);

	/* indicates if clause contains any volatile functions */
	VolatileFunctionStatus has_volatile pg_node_attr(equal_ignore);

	/* see comment above */
	Index		security_level;

	/* number of base rels in clause_relids */
	int			num_base_rels pg_node_attr(equal_ignore);

	/* The relids (varnos+varnullingrels) actually referenced in the clause: */
	Relids		clause_relids pg_node_attr(equal_ignore);

	/* The set of relids required to evaluate the clause: */
	Relids		required_relids;

	/* Relids above which we cannot evaluate the clause (see comment above) */
	Relids		incompatible_relids;

	/* If an outer-join clause, the outer-side relations, else NULL: */
	Relids		outer_relids;

	/*
	 * Relids in the left/right side of the clause.  These fields are set for
	 * any binary opclause.
	 */
	Relids		left_relids pg_node_attr(equal_ignore);
	Relids		right_relids pg_node_attr(equal_ignore);

	/*
	 * Modified clause with RestrictInfos.  This field is NULL unless clause
	 * is an OR clause.
	 */
	Expr	   *orclause pg_node_attr(equal_ignore);

	/*----------
	 * Serial number of this RestrictInfo.  This is unique within the current
	 * PlannerInfo context, with a few critical exceptions:
	 * 1. When we generate multiple clones of the same qual condition to
	 * cope with outer join identity 3, all the clones get the same serial
	 * number.  This reflects that we only want to apply one of them in any
	 * given plan.
	 * 2. If we manufacture a commuted version of a qual to use as an index
	 * condition, it copies the original's rinfo_serial, since it is in
	 * practice the same condition.
	 * 3. If we reduce a qual to constant-FALSE, the new constant-FALSE qual
	 * copies the original's rinfo_serial, since it is in practice the same
	 * condition.
	 * 4. RestrictInfos made for a child relation copy their parent's
	 * rinfo_serial.  Likewise, when an EquivalenceClass makes a derived
	 * equality clause for a child relation, it copies the rinfo_serial of
	 * the matching equality clause for the parent.  This allows detection
	 * of redundant pushed-down equality clauses.
	 *----------
	 */
	int			rinfo_serial;

	/*
	 * Generating EquivalenceClass.  This field is NULL unless clause is
	 * potentially redundant.
	 */
	EquivalenceClass *parent_ec pg_node_attr(copy_as_scalar, equal_ignore, read_write_ignore);

	/*
	 * cache space for cost and selectivity
	 */

	/* eval cost of clause; -1 if not yet set */
	QualCost	eval_cost pg_node_attr(equal_ignore);

	/* selectivity for "normal" (JOIN_INNER) semantics; -1 if not yet set */
	Selectivity norm_selec pg_node_attr(equal_ignore);
	/* selectivity for outer join semantics; -1 if not yet set */
	Selectivity outer_selec pg_node_attr(equal_ignore);

	/*
	 * opfamilies containing clause operator; valid if clause is
	 * mergejoinable, else NIL
	 */
	List	   *mergeopfamilies pg_node_attr(equal_ignore);

	/*
	 * cache space for mergeclause processing; NULL if not yet set
	 */

	/* EquivalenceClass containing lefthand */
	EquivalenceClass *left_ec pg_node_attr(copy_as_scalar, equal_ignore, read_write_ignore);
	/* EquivalenceClass containing righthand */
	EquivalenceClass *right_ec pg_node_attr(copy_as_scalar, equal_ignore, read_write_ignore);
	/* EquivalenceMember for lefthand */
	EquivalenceMember *left_em pg_node_attr(copy_as_scalar, equal_ignore);
	/* EquivalenceMember for righthand */
	EquivalenceMember *right_em pg_node_attr(copy_as_scalar, equal_ignore);

	/*
	 * List of MergeScanSelCache structs.  Those aren't Nodes, so hard to
	 * copy; instead replace with NIL.  That has the effect that copying will
	 * just reset the cache.  Likewise, can't compare or print them.
	 */
	List	   *scansel_cache pg_node_attr(copy_as(NIL), equal_ignore, read_write_ignore);

	/*
	 * transient workspace for use while considering a specific join path; T =
	 * outer var on left, F = on right
	 */
	bool		outer_is_left pg_node_attr(equal_ignore);

	/*
	 * copy of clause operator; valid if clause is hashjoinable, else
	 * InvalidOid
	 */
	Oid			hashjoinoperator pg_node_attr(equal_ignore);

	/*
	 * cache space for hashclause processing; -1 if not yet set
	 */
	/* avg bucketsize of left side */
	Selectivity left_bucketsize pg_node_attr(equal_ignore);
	/* avg bucketsize of right side */
	Selectivity right_bucketsize pg_node_attr(equal_ignore);
	/* left side's most common val's freq */
	Selectivity left_mcvfreq pg_node_attr(equal_ignore);
	/* right side's most common val's freq */
	Selectivity right_mcvfreq pg_node_attr(equal_ignore);

	/* hash equality operators used for memoize nodes, else InvalidOid */
	Oid			left_hasheqoperator pg_node_attr(equal_ignore);
	Oid			right_hasheqoperator pg_node_attr(equal_ignore);
} RestrictInfo;

/*
 * This macro embodies the correct way to test whether a RestrictInfo is
 * "pushed down" to a given outer join, that is, should be treated as a filter
 * clause rather than a join clause at that outer join.  This is certainly so
 * if is_pushed_down is true; but examining that is not sufficient anymore,
 * because outer-join clauses will get pushed down to lower outer joins when
 * we generate a path for the lower outer join that is parameterized by the
 * LHS of the upper one.  We can detect such a clause by noting that its
 * required_relids exceed the scope of the join.
 */
#define RINFO_IS_PUSHED_DOWN(rinfo, joinrelids) \
	((rinfo)->is_pushed_down || \
	 !bms_is_subset((rinfo)->required_relids, joinrelids))

/*
 * Since mergejoinscansel() is a relatively expensive function, and would
 * otherwise be invoked many times while planning a large join tree,
 * we go out of our way to cache its results.  Each mergejoinable
 * RestrictInfo carries a list of the specific sort orderings that have
 * been considered for use with it, and the resulting selectivities.
 */
typedef struct MergeScanSelCache
{
	/* Ordering details (cache lookup key) */
	Oid			opfamily;		/* btree opfamily defining the ordering */
	Oid			collation;		/* collation for the ordering */
	int			strategy;		/* sort direction (ASC or DESC) */
	bool		nulls_first;	/* do NULLs come before normal values? */
	/* Results */
	Selectivity leftstartsel;	/* first-join fraction for clause left side */
	Selectivity leftendsel;		/* last-join fraction for clause left side */
	Selectivity rightstartsel;	/* first-join fraction for clause right side */
	Selectivity rightendsel;	/* last-join fraction for clause right side */
} MergeScanSelCache;

/*
 * Placeholder node for an expression to be evaluated below the top level
 * of a plan tree.  This is used during planning to represent the contained
 * expression.  At the end of the planning process it is replaced by either
 * the contained expression or a Var referring to a lower-level evaluation of
 * the contained expression.  Generally the evaluation occurs below an outer
 * join, and Var references above the outer join might thereby yield NULL
 * instead of the expression value.
 *
 * phrels and phlevelsup correspond to the varno/varlevelsup fields of a
 * plain Var, except that phrels has to be a relid set since the evaluation
 * level of a PlaceHolderVar might be a join rather than a base relation.
 * Likewise, phnullingrels corresponds to varnullingrels.
 *
 * Although the planner treats this as an expression node type, it is not
 * recognized by the parser or executor, so we declare it here rather than
 * in primnodes.h.
 *
 * We intentionally do not compare phexpr.  Two PlaceHolderVars with the
 * same ID and levelsup should be considered equal even if the contained
 * expressions have managed to mutate to different states.  This will
 * happen during final plan construction when there are nested PHVs, since
 * the inner PHV will get replaced by a Param in some copies of the outer
 * PHV.  Another way in which it can happen is that initplan sublinks
 * could get replaced by differently-numbered Params when sublink folding
 * is done.  (The end result of such a situation would be some
 * unreferenced initplans, which is annoying but not really a problem.)
 * On the same reasoning, there is no need to examine phrels.  But we do
 * need to compare phnullingrels, as that represents effects that are
 * external to the original value of the PHV.
 */

typedef struct PlaceHolderVar
{
	pg_node_attr(no_query_jumble)

	Expr		xpr;

	/* the represented expression */
	Expr	   *phexpr pg_node_attr(equal_ignore);

	/* base+OJ relids syntactically within expr src */
	Relids		phrels pg_node_attr(equal_ignore);

	/* RT indexes of outer joins that can null PHV's value */
	Relids		phnullingrels;

	/* ID for PHV (unique within planner run) */
	Index		phid;

	/* > 0 if PHV belongs to outer query */
	Index		phlevelsup;
} PlaceHolderVar;

/*
 * "Special join" info.
 *
 * One-sided outer joins constrain the order of joining partially but not
 * completely.  We flatten such joins into the planner's top-level list of
 * relations to join, but record information about each outer join in a
 * SpecialJoinInfo struct.  These structs are kept in the PlannerInfo node's
 * join_info_list.
 *
 * Similarly, semijoins and antijoins created by flattening IN (subselect)
 * and EXISTS(subselect) clauses create partial constraints on join order.
 * These are likewise recorded in SpecialJoinInfo structs.
 *
 * We make SpecialJoinInfos for FULL JOINs even though there is no flexibility
 * of planning for them, because this simplifies make_join_rel()'s API.
 *
 * min_lefthand and min_righthand are the sets of base+OJ relids that must be
 * available on each side when performing the special join.
 * It is not valid for either min_lefthand or min_righthand to be empty sets;
 * if they were, this would break the logic that enforces join order.
 *
 * syn_lefthand and syn_righthand are the sets of base+OJ relids that are
 * syntactically below this special join.  (These are needed to help compute
 * min_lefthand and min_righthand for higher joins.)
 *
 * jointype is never JOIN_RIGHT; a RIGHT JOIN is handled by switching
 * the inputs to make it a LEFT JOIN.  It's never JOIN_RIGHT_ANTI either.
 * So the allowed values of jointype in a join_info_list member are only
 * LEFT, FULL, SEMI, or ANTI.
 *
 * ojrelid is the RT index of the join RTE representing this outer join,
 * if there is one.  It is zero when jointype is INNER or SEMI, and can be
 * zero for jointype ANTI (if the join was transformed from a SEMI join).
 * One use for this field is that when constructing the output targetlist of a
 * join relation that implements this OJ, we add ojrelid to the varnullingrels
 * and phnullingrels fields of nullable (RHS) output columns, so that the
 * output Vars and PlaceHolderVars correctly reflect the nulling that has
 * potentially happened to them.
 *
 * commute_above_l is filled with the relids of syntactically-higher outer
 * joins that have been found to commute with this one per outer join identity
 * 3 (see optimizer/README), when this join is in the LHS of the upper join
 * (so, this is the lower join in the first form of the identity).
 *
 * commute_above_r is filled with the relids of syntactically-higher outer
 * joins that have been found to commute with this one per outer join identity
 * 3, when this join is in the RHS of the upper join (so, this is the lower
 * join in the second form of the identity).
 *
 * commute_below_l is filled with the relids of syntactically-lower outer
 * joins that have been found to commute with this one per outer join identity
 * 3 and are in the LHS of this join (so, this is the upper join in the first
 * form of the identity).
 *
 * commute_below_r is filled with the relids of syntactically-lower outer
 * joins that have been found to commute with this one per outer join identity
 * 3 and are in the RHS of this join (so, this is the upper join in the second
 * form of the identity).
 *
 * lhs_strict is true if the special join's condition cannot succeed when the
 * LHS variables are all NULL (this means that an outer join can commute with
 * upper-level outer joins even if it appears in their RHS).  We don't bother
 * to set lhs_strict for FULL JOINs, however.
 *
 * For a semijoin, we also extract the join operators and their RHS arguments
 * and set semi_operators, semi_rhs_exprs, semi_can_btree, and semi_can_hash.
 * This is done in support of possibly unique-ifying the RHS, so we don't
 * bother unless at least one of semi_can_btree and semi_can_hash can be set
 * true.  (You might expect that this information would be computed during
 * join planning; but it's helpful to have it available during planning of
 * parameterized table scans, so we store it in the SpecialJoinInfo structs.)
 *
 * For purposes of join selectivity estimation, we create transient
 * SpecialJoinInfo structures for regular inner joins; so it is possible
 * to have jointype == JOIN_INNER in such a structure, even though this is
 * not allowed within join_info_list.  We also create transient
 * SpecialJoinInfos with jointype == JOIN_INNER for outer joins, since for
 * cost estimation purposes it is sometimes useful to know the join size under
 * plain innerjoin semantics.  Note that lhs_strict and the semi_xxx fields
 * are not set meaningfully within such structs.
 *
 * We also create transient SpecialJoinInfos for child joins during
 * partitionwise join planning, which are also not present in join_info_list.
 */
#ifndef HAVE_SPECIALJOININFO_TYPEDEF
typedef struct SpecialJoinInfo SpecialJoinInfo;
#define HAVE_SPECIALJOININFO_TYPEDEF 1
#endif

struct SpecialJoinInfo
{
	pg_node_attr(no_read, no_query_jumble)

	NodeTag		type;
	Relids		min_lefthand;	/* base+OJ relids in minimum LHS for join */
	Relids		min_righthand;	/* base+OJ relids in minimum RHS for join */
	Relids		syn_lefthand;	/* base+OJ relids syntactically within LHS */
	Relids		syn_righthand;	/* base+OJ relids syntactically within RHS */
	JoinType	jointype;		/* always INNER, LEFT, FULL, SEMI, or ANTI */
	Index		ojrelid;		/* outer join's RT index; 0 if none */
	Relids		commute_above_l;	/* commuting OJs above this one, if LHS */
	Relids		commute_above_r;	/* commuting OJs above this one, if RHS */
	Relids		commute_below_l;	/* commuting OJs in this one's LHS */
	Relids		commute_below_r;	/* commuting OJs in this one's RHS */
	bool		lhs_strict;		/* joinclause is strict for some LHS rel */
	/* Remaining fields are set only for JOIN_SEMI jointype: */
	bool		semi_can_btree; /* true if semi_operators are all btree */
	bool		semi_can_hash;	/* true if semi_operators are all hash */
	List	   *semi_operators; /* OIDs of equality join operators */
	List	   *semi_rhs_exprs; /* righthand-side expressions of these ops */
};

/*
 * Transient outer-join clause info.
 *
 * We set aside every outer join ON clause that looks mergejoinable,
 * and process it specially at the end of qual distribution.
 */
typedef struct OuterJoinClauseInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;
	RestrictInfo *rinfo;		/* a mergejoinable outer-join clause */
	SpecialJoinInfo *sjinfo;	/* the outer join's SpecialJoinInfo */
} OuterJoinClauseInfo;

/*
 * Append-relation info.
 *
 * When we expand an inheritable table or a UNION-ALL subselect into an
 * "append relation" (essentially, a list of child RTEs), we build an
 * AppendRelInfo for each child RTE.  The list of AppendRelInfos indicates
 * which child RTEs must be included when expanding the parent, and each node
 * carries information needed to translate between columns of the parent and
 * columns of the child.
 *
 * These structs are kept in the PlannerInfo node's append_rel_list, with
 * append_rel_array[] providing a convenient lookup method for the struct
 * associated with a particular child relid (there can be only one, though
 * parent rels may have many entries in append_rel_list).
 *
 * Note: after completion of the planner prep phase, any given RTE is an
 * append parent having entries in append_rel_list if and only if its
 * "inh" flag is set.  We clear "inh" for plain tables that turn out not
 * to have inheritance children, and (in an abuse of the original meaning
 * of the flag) we set "inh" for subquery RTEs that turn out to be
 * flattenable UNION ALL queries.  This lets us avoid useless searches
 * of append_rel_list.
 *
 * Note: the data structure assumes that append-rel members are single
 * baserels.  This is OK for inheritance, but it prevents us from pulling
 * up a UNION ALL member subquery if it contains a join.  While that could
 * be fixed with a more complex data structure, at present there's not much
 * point because no improvement in the plan could result.
 */

typedef struct AppendRelInfo
{
	pg_node_attr(no_query_jumble)

	NodeTag		type;

	/*
	 * These fields uniquely identify this append relationship.  There can be
	 * (in fact, always should be) multiple AppendRelInfos for the same
	 * parent_relid, but never more than one per child_relid, since a given
	 * RTE cannot be a child of more than one append parent.
	 */
	Index		parent_relid;	/* RT index of append parent rel */
	Index		child_relid;	/* RT index of append child rel */

	/*
	 * For an inheritance appendrel, the parent and child are both regular
	 * relations, and we store their rowtype OIDs here for use in translating
	 * whole-row Vars.  For a UNION-ALL appendrel, the parent and child are
	 * both subqueries with no named rowtype, and we store InvalidOid here.
	 */
	Oid			parent_reltype; /* OID of parent's composite type */
	Oid			child_reltype;	/* OID of child's composite type */

	/*
	 * The N'th element of this list is a Var or expression representing the
	 * child column corresponding to the N'th column of the parent. This is
	 * used to translate Vars referencing the parent rel into references to
	 * the child.  A list element is NULL if it corresponds to a dropped
	 * column of the parent (this is only possible for inheritance cases, not
	 * UNION ALL).  The list elements are always simple Vars for inheritance
	 * cases, but can be arbitrary expressions in UNION ALL cases.
	 *
	 * Notice we only store entries for user columns (attno > 0).  Whole-row
	 * Vars are special-cased, and system columns (attno < 0) need no special
	 * translation since their attnos are the same for all tables.
	 *
	 * Caution: the Vars have varlevelsup = 0.  Be careful to adjust as needed
	 * when copying into a subquery.
	 */
	List	   *translated_vars;	/* Expressions in the child's Vars */

	/*
	 * This array simplifies translations in the reverse direction, from
	 * child's column numbers to parent's.  The entry at [ccolno - 1] is the
	 * 1-based parent column number for child column ccolno, or zero if that
	 * child column is dropped or doesn't exist in the parent.
	 */
	int			num_child_cols; /* length of array */
	AttrNumber *parent_colnos pg_node_attr(array_size(num_child_cols));

	/*
	 * We store the parent table's OID here for inheritance, or InvalidOid for
	 * UNION ALL.  This is only needed to help in generating error messages if
	 * an attempt is made to reference a dropped parent column.
	 */
	Oid			parent_reloid;	/* OID of parent relation */
} AppendRelInfo;

/*
 * Information about a row-identity "resjunk" column in UPDATE/DELETE/MERGE.
 *
 * In partitioned UPDATE/DELETE/MERGE it's important for child partitions to
 * share row-identity columns whenever possible, so as not to chew up too many
 * targetlist columns.  We use these structs to track which identity columns
 * have been requested.  In the finished plan, each of these will give rise
 * to one resjunk entry in the targetlist of the ModifyTable's subplan node.
 *
 * All the Vars stored in RowIdentityVarInfos must have varno ROWID_VAR, for
 * convenience of detecting duplicate requests.  We'll replace that, in the
 * final plan, with the varno of the generating rel.
 *
 * Outside this list, a Var with varno ROWID_VAR and varattno k is a reference
 * to the k-th element of the row_identity_vars list (k counting from 1).
 * We add such a reference to root->processed_tlist when creating the entry,
 * and it propagates into the plan tree from there.
 */
typedef struct RowIdentityVarInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Var		   *rowidvar;		/* Var to be evaluated (but varno=ROWID_VAR) */
	int32		rowidwidth;		/* estimated average width */
	char	   *rowidname;		/* name of the resjunk column */
	Relids		rowidrels;		/* RTE indexes of target rels using this */
} RowIdentityVarInfo;

/*
 * For each distinct placeholder expression generated during planning, we
 * store a PlaceHolderInfo node in the PlannerInfo node's placeholder_list.
 * This stores info that is needed centrally rather than in each copy of the
 * PlaceHolderVar.  The phid fields identify which PlaceHolderInfo goes with
 * each PlaceHolderVar.  Note that phid is unique throughout a planner run,
 * not just within a query level --- this is so that we need not reassign ID's
 * when pulling a subquery into its parent.
 *
 * The idea is to evaluate the expression at (only) the ph_eval_at join level,
 * then allow it to bubble up like a Var until the ph_needed join level.
 * ph_needed has the same definition as attr_needed for a regular Var.
 *
 * The PlaceHolderVar's expression might contain LATERAL references to vars
 * coming from outside its syntactic scope.  If so, those rels are *not*
 * included in ph_eval_at, but they are recorded in ph_lateral.
 *
 * Notice that when ph_eval_at is a join rather than a single baserel, the
 * PlaceHolderInfo may create constraints on join order: the ph_eval_at join
 * has to be formed below any outer joins that should null the PlaceHolderVar.
 *
 * We create a PlaceHolderInfo only after determining that the PlaceHolderVar
 * is actually referenced in the plan tree, so that unreferenced placeholders
 * don't result in unnecessary constraints on join order.
 */

typedef struct PlaceHolderInfo
{
	pg_node_attr(no_read, no_query_jumble)

	NodeTag		type;

	/* ID for PH (unique within planner run) */
	Index		phid;

	/*
	 * copy of PlaceHolderVar tree (should be redundant for comparison, could
	 * be ignored)
	 */
	PlaceHolderVar *ph_var;

	/* lowest level we can evaluate value at */
	Relids		ph_eval_at;

	/* relids of contained lateral refs, if any */
	Relids		ph_lateral;

	/* highest level the value is needed at */
	Relids		ph_needed;

	/* estimated attribute width */
	int32		ph_width;
} PlaceHolderInfo;

/*
 * This struct describes one potentially index-optimizable MIN/MAX aggregate
 * function.  MinMaxAggPath contains a list of these, and if we accept that
 * path, the list is stored into root->minmax_aggs for use during setrefs.c.
 */
typedef struct MinMaxAggInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* pg_proc Oid of the aggregate */
	Oid			aggfnoid;

	/* Oid of its sort operator */
	Oid			aggsortop;

	/* expression we are aggregating on */
	Expr	   *target;

	/*
	 * modified "root" for planning the subquery; not printed, too large, not
	 * interesting enough
	 */
	PlannerInfo *subroot pg_node_attr(read_write_ignore);

	/* access path for subquery */
	Path	   *path;

	/* estimated cost to fetch first row */
	Cost		pathcost;

	/* param for subplan's output */
	Param	   *param;
} MinMaxAggInfo;

/*
 * At runtime, PARAM_EXEC slots are used to pass values around from one plan
 * node to another.  They can be used to pass values down into subqueries (for
 * outer references in subqueries), or up out of subqueries (for the results
 * of a subplan), or from a NestLoop plan node into its inner relation (when
 * the inner scan is parameterized with values from the outer relation).
 * The planner is responsible for assigning nonconflicting PARAM_EXEC IDs to
 * the PARAM_EXEC Params it generates.
 *
 * Outer references are managed via root->plan_params, which is a list of
 * PlannerParamItems.  While planning a subquery, each parent query level's
 * plan_params contains the values required from it by the current subquery.
 * During create_plan(), we use plan_params to track values that must be
 * passed from outer to inner sides of NestLoop plan nodes.
 *
 * The item a PlannerParamItem represents can be one of three kinds:
 *
 * A Var: the slot represents a variable of this level that must be passed
 * down because subqueries have outer references to it, or must be passed
 * from a NestLoop node to its inner scan.  The varlevelsup value in the Var
 * will always be zero.
 *
 * A PlaceHolderVar: this works much like the Var case, except that the
 * entry is a PlaceHolderVar node with a contained expression.  The PHV
 * will have phlevelsup = 0, and the contained expression is adjusted
 * to match in level.
 *
 * An Aggref (with an expression tree representing its argument): the slot
 * represents an aggregate expression that is an outer reference for some
 * subquery.  The Aggref itself has agglevelsup = 0, and its argument tree
 * is adjusted to match in level.
 *
 * Note: we detect duplicate Var and PlaceHolderVar parameters and coalesce
 * them into one slot, but we do not bother to do that for Aggrefs.
 * The scope of duplicate-elimination only extends across the set of
 * parameters passed from one query level into a single subquery, or for
 * nestloop parameters across the set of nestloop parameters used in a single
 * query level.  So there is no possibility of a PARAM_EXEC slot being used
 * for conflicting purposes.
 *
 * In addition, PARAM_EXEC slots are assigned for Params representing outputs
 * from subplans (values that are setParam items for those subplans).  These
 * IDs need not be tracked via PlannerParamItems, since we do not need any
 * duplicate-elimination nor later processing of the represented expressions.
 * Instead, we just record the assignment of the slot number by appending to
 * root->glob->paramExecTypes.
 */
typedef struct PlannerParamItem
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	Node	   *item;			/* the Var, PlaceHolderVar, or Aggref */
	int			paramId;		/* its assigned PARAM_EXEC slot number */
} PlannerParamItem;

/*
 * When making cost estimates for a SEMI/ANTI/inner_unique join, there are
 * some correction factors that are needed in both nestloop and hash joins
 * to account for the fact that the executor can stop scanning inner rows
 * as soon as it finds a match to the current outer row.  These numbers
 * depend only on the selected outer and inner join relations, not on the
 * particular paths used for them, so it's worthwhile to calculate them
 * just once per relation pair not once per considered path.  This struct
 * is filled by compute_semi_anti_join_factors and must be passed along
 * to the join cost estimation functions.
 *
 * outer_match_frac is the fraction of the outer tuples that are
 *		expected to have at least one match.
 * match_count is the average number of matches expected for
 *		outer tuples that have at least one match.
 */
typedef struct SemiAntiJoinFactors
{
	Selectivity outer_match_frac;
	Selectivity match_count;
} SemiAntiJoinFactors;

/*
 * Struct for extra information passed to subroutines of add_paths_to_joinrel
 *
 * restrictlist contains all of the RestrictInfo nodes for restriction
 *		clauses that apply to this join
 * mergeclause_list is a list of RestrictInfo nodes for available
 *		mergejoin clauses in this join
 * inner_unique is true if each outer tuple provably matches no more
 *		than one inner tuple
 * sjinfo is extra info about special joins for selectivity estimation
 * semifactors is as shown above (only valid for SEMI/ANTI/inner_unique joins)
 * param_source_rels are OK targets for parameterization of result paths
 */
typedef struct JoinPathExtraData
{
	List	   *restrictlist;
	List	   *mergeclause_list;
	bool		inner_unique;
	SpecialJoinInfo *sjinfo;
	SemiAntiJoinFactors semifactors;
	Relids		param_source_rels;
} JoinPathExtraData;

/*
 * Various flags indicating what kinds of grouping are possible.
 *
 * GROUPING_CAN_USE_SORT should be set if it's possible to perform
 * sort-based implementations of grouping.  When grouping sets are in use,
 * this will be true if sorting is potentially usable for any of the grouping
 * sets, even if it's not usable for all of them.
 *
 * GROUPING_CAN_USE_HASH should be set if it's possible to perform
 * hash-based implementations of grouping.
 *
 * GROUPING_CAN_PARTIAL_AGG should be set if the aggregation is of a type
 * for which we support partial aggregation (not, for example, grouping sets).
 * It says nothing about parallel-safety or the availability of suitable paths.
 */
#define GROUPING_CAN_USE_SORT       0x0001
#define GROUPING_CAN_USE_HASH       0x0002
#define GROUPING_CAN_PARTIAL_AGG	0x0004

/*
 * What kind of partitionwise aggregation is in use?
 *
 * PARTITIONWISE_AGGREGATE_NONE: Not used.
 *
 * PARTITIONWISE_AGGREGATE_FULL: Aggregate each partition separately, and
 * append the results.
 *
 * PARTITIONWISE_AGGREGATE_PARTIAL: Partially aggregate each partition
 * separately, append the results, and then finalize aggregation.
 */
typedef enum
{
	PARTITIONWISE_AGGREGATE_NONE,
	PARTITIONWISE_AGGREGATE_FULL,
	PARTITIONWISE_AGGREGATE_PARTIAL,
} PartitionwiseAggregateType;

/*
 * Struct for extra information passed to subroutines of create_grouping_paths
 *
 * flags indicating what kinds of grouping are possible.
 * partial_costs_set is true if the agg_partial_costs and agg_final_costs
 * 		have been initialized.
 * agg_partial_costs gives partial aggregation costs.
 * agg_final_costs gives finalization costs.
 * target_parallel_safe is true if target is parallel safe.
 * havingQual gives list of quals to be applied after aggregation.
 * targetList gives list of columns to be projected.
 * patype is the type of partitionwise aggregation that is being performed.
 */
typedef struct
{
	/* Data which remains constant once set. */
	int			flags;
	bool		partial_costs_set;
	AggClauseCosts agg_partial_costs;
	AggClauseCosts agg_final_costs;

	/* Data which may differ across partitions. */
	bool		target_parallel_safe;
	Node	   *havingQual;
	List	   *targetList;
	PartitionwiseAggregateType patype;
} GroupPathExtraData;

/*
 * Struct for extra information passed to subroutines of grouping_planner
 *
 * limit_needed is true if we actually need a Limit plan node.
 * limit_tuples is an estimated bound on the number of output tuples,
 *		or -1 if no LIMIT or couldn't estimate.
 * count_est and offset_est are the estimated values of the LIMIT and OFFSET
 * 		expressions computed by preprocess_limit() (see comments for
 * 		preprocess_limit() for more information).
 */
typedef struct
{
	bool		limit_needed;
	Cardinality limit_tuples;
	int64		count_est;
	int64		offset_est;
} FinalPathExtraData;

/*
 * For speed reasons, cost estimation for join paths is performed in two
 * phases: the first phase tries to quickly derive a lower bound for the
 * join cost, and then we check if that's sufficient to reject the path.
 * If not, we come back for a more refined cost estimate.  The first phase
 * fills a JoinCostWorkspace struct with its preliminary cost estimates
 * and possibly additional intermediate values.  The second phase takes
 * these values as inputs to avoid repeating work.
 *
 * (Ideally we'd declare this in cost.h, but it's also needed in pathnode.h,
 * so seems best to put it here.)
 */
typedef struct JoinCostWorkspace
{
	/* Preliminary cost estimates --- must not be larger than final ones! */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	/* Fields below here should be treated as private to costsize.c */
	Cost		run_cost;		/* non-startup cost components */

	/* private for cost_nestloop code */
	Cost		inner_run_cost; /* also used by cost_mergejoin code */
	Cost		inner_rescan_run_cost;

	/* private for cost_mergejoin code */
	Cardinality outer_rows;
	Cardinality inner_rows;
	Cardinality outer_skip_rows;
	Cardinality inner_skip_rows;

	/* private for cost_hashjoin code */
	int			numbuckets;
	int			numbatches;
	Cardinality inner_rows_total;
} JoinCostWorkspace;

/*
 * AggInfo holds information about an aggregate that needs to be computed.
 * Multiple Aggrefs in a query can refer to the same AggInfo by having the
 * same 'aggno' value, so that the aggregate is computed only once.
 */
typedef struct AggInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/*
	 * List of Aggref exprs that this state value is for.
	 *
	 * There will always be at least one, but there can be multiple identical
	 * Aggref's sharing the same per-agg.
	 */
	List	   *aggrefs;

	/* Transition state number for this aggregate */
	int			transno;

	/*
	 * "shareable" is false if this agg cannot share state values with other
	 * aggregates because the final function is read-write.
	 */
	bool		shareable;

	/* Oid of the final function, or InvalidOid if none */
	Oid			finalfn_oid;
} AggInfo;

/*
 * AggTransInfo holds information about transition state that is used by one
 * or more aggregates in the query.  Multiple aggregates can share the same
 * transition state, if they have the same inputs and the same transition
 * function.  Aggrefs that share the same transition info have the same
 * 'aggtransno' value.
 */
typedef struct AggTransInfo
{
	pg_node_attr(no_copy_equal, no_read, no_query_jumble)

	NodeTag		type;

	/* Inputs for this transition state */
	List	   *args;
	Expr	   *aggfilter;

	/* Oid of the state transition function */
	Oid			transfn_oid;

	/* Oid of the serialization function, or InvalidOid if none */
	Oid			serialfn_oid;

	/* Oid of the deserialization function, or InvalidOid if none */
	Oid			deserialfn_oid;

	/* Oid of the combine function, or InvalidOid if none */
	Oid			combinefn_oid;

	/* Oid of state value's datatype */
	Oid			aggtranstype;

	/* Additional data about transtype */
	int32		aggtranstypmod;
	int			transtypeLen;
	bool		transtypeByVal;

	/* Space-consumption estimate */
	int32		aggtransspace;

	/* Initial value from pg_aggregate entry */
	Datum		initValue pg_node_attr(read_write_ignore);
	bool		initValueIsNull;
} AggTransInfo;

#endif							/* PATHNODES_H */
