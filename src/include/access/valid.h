/*-------------------------------------------------------------------------
 *
 * valid.h
 *	  POSTGRES tuple qualification validity definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: valid.h,v 1.17 1999/02/13 23:20:59 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef VALID_H
#define VALID_H

#include <fmgr.h>
#include <access/heapam.h>
#include <access/valid.h>
#include <storage/bufmgr.h>
#include <storage/bufpage.h>
#include <utils/rel.h>
#include <utils/builtins.h>
#include <utils/tqual.h>

/* ----------------
 *		extern decl's
 * ----------------
 */

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
	int			__test; \
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
		if (__cur_keys->sk_func.fn_addr == (func_ptr) oideq)	/* optimization */ \
			__test = (__cur_keys->sk_argument == __atp); \
		else if (__cur_keys->sk_flags & SK_COMMUTE) \
			__test = (long) FMGR_PTR2(&__cur_keys->sk_func, \
									__cur_keys->sk_argument, __atp); \
		else \
			__test = (long) FMGR_PTR2(&__cur_keys->sk_func, \
									__atp, __cur_keys->sk_argument); \
 \
		if (!__test == !(__cur_keys->sk_flags & SK_NEGATE)) \
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
 *	Returns a valid HeapTuple if it satisfies the timequal and keytest.
 *	Returns NULL otherwise.  Used to be heap_satisifies (sic) which
 *	returned a boolean.  It now returns a tuple so that we can avoid doing two
 *	PageGetItem's per tuple.
 *
 *		Complete check of validity including LP_CTUP and keytest.
 *		This should perhaps be combined with valid somehow in the
 *		future.  (Also, additional rule tests/time range tests.)
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
						   seeself, \
						   nKeys, \
						   key) \
do \
{ \
/* We use underscores to protect the variable passed in as parameters */ \
	bool		_res; \
 \
	if ((key) != NULL) \
		HeapKeyTest(tuple, RelationGetDescr(relation), \
						   (nKeys), (key), _res); \
	else \
		_res = TRUE; \
 \
	if (_res) \
	{ \
		if ((relation)->rd_rel->relkind != RELKIND_UNCATALOGED) \
		{ \
			uint16	_infomask = (tuple)->t_data->t_infomask; \
			\
			_res = HeapTupleSatisfiesVisibility((tuple), (seeself)); \
			if ((tuple)->t_data->t_infomask != _infomask) \
				SetBufferCommitInfoNeedsSave(buffer); \
			if (!_res) \
				(tuple)->t_data = NULL; \
		} \
	} \
	else \
	{ \
		(tuple)->t_data = NULL; \
	} \
} while (0)

extern bool TupleUpdatedByCurXactAndCmd(HeapTuple t);

#endif	 /* VALID_H */
