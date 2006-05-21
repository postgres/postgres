/*-------------------------------------------------------------------------
 *
 * xact.c
 *	  top level transaction system support routines
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/xact.c,v 1.156.2.3 2006/05/21 20:06:43 tgl Exp $
 *
 * NOTES
 *		Transaction aborts can now occur two ways:
 *
 *		1)	system dies from some internal cause  (syntax error, etc..)
 *		2)	user types ABORT
 *
 *		These two cases used to be treated identically, but now
 *		we need to distinguish them.  Why?	consider the following
 *		two situations:
 *
 *				case 1							case 2
 *				------							------
 *		1) user types BEGIN				1) user types BEGIN
 *		2) user does something			2) user does something
 *		3) user does not like what		3) system aborts for some reason
 *		   she sees and types ABORT
 *
 *		In case 1, we want to abort the transaction and return to the
 *		default state.	In case 2, there may be more commands coming
 *		our way which are part of the same transaction block and we have
 *		to ignore these commands until we see a COMMIT transaction or
 *		ROLLBACK.
 *
 *		Internal aborts are now handled by AbortTransactionBlock(), just as
 *		they always have been, and user aborts are now handled by
 *		UserAbortTransactionBlock().  Both of them rely on AbortTransaction()
 *		to do all the real work.  The only difference is what state we
 *		enter after AbortTransaction() does its work:
 *
 *		* AbortTransactionBlock() leaves us in TBLOCK_ABORT and
 *		* UserAbortTransactionBlock() leaves us in TBLOCK_ENDABORT
 *
 *		Low-level transaction abort handling is divided into two phases:
 *		* AbortTransaction() executes as soon as we realize the transaction
 *		  has failed.  It should release all shared resources (locks etc)
 *		  so that we do not delay other backends unnecessarily.
 *		* CleanupTransaction() executes when we finally see a user COMMIT
 *		  or ROLLBACK command; it cleans things up and gets us out of
 *		  the transaction internally.  In particular, we mustn't destroy
 *		  TopTransactionContext until this point.
 *
 *	 NOTES
 *		The essential aspects of the transaction system are:
 *
 *				o  transaction id generation
 *				o  transaction log updating
 *				o  memory cleanup
 *				o  cache invalidation
 *				o  lock cleanup
 *
 *		Hence, the functional division of the transaction code is
 *		based on which of the above things need to be done during
 *		a start/commit/abort transaction.  For instance, the
 *		routine AtCommit_Memory() takes care of all the memory
 *		cleanup stuff done at commit time.
 *
 *		The code is layered as follows:
 *
 *				StartTransaction
 *				CommitTransaction
 *				AbortTransaction
 *				CleanupTransaction
 *
 *		are provided to do the lower level work like recording
 *		the transaction status in the log and doing memory cleanup.
 *		above these routines are another set of functions:
 *
 *				StartTransactionCommand
 *				CommitTransactionCommand
 *				AbortCurrentTransaction
 *
 *		These are the routines used in the postgres main processing
 *		loop.  They are sensitive to the current transaction block state
 *		and make calls to the lower level routines appropriately.
 *
 *		Support for transaction blocks is provided via the functions:
 *
 *				StartTransactionBlock
 *				CommitTransactionBlock
 *				AbortTransactionBlock
 *
 *		These are invoked only in response to a user "BEGIN WORK", "COMMIT",
 *		or "ROLLBACK" command.	The tricky part about these functions
 *		is that they are called within the postgres main loop, in between
 *		the StartTransactionCommand() and CommitTransactionCommand().
 *
 *		For example, consider the following sequence of user commands:
 *
 *		1)		begin
 *		2)		select * from foo
 *		3)		insert into foo (bar = baz)
 *		4)		commit
 *
 *		in the main processing loop, this results in the following
 *		transaction sequence:
 *
 *			/	StartTransactionCommand();
 *		1) /	ProcessUtility();				<< begin
 *		   \		StartTransactionBlock();
 *			\	CommitTransactionCommand();
 *
 *			/	StartTransactionCommand();
 *		2) <	ProcessQuery();					<< select * from foo
 *			\	CommitTransactionCommand();
 *
 *			/	StartTransactionCommand();
 *		3) <	ProcessQuery();					<< insert into foo (bar = baz)
 *			\	CommitTransactionCommand();
 *
 *			/	StartTransactionCommand();
 *		4) /	ProcessUtility();				<< commit
 *		   \		CommitTransactionBlock();
 *			\	CommitTransactionCommand();
 *
 *		The point of this example is to demonstrate the need for
 *		StartTransactionCommand() and CommitTransactionCommand() to
 *		be state smart -- they should do nothing in between the calls
 *		to StartTransactionBlock() and EndTransactionBlock() and
 *		outside these calls they need to do normal start/commit
 *		processing.
 *
 *		Furthermore, suppose the "select * from foo" caused an abort
 *		condition.	We would then want to abort the transaction and
 *		ignore all subsequent commands up to the "commit".
 *		-cim 3/23/90
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/time.h>

#include "access/gistscan.h"
#include "access/hash.h"
#include "access/nbtree.h"
#include "access/rtree.h"
#include "access/xact.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/namespace.h"
#include "commands/async.h"
#include "commands/tablecmds.h"
#include "commands/trigger.h"
#include "commands/user.h"
#include "executor/spi.h"
#include "libpq/be-fsstubs.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/catcache.h"
#include "utils/relcache.h"
#include "pgstat.h"


static void AbortTransaction(void);
static void AtAbort_Cache(void);
static void AtAbort_Locks(void);
static void AtAbort_Memory(void);
static void AtCleanup_Memory(void);
static void AtCommit_Cache(void);
static void AtCommit_LocalCache(void);
static void AtCommit_Locks(void);
static void AtCommit_Memory(void);
static void AtStart_Cache(void);
static void AtStart_Locks(void);
static void AtStart_Memory(void);
static void CallEOXactCallbacks(bool isCommit);
static void CleanupTransaction(void);
static void CommitTransaction(void);
static void RecordTransactionAbort(void);
static void StartTransaction(void);

/*
 *	global variables holding the current transaction state.
 */
