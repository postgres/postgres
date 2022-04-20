/*-------------------------------------------------------------------------
 *
 * relnode.c
 *	  Relation-node lookup/construction routines
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/relnode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"


typedef struct JoinHashEntry
{
	Relids		join_relids;	/* hash key --- MUST BE FIRST */
	RelOptInfo *join_rel;
} JoinHashEntry;

static void build_joinrel_tlist(PlannerInfo *root, RelOptInfo *joinrel,
								RelOptInfo *input_rel);
static List *build_joinrel_restrictlist(PlannerInfo *root,
										RelOptInfo *joinrel,
										RelOptInfo *outer_rel,
										RelOptInfo *inner_rel);
static void build_joinrel_joinlist(RelOptInfo *joinrel,
								   RelOptInfo *outer_rel,
								   RelOptInfo *inner_rel);
static List *subbuild_joinrel_restrictlist(RelOptInfo *joinrel,
										   List *joininfo_list,
										   List *new_restrictlist);
static List *subbuild_joinrel_joinlist(RelOptInfo *joinrel,
									   List *joininfo_list,
									   List *new_joininfo);
static void set_foreign_rel_properties(RelOptInfo *joinrel,
									   RelOptInfo *outer_rel, RelOptInfo *inner_rel);
static void add_join_rel(PlannerInfo *root, RelOptInfo *joinrel);
static void build_joinrel_partition_info(RelOptInfo *joinrel,
										 RelOptInfo *outer_rel, RelOptInfo *inner_rel,
										 List *restrictlist, JoinType jointype);
static bool have_partkey_equi_join(RelOptInfo *joinrel,
								   RelOptInfo *rel1, RelOptInfo *rel2,
								   JoinType jointype, List *restrictlist);
static int	match_expr_to_partition_keys(Expr *expr, RelOptInfo *rel,
										 bool strict_op);
static void set_joinrel_partition_key_exprs(RelOptInfo *joinrel,
											RelOptInfo *outer_rel, RelOptInfo *inner_rel,
											JoinType jointype);
static void build_child_join_reltarget(PlannerInfo *root,
									   RelOptInfo *parentrel,
									   RelOptInfo *childrel,
									   int nappinfos,
									   AppendRelInfo **appinfos);


/*
 * setup_simple_rel_arrays
 *	  Prepare the arrays we use for quickly accessing base relations
 *	  and AppendRelInfos.
 */
void
setup_simple_rel_arrays(PlannerInfo *root)
{
	int			size;
	Index		rti;
	ListCell   *lc;

	/* Arrays are accessed using RT indexes (1..N) */
	size = list_length(root->parse->rtable) + 1;
	root->simple_rel_array_size = size;

	/*
	 * simple_rel_array is initialized to all NULLs, since no RelOptInfos
	 * exist yet.  It'll be filled by later calls to build_simple_rel().
	 */
	root->simple_rel_array = (RelOptInfo **)
		palloc0(size * sizeof(RelOptInfo *));

	/* simple_rte_array is an array equivalent of the rtable list */
	root->simple_rte_array = (RangeTblEntry **)
		palloc0(size * sizeof(RangeTblEntry *));
	rti = 1;
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		root->simple_rte_array[rti++] = rte;
	}

	/* append_rel_array is not needed if there are no AppendRelInfos */
	if (root->append_rel_list == NIL)
	{
		root->append_rel_array = NULL;
		return;
	}

	root->append_rel_array = (AppendRelInfo **)
		palloc0(size * sizeof(AppendRelInfo *));

	/*
	 * append_rel_array is filled with any already-existing AppendRelInfos,
	 * which currently could only come from UNION ALL flattening.  We might
	 * add more later during inheritance expansion, but it's the
	 * responsibility of the expansion code to update the array properly.
	 */
	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = lfirst_node(AppendRelInfo, lc);
		int			child_relid = appinfo->child_relid;

		/* Sanity check */
		Assert(child_relid < size);

		if (root->append_rel_array[child_relid])
			elog(ERROR, "child relation already exists");

		root->append_rel_array[child_relid] = appinfo;
	}
}

/*
 * expand_planner_arrays
 *		Expand the PlannerInfo's per-RTE arrays by add_size members
 *		and initialize the newly added entries to NULLs
 *
 * Note: this causes the append_rel_array to become allocated even if
 * it was not before.  This is okay for current uses, because we only call
 * this when adding child relations, which always have AppendRelInfos.
 */
void
expand_planner_arrays(PlannerInfo *root, int add_size)
{
	int			new_size;

	Assert(add_size > 0);

	new_size = root->simple_rel_array_size + add_size;

	root->simple_rel_array = (RelOptInfo **)
		repalloc(root->simple_rel_array,
				 sizeof(RelOptInfo *) * new_size);
	MemSet(root->simple_rel_array + root->simple_rel_array_size,
		   0, sizeof(RelOptInfo *) * add_size);

	root->simple_rte_array = (RangeTblEntry **)
		repalloc(root->simple_rte_array,
				 sizeof(RangeTblEntry *) * new_size);
	MemSet(root->simple_rte_array + root->simple_rel_array_size,
		   0, sizeof(RangeTblEntry *) * add_size);

	if (root->append_rel_array)
	{
		root->append_rel_array = (AppendRelInfo **)
			repalloc(root->append_rel_array,
					 sizeof(AppendRelInfo *) * new_size);
		MemSet(root->append_rel_array + root->simple_rel_array_size,
			   0, sizeof(AppendRelInfo *) * add_size);
	}
	else
	{
		root->append_rel_array = (AppendRelInfo **)
			palloc0(sizeof(AppendRelInfo *) * new_size);
	}

	root->simple_rel_array_size = new_size;
}

/*
 * build_simple_rel
 *	  Construct a new RelOptInfo for a base relation or 'other' relation.
 */
