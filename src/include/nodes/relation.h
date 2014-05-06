/*-------------------------------------------------------------------------
 *
 * relation.h
 *	  Definitions for planner's internal data structures.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/nodes/relation.h,v 1.187 2010/07/06 19:19:00 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELATION_H
#define RELATION_H

#include "access/sdir.h"
#include "nodes/bitmapset.h"
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


/*----------
 * PlannerGlobal
 *		Global information for planning/optimization
 *
 * PlannerGlobal holds state for an entire planner invocation; this state
 * is shared across all levels of sub-Queries that exist in the command being
 * planned.
 *----------
 */
typedef struct PlannerGlobal
{
	NodeTag		type;

	ParamListInfo boundParams;	/* Param values provided to planner() */

	List	   *paramlist;		/* unused, will be removed in 9.3 */

	List	   *subplans;		/* Plans for SubPlan nodes */

	List	   *subrtables;		/* Rangetables for SubPlan nodes */

	List	   *subrowmarks;	/* PlanRowMarks for SubPlan nodes */

	Bitmapset  *rewindPlanIDs;	/* indices of subplans that require REWIND */

	List	   *finalrtable;	/* "flat" rangetable for executor */

	List	   *finalrowmarks;	/* "flat" list of PlanRowMarks */

	List	   *relationOids;	/* OIDs of relations the plan depends on */

	List	   *invalItems;		/* other dependencies, as PlanInvalItems */

	Index		lastPHId;		/* highest PlaceHolderVar ID assigned */

	Index		lastRowMarkId;	/* highest PlanRowMark ID assigned */

	bool		transientPlan;	/* redo plan when TransactionXmin changes? */

	/* Added post-release, will be in a saner place in 9.3: */
	int			nParamExec;		/* number of PARAM_EXEC Params used */
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
 *----------
 */
typedef struct PlannerInfo
{
	NodeTag		type;

	Query	   *parse;			/* the Query being planned */

	PlannerGlobal *glob;		/* global info for current planner run */

	Index		query_level;	/* 1 at the outermost Query */

	struct PlannerInfo *parent_root;	/* NULL at outermost Query */

	/*
	 * simple_rel_array holds pointers to "base rels" and "other rels" (see
	 * comments for RelOptInfo for more info).  It is indexed by rangetable
	 * index (so entry 0 is always wasted).  Entries can be NULL when an RTE
	 * does not correspond to a base relation, such as a join RTE or an
	 * unreferenced view RTE; or if the RelOptInfo hasn't been made yet.
	 */
	struct RelOptInfo **simple_rel_array;		/* All 1-rel RelOptInfos */
	int			simple_rel_array_size;	/* allocated size of array */

	/*
	 * simple_rte_array is the same length as simple_rel_array and holds
	 * pointers to the associated rangetable entries.  This lets us avoid
	 * rt_fetch(), which can be a bit slow once large inheritance sets have
	 * been expanded.
	 */
	RangeTblEntry **simple_rte_array;	/* rangetable as an array */

	/*
	 * join_rel_list is a list of all join-relation RelOptInfos we have
	 * considered in this planning run.  For small problems we just scan the
	 * list to do lookups, but when there are many join relations we build a
	 * hash table for faster lookups.  The hash table is present and valid
	 * when join_rel_hash is not NULL.  Note that we still maintain the list
	 * even when using the hash table for lookups; this simplifies life for
	 * GEQO.
	 */
	List	   *join_rel_list;	/* list of join-relation RelOptInfos */
	struct HTAB *join_rel_hash; /* optional hashtable for join relations */

	/*
	 * When doing a dynamic-programming-style join search, join_rel_level[k]
	 * is a list of all join-relation RelOptInfos of level k, and
	 * join_cur_level is the current level.  New join-relation RelOptInfos are
	 * automatically added to the join_rel_level[join_cur_level] list.
	 * join_rel_level is NULL if not in use.
	 */
	List	  **join_rel_level; /* lists of join-relation RelOptInfos */
	int			join_cur_level; /* index of list being extended */

	List	   *resultRelations;	/* integer list of RT indexes, or NIL */

	List	   *init_plans;		/* init SubPlans for query */

	List	   *cte_plan_ids;	/* per-CTE-item list of subplan IDs */

	List	   *eq_classes;		/* list of active EquivalenceClasses */

	List	   *canon_pathkeys; /* list of "canonical" PathKeys */

	List	   *left_join_clauses;		/* list of RestrictInfos for
										 * mergejoinable outer join clauses
										 * w/nonnullable var on left */

	List	   *right_join_clauses;		/* list of RestrictInfos for
										 * mergejoinable outer join clauses
										 * w/nonnullable var on right */

	List	   *full_join_clauses;		/* list of RestrictInfos for
										 * mergejoinable full join clauses */

	List	   *join_info_list; /* list of SpecialJoinInfos */

	List	   *append_rel_list;	/* list of AppendRelInfos */

	List	   *rowMarks;		/* list of PlanRowMarks */

	List	   *placeholder_list;		/* list of PlaceHolderInfos */

	List	   *query_pathkeys; /* desired pathkeys for query_planner(), and
								 * actual pathkeys afterwards */

	List	   *group_pathkeys; /* groupClause pathkeys, if any */
	List	   *window_pathkeys;	/* pathkeys of bottom window, if any */
	List	   *distinct_pathkeys;		/* distinctClause pathkeys, if any */
	List	   *sort_pathkeys;	/* sortClause pathkeys, if any */

	List	   *initial_rels;	/* RelOptInfos we are now trying to join */

	MemoryContext planner_cxt;	/* context holding PlannerInfo */

	double		total_table_pages;		/* # of pages in all tables of query */

	double		tuple_fraction; /* tuple_fraction passed to query_planner */

	bool		hasInheritedTarget;		/* true if parse->resultRelation is an
										 * inheritance child rel */
	bool		hasJoinRTEs;	/* true if any RTEs are RTE_JOIN kind */
	bool		hasHavingQual;	/* true if havingQual was non-null */
	bool		hasPseudoConstantQuals; /* true if any RestrictInfo has
										 * pseudoconstant = true */
	bool		hasRecursion;	/* true if planning a recursive WITH item */

	/* These fields are used only when hasRecursion is true: */
	int			wt_param_id;	/* PARAM_EXEC ID for the work table */
	struct Plan *non_recursive_plan;	/* plan for non-recursive term */

	/* optional private data for join_search_hook, e.g., GEQO */
	void	   *join_search_private;

	/* Added post-release, will be in a saner place in 9.3: */
	List	   *plan_params;	/* list of PlannerParamItems, see below */
} PlannerInfo;


/*
 * In places where it's known that simple_rte_array[] must have been prepared
 * already, we just index into it to fetch RTEs.  In code that might be
 * executed before or after entering query_planner(), use this macro.
 */
#define planner_rt_fetch(rti, root) \
	((root)->simple_rte_array ? (root)->simple_rte_array[rti] : \
	 rt_fetch(rti, (root)->parse->rtable))


/*----------
 * RelOptInfo
 *		Per-relation information for planning/optimization
 *
 * For planning purposes, a "base rel" is either a plain relation (a table)
 * or the output of a sub-SELECT or function that appears in the range table.
 * In either case it is uniquely identified by an RT index.  A "joinrel"
 * is the joining of two or more base rels.  A joinrel is identified by
 * the set of RT indexes for its component baserels.  We create RelOptInfo
 * nodes for each baserel and joinrel, and store them in the PlannerInfo's
 * simple_rel_array and join_rel_list respectively.
 *
 * Note that there is only one joinrel for any given set of component
 * baserels, no matter what order we assemble them in; so an unordered
 * set is the right datatype to identify it with.
 *
 * We also have "other rels", which are like base rels in that they refer to
 * single RT indexes; but they are not part of the join tree, and are given
 * a different RelOptKind to identify them.  Lastly, there is a RelOptKind
 * for "dead" relations, which are base rels that we have proven we don't
 * need to join after all.
 *
 * Currently the only kind of otherrels are those made for member relations
 * of an "append relation", that is an inheritance set or UNION ALL subquery.
 * An append relation has a parent RTE that is a base rel, which represents
 * the entire append relation.  The member RTEs are otherrels.  The parent
 * is present in the query join tree but the members are not.  The member
 * RTEs and otherrels are used to plan the scans of the individual tables or
 * subqueries of the append set; then the parent baserel is given an Append
 * plan comprising the best plans for the individual member rels.  (See
 * comments for AppendRelInfo for more information.)
 *
 * At one time we also made otherrels to represent join RTEs, for use in
 * handling join alias Vars.  Currently this is not needed because all join
 * alias Vars are expanded to non-aliased form during preprocess_expression.
 *
 * Parts of this data structure are specific to various scan and join
 * mechanisms.  It didn't seem worth creating new node types for them.
 *
 *		relids - Set of base-relation identifiers; it is a base relation
 *				if there is just one, a join relation if more than one
 *		rows - estimated number of tuples in the relation after restriction
 *			   clauses have been applied (ie, output rows of a plan for it)
 *		width - avg. number of bytes per tuple in the relation after the
 *				appropriate projections have been done (ie, output width)
 *		reltargetlist - List of Var and PlaceHolderVar nodes for the values
 *						we need to output from this relation.
 *						List is in no particular order, but all rels of an
 *						appendrel set must use corresponding orders.
 *						NOTE: in a child relation, may contain RowExpr or
 *						ConvertRowtypeExpr representing a whole-row Var.
 *		pathlist - List of Path nodes, one for each potentially useful
 *				   method of generating the relation
 *		cheapest_startup_path - the pathlist member with lowest startup cost
 *								(regardless of its ordering)
 *		cheapest_total_path - the pathlist member with lowest total cost
 *							  (regardless of its ordering)
 *		cheapest_unique_path - for caching cheapest path to produce unique
 *							   (no duplicates) output from relation
 *
 * If the relation is a base relation it will have these fields set:
 *
 *		relid - RTE index (this is redundant with the relids field, but
 *				is provided for convenience of access)
 *		rtekind - distinguishes plain relation, subquery, or function RTE
 *		min_attr, max_attr - range of valid AttrNumbers for rel
 *		attr_needed - array of bitmapsets indicating the highest joinrel
 *				in which each attribute is needed; if bit 0 is set then
 *				the attribute is needed as part of final targetlist
 *		attr_widths - cache space for per-attribute width estimates;
 *					  zero means not computed yet
 *		indexlist - list of IndexOptInfo nodes for relation's indexes
 *					(always NIL if it's not a table)
 *		pages - number of disk pages in relation (zero if not a table)
 *		tuples - number of tuples in relation (not considering restrictions)
 *		subplan - plan for subquery (NULL if it's not a subquery)
 *		subrtable - rangetable for subquery (NIL if it's not a subquery)
 *		subrowmark - rowmarks for subquery (NIL if it's not a subquery)
 *
 *		Note: for a subquery, tuples and subplan are not set immediately
 *		upon creation of the RelOptInfo object; they are filled in when
 *		set_base_rel_pathlist processes the object.
 *
 *		For otherrels that are appendrel members, these fields are filled
 *		in just as for a baserel.
 *
 * The presence of the remaining fields depends on the restrictions
 * and joins that the relation participates in:
 *
 *		baserestrictinfo - List of RestrictInfo nodes, containing info about
 *					each non-join qualification clause in which this relation
 *					participates (only used for base rels)
 *		baserestrictcost - Estimated cost of evaluating the baserestrictinfo
 *					clauses at a single tuple (only used for base rels)
 *		joininfo  - List of RestrictInfo nodes, containing info about each
 *					join clause in which this relation participates (but
 *					note this excludes clauses that might be derivable from
 *					EquivalenceClasses)
 *		has_eclass_joins - flag that EquivalenceClass joins are possible
 *		index_outer_relids - only used for base rels; set of outer relids
 *					that participate in indexable joinclauses for this rel
 *		index_inner_paths - only used for base rels; list of InnerIndexscanInfo
 *					nodes showing best indexpaths for various subsets of
 *					index_outer_relids.
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
 *----------
 */
typedef enum RelOptKind
{
	RELOPT_BASEREL,
	RELOPT_JOINREL,
	RELOPT_OTHER_MEMBER_REL,
	RELOPT_DEADREL
} RelOptKind;

typedef struct RelOptInfo
{
	NodeTag		type;

	RelOptKind	reloptkind;

	/* all relations included in this RelOptInfo */
	Relids		relids;			/* set of base relids (rangetable indexes) */

	/* size estimates generated by planner */
	double		rows;			/* estimated number of result tuples */
	int			width;			/* estimated avg width of result tuples */

	/* materialization information */
	List	   *reltargetlist;	/* Vars to be output by scan of relation */
	List	   *pathlist;		/* Path structures */
	struct Path *cheapest_startup_path;
	struct Path *cheapest_total_path;
	struct Path *cheapest_unique_path;

	/* information about a base rel (not set for join rels!) */
	Index		relid;
	Oid			reltablespace;	/* containing tablespace */
	RTEKind		rtekind;		/* RELATION, SUBQUERY, or FUNCTION */
	AttrNumber	min_attr;		/* smallest attrno of rel (often <0) */
	AttrNumber	max_attr;		/* largest attrno of rel */
	Relids	   *attr_needed;	/* array indexed [min_attr .. max_attr] */
	int32	   *attr_widths;	/* array indexed [min_attr .. max_attr] */
	List	   *indexlist;		/* list of IndexOptInfo */
	BlockNumber pages;
	double		tuples;
	struct Plan *subplan;		/* if subquery */
	List	   *subrtable;		/* if subquery */
	List	   *subrowmark;		/* if subquery */

	/* used by various scans and joins: */
	List	   *baserestrictinfo;		/* RestrictInfo structures (if base
										 * rel) */
	QualCost	baserestrictcost;		/* cost of evaluating the above */
	List	   *joininfo;		/* RestrictInfo structures for join clauses
								 * involving this rel */
	bool		has_eclass_joins;		/* T means joininfo is incomplete */

	/* cached info about inner indexscan paths for relation: */
	Relids		index_outer_relids;		/* other relids in indexable join
										 * clauses */
	List	   *index_inner_paths;		/* InnerIndexscanInfo nodes */

	/*
	 * Inner indexscans are not in the main pathlist because they are not
	 * usable except in specific join contexts.  We use the index_inner_paths
	 * list just to avoid recomputing the best inner indexscan repeatedly for
	 * similar outer relations.  See comments for InnerIndexscanInfo.
	 */
} RelOptInfo;

/*
 * IndexOptInfo
 *		Per-index information for planning/optimization
 *
 *		Prior to Postgres 7.0, RelOptInfo was used to describe both relations
 *		and indexes, but that created confusion without actually doing anything
 *		useful.  So now we have a separate IndexOptInfo struct for indexes.
 *
 *		opfamily[], indexkeys[], opcintype[], fwdsortop[], revsortop[],
 *		and nulls_first[] each have ncolumns entries.
 *		Note: for historical reasons, the opfamily array has an extra entry
 *		that is always zero.  Some code scans until it sees a zero entry,
 *		rather than looking at ncolumns.
 *
 *		Zeroes in the indexkeys[] array indicate index columns that are
 *		expressions; there is one element in indexprs for each such column.
 *
 *		For an unordered index, the sortop arrays contains zeroes.  Note that
 *		fwdsortop[] and nulls_first[] describe the sort ordering of a forward
 *		indexscan; we can also consider a backward indexscan, which will
 *		generate sort order described by revsortop/!nulls_first.
 *
 *		The indexprs and indpred expressions have been run through
 *		prepqual.c and eval_const_expressions() for ease of matching to
 *		WHERE clauses. indpred is in implicit-AND form.
 */
typedef struct IndexOptInfo
{
	NodeTag		type;

	Oid			indexoid;		/* OID of the index relation */
	Oid			reltablespace;	/* tablespace of index (not table) */
	RelOptInfo *rel;			/* back-link to index's table */

	/* statistics from pg_class */
	BlockNumber pages;			/* number of disk pages in index */
	double		tuples;			/* number of index tuples in index */

	/* index descriptor information */
	int			ncolumns;		/* number of columns in index */
	Oid		   *opfamily;		/* OIDs of operator families for columns */
	int		   *indexkeys;		/* column numbers of index's keys, or 0 */
	Oid		   *opcintype;		/* OIDs of opclass declared input data types */
	Oid		   *fwdsortop;		/* OIDs of sort operators for each column */
	Oid		   *revsortop;		/* OIDs of sort operators for backward scan */
	bool	   *nulls_first;	/* do NULLs come first in the sort order? */
	Oid			relam;			/* OID of the access method (in pg_am) */

	RegProcedure amcostestimate;	/* OID of the access method's cost fcn */

	List	   *indexprs;		/* expressions for non-simple index columns */
	List	   *indpred;		/* predicate if a partial index, else NIL */

	bool		predOK;			/* true if predicate matches query */
	bool		unique;			/* true if a unique index */
	bool		amoptionalkey;	/* can query omit key for the first column? */
	bool		amsearchnulls;	/* can AM search for NULL/NOT NULL entries? */
	bool		amhasgettuple;	/* does AM have amgettuple interface? */
	bool		amhasgetbitmap; /* does AM have amgetbitmap interface? */
	/* added in 9.0.4: */
	bool		hypothetical;	/* true if index doesn't really exist */
	/* added in 9.0.6: */
	bool		immediate;		/* is uniqueness enforced immediately? */
} IndexOptInfo;


/*
 * EquivalenceClasses
 *
 * Whenever we can determine that a mergejoinable equality clause A = B is
 * not delayed by any outer join, we create an EquivalenceClass containing
 * the expressions A and B to record this knowledge.  If we later find another
 * equivalence B = C, we add C to the existing EquivalenceClass; this may
 * require merging two existing EquivalenceClasses.  At the end of the qual
 * distribution process, we have sets of values that are known all transitively
 * equal to each other, where "equal" is according to the rules of the btree
 * operator family(s) shown in ec_opfamilies.  (We restrict an EC to contain
 * only equalities whose operators belong to the same set of opfamilies.  This
 * could probably be relaxed, but for now it's not worth the trouble, since
 * nearly all equality operators belong to only one btree opclass anyway.)
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
 * We allow equality clauses appearing below the nullable side of an outer join
 * to form EquivalenceClasses, but these have a slightly different meaning:
 * the included values might be all NULL rather than all the same non-null
 * values.  See src/backend/optimizer/README for more on that point.
 *
 * NB: if ec_merged isn't NULL, this class has been merged into another, and
 * should be ignored in favor of using the pointed-to class.
 */
typedef struct EquivalenceClass
{
	NodeTag		type;

	List	   *ec_opfamilies;	/* btree operator family OIDs */
	List	   *ec_members;		/* list of EquivalenceMembers */
	List	   *ec_sources;		/* list of generating RestrictInfos */
	List	   *ec_derives;		/* list of derived RestrictInfos */
	Relids		ec_relids;		/* all relids appearing in ec_members */
	bool		ec_has_const;	/* any pseudoconstants in ec_members? */
	bool		ec_has_volatile;	/* the (sole) member is a volatile expr */
	bool		ec_below_outer_join;	/* equivalence applies below an OJ */
	bool		ec_broken;		/* failed to generate needed clauses? */
	Index		ec_sortref;		/* originating sortclause label, or 0 */
	struct EquivalenceClass *ec_merged; /* set if merged into another EC */
} EquivalenceClass;

/*
 * If an EC contains a const and isn't below-outer-join, any PathKey depending
 * on it must be redundant, since there's only one possible value of the key.
 */
#define EC_MUST_BE_REDUNDANT(eclass)  \
	((eclass)->ec_has_const && !(eclass)->ec_below_outer_join)

/*
 * EquivalenceMember - one member expression of an EquivalenceClass
 *
 * em_is_child signifies that this element was built by transposing a member
 * for an inheritance parent relation to represent the corresponding expression
 * on an inheritance child.  The element should be ignored for all purposes
 * except constructing inner-indexscan paths for the child relation.  (Other
 * types of join are driven from transposed joininfo-list entries.)  Note
 * that the EC's ec_relids field does NOT include the child relation.
 *
 * em_datatype is usually the same as exprType(em_expr), but can be
 * different when dealing with a binary-compatible opfamily; in particular
 * anyarray_ops would never work without this.  Use em_datatype when
 * looking up a specific btree operator to work with this expression.
 */
typedef struct EquivalenceMember
{
	NodeTag		type;

	Expr	   *em_expr;		/* the expression represented */
	Relids		em_relids;		/* all relids appearing in em_expr */
	Relids		em_nullable_relids;		/* nullable by lower outer joins */
	bool		em_is_const;	/* expression is pseudoconstant? */
	bool		em_is_child;	/* derived version for a child relation? */
	Oid			em_datatype;	/* the "nominal type" used by the opfamily */
} EquivalenceMember;

/*
 * PathKeys
 *
 * The sort ordering of a path is represented by a list of PathKey nodes.
 * An empty list implies no known ordering.  Otherwise the first item
 * represents the primary sort key, the second the first secondary sort key,
 * etc.  The value being sorted is represented by linking to an
 * EquivalenceClass containing that value and including pk_opfamily among its
 * ec_opfamilies.  This is a convenient method because it makes it trivial
 * to detect equivalent and closely-related orderings.  (See optimizer/README
 * for more information.)
 *
 * Note: pk_strategy is either BTLessStrategyNumber (for ASC) or
 * BTGreaterStrategyNumber (for DESC).  We assume that all ordering-capable
 * index types will use btree-compatible strategy numbers.
 */

typedef struct PathKey
{
	NodeTag		type;

	EquivalenceClass *pk_eclass;	/* the value that is ordered */
	Oid			pk_opfamily;	/* btree opfamily defining the ordering */
	int			pk_strategy;	/* sort direction (ASC or DESC) */
	bool		pk_nulls_first; /* do NULLs come before normal values? */
} PathKey;

/*
 * Type "Path" is used as-is for sequential-scan paths, as well as some other
 * simple plan types that we don't need any extra information in the path for.
 * For other path types it is the first component of a larger struct.
 *
 * Note: "pathtype" is the NodeTag of the Plan node we could build from this
 * Path.  It is partially redundant with the Path's NodeTag, but allows us
 * to use the same Path type for multiple Plan types where there is no need
 * to distinguish the Plan type during path processing.
 */

typedef struct Path
{
	NodeTag		type;

	NodeTag		pathtype;		/* tag identifying scan/join method */

	RelOptInfo *parent;			/* the relation this path can build */

	/* estimated execution costs for path (see costsize.c for more info) */
	Cost		startup_cost;	/* cost expended before fetching any tuples */
	Cost		total_cost;		/* total cost (assuming all tuples fetched) */

	List	   *pathkeys;		/* sort ordering of path's output */
	/* pathkeys is a List of PathKey nodes; see above */
} Path;

/*----------
 * IndexPath represents an index scan over a single index.
 *
 * 'indexinfo' is the index to be scanned.
 *
 * 'indexclauses' is a list of index qualification clauses, with implicit
 * AND semantics across the list.  Each clause is a RestrictInfo node from
 * the query's WHERE or JOIN conditions.
 *
 * 'indexquals' has the same structure as 'indexclauses', but it contains
 * the actual indexqual conditions that can be used with the index.
 * In simple cases this is identical to 'indexclauses', but when special
 * indexable operators appear in 'indexclauses', they are replaced by the
 * derived indexscannable conditions in 'indexquals'.
 *
 * 'isjoininner' is TRUE if the path is a nestloop inner scan (that is,
 * some of the index conditions are join rather than restriction clauses).
 * Note that the path costs will be calculated differently from a plain
 * indexscan in this case, and in addition there's a special 'rows' value
 * different from the parent RelOptInfo's (see below).
 *
 * 'indexscandir' is one of:
 *		ForwardScanDirection: forward scan of an ordered index
 *		BackwardScanDirection: backward scan of an ordered index
 *		NoMovementScanDirection: scan of an unordered index, or don't care
 * (The executor doesn't care whether it gets ForwardScanDirection or
 * NoMovementScanDirection for an indexscan, but the planner wants to
 * distinguish ordered from unordered indexes for building pathkeys.)
 *
 * 'indextotalcost' and 'indexselectivity' are saved in the IndexPath so that
 * we need not recompute them when considering using the same index in a
 * bitmap index/heap scan (see BitmapHeapPath).  The costs of the IndexPath
 * itself represent the costs of an IndexScan plan type.
 *
 * 'rows' is the estimated result tuple count for the indexscan.  This
 * is the same as path.parent->rows for a simple indexscan, but it is
 * different for a nestloop inner scan, because the additional indexquals
 * coming from join clauses make the scan more selective than the parent
 * rel's restrict clauses alone would do.
 *----------
 */
typedef struct IndexPath
{
	Path		path;
	IndexOptInfo *indexinfo;
	List	   *indexclauses;
	List	   *indexquals;
	bool		isjoininner;
	ScanDirection indexscandir;
	Cost		indextotalcost;
	Selectivity indexselectivity;
	double		rows;			/* estimated number of result tuples */
} IndexPath;

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
 * to represent a regular IndexScan plan, and as the child of a BitmapHeapPath
 * that represents scanning the same index using a BitmapIndexScan.  The
 * startup_cost and total_cost figures of an IndexPath always represent the
 * costs to use it as a regular IndexScan.  The costs of a BitmapIndexScan
 * can be computed using the IndexPath's indextotalcost and indexselectivity.
 *
 * BitmapHeapPaths can be nestloop inner indexscans.  The isjoininner and
 * rows fields serve the same purpose as for plain IndexPaths.
 */
typedef struct BitmapHeapPath
{
	Path		path;
	Path	   *bitmapqual;		/* IndexPath, BitmapAndPath, BitmapOrPath */
	bool		isjoininner;	/* T if it's a nestloop inner scan */
	double		rows;			/* estimated number of result tuples */
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
 * "CTID = pseudoconstant" or "CTID = ANY(pseudoconstant_array)".
 * Note they are bare expressions, not RestrictInfos.
 */
typedef struct TidPath
{
	Path		path;
	List	   *tidquals;		/* qual(s) involving CTID = something */
} TidPath;

/*
 * AppendPath represents an Append plan, ie, successive execution of
 * several member plans.
 *
 * Note: it is possible for "subpaths" to contain only one, or even no,
 * elements.  These cases are optimized during create_append_plan.
 * In particular, an AppendPath with no subpaths is a "dummy" path that
 * is created to represent the case that a relation is provably empty.
 */
typedef struct AppendPath
{
	Path		path;
	List	   *subpaths;		/* list of component Paths */
} AppendPath;

#define IS_DUMMY_PATH(p) \
	(IsA((p), AppendPath) && ((AppendPath *) (p))->subpaths == NIL)

/*
 * ResultPath represents use of a Result plan node to compute a variable-free
 * targetlist with no underlying tables (a "SELECT expressions" query).
 * The query could have a WHERE clause, too, represented by "quals".
 *
 * Note that quals is a list of bare clauses, not RestrictInfos.
 */
typedef struct ResultPath
{
	Path		path;
	List	   *quals;
} ResultPath;

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
 * UniquePath represents elimination of distinct rows from the output of
 * its subpath.
 *
 * This is unlike the other Path nodes in that it can actually generate
 * different plans: either hash-based or sort-based implementation, or a
 * no-op if the input path can be proven distinct already.  The decision
 * is sufficiently localized that it's not worth having separate Path node
 * types.  (Note: in the no-op case, we could eliminate the UniquePath node
 * entirely and just return the subpath; but it's convenient to have a
 * UniquePath in the path tree to signal upper-level routines that the input
 * is known distinct.)
 */
typedef enum
{
	UNIQUE_PATH_NOOP,			/* input is known unique already */
	UNIQUE_PATH_HASH,			/* use hashing */
	UNIQUE_PATH_SORT			/* use sorting */
} UniquePathMethod;

typedef struct UniquePath
{
	Path		path;
	Path	   *subpath;
	UniquePathMethod umethod;
	List	   *in_operators;	/* equality operators of the IN clause */
	List	   *uniq_exprs;		/* expressions to be made unique */
	double		rows;			/* estimated number of result tuples */
} UniquePath;

/*
 * All join-type paths share these fields.
 */

typedef struct JoinPath
{
	Path		path;

	JoinType	jointype;

	Path	   *outerjoinpath;	/* path for the outer side of the join */
	Path	   *innerjoinpath;	/* path for the inner side of the join */

	List	   *joinrestrictinfo;		/* RestrictInfos to apply to join */

	/*
	 * See the notes for RelOptInfo to understand why joinrestrictinfo is
	 * needed in JoinPath, and can't be merged into the parent RelOptInfo.
	 */
} JoinPath;

/*
 * A nested-loop path needs no special fields.
 */

typedef JoinPath NestPath;

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
 * materialize_inner is TRUE if a Material node should be placed atop the
 * inner input.  This may appear with or without an inner Sort step.
 */

typedef struct MergePath
{
	JoinPath	jpath;
	List	   *path_mergeclauses;		/* join clauses to be used for merge */
	List	   *outersortkeys;	/* keys for explicit sort, if any */
	List	   *innersortkeys;	/* keys for explicit sort, if any */
	bool		materialize_inner;		/* add Materialize to inner? */
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
	List	   *path_hashclauses;		/* join clauses used for hashing */
	int			num_batches;	/* number of batches expected */
} HashPath;

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
 * If a restriction clause references more than one base rel, it will
 * appear in the joininfo list of every RelOptInfo that describes a strict
 * subset of the base rels mentioned in the clause.  The joininfo lists are
 * used to drive join tree building by selecting plausible join candidates.
 * The clause cannot actually be applied until we have built a join rel
 * containing all the base rels it references, however.
 *
 * When we construct a join rel that includes all the base rels referenced
 * in a multi-relation restriction clause, we place that clause into the
 * joinrestrictinfo lists of paths for the join rel, if neither left nor
 * right sub-path includes all base rels referenced in the clause.  The clause
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
 * When dealing with outer joins we have to be very careful about pushing qual
 * clauses up and down the tree.  An outer join's own JOIN/ON conditions must
 * be evaluated exactly at that join node, unless they are "degenerate"
 * conditions that reference only Vars from the nullable side of the join.
 * Quals appearing in WHERE or in a JOIN above the outer join cannot be pushed
 * down below the outer join, if they reference any nullable Vars.
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
 * conditions.  Possibly we should rename it to reflect that meaning?)
 *
 * RestrictInfo nodes also contain an outerjoin_delayed flag, which is true
 * if the clause's applicability must be delayed due to any outer joins
 * appearing below it (ie, it has to be postponed to some join level higher
 * than the set of relations it actually references).  There is also a
 * nullable_relids field, which is the set of rels it references that can be
 * forced null by some outer join below the clause.  outerjoin_delayed = true
 * is subtly different from nullable_relids != NULL: a clause might reference
 * some nullable rels and yet not be outerjoin_delayed because it also
 * references all the other rels of the outer join(s).  A clause that is not
 * outerjoin_delayed can be enforced anywhere it is computable.
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
 * When join clauses are generated from EquivalenceClasses, there may be
 * several equally valid ways to enforce join equivalence, of which we need
 * apply only one.  We mark clauses of this kind by setting parent_ec to
 * point to the generating EquivalenceClass.  Multiple clauses with the same
 * parent_ec in the same join are redundant.
 */

typedef struct RestrictInfo
{
	NodeTag		type;

	Expr	   *clause;			/* the represented clause of WHERE or JOIN */

	bool		is_pushed_down; /* TRUE if clause was pushed down in level */

	bool		outerjoin_delayed;		/* TRUE if delayed by lower outer join */

	bool		can_join;		/* see comment above */

	bool		pseudoconstant; /* see comment above */

	/* The set of relids (varnos) actually referenced in the clause: */
	Relids		clause_relids;

	/* The set of relids required to evaluate the clause: */
	Relids		required_relids;

	/* The relids used in the clause that are nullable by lower outer joins: */
	Relids		nullable_relids;

	/* These fields are set for any binary opclause: */
	Relids		left_relids;	/* relids in left side of clause */
	Relids		right_relids;	/* relids in right side of clause */

	/* This field is NULL unless clause is an OR clause: */
	Expr	   *orclause;		/* modified clause with RestrictInfos */

	/* This field is NULL unless clause is potentially redundant: */
	EquivalenceClass *parent_ec;	/* generating EquivalenceClass */

	/* cache space for cost and selectivity */
	QualCost	eval_cost;		/* eval cost of clause; -1 if not yet set */
	Selectivity norm_selec;		/* selectivity for "normal" (JOIN_INNER)
								 * semantics; -1 if not yet set; >1 means a
								 * redundant clause */
	Selectivity outer_selec;	/* selectivity for outer join semantics; -1 if
								 * not yet set */

	/* valid if clause is mergejoinable, else NIL */
	List	   *mergeopfamilies;	/* opfamilies containing clause operator */

	/* cache space for mergeclause processing; NULL if not yet set */
	EquivalenceClass *left_ec;	/* EquivalenceClass containing lefthand */
	EquivalenceClass *right_ec; /* EquivalenceClass containing righthand */
	EquivalenceMember *left_em; /* EquivalenceMember for lefthand */
	EquivalenceMember *right_em;	/* EquivalenceMember for righthand */
	List	   *scansel_cache;	/* list of MergeScanSelCache structs */

	/* transient workspace for use while considering a specific join path */
	bool		outer_is_left;	/* T = outer var on left, F = on right */

	/* valid if clause is hashjoinable, else InvalidOid: */
	Oid			hashjoinoperator;		/* copy of clause operator */

	/* cache space for hashclause processing; -1 if not yet set */
	Selectivity left_bucketsize;	/* avg bucketsize of left side */
	Selectivity right_bucketsize;		/* avg bucketsize of right side */
} RestrictInfo;

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
	int			strategy;		/* sort direction (ASC or DESC) */
	bool		nulls_first;	/* do NULLs come before normal values? */
	/* Results */
	Selectivity leftstartsel;	/* first-join fraction for clause left side */
	Selectivity leftendsel;		/* last-join fraction for clause left side */
	Selectivity rightstartsel;	/* first-join fraction for clause right side */
	Selectivity rightendsel;	/* last-join fraction for clause right side */
} MergeScanSelCache;