static TransactionStateData CurrentTransactionStateData = {
	0,							/* transaction id */
	FirstCommandId,				/* command id */
	0,							/* scan command id */
	0x0,						/* start time */
	TRANS_DEFAULT,				/* transaction state */
	TBLOCK_DEFAULT				/* transaction block state from the client
								 * perspective */
};

static TransactionState CurrentTransactionState = &CurrentTransactionStateData;

/*
 *	User-tweakable parameters
 */
int			DefaultXactIsoLevel = XACT_READ_COMMITTED;
int			XactIsoLevel;

bool		DefaultXactReadOnly = false;
bool		XactReadOnly;

int			CommitDelay = 0;	/* precommit delay in microseconds */
int			CommitSiblings = 5; /* number of concurrent xacts needed to
								 * sleep */


/*
 * List of add-on end-of-xact callbacks
 */
typedef struct EOXactCallbackItem
{
	struct EOXactCallbackItem *next;
	EOXactCallback callback;
	void	   *arg;
} EOXactCallbackItem;

static EOXactCallbackItem *EOXact_callbacks = NULL;

static void (*_RollbackFunc) (void *) = NULL;
static void *_RollbackData = NULL;


/* ----------------------------------------------------------------
 *	transaction state accessors
 * ----------------------------------------------------------------
 */

#ifdef NOT_USED

/* --------------------------------
 *	TransactionFlushEnabled()
 *	SetTransactionFlushEnabled()
 *
 *	These are used to test and set the "TransactionFlushState"
 *	variable.  If this variable is true (the default), then
 *	the system will flush all dirty buffers to disk at the end
 *	of each transaction.   If false then we are assuming the
 *	buffer pool resides in stable main memory, in which case we
 *	only do writes as necessary.
 * --------------------------------
 */
static int	TransactionFlushState = 1;

int
TransactionFlushEnabled(void)
{
	return TransactionFlushState;
}

void
SetTransactionFlushEnabled(bool state)
{
	TransactionFlushState = (state == true);
}
#endif


/*
 *	IsTransactionState
 *
 *	This returns true if we are currently running a query
 *	within an executing transaction.
 */
bool
IsTransactionState(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->state)
	{
		case TRANS_DEFAULT:
			return false;
		case TRANS_START:
			return true;
		case TRANS_INPROGRESS:
			return true;
		case TRANS_COMMIT:
			return true;
		case TRANS_ABORT:
			return true;
	}

	/*
	 * Shouldn't get here, but lint is not happy with this...
	 */
	return false;
}

/*
 *	IsAbortedTransactionBlockState
 *
 *	This returns true if we are currently running a query
 *	within an aborted transaction block.
 */
bool
IsAbortedTransactionBlockState(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_ABORT)
		return true;

	return false;
}


/*
 *	GetCurrentTransactionId
 */
TransactionId
GetCurrentTransactionId(void)
{
	TransactionState s = CurrentTransactionState;

	return s->transactionIdData;
}


/*
 *	GetCurrentCommandId
 */
CommandId
GetCurrentCommandId(void)
{
	TransactionState s = CurrentTransactionState;

	return s->commandId;
}


/*
 *	GetCurrentTransactionStartTime
 */
AbsoluteTime
GetCurrentTransactionStartTime(void)
{
	TransactionState s = CurrentTransactionState;

	return s->startTime;
}


/*
 *	GetCurrentTransactionStartTimeUsec
 */
AbsoluteTime
GetCurrentTransactionStartTimeUsec(int *msec)
{
	TransactionState s = CurrentTransactionState;

	*msec = s->startTimeUsec;

	return s->startTime;
}


/*
 *	TransactionIdIsCurrentTransactionId
 *
 *	During bootstrap, we cheat and say "it's not my transaction ID" even though
 *	it is.	Along with transam.c's cheat to say that the bootstrap XID is
 *	already committed, this causes the tqual.c routines to see previously
 *	inserted tuples as committed, which is what we need during bootstrap.
 */
bool
TransactionIdIsCurrentTransactionId(TransactionId xid)
{
	TransactionState s = CurrentTransactionState;

	if (AMI_OVERRIDE)
	{
		Assert(xid == BootstrapTransactionId);
		return false;
	}

	return TransactionIdEquals(xid, s->transactionIdData);
}


/*
 *	CommandIdIsCurrentCommandId
 */
bool
CommandIdIsCurrentCommandId(CommandId cid)
{
	TransactionState s = CurrentTransactionState;

	return (cid == s->commandId) ? true : false;
}


/*
 *	CommandCounterIncrement
 */
void
CommandCounterIncrement(void)
{
	TransactionState s = CurrentTransactionState;

	s->commandId += 1;
	if (s->commandId == FirstCommandId) /* check for overflow */
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cannot have more than 2^32-1 commands in a transaction")));

	/* Propagate new command ID into query snapshots, if set */
	if (QuerySnapshot)
		QuerySnapshot->curcid = s->commandId;
	if (SerializableSnapshot)
		SerializableSnapshot->curcid = s->commandId;

	/*
	 * make cache changes visible to me.  AtCommit_LocalCache() instead of
	 * AtCommit_Cache() is called here.
	 */
	AtCommit_LocalCache();
	AtStart_Cache();
}


/* ----------------------------------------------------------------
 *						StartTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 *	AtStart_Cache
 */
static void
AtStart_Cache(void)
{
	AcceptInvalidationMessages();
}

/*
 *		AtStart_Locks
 */
static void
AtStart_Locks(void)
{
	/*
	 * at present, it is unknown to me what belongs here -cim 3/18/90
	 *
	 * There isn't anything to do at the start of a xact for locks. -mer
	 * 5/24/92
	 */
}

/*
 *	AtStart_Memory
 */