RelOptInfo *
build_simple_rel(PlannerInfo *root, int relid, RelOptInfo *parent)
{
	RelOptInfo *rel;
	RangeTblEntry *rte;

	/* Rel should not exist already */
	Assert(relid > 0 && relid < root->simple_rel_array_size);
	if (root->simple_rel_array[relid] != NULL)
		elog(ERROR, "rel %d already exists", relid);

	/* Fetch RTE for relation */
	rte = root->simple_rte_array[relid];
	Assert(rte != NULL);

	rel = makeNode(RelOptInfo);
	rel->reloptkind = parent ? RELOPT_OTHER_MEMBER_REL : RELOPT_BASEREL;
	rel->relids = bms_make_singleton(relid);
	rel->rows = 0;
	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	rel->consider_startup = (root->tuple_fraction > 0);
	rel->consider_param_startup = false;	/* might get changed later */
	rel->consider_parallel = false; /* might get changed later */
	rel->reltarget = create_empty_pathtarget();
	rel->pathlist = NIL;
	rel->ppilist = NIL;
	rel->partial_pathlist = NIL;
	rel->cheapest_startup_path = NULL;
	rel->cheapest_total_path = NULL;
	rel->cheapest_unique_path = NULL;
	rel->cheapest_parameterized_paths = NIL;
	rel->relid = relid;
	rel->rtekind = rte->rtekind;
	/* min_attr, max_attr, attr_needed, attr_widths are set below */
	rel->lateral_vars = NIL;
	rel->indexlist = NIL;
	rel->statlist = NIL;
	rel->pages = 0;
	rel->tuples = 0;
	rel->allvisfrac = 0;
	rel->eclass_indexes = NULL;
	rel->subroot = NULL;
	rel->subplan_params = NIL;
	rel->rel_parallel_workers = -1; /* set up in get_relation_info */
	rel->amflags = 0;
	rel->serverid = InvalidOid;
	rel->userid = rte->checkAsUser;
	rel->useridiscurrent = false;
	rel->fdwroutine = NULL;
	rel->fdw_private = NULL;
	rel->unique_for_rels = NIL;
	rel->non_unique_for_rels = NIL;
	rel->baserestrictinfo = NIL;
	rel->baserestrictcost.startup = 0;
	rel->baserestrictcost.per_tuple = 0;
	rel->baserestrict_min_security = UINT_MAX;
	rel->joininfo = NIL;
	rel->has_eclass_joins = false;
	rel->consider_partitionwise_join = false;	/* might get changed later */
	rel->part_scheme = NULL;
	rel->nparts = -1;
	rel->boundinfo = NULL;
	rel->partbounds_merged = false;
	rel->partition_qual = NIL;
	rel->part_rels = NULL;
	rel->live_parts = NULL;
	rel->all_partrels = NULL;
	rel->partexprs = NULL;
	rel->nullable_partexprs = NULL;

	/*
	 * Pass assorted information down the inheritance hierarchy.
	 */
	if (parent)
	{
		/*
		 * Each direct or indirect child wants to know the relids of its
		 * topmost parent.
		 */
		if (parent->top_parent_relids)
			rel->top_parent_relids = parent->top_parent_relids;
		else
			rel->top_parent_relids = bms_copy(parent->relids);

		/*
		 * Also propagate lateral-reference information from appendrel parent
		 * rels to their child rels.  We intentionally give each child rel the
		 * same minimum parameterization, even though it's quite possible that
		 * some don't reference all the lateral rels.  This is because any
		 * append path for the parent will have to have the same
		 * parameterization for every child anyway, and there's no value in
		 * forcing extra reparameterize_path() calls.  Similarly, a lateral
		 * reference to the parent prevents use of otherwise-movable join rels
		 * for each child.
		 *
		 * It's possible for child rels to have their own children, in which
		 * case the topmost parent's lateral info propagates all the way down.
		 */
		rel->direct_lateral_relids = parent->direct_lateral_relids;
		rel->lateral_relids = parent->lateral_relids;
		rel->lateral_referencers = parent->lateral_referencers;
	}
	else
	{
		rel->top_parent_relids = NULL;
		rel->direct_lateral_relids = NULL;
		rel->lateral_relids = NULL;
		rel->lateral_referencers = NULL;
	}

	/* Check type of rtable entry */
	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* Table --- retrieve statistics from the system catalogs */
			get_relation_info(root, rte->relid, rte->inh, rel);
			break;
		case RTE_SUBQUERY:
		case RTE_FUNCTION:
		case RTE_TABLEFUNC:
		case RTE_VALUES:
		case RTE_CTE:
		case RTE_NAMEDTUPLESTORE:

			/*
			 * Subquery, function, tablefunc, values list, CTE, or ENR --- set
			 * up attr range and arrays
			 *
			 * Note: 0 is included in range to support whole-row Vars
			 */
			rel->min_attr = 0;
			rel->max_attr = list_length(rte->eref->colnames);
			rel->attr_needed = (Relids *)
				palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(Relids));
			rel->attr_widths = (int32 *)
				palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(int32));
			break;
		case RTE_RESULT:
			/* RTE_RESULT has no columns, nor could it have whole-row Var */
			rel->min_attr = 0;
			rel->max_attr = -1;
			rel->attr_needed = NULL;
			rel->attr_widths = NULL;
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d",
				 (int) rte->rtekind);
			break;
	}

	/*
	 * Copy the parent's quals to the child, with appropriate substitution of
	 * variables.  If any constant false or NULL clauses turn up, we can mark
	 * the child as dummy right away.  (We must do this immediately so that
	 * pruning works correctly when recursing in expand_partitioned_rtentry.)
	 */
	if (parent)
	{
		AppendRelInfo *appinfo = root->append_rel_array[relid];

		Assert(appinfo != NULL);
		if (!apply_child_basequals(root, parent, rel, rte, appinfo))
		{
			/*
			 * Some restriction clause reduced to constant FALSE or NULL after
			 * substitution, so this child need not be scanned.
			 */
			mark_dummy_rel(rel);
		}
	}

	/* Save the finished struct in the query's simple_rel_array */
	root->simple_rel_array[relid] = rel;

	return rel;
}

/*
 * find_base_rel
 *	  Find a base or other relation entry, which must already exist.
 */
RelOptInfo *
find_base_rel(PlannerInfo *root, int relid)
{
	RelOptInfo *rel;

	Assert(relid > 0);

	if (relid < root->simple_rel_array_size)
	{
		rel = root->simple_rel_array[relid];
		if (rel)
			return rel;
	}

	elog(ERROR, "no relation entry for relid %d", relid);

	return NULL;				/* keep compiler quiet */
}

/*
 * build_join_rel_hash
 *	  Construct the auxiliary hash table for join relations.
 */
