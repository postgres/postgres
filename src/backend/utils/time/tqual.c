/*-------------------------------------------------------------------------
 *
 * tqual.c
 *	  POSTGRES "time" qualification code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/utils/time/tqual.c,v 1.25 1999/02/13 23:20:19 momjian Exp $
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

SnapshotData	SnapshotDirtyData;
Snapshot		SnapshotDirty = &SnapshotDirtyData;

Snapshot		QuerySnapshot = NULL;
Snapshot		SerializableSnapshot = NULL;

/*
 * XXX Transaction system override hacks start here
 */
#ifndef GOODAMI

TransactionId HeapSpecialTransactionId = InvalidTransactionId;
CommandId	HeapSpecialCommandId = FirstCommandId;

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

#endif	 /* !defined(GOODAMI) */
/*
 * XXX Transaction system override hacks end here
 */

/*
 * HeapTupleSatisfiesItself 
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
HeapTupleSatisfiesItself(HeapTupleHeader tuple)
{

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)		/* xid invalid or
														 * aborted */
			return false;

		if (TransactionIdIsCurrentTransactionId(tuple->t_xmin))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return true;
			if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
				return true;
			return false;
		}

		if (!TransactionIdDidCommit(tuple->t_xmin))
		{
			if (TransactionIdDidAbort(tuple->t_xmin))
				tuple->t_infomask |= HEAP_XMIN_INVALID; /* aborted */
			return false;
		}

		tuple->t_infomask |= HEAP_XMIN_COMMITTED;
	}
	/* the tuple was inserted validly */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
			return true;
		return false;							/* updated by other */
	}

	if (TransactionIdIsCurrentTransactionId(tuple->t_xmax))
	{
		if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
			return true;
		return false;
	}

	if (!TransactionIdDidCommit(tuple->t_xmax))
	{
		if (TransactionIdDidAbort(tuple->t_xmax))
			tuple->t_infomask |= HEAP_XMAX_INVALID;		/* aborted */
		return true;
	}

	/* by here, deleting transaction has committed */
	tuple->t_infomask |= HEAP_XMAX_COMMITTED;

	if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
		return true;

	return false;
}

/*
 * HeapTupleSatisfiesNow 
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
HeapTupleSatisfiesNow(HeapTupleHeader tuple)
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
			return false;

		if (TransactionIdIsCurrentTransactionId(tuple->t_xmin))
		{
			if (CommandIdGEScanCommandId(tuple->t_cmin))
				return false;	/* inserted after scan started */

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return true;

			Assert(TransactionIdIsCurrentTransactionId(tuple->t_xmax));

			if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
				return true;

			if (CommandIdGEScanCommandId(tuple->t_cmax))
				return true;	/* deleted after scan started */
			else
				return false;	/* deleted before scan started */
		}

		/*
		 * this call is VERY expensive - requires a log table lookup.
		 */

		if (!TransactionIdDidCommit(tuple->t_xmin))
		{
			if (TransactionIdDidAbort(tuple->t_xmin))
				tuple->t_infomask |= HEAP_XMIN_INVALID; /* aborted */
			return false;
		}

		tuple->t_infomask |= HEAP_XMIN_COMMITTED;
	}

	/* by here, the inserting transaction has committed */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
			return true;
		return false;
	}

	if (TransactionIdIsCurrentTransactionId(tuple->t_xmax))
	{
		if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
			return true;
		if (CommandIdGEScanCommandId(tuple->t_cmax))
			return true;		/* deleted after scan started */
		else
			return false;		/* deleted before scan started */
	}

	if (!TransactionIdDidCommit(tuple->t_xmax))
	{
		if (TransactionIdDidAbort(tuple->t_xmax))
			tuple->t_infomask |= HEAP_XMAX_INVALID;		/* aborted */
		return true;
	}

	/* xmax transaction committed */
	tuple->t_infomask |= HEAP_XMAX_COMMITTED;

	if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
		return true;

	return false;
}