static void
AtStart_Memory(void)
{
	/*
	 * We shouldn't have a transaction context already.
	 */
	Assert(TopTransactionContext == NULL);

	/*
	 * Create a toplevel context for the transaction, and make it active.
	 */
	TopTransactionContext =
		AllocSetContextCreate(TopMemoryContext,
							  "TopTransactionContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	MemoryContextSwitchTo(TopTransactionContext);
}


/* ----------------------------------------------------------------
 *						CommitTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 *	RecordTransactionCommit
 */
void
RecordTransactionCommit(void)
{
	/*
	 * If we made neither any XLOG entries nor any temp-rel updates, we
	 * can omit recording the transaction commit at all.
	 */
	if (MyXactMadeXLogEntry || MyXactMadeTempRelUpdate)
	{
		TransactionId xid = GetCurrentTransactionId();
		bool		madeTCentries;
		XLogRecPtr	recptr;

		/* Tell bufmgr and smgr to prepare for commit */
		BufmgrCommit();

		START_CRIT_SECTION();

		/*
		 * If our transaction made any transaction-controlled XLOG entries,
		 * we need to lock out checkpoint start between writing our XLOG
		 * record and updating pg_clog.  Otherwise it is possible for the
		 * checkpoint to set REDO after the XLOG record but fail to flush the
		 * pg_clog update to disk, leading to loss of the transaction commit
		 * if we crash a little later.  Slightly klugy fix for problem
		 * discovered 2004-08-10.
		 *
		 * (If it made no transaction-controlled XLOG entries, its XID
		 * appears nowhere in permanent storage, so no one else will ever care
		 * if it committed; so it doesn't matter if we lose the commit flag.)
		 *
		 * Note we only need a shared lock.
		 */
		madeTCentries = (MyLastRecPtr.xrecoff != 0);
		if (madeTCentries)
			LWLockAcquire(CheckpointStartLock, LW_SHARED);

		/*
		 * We only need to log the commit in XLOG if the transaction made
		 * any transaction-controlled XLOG entries.
		 */
		if (madeTCentries)
		{
			/* Need to emit a commit record */
			XLogRecData rdata;
			xl_xact_commit xlrec;

			xlrec.xtime = time(NULL);
			rdata.buffer = InvalidBuffer;
			rdata.data = (char *) (&xlrec);
			rdata.len = SizeOfXactCommit;
			rdata.next = NULL;

			/*
			 * XXX SHOULD SAVE ARRAY OF RELFILENODE-s TO DROP
			 */
			recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_COMMIT, &rdata);
		}
		else
		{
			/* Just flush through last record written by me */
			recptr = ProcLastRecEnd;
		}

		/*
		 * We must flush our XLOG entries to disk if we made any XLOG
		 * entries, whether in or out of transaction control.  For
		 * example, if we reported a nextval() result to the client, this
		 * ensures that any XLOG record generated by nextval will hit the
		 * disk before we report the transaction committed.
		 */
		if (MyXactMadeXLogEntry)
		{
			/*
			 * Sleep before flush! So we can flush more than one commit
			 * records per single fsync.  (The idea is some other backend
			 * may do the XLogFlush while we're sleeping.  This needs work
			 * still, because on most Unixen, the minimum select() delay
			 * is 10msec or more, which is way too long.)
			 *
			 * We do not sleep if enableFsync is not turned on, nor if there
			 * are fewer than CommitSiblings other backends with active
			 * transactions.
			 */
			if (CommitDelay > 0 && enableFsync &&
				CountActiveBackends() >= CommitSiblings)
			{
				struct timeval delay;

				delay.tv_sec = 0;
				delay.tv_usec = CommitDelay;
				(void) select(0, NULL, NULL, NULL, &delay);
			}

			XLogFlush(recptr);
		}

		/*
		 * We must mark the transaction committed in clog if its XID
		 * appears either in permanent rels or in local temporary rels. We
		 * test this by seeing if we made transaction-controlled entries
		 * *OR* local-rel tuple updates.  Note that if we made only the
		 * latter, we have not emitted an XLOG record for our commit, and
		 * so in the event of a crash the clog update might be lost.  This
		 * is okay because no one else will ever care whether we
		 * committed.
		 */
		if (MyLastRecPtr.xrecoff != 0 || MyXactMadeTempRelUpdate)
			TransactionIdCommit(xid);

		/* Unlock checkpoint lock if we acquired it */
		if (madeTCentries)
			LWLockRelease(CheckpointStartLock);

		END_CRIT_SECTION();
	}

	/* Break the chain of back-links in the XLOG records I output */
	MyLastRecPtr.xrecoff = 0;
	MyXactMadeXLogEntry = false;
	MyXactMadeTempRelUpdate = false;

	/* Show myself as out of the transaction in PGPROC array */
	MyProc->logRec.xrecoff = 0;
}


/*
 *	AtCommit_Cache
 */
static void
AtCommit_Cache(void)
{
	/*
	 * Clean up the relation cache.
	 */
	AtEOXact_RelationCache(true);

	/*
	 * Make catalog changes visible to all backends.
	 */
	AtEOXactInvalidationMessages(true);
}

/*
 *	AtCommit_LocalCache
 */
static void
AtCommit_LocalCache(void)
{
	/*
	 * Make catalog changes visible to me for the next command.
	 */
	CommandEndInvalidationMessages(true);
}

/*
 *	AtCommit_Locks
 */
static void
AtCommit_Locks(void)
{
	/*
	 * XXX What if ProcReleaseLocks fails?	(race condition?)
	 *
	 * Then you're up a creek! -mer 5/24/92
	 */
	ProcReleaseLocks(true);
}

/*
 *	AtCommit_Memory
 */
static void
AtCommit_Memory(void)
{
	/*
	 * Now that we're "out" of a transaction, have the system allocate
	 * things in the top memory context instead of per-transaction
	 * contexts.
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Release all transaction-local memory.
	 */
	Assert(TopTransactionContext != NULL);
	MemoryContextDelete(TopTransactionContext);
	TopTransactionContext = NULL;
}

/* ----------------------------------------------------------------
 *						AbortTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 *	RecordTransactionAbort
 */
