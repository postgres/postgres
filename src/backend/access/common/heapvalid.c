/*-------------------------------------------------------------------------
 *
 * heapvalid.c--
 *	  heap tuple qualification validity checking code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/Attic/heapvalid.c,v 1.21 1997/09/22 03:58:32 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/valid.h>
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
