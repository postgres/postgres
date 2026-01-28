/*-------------------------------------------------------------------------
 *
 * relnode.c
 *	  Relation-node lookup/construction routines
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
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

#include "access/nbtree.h"
#include "catalog/pg_constraint.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parse_oper.h"
#include "parser/parse_relation.h"
#include "rewrite/rewriteManip.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/typcache.h"


typedef struct JoinHashEntry
{
	Relids		join_relids;	/* hash key --- MUST BE FIRST */
	RelOptInfo *join_rel;
} JoinHashEntry;

/* Hook for plugins to get control during joinrel setup */
joinrel_setup_hook_type joinrel_setup_hook = NULL;

static void build_joinrel_tlist(PlannerInfo *root, RelOptInfo *joinrel,
								RelOptInfo *input_rel,
								SpecialJoinInfo *sjinfo,
								List *pushed_down_joins,
								bool can_null);
static List *build_joinrel_restrictlist(PlannerInfo *root,
										RelOptInfo *joinrel,
										RelOptInfo *outer_rel,
										RelOptInfo *inner_rel,
										SpecialJoinInfo *sjinfo);
static void build_joinrel_joinlist(RelOptInfo *joinrel,
								   RelOptInfo *outer_rel,
								   RelOptInfo *inner_rel);
static List *subbuild_joinrel_restrictlist(PlannerInfo *root,
										   RelOptInfo *joinrel,
										   RelOptInfo *input_rel,
										   Relids both_input_relids,
										   List *new_restrictlist);
static List *subbuild_joinrel_joinlist(RelOptInfo *joinrel,
									   List *joininfo_list,
									   List *new_joininfo);
static void set_foreign_rel_properties(RelOptInfo *joinrel,
									   RelOptInfo *outer_rel, RelOptInfo *inner_rel);
static void add_join_rel(PlannerInfo *root, RelOptInfo *joinrel);
static void build_joinrel_partition_info(PlannerInfo *root,
										 RelOptInfo *joinrel,
										 RelOptInfo *outer_rel, RelOptInfo *inner_rel,
										 SpecialJoinInfo *sjinfo,
										 List *restrictlist);
static bool have_partkey_equi_join(PlannerInfo *root, RelOptInfo *joinrel,
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
static bool eager_aggregation_possible_for_relation(PlannerInfo *root,
													RelOptInfo *rel);
static bool init_grouping_targets(PlannerInfo *root, RelOptInfo *rel,
								  PathTarget *target, PathTarget *agg_input,
								  List **group_clauses, List **group_exprs);
static bool is_var_in_aggref_only(PlannerInfo *root, Var *var);
static bool is_var_needed_by_join(PlannerInfo *root, Var *var, RelOptInfo *rel);
static Index get_expression_sortgroupref(PlannerInfo *root, Expr *expr);


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
		palloc0_array(RelOptInfo *, size);

	/* simple_rte_array is an array equivalent of the rtable list */
	root->simple_rte_array = (RangeTblEntry **)
		palloc0_array(RangeTblEntry *, size);
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
		palloc0_array(AppendRelInfo *, size);

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

	root->simple_rel_array =
		repalloc0_array(root->simple_rel_array, RelOptInfo *, root->simple_rel_array_size, new_size);

	root->simple_rte_array =
		repalloc0_array(root->simple_rte_array, RangeTblEntry *, root->simple_rel_array_size, new_size);

	if (root->append_rel_array)
		root->append_rel_array =
			repalloc0_array(root->append_rel_array, AppendRelInfo *, root->simple_rel_array_size, new_size);
	else
		root->append_rel_array =
			palloc0_array(AppendRelInfo *, new_size);

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
	rel->pgs_mask = root->glob->default_pgs_mask;
	rel->reltarget = create_empty_pathtarget();
	rel->pathlist = NIL;
	rel->ppilist = NIL;
	rel->partial_pathlist = NIL;
	rel->cheapest_startup_path = NULL;
	rel->cheapest_total_path = NULL;
	rel->cheapest_parameterized_paths = NIL;
	rel->relid = relid;
	rel->rtekind = rte->rtekind;
	/* min_attr, max_attr, attr_needed, attr_widths are set below */
	rel->notnullattnums = NULL;
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
	if (rte->rtekind == RTE_RELATION)
	{
		Assert(parent == NULL ||
			   parent->rtekind == RTE_RELATION ||
			   parent->rtekind == RTE_SUBQUERY);

		/*
		 * For any RELATION rte, we need a userid with which to check
		 * permission access. Baserels simply use their own
		 * RTEPermissionInfo's checkAsUser.
		 *
		 * For otherrels normally there's no RTEPermissionInfo, so we use the
		 * parent's, which normally has one. The exceptional case is that the
		 * parent is a subquery, in which case the otherrel will have its own.
		 */
		if (rel->reloptkind == RELOPT_BASEREL ||
			(rel->reloptkind == RELOPT_OTHER_MEMBER_REL &&
			 parent->rtekind == RTE_SUBQUERY))
		{
			RTEPermissionInfo *perminfo;

			perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
			rel->userid = perminfo->checkAsUser;
		}
		else
			rel->userid = parent->userid;
	}
	else
		rel->userid = InvalidOid;
	rel->useridiscurrent = false;
	rel->fdwroutine = NULL;
	rel->fdw_private = NULL;
	rel->unique_for_rels = NIL;
	rel->non_unique_for_rels = NIL;
	rel->unique_rel = NULL;
	rel->unique_pathkeys = NIL;
	rel->unique_groupclause = NIL;
	rel->baserestrictinfo = NIL;
	rel->baserestrictcost.startup = 0;
	rel->baserestrictcost.per_tuple = 0;
	rel->baserestrict_min_security = UINT_MAX;
	rel->joininfo = NIL;
	rel->has_eclass_joins = false;
	rel->consider_partitionwise_join = false;	/* might get changed later */
	rel->agg_info = NULL;
	rel->grouped_rel = NULL;
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
		/* We keep back-links to immediate parent and topmost parent. */
		rel->parent = parent;
		rel->top_parent = parent->top_parent ? parent->top_parent : parent;
		rel->top_parent_relids = rel->top_parent->relids;

		/*
		 * A child rel is below the same outer joins as its parent.  (We
		 * presume this info was already calculated for the parent.)
		 */
		rel->nulling_relids = parent->nulling_relids;

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
		rel->parent = NULL;
		rel->top_parent = NULL;
		rel->top_parent_relids = NULL;
		rel->nulling_relids = NULL;
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
				palloc0_array(Relids, rel->max_attr - rel->min_attr + 1);
			rel->attr_widths = (int32 *)
				palloc0_array(int32, rel->max_attr - rel->min_attr + 1);
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
	 * We must apply the partially filled in RelOptInfo before calling
	 * apply_child_basequals due to some transformations within that function
	 * which require the RelOptInfo to be available in the simple_rel_array.
	 */
	root->simple_rel_array[relid] = rel;

	/*
	 * Apply the parent's quals to the child, with appropriate substitution of
	 * variables.  If the resulting clause is constant-FALSE or NULL after
	 * applying transformations, apply_child_basequals returns false to
	 * indicate that scanning this relation won't yield any rows.  In this
	 * case, we mark the child as dummy right away.  (We must do this
	 * immediately so that pruning works correctly when recursing in
	 * expand_partitioned_rtentry.)
	 */
	if (parent)
	{
		AppendRelInfo *appinfo = root->append_rel_array[relid];

		Assert(appinfo != NULL);
		if (!apply_child_basequals(root, parent, rel, rte, appinfo))
		{
			/*
			 * Restriction clause reduced to constant FALSE or NULL.  Mark as
			 * dummy so we won't scan this relation.
			 */
			mark_dummy_rel(rel);
		}
	}

	return rel;
}

