/*-------------------------------------------------------------------------
 *
 * relation.h
 *	  Definitions for internal planner nodes.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: relation.h,v 1.49 2000/09/29 18:21:39 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELATION_H
#define RELATION_H

#include "access/sdir.h"
#include "nodes/parsenodes.h"

/*
 * Relids
 *		List of relation identifiers (indexes into the rangetable).
 *
 *		Note: these are lists of integers, not Nodes.
 */

typedef List *Relids;

/*
 * When looking for a "cheapest path", this enum specifies whether we want
 * cheapest startup cost or cheapest total cost.
 */
typedef enum CostSelector
{
	STARTUP_COST, TOTAL_COST
} CostSelector;

/*----------
 * RelOptInfo
 *		Per-relation information for planning/optimization
 *
 *		For planning purposes, a "base rel" is either a plain relation (a
 *		table) or the output of a sub-SELECT that appears in the range table.
 *		In either case it is uniquely identified by an RT index.  A "joinrel"
 *		is the joining of two or more base rels.  A joinrel is identified by
 *		the set of RT indexes for its component baserels.
 *
 *		Note that there is only one joinrel for any given set of component
 *		baserels, no matter what order we assemble them in; so an unordered
 *		set is the right datatype to identify it with.
 *
 *		Parts of this data structure are specific to various scan and join
 *		mechanisms.  It didn't seem worth creating new node types for them.
 *
 *		relids - List of base-relation identifiers; it is a base relation
 *				if there is just one, a join relation if more than one
 *		rows - estimated number of tuples in the relation after restriction
 *			   clauses have been applied (ie, output rows of a plan for it)
 *		width - avg. number of bytes per tuple in the relation after the
 *				appropriate projections have been done (ie, output width)
 *		targetlist - List of TargetEntry nodes for the attributes we need
 *					 to output from this relation
 *		pathlist - List of Path nodes, one for each potentially useful
 *				   method of generating the relation
 *		cheapest_startup_path - the pathlist member with lowest startup cost
 *								(regardless of its ordering)
 *		cheapest_total_path - the pathlist member with lowest total cost
 *							  (regardless of its ordering)
 *		pruneable - flag to let the planner know whether it can prune the
 *					pathlist of this RelOptInfo or not.
 *
 *	 * If the relation is a base relation it will have these fields set:
 *
 *		issubquery - true if baserel is a subquery RTE rather than a table
 *		indexed - true if the relation has secondary indices (always false
 *				  if it's a subquery)
 *		pages - number of disk pages in relation (zero if a subquery)
 *		tuples - number of tuples in relation (not considering restrictions)
 *		subplan - plan for subquery (NULL if it's a plain table)
 *
 *		Note: for a subquery, tuples and subplan are not set immediately
 *		upon creation of the RelOptInfo object; they are filled in when
 *		set_base_rel_pathlist processes the object.
 *
 *	 * The presence of the remaining fields depends on the restrictions
 *		and joins that the relation participates in:
 *
 *		baserestrictinfo - List of RestrictInfo nodes, containing info about
 *					each qualification clause in which this relation
 *					participates (only used for base rels)
 *		baserestrictcost - Estimated cost of evaluating the baserestrictinfo
 *					clauses at a single tuple (only used for base rels)
 *		outerjoinset - If the rel appears within the nullable side of an outer
 *					join, the list of all relids participating in the highest
 *					such outer join; else NIL (only used for base rels)
 *		joininfo  - List of JoinInfo nodes, containing info about each join
 *					clause in which this relation participates
 *		innerjoin - List of Path nodes that represent indices that may be used
 *					as inner paths of nestloop joins. This field is non-null
 *					only for base rels, since join rels have no indices.
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

typedef struct RelOptInfo
{
	NodeTag		type;

	/* all relations included in this RelOptInfo */
	Relids		relids;			/* integer list of base relids (RT
								 * indexes) */

	/* size estimates generated by planner */
	double		rows;			/* estimated number of result tuples */
	int			width;			/* estimated avg width of result tuples */

	/* materialization information */
	List	   *targetlist;
	List	   *pathlist;		/* Path structures */
	struct Path *cheapest_startup_path;
	struct Path *cheapest_total_path;
	bool		pruneable;

	/* information about a base rel (not set for join rels!) */
	bool		issubquery;
	bool		indexed;
	long		pages;
	double		tuples;
	struct Plan *subplan;

	/* used by various scans and joins: */
	List	   *baserestrictinfo;		/* RestrictInfo structures (if
										 * base rel) */
	Cost		baserestrictcost;		/* cost of evaluating the above */
	Relids		outerjoinset;			/* integer list of base relids */
	List	   *joininfo;		/* JoinInfo structures */
	List	   *innerjoin;		/* potential indexscans for nestloop joins */

	/*
	 * innerjoin indexscans are not in the main pathlist because they are
	 * not usable except in specific join contexts; we have to test before
	 * seeing whether they can be used.
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
 *		indexoid - OID of the index relation itself
 *		pages - number of disk pages in index
 *		tuples - number of index tuples in index
 *		classlist - List of PG_AMOPCLASS OIDs for the index
 *		indexkeys - List of base-relation attribute numbers that are index keys
 *		ordering - List of PG_OPERATOR OIDs which order the indexscan result
 *		relam	  - the OID of the pg_am of the index
 *		amcostestimate - OID of the relam's cost estimator
 *		indproc   - OID of the function if a functional index, else 0
 *		indpred   - index predicate if a partial index, else NULL
 *		lossy	  - true if index is lossy (may return non-matching tuples)
 *
 *		NB. the last element of the arrays classlist, indexkeys and ordering
 *			is always 0.
 */

