/*-------------------------------------------------------------------------
 *
 * relnode.c
 *	  Relation-node lookup/construction routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/relnode.c,v 1.52.4.1 2003/12/08 18:20:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/cost.h"
#include "optimizer/joininfo.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"


static RelOptInfo *make_base_rel(Query *root, int relid);
static void build_joinrel_tlist(Query *root, RelOptInfo *joinrel);
static List *build_joinrel_restrictlist(Query *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   JoinType jointype);
static void build_joinrel_joinlist(RelOptInfo *joinrel,
					   RelOptInfo *outer_rel,
					   RelOptInfo *inner_rel);
static List *subbuild_joinrel_restrictlist(RelOptInfo *joinrel,
							  List *joininfo_list);
static void subbuild_joinrel_joinlist(RelOptInfo *joinrel,
						  List *joininfo_list);


/*
 * build_base_rel
 *	  Construct a new base relation RelOptInfo, and put it in the query's
 *	  base_rel_list.
 */
void
build_base_rel(Query *root, int relid)
{
	List	   *rels;
	RelOptInfo *rel;

	/* Rel should not exist already */
	foreach(rels, root->base_rel_list)
	{
		rel = (RelOptInfo *) lfirst(rels);
		if (relid == rel->relid)
			elog(ERROR, "rel already exists");
	}

	/* It should not exist as an "other" rel, either */
	foreach(rels, root->other_rel_list)
	{
		rel = (RelOptInfo *) lfirst(rels);
		if (relid == rel->relid)
			elog(ERROR, "rel already exists as \"other\" rel");
	}

	/* No existing RelOptInfo for this base rel, so make a new one */
	rel = make_base_rel(root, relid);

	/* and add it to the list */
	root->base_rel_list = lcons(rel, root->base_rel_list);
}

/*
 * build_other_rel
 *	  Returns relation entry corresponding to 'relid', creating a new one
 *	  if necessary.  This is for 'other' relations, which are much like
 *	  base relations except that they live in a different list.
 */
RelOptInfo *
build_other_rel(Query *root, int relid)
{
	List	   *rels;
	RelOptInfo *rel;

	/* Already made? */
	foreach(rels, root->other_rel_list)
	{
		rel = (RelOptInfo *) lfirst(rels);
		if (relid == rel->relid)
			return rel;
	}

	/* It should not exist as a base rel */
	foreach(rels, root->base_rel_list)
	{
		rel = (RelOptInfo *) lfirst(rels);
		if (relid == rel->relid)
			elog(ERROR, "rel already exists as base rel");
	}

	/* No existing RelOptInfo for this other rel, so make a new one */
	rel = make_base_rel(root, relid);

	/* presently, must be an inheritance child rel */
	Assert(rel->reloptkind == RELOPT_BASEREL);
	rel->reloptkind = RELOPT_OTHER_CHILD_REL;

	/* and add it to the list */
	root->other_rel_list = lcons(rel, root->other_rel_list);

	return rel;
}

/*
 * make_base_rel
 *	  Construct a base-relation RelOptInfo for the specified rangetable index.
 *
 * Common code for build_base_rel and build_other_rel.
 */