/*
 * Inner indexscan info.
 *
 * An inner indexscan is one that uses one or more joinclauses as index
 * conditions (perhaps in addition to plain restriction clauses).  So it
 * can only be used as the inner path of a nestloop join where the outer
 * relation includes all other relids appearing in those joinclauses.
 * The set of usable joinclauses, and thus the best inner indexscan,
 * thus varies depending on which outer relation we consider; so we have
 * to recompute the best such paths for every join.  To avoid lots of
 * redundant computation, we cache the results of such searches.  For
 * each relation we compute the set of possible otherrelids (all relids
 * appearing in joinquals that could become indexquals for this table).
 * Two outer relations whose relids have the same intersection with this
 * set will have the same set of available joinclauses and thus the same
 * best inner indexscans for the inner relation.  By taking the intersection
 * before scanning the cache, we avoid recomputing when considering
 * join rels that differ only by the inclusion of irrelevant other rels.
 *
 * The search key also includes a bool showing whether the join being
 * considered is an outer join.  Since we constrain the join order for
 * outer joins, I believe that this bool can only have one possible value
 * for any particular lookup key; but store it anyway to avoid confusion.
 */

typedef struct InnerIndexscanInfo
{
	NodeTag		type;
	/* The lookup key: */
	Relids		other_relids;	/* a set of relevant other relids */
	bool		isouterjoin;	/* true if join is outer */
	/* Best paths for this lookup key (NULL if no available indexscans): */
	Path	   *cheapest_startup_innerpath;		/* cheapest startup cost */
	Path	   *cheapest_total_innerpath;		/* cheapest total cost */
} InnerIndexscanInfo;

