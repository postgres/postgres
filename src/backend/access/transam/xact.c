/*-------------------------------------------------------------------------
 *
 * xact.c
 *	  top level transaction system support routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/xact.c,v 1.72 2000/10/11 21:28:17 momjian Exp $
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

#include "access/nbtree.h"
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
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/catcache.h"
#include "utils/relcache.h"
#include "utils/temprel.h"

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
static void RecordTransactionCommit(void);
static void StartTransaction(void);

/* ----------------
 *		global variables holding the current transaction state.
 *
 *		Note: when we are running several slave processes, the
 *			  current transaction state data is copied into shared memory
 *			  and the CurrentTransactionState pointer changed to
 *			  point to the shared copy.  All this occurrs in slaves.c
 * ----------------
 */
TransactionStateData CurrentTransactionStateData = {
	0,							/* transaction id */
	FirstCommandId,				/* command id */
	0,							/* scan command id */
	0x0,						/* start time */
	TRANS_DEFAULT,				/* transaction state */
	TBLOCK_DEFAULT				/* transaction block state */
};

TransactionState CurrentTransactionState = &CurrentTransactionStateData;

int			DefaultXactIsoLevel = XACT_READ_COMMITTED;
int			XactIsoLevel;

/* ----------------
 *		info returned when the system is disabled
 *
 * Apparently a lot of this code is inherited from other prototype systems.
 * For DisabledStartTime, use a symbolic value to make the relationships clearer.
 * The old value of 1073741823 corresponds to a date in y2004, which is coming closer
 *	every day. It appears that if we return a value guaranteed larger than
 *	any real time associated with a transaction then comparisons in other
 *	modules will still be correct. Let's use BIG_ABSTIME for this. tgl 2/14/97
 *
 *		Note:  I have no idea what the significance of the
 *			   1073741823 in DisabledStartTime.. I just carried
 *			   this over when converting things from the old
 *			   V1 transaction system.  -cim 3/18/90
 * ----------------
 */
TransactionId DisabledTransactionId = (TransactionId) -1;

CommandId	DisabledCommandId = (CommandId) -1;

AbsoluteTime DisabledStartTime = (AbsoluteTime) BIG_ABSTIME;	/* 1073741823; */

/* ----------------
 *		overflow flag
 * ----------------
 */
bool		CommandIdCounterOverflowFlag;

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
		case TRANS_DISABLED:
			return false;
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
IsAbortedTransactionBlockState()
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_ABORT)
		return true;

	return false;
}

/* --------------------------------
 *		OverrideTransactionSystem
 *
 *		This is used to temporarily disable the transaction
 *		processing system in order to do initialization of
 *		the transaction system data structures and relations
 *		themselves.
 * --------------------------------
 */
int			SavedTransactionState;

void
OverrideTransactionSystem(bool flag)
{
	TransactionState s = CurrentTransactionState;

	if (flag == true)
	{
		if (s->state == TRANS_DISABLED)
			return;

		SavedTransactionState = s->state;
		s->state = TRANS_DISABLED;
	}
	else
	{
		if (s->state != TRANS_DISABLED)
			return;

		s->state = SavedTransactionState;
	}
}

/* --------------------------------
 *		GetCurrentTransactionId
 *
 *		This returns the id of the current transaction, or
 *		the id of the "disabled" transaction.
 * --------------------------------
 */
TransactionId
GetCurrentTransactionId()
{
	TransactionState s = CurrentTransactionState;

	/* ----------------
	 *	if the transaction system is disabled, we return
	 *	the special "disabled" transaction id.
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return (TransactionId) DisabledTransactionId;

	/* ----------------
	 *	otherwise return the current transaction id.
	 * ----------------
	 */
	return (TransactionId) s->transactionIdData;
}


/* --------------------------------
 *		GetCurrentCommandId
 * --------------------------------
 */
