/*-------------------------------------------------------------------------
 *
 * xact.c
 *	  top level transaction system support routines
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/xact.c,v 1.115.2.2 2004/08/11 04:09:12 tgl Exp $
 *
 * NOTES
 *		Transaction aborts can now occur two ways:
 *
 *		1)	system dies from some internal cause  (Assert, etc..)
 *		2)	user types abort
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
 *		to ignore these commands until we see an END transaction.
 *		(or an ABORT! --djm)
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
 *		  TransactionCommandContext until this point.
 *
 *	 NOTES
 *		This file is an attempt at a redesign of the upper layer
 *		of the V1 transaction system which was too poorly thought
 *		out to describe.  This new system hopes to be both simpler
 *		in design, simpler to extend and needs to contain added
 *		functionality to solve problems beyond the scope of the V1
 *		system.  (In particuler, communication of transaction
 *		information between parallel backends has to be supported)
 *
 *		The essential aspects of the transaction system are:
 *
 *				o  transaction id generation
 *				o  transaction log updating
 *				o  memory cleanup
 *				o  cache invalidation
 *				o  lock cleanup
 *
 *		Hence, the functional division of the transaction code is
 *		based on what of the above things need to be done during
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
 *		These are invoked only in responce to a user "BEGIN", "END",
 *		or "ABORT" command.  The tricky part about these functions
 *		is that they are called within the postgres main loop, in between
 *		the StartTransactionCommand() and CommitTransactionCommand().
 *
 *		For example, consider the following sequence of user commands:
 *
 *		1)		begin
 *		2)		retrieve (foo.all)
 *		3)		append foo (bar = baz)
 *		4)		end
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
 *		2) <	ProcessQuery();					<< retrieve (foo.all)
 *			\	CommitTransactionCommand();
 *
 *			/	StartTransactionCommand();
 *		3) <	ProcessQuery();					<< append foo (bar = baz)
 *			\	CommitTransactionCommand();
 *
 *			/	StartTransactionCommand();
 *		4) /	ProcessUtility();				<< end
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
 *		Furthermore, suppose the "retrieve (foo.all)" caused an abort
 *		condition.	We would then want to abort the transaction and
 *		ignore all subsequent commands up to the "end".
 *		-cim 3/23/90
 *
 *-------------------------------------------------------------------------
 */

/*
 * Large object clean up added in CommitTransaction() to prevent buffer leaks.
 * [PA, 7/17/98]
 * [PA] is Pascal André <andre@via.ecp.fr>
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
#include "commands/async.h"
#include "commands/sequence.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "libpq/be-fsstubs.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/catcache.h"
#include "utils/relcache.h"
#include "utils/temprel.h"

#include "pgstat.h"

extern bool SharedBufferChanged;

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
static void CleanupTransaction(void);
static void CommitTransaction(void);
static void RecordTransactionAbort(void);
static void StartTransaction(void);

/* ----------------
 *		global variables holding the current transaction state.
 * ----------------
 */
static TransactionStateData CurrentTransactionStateData = {
	0,							/* transaction id */
	FirstCommandId,				/* command id */
	0,							/* scan command id */
	0x0,						/* start time */
	TRANS_DEFAULT,				/* transaction state */
	TBLOCK_DEFAULT				/* transaction block state */
};

TransactionState CurrentTransactionState = &CurrentTransactionStateData;

/*
 * User-tweakable parameters
 */
int			DefaultXactIsoLevel = XACT_READ_COMMITTED;
int			XactIsoLevel;

int			CommitDelay = 0;	/* precommit delay in microseconds */
int			CommitSiblings = 5; /* number of concurrent xacts needed to
								 * sleep */

static void (*_RollbackFunc) (void *) = NULL;
static void *_RollbackData = NULL;

/* ----------------
 *		catalog creation transaction bootstrapping flag.
 *		This should be eliminated and added to the transaction
 *		state stuff.  -cim 3/19/90
 * ----------------
 */
bool		AMI_OVERRIDE = false;

/* ----------------------------------------------------------------
 *					 transaction state accessors
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		TranactionFlushEnabled()
 *		SetTransactionFlushEnabled()
 *
 *		These are used to test and set the "TransactionFlushState"
 *		varable.  If this variable is true (the default), then
 *		the system will flush all dirty buffers to disk at the end
 *		of each transaction.   If false then we are assuming the
 *		buffer pool resides in stable main memory, in which case we
 *		only do writes as necessary.
 * --------------------------------
 */
