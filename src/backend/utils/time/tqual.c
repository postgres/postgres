/*-------------------------------------------------------------------------
 *
 * tqual.c--
 *	  POSTGRES "time" qualification code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/time/tqual.c,v 1.16 1998/06/15 19:29:58 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

/* #define TQUALDEBUG	1 */

#include "postgres.h"

#include "access/htup.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "access/transam.h"
#include "utils/elog.h"
#include "utils/palloc.h"
#include "utils/tqual.h"

extern bool PostgresIsInitialized;

/*
 * XXX Transaction system override hacks start here
 */
#ifndef GOODAMI

TransactionId HeapSpecialTransactionId = InvalidTransactionId;
CommandId HeapSpecialCommandId = FirstCommandId;

void
setheapoverride(bool on)
{
	if (on)
	{
		TransactionIdStore(GetCurrentTransactionId(),
						   &HeapSpecialTransactionId);
		HeapSpecialCommandId = GetCurrentCommandId();
	}
	else
		HeapSpecialTransactionId = InvalidTransactionId;
}

#endif							/* !defined(GOODAMI) */
/*
 * XXX Transaction system override hacks end here
 */

/*
 * HeapTupleSatisfiesItself --
 *		True iff heap tuple is valid for "itself."
 *		"{it}self" means valid as of everything that's happened
 *		in the current transaction, _including_ the current command.
 *
 * Note:
 *		Assumes heap tuple is valid.
 */
/*
 * The satisfaction of "itself" requires the following:
 *
 * ((Xmin == my-transaction &&				the row was updated by the current transaction, and
 *		(Xmax is null						it was not deleted
 *		 [|| Xmax != my-transaction)])			[or it was deleted by another transaction]
 * ||
 *
 * (Xmin is committed &&					the row was modified by a committed transaction, and
 *		(Xmax is null ||					the row has not been deleted, or
 *			(Xmax != my-transaction &&			the row was deleted by another transaction
 *			 Xmax is not committed)))			that has not been committed
 */
bool
HeapTupleSatisfiesItself(HeapTuple tuple)
{

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)		/* xid invalid or
														 * aborted */
			return (false);

		if (TransactionIdIsCurrentTransactionId(tuple->t_xmin))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return (true);
			else
				return (false);
		}

		if (!TransactionIdDidCommit(tuple->t_xmin))
		{
			if (TransactionIdDidAbort(tuple->t_xmin))
				tuple->t_infomask |= HEAP_XMIN_INVALID; /* aborted */
			return (false);
		}

		tuple->t_infomask |= HEAP_XMIN_COMMITTED;
	}
	/* the tuple was inserted validly */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return (true);

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
		return (false);

	if (TransactionIdIsCurrentTransactionId(tuple->t_xmax))
		return (false);

	if (!TransactionIdDidCommit(tuple->t_xmax))
	{
		if (TransactionIdDidAbort(tuple->t_xmax))
			tuple->t_infomask |= HEAP_XMAX_INVALID;		/* aborted */
		return (true);
	}

	/* by here, deleting transaction has committed */
	tuple->t_infomask |= HEAP_XMAX_COMMITTED;

	return (false);
}

/*
 * HeapTupleSatisfiesNow --
 *		True iff heap tuple is valid "now."
 *		"now" means valid including everything that's happened
 *		 in the current transaction _up to, but not including,_
 *		 the current command.
 *
 * Note:
 *		Assumes heap tuple is valid.
 */
/*
 * The satisfaction of "now" requires the following:
 *
 * ((Xmin == my-transaction &&				changed by the current transaction
 *	 Cmin != my-command &&					but not by this command, and
 *		(Xmax is null ||						the row has not been deleted, or
 *			(Xmax == my-transaction &&			it was deleted by the current transaction
 *			 Cmax != my-command)))				but not by this command,
 * ||										or
 *
 *	(Xmin is committed &&					the row was modified by a committed transaction, and
 *		(Xmax is null ||					the row has not been deleted, or
 *			(Xmax == my-transaction &&			the row is being deleted by this command, or
 *			 Cmax == my-command) ||
 *			(Xmax is not committed &&			the row was deleted by another transaction
 *			 Xmax != my-transaction))))			that has not been committed
 *
 * XXX
 *		CommandId stuff didn't work properly if one used SQL-functions in
 *		UPDATE/INSERT(fromSELECT)/DELETE scans: SQL-funcs call
 *		CommandCounterIncrement and made tuples changed/inserted by
 *		current command visible to command itself (so we had multiple
 *		update of updated tuples, etc).			- vadim 08/29/97
 *
 *		mao says 17 march 1993:  the tests in this routine are correct;
 *		if you think they're not, you're wrong, and you should think
 *		about it again.  i know, it happened to me.  we don't need to
 *		check commit time against the start time of this transaction
 *		because 2ph locking protects us from doing the wrong thing.
 *		if you mess around here, you'll break serializability.  the only
 *		problem with this code is that it does the wrong thing for system
 *		catalog updates, because the catalogs aren't subject to 2ph, so
 *		the serializability guarantees we provide don't extend to xacts
 *		that do catalog accesses.  this is unfortunate, but not critical.
 */
bool
HeapTupleSatisfiesNow(HeapTuple tuple)
{
	if (AMI_OVERRIDE)
		return true;

	/*
	 * If the transaction system isn't yet initialized, then we assume
	 * that transactions committed.  We only look at system catalogs
	 * during startup, so this is less awful than it seems, but it's still
	 * pretty awful.
	 */

	if (!PostgresIsInitialized)
		return ((bool) (TransactionIdIsValid(tuple->t_xmin) &&
						!TransactionIdIsValid(tuple->t_xmax)));

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)		/* xid invalid or
														 * aborted */
			return (false);

		if (TransactionIdIsCurrentTransactionId(tuple->t_xmin))
		{
			if (CommandIdGEScanCommandId(tuple->t_cmin))
				return (false); /* inserted after scan started */

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return (true);

			Assert(TransactionIdIsCurrentTransactionId(tuple->t_xmax));

			if (CommandIdGEScanCommandId(tuple->t_cmax))
				return (true);	/* deleted after scan started */
			else
				return (false); /* deleted before scan started */
		}

		/*
		 * this call is VERY expensive - requires a log table lookup.
		 */

		if (!TransactionIdDidCommit(tuple->t_xmin))
		{
			if (TransactionIdDidAbort(tuple->t_xmin))
				tuple->t_infomask |= HEAP_XMIN_INVALID; /* aborted */
			return (false);
		}

		tuple->t_infomask |= HEAP_XMIN_COMMITTED;
	}

	/* by here, the inserting transaction has committed */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return (true);

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
		return (false);

	if (TransactionIdIsCurrentTransactionId(tuple->t_xmax))
	{
		if (CommandIdGEScanCommandId(tuple->t_cmax))
			return (true);		/* deleted after scan started */
		else
			return (false);		/* deleted before scan started */
	}

	if (!TransactionIdDidCommit(tuple->t_xmax))
	{
		if (TransactionIdDidAbort(tuple->t_xmax))
			tuple->t_infomask |= HEAP_XMAX_INVALID;		/* aborted */
		return (true);
	}

	/* xmax transaction committed */
	tuple->t_infomask |= HEAP_XMAX_COMMITTED;

	return (false);
}
