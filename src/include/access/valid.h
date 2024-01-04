/*-------------------------------------------------------------------------
 *
 * valid.h
 *	  POSTGRES tuple qualification validity definitions.
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/valid.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef VALID_H
#define VALID_H

#include "access/htup.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/tupdesc.h"

/*
 *		HeapKeyTest
 *
 *		Test a heap tuple to see if it satisfies a scan key.
 */
static inline bool
HeapKeyTest(HeapTuple tuple, TupleDesc tupdesc, int nkeys, ScanKey keys)
{
	int			cur_nkeys = nkeys;
	ScanKey		cur_key = keys;

	for (; cur_nkeys--; cur_key++)
	{
		Datum		atp;
		bool		isnull;
		Datum		test;

		if (cur_key->sk_flags & SK_ISNULL)
			return false;

		atp = heap_getattr(tuple, cur_key->sk_attno, tupdesc, &isnull);

		if (isnull)
			return false;

		test = FunctionCall2Coll(&cur_key->sk_func,
								 cur_key->sk_collation,
								 atp, cur_key->sk_argument);

		if (!DatumGetBool(test))
			return false;
	}

	return true;
}

#endif							/* VALID_H */