static void
build_join_rel_hash(PlannerInfo *root)
{
	HTAB	   *hashtab;
	HASHCTL		hash_ctl;
	ListCell   *l;

	/* Create the hash table */
	hash_ctl.keysize = sizeof(Relids);
	hash_ctl.entrysize = sizeof(JoinHashEntry);
	hash_ctl.hash = bitmap_hash;
	hash_ctl.match = bitmap_match;
	hash_ctl.hcxt = CurrentMemoryContext;
	hashtab = hash_create("JoinRelHashTable",
						  256L,
						  &hash_ctl,
						  HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

	/* Insert all the already-existing joinrels */
	foreach(l, root->join_rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(l);
		JoinHashEntry *hentry;
		bool		found;

		hentry = (JoinHashEntry *) hash_search(hashtab,
											   &(rel->relids),
											   HASH_ENTER,
											   &found);
		Assert(!found);
		hentry->join_rel = rel;
	}

	root->join_rel_hash = hashtab;
}

/*
 * find_join_rel
 *	  Returns relation entry corresponding to 'relids' (a set of RT indexes),
 *	  or NULL if none exists.  This is for join relations.
 */
RelOptInfo *
find_join_rel(PlannerInfo *root, Relids relids)
{
	/*
	 * Switch to using hash lookup when list grows "too long".  The threshold
	 * is arbitrary and is known only here.
	 */
	if (!root->join_rel_hash && list_length(root->join_rel_list) > 32)
		build_join_rel_hash(root);

	/*
	 * Use either hashtable lookup or linear search, as appropriate.
	 *
	 * Note: the seemingly redundant hashkey variable is used to avoid taking
	 * the address of relids; unless the compiler is exceedingly smart, doing
	 * so would force relids out of a register and thus probably slow down the
	 * list-search case.
	 */
	if (root->join_rel_hash)
	{
		Relids		hashkey = relids;
		JoinHashEntry *hentry;

		hentry = (JoinHashEntry *) hash_search(root->join_rel_hash,
											   &hashkey,
											   HASH_FIND,
											   NULL);
		if (hentry)
			return hentry->join_rel;
	}
	else
	{
		ListCell   *l;

		foreach(l, root->join_rel_list)
		{
			RelOptInfo *rel = (RelOptInfo *) lfirst(l);

			if (bms_equal(rel->relids, relids))
				return rel;
		}
	}

	return NULL;
}

/*
 * set_foreign_rel_properties
 *		Set up foreign-join fields if outer and inner relation are foreign
 *		tables (or joins) belonging to the same server and assigned to the same
 *		user to check access permissions as.
 *
 * In addition to an exact match of userid, we allow the case where one side
 * has zero userid (implying current user) and the other side has explicit
 * userid that happens to equal the current user; but in that case, pushdown of
 * the join is only valid for the current user.  The useridiscurrent field
 * records whether we had to make such an assumption for this join or any
 * sub-join.
 *
 * Otherwise these fields are left invalid, so GetForeignJoinPaths will not be
 * called for the join relation.
 *
 */
static void
set_foreign_rel_properties(RelOptInfo *joinrel, RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel)
{
	if (OidIsValid(outer_rel->serverid) &&
		inner_rel->serverid == outer_rel->serverid)
	{
		if (inner_rel->userid == outer_rel->userid)
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = outer_rel->userid;
			joinrel->useridiscurrent = outer_rel->useridiscurrent || inner_rel->useridiscurrent;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
		else if (!OidIsValid(inner_rel->userid) &&
				 outer_rel->userid == GetUserId())
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = outer_rel->userid;
			joinrel->useridiscurrent = true;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
		else if (!OidIsValid(outer_rel->userid) &&
				 inner_rel->userid == GetUserId())
		{
			joinrel->serverid = outer_rel->serverid;
			joinrel->userid = inner_rel->userid;
			joinrel->useridiscurrent = true;
			joinrel->fdwroutine = outer_rel->fdwroutine;
		}
	}
}

/*
 * add_join_rel
 *		Add given join relation to the list of join relations in the given
 *		PlannerInfo. Also add it to the auxiliary hashtable if there is one.
 */
static void
add_join_rel(PlannerInfo *root, RelOptInfo *joinrel)
{
	/* GEQO requires us to append the new joinrel to the end of the list! */
	root->join_rel_list = lappend(root->join_rel_list, joinrel);

	/* store it into the auxiliary hashtable if there is one. */
	if (root->join_rel_hash)
	{
		JoinHashEntry *hentry;
		bool		found;

		hentry = (JoinHashEntry *) hash_search(root->join_rel_hash,
											   &(joinrel->relids),
											   HASH_ENTER,
											   &found);
		Assert(!found);
		hentry->join_rel = joinrel;
	}
}

/*
 * build_join_rel
 *	  Returns relation entry corresponding to the union of two given rels,
 *	  creating a new relation entry if none already exists.
 *
 * 'joinrelids' is the Relids set that uniquely identifies the join
 * 'outer_rel' and 'inner_rel' are relation nodes for the relations to be
 *		joined
 * 'sjinfo': join context info
 * 'restrictlist_ptr': result variable.  If not NULL, *restrictlist_ptr
 *		receives the list of RestrictInfo nodes that apply to this
 *		particular pair of joinable relations.
 *
 * restrictlist_ptr makes the routine's API a little grotty, but it saves
 * duplicated calculation of the restrictlist...
 */
RelOptInfo *
build_join_rel(PlannerInfo *root,
			   Relids joinrelids,
			   RelOptInfo *outer_rel,
			   RelOptInfo *inner_rel,
			   SpecialJoinInfo *sjinfo,
			   List **restrictlist_ptr)
{
	RelOptInfo *joinrel;
	List	   *restrictlist;

	/* This function should be used only for join between parents. */
	Assert(!IS_OTHER_REL(outer_rel) && !IS_OTHER_REL(inner_rel));

	/*
	 * See if we already have a joinrel for this set of base rels.
	 */
	joinrel = find_join_rel(root, joinrelids);

	if (joinrel)
	{
		/*
		 * Yes, so we only need to figure the restrictlist for this particular
		 * pair of component relations.
		 */
		if (restrictlist_ptr)
			*restrictlist_ptr = build_joinrel_restrictlist(root,
														   joinrel,
														   outer_rel,
														   inner_rel);
		return joinrel;
	}

	/*
	 * Nope, so make one.
	 */
	joinrel = makeNode(RelOptInfo);
	joinrel->reloptkind = RELOPT_JOINREL;
	joinrel->relids = bms_copy(joinrelids);
	joinrel->rows = 0;
	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	joinrel->consider_startup = (root->tuple_fraction > 0);
	joinrel->consider_param_startup = false;
	joinrel->consider_parallel = false;
	joinrel->reltarget = create_empty_pathtarget();
	joinrel->pathlist = NIL;
	joinrel->ppilist = NIL;
	joinrel->partial_pathlist = NIL;
	joinrel->cheapest_startup_path = NULL;
	joinrel->cheapest_total_path = NULL;
	joinrel->cheapest_unique_path = NULL;
	joinrel->cheapest_parameterized_paths = NIL;
	/* init direct_lateral_relids from children; we'll finish it up below */
	joinrel->direct_lateral_relids =
		bms_union(outer_rel->direct_lateral_relids,
				  inner_rel->direct_lateral_relids);
	joinrel->lateral_relids = min_join_parameterization(root, joinrel->relids,
														outer_rel, inner_rel);
	joinrel->relid = 0;			/* indicates not a baserel */
	joinrel->rtekind = RTE_JOIN;
	joinrel->min_attr = 0;
	joinrel->max_attr = 0;
	joinrel->attr_needed = NULL;
	joinrel->attr_widths = NULL;
	joinrel->lateral_vars = NIL;
	joinrel->lateral_referencers = NULL;
	joinrel->indexlist = NIL;
	joinrel->statlist = NIL;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->allvisfrac = 0;
	joinrel->eclass_indexes = NULL;
	joinrel->subroot = NULL;
	joinrel->subplan_params = NIL;
	joinrel->rel_parallel_workers = -1;
	joinrel->amflags = 0;
	joinrel->serverid = InvalidOid;
	joinrel->userid = InvalidOid;
	joinrel->useridiscurrent = false;
	joinrel->fdwroutine = NULL;
	joinrel->fdw_private = NULL;
	joinrel->unique_for_rels = NIL;
	joinrel->non_unique_for_rels = NIL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->baserestrict_min_security = UINT_MAX;
	joinrel->joininfo = NIL;
	joinrel->has_eclass_joins = false;
	joinrel->consider_partitionwise_join = false;	/* might get changed later */
	joinrel->top_parent_relids = NULL;
	joinrel->part_scheme = NULL;
	joinrel->nparts = -1;
	joinrel->boundinfo = NULL;
	joinrel->partbounds_merged = false;
	joinrel->partition_qual = NIL;
	joinrel->part_rels = NULL;
	joinrel->live_parts = NULL;
	joinrel->all_partrels = NULL;
	joinrel->partexprs = NULL;
	joinrel->nullable_partexprs = NULL;

	/* Compute information relevant to the foreign relations. */
	set_foreign_rel_properties(joinrel, outer_rel, inner_rel);

	/*
	 * Create a new tlist containing just the vars that need to be output from
	 * this join (ie, are needed for higher joinclauses or final output).
	 *
	 * NOTE: the tlist order for a join rel will depend on which pair of outer
	 * and inner rels we first try to build it from.  But the contents should
	 * be the same regardless.
	 */
	build_joinrel_tlist(root, joinrel, outer_rel);
	build_joinrel_tlist(root, joinrel, inner_rel);
	add_placeholders_to_joinrel(root, joinrel, outer_rel, inner_rel);

	/*
	 * add_placeholders_to_joinrel also took care of adding the ph_lateral
	 * sets of any PlaceHolderVars computed here to direct_lateral_relids, so
	 * now we can finish computing that.  This is much like the computation of
	 * the transitively-closed lateral_relids in min_join_parameterization,
	 * except that here we *do* have to consider the added PHVs.
	 */
	joinrel->direct_lateral_relids =
		bms_del_members(joinrel->direct_lateral_relids, joinrel->relids);
	if (bms_is_empty(joinrel->direct_lateral_relids))
		joinrel->direct_lateral_relids = NULL;

	/*
	 * Construct restrict and join clause lists for the new joinrel. (The
	 * caller might or might not need the restrictlist, but I need it anyway
	 * for set_joinrel_size_estimates().)
	 */
	restrictlist = build_joinrel_restrictlist(root, joinrel,
											  outer_rel, inner_rel);
	if (restrictlist_ptr)
		*restrictlist_ptr = restrictlist;
	build_joinrel_joinlist(joinrel, outer_rel, inner_rel);

	/*
	 * This is also the right place to check whether the joinrel has any
	 * pending EquivalenceClass joins.
	 */
	joinrel->has_eclass_joins = has_relevant_eclass_joinclause(root, joinrel);

	/* Store the partition information. */
	build_joinrel_partition_info(joinrel, outer_rel, inner_rel, restrictlist,
								 sjinfo->jointype);

	/*
	 * Set estimates of the joinrel's size.
	 */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
							   sjinfo, restrictlist);

	/*
	 * Set the consider_parallel flag if this joinrel could potentially be
	 * scanned within a parallel worker.  If this flag is false for either
	 * inner_rel or outer_rel, then it must be false for the joinrel also.
	 * Even if both are true, there might be parallel-restricted expressions
	 * in the targetlist or quals.
	 *
	 * Note that if there are more than two rels in this relation, they could
	 * be divided between inner_rel and outer_rel in any arbitrary way.  We
	 * assume this doesn't matter, because we should hit all the same baserels
	 * and joinclauses while building up to this joinrel no matter which we
	 * take; therefore, we should make the same decision here however we get
	 * here.
	 */
	if (inner_rel->consider_parallel && outer_rel->consider_parallel &&
		is_parallel_safe(root, (Node *) restrictlist) &&
		is_parallel_safe(root, (Node *) joinrel->reltarget->exprs))
		joinrel->consider_parallel = true;

	/* Add the joinrel to the PlannerInfo. */
	add_join_rel(root, joinrel);

	/*
	 * Also, if dynamic-programming join search is active, add the new joinrel
	 * to the appropriate sublist.  Note: you might think the Assert on number
	 * of members should be for equality, but some of the level 1 rels might
	 * have been joinrels already, so we can only assert <=.
	 */
	if (root->join_rel_level)
	{
		Assert(root->join_cur_level > 0);
		Assert(root->join_cur_level <= bms_num_members(joinrel->relids));
		root->join_rel_level[root->join_cur_level] =
			lappend(root->join_rel_level[root->join_cur_level], joinrel);
	}

	return joinrel;
}

/*
 * build_child_join_rel
 *	  Builds RelOptInfo representing join between given two child relations.
 *
 * 'outer_rel' and 'inner_rel' are the RelOptInfos of child relations being
 *		joined
 * 'parent_joinrel' is the RelOptInfo representing the join between parent
 *		relations. Some of the members of new RelOptInfo are produced by
 *		translating corresponding members of this RelOptInfo
 * 'sjinfo': child-join context info
 * 'restrictlist': list of RestrictInfo nodes that apply to this particular
 *		pair of joinable relations
 * 'jointype' is the join type (inner, left, full, etc)
 */
