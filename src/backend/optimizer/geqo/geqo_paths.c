/*-------------------------------------------------------------------------
 *
 * geqo_paths.c--
 *	  Routines to process redundant paths and relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_paths.c,v 1.12 1998/09/01 04:29:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"
#include "nodes/primnodes.h"

#include "utils/palloc.h"
#include "utils/elog.h"

#include "optimizer/internal.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"

#include "optimizer/geqo_paths.h"


static List *geqo_prune_rel(RelOptInfo * rel, List *other_rels);
static Path *set_paths(RelOptInfo * rel, Path *unorderedpath);

/*
 * geqo-prune-rels--
 *	  Removes any redundant relation entries from a list of rel nodes
 *	  'rel-list'.
 *
 * Returns the resulting list.
 *
 */
List *
geqo_prune_rels(List *rel_list)
{
	List	   *temp_list = NIL;

	if (rel_list != NIL)
	{
		temp_list = lcons(lfirst(rel_list),
		  geqo_prune_rels(geqo_prune_rel((RelOptInfo *) lfirst(rel_list),
										 lnext(rel_list))));
	}
	return temp_list;
}

/*
 * geqo-prune-rel--
 *	  Prunes those relations from 'other-rels' that are redundant with
 *	  'rel'.  A relation is redundant if it is built up of the same
 *	  relations as 'rel'.  Paths for the redundant relation are merged into
 *	  the pathlist of 'rel'.
 *
 * Returns a list of non-redundant relations, and sets the pathlist field
 * of 'rel' appropriately.
 *
 */
static List *
geqo_prune_rel(RelOptInfo * rel, List *other_rels)
{
	List	   *i = NIL;
	List	   *t_list = NIL;
	List	   *temp_node = NIL;
	RelOptInfo *other_rel = (RelOptInfo *) NULL;

	foreach(i, other_rels)
	{
		other_rel = (RelOptInfo *) lfirst(i);
		if (same(rel->relids, other_rel->relids))
		{
			rel->pathlist = add_pathlist(rel,
										 rel->pathlist,
										 other_rel->pathlist);
			t_list = nconc(t_list, NIL);		/* XXX is this right ? */
		}
		else
		{
			temp_node = lcons(other_rel, NIL);
			t_list = nconc(t_list, temp_node);
		}
	}
	return t_list;
}

/*
 * geqo-rel-paths--
 *	  For a relation 'rel' (which corresponds to a join
 *	  relation), set pointers to the unordered path and cheapest paths
 *	  (if the unordered path isn't the cheapest, it is pruned), and
 *	  reset the relation's size field to reflect the join.
 *
 * Returns nothing of interest.
 *
 */
void
geqo_rel_paths(RelOptInfo * rel)
{
	List	   *y = NIL;
	Path	   *path = (Path *) NULL;
	JoinPath   *cheapest = (JoinPath *) NULL;

	rel->size = 0;
	foreach(y, rel->pathlist)
	{
		path = (Path *) lfirst(y);

		if (!path->p_ordering.ord.sortop)
			break;
	}

	cheapest = (JoinPath *) set_paths(rel, path);
	if (IsA_JoinPath(cheapest))
		rel->size = compute_joinrel_size(cheapest);
}


/*
 * set-path--
 *	  Compares the unordered path for a relation with the cheapest path. If
 *	  the unordered path is not cheapest, it is pruned.
 *
 *	  Resets the pointers in 'rel' for unordered and cheapest paths.
 *
 * Returns the cheapest path.
 *
 */
static Path *
set_paths(RelOptInfo * rel, Path *unorderedpath)
{
	Path	   *cheapest = set_cheapest(rel, rel->pathlist);

	/* don't prune if not pruneable  -- JMH, 11/23/92 */
	if (unorderedpath != cheapest
		&& rel->pruneable)
	{

		rel->unorderedpath = (Path *) NULL;
		rel->pathlist = lremove(unorderedpath, rel->pathlist);
	}
	else
		rel->unorderedpath = (Path *) unorderedpath;

	return cheapest;
}