static int	TransactionFlushState = 1;

int
TransactionFlushEnabled(void)
{
	return TransactionFlushState;
}

#ifdef NOT_USED
void
SetTransactionFlushEnabled(bool state)
{
	TransactionFlushState = (state == true);
}


/* --------------------------------
 *		IsTransactionState
 *
 *		This returns true if we are currently running a query
 *		within an executing transaction.
 * --------------------------------
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
#endif

/* --------------------------------
 *		IsAbortedTransactionBlockState
 *
 *		This returns true if we are currently running a query
 *		within an aborted transaction block.
 * --------------------------------
 */
bool
IsAbortedTransactionBlockState(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_ABORT)
		return true;

	return false;
}


/* --------------------------------
 *		GetCurrentTransactionId
 * --------------------------------
 */
TransactionId
GetCurrentTransactionId(void)
{
	TransactionState s = CurrentTransactionState;

	return s->transactionIdData;
}


/* --------------------------------
 *		GetCurrentCommandId
 * --------------------------------
 */
CommandId
GetCurrentCommandId(void)
{
	TransactionState s = CurrentTransactionState;

	return s->commandId;
}

CommandId
GetScanCommandId(void)
{
	TransactionState s = CurrentTransactionState;

	return s->scanCommandId;
}


/* --------------------------------
 *		GetCurrentTransactionStartTime
 * --------------------------------
 */
AbsoluteTime
GetCurrentTransactionStartTime(void)
{
	TransactionState s = CurrentTransactionState;

	return s->startTime;
}


/* --------------------------------
 *		GetCurrentTransactionStartTimeUsec
 * --------------------------------
 */
AbsoluteTime
GetCurrentTransactionStartTimeUsec(int *msec)
{
	TransactionState s = CurrentTransactionState;

	*msec = s->startTimeUsec;

	return s->startTime;
}


/* --------------------------------
 *		TransactionIdIsCurrentTransactionId
 * --------------------------------
 */
bool
TransactionIdIsCurrentTransactionId(TransactionId xid)
{
	TransactionState s = CurrentTransactionState;

	if (AMI_OVERRIDE)
		return false;

	return TransactionIdEquals(xid, s->transactionIdData);
}


/* --------------------------------
 *		CommandIdIsCurrentCommandId
 * --------------------------------
 */
bool
CommandIdIsCurrentCommandId(CommandId cid)
{
	TransactionState s = CurrentTransactionState;

	if (AMI_OVERRIDE)
		return false;

	return (cid == s->commandId) ? true : false;
}

bool
CommandIdGEScanCommandId(CommandId cid)
{
	TransactionState s = CurrentTransactionState;

	if (AMI_OVERRIDE)
		return false;

	return (cid >= s->scanCommandId) ? true : false;
}


/* --------------------------------
 *		CommandCounterIncrement
 * --------------------------------
 */
void
CommandCounterIncrement(void)
{
	CurrentTransactionStateData.commandId += 1;
	if (CurrentTransactionStateData.commandId == FirstCommandId)
		elog(ERROR, "You may only have 2^32-1 commands per transaction");

	CurrentTransactionStateData.scanCommandId = CurrentTransactionStateData.commandId;

	/*
	 * make cache changes visible to me.  AtCommit_LocalCache() instead of
	 * AtCommit_Cache() is called here.
	 */
	AtCommit_LocalCache();
	AtStart_Cache();
}

void
SetScanCommandId(CommandId savedId)
{
	CurrentTransactionStateData.scanCommandId = savedId;
}

/* ----------------------------------------------------------------
 *						StartTransaction stuff
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		AtStart_Cache
 * --------------------------------
 */
static void
AtStart_Cache(void)
{
	AcceptInvalidationMessages();
}

/* --------------------------------
 *		AtStart_Locks
 * --------------------------------
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

/* --------------------------------
 *		AtStart_Memory
 * --------------------------------
 */
