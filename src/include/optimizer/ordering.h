/*-------------------------------------------------------------------------
 *
 * ordering.h--
 *    prototypes for ordering.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: ordering.h,v 1.3 1997/08/19 21:39:29 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ORDERING_H
#define ORDERING_H

#include <nodes/relation.h>

extern bool equal_path_path_ordering(PathOrder *path_ordering1,
				     PathOrder *path_ordering2);
extern bool equal_path_merge_ordering(Oid *path_ordering,
				      MergeOrder *merge_ordering);
extern bool equal_merge_merge_ordering(MergeOrder *merge_ordering1,
				       MergeOrder *merge_ordering2);
#endif	/* ORDERING_H */