/*
 * build_simple_grouped_rel
 *	  Construct a new RelOptInfo representing a grouped version of the input
 *	  simple relation.
 */
RelOptInfo *
build_simple_grouped_rel(PlannerInfo *root, RelOptInfo *rel)
{
	RelOptInfo *grouped_rel;
	RelAggInfo *agg_info;

	/*
	 * We should have available aggregate expressions and grouping
	 * expressions, otherwise we cannot reach here.
	 */
	Assert(root->agg_clause_list != NIL);
	Assert(root->group_expr_list != NIL);

	/* nothing to do for dummy rel */
	if (IS_DUMMY_REL(rel))
		return NULL;

	/*
	 * Prepare the information needed to create grouped paths for this simple
	 * relation.
	 */
	agg_info = create_rel_agg_info(root, rel, true);
	if (agg_info == NULL)
		return NULL;

	/*
	 * If grouped paths for the given simple relation are not considered
	 * useful, skip building the grouped relation.
	 */
	if (!agg_info->agg_useful)
		return NULL;

	/* Track the set of relids at which partial aggregation is applied */
	agg_info->apply_agg_at = bms_copy(rel->relids);

	/* build the grouped relation */
	grouped_rel = build_grouped_rel(root, rel);
	grouped_rel->reltarget = agg_info->target;
	grouped_rel->rows = agg_info->grouped_rows;
	grouped_rel->agg_info = agg_info;

	rel->grouped_rel = grouped_rel;

	return grouped_rel;
}

/*
 * build_grouped_rel
 *	  Build a grouped relation by flat copying the input relation and resetting
 *	  the necessary fields.
 */
RelOptInfo *
build_grouped_rel(PlannerInfo *root, RelOptInfo *rel)
{
	RelOptInfo *grouped_rel;

	grouped_rel = makeNode(RelOptInfo);
	memcpy(grouped_rel, rel, sizeof(RelOptInfo));

	/*
	 * clear path info
	 */
	grouped_rel->pathlist = NIL;
	grouped_rel->ppilist = NIL;
	grouped_rel->partial_pathlist = NIL;
	grouped_rel->cheapest_startup_path = NULL;
	grouped_rel->cheapest_total_path = NULL;
	grouped_rel->cheapest_parameterized_paths = NIL;

	/*
	 * clear partition info
	 */
	grouped_rel->part_scheme = NULL;
	grouped_rel->nparts = -1;
	grouped_rel->boundinfo = NULL;
	grouped_rel->partbounds_merged = false;
	grouped_rel->partition_qual = NIL;
	grouped_rel->part_rels = NULL;
	grouped_rel->live_parts = NULL;
	grouped_rel->all_partrels = NULL;
	grouped_rel->partexprs = NULL;
	grouped_rel->nullable_partexprs = NULL;
	grouped_rel->consider_partitionwise_join = false;

	/*
	 * clear size estimates
	 */
	grouped_rel->rows = 0;

	return grouped_rel;
}

/*
 * find_base_rel
 *	  Find a base or otherrel relation entry, which must already exist.
 */
RelOptInfo *
find_base_rel(PlannerInfo *root, int relid)
{
	RelOptInfo *rel;

	/* use an unsigned comparison to prevent negative array element access */
	if ((uint32) relid < (uint32) root->simple_rel_array_size)
	{
		rel = root->simple_rel_array[relid];
		if (rel)
			return rel;
	}

	elog(ERROR, "no relation entry for relid %d", relid);

	return NULL;				/* keep compiler quiet */
}

/*
 * find_base_rel_noerr
 *	  Find a base or otherrel relation entry, returning NULL if there's none
 */
RelOptInfo *
find_base_rel_noerr(PlannerInfo *root, int relid)
{
	/* use an unsigned comparison to prevent negative array element access */
	if ((uint32) relid < (uint32) root->simple_rel_array_size)
		return root->simple_rel_array[relid];
	return NULL;
}

/*
 * find_base_rel_ignore_join
 *	  Find a base or otherrel relation entry, which must already exist.
 *
 * Unlike find_base_rel, if relid references an outer join then this
 * will return NULL rather than raising an error.  This is convenient
 * for callers that must deal with relid sets including both base and
 * outer joins.
 */