static void
AtStart_Memory(void)
{
	/*
	 * We shouldn't have any transaction contexts already.
	 */
	Assert(TopTransactionContext == NULL);
	Assert(TransactionCommandContext == NULL);

	/*
	 * Create a toplevel context for the transaction.
	 */
	TopTransactionContext =
		AllocSetContextCreate(TopMemoryContext,
							  "TopTransactionContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Create a statement-level context and make it active.
	 */
	TransactionCommandContext =
		AllocSetContextCreate(TopTransactionContext,
							  "TransactionCommandContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);
	MemoryContextSwitchTo(TransactionCommandContext);
}


/* ----------------------------------------------------------------
 *						CommitTransaction stuff
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		RecordTransactionCommit
 *
 *		Note: the two calls to BufferManagerFlush() exist to ensure
 *			  that data pages are written before log pages.  These
 *			  explicit calls should be replaced by a more efficient
 *			  ordered page write scheme in the buffer manager
 *			  -cim 3/18/90
 * --------------------------------
 */
void
RecordTransactionCommit(void)
{
	TransactionId xid;
	bool		leak;

	leak = BufferPoolCheckLeak();

	xid = GetCurrentTransactionId();

	/*
	 * We only need to log the commit in xlog and clog if the transaction made
	 * any transaction-controlled XLOG entries.  (Otherwise, its XID appears
	 * nowhere in permanent storage, so no one will ever care if it
	 * committed.)  However, we must flush XLOG to disk if we made any XLOG
	 * entries, whether in or out of transaction control.  For example, if we
	 * reported a nextval() result to the client, this ensures that any XLOG
	 * record generated by nextval will hit the disk before we report the
	 * transaction committed.
	 */
	if (MyXactMadeXLogEntry)
	{
		bool		madeTCentries;
		XLogRecPtr	recptr;

		BufmgrCommit();

		START_CRIT_SECTION();

		madeTCentries = (MyLastRecPtr.xrecoff != 0);

		/*
		 * We need to lock out checkpoint start between writing our XLOG
		 * record and updating pg_clog.  Otherwise it is possible for the
		 * checkpoint to set REDO after the XLOG record but fail to flush the
		 * pg_clog update to disk, leading to loss of the transaction commit
		 * if we crash a little later.  Slightly klugy fix for problem
		 * discovered 2004-08-10.
		 */
		if (madeTCentries)
			LWLockAcquire(CheckpointStartLock, LW_SHARED);

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
		 * Sleep before flush! So we can flush more than one commit
		 * records per single fsync.  (The idea is some other backend may
		 * do the XLogFlush while we're sleeping.  This needs work still,
		 * because on most Unixen, the minimum select() delay is 10msec or
		 * more, which is way too long.)
		 *
		 * We do not sleep if enableFsync is not turned on, nor if there are
		 * fewer than CommitSiblings other backends with active
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

		/* Mark the transaction committed in clog, if needed */
		if (madeTCentries)
			TransactionIdCommit(xid);

		/* Unlock checkpoint lock if we acquired it */
		if (madeTCentries)
			LWLockRelease(CheckpointStartLock);

		END_CRIT_SECTION();
	}

	/* Break the chain of back-links in the XLOG records I output */
	MyLastRecPtr.xrecoff = 0;
	MyXactMadeXLogEntry = false;

	/* Show myself as out of the transaction in PROC array */
	MyProc->logRec.xrecoff = 0;

	if (leak)
		ResetBufferPool(true);
}


/* --------------------------------
 *		AtCommit_Cache
 * --------------------------------
 */
static void
AtCommit_Cache(void)
{
	/*
	 * Make catalog changes visible to all backends.
	 */
	AtEOXactInvalidationMessages(true);
}

/* --------------------------------
 *		AtCommit_LocalCache
 * --------------------------------
 */
static void
AtCommit_LocalCache(void)
{
	/*
	 * Make catalog changes visible to me for the next command.
	 */
	CommandEndInvalidationMessages(true);
}

/* --------------------------------
 *		AtCommit_Locks
 * --------------------------------
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

/* --------------------------------
 *		AtCommit_Memory
 * --------------------------------
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
	TransactionCommandContext = NULL;
}

/* ----------------------------------------------------------------
 *						AbortTransaction stuff
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		RecordTransactionAbort
 * --------------------------------
 */
static void
RecordTransactionAbort(void)
{
	TransactionId xid = GetCurrentTransactionId();

	/*
	 * We only need to log the abort in xlog and clog if the transaction made
	 * any transaction-controlled XLOG entries.  (Otherwise, its XID appears
	 * nowhere in permanent storage, so no one will ever care if it
	 * committed.)  We do not flush XLOG to disk in any case, since the
	 * default assumption after a crash would be that we aborted, anyway.
	 * For the same reason, we don't need to worry about interlocking
	 * against checkpoint start.
	 *
	 * Extra check here is to catch case that we aborted partway through
	 * RecordTransactionCommit ...
	 */
	if (MyLastRecPtr.xrecoff != 0 && !TransactionIdDidCommit(xid))
	{
		XLogRecData rdata;
		xl_xact_abort xlrec;
		XLogRecPtr	recptr;

		xlrec.xtime = time(NULL);
		rdata.buffer = InvalidBuffer;
		rdata.data = (char *) (&xlrec);
		rdata.len = SizeOfXactAbort;
		rdata.next = NULL;

		START_CRIT_SECTION();

		/*
		 * SHOULD SAVE ARRAY OF RELFILENODE-s TO DROP
		 */
		recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_ABORT, &rdata);

		/* Mark the transaction aborted in clog */
		TransactionIdAbort(xid);

		END_CRIT_SECTION();
	}

	/* Break the chain of back-links in the XLOG records I output */
	MyLastRecPtr.xrecoff = 0;
	MyXactMadeXLogEntry = false;

	/* Show myself as out of the transaction in PROC array */
	MyProc->logRec.xrecoff = 0;

	/*
	 * Tell bufmgr and smgr to release resources.
	 */
	ResetBufferPool(false);		/* false -> is abort */
}

/* --------------------------------
 *		AtAbort_Cache
 * --------------------------------
 */
static void
AtAbort_Cache(void)
{
	RelationCacheAbort();
	AtEOXactInvalidationMessages(false);
}

/* --------------------------------
 *		AtAbort_Locks
 * --------------------------------
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


/* --------------------------------
 *		AtAbort_Memory
 * --------------------------------
 */
static void
AtAbort_Memory(void)
{
	/*
	 * Make sure we are in a valid context (not a child of
	 * TransactionCommandContext...).  Note that it is possible for this
	 * code to be called when we aren't in a transaction at all; go
	 * directly to TopMemoryContext in that case.
	 */
	if (TransactionCommandContext != NULL)
	{
		MemoryContextSwitchTo(TransactionCommandContext);

		/*
		 * We do not want to destroy transaction contexts yet, but it
		 * should be OK to delete any command-local memory.
		 */
		MemoryContextResetAndDeleteChildren(TransactionCommandContext);
	}
	else
		MemoryContextSwitchTo(TopMemoryContext);
}


/* ----------------------------------------------------------------
 *						CleanupTransaction stuff
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		AtCleanup_Memory
 * --------------------------------
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
	TransactionCommandContext = NULL;
}


/* ----------------------------------------------------------------
 *						interface routines
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		StartTransaction
 *
 * --------------------------------
 */
static void
StartTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	FreeXactSnapshot();
	XactIsoLevel = DefaultXactIsoLevel;

	/*
	 * Check the current transaction state.  If the transaction system is
	 * switched off, or if we're already in a transaction, do nothing.
	 * We're already in a transaction when the monitor sends a null
	 * command to the backend to flush the comm channel.  This is a hacky
	 * fix to a communications problem, and we keep having to deal with it
	 * here.  We should fix the comm channel code.	mao 080891
	 */
	if (s->state == TRANS_INPROGRESS)
		return;

	/*
	 * set the current transaction state information appropriately during
	 * start processing
	 */
	s->state = TRANS_START;

	SetReindexProcessing(false);

	/*
	 * generate a new transaction id
	 */
	s->transactionIdData = GetNewTransactionId();

	XactLockTableInsert(s->transactionIdData);

	/*
	 * initialize current transaction state fields
	 */
	s->commandId = FirstCommandId;
	s->scanCommandId = FirstCommandId;
#if NOT_USED
	s->startTime = GetCurrentAbsoluteTime();
#endif
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

#ifdef NOT_USED
/* ---------------
 * Tell me if we are currently in progress
 * ---------------
 */
bool
CurrentXactInProgress(void)
{
	return CurrentTransactionState->state == TRANS_INPROGRESS;
}
#endif

/* --------------------------------
 *		CommitTransaction
 *
 * --------------------------------
 */
static void
CommitTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * check the current transaction state
	 */
	if (s->state != TRANS_INPROGRESS)
		elog(NOTICE, "CommitTransaction and not in in-progress state");

	/*
	 * Tell the trigger manager that this transaction is about to be
	 * committed. He'll invoke all trigger deferred until XACT before we
	 * really start on committing the transaction.
	 */
	DeferredTriggerEndXact();

	/* Prevent cancel/die interrupt while cleaning up */
	HOLD_INTERRUPTS();

	/*
	 * set the current transaction state information appropriately during
	 * the abort processing
	 */
	s->state = TRANS_COMMIT;

	/*
	 * do commit processing
	 */

	/* handle commit for large objects [ PA, 7/17/98 ] */
	lo_commit(true);

	/* NOTIFY commit must also come before lower-level cleanup */
	AtCommit_Notify();

	CloseSequences();
	AtEOXact_portals();

	/* Here is where we really truly commit. */
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
	if (MyProc != (PROC *) NULL)
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
	 */

	RelationPurgeLocalRelation(true);
	AtEOXact_temp_relations(true);
	smgrDoPendingDeletes(true);

	AtEOXact_SPI();
	AtEOXact_gist();
	AtEOXact_hash();
	AtEOXact_nbtree();
	AtEOXact_rtree();
	AtCommit_Cache();
	AtCommit_Locks();
	AtEOXact_CatCache(true);
	AtCommit_Memory();
	AtEOXact_Files();

	SharedBufferChanged = false;	/* safest place to do it */

	/* Count transaction commit in statistics collector */
	pgstat_count_xact_commit();

	/*
	 * done with commit processing, set current transaction state back to
	 * default
	 */
	s->state = TRANS_DEFAULT;

	RESUME_INTERRUPTS();
}

