/*-------------------------------------------------------------------------
 *
 * relnode.c
 *	  Relation manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/relnode.c,v 1.20 2000/01/09 00:26:41 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"


#include "optimizer/internal.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"



/*
 * get_base_rel
 *	  Returns relation entry corresponding to 'relid', creating a new one if
 *	  necessary. This is for base relations.
 *
 */
RelOptInfo *
get_base_rel(Query *root, int relid)
{
	Relids		relids = lconsi(relid, NIL);
	RelOptInfo *rel;

	rel = rel_member(relids, root->base_rel_list);
	if (rel == NULL)
	{
		rel = makeNode(RelOptInfo);
		rel->relids = relids;
		rel->rows = 0;
		rel->width = 0;
		rel->targetlist = NIL;
		rel->pathlist = NIL;
		rel->cheapestpath = (Path *) NULL;
		rel->pruneable = true;
		rel->indexed = false;
		rel->pages = 0;
		rel->tuples = 0;
		rel->restrictinfo = NIL;
		rel->joininfo = NIL;
		rel->innerjoin = NIL;

		root->base_rel_list = lcons(rel, root->base_rel_list);

		if (relid < 0)
		{
			/*
			 * If the relation is a materialized relation, assume
			 * constants for sizes.
			 */
			rel->pages = _NONAME_RELATION_PAGES_;
			rel->tuples = _NONAME_RELATION_TUPLES_;
		}
		else
		{
			/*
			 * Otherwise, retrieve relation statistics from the
			 * system catalogs.
			 */
			relation_info(root, relid,
						  &rel->indexed, &rel->pages, &rel->tuples);
		}
	}
	return rel;
}

/*
 * get_join_rel
 *	  Returns relation entry corresponding to 'relid' (a list of relids),
 *	  or NULL.
 */
RelOptInfo *
get_join_rel(Query *root, Relids relid)
{
	return rel_member(relid, root->join_rel_list);
}

/*
 * rel_member
 *	  Determines whether a relation of id 'relid' is contained within a list
 *	  'rels'.
 *
 * Returns the corresponding entry in 'rels' if it is there.
 *
 */
RelOptInfo *
rel_member(Relids relids, List *rels)
{
	if (relids != NIL && rels != NIL)
	{
		List	   *temp;

		foreach(temp, rels)
		{
			RelOptInfo *rel = (RelOptInfo *) lfirst(temp);

			if (same(rel->relids, relids))
				return rel;
		}
	}
	return NULL;
}