RelOptInfo *
find_base_rel_ignore_join(PlannerInfo *root, int relid)
{
	/* use an unsigned comparison to prevent negative array element access */
	if ((uint32) relid < (uint32) root->simple_rel_array_size)
	{
		RelOptInfo *rel;
		RangeTblEntry *rte;

		rel = root->simple_rel_array[relid];
		if (rel)
			return rel;

		/*
		 * We could just return NULL here, but for debugging purposes it seems
		 * best to actually verify that the relid is an outer join and not
		 * something weird.
		 */
		rte = root->simple_rte_array[relid];
		if (rte && rte->rtekind == RTE_JOIN && rte->jointype != JOIN_INNER)
			return NULL;
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
 * 'pushed_down_joins': any pushed-down outer joins that are now completed
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
			   List *pushed_down_joins,
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
														   inner_rel,
														   sjinfo);
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
	joinrel->pgs_mask = root->glob->default_pgs_mask;
	joinrel->reltarget = create_empty_pathtarget();
	joinrel->pathlist = NIL;
	joinrel->ppilist = NIL;
	joinrel->partial_pathlist = NIL;
	joinrel->cheapest_startup_path = NULL;
	joinrel->cheapest_total_path = NULL;
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
	joinrel->notnullattnums = NULL;
	joinrel->nulling_relids = NULL;
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
	joinrel->unique_rel = NULL;
	joinrel->unique_pathkeys = NIL;
	joinrel->unique_groupclause = NIL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->baserestrict_min_security = UINT_MAX;
	joinrel->joininfo = NIL;
	joinrel->has_eclass_joins = false;
	joinrel->consider_partitionwise_join = false;	/* might get changed later */
	joinrel->agg_info = NULL;
	joinrel->grouped_rel = NULL;
	joinrel->parent = NULL;
	joinrel->top_parent = NULL;
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
	 * Fill the joinrel's tlist with just the Vars and PHVs that need to be
	 * output from this join (ie, are needed for higher joinclauses or final
	 * output).
	 *
	 * NOTE: the tlist order for a join rel will depend on which pair of outer
	 * and inner rels we first try to build it from.  But the contents should
	 * be the same regardless.
	 */
	build_joinrel_tlist(root, joinrel, outer_rel, sjinfo, pushed_down_joins,
						(sjinfo->jointype == JOIN_FULL));
	build_joinrel_tlist(root, joinrel, inner_rel, sjinfo, pushed_down_joins,
						(sjinfo->jointype != JOIN_INNER));
	add_placeholders_to_joinrel(root, joinrel, outer_rel, inner_rel, sjinfo);

	/*
	 * add_placeholders_to_joinrel also took care of adding the ph_lateral
	 * sets of any PlaceHolderVars computed here to direct_lateral_relids, so
	 * now we can finish computing that.  This is much like the computation of
	 * the transitively-closed lateral_relids in min_join_parameterization,
	 * except that here we *do* have to consider the added PHVs.
	 */
	joinrel->direct_lateral_relids =
		bms_del_members(joinrel->direct_lateral_relids, joinrel->relids);

	/*
	 * Construct restrict and join clause lists for the new joinrel. (The
	 * caller might or might not need the restrictlist, but I need it anyway
	 * for set_joinrel_size_estimates().)
	 */
	restrictlist = build_joinrel_restrictlist(root, joinrel,
											  outer_rel, inner_rel,
											  sjinfo);
	if (restrictlist_ptr)
		*restrictlist_ptr = restrictlist;
	build_joinrel_joinlist(joinrel, outer_rel, inner_rel);

	/*
	 * This is also the right place to check whether the joinrel has any
	 * pending EquivalenceClass joins.
	 */
	joinrel->has_eclass_joins = has_relevant_eclass_joinclause(root, joinrel);

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

	/*
	 * Allow a plugin to editorialize on the new joinrel's properties. Actions
	 * might include altering the size estimate, clearing consider_parallel,
	 * or adjusting pgs_mask.
	 */
	if (joinrel_setup_hook)
		(*joinrel_setup_hook) (root, joinrel, outer_rel, inner_rel, sjinfo,
							   restrictlist);

	/* Store the partition information. */
	build_joinrel_partition_info(root, joinrel, outer_rel, inner_rel, sjinfo,
								 restrictlist);

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
 * 'restrictlist': list of RestrictInfo nodes that apply to this particular
 *		pair of joinable relations
 * 'sjinfo': child join's join-type details
 * 'nappinfos' and 'appinfos': AppendRelInfo array for child relids
 */
RelOptInfo *
build_child_join_rel(PlannerInfo *root, RelOptInfo *outer_rel,
					 RelOptInfo *inner_rel, RelOptInfo *parent_joinrel,
					 List *restrictlist, SpecialJoinInfo *sjinfo,
					 int nappinfos, AppendRelInfo **appinfos)
{
	RelOptInfo *joinrel = makeNode(RelOptInfo);

	/* Only joins between "other" relations land here. */
	Assert(IS_OTHER_REL(outer_rel) && IS_OTHER_REL(inner_rel));

	/* The parent joinrel should have consider_partitionwise_join set. */
	Assert(parent_joinrel->consider_partitionwise_join);

	joinrel->reloptkind = RELOPT_OTHER_JOINREL;
	joinrel->relids = adjust_child_relids(parent_joinrel->relids,
										  nappinfos, appinfos);
	joinrel->rows = 0;
	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	joinrel->consider_startup = (root->tuple_fraction > 0);
	joinrel->consider_param_startup = false;
	joinrel->consider_parallel = false;
	joinrel->pgs_mask = root->glob->default_pgs_mask;
	joinrel->reltarget = create_empty_pathtarget();
	joinrel->pathlist = NIL;
	joinrel->ppilist = NIL;
	joinrel->partial_pathlist = NIL;
	joinrel->cheapest_startup_path = NULL;
	joinrel->cheapest_total_path = NULL;
	joinrel->cheapest_parameterized_paths = NIL;
	joinrel->direct_lateral_relids = NULL;
	joinrel->lateral_relids = NULL;
	joinrel->relid = 0;			/* indicates not a baserel */
	joinrel->rtekind = RTE_JOIN;
	joinrel->min_attr = 0;
	joinrel->max_attr = 0;
	joinrel->attr_needed = NULL;
	joinrel->attr_widths = NULL;
	joinrel->notnullattnums = NULL;
	joinrel->nulling_relids = NULL;
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
	joinrel->unique_rel = NULL;
	joinrel->unique_pathkeys = NIL;
	joinrel->unique_groupclause = NIL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->joininfo = NIL;
	joinrel->has_eclass_joins = false;
	joinrel->consider_partitionwise_join = false;	/* might get changed later */
	joinrel->agg_info = NULL;
	joinrel->grouped_rel = NULL;
	joinrel->parent = parent_joinrel;
	joinrel->top_parent = parent_joinrel->top_parent ? parent_joinrel->top_parent : parent_joinrel;
	joinrel->top_parent_relids = joinrel->top_parent->relids;
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

	/* Compute information relevant to foreign relations. */
	set_foreign_rel_properties(joinrel, outer_rel, inner_rel);

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

	/* Child joinrel is parallel safe if parent is parallel safe. */
	joinrel->consider_parallel = parent_joinrel->consider_parallel;

	/* Set estimates of the child-joinrel's size. */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
							   sjinfo, restrictlist);

	/*
	 * Allow a plugin to editorialize on the new joinrel's properties. Actions
	 * might include altering the size estimate, clearing consider_parallel,
	 * or adjusting pgs_mask. (However, note that clearing consider_parallel
	 * would be better done in the parent joinrel rather than here.)
	 */
	if (joinrel_setup_hook)
		(*joinrel_setup_hook) (root, joinrel, outer_rel, inner_rel, sjinfo,
							   restrictlist);

	/* Is the join between partitions itself partitioned? */
	build_joinrel_partition_info(root, joinrel, outer_rel, inner_rel, sjinfo,
								 restrictlist);

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
 * Likewise for any PlaceHolderVars emitted by the input rel.
 *
 * We also compute the expected width of the join's output, making use
 * of data that was cached at the baserel level by set_rel_width().
 *
 * Pass can_null as true if the join is an outer join that can null Vars
 * from this input relation.  If so, we will (normally) add the join's relid
 * to the nulling bitmaps of Vars and PHVs bubbled up from the input.
 *
 * When forming an outer join's target list, special handling is needed in
 * case the outer join was commuted with another one per outer join identity 3
 * (see optimizer/README).  We must take steps to ensure that the output Vars
 * have the same nulling bitmaps that they would if the two joins had been
 * done in syntactic order; else they won't match Vars appearing higher in
 * the query tree.  An exception to the match-the-syntactic-order rule is
 * that when an outer join is pushed down into another one's RHS per identity
 * 3, we can't mark its Vars as nulled until the now-upper outer join is also
 * completed.  So we need to do three things:
 *
 * First, we add the outer join's relid to the nulling bitmap only if the
 * outer join has been completely performed and the Var or PHV actually
 * comes from within the syntactically nullable side(s) of the outer join.
 * This takes care of the possibility that we have transformed
 *		(A leftjoin B on (Pab)) leftjoin C on (Pbc)
 * to
 *		A leftjoin (B leftjoin C on (Pbc)) on (Pab)
 * Here the pushed-down B/C join cannot mark C columns as nulled yet,
 * while the now-upper A/B join must not mark C columns as nulled by itself.
 *
 * Second, perform the same operation for each SpecialJoinInfo listed in
 * pushed_down_joins (which, in this example, would be the B/C join when
 * we are at the now-upper A/B join).  This allows the now-upper join to
 * complete the marking of "C" Vars that now have fully valid values.
 *
 * Third, any relid in sjinfo->commute_above_r that is already part of
 * the joinrel is added to the nulling bitmaps of nullable Vars and PHVs.
 * This takes care of the reverse case where we implement
 *		A leftjoin (B leftjoin C on (Pbc)) on (Pab)
 * as
 *		(A leftjoin B on (Pab)) leftjoin C on (Pbc)
 * The C columns emitted by the B/C join need to be shown as nulled by both
 * the B/C and A/B joins, even though they've not physically traversed the
 * A/B join.
 */
static void
build_joinrel_tlist(PlannerInfo *root, RelOptInfo *joinrel,
					RelOptInfo *input_rel,
					SpecialJoinInfo *sjinfo,
					List *pushed_down_joins,
					bool can_null)
{
	Relids		relids = joinrel->relids;
	int64		tuple_width = joinrel->reltarget->width;
	ListCell   *vars;
	ListCell   *lc;

	foreach(vars, input_rel->reltarget->exprs)
	{
		Var		   *var = (Var *) lfirst(vars);

		/*
		 * For a PlaceHolderVar, we have to look up the PlaceHolderInfo.
		 */
		if (IsA(var, PlaceHolderVar))
		{
			PlaceHolderVar *phv = (PlaceHolderVar *) var;
			PlaceHolderInfo *phinfo = find_placeholder_info(root, phv);

			/* Is it still needed above this joinrel? */
			if (bms_nonempty_difference(phinfo->ph_needed, relids))
			{
				/*
				 * Yup, add it to the output.  If this join potentially nulls
				 * this input, we have to update the PHV's phnullingrels,
				 * which means making a copy.
				 */
				if (can_null)
				{
					phv = copyObject(phv);
					/* See comments above to understand this logic */
					if (sjinfo->ojrelid != 0 &&
						bms_is_member(sjinfo->ojrelid, relids) &&
						(bms_is_subset(phv->phrels, sjinfo->syn_righthand) ||
						 (sjinfo->jointype == JOIN_FULL &&
						  bms_is_subset(phv->phrels, sjinfo->syn_lefthand))))
						phv->phnullingrels = bms_add_member(phv->phnullingrels,
															sjinfo->ojrelid);
					foreach(lc, pushed_down_joins)
					{
						SpecialJoinInfo *othersj = (SpecialJoinInfo *) lfirst(lc);

						Assert(bms_is_member(othersj->ojrelid, relids));
						if (bms_is_subset(phv->phrels, othersj->syn_righthand))
							phv->phnullingrels = bms_add_member(phv->phnullingrels,
																othersj->ojrelid);
					}
					phv->phnullingrels =
						bms_join(phv->phnullingrels,
								 bms_intersect(sjinfo->commute_above_r,
											   relids));
				}

				joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs,
													phv);
				/* Bubbling up the precomputed result has cost zero */
				tuple_width += phinfo->ph_width;
			}
			continue;
		}

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
			/* UPDATE/DELETE/MERGE row identity vars are always needed */
			RowIdentityVarInfo *ridinfo = (RowIdentityVarInfo *)
				list_nth(root->row_identity_vars, var->varattno - 1);

			/* Update reltarget width estimate from RowIdentityVarInfo */
			tuple_width += ridinfo->rowidwidth;
		}
		else
		{
			RelOptInfo *baserel;
			int			ndx;

			/* Get the Var's original base rel */
			baserel = find_base_rel(root, var->varno);

			/* Is it still needed above this joinrel? */
			ndx = var->varattno - baserel->min_attr;
			if (!bms_nonempty_difference(baserel->attr_needed[ndx], relids))
				continue;		/* nope, skip it */

			/* Update reltarget width estimate from baserel's attr_widths */
			tuple_width += baserel->attr_widths[ndx];
		}

		/*
		 * Add the Var to the output.  If this join potentially nulls this
		 * input, we have to update the Var's varnullingrels, which means
		 * making a copy.  But note that we don't ever add nullingrel bits to
		 * row identity Vars (cf. comments in setrefs.c).
		 */
		if (can_null && var->varno != ROWID_VAR)
		{
			var = copyObject(var);
			/* See comments above to understand this logic */
			if (sjinfo->ojrelid != 0 &&
				bms_is_member(sjinfo->ojrelid, relids) &&
				(bms_is_member(var->varno, sjinfo->syn_righthand) ||
				 (sjinfo->jointype == JOIN_FULL &&
				  bms_is_member(var->varno, sjinfo->syn_lefthand))))
				var->varnullingrels = bms_add_member(var->varnullingrels,
													 sjinfo->ojrelid);
			foreach(lc, pushed_down_joins)
			{
				SpecialJoinInfo *othersj = (SpecialJoinInfo *) lfirst(lc);

				Assert(bms_is_member(othersj->ojrelid, relids));
				if (bms_is_member(var->varno, othersj->syn_righthand))
					var->varnullingrels = bms_add_member(var->varnullingrels,
														 othersj->ojrelid);
			}
			var->varnullingrels =
				bms_join(var->varnullingrels,
						 bms_intersect(sjinfo->commute_above_r,
									   relids));
		}

		joinrel->reltarget->exprs = lappend(joinrel->reltarget->exprs,
											var);

		/* Vars have cost zero, so no need to adjust reltarget->cost */
	}

	joinrel->reltarget->width = clamp_width_est(tuple_width);
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
 *	  If a join clause from an input relation refers to base+OJ rels still not
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
 * 'sjinfo': join context info
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
						   RelOptInfo *inner_rel,
						   SpecialJoinInfo *sjinfo)
{
	List	   *result;
	Relids		both_input_relids;

	both_input_relids = bms_union(outer_rel->relids, inner_rel->relids);

	/*
	 * Collect all the clauses that syntactically belong at this level,
	 * eliminating any duplicates (important since we will see many of the
	 * same clauses arriving from both input relations).
	 */
	result = subbuild_joinrel_restrictlist(root, joinrel, outer_rel,
										   both_input_relids, NIL);
	result = subbuild_joinrel_restrictlist(root, joinrel, inner_rel,
										   both_input_relids, result);

	/*
	 * Add on any clauses derived from EquivalenceClasses.  These cannot be
	 * redundant with the clauses in the joininfo lists, so don't bother
	 * checking.
	 */
	result = list_concat(result,
						 generate_join_implied_equalities(root,
														  joinrel->relids,
														  outer_rel->relids,
														  inner_rel,
														  sjinfo));

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
subbuild_joinrel_restrictlist(PlannerInfo *root,
							  RelOptInfo *joinrel,
							  RelOptInfo *input_rel,
							  Relids both_input_relids,
							  List *new_restrictlist)
{
	ListCell   *l;

	foreach(l, input_rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		if (bms_is_subset(rinfo->required_relids, joinrel->relids))
		{
			/*
			 * This clause should become a restriction clause for the joinrel,
			 * since it refers to no outside rels.  However, if it's a clone
			 * clause then it might be too late to evaluate it, so we have to
			 * check.  (If it is too late, just ignore the clause, taking it
			 * on faith that another clone was or will be selected.)  Clone
			 * clauses should always be outer-join clauses, so we compare
			 * against both_input_relids.
			 */
			if (rinfo->has_clone || rinfo->is_clone)
			{
				Assert(!RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids));
				if (!bms_is_subset(rinfo->required_relids, both_input_relids))
					continue;
				if (bms_overlap(rinfo->incompatible_relids, both_input_relids))
					continue;
			}
			else
			{
				/*
				 * For non-clone clauses, we just Assert it's OK.  These might
				 * be either join or filter clauses; if it's a join clause
				 * then it should not refer to the current join's output.
				 * (There is little point in checking incompatible_relids,
				 * because it'll be NULL.)
				 */
				Assert(RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids) ||
					   bms_is_subset(rinfo->required_relids,
									 both_input_relids));
			}

			/*
			 * OK, so add it to the list, being careful to eliminate
			 * duplicates.  (Since RestrictInfo nodes in different joinlists
			 * will have been multiply-linked rather than copied, pointer
			 * equality should be a sufficient test.)
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
	upperrel->pgs_mask = root->glob->default_pgs_mask;

	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	upperrel->consider_startup = (root->tuple_fraction > 0);
	upperrel->consider_param_startup = false;
	upperrel->consider_parallel = false;	/* might get changed later */
	upperrel->reltarget = create_empty_pathtarget();
	upperrel->pathlist = NIL;
	upperrel->cheapest_startup_path = NULL;
	upperrel->cheapest_total_path = NULL;
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
	List	   *eqclauses;
	Bitmapset  *pserials;
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
	 * necessarily satisfy join_clause_is_movable_into; but in assert-enabled
	 * builds, let's verify that.)
	 */
	eqclauses = generate_join_implied_equalities(root,
												 joinrelids,
												 required_outer,
												 baserel,
												 NULL);
#ifdef USE_ASSERT_CHECKING
	foreach(lc, eqclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(join_clause_is_movable_into(rinfo,
										   baserel->relids,
										   joinrelids));
	}
#endif
	pclauses = list_concat(pclauses, eqclauses);

	/* Compute set of serial numbers of the enforced clauses */
	pserials = NULL;
	foreach(lc, pclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		pserials = bms_add_member(pserials, rinfo->rinfo_serial);
	}

	/* Estimate the number of rows returned by the parameterized scan */
	rows = get_parameterized_baserel_size(root, baserel, pclauses);

	/* And now we can build the ParamPathInfo */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = rows;
	ppi->ppi_clauses = pclauses;
	ppi->ppi_serials = pserials;
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
												joinrel,
												NULL);
	/* We only want ones that aren't movable to lower levels */
	dropped_ecs = NIL;
	foreach(lc, eclauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(join_clause_is_movable_into(rinfo,
										   joinrel->relids,
										   join_and_req));
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

			Assert(join_clause_is_movable_into(rinfo,
											   outer_path->parent->relids,
											   real_outer_and_req));
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
	ppi->ppi_serials = NULL;
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
	ppi->ppi_serials = NULL;
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
 * get_param_path_clause_serials
 *		Given a parameterized Path, return the set of pushed-down clauses
 *		(identified by rinfo_serial numbers) enforced within the Path.
 */
Bitmapset *
get_param_path_clause_serials(Path *path)
{
	if (path->param_info == NULL)
		return NULL;			/* not parameterized */

	/*
	 * We don't currently support parameterized MergeAppend paths, as
	 * explained in the comments for generate_orderedappend_paths.
	 */
	Assert(!IsA(path, MergeAppendPath));

	if (IsA(path, NestPath) ||
		IsA(path, MergePath) ||
		IsA(path, HashPath))
	{
		/*
		 * For a join path, combine clauses enforced within either input path
		 * with those enforced as joinrestrictinfo in this path.  Note that
		 * joinrestrictinfo may include some non-pushed-down clauses, but for
		 * current purposes it's okay if we include those in the result. (To
		 * be more careful, we could check for clause_relids overlapping the
		 * path parameterization, but it's not worth the cycles for now.)
		 */
		JoinPath   *jpath = (JoinPath *) path;
		Bitmapset  *pserials;
		ListCell   *lc;

		pserials = NULL;
		pserials = bms_add_members(pserials,
								   get_param_path_clause_serials(jpath->outerjoinpath));
		pserials = bms_add_members(pserials,
								   get_param_path_clause_serials(jpath->innerjoinpath));
		foreach(lc, jpath->joinrestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			pserials = bms_add_member(pserials, rinfo->rinfo_serial);
		}
		return pserials;
	}
	else if (IsA(path, AppendPath))
	{
		/*
		 * For an appendrel, take the intersection of the sets of clauses
		 * enforced in each input path.
		 */
		AppendPath *apath = (AppendPath *) path;
		Bitmapset  *pserials;
		ListCell   *lc;

		pserials = NULL;
		foreach(lc, apath->subpaths)
		{
			Path	   *subpath = (Path *) lfirst(lc);
			Bitmapset  *subserials;

			subserials = get_param_path_clause_serials(subpath);
			if (lc == list_head(apath->subpaths))
				pserials = bms_copy(subserials);
			else
				pserials = bms_int_members(pserials, subserials);
		}
		return pserials;
	}
	else
	{
		/*
		 * Otherwise, it's a baserel path and we can use the
		 * previously-computed set of serial numbers.
		 */
		return path->param_info->ppi_serials;
	}
}

/*
 * build_joinrel_partition_info
 *		Checks if the two relations being joined can use partitionwise join
 *		and if yes, initialize partitioning information of the resulting
 *		partitioned join relation.
 */
static void
build_joinrel_partition_info(PlannerInfo *root,
							 RelOptInfo *joinrel, RelOptInfo *outer_rel,
							 RelOptInfo *inner_rel, SpecialJoinInfo *sjinfo,
							 List *restrictlist)
{
	PartitionScheme part_scheme;

	/* Nothing to do if partitionwise join technique is disabled. */
	if ((joinrel->pgs_mask & PGS_CONSIDER_PARTITIONWISE) == 0)
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
		!have_partkey_equi_join(root, joinrel, outer_rel, inner_rel,
								sjinfo->jointype, restrictlist))
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
	set_joinrel_partition_key_exprs(joinrel, outer_rel, inner_rel,
									sjinfo->jointype);

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
have_partkey_equi_join(PlannerInfo *root, RelOptInfo *joinrel,
					   RelOptInfo *rel1, RelOptInfo *rel2,
					   JoinType jointype, List *restrictlist)
{
	PartitionScheme part_scheme = rel1->part_scheme;
	bool		pk_known_equal[PARTITION_MAX_KEYS];
	int			num_equal_pks;
	ListCell   *lc;

	/*
	 * This function must only be called when the joined relations have same
	 * partitioning scheme.
	 */
	Assert(rel1->part_scheme == rel2->part_scheme);
	Assert(part_scheme);

	/* We use a bool array to track which partkey columns are known equal */
	memset(pk_known_equal, 0, sizeof(pk_known_equal));
	/* ... as well as a count of how many are known equal */
	num_equal_pks = 0;

	/* First, look through the join's restriction clauses */
	foreach(lc, restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		OpExpr	   *opexpr;
		Expr	   *expr1;
		Expr	   *expr2;
		bool		strict_op;
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
		 * Vars appearing in the relation's partition keys will not have any
		 * varnullingrels, but those in expr1 and expr2 will if we're above
		 * outer joins that could null the respective rels.  It's okay to
		 * match anyway, if the join operator is strict.
		 */
		if (strict_op)
		{
			if (bms_overlap(rel1->relids, root->outer_join_rels))
				expr1 = (Expr *) remove_nulling_relids((Node *) expr1,
													   root->outer_join_rels,
													   NULL);
			if (bms_overlap(rel2->relids, root->outer_join_rels))
				expr2 = (Expr *) remove_nulling_relids((Node *) expr2,
													   root->outer_join_rels,
													   NULL);
		}

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

		/* Ignore clause if we already proved these keys equal. */
		if (pk_known_equal[ipk1])
			continue;

		/* Reject if the partition key collation differs from the clause's. */
		if (rel1->part_scheme->partcollation[ipk1] != opexpr->inputcollid)
			return false;

		/*
		 * The clause allows partitionwise join only if it uses the same
		 * operator family as that specified by the partition key.
		 */
		if (part_scheme->strategy == PARTITION_STRATEGY_HASH)
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
		pk_known_equal[ipk1] = true;

		/* We can stop examining clauses once we prove all keys equal. */
		if (++num_equal_pks == part_scheme->partnatts)
			return true;
	}

	/*
	 * Also check to see if any keys are known equal by equivclass.c.  In most
	 * cases there would have been a join restriction clause generated from
	 * any EC that had such knowledge, but there might be no such clause, or
	 * it might happen to constrain other members of the ECs than the ones we
	 * are looking for.
	 */
	for (int ipk = 0; ipk < part_scheme->partnatts; ipk++)
	{
		Oid			btree_opfamily;

		/* Ignore if we already proved these keys equal. */
		if (pk_known_equal[ipk])
			continue;

		/*
		 * We need a btree opfamily to ask equivclass.c about.  If the
		 * partopfamily is a hash opfamily, look up its equality operator, and
		 * select some btree opfamily that that operator is part of.  (Any
		 * such opfamily should be good enough, since equivclass.c will track
		 * multiple opfamilies as appropriate.)
		 */
		if (part_scheme->strategy == PARTITION_STRATEGY_HASH)
		{
			Oid			eq_op;
			List	   *eq_opfamilies;

			eq_op = get_opfamily_member(part_scheme->partopfamily[ipk],
										part_scheme->partopcintype[ipk],
										part_scheme->partopcintype[ipk],
										HTEqualStrategyNumber);
			if (!OidIsValid(eq_op))
				break;			/* we're not going to succeed */
			eq_opfamilies = get_mergejoin_opfamilies(eq_op);
			if (eq_opfamilies == NIL)
				break;			/* we're not going to succeed */
			btree_opfamily = linitial_oid(eq_opfamilies);
		}
		else
			btree_opfamily = part_scheme->partopfamily[ipk];

		/*
		 * We consider only non-nullable partition keys here; nullable ones
		 * would not be treated as part of the same equivalence classes as
		 * non-nullable ones.
		 */
		foreach(lc, rel1->partexprs[ipk])
		{
			Node	   *expr1 = (Node *) lfirst(lc);
			ListCell   *lc2;
			Oid			partcoll1 = rel1->part_scheme->partcollation[ipk];
			Oid			exprcoll1 = exprCollation(expr1);

			foreach(lc2, rel2->partexprs[ipk])
			{
				Node	   *expr2 = (Node *) lfirst(lc2);

				if (exprs_known_equal(root, expr1, expr2, btree_opfamily))
				{
					/*
					 * Ensure that the collation of the expression matches
					 * that of the partition key. Checking just one collation
					 * (partcoll1 and exprcoll1) suffices because partcoll1
					 * and partcoll2, as well as exprcoll1 and exprcoll2,
					 * should be identical. This holds because both rel1 and
					 * rel2 use the same PartitionScheme and expr1 and expr2
					 * are equal.
					 */
					if (partcoll1 == exprcoll1)
					{
						Oid			partcoll2 PG_USED_FOR_ASSERTS_ONLY =
							rel2->part_scheme->partcollation[ipk];
						Oid			exprcoll2 PG_USED_FOR_ASSERTS_ONLY =
							exprCollation(expr2);

						Assert(partcoll2 == exprcoll2);
						pk_known_equal[ipk] = true;
						break;
					}
				}
			}
			if (pk_known_equal[ipk])
				break;
		}

		if (pk_known_equal[ipk])
		{
			/* We can stop examining keys once we prove all keys equal. */
			if (++num_equal_pks == part_scheme->partnatts)
				return true;
		}
		else
			break;				/* no chance to succeed, give up */
	}

	return false;
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

	joinrel->partexprs = palloc0_array(List *, partnatts);
	joinrel->nullable_partexprs = palloc0_array(List *, partnatts);

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
				 * classified as the same PartitionScheme.  Note that we
				 * intentionally leave out the varnullingrels decoration that
				 * would ordinarily appear on the Vars inside these
				 * CoalesceExprs, because have_partkey_equi_join will strip
				 * varnullingrels from the expressions it will compare to the
				 * partexprs.
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

/*
 * create_rel_agg_info
 *	  Create the RelAggInfo structure for the given relation if it can produce
 *	  grouped paths.  The given relation is the non-grouped one which has the
 *	  reltarget already constructed.
 *
 * calculate_grouped_rows: if true, calculate the estimated number of grouped
 * rows for the relation.  If false, skip the estimation to avoid unnecessary
 * planning overhead.
 */
RelAggInfo *
create_rel_agg_info(PlannerInfo *root, RelOptInfo *rel,
					bool calculate_grouped_rows)
{
	ListCell   *lc;
	RelAggInfo *result;
	PathTarget *agg_input;
	PathTarget *target;
	List	   *group_clauses = NIL;
	List	   *group_exprs = NIL;

	/*
	 * The lists of aggregate expressions and grouping expressions should have
	 * been constructed.
	 */
	Assert(root->agg_clause_list != NIL);
	Assert(root->group_expr_list != NIL);

	/*
	 * If this is a child rel, the grouped rel for its parent rel must have
	 * been created if it can.  So we can just use parent's RelAggInfo if
	 * there is one, with appropriate variable substitutions.
	 */
	if (IS_OTHER_REL(rel))
	{
		RelOptInfo *grouped_rel;
		RelAggInfo *agg_info;

		grouped_rel = rel->top_parent->grouped_rel;
		if (grouped_rel == NULL)
			return NULL;

		Assert(IS_GROUPED_REL(grouped_rel));

		/* Must do multi-level transformation */
		agg_info = (RelAggInfo *)
			adjust_appendrel_attrs_multilevel(root,
											  (Node *) grouped_rel->agg_info,
											  rel,
											  rel->top_parent);

		agg_info->apply_agg_at = NULL;	/* caller will change this later */

		if (calculate_grouped_rows)
		{
			agg_info->grouped_rows =
				estimate_num_groups(root, agg_info->group_exprs,
									rel->rows, NULL, NULL);

			/*
			 * The grouped paths for the given relation are considered useful
			 * iff the average group size is no less than
			 * min_eager_agg_group_size.
			 */
			agg_info->agg_useful =
				(rel->rows / agg_info->grouped_rows) >= min_eager_agg_group_size;
		}

		return agg_info;
	}

	/* Check if it's possible to produce grouped paths for this relation. */
	if (!eager_aggregation_possible_for_relation(root, rel))
		return NULL;

	/*
	 * Create targets for the grouped paths and for the input paths of the
	 * grouped paths.
	 */
	target = create_empty_pathtarget();
	agg_input = create_empty_pathtarget();

	/* ... and initialize these targets */
	if (!init_grouping_targets(root, rel, target, agg_input,
							   &group_clauses, &group_exprs))
		return NULL;

	/*
	 * Eager aggregation is not applicable if there are no available grouping
	 * expressions.
	 */
	if (group_clauses == NIL)
		return NULL;

	/* Add aggregates to the grouping target */
	foreach(lc, root->agg_clause_list)
	{
		AggClauseInfo *ac_info = lfirst_node(AggClauseInfo, lc);
		Aggref	   *aggref;

		Assert(IsA(ac_info->aggref, Aggref));

		aggref = (Aggref *) copyObject(ac_info->aggref);
		mark_partial_aggref(aggref, AGGSPLIT_INITIAL_SERIAL);

		add_column_to_pathtarget(target, (Expr *) aggref, 0);
	}

	/* Set the estimated eval cost and output width for both targets */
	set_pathtarget_cost_width(root, target);
	set_pathtarget_cost_width(root, agg_input);

	/* build the RelAggInfo result */
	result = makeNode(RelAggInfo);
	result->target = target;
	result->agg_input = agg_input;
	result->group_clauses = group_clauses;
	result->group_exprs = group_exprs;
	result->apply_agg_at = NULL;	/* caller will change this later */

	if (calculate_grouped_rows)
	{
		result->grouped_rows = estimate_num_groups(root, result->group_exprs,
												   rel->rows, NULL, NULL);

		/*
		 * The grouped paths for the given relation are considered useful iff
		 * the average group size is no less than min_eager_agg_group_size.
		 */
		result->agg_useful =
			(rel->rows / result->grouped_rows) >= min_eager_agg_group_size;
	}

	return result;
}

/*
 * eager_aggregation_possible_for_relation
 * 	  Check if it's possible to produce grouped paths for the given relation.
 */
static bool
eager_aggregation_possible_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	ListCell   *lc;
	int			cur_relid;

	/*
	 * Check to see if the given relation is in the nullable side of an outer
	 * join.  In this case, we cannot push a partial aggregation down to the
	 * relation, because the NULL-extended rows produced by the outer join
	 * would not be available when we perform the partial aggregation, while
	 * with a non-eager-aggregation plan these rows are available for the
	 * top-level aggregation.  Doing so may result in the rows being grouped
	 * differently than expected, or produce incorrect values from the
	 * aggregate functions.
	 */
	cur_relid = -1;
	while ((cur_relid = bms_next_member(rel->relids, cur_relid)) >= 0)
	{
		RelOptInfo *baserel = find_base_rel_ignore_join(root, cur_relid);

		if (baserel == NULL)
			continue;			/* ignore outer joins in rel->relids */

		if (!bms_is_subset(baserel->nulling_relids, rel->relids))
			return false;
	}

	/*
	 * For now we don't try to support PlaceHolderVars.
	 */
	foreach(lc, rel->reltarget->exprs)
	{
		Expr	   *expr = lfirst(lc);

		if (IsA(expr, PlaceHolderVar))
			return false;
	}

	/* Caller should only pass base relations or joins. */
	Assert(rel->reloptkind == RELOPT_BASEREL ||
		   rel->reloptkind == RELOPT_JOINREL);

	/*
	 * Check if all aggregate expressions can be evaluated on this relation
	 * level.
	 */
	foreach(lc, root->agg_clause_list)
	{
		AggClauseInfo *ac_info = lfirst_node(AggClauseInfo, lc);

		Assert(IsA(ac_info->aggref, Aggref));

		/*
		 * Give up if any aggregate requires relations other than the current
		 * one.  If the aggregate requires the current relation plus
		 * additional relations, grouping the current relation could make some
		 * input rows unavailable for the higher aggregate and may reduce the
		 * number of input rows it receives.  If the aggregate does not
		 * require the current relation at all, it should not be grouped, as
		 * we do not support joining two grouped relations.
		 */
		if (!bms_is_subset(ac_info->agg_eval_at, rel->relids))
			return false;
	}

	return true;
}

/*
 * init_grouping_targets
 *	  Initialize the target for grouped paths (target) as well as the target
 *	  for paths that generate input for the grouped paths (agg_input).
 *
 * We also construct the list of SortGroupClauses and the list of grouping
 * expressions for the partial aggregation, and return them in *group_clause
 * and *group_exprs.
 *
 * Return true if the targets could be initialized, false otherwise.
 */
static bool
init_grouping_targets(PlannerInfo *root, RelOptInfo *rel,
					  PathTarget *target, PathTarget *agg_input,
					  List **group_clauses, List **group_exprs)
{
	ListCell   *lc;
	List	   *possibly_dependent = NIL;
	Index		maxSortGroupRef;

	/* Identify the max sortgroupref */
	maxSortGroupRef = 0;
	foreach(lc, root->processed_tlist)
	{
		Index		ref = ((TargetEntry *) lfirst(lc))->ressortgroupref;

		if (ref > maxSortGroupRef)
			maxSortGroupRef = ref;
	}

	/*
	 * At this point, all Vars from this relation that are needed by upper
	 * joins or are required in the final targetlist should already be present
	 * in its reltarget.  Therefore, we can safely iterate over this
	 * relation's reltarget->exprs to construct the PathTarget and grouping
	 * clauses for the grouped paths.
	 */
	foreach(lc, rel->reltarget->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sortgroupref;

		/*
		 * Given that PlaceHolderVar currently prevents us from doing eager
		 * aggregation, the source target cannot contain anything more complex
		 * than a Var.
		 */
		Assert(IsA(expr, Var));

		/*
		 * Get the sortgroupref of the expr if it is found among, or can be
		 * deduced from, the original grouping expressions.
		 */
		sortgroupref = get_expression_sortgroupref(root, expr);
		if (sortgroupref > 0)
		{
			SortGroupClause *sgc;

			/* Find the matching SortGroupClause */
			sgc = get_sortgroupref_clause(sortgroupref, root->processed_groupClause);
			Assert(sgc->tleSortGroupRef <= maxSortGroupRef);

			/*
			 * If the target expression is to be used as a grouping key, it
			 * should be emitted by the grouped paths that have been pushed
			 * down to this relation level.
			 */
			add_column_to_pathtarget(target, expr, sortgroupref);

			/*
			 * ... and it also should be emitted by the input paths.
			 */
			add_column_to_pathtarget(agg_input, expr, sortgroupref);

			/*
			 * Record this SortGroupClause and grouping expression.  Note that
			 * this SortGroupClause might have already been recorded.
			 */
			if (!list_member(*group_clauses, sgc))
			{
				*group_clauses = lappend(*group_clauses, sgc);
				*group_exprs = lappend(*group_exprs, expr);
			}
		}
		else if (is_var_needed_by_join(root, (Var *) expr, rel))
		{
			/*
			 * The expression is needed for an upper join but is neither in
			 * the GROUP BY clause nor derivable from it using EC (otherwise,
			 * it would have already been included in the targets above).  We
			 * need to create a special SortGroupClause for this expression.
			 *
			 * It is important to include such expressions in the grouping
			 * keys.  This is essential to ensure that an aggregated row from
			 * the partial aggregation matches the other side of the join if
			 * and only if each row in the partial group does.  This ensures
			 * that all rows within the same partial group share the same
			 * 'destiny', which is crucial for maintaining correctness.
			 */
			SortGroupClause *sgc;
			TypeCacheEntry *tce;
			Oid			equalimageproc;

			/*
			 * But first, check if equality implies image equality for this
			 * expression.  If not, we cannot use it as a grouping key.  See
			 * comments in create_grouping_expr_infos().
			 */
			tce = lookup_type_cache(exprType((Node *) expr),
									TYPECACHE_BTREE_OPFAMILY);
			if (!OidIsValid(tce->btree_opf) ||
				!OidIsValid(tce->btree_opintype))
				return false;

			equalimageproc = get_opfamily_proc(tce->btree_opf,
											   tce->btree_opintype,
											   tce->btree_opintype,
											   BTEQUALIMAGE_PROC);
			if (!OidIsValid(equalimageproc) ||
				!DatumGetBool(OidFunctionCall1Coll(equalimageproc,
												   tce->typcollation,
												   ObjectIdGetDatum(tce->btree_opintype))))
				return false;

			/* Create the SortGroupClause. */
			sgc = makeNode(SortGroupClause);

			/* Initialize the SortGroupClause. */
			sgc->tleSortGroupRef = ++maxSortGroupRef;
			get_sort_group_operators(exprType((Node *) expr),
									 false, true, false,
									 &sgc->sortop, &sgc->eqop, NULL,
									 &sgc->hashable);

			/* This expression should be emitted by the grouped paths */
			add_column_to_pathtarget(target, expr, sgc->tleSortGroupRef);

			/* ... and it also should be emitted by the input paths. */
			add_column_to_pathtarget(agg_input, expr, sgc->tleSortGroupRef);

			/* Record this SortGroupClause and grouping expression */
			*group_clauses = lappend(*group_clauses, sgc);
			*group_exprs = lappend(*group_exprs, expr);
		}
		else if (is_var_in_aggref_only(root, (Var *) expr))
		{
			/*
			 * The expression is referenced by an aggregate function pushed
			 * down to this relation and does not appear elsewhere in the
			 * targetlist or havingQual.  Add it to 'agg_input' but not to
			 * 'target'.
			 */
			add_new_column_to_pathtarget(agg_input, expr);
		}
		else
		{
			/*
			 * The expression may be functionally dependent on other
			 * expressions in the target, but we cannot verify this until all
			 * target expressions have been constructed.
			 */
			possibly_dependent = lappend(possibly_dependent, expr);
		}
	}

	/*
	 * Now we can verify whether an expression is functionally dependent on
	 * others.
	 */
	foreach(lc, possibly_dependent)
	{
		Var		   *tvar;
		List	   *deps = NIL;
		RangeTblEntry *rte;

		tvar = lfirst_node(Var, lc);
		rte = root->simple_rte_array[tvar->varno];

		if (check_functional_grouping(rte->relid, tvar->varno,
									  tvar->varlevelsup,
									  target->exprs, &deps))
		{
			/*
			 * The expression is functionally dependent on other target
			 * expressions, so it can be included in the targets.  Since it
			 * will not be used as a grouping key, a sortgroupref is not
			 * needed for it.
			 */
			add_new_column_to_pathtarget(target, (Expr *) tvar);
			add_new_column_to_pathtarget(agg_input, (Expr *) tvar);
		}
		else
		{
			/*
			 * We may arrive here with a grouping expression that is proven
			 * redundant by EquivalenceClass processing, such as 't1.a' in the
			 * query below.
			 *
			 * select max(t1.c) from t t1, t t2 where t1.a = 1 group by t1.a,
			 * t1.b;
			 *
			 * For now we just give up in this case.
			 */
			return false;
		}
	}

	return true;
}

/*
 * is_var_in_aggref_only
 *	  Check whether the given Var appears in aggregate expressions and not
 *	  elsewhere in the targetlist or havingQual.
 */
static bool
is_var_in_aggref_only(PlannerInfo *root, Var *var)
{
	ListCell   *lc;

	/*
	 * Search the list of aggregate expressions for the Var.
	 */
	foreach(lc, root->agg_clause_list)
	{
		AggClauseInfo *ac_info = lfirst_node(AggClauseInfo, lc);
		List	   *vars;

		Assert(IsA(ac_info->aggref, Aggref));

		if (!bms_is_member(var->varno, ac_info->agg_eval_at))
			continue;

		vars = pull_var_clause((Node *) ac_info->aggref,
							   PVC_RECURSE_AGGREGATES |
							   PVC_RECURSE_WINDOWFUNCS |
							   PVC_RECURSE_PLACEHOLDERS);

		if (list_member(vars, var))
		{
			list_free(vars);
			break;
		}

		list_free(vars);
	}

	return (lc != NULL && !list_member(root->tlist_vars, var));
}

/*
 * is_var_needed_by_join
 *	  Check if the given Var is needed by joins above the current rel.
 */
static bool
is_var_needed_by_join(PlannerInfo *root, Var *var, RelOptInfo *rel)
{
	Relids		relids;
	int			attno;
	RelOptInfo *baserel;

	/*
	 * Note that when checking if the Var is needed by joins above, we want to
	 * exclude cases where the Var is only needed in the final targetlist.  So
	 * include "relation 0" in the check.
	 */
	relids = bms_copy(rel->relids);
	relids = bms_add_member(relids, 0);

	baserel = find_base_rel(root, var->varno);
	attno = var->varattno - baserel->min_attr;

	return bms_nonempty_difference(baserel->attr_needed[attno], relids);
}

/*
 * get_expression_sortgroupref
 *	  Return the sortgroupref of the given "expr" if it is found among the
 *	  original grouping expressions, or is known equal to any of the original
 *	  grouping expressions due to equivalence relationships.  Return 0 if no
 *	  match is found.
 */
static Index
get_expression_sortgroupref(PlannerInfo *root, Expr *expr)
{
	ListCell   *lc;

	Assert(IsA(expr, Var));

	foreach(lc, root->group_expr_list)
	{
		GroupingExprInfo *ge_info = lfirst_node(GroupingExprInfo, lc);
		ListCell   *lc1;

		Assert(IsA(ge_info->expr, Var));
		Assert(ge_info->sortgroupref > 0);

		if (equal(expr, ge_info->expr))
			return ge_info->sortgroupref;

		if (ge_info->ec == NULL ||
			!bms_is_member(((Var *) expr)->varno, ge_info->ec->ec_relids))
			continue;

		/*
		 * Scan the EquivalenceClass, looking for a match to the given
		 * expression.  We ignore child members here.
		 */
		foreach(lc1, ge_info->ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc1);

			/* Child members should not exist in ec_members */
			Assert(!em->em_is_child);

			if (equal(expr, em->em_expr))
				return ge_info->sortgroupref;
		}
	}

	/* no match is found */
	return 0;
}
