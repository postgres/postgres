/*-------------------------------------------------------------------------
 *
 * ordering.h--
 *    prototypes for ordering.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: ordering.h,v 1.1.1.1 1996/07/09 06:21:34 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef ORDERING_H
#define ORDERING_H

extern bool equal_path_path_ordering(PathOrder *path_ordering1,
				     PathOrder *path_ordering2);
extern bool equal_path_merge_ordering(Oid *path_ordering,
				      MergeOrder *merge_ordering);
extern bool equal_merge_merge_ordering(MergeOrder *merge_ordering1,
				       MergeOrder *merge_ordering2);
extern bool equal_sortops_order(Oid *ordering1, Oid *ordering2);

#endif	/* ORDERING_H */
