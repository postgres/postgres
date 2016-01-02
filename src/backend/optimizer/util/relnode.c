/*-------------------------------------------------------------------------
 *
 * relnode.c
 *	  Relation-node lookup/construction routines
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/util/relnode.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/placeholder.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "utils/hsearch.h"


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


/*
 * setup_simple_rel_arrays
 *	  Prepare the arrays we use for quickly accessing base relations.
 */
void
setup_simple_rel_arrays(PlannerInfo *root)
{
	Index		rti;
	ListCell   *lc;

	/* Arrays are accessed using RT indexes (1..N) */
	root->simple_rel_array_size = list_length(root->parse->rtable) + 1;

	/* simple_rel_array is initialized to all NULLs */
	root->simple_rel_array = (RelOptInfo **)
		palloc0(root->simple_rel_array_size * sizeof(RelOptInfo *));

	/* simple_rte_array is an array equivalent of the rtable list */
	root->simple_rte_array = (RangeTblEntry **)
		palloc0(root->simple_rel_array_size * sizeof(RangeTblEntry *));
	rti = 1;
	foreach(lc, root->parse->rtable)
	{
		RangeTblEntry *rte = (RangeTblEntry *) lfirst(lc);

		root->simple_rte_array[rti++] = rte;
	}
}

/*
 * build_simple_rel
 *	  Construct a new RelOptInfo for a base relation or 'other' relation.
 */