static void
RecordTransactionAbort(void)
{
	/*
	 * If we made neither any transaction-controlled XLOG entries nor any
	 * temp-rel updates, we can omit recording the transaction abort at
	 * all. No one will ever care that it aborted.
	 */
	if (MyLastRecPtr.xrecoff != 0 || MyXactMadeTempRelUpdate)
	{
		TransactionId xid = GetCurrentTransactionId();

		/*
		 * Catch the scenario where we aborted partway through
		 * RecordTransactionCommit ...
		 */
		if (TransactionIdDidCommit(xid))
			elog(PANIC, "cannot abort transaction %u, it was already committed", xid);

		START_CRIT_SECTION();

		/*
		 * We only need to log the abort in XLOG if the transaction made
		 * any transaction-controlled XLOG entries.  (Otherwise, its XID
		 * appears nowhere in permanent storage, so no one else will ever
		 * care if it committed.)  We do not flush XLOG to disk in any
		 * case, since the default assumption after a crash would be that
		 * we aborted, anyway.
		 * For the same reason, we don't need to worry about interlocking
		 * against checkpoint start.
		 */
		if (MyLastRecPtr.xrecoff != 0)
		{
			XLogRecData rdata;
			xl_xact_abort xlrec;
			XLogRecPtr	recptr;

			xlrec.xtime = time(NULL);
			rdata.buffer = InvalidBuffer;
			rdata.data = (char *) (&xlrec);
			rdata.len = SizeOfXactAbort;
			rdata.next = NULL;

			/*
			 * SHOULD SAVE ARRAY OF RELFILENODE-s TO DROP
			 */
			recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_ABORT, &rdata);
		}

		/*
		 * Mark the transaction aborted in clog.  This is not absolutely
		 * necessary but we may as well do it while we are here.
		 */
		TransactionIdAbort(xid);

		END_CRIT_SECTION();
	}

	/* Break the chain of back-links in the XLOG records I output */
	MyLastRecPtr.xrecoff = 0;
	MyXactMadeXLogEntry = false;
	MyXactMadeTempRelUpdate = false;

	/* Show myself as out of the transaction in PGPROC array */
	MyProc->logRec.xrecoff = 0;
}

/*
 *	AtAbort_Cache
 */
static void
AtAbort_Cache(void)
{
	AtEOXact_RelationCache(false);
	AtEOXactInvalidationMessages(false);
}

/*
 *	AtAbort_Locks
 */
static void
AtAbort_Locks(void)
{
	/*
	 * XXX What if ProcReleaseLocks() fails?  (race condition?)
	 *
	 * Then you're up a creek without a paddle! -mer
	 */
	ProcReleaseLocks(false);
}


/*
 *	AtAbort_Memory
 */
static void
AtAbort_Memory(void)
{
	/*
	 * Make sure we are in a valid context (not a child of
	 * TopTransactionContext...).  Note that it is possible for this code
	 * to be called when we aren't in a transaction at all; go directly to
	 * TopMemoryContext in that case.
	 */
	if (TopTransactionContext != NULL)
	{
		MemoryContextSwitchTo(TopTransactionContext);

		/*
		 * We do not want to destroy the transaction's global state yet,
		 * so we can't free any memory here.
		 */
	}
	else
		MemoryContextSwitchTo(TopMemoryContext);
}


/* ----------------------------------------------------------------
 *						CleanupTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 *	AtCleanup_Memory
 */
static void
AtCleanup_Memory(void)
{
	/*
	 * Now that we're "out" of a transaction, have the system allocate
	 * things in the top memory context instead of per-transaction
	 * contexts.
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Release all transaction-local memory.
	 */
	if (TopTransactionContext != NULL)
		MemoryContextDelete(TopTransactionContext);
	TopTransactionContext = NULL;
}


/* ----------------------------------------------------------------
 *						interface routines
 * ----------------------------------------------------------------
 */

/*
 *	StartTransaction
 */
static void
StartTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * check the current transaction state
	 */
	if (s->state != TRANS_DEFAULT)
		elog(WARNING, "StartTransaction and not in default state");

	/*
	 * set the current transaction state information appropriately during
	 * start processing
	 */
	s->state = TRANS_START;

	/*
	 * Make sure we've freed any old snapshot, and reset xact state variables
	 */
	FreeXactSnapshot();
	XactIsoLevel = DefaultXactIsoLevel;
	XactReadOnly = DefaultXactReadOnly;

	/*
	 * generate a new transaction id
	 */
	s->transactionIdData = GetNewTransactionId();

	XactLockTableInsert(s->transactionIdData);

	/*
	 * initialize current transaction state fields
	 */
	s->commandId = FirstCommandId;
	s->startTime = GetCurrentAbsoluteTimeUsec(&(s->startTimeUsec));

	/*
	 * initialize the various transaction subsystems
	 */
	AtStart_Memory();
	AtStart_Cache();
	AtStart_Locks();

	/*
	 * Tell the trigger manager to we're starting a transaction
	 */
	DeferredTriggerBeginXact();

	/*
	 * done with start processing, set current transaction state to "in
	 * progress"
	 */
	s->state = TRANS_INPROGRESS;

}

/*
 *	CommitTransaction
 */