/* --------------------------------
 *		AbortTransaction
 *
 * --------------------------------
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
	 */
	if (s->state != TRANS_INPROGRESS)
		elog(NOTICE, "AbortTransaction and not in in-progress state");

	/*
	 * set the current transaction state information appropriately during
	 * the abort processing
	 */
	s->state = TRANS_ABORT;

	/*
	 * Reset user id which might have been changed transiently
	 */
	SetUserId(GetSessionUserId());

	/*
	 * do abort processing
	 */
	DeferredTriggerAbortXact();
	lo_commit(false);			/* 'false' means it's abort */
	AtAbort_Notify();
	CloseSequences();
	AtEOXact_portals();

	/* Advertise the fact that we aborted in pg_clog. */
	RecordTransactionAbort();

	/*
	 * Let others know about no transaction in progress by me. Note that
	 * this must be done _before_ releasing locks we hold and _after_
	 * RecordTransactionAbort.
	 */
	if (MyProc != (PROC *) NULL)
	{
		/* Lock SInvalLock because that's what GetSnapshotData uses. */
		LWLockAcquire(SInvalLock, LW_EXCLUSIVE);
		MyProc->xid = InvalidTransactionId;
		MyProc->xmin = InvalidTransactionId;
		LWLockRelease(SInvalLock);
	}

	RelationPurgeLocalRelation(false);
	AtEOXact_temp_relations(false);
	smgrDoPendingDeletes(false);

	AtEOXact_SPI();
	AtEOXact_gist();
	AtEOXact_hash();
	AtEOXact_nbtree();
	AtEOXact_rtree();
	AtAbort_Cache();
	AtEOXact_CatCache(false);
	AtAbort_Memory();
	AtEOXact_Files();
	AtAbort_Locks();

	SharedBufferChanged = false;	/* safest place to do it */

	/* Count transaction abort in statistics collector */
	pgstat_count_xact_rollback();

	/*
	 * State remains TRANS_ABORT until CleanupTransaction().
	 */
	RESUME_INTERRUPTS();
}

