/*-------------------------------------------------------------------------
 *
 * heapvalid.c--
 *	  heap tuple qualification validity checking code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/Attic/heapvalid.c,v 1.20 1997/09/18 14:19:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/heapam.h>
#include <access/xact.h>

/*
 *	TupleUpdatedByCurXactAndCmd() -- Returns true if this tuple has
 *		already been updated once by the current transaction/command
 *		pair.
 */
bool
TupleUpdatedByCurXactAndCmd(HeapTuple t)
{
	if (TransactionIdEquals(t->t_xmax,
							GetCurrentTransactionId()) &&
		CommandIdGEScanCommandId(t->t_cmax))
		return true;

	return false;
}
