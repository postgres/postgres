/*-------------------------------------------------------------------------
 *
 * relation.h
 *	  Definitions for planner's internal data structures.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relation.h,v 1.85 2003/08/08 21:42:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELATION_H
#define RELATION_H

#include "access/sdir.h"
#include "nodes/bitmapset.h"
#include "nodes/parsenodes.h"


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
 * RelOptInfo
 *		Per-relation information for planning/optimization
 *
 * For planning purposes, a "base rel" is either a plain relation (a table)
 * or the output of a sub-SELECT or function that appears in the range table.
 * In either case it is uniquely identified by an RT index.  A "joinrel"
 * is the joining of two or more base rels.  A joinrel is identified by
 * the set of RT indexes for its component baserels.  We create RelOptInfo
 * nodes for each baserel and joinrel, and store them in the Query's
 * base_rel_list and join_rel_list respectively.
 *
 * Note that there is only one joinrel for any given set of component
 * baserels, no matter what order we assemble them in; so an unordered
 * set is the right datatype to identify it with.
 *
 * We also have "other rels", which are like base rels in that they refer to
 * single RT indexes; but they are not part of the join tree, and are stored
 * in other_rel_list not base_rel_list.
 *
 * Currently the only kind of otherrels are those made for child relations
 * of an inheritance scan (SELECT FROM foo*).  The parent table's RTE and
 * corresponding baserel represent the whole result of the inheritance scan.
 * The planner creates separate RTEs and associated RelOptInfos for each child
 * table (including the parent table, in its capacity as a member of the
 * inheritance set).  These RelOptInfos are physically identical to baserels,
 * but are otherrels because they are not in the main join tree.  These added
 * RTEs and otherrels are used to plan the scans of the individual tables in
 * the inheritance set; then the parent baserel is given an Append plan
 * comprising the best plans for the individual child tables.
 *
 * At one time we also made otherrels to represent join RTEs, for use in
 * handling join alias Vars.  Currently this is not needed because all join
 * alias Vars are expanded to non-aliased form during preprocess_expression.
 *
 * Parts of this data structure are specific to various scan and join
 * mechanisms.	It didn't seem worth creating new node types for them.
 *
 *		relids - Set of base-relation identifiers; it is a base relation
 *				if there is just one, a join relation if more than one
 *		rows - estimated number of tuples in the relation after restriction
 *			   clauses have been applied (ie, output rows of a plan for it)
 *		width - avg. number of bytes per tuple in the relation after the
 *				appropriate projections have been done (ie, output width)
 *		reltargetlist - List of Var nodes for the attributes we need to
 *						output from this relation (in no particular order)
 *		pathlist - List of Path nodes, one for each potentially useful
 *				   method of generating the relation
 *		cheapest_startup_path - the pathlist member with lowest startup cost
 *								(regardless of its ordering)
 *		cheapest_total_path - the pathlist member with lowest total cost
 *							  (regardless of its ordering)
 *		cheapest_unique_path - for caching cheapest path to produce unique
 *							   (no duplicates) output from relation
 *		pruneable - flag to let the planner know whether it can prune the
 *					pathlist of this RelOptInfo or not.
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
 *
 *		Note: for a subquery, tuples and subplan are not set immediately
 *		upon creation of the RelOptInfo object; they are filled in when
 *		set_base_rel_pathlist processes the object.
 *
 *		For otherrels that are inheritance children, these fields are filled
 *		in just as for a baserel.
 *
 * The presence of the remaining fields depends on the restrictions
 * and joins that the relation participates in:
 *
 *		baserestrictinfo - List of RestrictInfo nodes, containing info about
 *					each qualification clause in which this relation
 *					participates (only used for base rels)
 *		baserestrictcost - Estimated cost of evaluating the baserestrictinfo
 *					clauses at a single tuple (only used for base rels)
 *		outerjoinset - For a base rel: if the rel appears within the nullable
 *					side of an outer join, the set of all relids
 *					participating in the highest such outer join; else NULL.
 *					Otherwise, unused.
 *		joininfo  - List of JoinInfo nodes, containing info about each join
 *					clause in which this relation participates
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
 * the RelOptInfo to store the joininfo lists, because those are the same
 * for a given rel no matter how we form it.
 *
 * We store baserestrictcost in the RelOptInfo (for base relations) because
 * we know we will need it at least once (to price the sequential scan)
 * and may need it multiple times to price index scans.
 *
 * outerjoinset is used to ensure correct placement of WHERE clauses that
 * apply to outer-joined relations; we must not apply such WHERE clauses
 * until after the outer join is performed.
 *----------
 */