/* --------------------------------
 *		CleanupTransaction
 *
 * --------------------------------
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
	AtCleanup_Memory();

	/*
	 * done with abort processing, set current transaction state back to
	 * default
	 */
	s->state = TRANS_DEFAULT;
}

/* --------------------------------
 *		StartTransactionCommand
 * --------------------------------
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
			elog(NOTICE, "StartTransactionCommand: unexpected TBLOCK_BEGIN");
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
			elog(NOTICE, "StartTransactionCommand: unexpected TBLOCK_END");
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
			elog(NOTICE, "StartTransactionCommand: unexpected TBLOCK_ENDABORT");
			break;
	}

	/*
	 * We must switch to TransactionCommandContext before returning. This
	 * is already done if we called StartTransaction, otherwise not.
	 */
	Assert(TransactionCommandContext != NULL);
	MemoryContextSwitchTo(TransactionCommandContext);
}

/* --------------------------------
 *		CommitTransactionCommand
 * --------------------------------
 */
void
CommitTransactionCommand(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * if we aren't in a transaction block, we just do our usual
			 * transaction commit
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
			 * command counter and return.	Someday we may free resources
			 * local to the command.
			 *
			 * That someday is today, at least for memory allocated in
			 * TransactionCommandContext. - vadim 03/25/97
			 */
		case TBLOCK_INPROGRESS:
			CommandCounterIncrement();
			MemoryContextResetAndDeleteChildren(TransactionCommandContext);
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

/* --------------------------------
 *		AbortCurrentTransaction
 * --------------------------------
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

/* ----------------------------------------------------------------
 *					   transaction block support
 * ----------------------------------------------------------------
 */
/* --------------------------------
 *		BeginTransactionBlock
 * --------------------------------
 */
void
BeginTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * check the current transaction state
	 */
	if (s->blockState != TBLOCK_DEFAULT)
		elog(NOTICE, "BEGIN: already a transaction in progress");

	/*
	 * set the current transaction block state information appropriately
	 * during begin processing
	 */
	s->blockState = TBLOCK_BEGIN;

	/*
	 * do begin processing
	 */

	/*
	 * done with begin processing, set block state to inprogress
	 */
	s->blockState = TBLOCK_INPROGRESS;
}