static void
CommitTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * check the current transaction state
	 */
	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "CommitTransaction and not in in-progress state");

	/*
	 * Do pre-commit processing (most of this stuff requires database
	 * access, and in fact could still cause an error...)
	 */

	/*
	 * Tell the trigger manager that this transaction is about to be
	 * committed. He'll invoke all trigger deferred until XACT before we
	 * really start on committing the transaction.
	 */
	DeferredTriggerEndXact();

	/* Close open cursors */
	AtCommit_Portals();

	/*
	 * Let ON COMMIT management do its thing (must happen after closing
	 * cursors, to avoid dangling-reference problems)
	 */
	PreCommit_on_commit_actions();

	/* handle commit for large objects [ PA, 7/17/98 ] */
	/* XXX probably this does not belong here */
	lo_commit(true);

	/* NOTIFY commit must come before lower-level cleanup */
	AtCommit_Notify();

	/* Update the flat password file if we changed pg_shadow or pg_group */
	AtEOXact_UpdatePasswordFile(true);

	/* Prevent cancel/die interrupt while cleaning up */
	HOLD_INTERRUPTS();

	/*
	 * set the current transaction state information appropriately during
	 * the abort processing
	 */
	s->state = TRANS_COMMIT;

	/*
	 * Here is where we really truly commit.
	 */
	RecordTransactionCommit();

	/*
	 * Let others know about no transaction in progress by me. Note that
	 * this must be done _before_ releasing locks we hold and _after_
	 * RecordTransactionCommit.
	 *
	 * LWLockAcquire(SInvalLock) is required: UPDATE with xid 0 is blocked by
	 * xid 1' UPDATE, xid 1 is doing commit while xid 2 gets snapshot - if
	 * xid 2' GetSnapshotData sees xid 1 as running then it must see xid 0
	 * as running as well or it will see two tuple versions - one deleted
	 * by xid 1 and one inserted by xid 0.	See notes in GetSnapshotData.
	 */
	if (MyProc != (PGPROC *) NULL)
	{
		/* Lock SInvalLock because that's what GetSnapshotData uses. */
		LWLockAcquire(SInvalLock, LW_EXCLUSIVE);
		MyProc->xid = InvalidTransactionId;
		MyProc->xmin = InvalidTransactionId;
		LWLockRelease(SInvalLock);
	}

	/*
	 * This is all post-commit cleanup.  Note that if an error is raised
	 * here, it's too late to abort the transaction.  This should be just
	 * noncritical resource releasing.
	 *
	 * The ordering of operations is not entirely random.  The idea is:
	 * release resources visible to other backends (eg, files, buffer
	 * pins); then release locks; then release backend-local resources. We
	 * want to release locks at the point where any backend waiting for us
	 * will see our transaction as being fully cleaned up.
	 */

	smgrDoPendingDeletes(true);
	AtCommit_Cache();
	AtEOXact_Buffers(true);
	/* smgrcommit already done */

	AtCommit_Locks();

	CallEOXactCallbacks(true);
	AtEOXact_GUC(true);
	AtEOXact_SPI();
	AtEOXact_gist();
	AtEOXact_hash();
	AtEOXact_nbtree();
	AtEOXact_rtree();
	AtEOXact_on_commit_actions(true);
	AtEOXact_Namespace(true);
	AtEOXact_CatCache(true);
	AtEOXact_Files();
	pgstat_count_xact_commit();
	AtCommit_Memory();

	/*
	 * done with commit processing, set current transaction state back to
	 * default
	 */
	s->state = TRANS_DEFAULT;

	RESUME_INTERRUPTS();
}

/*
 *	AbortTransaction
 */
static void
AbortTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/* Prevent cancel/die interrupt while cleaning up */
	HOLD_INTERRUPTS();

	/*
	 * Release any LW locks we might be holding as quickly as possible.
	 * (Regular locks, however, must be held till we finish aborting.)
	 * Releasing LW locks is critical since we might try to grab them
	 * again while cleaning up!
	 */
	LWLockReleaseAll();

	/* Clean up buffer I/O and buffer context locks, too */
	AbortBufferIO();
	UnlockBuffers();

	/*
	 * Also clean up any open wait for lock, since the lock manager will
	 * choke if we try to wait for another lock before doing this.
	 */
	LockWaitCancel();

	/*
	 * check the current transaction state
	 *
	 * reduced to DEBUG2 because this is expected when rejecting an
	 * invalidly-encoded query outside a transaction block.  PG 8.0
	 * and up fix it better, but it's not worth back-porting those
	 * changes to 7.4.
	 */
	if (s->state != TRANS_INPROGRESS)
		elog(DEBUG2, "AbortTransaction and not in in-progress state");

	/*
	 * set the current transaction state information appropriately during
	 * the abort processing
	 */
	s->state = TRANS_ABORT;

	/* Make sure we are in a valid memory context */
	AtAbort_Memory();

	/*
	 * Reset user id which might have been changed transiently
	 */
	SetUserId(GetSessionUserId());

	/*
	 * do abort processing
	 */
	DeferredTriggerAbortXact();
	AtAbort_Portals();
	lo_commit(false);			/* 'false' means it's abort */
	AtAbort_Notify();
	AtEOXact_UpdatePasswordFile(false);

	/* Advertise the fact that we aborted in pg_clog. */
	RecordTransactionAbort();

	/*
	 * Let others know about no transaction in progress by me. Note that
	 * this must be done _before_ releasing locks we hold and _after_
	 * RecordTransactionAbort.
	 */
	if (MyProc != (PGPROC *) NULL)
	{
		/* Lock SInvalLock because that's what GetSnapshotData uses. */
		LWLockAcquire(SInvalLock, LW_EXCLUSIVE);
		MyProc->xid = InvalidTransactionId;
		MyProc->xmin = InvalidTransactionId;
		LWLockRelease(SInvalLock);
	}

	/*
	 * Post-abort cleanup.	See notes in CommitTransaction() concerning
	 * ordering.
	 */

	smgrDoPendingDeletes(false);
	AtAbort_Cache();
	AtEOXact_Buffers(false);
	smgrabort();

	AtAbort_Locks();

	CallEOXactCallbacks(false);
	AtEOXact_GUC(false);
	AtEOXact_SPI();
	AtEOXact_gist();
	AtEOXact_hash();
	AtEOXact_nbtree();
	AtEOXact_rtree();
	AtEOXact_on_commit_actions(false);
	AtEOXact_Namespace(false);
	AtEOXact_CatCache(false);
	AtEOXact_Files();
	SetReindexProcessing(InvalidOid, InvalidOid);
	pgstat_count_xact_rollback();

	/*
	 * State remains TRANS_ABORT until CleanupTransaction().
	 */
	RESUME_INTERRUPTS();
}

/*
 *	CleanupTransaction
 */