RelOptInfo *
build_simple_rel(PlannerInfo *root, int relid, RelOptKind reloptkind)
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
	rel->reloptkind = reloptkind;
	rel->relids = bms_make_singleton(relid);
	rel->rows = 0;
	rel->width = 0;
	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	rel->consider_startup = (root->tuple_fraction > 0);
	rel->consider_param_startup = false;		/* might get changed later */
	rel->consider_parallel = false;		/* might get changed later */
	rel->reltargetlist = NIL;
	rel->pathlist = NIL;
	rel->ppilist = NIL;
	rel->cheapest_startup_path = NULL;
	rel->cheapest_total_path = NULL;
	rel->cheapest_unique_path = NULL;
	rel->cheapest_parameterized_paths = NIL;
	rel->direct_lateral_relids = NULL;
	rel->lateral_relids = NULL;
	rel->relid = relid;
	rel->rtekind = rte->rtekind;
	/* min_attr, max_attr, attr_needed, attr_widths are set below */
	rel->lateral_vars = NIL;
	rel->lateral_referencers = NULL;
	rel->indexlist = NIL;
	rel->pages = 0;
	rel->tuples = 0;
	rel->allvisfrac = 0;
	rel->subplan = NULL;
	rel->subroot = NULL;
	rel->subplan_params = NIL;
	rel->serverid = InvalidOid;
	rel->fdwroutine = NULL;
	rel->fdw_private = NULL;
	rel->baserestrictinfo = NIL;
	rel->baserestrictcost.startup = 0;
	rel->baserestrictcost.per_tuple = 0;
	rel->joininfo = NIL;
	rel->has_eclass_joins = false;

	/* Check type of rtable entry */
	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* Table --- retrieve statistics from the system catalogs */
			get_relation_info(root, rte->relid, rte->inh, rel);
			break;
		case RTE_SUBQUERY:
		case RTE_FUNCTION:
		case RTE_VALUES:
		case RTE_CTE:

			/*
			 * Subquery, function, or values list --- set up attr range and
			 * arrays
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
		default:
			elog(ERROR, "unrecognized RTE kind: %d",
				 (int) rte->rtekind);
			break;
	}

	/* Save the finished struct in the query's simple_rel_array */
	root->simple_rel_array[relid] = rel;

	/*
	 * If this rel is an appendrel parent, recurse to build "other rel"
	 * RelOptInfos for its children.  They are "other rels" because they are
	 * not in the main join tree, but we will need RelOptInfos to plan access
	 * to them.
	 */
	if (rte->inh)
	{
		ListCell   *l;

		foreach(l, root->append_rel_list)
		{
			AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(l);

			/* append_rel_list contains all append rels; ignore others */
			if (appinfo->parent_relid != relid)
				continue;

			(void) build_simple_rel(root, appinfo->child_relid,
									RELOPT_OTHER_MEMBER_REL);
		}
	}

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
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
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
	joinrel->width = 0;
	/* cheap startup cost is interesting iff not all tuples to be retrieved */
	joinrel->consider_startup = (root->tuple_fraction > 0);
	joinrel->consider_param_startup = false;
	joinrel->consider_parallel = false;
	joinrel->reltargetlist = NIL;
	joinrel->pathlist = NIL;
	joinrel->ppilist = NIL;
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
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->allvisfrac = 0;
	joinrel->subplan = NULL;
	joinrel->subroot = NULL;
	joinrel->subplan_params = NIL;
	joinrel->serverid = InvalidOid;
	joinrel->fdwroutine = NULL;
	joinrel->fdw_private = NULL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->joininfo = NIL;
	joinrel->has_eclass_joins = false;

	/*
	 * Set up foreign-join fields if outer and inner relation are foreign
	 * tables (or joins) belonging to the same server.
	 */
	if (OidIsValid(outer_rel->serverid) &&
		inner_rel->serverid == outer_rel->serverid)
	{
		joinrel->serverid = outer_rel->serverid;
		joinrel->fdwroutine = outer_rel->fdwroutine;
	}

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
	add_placeholders_to_joinrel(root, joinrel);

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

	/*
	 * Set estimates of the joinrel's size.
	 */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
							   sjinfo, restrictlist);

	/*
	 * Set the consider_parallel flag if this joinrel could potentially be
	 * scanned within a parallel worker.  If this flag is false for either
	 * inner_rel or outer_rel, then it must be false for the joinrel also.
	 * Even if both are true, there might be parallel-restricted quals at our
	 * level.
	 *
	 * Note that if there are more than two rels in this relation, they could
	 * be divided between inner_rel and outer_rel in any arbitary way.  We
	 * assume this doesn't matter, because we should hit all the same baserels
	 * and joinclauses while building up to this joinrel no matter which we
	 * take; therefore, we should make the same decision here however we get
	 * here.
	 */
	if (inner_rel->consider_parallel && outer_rel->consider_parallel &&
		!has_parallel_hazard((Node *) restrictlist, false))
		joinrel->consider_parallel = true;

	/*
	 * Add the joinrel to the query's joinrel list, and store it into the
	 * auxiliary hashtable if there is one.  NB: GEQO requires us to append
	 * the new joinrel to the end of the list!
	 */
	root->join_rel_list = lappend(root->join_rel_list, joinrel);

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

	foreach(vars, input_rel->reltargetlist)
	{
		Var		   *var = (Var *) lfirst(vars);
		RelOptInfo *baserel;
		int			ndx;

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
			elog(ERROR, "unexpected node type in reltargetlist: %d",
				 (int) nodeTag(var));

		/* Get the Var's original base rel */
		baserel = find_base_rel(root, var->varno);

		/* Is it still needed above this joinrel? */
		ndx = var->varattno - baserel->min_attr;
		if (bms_nonempty_difference(baserel->attr_needed[ndx], relids))
		{
			/* Yup, add it to the output */
			joinrel->reltargetlist = lappend(joinrel->reltargetlist, var);
			joinrel->width += baserel->attr_widths[ndx];
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
 *	  In many case we will find the same RestrictInfos in both input
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
 * build_empty_join_rel
 *		Build a dummy join relation describing an empty set of base rels.
 *
 * This is used for queries with empty FROM clauses, such as "SELECT 2+2" or
 * "INSERT INTO foo VALUES(...)".  We don't try very hard to make the empty
 * joinrel completely valid, since no real planning will be done with it ---
 * we just need it to carry a simple Result path out of query_planner().
 */
RelOptInfo *
build_empty_join_rel(PlannerInfo *root)
{
	RelOptInfo *joinrel;

	/* The dummy join relation should be the only one ... */
	Assert(root->join_rel_list == NIL);

	joinrel = makeNode(RelOptInfo);
	joinrel->reloptkind = RELOPT_JOINREL;
	joinrel->relids = NULL;		/* empty set */
	joinrel->rows = 1;			/* we produce one row for such cases */
	joinrel->width = 0;			/* it contains no Vars */
	joinrel->rtekind = RTE_JOIN;

	root->join_rel_list = lappend(root->join_rel_list, joinrel);

	return joinrel;
}


/*
 * find_childrel_appendrelinfo
 *		Get the AppendRelInfo associated with an appendrel child rel.
 *
 * This search could be eliminated by storing a link in child RelOptInfos,
 * but for now it doesn't seem performance-critical.  (Also, it might be
 * difficult to maintain such a link during mutation of the append_rel_list.)
 */
AppendRelInfo *
find_childrel_appendrelinfo(PlannerInfo *root, RelOptInfo *rel)
{
	Index		relid = rel->relid;
	ListCell   *lc;

	/* Should only be called on child rels */
	Assert(rel->reloptkind == RELOPT_OTHER_MEMBER_REL);

	foreach(lc, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lc);

		if (appinfo->child_relid == relid)
			return appinfo;
	}
	/* should have found the entry ... */
	elog(ERROR, "child rel %d not found in append_rel_list", relid);
	return NULL;				/* not reached */
}


/*
 * find_childrel_top_parent
 *		Fetch the topmost appendrel parent rel of an appendrel child rel.
 *
 * Since appendrels can be nested, a child could have multiple levels of
 * appendrel ancestors.  This function locates the topmost ancestor,
 * which will be a regular baserel not an otherrel.
 */
RelOptInfo *
find_childrel_top_parent(PlannerInfo *root, RelOptInfo *rel)
{
	do
	{
		AppendRelInfo *appinfo = find_childrel_appendrelinfo(root, rel);
		Index		prelid = appinfo->parent_relid;

		/* traverse up to the parent rel, loop if it's also a child rel */
		rel = find_base_rel(root, prelid);
	} while (rel->reloptkind == RELOPT_OTHER_MEMBER_REL);

	Assert(rel->reloptkind == RELOPT_BASEREL);

	return rel;
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

	do
	{
		AppendRelInfo *appinfo = find_childrel_appendrelinfo(root, rel);
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

	/* Unparameterized paths have no ParamPathInfo */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(baserel->relids, required_outer));

	/* If we already have a PPI for this parameterization, just return it */
	foreach(lc, baserel->ppilist)
	{
		ppi = (ParamPathInfo *) lfirst(lc);
		if (bms_equal(ppi->ppi_req_outer, required_outer))
			return ppi;
	}

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
	double		rows;
	ListCell   *lc;

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
		if (!join_clause_is_movable_into(rinfo,
										 outer_path->parent->relids,
										 outer_and_req) &&
			!join_clause_is_movable_into(rinfo,
										 inner_path->parent->relids,
										 inner_and_req))
			pclauses = lappend(pclauses, rinfo);
	}

	/*
	 * Now, attach the identified moved-down clauses to the caller's
	 * restrict_clauses list.  By using list_concat in this order, we leave
	 * the original list structure of restrict_clauses undamaged.
	 */
	*restrict_clauses = list_concat(pclauses, *restrict_clauses);

	/* If we already have a PPI for this parameterization, just return it */
	foreach(lc, joinrel->ppilist)
	{
		ppi = (ParamPathInfo *) lfirst(lc);
		if (bms_equal(ppi->ppi_req_outer, required_outer))
			return ppi;
	}

	/* Estimate the number of rows returned by the parameterized join */
	rows = get_parameterized_joinrel_size(root, joinrel,
										  outer_path->rows,
										  inner_path->rows,
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
	ListCell   *lc;

	/* Unparameterized paths have no ParamPathInfo */
	if (bms_is_empty(required_outer))
		return NULL;

	Assert(!bms_overlap(appendrel->relids, required_outer));

	/* If we already have a PPI for this parameterization, just return it */
	foreach(lc, appendrel->ppilist)
	{
		ppi = (ParamPathInfo *) lfirst(lc);
		if (bms_equal(ppi->ppi_req_outer, required_outer))
			return ppi;
	}

	/* Else build the ParamPathInfo */
	ppi = makeNode(ParamPathInfo);
	ppi->ppi_req_outer = required_outer;
	ppi->ppi_rows = 0;
	ppi->ppi_clauses = NIL;
	appendrel->ppilist = lappend(appendrel->ppilist, ppi);

	return ppi;
}
