/*-------------------------------------------------------------------------
 *
 * geqo_paths.c
 *	  Routines to process redundant paths and relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: geqo_paths.c,v 1.21 1999/02/15 02:04:58 tgl Exp $
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


static List *geqo_prune_rel(RelOptInfo *rel, List *other_rels);

/*
 * geqo_prune_rels
 *	  Removes any redundant relation entries from a list of rel nodes
 *	  'rel_list'.
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
 * geqo_prune_rel
 *	  Prunes those relations from 'other_rels' that are redundant with
 *	  'rel'.  A relation is redundant if it is built up of the same
 *	  relations as 'rel'.  Paths for the redundant relation are merged into
 *	  the pathlist of 'rel'.
 *
 * Returns a list of non-redundant relations, and sets the pathlist field
 * of 'rel' appropriately.
 *
 */
static List *
geqo_prune_rel(RelOptInfo *rel, List *other_rels)
{
	List	   *t_list = NIL;
	List	   *i;
	RelOptInfo *other_rel;

	foreach(i, other_rels)
	{
		other_rel = (RelOptInfo *) lfirst(i);
		if (same(rel->relids, other_rel->relids))
		{
			rel->pathlist = add_pathlist(rel,
										 rel->pathlist,
										 other_rel->pathlist);
		}
		else
		{
			t_list = lappend(t_list, other_rel);
		}
	}
	return t_list;
}

/*
 * geqo_set_cheapest
 *	  For a relation 'rel' (which corresponds to a join
 *	  relation), set pointers to the cheapest path
 */
void
geqo_set_cheapest(RelOptInfo *rel)
{
	JoinPath *cheapest = (JoinPath *)set_cheapest(rel, rel->pathlist);

	if (IsA_JoinPath(cheapest))
		rel->size = compute_joinrel_size(cheapest);
	else
		rel->size = 0;
}