typedef struct IndexOptInfo
{
	NodeTag		type;

	Oid			indexoid;		/* OID of the index relation */

	/* statistics from pg_class */
	long		pages;
	double		tuples;

	/* index descriptor information */
	Oid		   *classlist;		/* classes of AM operators */
	int		   *indexkeys;		/* keys over which we're indexing */
	Oid		   *ordering;		/* OIDs of sort operators for each key */
	Oid			relam;			/* OID of the access method (in pg_am) */

	RegProcedure amcostestimate;/* OID of the access method's cost fcn */

	Oid			indproc;		/* if a functional index */
	List	   *indpred;		/* if a partial index */
	bool		lossy;			/* if a lossy index */
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
	 * can also point to a Func clause representing the value indexed by a
	 * functional index.  Someday we might allow arbitrary expressions as
	 * path keys, so don't assume more than you must.
	 */
} PathKeyItem;

/*
 * Type "Path" is used as-is for sequential-scan paths.  For other
 * path types it is the first component of a larger struct.
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
	/* XXX why is pathtype separate from the NodeTag? */

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
 * 'indexid' is a list of index relation OIDs, one per scan to be performed.
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
 * 'indexscandir' is one of:
 *		ForwardScanDirection: forward scan of an ordered index
 *		BackwardScanDirection: backward scan of an ordered index
 *		NoMovementScanDirection: scan of an unordered index, or don't care
 * (The executor doesn't care whether it gets ForwardScanDirection or
 * NoMovementScanDirection for an indexscan, but the planner wants to
 * distinguish ordered from unordered indexes for building pathkeys.)
 *
 * 'joinrelids' is only used in IndexPaths that are constructed for use
 * as the inner path of a nestloop join.  These paths have indexquals
 * that refer to values of other rels, so those other rels must be
 * included in the outer joinrel in order to make a usable join.
 *
 * 'alljoinquals' is also used only for inner paths of nestloop joins.
 * This flag is TRUE iff all the indexquals came from non-pushed-down
 * JOIN/ON conditions, which means the path is safe to use for an outer join.
 *
 * 'rows' is the estimated result tuple count for the indexscan.  This
 * is the same as path.parent->rows for a simple indexscan, but it is
 * different for a nestloop inner path, because the additional indexquals
 * coming from join clauses make the scan more selective than the parent
 * rel's restrict clauses alone would do.
 *----------
 */
typedef struct IndexPath
{
	Path		path;
	List	   *indexid;
	List	   *indexqual;
	ScanDirection indexscandir;
	Relids		joinrelids;		/* other rels mentioned in indexqual */
	bool		alljoinquals;	/* all indexquals derived from JOIN conds? */
	double		rows;			/* estimated number of result tuples */
} IndexPath;

typedef struct TidPath
{
	Path		path;
	List	   *tideval;
	Relids		unjoined_relids;/* some rels not yet part of my Path */
} TidPath;

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
 * that will be used in the merge.	(Before 7.0, this was a list of
 * bare clause expressions, but we can save on list memory by leaving
 * it in the form of a RestrictInfo list.)
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
 * (But note that path_hashclauses will always be a one-element list,
 * since we only hash on one hashable clause.)
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
 * of the final Plan node, where they will be enforced by general-purpose
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

	/* valid if clause is mergejoinable, else InvalidOid: */
	Oid			mergejoinoperator;		/* copy of clause operator */
	Oid			left_sortop;	/* leftside sortop needed for mergejoin */
	Oid			right_sortop;	/* rightside sortop needed for mergejoin */

	/* valid if clause is hashjoinable, else InvalidOid: */
	Oid			hashjoinoperator;		/* copy of clause operator */
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
	Relids		unjoined_relids; /* some rels not yet part of my RelOptInfo */
	List	   *jinfo_restrictinfo;		/* relevant RestrictInfos */
} JoinInfo;

/*
 *	Stream:
 *	A stream represents a root-to-leaf path in a plan tree (i.e. a tree of
 *	JoinPaths and Paths).  The stream includes pointers to all Path nodes,
 *	as well as to any clauses that reside above Path nodes. This structure
 *	is used to make Path nodes and clauses look similar, so that Predicate
 *	Migration can run.
 *
 *	XXX currently, Predicate Migration is dead code, and so is this node type.
 *	Probably should remove support for it.
 *
 *	pathptr -- pointer to the current path node
 *	cinfo -- if NULL, this stream node referes to the path node.
 *			  Otherwise this is a pointer to the current clause.
 *	clausetype -- whether cinfo is in loc_restrictinfo or pathinfo in the
 *			  path node (XXX this is now used only by dead code, which is
 *			  good because the distinction no longer exists...)
 *	upstream -- linked list pointer upwards
 *	downstream -- ditto, downwards
 *	groupup -- whether or not this node is in a group with the node upstream
 *	groupcost -- total cost of the group that node is in
 *	groupsel -- total selectivity of the group that node is in
 */
typedef struct Stream *StreamPtr;

typedef struct Stream
{
	NodeTag		type;
	Path	   *pathptr;
	RestrictInfo *cinfo;
	int		   *clausetype;
	StreamPtr	upstream;
	StreamPtr	downstream;
	bool		groupup;
	Cost		groupcost;
	Selectivity groupsel;
} Stream;

#endif	 /* RELATION_H */
