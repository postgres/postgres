/*-------------------------------------------------------------------------
 *
 * transam.c
 *	  postgres transaction log interface routines
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/transam/transam.c,v 1.60 2004/08/29 04:12:23 momjian Exp $
 *
 * NOTES
 *	  This file contains the high level access-method interface to the
 *	  transaction system.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/clog.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "utils/tqual.h"


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


static XidStatus TransactionLogFetch(TransactionId transactionId);
static void TransactionLogUpdate(TransactionId transactionId,
					 XidStatus status);

/* ----------------
 *		Single-item cache for results of TransactionLogFetch.
 * ----------------
 */
static TransactionId cachedFetchXid = InvalidTransactionId;
static XidStatus cachedFetchXidStatus;


/* ----------------------------------------------------------------
 *		postgres log access method interface
 *
 *		TransactionLogFetch
 *		TransactionLogUpdate
 * ----------------------------------------------------------------
 */

/*
 * TransactionLogFetch --- fetch commit status of specified transaction id
 */
static XidStatus
TransactionLogFetch(TransactionId transactionId)
{
	XidStatus	xidstatus;

	/*
	 * Before going to the commit log manager, check our single item cache
	 * to see if we didn't just check the transaction status a moment ago.
	 */
	if (TransactionIdEquals(transactionId, cachedFetchXid))
		return cachedFetchXidStatus;

	/*
	 * Also, check to see if the transaction ID is a permanent one.
	 */
	if (!TransactionIdIsNormal(transactionId))
	{
		if (TransactionIdEquals(transactionId, BootstrapTransactionId))
			return TRANSACTION_STATUS_COMMITTED;
		if (TransactionIdEquals(transactionId, FrozenTransactionId))
			return TRANSACTION_STATUS_COMMITTED;
		return TRANSACTION_STATUS_ABORTED;
	}

	/*
	 * Get the status.
	 */
	xidstatus = TransactionIdGetStatus(transactionId);

	/*
	 * DO NOT cache status for unfinished or sub-committed transactions!
	 * We only cache status that is guaranteed not to change.
	 */
	if (xidstatus != TRANSACTION_STATUS_IN_PROGRESS &&
		xidstatus != TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionIdStore(transactionId, &cachedFetchXid);
		cachedFetchXidStatus = xidstatus;
	}

	return xidstatus;
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
}

/*
 * TransactionLogMultiUpdate
 *
 * Update multiple transaction identifiers to a given status.
 * Don't depend on this being atomic; it's not.
 */
static void
TransactionLogMultiUpdate(int nxids, TransactionId *xids, XidStatus status)
{
	int i;

	Assert(nxids != 0);

	for (i = 0; i < nxids; i++)
		TransactionIdSetStatus(xids[i], status);
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
	XidStatus	xidstatus;

	if (AMI_OVERRIDE)
	{
		Assert(transactionId == BootstrapTransactionId);
		return true;
	}

	xidstatus = TransactionLogFetch(transactionId);

	/*
	 * If it's marked committed, it's committed.
	 */
	if (xidstatus == TRANSACTION_STATUS_COMMITTED)
		return true;

	/*
	 * If it's marked subcommitted, we have to check the parent recursively.
	 * However, if it's older than RecentXmin, we can't look at pg_subtrans;
	 * instead assume that the parent crashed without cleaning up its children.
	 */
	if (xidstatus == TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionId parentXid;

		if (TransactionIdPrecedes(transactionId, RecentXmin))
			return false;
		parentXid = SubTransGetParent(transactionId);
		Assert(TransactionIdIsValid(parentXid));
		return TransactionIdDidCommit(parentXid);
	}

	/* 
	 * It's not committed.
	 */
	return false;
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
	XidStatus	xidstatus;

	if (AMI_OVERRIDE)
	{
		Assert(transactionId == BootstrapTransactionId);
		return false;
	}

	xidstatus = TransactionLogFetch(transactionId);

	/*
	 * If it's marked aborted, it's aborted.
	 */
	if (xidstatus == TRANSACTION_STATUS_ABORTED)
		return true;

	/*
	 * If it's marked subcommitted, we have to check the parent recursively.
	 * However, if it's older than RecentXmin, we can't look at pg_subtrans;
	 * instead assume that the parent crashed without cleaning up its children.
	 */
	if (xidstatus == TRANSACTION_STATUS_SUB_COMMITTED)
	{
		TransactionId parentXid;

		if (TransactionIdPrecedes(transactionId, RecentXmin))
			return true;
		parentXid = SubTransGetParent(transactionId);
		Assert(TransactionIdIsValid(parentXid));
		return TransactionIdDidAbort(parentXid);
	}

	/*
	 * It's not aborted.
	 */
	return false;
}

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
 * TransactionIdSubCommit
 *		Marks the subtransaction associated with the identifier as
 *		sub-committed.
 */
void
TransactionIdSubCommit(TransactionId transactionId)
{
	TransactionLogUpdate(transactionId, TRANSACTION_STATUS_SUB_COMMITTED);
}

/*
 * TransactionIdCommitTree
 *		Marks all the given transaction ids as committed.
 *
 * The caller has to be sure that this is used only to mark subcommitted
 * subtransactions as committed, and only *after* marking the toplevel
 * parent as committed.  Otherwise there is a race condition against
 * TransactionIdDidCommit.
 */
void
TransactionIdCommitTree(int nxids, TransactionId *xids)
{
	if (nxids > 0)
		TransactionLogMultiUpdate(nxids, xids, TRANSACTION_STATUS_COMMITTED);
}

/*
 * TransactionIdAbortTree
 *		Marks all the given transaction ids as aborted.
 *
 * We don't need to worry about the non-atomic behavior, since any onlookers
 * will consider all the xacts as not-yet-committed anyway.
 */
void
TransactionIdAbortTree(int nxids, TransactionId *xids)
{
	if (nxids > 0)
		TransactionLogMultiUpdate(nxids, xids, TRANSACTION_STATUS_ABORTED);
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
