/*-------------------------------------------------------------------------
 *
 * valid.h
 *	  POSTGRES tuple qualification validity definitions.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/valid.h,v 1.36 2004/12/31 22:03:21 pgsql Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VALID_H
#define VALID_H

/*
 *		HeapKeyTest
 *
 *		Test a heap tuple to see if it satisfies a scan key.
 */
#define HeapKeyTest(tuple, \
					tupdesc, \
					nkeys, \
					keys, \
					result) \
do \
{ \
	/* Use underscores to protect the variables passed in as parameters */ \
	int			__cur_nkeys = (nkeys); \
	ScanKey		__cur_keys = (keys); \
 \
	(result) = true; /* may change */ \
	for (; __cur_nkeys--; __cur_keys++) \
	{ \
		Datum	__atp; \
		bool	__isnull; \
		Datum	__test; \
 \
		if (__cur_keys->sk_flags & SK_ISNULL) \
		{ \
			(result) = false; \
			break; \
		} \
 \
		__atp = heap_getattr((tuple), \
							 __cur_keys->sk_attno, \
							 (tupdesc), \
							 &__isnull); \
 \
		if (__isnull) \
		{ \
			(result) = false; \
			break; \
		} \
 \
		__test = FunctionCall2(&__cur_keys->sk_func, \
							   __atp, __cur_keys->sk_argument); \
 \
		if (!DatumGetBool(__test)) \
		{ \
			(result) = false; \
			break; \
		} \
	} \
} while (0)

/*
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
	if ((key) != NULL) \
		HeapKeyTest(tuple, RelationGetDescr(relation), nKeys, key, res); \
	else \
		(res) = true; \
 \
	if ((res) && (relation)->rd_rel->relkind != RELKIND_UNCATALOGED) \
		(res) = HeapTupleSatisfiesVisibility(tuple, snapshot, buffer); \
} while (0)

#endif   /* VALID_H */
