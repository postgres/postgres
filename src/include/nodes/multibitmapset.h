/*-------------------------------------------------------------------------
 *
 * multibitmapset.h
 *	  Lists of Bitmapsets
 *
 * A multibitmapset is useful in situations where members of a set can
 * be identified by two small integers; for example, varno and varattno
 * of a group of Vars within a query.  The implementation is a List of
 * Bitmapsets, so that the empty set can be represented by NIL.  (But,
 * as with Bitmapsets, that's not the only allowed representation.)
 * The zero-based index of a List element is the first identifying value,
 * and the (also zero-based) index of a bit within that Bitmapset is
 * the second identifying value.  There is no expectation that the
 * Bitmapsets should all be the same size.
 *
 * The available operations on multibitmapsets are intended to parallel
 * those on bitmapsets, for example union and intersection.  So far only
 * a small fraction of that has been built out; we'll add more as needed.
 *
 *
 * Copyright (c) 2022-2025, PostgreSQL Global Development Group
 *
 * src/include/nodes/multibitmapset.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MULTIBITMAPSET_H
#define MULTIBITMAPSET_H

#include "nodes/bitmapset.h"
#include "nodes/pg_list.h"

extern List *mbms_add_member(List *a, int listidx, int bitidx);
extern List *mbms_add_members(List *a, const List *b);
extern List *mbms_int_members(List *a, const List *b);
extern bool mbms_is_member(int listidx, int bitidx, const List *a);
extern Bitmapset *mbms_overlap_sets(const List *a, const List *b);

#endif							/* MULTIBITMAPSET_H */