int
HeapTupleSatisfiesUpdate(HeapTuple tuple)
{
	HeapTupleHeader	th = tuple->t_data;

	if (AMI_OVERRIDE)
		return HeapTupleMayBeUpdated;

	if (!(th->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (th->t_infomask & HEAP_XMIN_INVALID)	/* xid invalid or aborted */
			return HeapTupleInvisible;

		if (TransactionIdIsCurrentTransactionId(th->t_xmin))
		{
			if (CommandIdGEScanCommandId(th->t_cmin) && !heapisoverride())
				return HeapTupleInvisible;	/* inserted after scan started */

			if (th->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return HeapTupleMayBeUpdated;

			Assert(TransactionIdIsCurrentTransactionId(th->t_xmax));

			if (th->t_infomask & HEAP_MARKED_FOR_UPDATE)
				return HeapTupleMayBeUpdated;

			if (CommandIdGEScanCommandId(th->t_cmax))
				return HeapTupleSelfUpdated;/* updated after scan started */
			else
				return HeapTupleInvisible;	/* updated before scan started */
		}

		/*
		 * This call is VERY expensive - requires a log table lookup.
		 * Actually, this should be done by query before...
		 */

		if (!TransactionIdDidCommit(th->t_xmin))
		{
			if (TransactionIdDidAbort(th->t_xmin))
				th->t_infomask |= HEAP_XMIN_INVALID; /* aborted */
			return HeapTupleInvisible;
		}

		th->t_infomask |= HEAP_XMIN_COMMITTED;
	}

	/* by here, the inserting transaction has committed */

	if (th->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return HeapTupleMayBeUpdated;

	if (th->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (th->t_infomask & HEAP_MARKED_FOR_UPDATE)
			return HeapTupleMayBeUpdated;
		return HeapTupleUpdated;			/* updated by other */
	}

	if (TransactionIdIsCurrentTransactionId(th->t_xmax))
	{
		if (th->t_infomask & HEAP_MARKED_FOR_UPDATE)
			return HeapTupleMayBeUpdated;
		if (CommandIdGEScanCommandId(th->t_cmax))
			return HeapTupleSelfUpdated;/* updated after scan started */
		else
			return HeapTupleInvisible;	/* updated before scan started */
	}

	if (!TransactionIdDidCommit(th->t_xmax))
	{
		if (TransactionIdDidAbort(th->t_xmax))
		{
			th->t_infomask |= HEAP_XMAX_INVALID;		/* aborted */
			return HeapTupleMayBeUpdated;
		}
		/* running xact */
		return HeapTupleBeingUpdated;	/* in updation by other */
	}

	/* xmax transaction committed */
	th->t_infomask |= HEAP_XMAX_COMMITTED;

	if (th->t_infomask & HEAP_MARKED_FOR_UPDATE)
		return HeapTupleMayBeUpdated;

	return HeapTupleUpdated;			/* updated by other */
}

bool
HeapTupleSatisfiesDirty(HeapTupleHeader tuple)
{
	SnapshotDirty->xmin = SnapshotDirty->xmax = InvalidTransactionId;
	ItemPointerSetInvalid(&(SnapshotDirty->tid));

	if (AMI_OVERRIDE)
		return true;

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)	/* xid invalid or aborted */
			return false;

		if (TransactionIdIsCurrentTransactionId(tuple->t_xmin))
		{
			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return true;

			Assert(TransactionIdIsCurrentTransactionId(tuple->t_xmax));

			if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
				return true;

			return false;
		}

		if (!TransactionIdDidCommit(tuple->t_xmin))
		{
			if (TransactionIdDidAbort(tuple->t_xmin))
			{
				tuple->t_infomask |= HEAP_XMIN_INVALID; /* aborted */
				return false;
			}
			SnapshotDirty->xmin = tuple->t_xmin;
			return true;						/* in insertion by other */
		}

		tuple->t_infomask |= HEAP_XMIN_COMMITTED;
	}

	/* by here, the inserting transaction has committed */

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return true;

	if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
	{
		if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
			return true;
		SnapshotDirty->tid = tuple->t_ctid;
		return false;							/* updated by other */
	}

	if (TransactionIdIsCurrentTransactionId(tuple->t_xmax))
		return false;

	if (!TransactionIdDidCommit(tuple->t_xmax))
	{
		if (TransactionIdDidAbort(tuple->t_xmax))
		{
			tuple->t_infomask |= HEAP_XMAX_INVALID;		/* aborted */
			return true;
		}
		/* running xact */
		SnapshotDirty->xmax = tuple->t_xmax;
		return true;							/* in updation by other */
	}

	/* xmax transaction committed */
	tuple->t_infomask |= HEAP_XMAX_COMMITTED;

	if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
		return true;

	SnapshotDirty->tid = tuple->t_ctid;
	return false;								/* updated by other */
}

bool
HeapTupleSatisfiesSnapshot(HeapTupleHeader tuple, Snapshot snapshot)
{
	if (AMI_OVERRIDE)
		return true;

	if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
	{
		if (tuple->t_infomask & HEAP_XMIN_INVALID)		/* xid invalid or
														 * aborted */
			return false;

		if (TransactionIdIsCurrentTransactionId(tuple->t_xmin))
		{
			if (CommandIdGEScanCommandId(tuple->t_cmin))
				return false;	/* inserted after scan started */

			if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid */
				return true;

			Assert(TransactionIdIsCurrentTransactionId(tuple->t_xmax));

			if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
				return true;

			if (CommandIdGEScanCommandId(tuple->t_cmax))
				return true;	/* deleted after scan started */
			else
				return false;	/* deleted before scan started */
		}

		/*
		 * this call is VERY expensive - requires a log table lookup.
		 */

		if (!TransactionIdDidCommit(tuple->t_xmin))
		{
			if (TransactionIdDidAbort(tuple->t_xmin))
				tuple->t_infomask |= HEAP_XMIN_INVALID; /* aborted */
			return false;
		}

		tuple->t_infomask |= HEAP_XMIN_COMMITTED;
	}

	/* 
	 * By here, the inserting transaction has committed -
	 * have to check when...
	 */

	if (tuple->t_xmin >= snapshot->xmax)
		return false;
	if (tuple->t_xmin >= snapshot->xmin)
	{
		uint32	i;
		
		for (i = 0; i < snapshot->xcnt; i++)
		{
			if (tuple->t_xmin == snapshot->xip[i])
				return false;
		}
	}

	if (tuple->t_infomask & HEAP_XMAX_INVALID)	/* xid invalid or aborted */
		return true;

	if (tuple->t_infomask & HEAP_MARKED_FOR_UPDATE)
		return true;

	if (!(tuple->t_infomask & HEAP_XMAX_COMMITTED))
	{
		if (TransactionIdIsCurrentTransactionId(tuple->t_xmax))
		{
			if (CommandIdGEScanCommandId(tuple->t_cmax))
				return true;		/* deleted after scan started */
			else
				return false;		/* deleted before scan started */
		}

		if (!TransactionIdDidCommit(tuple->t_xmax))
		{
			if (TransactionIdDidAbort(tuple->t_xmax))
				tuple->t_infomask |= HEAP_XMAX_INVALID;		/* aborted */
			return true;
		}

		/* xmax transaction committed */
		tuple->t_infomask |= HEAP_XMAX_COMMITTED;
	}

	if (tuple->t_xmax >= snapshot->xmax)
		return true;
	if (tuple->t_xmax >= snapshot->xmin)
	{
		uint32	i;
		
		for (i = 0; i < snapshot->xcnt; i++)
		{
			if (tuple->t_xmax == snapshot->xip[i])
				return true;
		}
	}

	return false;
}

void
SetQuerySnapshot(void)
{

	/* 1st call in xaction */
	if (SerializableSnapshot == NULL)
	{
		SerializableSnapshot = GetSnapshotData(true);
		QuerySnapshot = SerializableSnapshot;
		Assert(QuerySnapshot != NULL);
		return;
	}

	if (QuerySnapshot != SerializableSnapshot)
	{
		free(QuerySnapshot->xip);
		free(QuerySnapshot);
	}

	if (XactIsoLevel == XACT_SERIALIZABLE)
		QuerySnapshot = SerializableSnapshot;
	else
		QuerySnapshot = GetSnapshotData(false);

	Assert(QuerySnapshot != NULL);

}

void
FreeXactSnapshot(void)
{

	if (QuerySnapshot != NULL && QuerySnapshot != SerializableSnapshot)
	{
		free(QuerySnapshot->xip);
		free(QuerySnapshot);
	}

	QuerySnapshot = NULL;

	if (SerializableSnapshot != NULL)
	{
		free(SerializableSnapshot->xip);
		free(SerializableSnapshot);
	}

	SerializableSnapshot = NULL;

}