RelOptInfo *
build_child_join_rel(PlannerInfo *root, RelOptInfo *outer_rel,
					 RelOptInfo *inner_rel, RelOptInfo *parent_joinrel,
					 List *restrictlist, SpecialJoinInfo *sjinfo,
					 JoinType jointype)
{
	RelOptInfo *joinrel = makeNode(RelOptInfo);
	AppendRelInfo **appinfos;
	int			nappinfos;

	/* Only joins between "other" relations land here. */
	Assert(IS_OTHER_REL(outer_rel) && IS_OTHER_REL(inner_rel));

	/* The parent joinrel should have consider_partitionwise_join set. */
	Assert(parent_joinrel->consider_partitionwise_join);

	joinrel->reloptkind = RELOPT_OTHER_JOINREL;
	joinrel->relids = bms_union(outer_rel->relids, inner_rel->relids);
	joinrel->rows = 0;
	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	joinrel->consider_startup = (root->tuple_fraction > 0);
	joinrel->consider_param_startup = false;
	joinrel->consider_parallel = false;
	joinrel->reltarget = create_empty_pathtarget();
	joinrel->pathlist = NIL;
	joinrel->ppilist = NIL;
	joinrel->partial_pathlist = NIL;
	joinrel->cheapest_startup_path = NULL;
	joinrel->cheapest_total_path = NULL;
	joinrel->cheapest_unique_path = NULL;
	joinrel->cheapest_parameterized_paths = NIL;
	joinrel->direct_lateral_relids = NULL;
	joinrel->lateral_relids = NULL;
	joinrel->relid = 0;			/* indicates not a baserel */
	joinrel->rtekind = RTE_JOIN;
	joinrel->min_attr = 0;
	joinrel->max_attr = 0;
	joinrel->attr_needed = NULL;
	joinrel->attr_widths = NULL;
	joinrel->lateral_vars = NIL;
	joinrel->lateral_referencers = NULL;
	joinrel->indexlist = NIL;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->allvisfrac = 0;
	joinrel->eclass_indexes = NULL;
	joinrel->subroot = NULL;
	joinrel->subplan_params = NIL;
	joinrel->amflags = 0;
	joinrel->serverid = InvalidOid;
	joinrel->userid = InvalidOid;
	joinrel->useridiscurrent = false;
	joinrel->fdwroutine = NULL;
	joinrel->fdw_private = NULL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->joininfo = NIL;
	joinrel->has_eclass_joins = false;
	joinrel->consider_partitionwise_join = false;	/* might get changed later */
	joinrel->top_parent_relids = NULL;
	joinrel->part_scheme = NULL;
	joinrel->nparts = -1;
	joinrel->boundinfo = NULL;
	joinrel->partbounds_merged = false;
	joinrel->partition_qual = NIL;
	joinrel->part_rels = NULL;
	joinrel->live_parts = NULL;
	joinrel->all_partrels = NULL;
	joinrel->partexprs = NULL;
	joinrel->nullable_partexprs = NULL;

	joinrel->top_parent_relids = bms_union(outer_rel->top_parent_relids,
										   inner_rel->top_parent_relids);

	/* Compute information relevant to foreign relations. */
	set_foreign_rel_properties(joinrel, outer_rel, inner_rel);

	/* Compute information needed for mapping Vars to the child rel */
	appinfos = find_appinfos_by_relids(root, joinrel->relids, &nappinfos);

	/* Set up reltarget struct */
	build_child_join_reltarget(root, parent_joinrel, joinrel,
							   nappinfos, appinfos);

	/* Construct joininfo list. */
	joinrel->joininfo = (List *) adjust_appendrel_attrs(root,
														(Node *) parent_joinrel->joininfo,
														nappinfos,
														appinfos);

	/*
	 * Lateral relids referred in child join will be same as that referred in
	 * the parent relation.
	 */
	joinrel->direct_lateral_relids = (Relids) bms_copy(parent_joinrel->direct_lateral_relids);
	joinrel->lateral_relids = (Relids) bms_copy(parent_joinrel->lateral_relids);

	/*
	 * If the parent joinrel has pending equivalence classes, so does the
	 * child.
	 */
	joinrel->has_eclass_joins = parent_joinrel->has_eclass_joins;

	/* Is the join between partitions itself partitioned? */
	build_joinrel_partition_info(joinrel, outer_rel, inner_rel, restrictlist,
								 jointype);

	/* Child joinrel is parallel safe if parent is parallel safe. */
	joinrel->consider_parallel = parent_joinrel->consider_parallel;

	/* Set estimates of the child-joinrel's size. */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
							   sjinfo, restrictlist);

	/* We build the join only once. */
	Assert(!find_join_rel(root, joinrel->relids));

	/* Add the relation to the PlannerInfo. */
	add_join_rel(root, joinrel);

	/*
	 * We might need EquivalenceClass members corresponding to the child join,
	 * so that we can represent sort pathkeys for it.  As with children of
	 * baserels, we shouldn't need this unless there are relevant eclass joins
	 * (implying that a merge join might be possible) or pathkeys to sort by.
	 */
	if (joinrel->has_eclass_joins || has_useful_pathkeys(root, parent_joinrel))
		add_child_join_rel_equivalences(root,
										nappinfos, appinfos,
										parent_joinrel, joinrel);

	pfree(appinfos);

	return joinrel;
}

/*
 * min_join_parameterization
 *
 * Determine the minimum possible parameterization of a joinrel, that is, the
 * set of other rels it contains LATERAL references to.  We save this value in
 * the join's RelOptInfo.  This function is split out of build_join_rel()
 * because join_is_legal() needs the value to check a prospective join.
 */
Relids
min_join_parameterization(PlannerInfo *root,
						  Relids joinrelids,
						  RelOptInfo *outer_rel,
						  RelOptInfo *inner_rel)
{
	Relids		result;

	/*
	 * Basically we just need the union of the inputs' lateral_relids, less
	 * whatever is already in the join.
	 *
	 * It's not immediately obvious that this is a valid way to compute the
	 * result, because it might seem that we're ignoring possible lateral refs
	 * of PlaceHolderVars that are due to be computed at the join but not in
	 * either input.  However, because create_lateral_join_info() already
	 * charged all such PHV refs to each member baserel of the join, they'll
	 * be accounted for already in the inputs' lateral_relids.  Likewise, we
	 * do not need to worry about doing transitive closure here, because that
	 * was already accounted for in the original baserel lateral_relids.
	 */
	result = bms_union(outer_rel->lateral_relids, inner_rel->lateral_relids);
	result = bms_del_members(result, joinrelids);

	/* Maintain invariant that result is exactly NULL if empty */
	if (bms_is_empty(result))
		result = NULL;

	return result;
}

/*
 * build_joinrel_tlist
 *	  Builds a join relation's target list from an input relation.
 *	  (This is invoked twice to handle the two input relations.)
 *
 * The join's targetlist includes all Vars of its member relations that
 * will still be needed above the join.  This subroutine adds all such
 * Vars from the specified input rel's tlist to the join rel's tlist.
 *
 * We also compute the expected width of the join's output, making use
 * of data that was cached at the baserel level by set_rel_width().
 */
static void
build_joinrel_tlist(PlannerInfo *root, RelOptInfo *joinrel,
					RelOptInfo *input_rel)
{
	Relids		relids = joinrel->relids;
	ListCell   *vars;

	foreach(vars, input_rel->reltarget->exprs)
	{
		Var		   *var = (Var *) lfirst(vars);

		/*
		 * Ignore PlaceHolderVars in the input tlists; we'll make our own
		 * decisions about whether to copy them.
		 */
		if (IsA(var, PlaceHolderVar))
			continue;

		/*
		 * Otherwise, anything in a baserel or joinrel targetlist ought to be
		 * a Var.  (More general cases can only appear in appendrel child
		 * rels, which will never be seen here.)
		 */
		if (!IsA(var, Var))
			elog(ERROR, "unexpected node type in rel targetlist: %d",
				 (int) nodeTag(var));

		if (var->varno == ROWID_VAR)
		{
			/* UPDATE/DELETE row identity vars are always needed */
			RowIdentityVarInfo *ridinfo = (RowIdentityVarInfo *)
			list_nth(root->row_identity_vars, var->varattno - 1);

			joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs,
												var);
			/* Vars have cost zero, so no need to adjust reltarget->cost */
			joinrel->reltarget->width += ridinfo->rowidwidth;
		}
		else
		{
			RelOptInfo *baserel;
			int			ndx;

			/* Get the Var's original base rel */
			baserel = find_base_rel(root, var->varno);

			/* Is it still needed above this joinrel? */
			ndx = var->varattno - baserel->min_attr;
			if (bms_nonempty_difference(baserel->attr_needed[ndx], relids))
			{
				/* Yup, add it to the output */
				joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs,
													var);
				/* Vars have cost zero, so no need to adjust reltarget->cost */
				joinrel->reltarget->width += baserel->attr_widths[ndx];
			}
		}
	}
}

