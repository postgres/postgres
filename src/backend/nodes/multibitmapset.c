/*-------------------------------------------------------------------------
 *
 * multibitmapset.c
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
 * Copyright (c) 2022-2023, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/nodes/multibitmapset.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/multibitmapset.h"


/*
 * mbms_add_member
 *		Add a new member to a multibitmapset.
 *
 * The new member is identified by "listidx", the zero-based index of the
 * List element it should go into, and "bitidx", which specifies the bit
 * number to be set therein.
 *
 * This is like bms_add_member, but for multibitmapsets.
 */
List *
mbms_add_member(List *a, int listidx, int bitidx)
{
	Bitmapset  *bms;
	ListCell   *lc;

	if (listidx < 0 || bitidx < 0)
		elog(ERROR, "negative multibitmapset member index not allowed");
	/* Add empty elements as needed */
	while (list_length(a) <= listidx)
		a = lappend(a, NULL);
	/* Update the target element */
	lc = list_nth_cell(a, listidx);
	bms = lfirst_node(Bitmapset, lc);
	bms = bms_add_member(bms, bitidx);
	lfirst(lc) = bms;
	return a;
}

/*
 * mbms_add_members
 *		Add all members of set b to set a.
 *
 * This is a UNION operation, but the left input is modified in-place.
 *
 * This is like bms_add_members, but for multibitmapsets.
 */
List *
mbms_add_members(List *a, const List *b)
{
	ListCell   *lca,
			   *lcb;

	/* Add empty elements to a, as needed */
	while (list_length(a) < list_length(b))
		a = lappend(a, NULL);
	/* forboth will stop at the end of the shorter list, which is fine */
	forboth(lca, a, lcb, b)
	{
		Bitmapset  *bmsa = lfirst_node(Bitmapset, lca);
		const Bitmapset *bmsb = lfirst_node(Bitmapset, lcb);

		bmsa = bms_add_members(bmsa, bmsb);
		lfirst(lca) = bmsa;
	}
	return a;
}

/*
 * mbms_int_members
 *		Reduce set a to its intersection with set b.
 *
 * This is an INTERSECT operation, but the left input is modified in-place.
 *
 * This is like bms_int_members, but for multibitmapsets.
 */
List *
mbms_int_members(List *a, const List *b)
{
	ListCell   *lca,
			   *lcb;

	/* Remove any elements of a that are no longer of use */
	a = list_truncate(a, list_length(b));
	/* forboth will stop at the end of the shorter list, which is fine */
	forboth(lca, a, lcb, b)
	{
		Bitmapset  *bmsa = lfirst_node(Bitmapset, lca);
		const Bitmapset *bmsb = lfirst_node(Bitmapset, lcb);

		bmsa = bms_int_members(bmsa, bmsb);
		lfirst(lca) = bmsa;
	}
	return a;
}

/*
 * mbms_is_member
 *		Is listidx/bitidx a member of A?
 *
 * This is like bms_is_member, but for multibitmapsets.
 */
bool
mbms_is_member(int listidx, int bitidx, const List *a)
{
	const Bitmapset *bms;

	/* XXX better to just return false for negative indexes? */
	if (listidx < 0 || bitidx < 0)
		elog(ERROR, "negative multibitmapset member index not allowed");
	if (listidx >= list_length(a))
		return false;
	bms = list_nth_node(Bitmapset, a, listidx);
	return bms_is_member(bitidx, bms);
}

/*
 * mbms_overlap_sets
 *		Identify the bitmapsets having common members in a and b.
 *
 * The result is a bitmapset of the list indexes of bitmapsets that overlap.
 */
Bitmapset *
mbms_overlap_sets(const List *a, const List *b)
{
	Bitmapset  *result = NULL;
	ListCell   *lca,
			   *lcb;

	/* forboth will stop at the end of the shorter list, which is fine */
	forboth(lca, a, lcb, b)
	{
		const Bitmapset *bmsa = lfirst_node(Bitmapset, lca);
		const Bitmapset *bmsb = lfirst_node(Bitmapset, lcb);

		if (bms_overlap(bmsa, bmsb))
			result = bms_add_member(result, foreach_current_index(lca));
	}
	return result;
}
