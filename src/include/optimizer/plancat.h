/*-------------------------------------------------------------------------
 *
 * plancat.h
 *	  prototypes for plancat.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: plancat.h,v 1.26 2001/11/05 17:46:34 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PLANCAT_H
#define PLANCAT_H

#include "nodes/relation.h"


extern void get_relation_info(Oid relationObjectId,
				  bool *hasindex, long *pages, double *tuples);

extern List *find_secondary_indexes(Oid relationObjectId);

extern List *find_inheritance_children(Oid inhparent);

extern bool has_subclass(Oid relationId);

extern bool has_unique_index(RelOptInfo *rel, AttrNumber attno);

extern Selectivity restriction_selectivity(Query *root,
						Oid operator,
						List *args,
						int varRelid);

extern Selectivity join_selectivity(Query *root,
				 Oid operator,
				 List *args);

#endif   /* PLANCAT_H */
