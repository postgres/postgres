/*-------------------------------------------------------------------------
 *
 * valid.h
 *	  POSTGRES tuple qualification validity definitions.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: valid.h,v 1.31 2003/09/25 18:58:35 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VALID_H
#define VALID_H

/* ----------------
 *		HeapKeyTest
 *
 *		Test a heap tuple with respect to a scan key.
 * ----------------
 */

#define HeapKeyTest(tuple, \
					tupdesc, \
					nkeys, \
					keys, \
					result) \
do \
{ \
/* We use underscores to protect the variable passed in as parameters */ \
/* We use two underscore here because this macro is included in the \
   macro below */ \
	bool		__isnull; \
	Datum		__atp; \
	Datum		__test; \
	int			__cur_nkeys = (nkeys); \
	ScanKey		__cur_keys = (keys); \
 \
	(result) = true; /* may change */ \
	for (; __cur_nkeys--; __cur_keys++) \
	{ \
		__atp = heap_getattr((tuple), \
							 __cur_keys->sk_attno, \
							 (tupdesc), \
							 &__isnull); \
 \
		if (__isnull) \
		{ \
			/* XXX eventually should check if SK_ISNULL */ \
			(result) = false; \
			break; \
		} \
 \
		if (__cur_keys->sk_flags & SK_ISNULL) \
		{ \
			(result) = false; \
			break; \
		} \
 \
		if (__cur_keys->sk_flags & SK_COMMUTE) \
			__test = FunctionCall2(&__cur_keys->sk_func, \
								   __cur_keys->sk_argument, __atp); \
		else \
			__test = FunctionCall2(&__cur_keys->sk_func, \
								   __atp, __cur_keys->sk_argument); \
 \
		if (DatumGetBool(__test) == !!(__cur_keys->sk_flags & SK_NEGATE)) \
		{ \
			/* XXX eventually should check if SK_ISNULL */ \
			(result) = false; \
			break; \
		} \
	} \
} while (0)

/* ----------------
 *		HeapTupleSatisfies
 *
 *	res is set TRUE if the HeapTuple satisfies the timequal and keytest,
 *	otherwise it is set FALSE.	Note that the hint bits in the HeapTuple's
 *	t_infomask may be updated as a side effect.
 *
 *	on 8/21/92 mao says:  i rearranged the tests here to do keytest before
 *	SatisfiesTimeQual.	profiling indicated that even for vacuumed relations,
 *	time qual checking was more expensive than key testing.  time qual is
 *	least likely to fail, too.	we should really add the time qual test to
 *	the restriction and optimize it in the normal way.	this has interactions
 *	with joey's expensive function work.
 * ----------------
 */
#define HeapTupleSatisfies(tuple, \
						   relation, \
						   buffer, \
						   disk_page, \
						   snapshot, \
						   nKeys, \
						   key, \
						   res) \
do \
{ \
/* We use underscores to protect the variable passed in as parameters */ \
	if ((key) != NULL) \
		HeapKeyTest(tuple, RelationGetDescr(relation), \
						   (nKeys), (key), (res)); \
	else \
		(res) = true; \
 \
	if (res) \
	{ \
		if ((relation)->rd_rel->relkind != RELKIND_UNCATALOGED) \
		{ \
			uint16	_infomask = (tuple)->t_data->t_infomask; \
			\
			(res) = HeapTupleSatisfiesVisibility((tuple), (snapshot)); \
			if ((tuple)->t_data->t_infomask != _infomask) \
				SetBufferCommitInfoNeedsSave(buffer); \
		} \
	} \
} while (0)

#endif   /* VALID_H */