CommandId
GetCurrentCommandId()
{
	TransactionState s = CurrentTransactionState;

	/* ----------------
	 *	if the transaction system is disabled, we return
	 *	the special "disabled" command id.
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return (CommandId) DisabledCommandId;

	return s->commandId;
}

CommandId
GetScanCommandId()
{
	TransactionState s = CurrentTransactionState;

	/* ----------------
	 *	if the transaction system is disabled, we return
	 *	the special "disabled" command id.
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return (CommandId) DisabledCommandId;

	return s->scanCommandId;
}


/* --------------------------------
 *		GetCurrentTransactionStartTime
 * --------------------------------
 */
AbsoluteTime
GetCurrentTransactionStartTime()
{
	TransactionState s = CurrentTransactionState;

	/* ----------------
	 *	if the transaction system is disabled, we return
	 *	the special "disabled" starting time.
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return (AbsoluteTime) DisabledStartTime;

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

	return (bool)
		TransactionIdEquals(xid, s->transactionIdData);
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
 *		ClearCommandIdCounterOverflowFlag
 * --------------------------------
 */
#ifdef NOT_USED
void
ClearCommandIdCounterOverflowFlag()
{
	CommandIdCounterOverflowFlag = false;
}

#endif

/* --------------------------------
 *		CommandCounterIncrement
 * --------------------------------
 */
void
CommandCounterIncrement()
{
	CurrentTransactionStateData.commandId += 1;
	if (CurrentTransactionStateData.commandId == FirstCommandId)
	{
		CommandIdCounterOverflowFlag = true;
		elog(ERROR, "You may only have 2^32-1 commands per transaction");
	}

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
 *						initialization stuff
 * ----------------------------------------------------------------
 */
void
InitializeTransactionSystem()
{
	InitializeTransactionLog();
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
AtStart_Cache()
{
	DiscardInvalid();
}

/* --------------------------------
 *		AtStart_Locks
 * --------------------------------
 */
static void
AtStart_Locks()
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
AtStart_Memory()
{
	/* ----------------
	 *	We shouldn't have any transaction contexts already.
	 * ----------------
	 */
	Assert(TopTransactionContext == NULL);
	Assert(TransactionCommandContext == NULL);

	/* ----------------
	 *	Create a toplevel context for the transaction.
	 * ----------------
	 */
	TopTransactionContext =
		AllocSetContextCreate(TopMemoryContext,
							  "TopTransactionContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/* ----------------
	 *	Create a statement-level context and make it active.
	 * ----------------
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
static void
RecordTransactionCommit()
{
	TransactionId xid;
	int			leak;

	/* ----------------
	 *	get the current transaction id
	 * ----------------
	 */
	xid = GetCurrentTransactionId();

	/*
	 * flush the buffer manager pages.	Note: if we have stable main
	 * memory, dirty shared buffers are not flushed plai 8/7/90
	 */
	leak = BufferPoolCheckLeak();

	/*
	 * If no one shared buffer was changed by this transaction then we
	 * don't flush shared buffers and don't record commit status.
	 */
	if (SharedBufferChanged)
	{
		FlushBufferPool();
		if (leak)
			ResetBufferPool(true);

		/*
		 * have the transaction access methods record the status of this
		 * transaction id in the pg_log relation.
		 */
		TransactionIdCommit(xid);

		/*
		 * Now write the log info to the disk too.
		 */
		leak = BufferPoolCheckLeak();
		FlushBufferPool();
	}

	if (leak)
		ResetBufferPool(true);
}


/* --------------------------------
 *		AtCommit_Cache
 * --------------------------------
 */
static void
AtCommit_Cache()
{
	/* ----------------
	 * Make catalog changes visible to all backend.
	 * ----------------
	 */
	RegisterInvalid(true);
}

/* --------------------------------
 *		AtCommit_LocalCache
 * --------------------------------
 */
static void
AtCommit_LocalCache()
{
	/* ----------------
	 * Make catalog changes visible to me for the next command.
	 * ----------------
	 */
	ImmediateLocalInvalidation(true);
}

/* --------------------------------
 *		AtCommit_Locks
 * --------------------------------
 */
static void
AtCommit_Locks()
{
	/* ----------------
	 *	XXX What if ProcReleaseLocks fails?  (race condition?)
	 *
	 *	Then you're up a creek! -mer 5/24/92
	 * ----------------
	 */
	ProcReleaseLocks();
}

/* --------------------------------
 *		AtCommit_Memory
 * --------------------------------
 */
static void
AtCommit_Memory()
{
	/* ----------------
	 *	Now that we're "out" of a transaction, have the
	 *	system allocate things in the top memory context instead
	 *	of per-transaction contexts.
	 * ----------------
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	/* ----------------
	 *	Release all transaction-local memory.
	 * ----------------
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
RecordTransactionAbort()
{
	TransactionId xid;

	/* ----------------
	 *	get the current transaction id
	 * ----------------
	 */
	xid = GetCurrentTransactionId();

	/*
	 * Have the transaction access methods record the status of this
	 * transaction id in the pg_log relation. We skip it if no one shared
	 * buffer was changed by this transaction.
	 */
	if (SharedBufferChanged && !TransactionIdDidCommit(xid))
		TransactionIdAbort(xid);

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
AtAbort_Cache()
{
	RelationCacheAbort();
	SystemCacheAbort();
	RegisterInvalid(false);
}

/* --------------------------------
 *		AtAbort_Locks
 * --------------------------------
 */
static void
AtAbort_Locks()
{
	/* ----------------
	 *	XXX What if ProcReleaseLocks() fails?  (race condition?)
	 *
	 *	Then you're up a creek without a paddle! -mer
	 * ----------------
	 */
	ProcReleaseLocks();
}


/* --------------------------------
 *		AtAbort_Memory
 * --------------------------------
 */
static void
AtAbort_Memory()
{
	/* ----------------
	 *	Make sure we are in a valid context (not a child of
	 *	TransactionCommandContext...).  Note that it is possible
	 *	for this code to be called when we aren't in a transaction
	 *	at all; go directly to TopMemoryContext in that case.
	 * ----------------
	 */
	if (TransactionCommandContext != NULL)
	{
		MemoryContextSwitchTo(TransactionCommandContext);

		/* ----------------
		 *	We do not want to destroy transaction contexts yet,
		 *	but it should be OK to delete any command-local memory.
		 * ----------------
		 */
		MemoryContextResetAndDeleteChildren(TransactionCommandContext);
	}
	else
	{
		MemoryContextSwitchTo(TopMemoryContext);
	}
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
AtCleanup_Memory()
{
	/* ----------------
	 *	Now that we're "out" of a transaction, have the
	 *	system allocate things in the top memory context instead
	 *	of per-transaction contexts.
	 * ----------------
	 */
	MemoryContextSwitchTo(TopMemoryContext);

	/* ----------------
	 *	Release all transaction-local memory.
	 * ----------------
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
StartTransaction()
{
	TransactionState s = CurrentTransactionState;

	FreeXactSnapshot();
	XactIsoLevel = DefaultXactIsoLevel;

	/* ----------------
	 *	Check the current transaction state.  If the transaction system
	 *	is switched off, or if we're already in a transaction, do nothing.
	 *	We're already in a transaction when the monitor sends a null
	 *	command to the backend to flush the comm channel.  This is a
	 *	hacky fix to a communications problem, and we keep having to
	 *	deal with it here.	We should fix the comm channel code.  mao 080891
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED || s->state == TRANS_INPROGRESS)
		return;

	/* ----------------
	 *	set the current transaction state information
	 *	appropriately during start processing
	 * ----------------
	 */
	s->state = TRANS_START;

	SetReindexProcessing(false);

	/* ----------------
	 *	generate a new transaction id
	 * ----------------
	 */
	GetNewTransactionId(&(s->transactionIdData));

	XactLockTableInsert(s->transactionIdData);

	/* ----------------
	 *	initialize current transaction state fields
	 * ----------------
	 */
	s->commandId = FirstCommandId;
	s->scanCommandId = FirstCommandId;
	s->startTime = GetCurrentAbsoluteTime();

	/* ----------------
	 *	initialize the various transaction subsystems
	 * ----------------
	 */
	AtStart_Memory();
	AtStart_Cache();
	AtStart_Locks();

	/* ----------------
	 *	Tell the trigger manager to we're starting a transaction
	 * ----------------
	 */
	DeferredTriggerBeginXact();

	/* ----------------
	 *	done with start processing, set current transaction
	 *	state to "in progress"
	 * ----------------
	 */
	s->state = TRANS_INPROGRESS;

}

#ifdef NOT_USED
/* ---------------
 * Tell me if we are currently in progress
 * ---------------
 */
bool
CurrentXactInProgress()
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
CommitTransaction()
{
	TransactionState s = CurrentTransactionState;

	/* ----------------
	 *	check the current transaction state
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return;

	if (s->state != TRANS_INPROGRESS)
		elog(NOTICE, "CommitTransaction and not in in-progress state ");

	/* ----------------
	 *	Tell the trigger manager that this transaction is about to be
	 *	committed. He'll invoke all trigger deferred until XACT before
	 *	we really start on committing the transaction.
	 * ----------------
	 */
	DeferredTriggerEndXact();

	/* ----------------
	 *	set the current transaction state information
	 *	appropriately during the abort processing
	 * ----------------
	 */
	s->state = TRANS_COMMIT;

	/* ----------------
	 *	do commit processing
	 * ----------------
	 */

	/* handle commit for large objects [ PA, 7/17/98 ] */
	lo_commit(true);

	/* NOTIFY commit must also come before lower-level cleanup */
	AtCommit_Notify();

	CloseSequences();
	AtEOXact_portals();
	RecordTransactionCommit();

	/*
	 * Let others know about no transaction in progress by me. Note that
	 * this must be done _before_ releasing locks we hold and
	 * SpinAcquire(SInvalLock) is required: UPDATE with xid 0 is blocked
	 * by xid 1' UPDATE, xid 1 is doing commit while xid 2 gets snapshot -
	 * if xid 2' GetSnapshotData sees xid 1 as running then it must see
	 * xid 0 as running as well or it will see two tuple versions - one
	 * deleted by xid 1 and one inserted by xid 0.
	 */
	if (MyProc != (PROC *) NULL)
	{
		/* Lock SInvalLock because that's what GetSnapshotData uses. */
		SpinAcquire(SInvalLock);
		MyProc->xid = InvalidTransactionId;
		MyProc->xmin = InvalidTransactionId;
		SpinRelease(SInvalLock);
	}

	RelationPurgeLocalRelation(true);
	AtEOXact_SPI();
	AtEOXact_nbtree();
	AtCommit_Cache();
	AtCommit_Locks();
	AtCommit_Memory();
	AtEOXact_Files();

	SharedBufferChanged = false; /* safest place to do it */

	/* ----------------
	 *	done with commit processing, set current transaction
	 *	state back to default
	 * ----------------
	 */
	s->state = TRANS_DEFAULT;
}

/* --------------------------------
 *		AbortTransaction
 *
 * --------------------------------
 */
static void
AbortTransaction()
{
	TransactionState s = CurrentTransactionState;

	/*
	 * Let others to know about no transaction in progress - vadim
	 * 11/26/96
	 */
	if (MyProc != (PROC *) NULL)
	{
		MyProc->xid = InvalidTransactionId;
		MyProc->xmin = InvalidTransactionId;
	}

	/* ----------------
	 *	check the current transaction state
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return;

	if (s->state != TRANS_INPROGRESS)
		elog(NOTICE, "AbortTransaction and not in in-progress state");

	/*
	 * Reset user id which might have been changed transiently
	 */
	SetUserId(GetSessionUserId());

	/* ----------------
	 *	Tell the trigger manager that this transaction is about to be
	 *	aborted.
	 * ----------------
	 */
	DeferredTriggerAbortXact();

	/* ----------------
	 *	set the current transaction state information
	 *	appropriately during the abort processing
	 * ----------------
	 */
	s->state = TRANS_ABORT;

	/* ----------------
	 *	do abort processing
	 * ----------------
	 */
	lo_commit(false);			/* 'false' means it's abort */
	UnlockBuffers();
	AtAbort_Notify();
	CloseSequences();
	AtEOXact_portals();
	RecordTransactionAbort();
	RelationPurgeLocalRelation(false);
	remove_temp_rel_in_myxid();
	AtEOXact_SPI();
	AtEOXact_nbtree();
	AtAbort_Cache();
	AtAbort_Locks();
	AtAbort_Memory();
	AtEOXact_Files();

	SharedBufferChanged = false; /* safest place to do it */

	/* ----------------
	 *	State remains TRANS_ABORT until CleanupTransaction().
	 * ----------------
	 */
}

/* --------------------------------
 *		CleanupTransaction
 *
 * --------------------------------
 */
static void
CleanupTransaction()
{
	TransactionState s = CurrentTransactionState;

	if (s->state == TRANS_DISABLED)
		return;

	/* ----------------
	 *	State should still be TRANS_ABORT from AbortTransaction().
	 * ----------------
	 */
	if (s->state != TRANS_ABORT)
		elog(FATAL, "CleanupTransaction and not in abort state");

	/* ----------------
	 *	do abort cleanup processing
	 * ----------------
	 */
	AtCleanup_Memory();

	/* ----------------
	 *	done with abort processing, set current transaction
	 *	state back to default
	 * ----------------
	 */
	s->state = TRANS_DEFAULT;
}

/* --------------------------------
 *		StartTransactionCommand
 * --------------------------------
 */
void
StartTransactionCommand()
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/* ----------------
			 *		if we aren't in a transaction block, we
			 *		just do our usual start transaction.
			 * ----------------
			 */
		case TBLOCK_DEFAULT:
			StartTransaction();
			break;

			/* ----------------
			 *		We should never experience this -- if we do it
			 *		means the BEGIN state was not changed in the previous
			 *		CommitTransactionCommand().  If we get it, we print
			 *		a warning and change to the in-progress state.
			 * ----------------
			 */
		case TBLOCK_BEGIN:
			elog(NOTICE, "StartTransactionCommand: unexpected TBLOCK_BEGIN");
			s->blockState = TBLOCK_INPROGRESS;
			break;

			/* ----------------
			 *		This is the case when are somewhere in a transaction
			 *		block and about to start a new command.  For now we
			 *		do nothing but someday we may do command-local resource
			 *		initialization.
			 * ----------------
			 */
		case TBLOCK_INPROGRESS:
			break;

			/* ----------------
			 *		As with BEGIN, we should never experience this
			 *		if we do it means the END state was not changed in the
			 *		previous CommitTransactionCommand().  If we get it, we
			 *		print a warning, commit the transaction, start a new
			 *		transaction and change to the default state.
			 * ----------------
			 */
		case TBLOCK_END:
			elog(NOTICE, "StartTransactionCommand: unexpected TBLOCK_END");
			s->blockState = TBLOCK_DEFAULT;
			CommitTransaction();
			StartTransaction();
			break;

			/* ----------------
			 *		Here we are in the middle of a transaction block but
			 *		one of the commands caused an abort so we do nothing
			 *		but remain in the abort state.	Eventually we will get
			 *		to the "END TRANSACTION" which will set things straight.
			 * ----------------
			 */
		case TBLOCK_ABORT:
			break;

			/* ----------------
			 *		This means we somehow aborted and the last call to
			 *		CommitTransactionCommand() didn't clear the state so
			 *		we remain in the ENDABORT state and maybe next time
			 *		we get to CommitTransactionCommand() the state will
			 *		get reset to default.
			 * ----------------
			 */
		case TBLOCK_ENDABORT:
			elog(NOTICE, "StartTransactionCommand: unexpected TBLOCK_ENDABORT");
			break;
	}

	/*
	 * We must switch to TransactionCommandContext before returning.
	 * This is already done if we called StartTransaction, otherwise not.
	 */
	Assert(TransactionCommandContext != NULL);
	MemoryContextSwitchTo(TransactionCommandContext);
}

/* --------------------------------
 *		CommitTransactionCommand
 * --------------------------------
 */
void
CommitTransactionCommand()
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/* ----------------
			 *		if we aren't in a transaction block, we
			 *		just do our usual transaction commit
			 * ----------------
			 */
		case TBLOCK_DEFAULT:
			CommitTransaction();
			break;

			/* ----------------
			 *		This is the case right after we get a "BEGIN TRANSACTION"
			 *		command, but the user hasn't done anything else yet, so
			 *		we change to the "transaction block in progress" state
			 *		and return.
			 * ----------------
			 */
		case TBLOCK_BEGIN:
			s->blockState = TBLOCK_INPROGRESS;
			break;

			/* ----------------
			 *		This is the case when we have finished executing a command
			 *		someplace within a transaction block.  We increment the
			 *		command counter and return.  Someday we may free resources
			 *		local to the command.
			 *
			 *		That someday is today, at least for memory allocated in
			 *		TransactionCommandContext.
			 *				- vadim 03/25/97
			 * ----------------
			 */
		case TBLOCK_INPROGRESS:
			CommandCounterIncrement();
			MemoryContextResetAndDeleteChildren(TransactionCommandContext);
			break;

			/* ----------------
			 *		This is the case when we just got the "END TRANSACTION"
			 *		statement, so we commit the transaction and go back to
			 *		the default state.
			 * ----------------
			 */
		case TBLOCK_END:
			CommitTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/* ----------------
			 *		Here we are in the middle of a transaction block but
			 *		one of the commands caused an abort so we do nothing
			 *		but remain in the abort state.	Eventually we will get
			 *		to the "END TRANSACTION" which will set things straight.
			 * ----------------
			 */
		case TBLOCK_ABORT:
			break;

			/* ----------------
			 *		Here we were in an aborted transaction block which
			 *		just processed the "END TRANSACTION" command from the
			 *		user, so clean up and return to the default state.
			 * ----------------
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
AbortCurrentTransaction()
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/* ----------------
			 *		if we aren't in a transaction block, we
			 *		just do the basic abort & cleanup transaction.
			 * ----------------
			 */
		case TBLOCK_DEFAULT:
			AbortTransaction();
			CleanupTransaction();
			break;

			/* ----------------
			 *		If we are in the TBLOCK_BEGIN it means something
			 *		screwed up right after reading "BEGIN TRANSACTION"
			 *		so we enter the abort state.  Eventually an "END
			 *		TRANSACTION" will fix things.
			 * ----------------
			 */
		case TBLOCK_BEGIN:
			s->blockState = TBLOCK_ABORT;
			AbortTransaction();
			/* CleanupTransaction happens when we exit TBLOCK_ABORT */
			break;

			/* ----------------
			 *		This is the case when are somewhere in a transaction
			 *		block which aborted so we abort the transaction and
			 *		set the ABORT state.  Eventually an "END TRANSACTION"
			 *		will fix things and restore us to a normal state.
			 * ----------------
			 */
		case TBLOCK_INPROGRESS:
			s->blockState = TBLOCK_ABORT;
			AbortTransaction();
			/* CleanupTransaction happens when we exit TBLOCK_ABORT */
			break;

			/* ----------------
			 *		Here, the system was fouled up just after the
			 *		user wanted to end the transaction block so we
			 *		abort the transaction and put us back into the
			 *		default state.
			 * ----------------
			 */
		case TBLOCK_END:
			s->blockState = TBLOCK_DEFAULT;
			AbortTransaction();
			CleanupTransaction();
			break;

			/* ----------------
			 *		Here, we are already in an aborted transaction
			 *		state and are waiting for an "END TRANSACTION" to
			 *		come along and lo and behold, we abort again!
			 *		So we just remain in the abort state.
			 * ----------------
			 */
		case TBLOCK_ABORT:
			break;

			/* ----------------
			 *		Here we were in an aborted transaction block which
			 *		just processed the "END TRANSACTION" command but somehow
			 *		aborted again.. since we must have done the abort
			 *		processing, we clean up and return to the default state.
			 * ----------------
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

	/* ----------------
	 *	check the current transaction state
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return;

	if (s->blockState != TBLOCK_DEFAULT)
		elog(NOTICE, "BEGIN: already a transaction in progress");

	/* ----------------
	 *	set the current transaction block state information
	 *	appropriately during begin processing
	 * ----------------
	 */
	s->blockState = TBLOCK_BEGIN;

	/* ----------------
	 *	do begin processing
	 * ----------------
	 */

	/* ----------------
	 *	done with begin processing, set block state to inprogress
	 * ----------------
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

	/* ----------------
	 *	check the current transaction state
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return;

	if (s->blockState == TBLOCK_INPROGRESS)
	{
		/* ----------------
		 *	here we are in a transaction block which should commit
		 *	when we get to the upcoming CommitTransactionCommand()
		 *	so we set the state to "END".  CommitTransactionCommand()
		 *	will recognize this and commit the transaction and return
		 *	us to the default state
		 * ----------------
		 */
		s->blockState = TBLOCK_END;
		return;
	}

	if (s->blockState == TBLOCK_ABORT)
	{
		/* ----------------
		 *	here, we are in a transaction block which aborted
		 *	and since the AbortTransaction() was already done,
		 *	we do whatever is needed and change to the special
		 *	"END ABORT" state.	The upcoming CommitTransactionCommand()
		 *	will recognise this and then put us back in the default
		 *	state.
		 * ----------------
		 */
		s->blockState = TBLOCK_ENDABORT;
		return;
	}

	/* ----------------
	 *	here, the user issued COMMIT when not inside a transaction.
	 *	Issue a notice and go to abort state.  The upcoming call to
	 *	CommitTransactionCommand() will then put us back into the
	 *	default state.
	 * ----------------
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

	/* ----------------
	 *	check the current transaction state
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return;

	if (s->blockState == TBLOCK_INPROGRESS)
	{
		/* ----------------
		 *	here we were inside a transaction block something
		 *	screwed up inside the system so we enter the abort state,
		 *	do the abort processing and then return.
		 *	We remain in the abort state until we see an
		 *	END TRANSACTION command.
		 * ----------------
		 */
		s->blockState = TBLOCK_ABORT;
		AbortTransaction();
		return;
	}

	/* ----------------
	 *	here, the user issued ABORT when not inside a transaction.
	 *	Issue a notice and go to abort state.  The upcoming call to
	 *	CommitTransactionCommand() will then put us back into the
	 *	default state.
	 * ----------------
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
UserAbortTransactionBlock()
{
	TransactionState s = CurrentTransactionState;

	/* ----------------
	 *	check the current transaction state
	 * ----------------
	 */
	if (s->state == TRANS_DISABLED)
		return;

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
		/* ----------------
		 *	here we were inside a transaction block and we
		 *	got an abort command from the user, so we move to
		 *	the abort state, do the abort processing and
		 *	then change to the ENDABORT state so we will end up
		 *	in the default state after the upcoming
		 *	CommitTransactionCommand().
		 * ----------------
		 */
		s->blockState = TBLOCK_ABORT;
		AbortTransaction();
		s->blockState = TBLOCK_ENDABORT;
		return;
	}

	/* ----------------
	 *	here, the user issued ABORT when not inside a transaction.
	 *	Issue a notice and go to abort state.  The upcoming call to
	 *	CommitTransactionCommand() will then put us back into the
	 *	default state.
	 * ----------------
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
AbortOutOfAnyTransaction()
{
	TransactionState s = CurrentTransactionState;

	/*
	 * Get out of any low-level transaction
	 */
	if (s->state != TRANS_DEFAULT)
	{
		AbortTransaction();
		CleanupTransaction();
	}

	/*
	 * Now reset the high-level state
	 */
	s->blockState = TBLOCK_DEFAULT;
}

bool
IsTransactionBlock()
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_INPROGRESS
		|| s->blockState == TBLOCK_ABORT
		|| s->blockState == TBLOCK_ENDABORT)
		return true;

	return false;
}
