/*-------------------------------------------------------------------------
 *
 * transam.c
 *	  postgres transaction log interface routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/transam.c,v 1.55 2003/08/04 02:39:57 momjian Exp $
 *
 * NOTES
 *	  This file contains the high level access-method interface to the
 *	  transaction system.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/transam.h"


/* ----------------
 *		Flag indicating that we are bootstrapping.
 *
 * Transaction ID generation is disabled during bootstrap; we just use
 * BootstrapTransactionId.	Also, the transaction ID status-check routines
 * are short-circuited; they claim that BootstrapTransactionId has already
 * committed, allowing tuples already inserted to be seen immediately.
 * ----------------
 */
bool		AMI_OVERRIDE = false;


static bool TransactionLogTest(TransactionId transactionId, XidStatus status);
static void TransactionLogUpdate(TransactionId transactionId,
					 XidStatus status);

/* ----------------
 *		Single-item cache for results of TransactionLogTest.
 * ----------------
 */
static TransactionId cachedTestXid = InvalidTransactionId;
static XidStatus cachedTestXidStatus;


/* ----------------------------------------------------------------
 *		postgres log access method interface
 *
 *		TransactionLogTest
 *		TransactionLogUpdate
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		TransactionLogTest
 * --------------------------------
 */

static bool						/* true/false: does transaction id have
								 * specified status? */
TransactionLogTest(TransactionId transactionId, /* transaction id to test */
				   XidStatus status)	/* transaction status */
{
	XidStatus	xidstatus;		/* recorded status of xid */

	/*
	 * Before going to the commit log manager, check our single item cache
	 * to see if we didn't just check the transaction status a moment ago.
	 */
	if (TransactionIdEquals(transactionId, cachedTestXid))
		return (status == cachedTestXidStatus);

	/*
	 * Also, check to see if the transaction ID is a permanent one.
	 */
	if (!TransactionIdIsNormal(transactionId))
	{
		if (TransactionIdEquals(transactionId, BootstrapTransactionId))
			return (status == TRANSACTION_STATUS_COMMITTED);
		if (TransactionIdEquals(transactionId, FrozenTransactionId))
			return (status == TRANSACTION_STATUS_COMMITTED);
		return (status == TRANSACTION_STATUS_ABORTED);
	}

	/*
	 * Get the status.
	 */
	xidstatus = TransactionIdGetStatus(transactionId);

	/*
	 * DO NOT cache status for unfinished transactions!
	 */
	if (xidstatus != TRANSACTION_STATUS_IN_PROGRESS)
	{
		TransactionIdStore(transactionId, &cachedTestXid);
		cachedTestXidStatus = xidstatus;
	}

	return (status == xidstatus);
}

/* --------------------------------
 *		TransactionLogUpdate
 * --------------------------------
 */
static void
TransactionLogUpdate(TransactionId transactionId,		/* trans id to update */
					 XidStatus status)	/* new trans status */
{
	/*
	 * update the commit log
	 */
	TransactionIdSetStatus(transactionId, status);

	/*
	 * update (invalidate) our single item TransactionLogTest cache.
	 */
	TransactionIdStore(transactionId, &cachedTestXid);
	cachedTestXidStatus = status;
}

/* --------------------------------
 *		AmiTransactionOverride
 *
 *		This function is used to manipulate the bootstrap flag.
 * --------------------------------
 */
void
AmiTransactionOverride(bool flag)
{
	AMI_OVERRIDE = flag;
}

/* ----------------------------------------------------------------
 *						Interface functions
 *
 *		TransactionId DidCommit
 *		TransactionId DidAbort
 *		TransactionId IsInProgress
 *		========
 *		   these functions test the transaction status of
 *		   a specified transaction id.
 *
 *		TransactionId Commit
 *		TransactionId Abort
 *		========
 *		   these functions set the transaction status
 *		   of the specified xid.
 *
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		TransactionId DidCommit
 *		TransactionId DidAbort
 *		TransactionId IsInProgress
 * --------------------------------
 */

/*
 * TransactionIdDidCommit
 *		True iff transaction associated with the identifier did commit.
 *
 * Note:
 *		Assumes transaction identifier is valid.
 */
bool							/* true if given transaction committed */
TransactionIdDidCommit(TransactionId transactionId)
{
	if (AMI_OVERRIDE)
	{
		Assert(transactionId == BootstrapTransactionId);
		return true;
	}

	return TransactionLogTest(transactionId, TRANSACTION_STATUS_COMMITTED);
}

/*
 * TransactionIdDidAbort
 *		True iff transaction associated with the identifier did abort.
 *
 * Note:
 *		Assumes transaction identifier is valid.
 */
bool							/* true if given transaction aborted */
TransactionIdDidAbort(TransactionId transactionId)
{
	if (AMI_OVERRIDE)
	{
		Assert(transactionId == BootstrapTransactionId);
		return false;
	}

	return TransactionLogTest(transactionId, TRANSACTION_STATUS_ABORTED);
}

/*
 * Now this func in shmem.c and gives quality answer by scanning
 * PGPROC structures of all running backend. - vadim 11/26/96
 *
 * Old comments:
 * true if given transaction has neither committed nor aborted
 */
#ifdef NOT_USED
bool
TransactionIdIsInProgress(TransactionId transactionId)
{
	if (AMI_OVERRIDE)
	{
		Assert(transactionId == BootstrapTransactionId);
		return false;
	}

	return TransactionLogTest(transactionId, TRANSACTION_STATUS_IN_PROGRESS);
}
#endif   /* NOT_USED */

/* --------------------------------
 *		TransactionId Commit
 *		TransactionId Abort
 * --------------------------------
 */

/*
 * TransactionIdCommit
 *		Commits the transaction associated with the identifier.
 *
 * Note:
 *		Assumes transaction identifier is valid.
 */
void
TransactionIdCommit(TransactionId transactionId)
{
	TransactionLogUpdate(transactionId, TRANSACTION_STATUS_COMMITTED);
}

/*
 * TransactionIdAbort
 *		Aborts the transaction associated with the identifier.
 *
 * Note:
 *		Assumes transaction identifier is valid.
 */
void
TransactionIdAbort(TransactionId transactionId)
{
	TransactionLogUpdate(transactionId, TRANSACTION_STATUS_ABORTED);
}


/*
 * TransactionIdPrecedes --- is id1 logically < id2?
 */
bool
TransactionIdPrecedes(TransactionId id1, TransactionId id2)
{
	/*
	 * If either ID is a permanent XID then we can just do unsigned
	 * comparison.	If both are normal, do a modulo-2^31 comparison.
	 */
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 < id2);

	diff = (int32) (id1 - id2);
	return (diff < 0);
}

/*
 * TransactionIdPrecedesOrEquals --- is id1 logically <= id2?
 */
bool
TransactionIdPrecedesOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 <= id2);

	diff = (int32) (id1 - id2);
	return (diff <= 0);
}

/*
 * TransactionIdFollows --- is id1 logically > id2?
 */
bool
TransactionIdFollows(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 > id2);

	diff = (int32) (id1 - id2);
	return (diff > 0);
}

/*
 * TransactionIdFollowsOrEquals --- is id1 logically >= id2?
 */
bool
TransactionIdFollowsOrEquals(TransactionId id1, TransactionId id2)
{
	int32		diff;

	if (!TransactionIdIsNormal(id1) || !TransactionIdIsNormal(id2))
		return (id1 >= id2);

	diff = (int32) (id1 - id2);
	return (diff >= 0);
}
