/*-------------------------------------------------------------------------
 *
 * relnode.c
 *	  Relation manipulation routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/optimizer/util/relnode.c,v 1.14 1999/02/15 03:22:17 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/relation.h"

#include "optimizer/internal.h"
#include "optimizer/pathnode.h" /* where the decls go */
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
	List	   *relids;
	RelOptInfo *rel;

	relids = lconsi(relid, NIL);
	rel = rel_member(relids, root->base_rel_list);
	if (rel == NULL)
	{
		rel = makeNode(RelOptInfo);
		rel->relids = relids;
		rel->indexed = false;
		rel->pages = 0;
		rel->tuples = 0;
		rel->width = 0;
		rel->targetlist = NIL;
		rel->pathlist = NIL;
		rel->cheapestpath = (Path *) NULL;
		rel->pruneable = true;
		rel->classlist = NULL;
		rel->ordering = NULL;
		rel->relam = InvalidOid;
		rel->restrictinfo = NIL;
		rel->joininfo = NIL;
		rel->innerjoin = NIL;
		rel->superrels = NIL;

		root->base_rel_list = lcons(rel, root->base_rel_list);

		/*
		 * ??? the old lispy C code (get_rel) do a listp(relid) here but
		 * that can never happen since we already established relid is not
		 * a list.								-ay 10/94
		 */
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
			bool		hasindex;
			int			pages,
						tuples;

			/*
			 * Otherwise, retrieve relation characteristics from the
			 * system catalogs.
			 */
			relation_info(root, relid, &hasindex, &pages, &tuples);
			rel->indexed = hasindex;
			rel->pages = pages;
			rel->tuples = tuples;
		}
	}
	return rel;
}

/*
 * get_join_rel
 *	  Returns relation entry corresponding to 'relid' (a list of relids),
 *	  creating a new one if necessary. This is for join relations.
 *
 */
RelOptInfo *
get_join_rel(Query *root, List *relid)
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
rel_member(List *relid, List *rels)
{
	List	   *temp = NIL;
	List	   *temprelid = NIL;

	if (relid != NIL && rels != NIL)
	{
		foreach(temp, rels)
		{
			temprelid = ((RelOptInfo *) lfirst(temp))->relids;
			if (same(temprelid, relid))
				return (RelOptInfo *) (lfirst(temp));
		}
	}
	return NULL;
}
