/*-------------------------------------------------------------------------
 *
 * ordering.h
 *	  prototypes for ordering.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: ordering.h,v 1.13 1999/02/13 23:21:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ORDERING_H
#define ORDERING_H

#include <nodes/relation.h>

extern bool pathorder_match(PathOrder *path_ordering1,
						 PathOrder *path_ordering2, int *better_sort);
extern bool equal_path_merge_ordering(Oid *path_ordering,
						  MergeOrder *merge_ordering);
extern bool equal_merge_ordering(MergeOrder *merge_ordering1,
						   MergeOrder *merge_ordering2);

#endif	 /* ORDERING_H */