typedef enum RelOptKind
{
	RELOPT_BASEREL,
	RELOPT_JOINREL,
	RELOPT_OTHER_CHILD_REL
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
	FastList	reltargetlist;
	List	   *pathlist;		/* Path structures */
	struct Path *cheapest_startup_path;
	struct Path *cheapest_total_path;
	struct Path *cheapest_unique_path;
	bool		pruneable;

	/* information about a base rel (not set for join rels!) */
	Index		relid;
	RTEKind		rtekind;		/* RELATION, SUBQUERY, or FUNCTION */
	AttrNumber	min_attr;		/* smallest attrno of rel (often <0) */
	AttrNumber	max_attr;		/* largest attrno of rel */
	Relids	   *attr_needed;	/* array indexed [min_attr .. max_attr] */
	int32	   *attr_widths;	/* array indexed [min_attr .. max_attr] */
	List	   *indexlist;
	long		pages;
	double		tuples;
	struct Plan *subplan;		/* if subquery */

	/* used by various scans and joins: */
	List	   *baserestrictinfo;		/* RestrictInfo structures (if
										 * base rel) */
	QualCost	baserestrictcost;		/* cost of evaluating the above */
	Relids		outerjoinset;	/* set of base relids */
	List	   *joininfo;		/* JoinInfo structures */

	/* cached info about inner indexscan paths for relation: */
	Relids		index_outer_relids;		/* other relids in indexable join
										 * clauses */
	List	   *index_inner_paths;		/* InnerIndexscanInfo nodes */

	/*
	 * Inner indexscans are not in the main pathlist because they are not
	 * usable except in specific join contexts.  We use the
	 * index_inner_paths list just to avoid recomputing the best inner
	 * indexscan repeatedly for similar outer relations.  See comments for
	 * InnerIndexscanInfo.
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
 *		classlist[], indexkeys[], and ordering[] have ncolumns entries.
 *		Zeroes in the indexkeys[] array indicate index columns that are
 *		expressions; there is one element in indexprs for each such column.
 *
 *		Note: for historical reasons, the classlist and ordering arrays have
 *		an extra entry that is always zero.  Some code scans until it sees a
 *		zero entry, rather than looking at ncolumns.
 *
 *		The indexprs and indpred expressions have been run through
 *		eval_const_expressions() for ease of matching to WHERE clauses.
 */

typedef struct IndexOptInfo
{
	NodeTag		type;

	Oid			indexoid;		/* OID of the index relation */

	/* statistics from pg_class */
	long		pages;			/* number of disk pages in index */
	double		tuples;			/* number of index tuples in index */

	/* index descriptor information */
	int			ncolumns;		/* number of columns in index */
	Oid		   *classlist;		/* OIDs of operator classes for columns */
	int		   *indexkeys;		/* column numbers of index's keys, or 0 */
	Oid		   *ordering;		/* OIDs of sort operators for each column */
	Oid			relam;			/* OID of the access method (in pg_am) */

	RegProcedure amcostestimate;	/* OID of the access method's cost fcn */

	List	   *indexprs;		/* expressions for non-simple index
								 * columns */
	List	   *indpred;		/* predicate if a partial index, else NIL */
	bool		unique;			/* true if a unique index */

	/* cached info about inner indexscan paths for index */
	Relids		outer_relids;	/* other relids in usable join clauses */
	List	   *inner_paths;	/* List of InnerIndexscanInfo nodes */
} IndexOptInfo;


/*
 * PathKeys
 *
 *	The sort ordering of a path is represented by a list of sublists of
 *	PathKeyItem nodes.	An empty list implies no known ordering.  Otherwise
 *	the first sublist represents the primary sort key, the second the
 *	first secondary sort key, etc.	Each sublist contains one or more
 *	PathKeyItem nodes, each of which can be taken as the attribute that
 *	appears at that sort position.	(See the top of optimizer/path/pathkeys.c
 *	for more information.)
 */

typedef struct PathKeyItem
{
	NodeTag		type;

	Node	   *key;			/* the item that is ordered */
	Oid			sortop;			/* the ordering operator ('<' op) */

	/*
	 * key typically points to a Var node, ie a relation attribute, but it
	 * can also point to an arbitrary expression representing the value
	 * indexed by an index expression.
	 */
} PathKeyItem;

/*
 * Type "Path" is used as-is for sequential-scan paths.  For other
 * path types it is the first component of a larger struct.
 *
 * Note: "pathtype" is the NodeTag of the Plan node we could build from this
 * Path.  It is partially redundant with the Path's NodeTag, but allows us
 * to use the same Path type for multiple Plan types where there is no need
 * to distinguish the Plan type during path processing.
 */

typedef struct Path
{
	NodeTag		type;

	RelOptInfo *parent;			/* the relation this path can build */

	/* estimated execution costs for path (see costsize.c for more info) */
	Cost		startup_cost;	/* cost expended before fetching any
								 * tuples */
	Cost		total_cost;		/* total cost (assuming all tuples
								 * fetched) */

	NodeTag		pathtype;		/* tag identifying scan/join method */

	List	   *pathkeys;		/* sort ordering of path's output */
	/* pathkeys is a List of Lists of PathKeyItem nodes; see above */
} Path;

/*----------
 * IndexPath represents an index scan.	Although an indexscan can only read
 * a single relation, it can scan it more than once, potentially using a
 * different index during each scan.  The result is the union (OR) of all the
 * tuples matched during any scan.	(The executor is smart enough not to return
 * the same tuple more than once, even if it is matched in multiple scans.)
 *
 * 'indexinfo' is a list of IndexOptInfo nodes, one per scan to be performed.
 *
 * 'indexqual' is a list of index qualifications, also one per scan.
 * Each entry in 'indexqual' is a sublist of qualification expressions with
 * implicit AND semantics across the sublist items.  Only expressions that
 * are usable as indexquals (as determined by indxpath.c) may appear here.
 * NOTE that the semantics of the top-level list in 'indexqual' is OR
 * combination, while the sublists are implicitly AND combinations!
 * Also note that indexquals lists do not contain RestrictInfo nodes,
 * just bare clause expressions.
 *
 * 'indexjoinclauses' is NIL for an ordinary indexpath (one that does not
 * use any join clauses in the index conditions).  For an innerjoin indexpath,
 * it has the same structure as 'indexqual', but references the RestrictInfo
 * nodes from which the indexqual was built, rather than the bare clause
 * expressions.  (Note: there isn't necessarily a one-to-one correspondence
 * between RestrictInfos and expressions, because of expansion of special
 * indexable operators.)  We need this so that we can eliminate redundant
 * join clauses when plans are built.
 *
 * 'indexscandir' is one of:
 *		ForwardScanDirection: forward scan of an ordered index
 *		BackwardScanDirection: backward scan of an ordered index
 *		NoMovementScanDirection: scan of an unordered index, or don't care
 * (The executor doesn't care whether it gets ForwardScanDirection or
 * NoMovementScanDirection for an indexscan, but the planner wants to
 * distinguish ordered from unordered indexes for building pathkeys.)
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
	List	   *indexinfo;
	List	   *indexqual;
	List	   *indexjoinclauses;
	ScanDirection indexscandir;
	double		rows;			/* estimated number of result tuples */
} IndexPath;