static RelOptInfo *
make_base_rel(Query *root, int relid)
{
	RelOptInfo *rel = makeNode(RelOptInfo);
	RangeTblEntry *rte = rt_fetch(relid, root->rtable);

	rel->reloptkind = RELOPT_BASEREL;
	rel->relids = bms_make_singleton(relid);
	rel->rows = 0;
	rel->width = 0;
	FastListInit(&rel->reltargetlist);
	rel->pathlist = NIL;
	rel->cheapest_startup_path = NULL;
	rel->cheapest_total_path = NULL;
	rel->cheapest_unique_path = NULL;
	rel->pruneable = true;
	rel->relid = relid;
	rel->rtekind = rte->rtekind;
	/* min_attr, max_attr, attr_needed, attr_widths are set below */
	rel->indexlist = NIL;
	rel->pages = 0;
	rel->tuples = 0;
	rel->subplan = NULL;
	rel->baserestrictinfo = NIL;
	rel->baserestrictcost.startup = 0;
	rel->baserestrictcost.per_tuple = 0;
	rel->outerjoinset = NULL;
	rel->joininfo = NIL;
	rel->index_outer_relids = NULL;
	rel->index_inner_paths = NIL;

	/* Check type of rtable entry */
	switch (rte->rtekind)
	{
		case RTE_RELATION:
			/* Table --- retrieve statistics from the system catalogs */
			get_relation_info(rte->relid, rel);
			break;
		case RTE_SUBQUERY:
		case RTE_FUNCTION:
			/* Subquery or function --- need only set up attr range */
			/* Note: 0 is included in range to support whole-row Vars */
			rel->min_attr = 0;
			rel->max_attr = length(rte->eref->colnames);
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d",
				 (int) rte->rtekind);
			break;
	}

	Assert(rel->max_attr >= rel->min_attr);
	rel->attr_needed = (Relids *)
		palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(Relids));
	rel->attr_widths = (int32 *)
		palloc0((rel->max_attr - rel->min_attr + 1) * sizeof(int32));

	return rel;
}

/*
 * find_base_rel
 *	  Find a base or other relation entry, which must already exist
 *	  (since we'd have no idea which list to add it to).
 */
RelOptInfo *
find_base_rel(Query *root, int relid)
{
	List	   *rels;
	RelOptInfo *rel;

	foreach(rels, root->base_rel_list)
	{
		rel = (RelOptInfo *) lfirst(rels);
		if (relid == rel->relid)
			return rel;
	}

	foreach(rels, root->other_rel_list)
	{
		rel = (RelOptInfo *) lfirst(rels);
		if (relid == rel->relid)
			return rel;
	}

	elog(ERROR, "no relation entry for relid %d", relid);

	return NULL;				/* keep compiler quiet */
}

/*
 * find_join_rel
 *	  Returns relation entry corresponding to 'relids' (a set of RT indexes),
 *	  or NULL if none exists.  This is for join relations.
 *
 * Note: there is probably no good reason for this to be called from
 * anywhere except build_join_rel, but keep it as a separate routine
 * just in case.
 */