static void
CleanupTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * State should still be TRANS_ABORT from AbortTransaction().
	 */
	if (s->state != TRANS_ABORT)
		elog(FATAL, "CleanupTransaction and not in abort state");

	/*
	 * do abort cleanup processing
	 */
	AtCleanup_Portals();		/* now safe to release portal memory */
	AtCleanup_Memory();			/* and transaction memory */

	/*
	 * done with abort processing, set current transaction state back to
	 * default
	 */
	s->state = TRANS_DEFAULT;
}

/*
 *	StartTransactionCommand
 */
void
StartTransactionCommand(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * if we aren't in a transaction block, we just do our usual
			 * start transaction.
			 */
		case TBLOCK_DEFAULT:
			StartTransaction();
			break;

			/*
			 * We should never experience this -- if we do it means the
			 * BEGIN state was not changed in the previous
			 * CommitTransactionCommand().	If we get it, we print a
			 * warning and change to the in-progress state.
			 */
		case TBLOCK_BEGIN:
			elog(WARNING, "StartTransactionCommand: unexpected TBLOCK_BEGIN");
			s->blockState = TBLOCK_INPROGRESS;
			break;

			/*
			 * This is the case when are somewhere in a transaction block
			 * and about to start a new command.  For now we do nothing
			 * but someday we may do command-local resource
			 * initialization.
			 */
		case TBLOCK_INPROGRESS:
			break;

			/*
			 * As with BEGIN, we should never experience this if we do it
			 * means the END state was not changed in the previous
			 * CommitTransactionCommand().	If we get it, we print a
			 * warning, commit the transaction, start a new transaction
			 * and change to the default state.
			 */
		case TBLOCK_END:
			elog(WARNING, "StartTransactionCommand: unexpected TBLOCK_END");
			s->blockState = TBLOCK_DEFAULT;
			CommitTransaction();
			StartTransaction();
			break;

			/*
			 * Here we are in the middle of a transaction block but one of
			 * the commands caused an abort so we do nothing but remain in
			 * the abort state.  Eventually we will get to the "END
			 * TRANSACTION" which will set things straight.
			 */
		case TBLOCK_ABORT:
			break;

			/*
			 * This means we somehow aborted and the last call to
			 * CommitTransactionCommand() didn't clear the state so we
			 * remain in the ENDABORT state and maybe next time we get to
			 * CommitTransactionCommand() the state will get reset to
			 * default.
			 */
		case TBLOCK_ENDABORT:
			elog(WARNING, "StartTransactionCommand: unexpected TBLOCK_ENDABORT");
			break;
	}

	/*
	 * We must switch to TopTransactionContext before returning. This is
	 * already done if we called StartTransaction, otherwise not.
	 */
	Assert(TopTransactionContext != NULL);
	MemoryContextSwitchTo(TopTransactionContext);
}

/*
 *	CommitTransactionCommand
 */
void
CommitTransactionCommand(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * If we aren't in a transaction block, just do our usual
			 * transaction commit.
			 */
		case TBLOCK_DEFAULT:
			CommitTransaction();
			break;

			/*
			 * This is the case right after we get a "BEGIN TRANSACTION"
			 * command, but the user hasn't done anything else yet, so we
			 * change to the "transaction block in progress" state and
			 * return.
			 */
		case TBLOCK_BEGIN:
			s->blockState = TBLOCK_INPROGRESS;
			break;

			/*
			 * This is the case when we have finished executing a command
			 * someplace within a transaction block.  We increment the
			 * command counter and return.
			 */
		case TBLOCK_INPROGRESS:
			CommandCounterIncrement();
			break;

			/*
			 * This is the case when we just got the "END TRANSACTION"
			 * statement, so we commit the transaction and go back to the
			 * default state.
			 */
		case TBLOCK_END:
			CommitTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * Here we are in the middle of a transaction block but one of
			 * the commands caused an abort so we do nothing but remain in
			 * the abort state.  Eventually we will get to the "END
			 * TRANSACTION" which will set things straight.
			 */
		case TBLOCK_ABORT:
			break;

			/*
			 * Here we were in an aborted transaction block which just
			 * processed the "END TRANSACTION" command from the user, so
			 * clean up and return to the default state.
			 */
		case TBLOCK_ENDABORT:
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;
	}
}

/*
 *	AbortCurrentTransaction
 */
void
AbortCurrentTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * if we aren't in a transaction block, we just do the basic
			 * abort & cleanup transaction.
			 */
		case TBLOCK_DEFAULT:
			AbortTransaction();
			CleanupTransaction();
			break;

			/*
			 * If we are in the TBLOCK_BEGIN it means something screwed up
			 * right after reading "BEGIN TRANSACTION" so we enter the
			 * abort state.  Eventually an "END TRANSACTION" will fix
			 * things.
			 */
		case TBLOCK_BEGIN:
			s->blockState = TBLOCK_ABORT;
			AbortTransaction();
			/* CleanupTransaction happens when we exit TBLOCK_ABORT */
			break;

			/*
			 * This is the case when are somewhere in a transaction block
			 * which aborted so we abort the transaction and set the ABORT
			 * state.  Eventually an "END TRANSACTION" will fix things and
			 * restore us to a normal state.
			 */
		case TBLOCK_INPROGRESS:
			s->blockState = TBLOCK_ABORT;
			AbortTransaction();
			/* CleanupTransaction happens when we exit TBLOCK_ABORT */
			break;

			/*
			 * Here, the system was fouled up just after the user wanted
			 * to end the transaction block so we abort the transaction
			 * and put us back into the default state.
			 */
		case TBLOCK_END:
			s->blockState = TBLOCK_DEFAULT;
			AbortTransaction();
			CleanupTransaction();
			break;

			/*
			 * Here, we are already in an aborted transaction state and
			 * are waiting for an "END TRANSACTION" to come along and lo
			 * and behold, we abort again! So we just remain in the abort
			 * state.
			 */
		case TBLOCK_ABORT:
			break;

			/*
			 * Here we were in an aborted transaction block which just
			 * processed the "END TRANSACTION" command but somehow aborted
			 * again.. since we must have done the abort processing, we
			 * clean up and return to the default state.
			 */
		case TBLOCK_ENDABORT:
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;
	}
}