/*
 * Placeholder node for an expression to be evaluated below the top level
 * of a plan tree.  This is used during planning to represent the contained
 * expression.  At the end of the planning process it is replaced by either
 * the contained expression or a Var referring to a lower-level evaluation of
 * the contained expression.  Typically the evaluation occurs below an outer
 * join, and Var references above the outer join might thereby yield NULL
 * instead of the expression value.
 *
 * Although the planner treats this as an expression node type, it is not
 * recognized by the parser or executor, so we declare it here rather than
 * in primnodes.h.
 */

typedef struct PlaceHolderVar
{
	Expr		xpr;
	Expr	   *phexpr;			/* the represented expression */
	Relids		phrels;			/* base relids syntactically within expr src */
	Index		phid;			/* ID for PHV (unique within planner run) */
	Index		phlevelsup;		/* > 0 if PHV belongs to outer query */
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
 * min_lefthand and min_righthand are the sets of base relids that must be
 * available on each side when performing the special join.  lhs_strict is
 * true if the special join's condition cannot succeed when the LHS variables
 * are all NULL (this means that an outer join can commute with upper-level
 * outer joins even if it appears in their RHS).  We don't bother to set
 * lhs_strict for FULL JOINs, however.
 *
 * It is not valid for either min_lefthand or min_righthand to be empty sets;
 * if they were, this would break the logic that enforces join order.
 *
 * syn_lefthand and syn_righthand are the sets of base relids that are
 * syntactically below this special join.  (These are needed to help compute
 * min_lefthand and min_righthand for higher joins.)
 *
 * delay_upper_joins is set TRUE if we detect a pushed-down clause that has
 * to be evaluated after this join is formed (because it references the RHS).
 * Any outer joins that have such a clause and this join in their RHS cannot
 * commute with this join, because that would leave noplace to check the
 * pushed-down clause.  (We don't track this for FULL JOINs, either.)
 *
 * join_quals is an implicit-AND list of the quals syntactically associated
 * with the join (they may or may not end up being applied at the join level).
 * This is just a side list and does not drive actual application of quals.
 * For JOIN_SEMI joins, this is cleared to NIL in create_unique_path() if
 * the join is found not to be suitable for a uniqueify-the-RHS plan.
 *
 * jointype is never JOIN_RIGHT; a RIGHT JOIN is handled by switching
 * the inputs to make it a LEFT JOIN.  So the allowed values of jointype
 * in a join_info_list member are only LEFT, FULL, SEMI, or ANTI.
 *
 * For purposes of join selectivity estimation, we create transient
 * SpecialJoinInfo structures for regular inner joins; so it is possible
 * to have jointype == JOIN_INNER in such a structure, even though this is
 * not allowed within join_info_list.  We also create transient
 * SpecialJoinInfos with jointype == JOIN_INNER for outer joins, since for
 * cost estimation purposes it is sometimes useful to know the join size under
 * plain innerjoin semantics.  Note that lhs_strict, delay_upper_joins, and
 * join_quals are not set meaningfully within such structs.
 */

typedef struct SpecialJoinInfo
{
	NodeTag		type;
	Relids		min_lefthand;	/* base relids in minimum LHS for join */
	Relids		min_righthand;	/* base relids in minimum RHS for join */
	Relids		syn_lefthand;	/* base relids syntactically within LHS */
	Relids		syn_righthand;	/* base relids syntactically within RHS */
	JoinType	jointype;		/* always INNER, LEFT, FULL, SEMI, or ANTI */
	bool		lhs_strict;		/* joinclause is strict for some LHS rel */
	bool		delay_upper_joins;		/* can't commute with upper RHS */
	List	   *join_quals;		/* join quals, in implicit-AND list format */
} SpecialJoinInfo;

/*
 * Append-relation info.
 *
 * When we expand an inheritable table or a UNION-ALL subselect into an
 * "append relation" (essentially, a list of child RTEs), we build an
 * AppendRelInfo for each child RTE.  The list of AppendRelInfos indicates
 * which child RTEs must be included when expanding the parent, and each
 * node carries information needed to translate Vars referencing the parent
 * into Vars referencing that child.
 *
 * These structs are kept in the PlannerInfo node's append_rel_list.
 * Note that we just throw all the structs into one list, and scan the
 * whole list when desiring to expand any one parent.  We could have used
 * a more complex data structure (eg, one list per parent), but this would
 * be harder to update during operations such as pulling up subqueries,
 * and not really any easier to scan.  Considering that typical queries
 * will not have many different append parents, it doesn't seem worthwhile
 * to complicate things.
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
	 * We store the parent table's OID here for inheritance, or InvalidOid for
	 * UNION ALL.  This is only needed to help in generating error messages if
	 * an attempt is made to reference a dropped parent column.
	 */
	Oid			parent_reloid;	/* OID of parent relation */
} AppendRelInfo;

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
 * ph_may_need is an initial estimate of ph_needed, formed using the
 * syntactic locations of references to the PHV.  We need this in order to
 * determine whether the PHV reference forces a join ordering constraint:
 * if the PHV has to be evaluated below the nullable side of an outer join,
 * and then used above that outer join, we must constrain join order to ensure
 * there's a valid place to evaluate the PHV below the join.  The final
 * actual ph_needed level might be lower than ph_may_need, but we can't
 * determine that until later on.  Fortunately this doesn't matter for what
 * we need ph_may_need for: if there's a PHV reference syntactically
 * above the outer join, it's not going to be allowed to drop below the outer
 * join, so we would come to the same conclusions about join order even if
 * we had the final ph_needed value to compare to.
 *
 * We create a PlaceHolderInfo only after determining that the PlaceHolderVar
 * is actually referenced in the plan tree, so that unreferenced placeholders
 * don't result in unnecessary constraints on join order.
 */

typedef struct PlaceHolderInfo
{
	NodeTag		type;

	Index		phid;			/* ID for PH (unique within planner run) */
	PlaceHolderVar *ph_var;		/* copy of PlaceHolderVar tree */
	Relids		ph_eval_at;		/* lowest level we can evaluate value at */
	Relids		ph_needed;		/* highest level the value is needed at */
	Relids		ph_may_need;	/* highest level it might be needed at */
	int32		ph_width;		/* estimated attribute width */
} PlaceHolderInfo;

/*
 * At runtime, PARAM_EXEC slots are used to pass values around from one plan
 * node to another.  They can be used to pass values down into subqueries (for
 * outer references in subqueries), or up out of subqueries (for the results
 * of a subplan).
 * The planner is responsible for assigning nonconflicting PARAM_EXEC IDs to
 * the PARAM_EXEC Params it generates.
 *
 * Outer references are managed via root->plan_params, which is a list of
 * PlannerParamItems.  While planning a subquery, each parent query level's
 * plan_params contains the values required from it by the current subquery.
 *
 * The item a PlannerParamItem represents can be one of three kinds:
 *
 * A Var: the slot represents a variable of this level that must be passed
 * down because subqueries have outer references to it.  The varlevelsup
 * value in the Var will always be zero.
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
 * parameters passed from one query level into a single subquery.  So there is
 * no possibility of a PARAM_EXEC slot being used for conflicting purposes.
 *
 * In addition, PARAM_EXEC slots are assigned for Params representing outputs
 * from subplans (values that are setParam items for those subplans).  These
 * IDs need not be tracked via PlannerParamItems, since we do not need any
 * duplicate-elimination nor later processing of the represented expressions.
 * Instead, we just record the assignment of the slot number by incrementing
 * root->glob->nParamExec.
 */
typedef struct PlannerParamItem
{
	NodeTag		type;

	Node	   *item;			/* the Var, PlaceHolderVar, or Aggref */
	int			paramId;		/* its assigned PARAM_EXEC slot number */
} PlannerParamItem;

#endif   /* RELATION_H */
