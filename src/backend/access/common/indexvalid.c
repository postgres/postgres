/*-------------------------------------------------------------------------
 *
 * indexvalid.c
 *	  index tuple qualification validity checking code
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/Attic/indexvalid.c,v 1.26 2001/01/24 19:42:47 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/iqual.h"
#include "executor/execdebug.h"

/* ----------------------------------------------------------------
 *				  index scan key qualification code
 * ----------------------------------------------------------------
 */
int			NIndexTupleProcessed;

/* ----------------
 *		index_keytest
 *
 * old comments
 *		May eventually combine with other tests (like timeranges)?
 *		Should have Buffer buffer; as an argument and pass it to amgetattr.
 * ----------------
 */
bool
index_keytest(IndexTuple tuple,
			  TupleDesc tupdesc,
			  int scanKeySize,
			  ScanKey key)
{
	bool		isNull;
	Datum		datum;
	Datum		test;

	IncrIndexProcessed();

	while (scanKeySize > 0)
	{
		datum = index_getattr(tuple,
							  key[0].sk_attno,
							  tupdesc,
							  &isNull);

		if (isNull)
		{
			/* XXX eventually should check if SK_ISNULL */
			return false;
		}

		if (key[0].sk_flags & SK_ISNULL)
			return false;

		if (key[0].sk_flags & SK_COMMUTE)
		{
			test = FunctionCall2(&key[0].sk_func,
								 key[0].sk_argument, datum);
		}
		else
		{
			test = FunctionCall2(&key[0].sk_func,
								 datum, key[0].sk_argument);
		}

		if (DatumGetBool(test) == !!(key[0].sk_flags & SK_NEGATE))
			return false;

		scanKeySize -= 1;
		key++;
	}

	return true;
}