/*
 * build_joinrel_restrictlist
 * build_joinrel_joinlist
 *	  These routines build lists of restriction and join clauses for a
 *	  join relation from the joininfo lists of the relations it joins.
 *
 *	  These routines are separate because the restriction list must be
 *	  built afresh for each pair of input sub-relations we consider, whereas
 *	  the join list need only be computed once for any join RelOptInfo.
 *	  The join list is fully determined by the set of rels making up the
 *	  joinrel, so we should get the same results (up to ordering) from any
 *	  candidate pair of sub-relations.  But the restriction list is whatever
 *	  is not handled in the sub-relations, so it depends on which
 *	  sub-relations are considered.
 *
 *	  If a join clause from an input relation refers to base rels still not
 *	  present in the joinrel, then it is still a join clause for the joinrel;
 *	  we put it into the joininfo list for the joinrel.  Otherwise,
 *	  the clause is now a restrict clause for the joined relation, and we
 *	  return it to the caller of build_joinrel_restrictlist() to be stored in
 *	  join paths made from this pair of sub-relations.  (It will not need to
 *	  be considered further up the join tree.)
 *
 *	  In many cases we will find the same RestrictInfos in both input
 *	  relations' joinlists, so be careful to eliminate duplicates.
 *	  Pointer equality should be a sufficient test for dups, since all
 *	  the various joinlist entries ultimately refer to RestrictInfos
 *	  pushed into them by distribute_restrictinfo_to_rels().
 *
 * 'joinrel' is a join relation node
 * 'outer_rel' and 'inner_rel' are a pair of relations that can be joined
 *		to form joinrel.
 *
 * build_joinrel_restrictlist() returns a list of relevant restrictinfos,
 * whereas build_joinrel_joinlist() stores its results in the joinrel's
 * joininfo list.  One or the other must accept each given clause!
 *
 * NB: Formerly, we made deep(!) copies of each input RestrictInfo to pass
 * up to the join relation.  I believe this is no longer necessary, because
 * RestrictInfo nodes are no longer context-dependent.  Instead, just include
 * the original nodes in the lists made for the join relation.
 */
static List *
build_joinrel_restrictlist(PlannerInfo *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel)
{
	List	   *result;

	/*
	 * Collect all the clauses that syntactically belong at this level,
	 * eliminating any duplicates (important since we will see many of the
	 * same clauses arriving from both input relations).
	 */
	result = subbuild_joinrel_restrictlist(joinrel, outer_rel->joininfo, NIL);
	result = subbuild_joinrel_restrictlist(joinrel, inner_rel->joininfo, result);

	/*
	 * Add on any clauses derived from EquivalenceClasses.  These cannot be
	 * redundant with the clauses in the joininfo lists, so don't bother
	 * checking.
	 */
	result = list_concat(result,
						 generate_join_implied_equalities(root,
														  joinrel->relids,
														  outer_rel->relids,
														  inner_rel));

	return result;
}

static void
build_joinrel_joinlist(RelOptInfo *joinrel,
					   RelOptInfo *outer_rel,
					   RelOptInfo *inner_rel)
{
	List	   *result;

	/*
	 * Collect all the clauses that syntactically belong above this level,
	 * eliminating any duplicates (important since we will see many of the
	 * same clauses arriving from both input relations).
	 */
	result = subbuild_joinrel_joinlist(joinrel, outer_rel->joininfo, NIL);
	result = subbuild_joinrel_joinlist(joinrel, inner_rel->joininfo, result);

	joinrel->joininfo = result;
}

static List *
subbuild_joinrel_restrictlist(RelOptInfo *joinrel,
							  List *joininfo_list,
							  List *new_restrictlist)
{
	ListCell   *l;

	foreach(l, joininfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_is_subset(rinfo->required_relids, joinrel->relids))
		{
			/*
			 * This clause becomes a restriction clause for the joinrel, since
			 * it refers to no outside rels.  Add it to the list, being
			 * careful to eliminate duplicates. (Since RestrictInfo nodes in
			 * different joinlists will have been multiply-linked rather than
			 * copied, pointer equality should be a sufficient test.)
			 */
			new_restrictlist = list_append_unique_ptr(new_restrictlist, rinfo);
		}
		else
		{
			/*
			 * This clause is still a join clause at this level, so we ignore
			 * it in this routine.
			 */
		}
	}

	return new_restrictlist;
}

static List *
subbuild_joinrel_joinlist(RelOptInfo *joinrel,
						  List *joininfo_list,
						  List *new_joininfo)
{
	ListCell   *l;

	/* Expected to be called only for join between parent relations. */
	Assert(joinrel->reloptkind == RELOPT_JOINREL);

	foreach(l, joininfo_list)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_is_subset(rinfo->required_relids, joinrel->relids))
		{
			/*
			 * This clause becomes a restriction clause for the joinrel, since
			 * it refers to no outside rels.  So we can ignore it in this
			 * routine.
			 */
		}
		else
		{
			/*
			 * This clause is still a join clause at this level, so add it to
			 * the new joininfo list, being careful to eliminate duplicates.
			 * (Since RestrictInfo nodes in different joinlists will have been
			 * multiply-linked rather than copied, pointer equality should be
			 * a sufficient test.)
			 */
			new_joininfo = list_append_unique_ptr(new_joininfo, rinfo);
		}
	}

	return new_joininfo;
}


/*
 * fetch_upper_rel
 *		Build a RelOptInfo describing some post-scan/join query processing,
 *		or return a pre-existing one if somebody already built it.
 *
 * An "upper" relation is identified by an UpperRelationKind and a Relids set.
 * The meaning of the Relids set is not specified here, and very likely will
 * vary for different relation kinds.
 *
 * Most of the fields in an upper-level RelOptInfo are not used and are not
 * set here (though makeNode should ensure they're zeroes).  We basically only
 * care about fields that are of interest to add_path() and set_cheapest().
 */
RelOptInfo *
fetch_upper_rel(PlannerInfo *root, UpperRelationKind kind, Relids relids)
{
	RelOptInfo *upperrel;
	ListCell   *lc;

	/*
	 * For the moment, our indexing data structure is just a List for each
	 * relation kind.  If we ever get so many of one kind that this stops
	 * working well, we can improve it.  No code outside this function should
	 * assume anything about how to find a particular upperrel.
	 */

	/* If we already made this upperrel for the query, return it */
	foreach(lc, root->upper_rels[kind])
	{
		upperrel = (RelOptInfo *) lfirst(lc);

		if (bms_equal(upperrel->relids, relids))
			return upperrel;
	}

	upperrel = makeNode(RelOptInfo);
	upperrel->reloptkind = RELOPT_UPPER_REL;
	upperrel->relids = bms_copy(relids);

	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	upperrel->consider_startup = (root->tuple_fraction > 0);
	upperrel->consider_param_startup = false;
	upperrel->consider_parallel = false;	/* might get changed later */
	upperrel->reltarget = create_empty_pathtarget();
	upperrel->pathlist = NIL;
	upperrel->cheapest_startup_path = NULL;
	upperrel->cheapest_total_path = NULL;
	upperrel->cheapest_unique_path = NULL;
	upperrel->cheapest_parameterized_paths = NIL;

	root->upper_rels[kind] = lappend(root->upper_rels[kind], upperrel);

	return upperrel;
}


/*
 * find_childrel_parents
 *		Compute the set of parent relids of an appendrel child rel.
 *
 * Since appendrels can be nested, a child could have multiple levels of
 * appendrel ancestors.  This function computes a Relids set of all the
 * parent relation IDs.
 */
Relids
find_childrel_parents(PlannerInfo *root, RelOptInfo *rel)
{
	Relids		result = NULL;

	Assert(rel->reloptkind == RELOPT_OTHER_MEMBER_REL);
	Assert(rel->relid > 0 && rel->relid < root->simple_rel_array_size);

	do
	{
		AppendRelInfo *appinfo = root->append_rel_array[rel->relid];
		Index		prelid = appinfo->parent_relid;

		result = bms_add_member(result, prelid);

		/* traverse up to the parent rel, loop if it's also a child rel */
		rel = find_base_rel(root, prelid);
	} while (rel->reloptkind == RELOPT_OTHER_MEMBER_REL);

	Assert(rel->reloptkind == RELOPT_BASEREL);

	return result;
}