/*
 *	PreventTransactionChain
 *
 *	This routine is to be called by statements that must not run inside
 *	a transaction block, typically because they have non-rollback-able
 *	side effects or do internal commits.
 *
 *	If we have already started a transaction block, issue an error; also issue
 *	an error if we appear to be running inside a user-defined function (which
 *	could issue more commands and possibly cause a failure after the statement
 *	completes).
 *
 *	stmtNode: pointer to parameter block for statement; this is used in
 *	a very klugy way to determine whether we are inside a function.
 *	stmtType: statement type name for error messages.
 */
void
PreventTransactionChain(void *stmtNode, const char *stmtType)
{
	/*
	 * xact block already started?
	 */
	if (IsTransactionBlock())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		/* translator: %s represents an SQL statement name */
				 errmsg("%s cannot run inside a transaction block",
						stmtType)));

	/*
	 * Are we inside a function call?  If the statement's parameter block
	 * was allocated in QueryContext, assume it is an interactive command.
	 * Otherwise assume it is coming from a function.
	 */
	if (!MemoryContextContains(QueryContext, stmtNode))
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		/* translator: %s represents an SQL statement name */
			 errmsg("%s cannot be executed from a function", stmtType)));
	/* If we got past IsTransactionBlock test, should be in default state */
	if (CurrentTransactionState->blockState != TBLOCK_DEFAULT)
		elog(ERROR, "cannot prevent transaction chain");
	/* all okay */
}

/*
 *	RequireTransactionChain
 *
 *	This routine is to be called by statements that must run inside
 *	a transaction block, because they have no effects that persist past
 *	transaction end (and so calling them outside a transaction block
 *	is presumably an error).  DECLARE CURSOR is an example.
 *
 *	If we appear to be running inside a user-defined function, we do not
 *	issue an error, since the function could issue more commands that make
 *	use of the current statement's results.  Thus this is an inverse for
 *	PreventTransactionChain.
 *
 *	stmtNode: pointer to parameter block for statement; this is used in
 *	a very klugy way to determine whether we are inside a function.
 *	stmtType: statement type name for error messages.
 */
void
RequireTransactionChain(void *stmtNode, const char *stmtType)
{
	/*
	 * xact block already started?
	 */
	if (IsTransactionBlock())
		return;

	/*
	 * Are we inside a function call?  If the statement's parameter block
	 * was allocated in QueryContext, assume it is an interactive command.
	 * Otherwise assume it is coming from a function.
	 */
	if (!MemoryContextContains(QueryContext, stmtNode))
		return;
	ereport(ERROR,
			(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
	/* translator: %s represents an SQL statement name */
			 errmsg("%s may only be used in transaction blocks",
					stmtType)));
}


/*
 * Register or deregister callback functions for end-of-xact cleanup
 *
 * These functions are intended for use by dynamically loaded modules.
 * For built-in modules we generally just hardwire the appropriate calls
 * (mainly because it's easier to control the order that way, where needed).
 *
 * Note that the callback occurs post-commit or post-abort, so the callback
 * functions can only do noncritical cleanup.
 */
void
RegisterEOXactCallback(EOXactCallback callback, void *arg)
{
	EOXactCallbackItem *item;

	item = (EOXactCallbackItem *)
		MemoryContextAlloc(TopMemoryContext, sizeof(EOXactCallbackItem));
	item->callback = callback;
	item->arg = arg;
	item->next = EOXact_callbacks;
	EOXact_callbacks = item;
}

void
UnregisterEOXactCallback(EOXactCallback callback, void *arg)
{
	EOXactCallbackItem *item;
	EOXactCallbackItem *prev;

	prev = NULL;
	for (item = EOXact_callbacks; item; prev = item, item = item->next)
	{
		if (item->callback == callback && item->arg == arg)
		{
			if (prev)
				prev->next = item->next;
			else
				EOXact_callbacks = item->next;
			pfree(item);
			break;
		}
	}
}

static void
CallEOXactCallbacks(bool isCommit)
{
	EOXactCallbackItem *item;

	for (item = EOXact_callbacks; item; item = item->next)
	{
		(*item->callback) (isCommit, item->arg);
	}
}


/* ----------------------------------------------------------------
 *					   transaction block support
 * ----------------------------------------------------------------
 */
/*
 *	BeginTransactionBlock
 */
void
BeginTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * check the current transaction state
	 */
	if (s->blockState != TBLOCK_DEFAULT)
		ereport(WARNING,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("there is already a transaction in progress")));

	/*
	 * set the current transaction block state information appropriately
	 * during begin processing
	 */
	s->blockState = TBLOCK_BEGIN;

	/*
	 * do begin processing here.  Nothing to do at present.
	 */

	/*
	 * done with begin processing, set block state to inprogress
	 */
	s->blockState = TBLOCK_INPROGRESS;
}

/*
 *	EndTransactionBlock
 */
void
EndTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * check the current transaction state
	 */
	if (s->blockState == TBLOCK_INPROGRESS)
	{
		/*
		 * here we are in a transaction block which should commit when we
		 * get to the upcoming CommitTransactionCommand() so we set the
		 * state to "END".	CommitTransactionCommand() will recognize this
		 * and commit the transaction and return us to the default state
		 */
		s->blockState = TBLOCK_END;
		return;
	}

	if (s->blockState == TBLOCK_ABORT)
	{
		/*
		 * here, we are in a transaction block which aborted and since the
		 * AbortTransaction() was already done, we do whatever is needed
		 * and change to the special "END ABORT" state.  The upcoming
		 * CommitTransactionCommand() will recognise this and then put us
		 * back in the default state.
		 */
		s->blockState = TBLOCK_ENDABORT;
		return;
	}

	/*
	 * here, the user issued COMMIT when not inside a transaction. Issue a
	 * WARNING and go to abort state.  The upcoming call to
	 * CommitTransactionCommand() will then put us back into the default
	 * state.
	 */
	ereport(WARNING,
			(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
			 errmsg("there is no transaction in progress")));
	AbortTransaction();
	s->blockState = TBLOCK_ENDABORT;
}