/*
 * TidPath represents a scan by TID
 */
typedef struct TidPath
{
	Path		path;
	List	   *tideval;		/* qual(s) involving CTID = something */
} TidPath;

/*
 * AppendPath represents an Append plan, ie, successive execution of
 * several member plans.  Currently it is only used to handle expansion
 * of inheritance trees.
 */
typedef struct AppendPath
{
	Path		path;
	List	   *subpaths;		/* list of component Paths */
} AppendPath;

/*
 * ResultPath represents use of a Result plan node, either to compute a
 * variable-free targetlist or to gate execution of a subplan with a
 * one-time (variable-free) qual condition.  Note that in the former case
 * path.parent will be NULL; in the latter case it is copied from the subpath.
 */
typedef struct ResultPath
{
	Path		path;
	Path	   *subpath;
	List	   *constantqual;
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
 * two different plans: either hash-based or sort-based implementation.
 * The decision is sufficiently localized that it's not worth having two
 * separate Path node types.
 */
typedef struct UniquePath
{
	Path		path;
	Path	   *subpath;
	bool		use_hash;
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
 * path_mergeclauses lists the clauses (in the form of RestrictInfos)
 * that will be used in the merge.	(Before 7.0, this was a list of bare
 * clause expressions, but we can save on list memory and cost_qual_eval
 * work by leaving it in the form of a RestrictInfo list.)
 *
 * Note that the mergeclauses are a subset of the parent relation's
 * restriction-clause list.  Any join clauses that are not mergejoinable
 * appear only in the parent's restrict list, and must be checked by a
 * qpqual at execution time.
 *
 * outersortkeys (resp. innersortkeys) is NIL if the outer path
 * (resp. inner path) is already ordered appropriately for the
 * mergejoin.  If it is not NIL then it is a PathKeys list describing
 * the ordering that must be created by an explicit sort step.
 */

typedef struct MergePath
{
	JoinPath	jpath;
	List	   *path_mergeclauses;		/* join clauses to be used for
										 * merge */
	List	   *outersortkeys;	/* keys for explicit sort, if any */
	List	   *innersortkeys;	/* keys for explicit sort, if any */
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
 * appear in the JoinInfo lists of every RelOptInfo that describes a strict
 * subset of the base rels mentioned in the clause.  The JoinInfo lists are
 * used to drive join tree building by selecting plausible join candidates.
 * The clause cannot actually be applied until we have built a join rel
 * containing all the base rels it references, however.
 *
 * When we construct a join rel that includes all the base rels referenced
 * in a multi-relation restriction clause, we place that clause into the
 * joinrestrictinfo lists of paths for the join rel, if neither left nor
 * right sub-path includes all base rels referenced in the clause.	The clause
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
 * When dealing with outer joins we have to be very careful about pushing qual
 * clauses up and down the tree.  An outer join's own JOIN/ON conditions must
 * be evaluated exactly at that join node, and any quals appearing in WHERE or
 * in a JOIN above the outer join cannot be pushed down below the outer join.
 * Otherwise the outer join will produce wrong results because it will see the
 * wrong sets of input rows.  All quals are stored as RestrictInfo nodes
 * during planning, but there's a flag to indicate whether a qual has been
 * pushed down to a lower level than its original syntactic placement in the
 * join tree would suggest.  If an outer join prevents us from pushing a qual
 * down to its "natural" semantic level (the level associated with just the
 * base rels used in the qual) then the qual will appear in JoinInfo lists
 * that reference more than just the base rels it actually uses.  By
 * pretending that the qual references all the rels appearing in the outer
 * join, we prevent it from being evaluated below the outer join's joinrel.
 * When we do form the outer join's joinrel, we still need to distinguish
 * those quals that are actually in that join's JOIN/ON condition from those
 * that appeared higher in the tree and were pushed down to the join rel
 * because they used no other rels.  That's what the ispusheddown flag is for;
 * it tells us that a qual came from a point above the join of the specific
 * set of base rels that it uses (or that the JoinInfo structures claim it
 * uses).  A clause that originally came from WHERE will *always* have its
 * ispusheddown flag set; a clause that came from an INNER JOIN condition,
 * but doesn't use all the rels being joined, will also have ispusheddown set
 * because it will get attached to some lower joinrel.
 *
 * In general, the referenced clause might be arbitrarily complex.	The
 * kinds of clauses we can handle as indexscan quals, mergejoin clauses,
 * or hashjoin clauses are fairly limited --- the code for each kind of
 * path is responsible for identifying the restrict clauses it can use
 * and ignoring the rest.  Clauses not implemented by an indexscan,
 * mergejoin, or hashjoin will be placed in the plan qual or joinqual field
 * of the finished Plan node, where they will be enforced by general-purpose
 * qual-expression-evaluation code.  (But we are still entitled to count
 * their selectivity when estimating the result tuple count, if we
 * can guess what it is...)
 */

typedef struct RestrictInfo
{
	NodeTag		type;

	Expr	   *clause;			/* the represented clause of WHERE or JOIN */

	bool		ispusheddown;	/* TRUE if clause was pushed down in level */

	/* only used if clause is an OR clause: */
	List	   *subclauseindices;		/* indexes matching subclauses */
	/* subclauseindices is a List of Lists of IndexOptInfos */

	/* cache space for costs (currently only used for join clauses) */
	QualCost	eval_cost;		/* eval cost of clause; -1 if not yet set */
	Selectivity this_selec;		/* selectivity; -1 if not yet set */

	/*
	 * If the clause looks useful for joining --- that is, it is a binary
	 * opclause with nonoverlapping sets of relids referenced in the left
	 * and right sides --- then these two fields are set to sets of the
	 * referenced relids.  Otherwise they are both NULL.
	 */
	Relids		left_relids;	/* relids in left side of join clause */
	Relids		right_relids;	/* relids in right side of join clause */

	/* valid if clause is mergejoinable, else InvalidOid: */
	Oid			mergejoinoperator;		/* copy of clause operator */
	Oid			left_sortop;	/* leftside sortop needed for mergejoin */
	Oid			right_sortop;	/* rightside sortop needed for mergejoin */

	/* cache space for mergeclause processing; NIL if not yet set */
	List	   *left_pathkey;	/* canonical pathkey for left side */
	List	   *right_pathkey;	/* canonical pathkey for right side */

	/* cache space for mergeclause processing; -1 if not yet set */
	Selectivity left_mergescansel;		/* fraction of left side to scan */
	Selectivity right_mergescansel;		/* fraction of right side to scan */

	/* valid if clause is hashjoinable, else InvalidOid: */
	Oid			hashjoinoperator;		/* copy of clause operator */

	/* cache space for hashclause processing; -1 if not yet set */
	Selectivity left_bucketsize;	/* avg bucketsize of left side */
	Selectivity right_bucketsize;		/* avg bucketsize of right side */
} RestrictInfo;

/*
 * Join clause info.
 *
 * We make a list of these for each RelOptInfo, containing info about
 * all the join clauses this RelOptInfo participates in.  (For this
 * purpose, a "join clause" is a WHERE clause that mentions both vars
 * belonging to this relation and vars belonging to relations not yet
 * joined to it.)  We group these clauses according to the set of
 * other base relations (unjoined relations) mentioned in them.
 * There is one JoinInfo for each distinct set of unjoined_relids,
 * and its jinfo_restrictinfo lists the clause(s) that use that set
 * of other relations.
 */

typedef struct JoinInfo
{
	NodeTag		type;
	Relids		unjoined_relids;	/* some rels not yet part of my RelOptInfo */
	List	   *jinfo_restrictinfo;		/* relevant RestrictInfos */
} JoinInfo;

/*
 * Inner indexscan info.
 *
 * An inner indexscan is one that uses one or more joinclauses as index
 * conditions (perhaps in addition to plain restriction clauses).  So it
 * can only be used as the inner path of a nestloop join where the outer
 * relation includes all other relids appearing in those joinclauses.
 * The set of usable joinclauses, and thus the best inner indexscan,
 * thus varies depending on which outer relation we consider; so we have
 * to recompute the best such path for every join.	To avoid lots of
 * redundant computation, we cache the results of such searches.  For
 * each index we compute the set of possible otherrelids (all relids
 * appearing in joinquals that could become indexquals for this index).
 * Two outer relations whose relids have the same intersection with this
 * set will have the same set of available joinclauses and thus the same
 * best inner indexscan for that index.  Similarly, for each base relation,
 * we form the union of the per-index otherrelids sets.  Two outer relations
 * with the same intersection with that set will have the same best overall
 * inner indexscan for the base relation.  We use lists of InnerIndexscanInfo
 * nodes to cache the results of these searches at both the index and
 * relation level.
 *
 * The search key also includes a bool showing whether the join being
 * considered is an outer join.  Since we constrain the join order for
 * outer joins, I believe that this bool can only have one possible value
 * for any particular base relation; but store it anyway to avoid confusion.
 */

typedef struct InnerIndexscanInfo
{
	NodeTag		type;
	/* The lookup key: */
	Relids		other_relids;	/* a set of relevant other relids */
	bool		isouterjoin;	/* true if join is outer */
	/* Best path for this lookup key: */
	Path	   *best_innerpath; /* best inner indexscan, or NULL if none */
} InnerIndexscanInfo;

/*
 * IN clause info.
 *
 * When we convert top-level IN quals into join operations, we must restrict
 * the order of joining and use special join methods at some join points.
 * We record information about each such IN clause in an InClauseInfo struct.
 * These structs are kept in the Query node's in_info_list.
 */

typedef struct InClauseInfo
{
	NodeTag		type;
	Relids		lefthand;		/* base relids in lefthand expressions */
	Relids		righthand;		/* base relids coming from the subselect */
	List	   *sub_targetlist; /* targetlist of original RHS subquery */

	/*
	 * Note: sub_targetlist is just a list of Vars or expressions; it does
	 * not contain TargetEntry nodes.
	 */
} InClauseInfo;

#endif   /* RELATION_H */