/*
 * get_baserel_parampathinfo
 *		Get the ParamPathInfo for a parameterized path for a base relation,
 *		constructing one if we don't have one already.
 *
 * This centralizes estimating the rowcounts for parameterized paths.
 * We need to cache those to be sure we use the same rowcount for all paths
 * of the same parameterization for a given rel.  This is also a convenient
 * place to determine which movable join clauses the parameterized path will
 * be responsible for evaluating.
 */
ParamPathInfo *
get_baserel_parampathinfo(PlannerInfo *root, RelOptInfo *baserel,
						  Relids required_outer)
{
	ParamPathInfo *ppi;
	Relids		joinrelids;
	List	   *pclauses;
	double		rows;
	ListCell   *lc;

	/* If rel has LATERAL refs, every path for it should account for them */
	Assert(bms_is_subset(baserel->lateral_relids, required_outer));

	/* Unparameterized paths have no ParamPathInfo */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(baserel->relids, required_outer));

	/* If we already have a PPI for this parameterization, just return it */
	if ((ppi = find_param_path_info(baserel, required_outer)))
		return ppi;

	/*
	 * Identify all joinclauses that are movable to this base rel given this
	 * parameterization.
	 */
	joinrelids = bms_union(baserel->relids, required_outer);
	pclauses = NIL;
	foreach(lc, baserel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (join_clause_is_movable_into(rinfo,
										baserel->relids,
										joinrelids))
			pclauses = lappend(pclauses, rinfo);
	}

	/*
	 * Add in joinclauses generated by EquivalenceClasses, too.  (These
	 * necessarily satisfy join_clause_is_movable_into.)
	 */
	pclauses = list_concat(pclauses,
						   generate_join_implied_equalities(root,
															joinrelids,
															required_outer,
															baserel));

	/* Estimate the number of rows returned by the parameterized scan */
	rows = get_parameterized_baserel_size(root, baserel, pclauses);

	/* And now we can build the ParamPathInfo */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = rows;
	ppi->ppi_clauses = pclauses;
	baserel->ppilist = lappend(baserel->ppilist, ppi);

	return ppi;
}

/*
 * get_joinrel_parampathinfo
 *		Get the ParamPathInfo for a parameterized path for a join relation,
 *		constructing one if we don't have one already.
 *
 * This centralizes estimating the rowcounts for parameterized paths.
 * We need to cache those to be sure we use the same rowcount for all paths
 * of the same parameterization for a given rel.  This is also a convenient
 * place to determine which movable join clauses the parameterized path will
 * be responsible for evaluating.
 *
 * outer_path and inner_path are a pair of input paths that can be used to
 * construct the join, and restrict_clauses is the list of regular join
 * clauses (including clauses derived from EquivalenceClasses) that must be
 * applied at the join node when using these inputs.
 *
 * Unlike the situation for base rels, the set of movable join clauses to be
 * enforced at a join varies with the selected pair of input paths, so we
 * must calculate that and pass it back, even if we already have a matching
 * ParamPathInfo.  We handle this by adding any clauses moved down to this
 * join to *restrict_clauses, which is an in/out parameter.  (The addition
 * is done in such a way as to not modify the passed-in List structure.)
 *
 * Note: when considering a nestloop join, the caller must have removed from
 * restrict_clauses any movable clauses that are themselves scheduled to be
 * pushed into the right-hand path.  We do not do that here since it's
 * unnecessary for other join types.
 */
ParamPathInfo *
get_joinrel_parampathinfo(PlannerInfo *root, RelOptInfo *joinrel,
						  Path *outer_path,
						  Path *inner_path,
						  SpecialJoinInfo *sjinfo,
						  Relids required_outer,
						  List **restrict_clauses)
{
	ParamPathInfo *ppi;
	Relids		join_and_req;
	Relids		outer_and_req;
	Relids		inner_and_req;
	List	   *pclauses;
	List	   *eclauses;
	List	   *dropped_ecs;
	double		rows;
	ListCell   *lc;

	/* If rel has LATERAL refs, every path for it should account for them */
	Assert(bms_is_subset(joinrel->lateral_relids, required_outer));

	/* Unparameterized paths have no ParamPathInfo or extra join clauses */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(joinrel->relids, required_outer));

	/*
	 * Identify all joinclauses that are movable to this join rel given this
	 * parameterization.  These are the clauses that are movable into this
	 * join, but not movable into either input path.  Treat an unparameterized
	 * input path as not accepting parameterized clauses (because it won't,
	 * per the shortcut exit above), even though the joinclause movement rules
	 * might allow the same clauses to be moved into a parameterized path for
	 * that rel.
	 */
	join_and_req = bms_union(joinrel->relids, required_outer);
	if (outer_path->param_info)
		outer_and_req = bms_union(outer_path->parent->relids,
								  PATH_REQ_OUTER(outer_path));
	else
		outer_and_req = NULL;	/* outer path does not accept parameters */
	if (inner_path->param_info)
		inner_and_req = bms_union(inner_path->parent->relids,
								  PATH_REQ_OUTER(inner_path));
	else
		inner_and_req = NULL;	/* inner path does not accept parameters */

	pclauses = NIL;
	foreach(lc, joinrel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		if (join_clause_is_movable_into(rinfo,
										joinrel->relids,
										join_and_req) &&
			!join_clause_is_movable_into(rinfo,
										 outer_path->parent->relids,
										 outer_and_req) &&
			!join_clause_is_movable_into(rinfo,
										 inner_path->parent->relids,
										 inner_and_req))
			pclauses = lappend(pclauses, rinfo);
	}

	/* Consider joinclauses generated by EquivalenceClasses, too */
	eclauses = generate_join_implied_equalities(root,
												join_and_req,
												required_outer,
												joinrel);
	/* We only want ones that aren't movable to lower levels */
	dropped_ecs = NIL;
	foreach(lc, eclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/*
		 * In principle, join_clause_is_movable_into() should accept anything
		 * returned by generate_join_implied_equalities(); but because its
		 * analysis is only approximate, sometimes it doesn't.  So we
		 * currently cannot use this Assert; instead just assume it's okay to
		 * apply the joinclause at this level.
		 */
#ifdef NOT_USED
		Assert(join_clause_is_movable_into(rinfo,
										   joinrel->relids,
										   join_and_req));
#endif
		if (join_clause_is_movable_into(rinfo,
										outer_path->parent->relids,
										outer_and_req))
			continue;			/* drop if movable into LHS */
		if (join_clause_is_movable_into(rinfo,
										inner_path->parent->relids,
										inner_and_req))
		{
			/* drop if movable into RHS, but remember EC for use below */
			Assert(rinfo->left_ec == rinfo->right_ec);
			dropped_ecs = lappend(dropped_ecs, rinfo->left_ec);
			continue;
		}
		pclauses = lappend(pclauses, rinfo);
	}

	/*
	 * EquivalenceClasses are harder to deal with than we could wish, because
	 * of the fact that a given EC can generate different clauses depending on
	 * context.  Suppose we have an EC {X.X, Y.Y, Z.Z} where X and Y are the
	 * LHS and RHS of the current join and Z is in required_outer, and further
	 * suppose that the inner_path is parameterized by both X and Z.  The code
	 * above will have produced either Z.Z = X.X or Z.Z = Y.Y from that EC,
	 * and in the latter case will have discarded it as being movable into the
	 * RHS.  However, the EC machinery might have produced either Y.Y = X.X or
	 * Y.Y = Z.Z as the EC enforcement clause within the inner_path; it will
	 * not have produced both, and we can't readily tell from here which one
	 * it did pick.  If we add no clause to this join, we'll end up with
	 * insufficient enforcement of the EC; either Z.Z or X.X will fail to be
	 * constrained to be equal to the other members of the EC.  (When we come
	 * to join Z to this X/Y path, we will certainly drop whichever EC clause
	 * is generated at that join, so this omission won't get fixed later.)
	 *
	 * To handle this, for each EC we discarded such a clause from, try to
	 * generate a clause connecting the required_outer rels to the join's LHS
	 * ("Z.Z = X.X" in the terms of the above example).  If successful, and if
	 * the clause can't be moved to the LHS, add it to the current join's
	 * restriction clauses.  (If an EC cannot generate such a clause then it
	 * has nothing that needs to be enforced here, while if the clause can be
	 * moved into the LHS then it should have been enforced within that path.)
	 *
	 * Note that we don't need similar processing for ECs whose clause was
	 * considered to be movable into the LHS, because the LHS can't refer to
	 * the RHS so there is no comparable ambiguity about what it might
	 * actually be enforcing internally.
	 */
	if (dropped_ecs)
	{
		Relids		real_outer_and_req;

		real_outer_and_req = bms_union(outer_path->parent->relids,
									   required_outer);
		eclauses =
			generate_join_implied_equalities_for_ecs(root,
													 dropped_ecs,
													 real_outer_and_req,
													 required_outer,
													 outer_path->parent);
		foreach(lc, eclauses)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			/* As above, can't quite assert this here */
#ifdef NOT_USED
			Assert(join_clause_is_movable_into(rinfo,
											   outer_path->parent->relids,
											   real_outer_and_req));
#endif
			if (!join_clause_is_movable_into(rinfo,
											 outer_path->parent->relids,
											 outer_and_req))
				pclauses = lappend(pclauses, rinfo);
		}
	}

	/*
	 * Now, attach the identified moved-down clauses to the caller's
	 * restrict_clauses list.  By using list_concat in this order, we leave
	 * the original list structure of restrict_clauses undamaged.
	 */
	*restrict_clauses = list_concat(pclauses, *restrict_clauses);

	/* If we already have a PPI for this parameterization, just return it */
	if ((ppi = find_param_path_info(joinrel, required_outer)))
		return ppi;

	/* Estimate the number of rows returned by the parameterized join */
	rows = get_parameterized_joinrel_size(root, joinrel,
										  outer_path,
										  inner_path,
										  sjinfo,
										  *restrict_clauses);

	/*
	 * And now we can build the ParamPathInfo.  No point in saving the
	 * input-pair-dependent clause list, though.
	 *
	 * Note: in GEQO mode, we'll be called in a temporary memory context, but
	 * the joinrel structure is there too, so no problem.
	 */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = rows;
	ppi->ppi_clauses = NIL;
	joinrel->ppilist = lappend(joinrel->ppilist, ppi);

	return ppi;
}