static RelOptInfo *
find_join_rel(Query *root, Relids relids)
{
	List	   *joinrels;

	foreach(joinrels, root->join_rel_list)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(joinrels);

		if (bms_equal(rel->relids, relids))
			return rel;
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
 * 'jointype': type of join (inner/outer)
 * 'restrictlist_ptr': result variable.  If not NULL, *restrictlist_ptr
 *		receives the list of RestrictInfo nodes that apply to this
 *		particular pair of joinable relations.
 *
 * restrictlist_ptr makes the routine's API a little grotty, but it saves
 * duplicated calculation of the restrictlist...
 */
RelOptInfo *
build_join_rel(Query *root,
			   Relids joinrelids,
			   RelOptInfo *outer_rel,
			   RelOptInfo *inner_rel,
			   JoinType jointype,
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
		 * Yes, so we only need to figure the restrictlist for this
		 * particular pair of component relations.
		 */
		if (restrictlist_ptr)
			*restrictlist_ptr = build_joinrel_restrictlist(root,
														   joinrel,
														   outer_rel,
														   inner_rel,
														   jointype);
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
	FastListInit(&joinrel->reltargetlist);
	joinrel->pathlist = NIL;
	joinrel->cheapest_startup_path = NULL;
	joinrel->cheapest_total_path = NULL;
	joinrel->cheapest_unique_path = NULL;
	joinrel->pruneable = true;
	joinrel->relid = 0;			/* indicates not a baserel */
	joinrel->rtekind = RTE_JOIN;
	joinrel->min_attr = 0;
	joinrel->max_attr = 0;
	joinrel->attr_needed = NULL;
	joinrel->attr_widths = NULL;
	joinrel->indexlist = NIL;
	joinrel->pages = 0;
	joinrel->tuples = 0;
	joinrel->subplan = NULL;
	joinrel->baserestrictinfo = NIL;
	joinrel->baserestrictcost.startup = 0;
	joinrel->baserestrictcost.per_tuple = 0;
	joinrel->outerjoinset = NULL;
	joinrel->joininfo = NIL;
	joinrel->index_outer_relids = NULL;
	joinrel->index_inner_paths = NIL;

	/*
	 * Create a new tlist containing just the vars that need to be output
	 * from this join (ie, are needed for higher joinclauses or final
	 * output).
	 */
	build_joinrel_tlist(root, joinrel);

	/*
	 * Construct restrict and join clause lists for the new joinrel. (The
	 * caller might or might not need the restrictlist, but I need it
	 * anyway for set_joinrel_size_estimates().)
	 */
	restrictlist = build_joinrel_restrictlist(root,
											  joinrel,
											  outer_rel,
											  inner_rel,
											  jointype);
	if (restrictlist_ptr)
		*restrictlist_ptr = restrictlist;
	build_joinrel_joinlist(joinrel, outer_rel, inner_rel);

	/*
	 * Set estimates of the joinrel's size.
	 */
	set_joinrel_size_estimates(root, joinrel, outer_rel, inner_rel,
							   jointype, restrictlist);

	/*
	 * Add the joinrel to the query's joinrel list.
	 */
	root->join_rel_list = lcons(joinrel, root->join_rel_list);

	return joinrel;
}

/*
 * build_joinrel_tlist
 *	  Builds a join relation's target list.
 *
 * The join's targetlist includes all Vars of its member relations that
 * will still be needed above the join.
 *
 * In a former lifetime, this just merged the tlists of the two member
 * relations first presented.  While we could still do that, working from
 * lists of Vars would mean doing a find_base_rel lookup for each Var.
 * It seems more efficient to scan the list of base rels and collect the
 * needed vars directly from there.
 *
 * We also compute the expected width of the join's output, making use
 * of data that was cached at the baserel level by set_rel_width().
 */
static void
build_joinrel_tlist(Query *root, RelOptInfo *joinrel)
{
	Relids		relids = joinrel->relids;
	List	   *rels;
	List	   *vars;

	FastListInit(&joinrel->reltargetlist);
	joinrel->width = 0;

	foreach(rels, root->base_rel_list)
	{
		RelOptInfo *baserel = (RelOptInfo *) lfirst(rels);

		if (!bms_is_member(baserel->relid, relids))
			continue;

		foreach(vars, FastListValue(&baserel->reltargetlist))
		{
			Var		   *var = (Var *) lfirst(vars);
			int			ndx = var->varattno - baserel->min_attr;

			if (bms_nonempty_difference(baserel->attr_needed[ndx], relids))
			{
				FastAppend(&joinrel->reltargetlist, var);
				Assert(baserel->attr_widths[ndx] > 0);
				joinrel->width += baserel->attr_widths[ndx];
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
 *	  the join lists need only be computed once for any join RelOptInfo.
 *	  The join lists are fully determined by the set of rels making up the
 *	  joinrel, so we should get the same results (up to ordering) from any
 *	  candidate pair of sub-relations.	But the restriction list is whatever
 *	  is not handled in the sub-relations, so it depends on which
 *	  sub-relations are considered.
 *
 *	  If a join clause from an input relation refers to base rels still not
 *	  present in the joinrel, then it is still a join clause for the joinrel;
 *	  we put it into an appropriate JoinInfo list for the joinrel.	Otherwise,
 *	  the clause is now a restrict clause for the joined relation, and we
 *	  return it to the caller of build_joinrel_restrictlist() to be stored in
 *	  join paths made from this pair of sub-relations.	(It will not need to
 *	  be considered further up the join tree.)
 *
 *	  When building a restriction list, we eliminate redundant clauses.
 *	  We don't try to do that for join clause lists, since the join clauses
 *	  aren't really doing anything, just waiting to become part of higher
 *	  levels' restriction lists.
 *
 * 'joinrel' is a join relation node
 * 'outer_rel' and 'inner_rel' are a pair of relations that can be joined
 *		to form joinrel.
 * 'jointype' is the type of join used.
 *
 * build_joinrel_restrictlist() returns a list of relevant restrictinfos,
 * whereas build_joinrel_joinlist() stores its results in the joinrel's
 * joininfo lists.	One or the other must accept each given clause!
 *
 * NB: Formerly, we made deep(!) copies of each input RestrictInfo to pass
 * up to the join relation.  I believe this is no longer necessary, because
 * RestrictInfo nodes are no longer context-dependent.	Instead, just include
 * the original nodes in the lists made for the join relation.
 */
static List *
build_joinrel_restrictlist(Query *root,
						   RelOptInfo *joinrel,
						   RelOptInfo *outer_rel,
						   RelOptInfo *inner_rel,
						   JoinType jointype)
{
	List	   *result;
	List	   *rlist;

	/*
	 * Collect all the clauses that syntactically belong at this level.
	 */
	rlist = nconc(subbuild_joinrel_restrictlist(joinrel,
												outer_rel->joininfo),
				  subbuild_joinrel_restrictlist(joinrel,
												inner_rel->joininfo));

	/*
	 * Eliminate duplicate and redundant clauses.
	 *
	 * We must eliminate duplicates, since we will see many of the same
	 * clauses arriving from both input relations.	Also, if a clause is a
	 * mergejoinable clause, it's possible that it is redundant with
	 * previous clauses (see optimizer/README for discussion).	We detect
	 * that case and omit the redundant clause from the result list.
	 */
	result = remove_redundant_join_clauses(root, rlist, jointype);

	freeList(rlist);

	return result;
}

static void
build_joinrel_joinlist(RelOptInfo *joinrel,
					   RelOptInfo *outer_rel,
					   RelOptInfo *inner_rel)
{
	subbuild_joinrel_joinlist(joinrel, outer_rel->joininfo);
	subbuild_joinrel_joinlist(joinrel, inner_rel->joininfo);
}

static List *
subbuild_joinrel_restrictlist(RelOptInfo *joinrel,
							  List *joininfo_list)
{
	List	   *restrictlist = NIL;
	List	   *xjoininfo;

	foreach(xjoininfo, joininfo_list)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(xjoininfo);

		if (bms_is_subset(joininfo->unjoined_relids, joinrel->relids))
		{
			/*
			 * Clauses in this JoinInfo list become restriction clauses
			 * for the joinrel, since they refer to no outside rels.
			 *
			 * We must copy the list to avoid disturbing the input relation,
			 * but we can use a shallow copy.
			 */
			restrictlist = nconc(restrictlist,
								 listCopy(joininfo->jinfo_restrictinfo));
		}
		else
		{
			/*
			 * These clauses are still join clauses at this level, so we
			 * ignore them in this routine.
			 */
		}
	}

	return restrictlist;
}

static void
subbuild_joinrel_joinlist(RelOptInfo *joinrel,
						  List *joininfo_list)
{
	List	   *xjoininfo;

	foreach(xjoininfo, joininfo_list)
	{
		JoinInfo   *joininfo = (JoinInfo *) lfirst(xjoininfo);
		Relids		new_unjoined_relids;

		new_unjoined_relids = bms_difference(joininfo->unjoined_relids,
											 joinrel->relids);
		if (bms_is_empty(new_unjoined_relids))
		{
			/*
			 * Clauses in this JoinInfo list become restriction clauses
			 * for the joinrel, since they refer to no outside rels. So we
			 * can ignore them in this routine.
			 */
			bms_free(new_unjoined_relids);
		}
		else
		{
			/*
			 * These clauses are still join clauses at this level, so find
			 * or make the appropriate JoinInfo item for the joinrel, and
			 * add the clauses to it, eliminating duplicates.  (Since
			 * RestrictInfo nodes are normally multiply-linked rather than
			 * copied, pointer equality should be a sufficient test.  If
			 * two equal() nodes should happen to sneak in, no great harm
			 * is done --- they'll be detected by redundant-clause testing
			 * when they reach a restriction list.)
			 */
			JoinInfo   *new_joininfo;

			new_joininfo = make_joininfo_node(joinrel, new_unjoined_relids);
			new_joininfo->jinfo_restrictinfo =
				set_ptrUnion(new_joininfo->jinfo_restrictinfo,
							 joininfo->jinfo_restrictinfo);
		}
	}
}