/*
 *	AbortTransactionBlock
 */
#ifdef NOT_USED
static void
AbortTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * check the current transaction state
	 */
	if (s->blockState == TBLOCK_INPROGRESS)
	{
		/*
		 * here we were inside a transaction block something screwed up
		 * inside the system so we enter the abort state, do the abort
		 * processing and then return. We remain in the abort state until
		 * we see an END TRANSACTION command.
		 */
		s->blockState = TBLOCK_ABORT;
		AbortTransaction();
		return;
	}

	/*
	 * here, the user issued ABORT when not inside a transaction. Issue a
	 * WARNING and go to abort state.  The upcoming call to
	 * CommitTransactionCommand() will then put us back into the default
	 * state.
	 */
	ereport(WARNING,
			(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
			 errmsg("there is no transaction in progress")));
	AbortTransaction();
	s->blockState = TBLOCK_ENDABORT;
}
#endif

/*
 *	UserAbortTransactionBlock
 */
void
UserAbortTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * if the transaction has already been automatically aborted with an
	 * error, and the user subsequently types 'abort', allow it.  (the
	 * behavior is the same as if they had typed 'end'.)
	 */
	if (s->blockState == TBLOCK_ABORT)
	{
		s->blockState = TBLOCK_ENDABORT;
		return;
	}

	if (s->blockState == TBLOCK_INPROGRESS)
	{
		/*
		 * here we were inside a transaction block and we got an abort
		 * command from the user, so we move to the abort state, do the
		 * abort processing and then change to the ENDABORT state so we
		 * will end up in the default state after the upcoming
		 * CommitTransactionCommand().
		 */
		s->blockState = TBLOCK_ABORT;
		AbortTransaction();
		s->blockState = TBLOCK_ENDABORT;
		return;
	}

	/*
	 * here, the user issued ABORT when not inside a transaction. Issue a
	 * WARNING and go to abort state.  The upcoming call to
	 * CommitTransactionCommand() will then put us back into the default
	 * state.
	 */
	ereport(WARNING,
			(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
			 errmsg("there is no transaction in progress")));
	AbortTransaction();
	s->blockState = TBLOCK_ENDABORT;
}

/*
 *	AbortOutOfAnyTransaction
 *
 *	This routine is provided for error recovery purposes.  It aborts any
 *	active transaction or transaction block, leaving the system in a known
 *	idle state.
 */
void
AbortOutOfAnyTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * Get out of any low-level transaction
	 */
	switch (s->state)
	{
		case TRANS_START:
		case TRANS_INPROGRESS:
		case TRANS_COMMIT:
			/* In a transaction, so clean up */
			AbortTransaction();
			CleanupTransaction();
			break;
		case TRANS_ABORT:
			/* AbortTransaction already done, still need Cleanup */
			CleanupTransaction();
			break;
		case TRANS_DEFAULT:
			/* Not in a transaction, do nothing */
			break;
	}

	/*
	 * Now reset the high-level state
	 */
	s->blockState = TBLOCK_DEFAULT;
}

/*
 * IsTransactionBlock --- are we within a transaction block?
 */
bool
IsTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_DEFAULT)
		return false;

	return true;
}

/*
 * IsTransactionOrTransactionBlock --- are we within either a transaction
 * or a transaction block?  (The backend is only really "idle" when this
 * returns false.)
 *
 * This should match up with IsTransactionBlock and IsTransactionState.
 */
bool
IsTransactionOrTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_DEFAULT && s->state == TRANS_DEFAULT)
		return false;

	return true;
}

/*
 * TransactionBlockStatusCode - return status code to send in ReadyForQuery
 */
char
TransactionBlockStatusCode(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
		case TBLOCK_DEFAULT:
			return 'I';			/* idle --- not in transaction */
		case TBLOCK_BEGIN:
		case TBLOCK_INPROGRESS:
		case TBLOCK_END:
			return 'T';			/* in transaction */
		case TBLOCK_ABORT:
		case TBLOCK_ENDABORT:
			return 'E';			/* in failed transaction */
	}

	/* should never get here */
	elog(ERROR, "invalid transaction block state: %d",
		 (int) s->blockState);
	return 0;					/* keep compiler quiet */
}


/*
 *	XLOG support routines
 */

void
xact_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_XACT_COMMIT)
	{
		TransactionIdCommit(record->xl_xid);
		/* SHOULD REMOVE FILES OF ALL DROPPED RELATIONS */
	}
	else if (info == XLOG_XACT_ABORT)
	{
		TransactionIdAbort(record->xl_xid);
		/* SHOULD REMOVE FILES OF ALL FAILED-TO-BE-CREATED RELATIONS */
	}
	else
		elog(PANIC, "xact_redo: unknown op code %u", info);
}

void
xact_undo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_XACT_COMMIT)		/* shouldn't be called by XLOG */
		elog(PANIC, "xact_undo: can't undo committed xaction");
	else if (info != XLOG_XACT_ABORT)
		elog(PANIC, "xact_redo: unknown op code %u", info);
}

void
xact_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_XACT_COMMIT)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) rec;
		struct tm  *tm = localtime(&xlrec->xtime);

		sprintf(buf + strlen(buf), "commit: %04u-%02u-%02u %02u:%02u:%02u",
				tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
				tm->tm_hour, tm->tm_min, tm->tm_sec);
	}
	else if (info == XLOG_XACT_ABORT)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) rec;
		struct tm  *tm = localtime(&xlrec->xtime);

		sprintf(buf + strlen(buf), "abort: %04u-%02u-%02u %02u:%02u:%02u",
				tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
				tm->tm_hour, tm->tm_min, tm->tm_sec);
	}
	else
		strcat(buf, "UNKNOWN");
}

void
			XactPushRollback(void (*func) (void *), void *data)
{
#ifdef XLOG_II
	if (_RollbackFunc != NULL)
		elog(PANIC, "XactPushRollback: already installed");
#endif

	_RollbackFunc = func;
	_RollbackData = data;
}

void
XactPopRollback(void)
{
	_RollbackFunc = NULL;
}