/*
 * get_appendrel_parampathinfo
 *		Get the ParamPathInfo for a parameterized path for an append relation.
 *
 * For an append relation, the rowcount estimate will just be the sum of
 * the estimates for its children.  However, we still need a ParamPathInfo
 * to flag the fact that the path requires parameters.  So this just creates
 * a suitable struct with zero ppi_rows (and no ppi_clauses either, since
 * the Append node isn't responsible for checking quals).
 */
ParamPathInfo *
get_appendrel_parampathinfo(RelOptInfo *appendrel, Relids required_outer)
{
	ParamPathInfo *ppi;

	/* If rel has LATERAL refs, every path for it should account for them */
	Assert(bms_is_subset(appendrel->lateral_relids, required_outer));

	/* Unparameterized paths have no ParamPathInfo */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(appendrel->relids, required_outer));

	/* If we already have a PPI for this parameterization, just return it */
	if ((ppi = find_param_path_info(appendrel, required_outer)))
		return ppi;

	/* Else build the ParamPathInfo */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = 0;
	ppi->ppi_clauses = NIL;
	appendrel->ppilist = lappend(appendrel->ppilist, ppi);

	return ppi;
}

/*
 * Returns a ParamPathInfo for the parameterization given by required_outer, if
 * already available in the given rel. Returns NULL otherwise.
 */
ParamPathInfo *
find_param_path_info(RelOptInfo *rel, Relids required_outer)
{
	ListCell   *lc;

	foreach(lc, rel->ppilist)
	{
		ParamPathInfo *ppi = (ParamPathInfo *) lfirst(lc);

		if (bms_equal(ppi->ppi_req_outer, required_outer))
			return ppi;
	}

	return NULL;
}

/*
 * build_joinrel_partition_info
 *		Checks if the two relations being joined can use partitionwise join
 *		and if yes, initialize partitioning information of the resulting
 *		partitioned join relation.
 */
static void
build_joinrel_partition_info(RelOptInfo *joinrel, RelOptInfo *outer_rel,
							 RelOptInfo *inner_rel, List *restrictlist,
							 JoinType jointype)
{
	PartitionScheme part_scheme;

	/* Nothing to do if partitionwise join technique is disabled. */
	if (!enable_partitionwise_join)
	{
		Assert(!IS_PARTITIONED_REL(joinrel));
		return;
	}

	/*
	 * We can only consider this join as an input to further partitionwise
	 * joins if (a) the input relations are partitioned and have
	 * consider_partitionwise_join=true, (b) the partition schemes match, and
	 * (c) we can identify an equi-join between the partition keys.  Note that
	 * if it were possible for have_partkey_equi_join to return different
	 * answers for the same joinrel depending on which join ordering we try
	 * first, this logic would break.  That shouldn't happen, though, because
	 * of the way the query planner deduces implied equalities and reorders
	 * the joins.  Please see optimizer/README for details.
	 */
	if (outer_rel->part_scheme == NULL || inner_rel->part_scheme == NULL ||
		!outer_rel->consider_partitionwise_join ||
		!inner_rel->consider_partitionwise_join ||
		outer_rel->part_scheme != inner_rel->part_scheme ||
		!have_partkey_equi_join(joinrel, outer_rel, inner_rel,
								jointype, restrictlist))
	{
		Assert(!IS_PARTITIONED_REL(joinrel));
		return;
	}

	part_scheme = outer_rel->part_scheme;

	/*
	 * This function will be called only once for each joinrel, hence it
	 * should not have partitioning fields filled yet.
	 */
	Assert(!joinrel->part_scheme && !joinrel->partexprs &&
		   !joinrel->nullable_partexprs && !joinrel->part_rels &&
		   !joinrel->boundinfo);

	/*
	 * If the join relation is partitioned, it uses the same partitioning
	 * scheme as the joining relations.
	 *
	 * Note: we calculate the partition bounds, number of partitions, and
	 * child-join relations of the join relation in try_partitionwise_join().
	 */
	joinrel->part_scheme = part_scheme;
	set_joinrel_partition_key_exprs(joinrel, outer_rel, inner_rel, jointype);

	/*
	 * Set the consider_partitionwise_join flag.
	 */
	Assert(outer_rel->consider_partitionwise_join);
	Assert(inner_rel->consider_partitionwise_join);
	joinrel->consider_partitionwise_join = true;
}

/*
 * have_partkey_equi_join
 *
 * Returns true if there exist equi-join conditions involving pairs
 * of matching partition keys of the relations being joined for all
 * partition keys.
 */
