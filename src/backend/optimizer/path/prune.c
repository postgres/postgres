/*-------------------------------------------------------------------------
 *
 * prune.c
 *	  Routines to prune redundant paths and relations
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/path/Attic/prune.c,v 1.37 1999/02/18 00:49:21 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "nodes/relation.h"

#include "optimizer/internal.h"
#include "optimizer/cost.h"
#include "optimizer/paths.h"
#include "optimizer/pathnode.h"

#include "utils/elog.h"


static List *merge_rel_with_same_relids(RelOptInfo *rel, Relids unjoined_relids);

/*
 * merge_rels_with_same_relids
 *	  Removes any redundant relation entries from a list of rel nodes
 *	  'rel_list'.  Obviously, the first relation can't be a duplicate.
 *
 * Returns the resulting list.
 *
 */
void
merge_rels_with_same_relids(List *rel_list)
{
	List	   *i;

	/*
	 * rel_list can shorten while running as duplicate relations are
	 * deleted
	 */
	foreach(i, rel_list)
		lnext(i) = merge_rel_with_same_relids((RelOptInfo *) lfirst(i), lnext(i));
}

/*
 * merge_rel_with_same_relids
 *	  Prunes those relations from 'unjoined_relids' that are redundant with
 *	  'rel'.  A relation is redundant if it is built up of the same
 *	  relations as 'rel'.  Paths for the redundant relation are merged into
 *	  the pathlist of 'rel'.
 *
 * Returns a list of non-redundant relations, and sets the pathlist field
 * of 'rel' appropriately.
 *
 */
static List *
merge_rel_with_same_relids(RelOptInfo *rel, Relids unjoined_relids)
{
	List	   *i = NIL;
	List	   *result = NIL;

	foreach(i, unjoined_relids)
	{
		RelOptInfo *unjoined_rel = (RelOptInfo *) lfirst(i);

		if (same(rel->relids, unjoined_rel->relids))
			/*
			 *	This are on the same relations,
			 *	so get the best of their pathlists.
			 */
			rel->pathlist = add_pathlist(rel,
										 rel->pathlist,
										 unjoined_rel->pathlist);
		else
			result = lappend(result, unjoined_rel);
	}
	return result;
}

/*
 * rels_set_cheapest
 *	  For each relation entry in 'rel_list' (which corresponds to a join
 *	  relation), set pointers to the cheapest path
 */
void
rels_set_cheapest(List *rel_list)
{
	List	   *x = NIL;
	RelOptInfo *rel = (RelOptInfo *) NULL;
	JoinPath	*cheapest;

	foreach(x, rel_list)
	{
		rel = (RelOptInfo *) lfirst(x);

		cheapest = (JoinPath *) set_cheapest(rel, rel->pathlist);
		if (IsA_JoinPath(cheapest))
			rel->size = compute_joinrel_size(cheapest);
		else
			elog(ERROR, "non JoinPath called");
	}
}