/* --------------------------------
 *		EndTransactionBlock
 * --------------------------------
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
	 * notice and go to abort state.  The upcoming call to
	 * CommitTransactionCommand() will then put us back into the default
	 * state.
	 */
	elog(NOTICE, "COMMIT: no transaction in progress");
	AbortTransaction();
	s->blockState = TBLOCK_ENDABORT;
}

/* --------------------------------
 *		AbortTransactionBlock
 * --------------------------------
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
	 * notice and go to abort state.  The upcoming call to
	 * CommitTransactionCommand() will then put us back into the default
	 * state.
	 */
	elog(NOTICE, "ROLLBACK: no transaction in progress");
	AbortTransaction();
	s->blockState = TBLOCK_ENDABORT;
}
#endif

/* --------------------------------
 *		UserAbortTransactionBlock
 * --------------------------------
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
	 * notice and go to abort state.  The upcoming call to
	 * CommitTransactionCommand() will then put us back into the default
	 * state.
	 */
	elog(NOTICE, "ROLLBACK: no transaction in progress");
	AbortTransaction();
	s->blockState = TBLOCK_ENDABORT;
}

/* --------------------------------
 *		AbortOutOfAnyTransaction
 *
 * This routine is provided for error recovery purposes.  It aborts any
 * active transaction or transaction block, leaving the system in a known
 * idle state.
 * --------------------------------
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

bool
IsTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_INPROGRESS
		|| s->blockState == TBLOCK_ABORT
		|| s->blockState == TBLOCK_ENDABORT)
		return true;

	return false;
}

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
		elog(STOP, "xact_redo: unknown op code %u", info);
}

void
xact_undo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_XACT_COMMIT)		/* shouldn't be called by XLOG */
		elog(STOP, "xact_undo: can't undo committed xaction");
	else if (info != XLOG_XACT_ABORT)
		elog(STOP, "xact_redo: unknown op code %u", info);
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
		elog(STOP, "XactPushRollback: already installed");
#endif

	_RollbackFunc = func;
	_RollbackData = data;
}

void
XactPopRollback(void)
{
	_RollbackFunc = NULL;
}