static bool
have_partkey_equi_join(RelOptInfo *joinrel,
					   RelOptInfo *rel1, RelOptInfo *rel2,
					   JoinType jointype, List *restrictlist)
{
	PartitionScheme part_scheme = rel1->part_scheme;
	ListCell   *lc;
	int			cnt_pks;
	bool		pk_has_clause[PARTITION_MAX_KEYS];
	bool		strict_op;

	/*
	 * This function must only be called when the joined relations have same
	 * partitioning scheme.
	 */
	Assert(rel1->part_scheme == rel2->part_scheme);
	Assert(part_scheme);

	memset(pk_has_clause, 0, sizeof(pk_has_clause));
	foreach(lc, restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		OpExpr	   *opexpr;
		Expr	   *expr1;
		Expr	   *expr2;
		int			ipk1;
		int			ipk2;

		/* If processing an outer join, only use its own join clauses. */
		if (IS_OUTER_JOIN(jointype) &&
			RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
			continue;

		/* Skip clauses which can not be used for a join. */
		if (!rinfo->can_join)
			continue;

		/* Skip clauses which are not equality conditions. */
		if (!rinfo->mergeopfamilies && !OidIsValid(rinfo->hashjoinoperator))
			continue;

		/* Should be OK to assume it's an OpExpr. */
		opexpr = castNode(OpExpr, rinfo->clause);

		/* Match the operands to the relation. */
		if (bms_is_subset(rinfo->left_relids, rel1->relids) &&
			bms_is_subset(rinfo->right_relids, rel2->relids))
		{
			expr1 = linitial(opexpr->args);
			expr2 = lsecond(opexpr->args);
		}
		else if (bms_is_subset(rinfo->left_relids, rel2->relids) &&
				 bms_is_subset(rinfo->right_relids, rel1->relids))
		{
			expr1 = lsecond(opexpr->args);
			expr2 = linitial(opexpr->args);
		}
		else
			continue;

		/*
		 * Now we need to know whether the join operator is strict; see
		 * comments in pathnodes.h.
		 */
		strict_op = op_strict(opexpr->opno);

		/*
		 * Only clauses referencing the partition keys are useful for
		 * partitionwise join.
		 */
		ipk1 = match_expr_to_partition_keys(expr1, rel1, strict_op);
		if (ipk1 < 0)
			continue;
		ipk2 = match_expr_to_partition_keys(expr2, rel2, strict_op);
		if (ipk2 < 0)
			continue;

		/*
		 * If the clause refers to keys at different ordinal positions, it can
		 * not be used for partitionwise join.
		 */
		if (ipk1 != ipk2)
			continue;

		/*
		 * The clause allows partitionwise join only if it uses the same
		 * operator family as that specified by the partition key.
		 */
		if (rel1->part_scheme->strategy == PARTITION_STRATEGY_HASH)
		{
			if (!OidIsValid(rinfo->hashjoinoperator) ||
				!op_in_opfamily(rinfo->hashjoinoperator,
								part_scheme->partopfamily[ipk1]))
				continue;
		}
		else if (!list_member_oid(rinfo->mergeopfamilies,
								  part_scheme->partopfamily[ipk1]))
			continue;

		/* Mark the partition key as having an equi-join clause. */
		pk_has_clause[ipk1] = true;
	}

	/* Check whether every partition key has an equi-join condition. */
	for (cnt_pks = 0; cnt_pks < part_scheme->partnatts; cnt_pks++)
	{
		if (!pk_has_clause[cnt_pks])
			return false;
	}

	return true;
}

/*
 * match_expr_to_partition_keys
 *
 * Tries to match an expression to one of the nullable or non-nullable
 * partition keys of "rel".  Returns the matched key's ordinal position,
 * or -1 if the expression could not be matched to any of the keys.
 *
 * strict_op must be true if the expression will be compared with the
 * partition key using a strict operator.  This allows us to consider
 * nullable as well as nonnullable partition keys.
 */
static int
match_expr_to_partition_keys(Expr *expr, RelOptInfo *rel, bool strict_op)
{
	int			cnt;

	/* This function should be called only for partitioned relations. */
	Assert(rel->part_scheme);
	Assert(rel->partexprs);
	Assert(rel->nullable_partexprs);

	/* Remove any relabel decorations. */
	while (IsA(expr, RelabelType))
		expr = (Expr *) (castNode(RelabelType, expr))->arg;

	for (cnt = 0; cnt < rel->part_scheme->partnatts; cnt++)
	{
		ListCell   *lc;

		/* We can always match to the non-nullable partition keys. */
		foreach(lc, rel->partexprs[cnt])
		{
			if (equal(lfirst(lc), expr))
				return cnt;
		}

		if (!strict_op)
			continue;

		/*
		 * If it's a strict join operator then a NULL partition key on one
		 * side will not join to any partition key on the other side, and in
		 * particular such a row can't join to a row from a different
		 * partition on the other side.  So, it's okay to search the nullable
		 * partition keys as well.
		 */
		foreach(lc, rel->nullable_partexprs[cnt])
		{
			if (equal(lfirst(lc), expr))
				return cnt;
		}
	}

	return -1;
}

/*
 * set_joinrel_partition_key_exprs
 *		Initialize partition key expressions for a partitioned joinrel.
 */
static void
set_joinrel_partition_key_exprs(RelOptInfo *joinrel,
								RelOptInfo *outer_rel, RelOptInfo *inner_rel,
								JoinType jointype)
{
	PartitionScheme part_scheme = joinrel->part_scheme;
	int			partnatts = part_scheme->partnatts;

	joinrel->partexprs = (List **) palloc0(sizeof(List *) * partnatts);
	joinrel->nullable_partexprs =
		(List **) palloc0(sizeof(List *) * partnatts);

	/*
	 * The joinrel's partition expressions are the same as those of the input
	 * rels, but we must properly classify them as nullable or not in the
	 * joinrel's output.  (Also, we add some more partition expressions if
	 * it's a FULL JOIN.)
	 */
	for (int cnt = 0; cnt < partnatts; cnt++)
	{
		/* mark these const to enforce that we copy them properly */
		const List *outer_expr = outer_rel->partexprs[cnt];
		const List *outer_null_expr = outer_rel->nullable_partexprs[cnt];
		const List *inner_expr = inner_rel->partexprs[cnt];
		const List *inner_null_expr = inner_rel->nullable_partexprs[cnt];
		List	   *partexpr = NIL;
		List	   *nullable_partexpr = NIL;
		ListCell   *lc;

		switch (jointype)
		{
				/*
				 * A join relation resulting from an INNER join may be
				 * regarded as partitioned by either of the inner and outer
				 * relation keys.  For example, A INNER JOIN B ON A.a = B.b
				 * can be regarded as partitioned on either A.a or B.b.  So we
				 * add both keys to the joinrel's partexpr lists.  However,
				 * anything that was already nullable still has to be treated
				 * as nullable.
				 */
			case JOIN_INNER:
				partexpr = list_concat_copy(outer_expr, inner_expr);
				nullable_partexpr = list_concat_copy(outer_null_expr,
													 inner_null_expr);
				break;

				/*
				 * A join relation resulting from a SEMI or ANTI join may be
				 * regarded as partitioned by the outer relation keys.  The
				 * inner relation's keys are no longer interesting; since they
				 * aren't visible in the join output, nothing could join to
				 * them.
				 */
			case JOIN_SEMI:
			case JOIN_ANTI:
				partexpr = list_copy(outer_expr);
				nullable_partexpr = list_copy(outer_null_expr);
				break;

				/*
				 * A join relation resulting from a LEFT OUTER JOIN likewise
				 * may be regarded as partitioned on the (non-nullable) outer
				 * relation keys.  The inner (nullable) relation keys are okay
				 * as partition keys for further joins as long as they involve
				 * strict join operators.
				 */
			case JOIN_LEFT:
				partexpr = list_copy(outer_expr);
				nullable_partexpr = list_concat_copy(inner_expr,
													 outer_null_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												inner_null_expr);
				break;

				/*
				 * For FULL OUTER JOINs, both relations are nullable, so the
				 * resulting join relation may be regarded as partitioned on
				 * either of inner and outer relation keys, but only for joins
				 * that involve strict join operators.
				 */
			case JOIN_FULL:
				nullable_partexpr = list_concat_copy(outer_expr,
													 inner_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												outer_null_expr);
				nullable_partexpr = list_concat(nullable_partexpr,
												inner_null_expr);

				/*
				 * Also add CoalesceExprs corresponding to each possible
				 * full-join output variable (that is, left side coalesced to
				 * right side), so that we can match equijoin expressions
				 * using those variables.  We really only need these for
				 * columns merged by JOIN USING, and only with the pairs of
				 * input items that correspond to the data structures that
				 * parse analysis would build for such variables.  But it's
				 * hard to tell which those are, so just make all the pairs.
				 * Extra items in the nullable_partexprs list won't cause big
				 * problems.  (It's possible that such items will get matched
				 * to user-written COALESCEs, but it should still be valid to
				 * partition on those, since they're going to be either the
				 * partition column or NULL; it's the same argument as for
				 * partitionwise nesting of any outer join.)  We assume no
				 * type coercions are needed to make the coalesce expressions,
				 * since columns of different types won't have gotten
				 * classified as the same PartitionScheme.
				 */
				foreach(lc, list_concat_copy(outer_expr, outer_null_expr))
				{
					Node	   *larg = (Node *) lfirst(lc);
					ListCell   *lc2;

					foreach(lc2, list_concat_copy(inner_expr, inner_null_expr))
					{
						Node	   *rarg = (Node *) lfirst(lc2);
						CoalesceExpr *c = makeNode(CoalesceExpr);

						c->coalescetype = exprType(larg);
						c->coalescecollid = exprCollation(larg);
						c->args = list_make2(larg, rarg);
						c->location = -1;
						nullable_partexpr = lappend(nullable_partexpr, c);
					}
				}
				break;

			default:
				elog(ERROR, "unrecognized join type: %d", (int) jointype);
		}

		joinrel->partexprs[cnt] = partexpr;
		joinrel->nullable_partexprs[cnt] = nullable_partexpr;
	}
}

/*
 * build_child_join_reltarget
 *	  Set up a child-join relation's reltarget from a parent-join relation.
 */
static void
build_child_join_reltarget(PlannerInfo *root,
						   RelOptInfo *parentrel,
						   RelOptInfo *childrel,
						   int nappinfos,
						   AppendRelInfo **appinfos)
{
	/* Build the targetlist */
	childrel->reltarget->exprs = (List *)
		adjust_appendrel_attrs(root,
							   (Node *) parentrel->reltarget->exprs,
							   nappinfos, appinfos);

	/* Set the cost and width fields */
	childrel->reltarget->cost.startup = parentrel->reltarget->cost.startup;
	childrel->reltarget->cost.per_tuple = parentrel->reltarget->cost.per_tuple;
	childrel->reltarget->width = parentrel->reltarget->width;
}
