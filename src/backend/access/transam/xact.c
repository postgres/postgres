/*-------------------------------------------------------------------------
 *
 * xact.c
 *	  top level transaction system support routines
 *
 * See src/backend/access/transam/README for more information.
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/transam/xact.c,v 1.187 2004/09/10 18:39:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <time.h>
#include <unistd.h>

#include "access/subtrans.h"
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
#include "storage/fd.h"
#include "storage/proc.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/relcache.h"
#include "utils/resowner.h"
#include "pgstat.h"


/*
 *	transaction states - transaction state from server perspective
 */
typedef enum TransState
{
	TRANS_DEFAULT,
	TRANS_START,
	TRANS_INPROGRESS,
	TRANS_COMMIT,
	TRANS_ABORT
} TransState;

/*
 *	transaction block states - transaction state of client queries
 */
typedef enum TBlockState
{
	/* not-in-transaction-block states */
	TBLOCK_DEFAULT,
	TBLOCK_STARTED,

	/* transaction block states */
	TBLOCK_BEGIN,
	TBLOCK_INPROGRESS,
	TBLOCK_END,
	TBLOCK_ABORT,
	TBLOCK_ENDABORT,

	/* subtransaction states */
	TBLOCK_SUBBEGIN,
	TBLOCK_SUBINPROGRESS,
	TBLOCK_SUBEND,
	TBLOCK_SUBABORT,
	TBLOCK_SUBABORT_PENDING,
	TBLOCK_SUBENDABORT_ALL,
	TBLOCK_SUBENDABORT_RELEASE,
	TBLOCK_SUBENDABORT
} TBlockState;

/*
 *	transaction state structure
 */
typedef struct TransactionStateData
{
	TransactionId transactionIdData;	/* my XID */
	char	   *name;			/* savepoint name, if any */
	int			savepointLevel; /* savepoint level */
	CommandId	commandId;		/* current CID */
	TransState	state;			/* low-level state */
	TBlockState blockState;		/* high-level state */
	int			nestingLevel;	/* nest depth */
	MemoryContext curTransactionContext;		/* my xact-lifetime
												 * context */
	ResourceOwner curTransactionOwner;	/* my query resources */
	List	   *childXids;		/* subcommitted child XIDs */
	AclId		currentUser;	/* subxact start current_user */
	bool		prevXactReadOnly;		/* entry-time xact r/o state */
	struct TransactionStateData *parent;		/* back link to parent */
} TransactionStateData;

typedef TransactionStateData *TransactionState;

/*
 * childXids is currently implemented as an integer List, relying on the
 * assumption that TransactionIds are no wider than int.  We use these
 * macros to provide some isolation in case that changes in the future.
 */
#define lfirst_xid(lc)				((TransactionId) lfirst_int(lc))
#define lappend_xid(list, datum)	lappend_int(list, (int) (datum))


static void AbortTransaction(void);
static void AtAbort_Memory(void);
static void AtCleanup_Memory(void);
static void AtCommit_LocalCache(void);
static void AtCommit_Memory(void);
static void AtStart_Cache(void);
static void AtStart_Memory(void);
static void AtStart_ResourceOwner(void);
static void CallXactCallbacks(XactEvent event, TransactionId parentXid);
static void CleanupTransaction(void);
static void CommitTransaction(void);
static void RecordTransactionAbort(void);
static void StartTransaction(void);

static void RecordSubTransactionCommit(void);
static void StartSubTransaction(void);
static void CommitSubTransaction(void);
static void AbortSubTransaction(void);
static void CleanupSubTransaction(void);
static void StartAbortedSubTransaction(void);
static void PushTransaction(void);
static void PopTransaction(void);
static char *CleanupAbortedSubTransactions(bool returnName);

static void AtSubAbort_Memory(void);
static void AtSubCleanup_Memory(void);
static void AtSubCommit_Memory(void);
static void AtSubStart_Memory(void);
static void AtSubStart_ResourceOwner(void);

static void ShowTransactionState(const char *str);
static void ShowTransactionStateRec(TransactionState state);
static const char *BlockStateAsString(TBlockState blockState);
static const char *TransStateAsString(TransState state);

/*
 * CurrentTransactionState always points to the current transaction state
 * block.  It will point to TopTransactionStateData when not in a
 * transaction at all, or when in a top-level transaction.
 */
static TransactionStateData TopTransactionStateData = {
	0,							/* transaction id */
	NULL,						/* savepoint name */
	0,							/* savepoint level */
	FirstCommandId,				/* command id */
	TRANS_DEFAULT,				/* transaction state */
	TBLOCK_DEFAULT,				/* transaction block state from the client
								 * perspective */
	0,							/* nesting level */
	NULL,						/* cur transaction context */
	NULL,						/* cur transaction resource owner */
	NIL,						/* subcommitted child Xids */
	0,							/* entry-time current userid */
	false,						/* entry-time xact r/o state */
	NULL						/* link to parent state block */
};

static TransactionState CurrentTransactionState = &TopTransactionStateData;

/*
 * These vars hold the value of now(), ie, the transaction start time.
 * This does not change as we enter and exit subtransactions, so we don't
 * keep it inside the TransactionState stack.
 */
static AbsoluteTime xactStartTime;		/* integer part */
static int	xactStartTimeUsec;	/* microsecond part */


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
 * List of add-on start- and end-of-xact callbacks
 */
typedef struct XactCallbackItem
{
	struct XactCallbackItem *next;
	XactCallback callback;
	void	   *arg;
} XactCallbackItem;

static XactCallbackItem *Xact_callbacks = NULL;

static void (*_RollbackFunc) (void *) = NULL;
static void *_RollbackData = NULL;


/* ----------------------------------------------------------------
 *	transaction state accessors
 * ----------------------------------------------------------------
 */

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

	if (s->blockState == TBLOCK_ABORT ||
		s->blockState == TBLOCK_SUBABORT)
		return true;

	return false;
}


/*
 *	GetTopTransactionId
 *
 * Get the ID of the main transaction, even if we are currently inside
 * a subtransaction.
 */
TransactionId
GetTopTransactionId(void)
{
	return TopTransactionStateData.transactionIdData;
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
	return xactStartTime;
}


/*
 *	GetCurrentTransactionStartTimeUsec
 */
AbsoluteTime
GetCurrentTransactionStartTimeUsec(int *msec)
{
	*msec = xactStartTimeUsec;
	return xactStartTime;
}


/*
 *	GetCurrentTransactionNestLevel
 *
 * Note: this will return zero when not inside any transaction, one when
 * inside a top-level transaction, etc.
 */
int
GetCurrentTransactionNestLevel(void)
{
	TransactionState s = CurrentTransactionState;

	return s->nestingLevel;
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
	TransactionState s;

	if (AMI_OVERRIDE)
	{
		Assert(xid == BootstrapTransactionId);
		return false;
	}

	/*
	 * We will return true for the Xid of the current subtransaction, any
	 * of its subcommitted children, any of its parents, or any of their
	 * previously subcommitted children.  However, a transaction being
	 * aborted is no longer "current", even though it may still have an
	 * entry on the state stack.
	 */
	for (s = CurrentTransactionState; s != NULL; s = s->parent)
	{
		ListCell   *cell;

		if (s->state == TRANS_ABORT)
			continue;
		if (TransactionIdEquals(xid, s->transactionIdData))
			return true;
		foreach(cell, s->childXids)
		{
			if (TransactionIdEquals(xid, lfirst_xid(cell)))
				return true;
		}
	}

	return false;
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
	 * make cache changes visible to me.
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
 *	AtStart_Memory
 */
static void
AtStart_Memory(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * We shouldn't have a transaction context already.
	 */
	Assert(TopTransactionContext == NULL);

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
	 * In a top-level transaction, CurTransactionContext is the same as
	 * TopTransactionContext.
	 */
	CurTransactionContext = TopTransactionContext;
	s->curTransactionContext = CurTransactionContext;

	/* Make the CurTransactionContext active. */
	MemoryContextSwitchTo(CurTransactionContext);
}

/*
 *	AtStart_ResourceOwner
 */
static void
AtStart_ResourceOwner(void)
{
	TransactionState s = CurrentTransactionState;

	/*
	 * We shouldn't have a transaction resource owner already.
	 */
	Assert(TopTransactionResourceOwner == NULL);

	/*
	 * Create a toplevel resource owner for the transaction.
	 */
	s->curTransactionOwner = ResourceOwnerCreate(NULL, "TopTransaction");

	TopTransactionResourceOwner = s->curTransactionOwner;
	CurTransactionResourceOwner = s->curTransactionOwner;
	CurrentResourceOwner = s->curTransactionOwner;
}

/* ----------------------------------------------------------------
 *						StartSubTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 * AtSubStart_Memory
 */
static void
AtSubStart_Memory(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(CurTransactionContext != NULL);

	/*
	 * Create a CurTransactionContext, which will be used to hold data
	 * that survives subtransaction commit but disappears on
	 * subtransaction abort. We make it a child of the immediate parent's
	 * CurTransactionContext.
	 */
	CurTransactionContext = AllocSetContextCreate(CurTransactionContext,
												  "CurTransactionContext",
												ALLOCSET_DEFAULT_MINSIZE,
											   ALLOCSET_DEFAULT_INITSIZE,
											   ALLOCSET_DEFAULT_MAXSIZE);
	s->curTransactionContext = CurTransactionContext;

	/* Make the CurTransactionContext active. */
	MemoryContextSwitchTo(CurTransactionContext);
}

/*
 * AtSubStart_ResourceOwner
 */
static void
AtSubStart_ResourceOwner(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(s->parent != NULL);

	/*
	 * Create a resource owner for the subtransaction.	We make it a child
	 * of the immediate parent's resource owner.
	 */
	s->curTransactionOwner =
		ResourceOwnerCreate(s->parent->curTransactionOwner,
							"SubTransaction");

	CurTransactionResourceOwner = s->curTransactionOwner;
	CurrentResourceOwner = s->curTransactionOwner;
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
	int			nrels;
	RelFileNode *rptr;
	int			nchildren;
	TransactionId *children;

	/* Get data needed for commit record */
	nrels = smgrGetPendingDeletes(true, &rptr);
	nchildren = xactGetCommittedChildren(&children);

	/*
	 * If we made neither any XLOG entries nor any temp-rel updates, and
	 * have no files to be deleted, we can omit recording the transaction
	 * commit at all.  (This test includes the effects of subtransactions,
	 * so the presence of committed subxacts need not alone force a
	 * write.)
	 */
	if (MyXactMadeXLogEntry || MyXactMadeTempRelUpdate || nrels > 0)
	{
		TransactionId xid = GetCurrentTransactionId();
		bool		madeTCentries;
		XLogRecPtr	recptr;

		/* Tell bufmgr and smgr to prepare for commit */
		BufmgrCommit();

		START_CRIT_SECTION();

		/*
		 * If our transaction made any transaction-controlled XLOG
		 * entries, we need to lock out checkpoint start between writing
		 * our XLOG record and updating pg_clog.  Otherwise it is possible
		 * for the checkpoint to set REDO after the XLOG record but fail
		 * to flush the pg_clog update to disk, leading to loss of the
		 * transaction commit if we crash a little later.  Slightly klugy
		 * fix for problem discovered 2004-08-10.
		 *
		 * (If it made no transaction-controlled XLOG entries, its XID
		 * appears nowhere in permanent storage, so no one else will ever
		 * care if it committed; so it doesn't matter if we lose the
		 * commit flag.)
		 *
		 * Note we only need a shared lock.
		 */
		madeTCentries = (MyLastRecPtr.xrecoff != 0);
		if (madeTCentries)
			LWLockAcquire(CheckpointStartLock, LW_SHARED);

		/*
		 * We only need to log the commit in XLOG if the transaction made
		 * any transaction-controlled XLOG entries or will delete files.
		 */
		if (madeTCentries || nrels > 0)
		{
			XLogRecData rdata[3];
			int			lastrdata = 0;
			xl_xact_commit xlrec;

			xlrec.xtime = time(NULL);
			xlrec.nrels = nrels;
			xlrec.nsubxacts = nchildren;
			rdata[0].buffer = InvalidBuffer;
			rdata[0].data = (char *) (&xlrec);
			rdata[0].len = MinSizeOfXactCommit;
			/* dump rels to delete */
			if (nrels > 0)
			{
				rdata[0].next = &(rdata[1]);
				rdata[1].buffer = InvalidBuffer;
				rdata[1].data = (char *) rptr;
				rdata[1].len = nrels * sizeof(RelFileNode);
				lastrdata = 1;
			}
			/* dump committed child Xids */
			if (nchildren > 0)
			{
				rdata[lastrdata].next = &(rdata[2]);
				rdata[2].buffer = InvalidBuffer;
				rdata[2].data = (char *) children;
				rdata[2].len = nchildren * sizeof(TransactionId);
				lastrdata = 2;
			}
			rdata[lastrdata].next = NULL;

			recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_COMMIT, rdata);
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
		 *
		 * Note: if we generated a commit record above, MyXactMadeXLogEntry
		 * will certainly be set now.
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
				pg_usleep(CommitDelay);

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
		if (madeTCentries || MyXactMadeTempRelUpdate)
		{
			TransactionIdCommit(xid);
			/* to avoid race conditions, the parent must commit first */
			TransactionIdCommitTree(nchildren, children);
		}

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

	/* And clean up local data */
	if (rptr)
		pfree(rptr);
	if (children)
		pfree(children);
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
	CommandEndInvalidationMessages();
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
	CurTransactionContext = NULL;
	CurrentTransactionState->curTransactionContext = NULL;
}

/* ----------------------------------------------------------------
 *						CommitSubTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 * AtSubCommit_Memory
 *
 * We do not throw away the child's CurTransactionContext, since the data
 * it contains will be needed at upper commit.
 */
static void
AtSubCommit_Memory(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(s->parent != NULL);

	/* Return to parent transaction level's memory context. */
	CurTransactionContext = s->parent->curTransactionContext;
	MemoryContextSwitchTo(CurTransactionContext);
}

/*
 * AtSubCommit_childXids
 *
 * Pass my own XID and my child XIDs up to my parent as committed children.
 */
static void
AtSubCommit_childXids(void)
{
	TransactionState s = CurrentTransactionState;
	MemoryContext old_cxt;

	Assert(s->parent != NULL);

	old_cxt = MemoryContextSwitchTo(s->parent->curTransactionContext);

	s->parent->childXids = lappend_xid(s->parent->childXids,
									   s->transactionIdData);

	s->parent->childXids = list_concat(s->parent->childXids, s->childXids);
	s->childXids = NIL;			/* ensure list not doubly referenced */

	MemoryContextSwitchTo(old_cxt);
}

/*
 * RecordSubTransactionCommit
 */
static void
RecordSubTransactionCommit(void)
{
	/*
	 * We do not log the subcommit in XLOG; it doesn't matter until the
	 * top-level transaction commits.
	 *
	 * We must mark the subtransaction subcommitted in clog if its XID
	 * appears either in permanent rels or in local temporary rels. We
	 * test this by seeing if we made transaction-controlled entries *OR*
	 * local-rel tuple updates.  (The test here actually covers the entire
	 * transaction tree so far, so it may mark subtransactions that don't
	 * really need it, but it's probably not worth being tenser. Note that
	 * if a prior subtransaction dirtied these variables, then
	 * RecordTransactionCommit will have to do the full pushup anyway...)
	 */
	if (MyLastRecPtr.xrecoff != 0 || MyXactMadeTempRelUpdate)
	{
		TransactionId xid = GetCurrentTransactionId();

		/* XXX does this really need to be a critical section? */
		START_CRIT_SECTION();

		/* Record subtransaction subcommit */
		TransactionIdSubCommit(xid);

		END_CRIT_SECTION();
	}
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
	int			nrels;
	RelFileNode *rptr;
	int			nchildren;
	TransactionId *children;

	/* Get data needed for abort record */
	nrels = smgrGetPendingDeletes(false, &rptr);
	nchildren = xactGetCommittedChildren(&children);

	/*
	 * If we made neither any transaction-controlled XLOG entries nor any
	 * temp-rel updates, and are not going to delete any files, we can
	 * omit recording the transaction abort at all.  No one will ever care
	 * that it aborted.  (These tests cover our whole transaction tree.)
	 */
	if (MyLastRecPtr.xrecoff != 0 || MyXactMadeTempRelUpdate || nrels > 0)
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
		 * any transaction-controlled XLOG entries or will delete files.
		 * (If it made no transaction-controlled XLOG entries, its XID
		 * appears nowhere in permanent storage, so no one else will ever
		 * care if it committed.)
		 *
		 * We do not flush XLOG to disk unless deleting files, since the
		 * default assumption after a crash would be that we aborted,
		 * anyway. For the same reason, we don't need to worry about
		 * interlocking against checkpoint start.
		 */
		if (MyLastRecPtr.xrecoff != 0 || nrels > 0)
		{
			XLogRecData rdata[3];
			int			lastrdata = 0;
			xl_xact_abort xlrec;
			XLogRecPtr	recptr;

			xlrec.xtime = time(NULL);
			xlrec.nrels = nrels;
			xlrec.nsubxacts = nchildren;
			rdata[0].buffer = InvalidBuffer;
			rdata[0].data = (char *) (&xlrec);
			rdata[0].len = MinSizeOfXactAbort;
			/* dump rels to delete */
			if (nrels > 0)
			{
				rdata[0].next = &(rdata[1]);
				rdata[1].buffer = InvalidBuffer;
				rdata[1].data = (char *) rptr;
				rdata[1].len = nrels * sizeof(RelFileNode);
				lastrdata = 1;
			}
			/* dump committed child Xids */
			if (nchildren > 0)
			{
				rdata[lastrdata].next = &(rdata[2]);
				rdata[2].buffer = InvalidBuffer;
				rdata[2].data = (char *) children;
				rdata[2].len = nchildren * sizeof(TransactionId);
				lastrdata = 2;
			}
			rdata[lastrdata].next = NULL;

			recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_ABORT, rdata);

			/* Must flush if we are deleting files... */
			if (nrels > 0)
				XLogFlush(recptr);
		}

		/*
		 * Mark the transaction aborted in clog.  This is not absolutely
		 * necessary but we may as well do it while we are here.
		 *
		 * The ordering here isn't critical but it seems best to mark the
		 * parent first.  This assures an atomic transition of all the
		 * subtransactions to aborted state from the point of view of
		 * concurrent TransactionIdDidAbort calls.
		 */
		TransactionIdAbort(xid);
		TransactionIdAbortTree(nchildren, children);

		END_CRIT_SECTION();
	}

	/* Break the chain of back-links in the XLOG records I output */
	MyLastRecPtr.xrecoff = 0;
	MyXactMadeXLogEntry = false;
	MyXactMadeTempRelUpdate = false;

	/* Show myself as out of the transaction in PGPROC array */
	MyProc->logRec.xrecoff = 0;

	/* And clean up local data */
	if (rptr)
		pfree(rptr);
	if (children)
		pfree(children);
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


/*
 * AtSubAbort_Memory
 */
static void
AtSubAbort_Memory(void)
{
	Assert(TopTransactionContext != NULL);

	MemoryContextSwitchTo(TopTransactionContext);
}

/*
 * RecordSubTransactionAbort
 */
static void
RecordSubTransactionAbort(void)
{
	int			nrels;
	RelFileNode *rptr;
	TransactionId xid = GetCurrentTransactionId();
	int			nchildren;
	TransactionId *children;

	/* Get data needed for abort record */
	nrels = smgrGetPendingDeletes(false, &rptr);
	nchildren = xactGetCommittedChildren(&children);

	/*
	 * If we made neither any transaction-controlled XLOG entries nor any
	 * temp-rel updates, and are not going to delete any files, we can
	 * omit recording the transaction abort at all.  No one will ever care
	 * that it aborted.  (These tests cover our whole transaction tree,
	 * and therefore may mark subxacts that don't really need it, but it's
	 * probably not worth being tenser.)
	 *
	 * In this case we needn't worry about marking subcommitted children as
	 * aborted, because they didn't mark themselves as subcommitted in the
	 * first place; see the optimization in RecordSubTransactionCommit.
	 */
	if (MyLastRecPtr.xrecoff != 0 || MyXactMadeTempRelUpdate || nrels > 0)
	{
		START_CRIT_SECTION();

		/*
		 * We only need to log the abort in XLOG if the transaction made
		 * any transaction-controlled XLOG entries or will delete files.
		 */
		if (MyLastRecPtr.xrecoff != 0 || nrels > 0)
		{
			XLogRecData rdata[3];
			int			lastrdata = 0;
			xl_xact_abort xlrec;
			XLogRecPtr	recptr;

			xlrec.xtime = time(NULL);
			xlrec.nrels = nrels;
			xlrec.nsubxacts = nchildren;
			rdata[0].buffer = InvalidBuffer;
			rdata[0].data = (char *) (&xlrec);
			rdata[0].len = MinSizeOfXactAbort;
			/* dump rels to delete */
			if (nrels > 0)
			{
				rdata[0].next = &(rdata[1]);
				rdata[1].buffer = InvalidBuffer;
				rdata[1].data = (char *) rptr;
				rdata[1].len = nrels * sizeof(RelFileNode);
				lastrdata = 1;
			}
			/* dump committed child Xids */
			if (nchildren > 0)
			{
				rdata[lastrdata].next = &(rdata[2]);
				rdata[2].buffer = InvalidBuffer;
				rdata[2].data = (char *) children;
				rdata[2].len = nchildren * sizeof(TransactionId);
				lastrdata = 2;
			}
			rdata[lastrdata].next = NULL;

			recptr = XLogInsert(RM_XACT_ID, XLOG_XACT_ABORT, rdata);

			/* Must flush if we are deleting files... */
			if (nrels > 0)
				XLogFlush(recptr);
		}

		/*
		 * Mark the transaction aborted in clog.  This is not absolutely
		 * necessary but we may as well do it while we are here.
		 */
		TransactionIdAbort(xid);
		TransactionIdAbortTree(nchildren, children);

		END_CRIT_SECTION();
	}

	/*
	 * We can immediately remove failed XIDs from PGPROC's cache of
	 * running child XIDs. It's easiest to do it here while we have the
	 * child XID array at hand, even though in the main-transaction case
	 * the equivalent work happens just after return from
	 * RecordTransactionAbort.
	 */
	XidCacheRemoveRunningXids(xid, nchildren, children);

	/* And clean up local data */
	if (rptr)
		pfree(rptr);
	if (children)
		pfree(children);
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

	Assert(CurrentTransactionState->parent == NULL);

	/*
	 * Release all transaction-local memory.
	 */
	if (TopTransactionContext != NULL)
		MemoryContextDelete(TopTransactionContext);
	TopTransactionContext = NULL;
	CurTransactionContext = NULL;
	CurrentTransactionState->curTransactionContext = NULL;
}


/* ----------------------------------------------------------------
 *						CleanupSubTransaction stuff
 * ----------------------------------------------------------------
 */

/*
 * AtSubCleanup_Memory
 */
static void
AtSubCleanup_Memory(void)
{
	TransactionState s = CurrentTransactionState;

	Assert(s->parent != NULL);

	/* Make sure we're not in an about-to-be-deleted context */
	MemoryContextSwitchTo(s->parent->curTransactionContext);
	CurTransactionContext = s->parent->curTransactionContext;

	/*
	 * Delete the subxact local memory contexts. Its CurTransactionContext
	 * can go too (note this also kills CurTransactionContexts from any
	 * children of the subxact).
	 */
	MemoryContextDelete(s->curTransactionContext);
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
		elog(WARNING, "StartTransaction while in %s state",
			 TransStateAsString(s->state));

	/*
	 * set the current transaction state information appropriately during
	 * start processing
	 */
	s->state = TRANS_START;

	/*
	 * Make sure we've freed any old snapshot, and reset xact state
	 * variables
	 */
	FreeXactSnapshot();
	XactIsoLevel = DefaultXactIsoLevel;
	XactReadOnly = DefaultXactReadOnly;

	/*
	 * must initialize resource-management stuff first
	 */
	AtStart_Memory();
	AtStart_ResourceOwner();

	/*
	 * generate a new transaction id
	 */
	s->transactionIdData = GetNewTransactionId(false);

	XactLockTableInsert(s->transactionIdData);

	/*
	 * set now()
	 */
	xactStartTime = GetCurrentAbsoluteTimeUsec(&(xactStartTimeUsec));

	/*
	 * initialize current transaction state fields
	 */
	s->commandId = FirstCommandId;
	s->nestingLevel = 1;
	s->childXids = NIL;

	/*
	 * You might expect to see "s->currentUser = GetUserId();" here, but
	 * you won't because it doesn't work during startup; the userid isn't
	 * set yet during a backend's first transaction start.  We only use
	 * the currentUser field in sub-transaction state structs.
	 *
	 * prevXactReadOnly is also valid only in sub-transactions.
	 */

	/*
	 * initialize other subsystems for new transaction
	 */
	AtStart_Inval();
	AtStart_Cache();
	AfterTriggerBeginXact();

	/*
	 * done with start processing, set current transaction state to "in
	 * progress"
	 */
	s->state = TRANS_INPROGRESS;

	ShowTransactionState("StartTransaction");
}

/*
 *	CommitTransaction
 */
static void
CommitTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	ShowTransactionState("CommitTransaction");

	/*
	 * check the current transaction state
	 */
	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "CommitTransaction while in %s state",
			 TransStateAsString(s->state));
	Assert(s->parent == NULL);

	/*
	 * Tell the trigger manager that this transaction is about to be
	 * committed. He'll invoke all trigger deferred until XACT before we
	 * really start on committing the transaction.
	 */
	AfterTriggerEndXact();

	/*
	 * Similarly, let ON COMMIT management do its thing before we start to
	 * commit.
	 */
	PreCommit_on_commit_actions();

	/* Prevent cancel/die interrupt while cleaning up */
	HOLD_INTERRUPTS();

	/*
	 * set the current transaction state information appropriately during
	 * the abort processing
	 */
	s->state = TRANS_COMMIT;

	/*
	 * Do pre-commit processing (most of this stuff requires database
	 * access, and in fact could still cause an error...)
	 */

	AtCommit_Portals();

	/* close large objects before lower-level cleanup */
	AtEOXact_LargeObject(true);

	/* NOTIFY commit must come before lower-level cleanup */
	AtCommit_Notify();

	/* Update the flat password file if we changed pg_shadow or pg_group */
	/* This should be the last step before commit */
	AtEOXact_UpdatePasswordFile(true);

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
	if (MyProc != NULL)
	{
		/* Lock SInvalLock because that's what GetSnapshotData uses. */
		LWLockAcquire(SInvalLock, LW_EXCLUSIVE);
		MyProc->xid = InvalidTransactionId;
		MyProc->xmin = InvalidTransactionId;

		/* Clear the subtransaction-XID cache too while holding the lock */
		MyProc->subxids.nxids = 0;
		MyProc->subxids.overflowed = false;

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
	 *
	 * Resources that can be associated with individual queries are handled
	 * by the ResourceOwner mechanism.	The other calls here are for
	 * backend-wide state.
	 */

	CallXactCallbacks(XACT_EVENT_COMMIT, InvalidTransactionId);

	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 true, true);

	/*
	 * Make catalog changes visible to all backends.  This has to happen
	 * after relcache references are dropped (see comments for
	 * AtEOXact_RelationCache), but before locks are released (if anyone
	 * is waiting for lock on a relation we've modified, we want them to
	 * know about the catalog change before they start using the
	 * relation).
	 */
	AtEOXact_Inval(true);

	/*
	 * Likewise, dropping of files deleted during the transaction is best done
	 * after releasing relcache and buffer pins.  (This is not strictly
	 * necessary during commit, since such pins should have been released
	 * already, but this ordering is definitely critical during abort.)
	 */
	smgrDoPendingDeletes(true);

	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_LOCKS,
						 true, true);
	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 true, true);

	AtEOXact_GUC(true, false);
	AtEOXact_SPI(true);
	AtEOXact_on_commit_actions(true, s->transactionIdData);
	AtEOXact_Namespace(true);
	/* smgrcommit already done */
	AtEOXact_Files();
	pgstat_count_xact_commit();

	CurrentResourceOwner = NULL;
	ResourceOwnerDelete(TopTransactionResourceOwner);
	s->curTransactionOwner = NULL;
	CurTransactionResourceOwner = NULL;
	TopTransactionResourceOwner = NULL;

	AtCommit_Memory();

	s->nestingLevel = 0;
	s->childXids = NIL;

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
	 */
	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "AbortTransaction while in %s state",
			 TransStateAsString(s->state));
	Assert(s->parent == NULL);

	/*
	 * set the current transaction state information appropriately during
	 * the abort processing
	 */
	s->state = TRANS_ABORT;

	/* Make sure we are in a valid memory context */
	AtAbort_Memory();

	/*
	 * Reset user id which might have been changed transiently.  We cannot
	 * use s->currentUser, but must get the session userid from
	 * miscinit.c.
	 *
	 * (Note: it is not necessary to restore session authorization here
	 * because that can only be changed via GUC, and GUC will take care of
	 * rolling it back if need be.	However, an error within a SECURITY
	 * DEFINER function could send control here with the wrong current
	 * userid.)
	 */
	SetUserId(GetSessionUserId());

	/*
	 * do abort processing
	 */
	AfterTriggerAbortXact();
	AtAbort_Portals();
	AtEOXact_LargeObject(false);	/* 'false' means it's abort */
	AtAbort_Notify();
	AtEOXact_UpdatePasswordFile(false);

	/* Advertise the fact that we aborted in pg_clog. */
	RecordTransactionAbort();

	/*
	 * Let others know about no transaction in progress by me. Note that
	 * this must be done _before_ releasing locks we hold and _after_
	 * RecordTransactionAbort.
	 */
	if (MyProc != NULL)
	{
		/* Lock SInvalLock because that's what GetSnapshotData uses. */
		LWLockAcquire(SInvalLock, LW_EXCLUSIVE);
		MyProc->xid = InvalidTransactionId;
		MyProc->xmin = InvalidTransactionId;

		/* Clear the subtransaction-XID cache too while holding the lock */
		MyProc->subxids.nxids = 0;
		MyProc->subxids.overflowed = false;

		LWLockRelease(SInvalLock);
	}

	/*
	 * Post-abort cleanup.	See notes in CommitTransaction() concerning
	 * ordering.
	 */

	CallXactCallbacks(XACT_EVENT_ABORT, InvalidTransactionId);

	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 false, true);
	AtEOXact_Inval(false);
	smgrDoPendingDeletes(false);
	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_LOCKS,
						 false, true);
	ResourceOwnerRelease(TopTransactionResourceOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 false, true);

	AtEOXact_GUC(false, false);
	AtEOXact_SPI(false);
	AtEOXact_on_commit_actions(false, s->transactionIdData);
	AtEOXact_Namespace(false);
	smgrabort();
	AtEOXact_Files();
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
		elog(FATAL, "CleanupTransaction: unexpected state %s",
			 TransStateAsString(s->state));

	/*
	 * do abort cleanup processing
	 */
	AtCleanup_Portals();		/* now safe to release portal memory */

	CurrentResourceOwner = NULL;	/* and resource owner */
	ResourceOwnerDelete(TopTransactionResourceOwner);
	s->curTransactionOwner = NULL;
	CurTransactionResourceOwner = NULL;
	TopTransactionResourceOwner = NULL;

	AtCleanup_Memory();			/* and transaction memory */

	s->nestingLevel = 0;
	s->childXids = NIL;

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
			s->blockState = TBLOCK_STARTED;
			break;

			/*
			 * This is the case when we are somewhere in a transaction
			 * block and about to start a new command.	For now we do
			 * nothing but someday we may do command-local resource
			 * initialization.
			 */
		case TBLOCK_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
			break;

			/*
			 * Here we are in the middle of a transaction block but one of
			 * the commands caused an abort so we do nothing but remain in
			 * the abort state.  Eventually we will get to the "END
			 * TRANSACTION" which will set things straight.
			 */
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
			break;

			/* These cases are invalid. */
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_END:
		case TBLOCK_SUBEND:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
		case TBLOCK_ENDABORT:
			elog(FATAL, "StartTransactionCommand: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	/*
	 * We must switch to CurTransactionContext before returning. This is
	 * already done if we called StartTransaction, otherwise not.
	 */
	Assert(CurTransactionContext != NULL);
	MemoryContextSwitchTo(CurTransactionContext);
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
			 * This shouldn't happen, because it means the previous
			 * StartTransactionCommand didn't set the STARTED state
			 * appropriately, or we didn't manage previous pending abort
			 * states.
			 */
		case TBLOCK_DEFAULT:
		case TBLOCK_SUBABORT_PENDING:
			elog(FATAL, "CommitTransactionCommand: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;

			/*
			 * If we aren't in a transaction block, just do our usual
			 * transaction commit.
			 */
		case TBLOCK_STARTED:
			CommitTransaction();
			s->blockState = TBLOCK_DEFAULT;
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

			/*
			 * Ditto, but in a subtransaction.	AbortOutOfAnyTransaction
			 * will do the dirty work.
			 */
		case TBLOCK_SUBENDABORT_ALL:
			AbortOutOfAnyTransaction();
			s = CurrentTransactionState;		/* changed by
												 * AbortOutOfAnyTransaction
												 * */
			/* AbortOutOfAnyTransaction sets the blockState */
			break;

			/*
			 * We were just issued a SAVEPOINT inside a transaction block.
			 * Start a subtransaction.	(DefineSavepoint already did
			 * PushTransaction, so as to have someplace to put the
			 * SUBBEGIN state.)
			 */
		case TBLOCK_SUBBEGIN:
			StartSubTransaction();
			s->blockState = TBLOCK_SUBINPROGRESS;
			break;

			/*
			 * Inside a subtransaction, increment the command counter.
			 */
		case TBLOCK_SUBINPROGRESS:
			CommandCounterIncrement();
			break;

			/*
			 * We were issued a COMMIT or RELEASE command, so we end the
			 * current subtransaction and return to the parent transaction.
			 * Lather, rinse, and repeat until we get out of all SUBEND'ed
			 * subtransaction levels.
			 */
		case TBLOCK_SUBEND:
			do
			{
				CommitSubTransaction();
				PopTransaction();
				s = CurrentTransactionState;	/* changed by pop */
			} while (s->blockState == TBLOCK_SUBEND);
			/* If we had a COMMIT command, finish off the main xact too */
			if (s->blockState == TBLOCK_END)
			{
				Assert(s->parent == NULL);
				CommitTransaction();
				s->blockState = TBLOCK_DEFAULT;
			}
			break;

			/*
			 * If we are in an aborted subtransaction, do nothing.
			 */
		case TBLOCK_SUBABORT:
			break;

			/*
			 * The current subtransaction is ending.  Do the equivalent of
			 * a ROLLBACK TO followed by a RELEASE command.
			 */
		case TBLOCK_SUBENDABORT_RELEASE:
			CleanupAbortedSubTransactions(false);
			break;

			/*
			 * The current subtransaction is ending due to a ROLLBACK TO
			 * command, so close all savepoints up to the target level.
			 * When finished, recreate the savepoint.
			 */
		case TBLOCK_SUBENDABORT:
			{
				char	   *name = CleanupAbortedSubTransactions(true);

				Assert(PointerIsValid(name));
				DefineSavepoint(name);
				s = CurrentTransactionState;	/* changed by
												 * DefineSavepoint */
				pfree(name);

				/* This is the same as TBLOCK_SUBBEGIN case */
				AssertState(s->blockState == TBLOCK_SUBBEGIN);
				StartSubTransaction();
				s->blockState = TBLOCK_SUBINPROGRESS;
			}
			break;
	}
}

/*
 * CleanupAbortedSubTransactions
 *
 * Helper function for CommitTransactionCommand.  Aborts and cleans up
 * dead subtransactions after a ROLLBACK TO command.  Optionally returns
 * the name of the last dead subtransaction so it can be reused to redefine
 * the savepoint.  (Caller is responsible for pfree'ing the result.)
 */
static char *
CleanupAbortedSubTransactions(bool returnName)
{
	TransactionState s = CurrentTransactionState;
	char	   *name = NULL;

	AssertState(PointerIsValid(s->parent));
	Assert(s->parent->blockState == TBLOCK_SUBINPROGRESS ||
		   s->parent->blockState == TBLOCK_INPROGRESS ||
		   s->parent->blockState == TBLOCK_STARTED ||
		   s->parent->blockState == TBLOCK_SUBABORT_PENDING);

	/*
	 * Abort everything up to the target level.  The current
	 * subtransaction only needs cleanup.  If we need to save the name,
	 * look for the last subtransaction in TBLOCK_SUBABORT_PENDING state.
	 */
	if (returnName && s->parent->blockState != TBLOCK_SUBABORT_PENDING)
		name = MemoryContextStrdup(TopMemoryContext, s->name);

	CleanupSubTransaction();
	PopTransaction();
	s = CurrentTransactionState;	/* changed by pop */

	while (s->blockState == TBLOCK_SUBABORT_PENDING)
	{
		AbortSubTransaction();
		if (returnName && s->parent->blockState != TBLOCK_SUBABORT_PENDING)
			name = MemoryContextStrdup(TopMemoryContext, s->name);
		CleanupSubTransaction();
		PopTransaction();
		s = CurrentTransactionState;
	}

	AssertState(s->blockState == TBLOCK_SUBINPROGRESS ||
				s->blockState == TBLOCK_INPROGRESS ||
				s->blockState == TBLOCK_STARTED);

	return name;
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
			 * we aren't in a transaction, so we do nothing.
			 */
		case TBLOCK_DEFAULT:
			break;

			/*
			 * if we aren't in a transaction block, we just do the basic
			 * abort & cleanup transaction.
			 */
		case TBLOCK_STARTED:
			AbortTransaction();
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * If we are in TBLOCK_BEGIN it means something screwed up
			 * right after reading "BEGIN TRANSACTION" so we enter the
			 * abort state.  Eventually an "END TRANSACTION" will fix
			 * things.
			 */
		case TBLOCK_BEGIN:
			AbortTransaction();
			s->blockState = TBLOCK_ABORT;
			/* CleanupTransaction happens when we exit TBLOCK_ENDABORT */
			break;

			/*
			 * This is the case when we are somewhere in a transaction
			 * block and we've gotten a failure, so we abort the
			 * transaction and set up the persistent ABORT state.  We will
			 * stay in ABORT until we get an "END TRANSACTION".
			 */
		case TBLOCK_INPROGRESS:
			AbortTransaction();
			s->blockState = TBLOCK_ABORT;
			/* CleanupTransaction happens when we exit TBLOCK_ENDABORT */
			break;

			/*
			 * Here, the system was fouled up just after the user wanted
			 * to end the transaction block so we abort the transaction
			 * and return to the default state.
			 */
		case TBLOCK_END:
			AbortTransaction();
			CleanupTransaction();
			s->blockState = TBLOCK_DEFAULT;
			break;

			/*
			 * Here, we are already in an aborted transaction state and
			 * are waiting for an "END TRANSACTION" to come along and lo
			 * and behold, we abort again! So we just remain in the abort
			 * state.
			 */
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
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

			/*
			 * If we are just starting a subtransaction, put it in aborted
			 * state.
			 */
		case TBLOCK_SUBBEGIN:
			StartAbortedSubTransaction();
			s->blockState = TBLOCK_SUBABORT;
			break;

		case TBLOCK_SUBINPROGRESS:
			AbortSubTransaction();
			s->blockState = TBLOCK_SUBABORT;
			break;

			/*
			 * If we are aborting an ending transaction, we have to abort
			 * the parent transaction too.
			 */
		case TBLOCK_SUBEND:
		case TBLOCK_SUBABORT_PENDING:
			AbortSubTransaction();
			CleanupSubTransaction();
			PopTransaction();
			s = CurrentTransactionState;		/* changed by pop */
			Assert(s->blockState != TBLOCK_SUBEND &&
				   s->blockState != TBLOCK_SUBENDABORT);
			AbortCurrentTransaction();
			break;

			/*
			 * Same as above, except the Abort() was already done.
			 */
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBENDABORT_RELEASE:
			CleanupSubTransaction();
			PopTransaction();
			s = CurrentTransactionState;		/* changed by pop */
			Assert(s->blockState != TBLOCK_SUBEND &&
				   s->blockState != TBLOCK_SUBENDABORT);
			AbortCurrentTransaction();
			break;

			/*
			 * We are already aborting the whole transaction tree. Do
			 * nothing, CommitTransactionCommand will call
			 * AbortOutOfAnyTransaction and set things straight.
			 */
		case TBLOCK_SUBENDABORT_ALL:
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
 *	completes).  Subtransactions are verboten too.
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
	 * subtransaction?
	 */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		/* translator: %s represents an SQL statement name */
				 errmsg("%s cannot run inside a subtransaction",
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
	if (CurrentTransactionState->blockState != TBLOCK_DEFAULT &&
		CurrentTransactionState->blockState != TBLOCK_STARTED)
		elog(FATAL, "cannot prevent transaction chain");
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
 *	use of the current statement's results.  Likewise subtransactions.
 *	Thus this is an inverse for PreventTransactionChain.
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
	 * subtransaction?
	 */
	if (IsSubTransaction())
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
 *	IsInTransactionChain
 *
 *	This routine is for statements that need to behave differently inside
 *	a transaction block than when running as single commands.  ANALYZE is
 *	currently the only example.
 *
 *	stmtNode: pointer to parameter block for statement; this is used in
 *	a very klugy way to determine whether we are inside a function.
 */
bool
IsInTransactionChain(void *stmtNode)
{
	/*
	 * Return true on same conditions that would make
	 * PreventTransactionChain error out
	 */
	if (IsTransactionBlock())
		return true;

	if (IsSubTransaction())
		return true;

	if (!MemoryContextContains(QueryContext, stmtNode))
		return true;

	if (CurrentTransactionState->blockState != TBLOCK_DEFAULT &&
		CurrentTransactionState->blockState != TBLOCK_STARTED)
		return true;

	return false;
}


/*
 * Register or deregister callback functions for start- and end-of-xact
 * operations.
 *
 * These functions are intended for use by dynamically loaded modules.
 * For built-in modules we generally just hardwire the appropriate calls
 * (mainly because it's easier to control the order that way, where needed).
 *
 * At transaction end, the callback occurs post-commit or post-abort, so the
 * callback functions can only do noncritical cleanup.	At subtransaction
 * start, the callback is called when the subtransaction has finished
 * initializing.
 */
void
RegisterXactCallback(XactCallback callback, void *arg)
{
	XactCallbackItem *item;

	item = (XactCallbackItem *)
		MemoryContextAlloc(TopMemoryContext, sizeof(XactCallbackItem));
	item->callback = callback;
	item->arg = arg;
	item->next = Xact_callbacks;
	Xact_callbacks = item;
}

void
UnregisterXactCallback(XactCallback callback, void *arg)
{
	XactCallbackItem *item;
	XactCallbackItem *prev;

	prev = NULL;
	for (item = Xact_callbacks; item; prev = item, item = item->next)
	{
		if (item->callback == callback && item->arg == arg)
		{
			if (prev)
				prev->next = item->next;
			else
				Xact_callbacks = item->next;
			pfree(item);
			break;
		}
	}
}

static void
CallXactCallbacks(XactEvent event, TransactionId parentXid)
{
	XactCallbackItem *item;

	for (item = Xact_callbacks; item; item = item->next)
		(*item->callback) (event, parentXid, item->arg);
}


/* ----------------------------------------------------------------
 *					   transaction block support
 * ----------------------------------------------------------------
 */

/*
 *	BeginTransactionBlock
 *		This executes a BEGIN command.
 */
void
BeginTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * We are not inside a transaction block, so allow one to
			 * begin.
			 */
		case TBLOCK_STARTED:
			s->blockState = TBLOCK_BEGIN;
			break;

			/*
			 * Already a transaction block in progress.
			 */
		case TBLOCK_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
			ereport(WARNING,
					(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				  errmsg("there is already a transaction in progress")));
			break;

			/* These cases are invalid.  Reject them altogether. */
		case TBLOCK_DEFAULT:
		case TBLOCK_BEGIN:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_ENDABORT:
		case TBLOCK_END:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
		case TBLOCK_SUBEND:
			elog(FATAL, "BeginTransactionBlock: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}
}

/*
 *	EndTransactionBlock
 *		This executes a COMMIT command.
 *
 * Since COMMIT may actually do a ROLLBACK, the result indicates what
 * happened: TRUE for COMMIT, FALSE for ROLLBACK.
 */
bool
EndTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;
	bool		result = false;

	switch (s->blockState)
	{
			/*
			 * We are in a transaction block which should commit when we
			 * get to the upcoming CommitTransactionCommand() so we set
			 * the state to "END".	CommitTransactionCommand() will
			 * recognize this and commit the transaction and return us to
			 * the default state.
			 */
		case TBLOCK_INPROGRESS:
			s->blockState = TBLOCK_END;
			result = true;
			break;

			/*
			 * We are in a transaction block which aborted. Since the
			 * AbortTransaction() was already done, we need only change to
			 * the special "END ABORT" state.  The upcoming
			 * CommitTransactionCommand() will recognise this and then put
			 * us back in the default state.
			 */
		case TBLOCK_ABORT:
			s->blockState = TBLOCK_ENDABORT;
			break;

			/*
			 * We are in a live subtransaction block.  Set up to subcommit
			 * all open subtransactions and then commit the main transaction.
			 */
		case TBLOCK_SUBINPROGRESS:
			while (s->parent != NULL)
			{
				Assert(s->blockState == TBLOCK_SUBINPROGRESS);
				s->blockState = TBLOCK_SUBEND;
				s = s->parent;
			}
			Assert(s->blockState == TBLOCK_INPROGRESS);
			s->blockState = TBLOCK_END;
			result = true;
			break;

			/*
			 * Here we are inside an aborted subtransaction.  Go to the
			 * "abort the whole tree" state so that
			 * CommitTransactionCommand() calls AbortOutOfAnyTransaction.
			 */
		case TBLOCK_SUBABORT:
			s->blockState = TBLOCK_SUBENDABORT_ALL;
			break;

		case TBLOCK_STARTED:

			/*
			 * here, the user issued COMMIT when not inside a transaction.
			 * Issue a WARNING and go to abort state.  The upcoming call
			 * to CommitTransactionCommand() will then put us back into
			 * the default state.
			 */
			ereport(WARNING,
					(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
					 errmsg("there is no transaction in progress")));
			AbortTransaction();
			s->blockState = TBLOCK_ENDABORT;
			break;

			/* these cases are invalid. */
		case TBLOCK_DEFAULT:
		case TBLOCK_BEGIN:
		case TBLOCK_ENDABORT:
		case TBLOCK_END:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_SUBEND:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
			elog(FATAL, "EndTransactionBlock: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	return result;
}

/*
 *	UserAbortTransactionBlock
 *		This executes a ROLLBACK command.
 */
void
UserAbortTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/*
			 * We are inside a failed transaction block and we got an
			 * abort command from the user.  Abort processing is already
			 * done, we just need to move to the ENDABORT state so we will
			 * end up in the default state after the upcoming
			 * CommitTransactionCommand().
			 */
		case TBLOCK_ABORT:
			s->blockState = TBLOCK_ENDABORT;
			break;

			/*
			 * We are inside a failed subtransaction and we got an abort
			 * command from the user.  Abort processing is already done,
			 * so go to the "abort all" state and CommitTransactionCommand
			 * will call AbortOutOfAnyTransaction to set things straight.
			 */
		case TBLOCK_SUBABORT:
			s->blockState = TBLOCK_SUBENDABORT_ALL;
			break;

			/*
			 * We are inside a transaction block and we got an abort
			 * command from the user, so we move to the ENDABORT state and
			 * do abort processing so we will end up in the default state
			 * after the upcoming CommitTransactionCommand().
			 */
		case TBLOCK_INPROGRESS:
			AbortTransaction();
			s->blockState = TBLOCK_ENDABORT;
			break;

			/*
			 * We are inside a subtransaction.	Abort the current
			 * subtransaction and go to the "abort all" state, so
			 * CommitTransactionCommand will call AbortOutOfAnyTransaction
			 * to set things straight.
			 */
		case TBLOCK_SUBINPROGRESS:
			AbortSubTransaction();
			s->blockState = TBLOCK_SUBENDABORT_ALL;
			break;

			/*
			 * The user issued ABORT when not inside a transaction. Issue
			 * a WARNING and go to abort state.  The upcoming call to
			 * CommitTransactionCommand() will then put us back into the
			 * default state.
			 */
		case TBLOCK_STARTED:
			ereport(WARNING,
					(errcode(ERRCODE_NO_ACTIVE_SQL_TRANSACTION),
					 errmsg("there is no transaction in progress")));
			AbortTransaction();
			s->blockState = TBLOCK_ENDABORT;
			break;

			/* These cases are invalid. */
		case TBLOCK_DEFAULT:
		case TBLOCK_BEGIN:
		case TBLOCK_END:
		case TBLOCK_ENDABORT:
		case TBLOCK_SUBEND:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
		case TBLOCK_SUBBEGIN:
			elog(FATAL, "UserAbortTransactionBlock: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}
}

/*
 * DefineSavepoint
 *		This executes a SAVEPOINT command.
 */
void
DefineSavepoint(char *name)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
		case TBLOCK_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
			/* Normal subtransaction start */
			PushTransaction();
			s = CurrentTransactionState;		/* changed by push */

			/*
			 * Note that we are allocating the savepoint name in the
			 * parent transaction's CurTransactionContext, since we don't
			 * yet have a transaction context for the new guy.
			 */
			s->name = MemoryContextStrdup(CurTransactionContext, name);
			s->blockState = TBLOCK_SUBBEGIN;
			break;

			/* These cases are invalid.  Reject them altogether. */
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
		case TBLOCK_ENDABORT:
		case TBLOCK_END:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
		case TBLOCK_SUBEND:
			elog(FATAL, "DefineSavepoint: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}
}

/*
 * ReleaseSavepoint
 *		This executes a RELEASE command.
 */
void
ReleaseSavepoint(List *options)
{
	TransactionState s = CurrentTransactionState;
	TransactionState target,
				xact;
	ListCell   *cell;
	char	   *name = NULL;

	/*
	 * Check valid block state transaction status.
	 */
	switch (s->blockState)
	{
		case TBLOCK_INPROGRESS:
		case TBLOCK_ABORT:
			ereport(ERROR,
					(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
					 errmsg("no such savepoint")));
			break;

			/*
			 * We are in a non-aborted subtransaction.	This is the only
			 * valid case.
			 */
		case TBLOCK_SUBINPROGRESS:
			break;

			/* these cases are invalid. */
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_ENDABORT:
		case TBLOCK_END:
		case TBLOCK_SUBABORT:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_SUBEND:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
			elog(FATAL, "ReleaseSavepoint: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	foreach(cell, options)
	{
		DefElem    *elem = lfirst(cell);

		if (strcmp(elem->defname, "savepoint_name") == 0)
			name = strVal(elem->arg);
	}

	Assert(PointerIsValid(name));

	for (target = s; PointerIsValid(target); target = target->parent)
	{
		if (PointerIsValid(target->name) && strcmp(target->name, name) == 0)
			break;
	}

	if (!PointerIsValid(target))
		ereport(ERROR,
				(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
				 errmsg("no such savepoint")));

	/* disallow crossing savepoint level boundaries */
	if (target->savepointLevel != s->savepointLevel)
		ereport(ERROR,
				(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
				 errmsg("no such savepoint")));

	/*
	 * Mark "commit pending" all subtransactions up to the target
	 * subtransaction.	The actual commits will happen when control gets
	 * to CommitTransactionCommand.
	 */
	xact = CurrentTransactionState;
	for (;;)
	{
		Assert(xact->blockState == TBLOCK_SUBINPROGRESS);
		xact->blockState = TBLOCK_SUBEND;
		if (xact == target)
			break;
		xact = xact->parent;
		Assert(PointerIsValid(xact));
	}
}

/*
 * RollbackToSavepoint
 *		This executes a ROLLBACK TO <savepoint> command.
 */
void
RollbackToSavepoint(List *options)
{
	TransactionState s = CurrentTransactionState;
	TransactionState target,
				xact;
	ListCell   *cell;
	char	   *name = NULL;

	switch (s->blockState)
	{
			/*
			 * We can't rollback to a savepoint if there is no saveopint
			 * defined.
			 */
		case TBLOCK_ABORT:
		case TBLOCK_INPROGRESS:
			ereport(ERROR,
					(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
					 errmsg("no such savepoint")));
			break;

			/*
			 * There is at least one savepoint, so proceed.
			 */
		case TBLOCK_SUBABORT:
		case TBLOCK_SUBINPROGRESS:

			/*
			 * Have to do AbortSubTransaction, but first check if this is
			 * the right subtransaction
			 */
			break;

			/* these cases are invalid. */
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
		case TBLOCK_BEGIN:
		case TBLOCK_END:
		case TBLOCK_ENDABORT:
		case TBLOCK_SUBEND:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
		case TBLOCK_SUBBEGIN:
			elog(FATAL, "RollbackToSavepoint: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	foreach(cell, options)
	{
		DefElem    *elem = lfirst(cell);

		if (strcmp(elem->defname, "savepoint_name") == 0)
			name = strVal(elem->arg);
	}

	Assert(PointerIsValid(name));

	for (target = s; PointerIsValid(target); target = target->parent)
	{
		if (PointerIsValid(target->name) && strcmp(target->name, name) == 0)
			break;
	}

	if (!PointerIsValid(target))
		ereport(ERROR,
				(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
				 errmsg("no such savepoint")));

	/* disallow crossing savepoint level boundaries */
	if (target->savepointLevel != s->savepointLevel)
		ereport(ERROR,
				(errcode(ERRCODE_S_E_INVALID_SPECIFICATION),
				 errmsg("no such savepoint")));

	/*
	 * Abort the current subtransaction, if needed.  We can't Cleanup the
	 * savepoint yet, so signal CommitTransactionCommand to do it and
	 * close all savepoints up to the target level.
	 */
	if (s->blockState == TBLOCK_SUBINPROGRESS)
		AbortSubTransaction();
	s->blockState = TBLOCK_SUBENDABORT;

	/*
	 * Mark "abort pending" all subtransactions up to the target
	 * subtransaction.	(Except the current subtransaction!)
	 */
	xact = CurrentTransactionState;

	while (xact != target)
	{
		xact = xact->parent;
		Assert(PointerIsValid(xact));
		Assert(xact->blockState == TBLOCK_SUBINPROGRESS);
		xact->blockState = TBLOCK_SUBABORT_PENDING;
	}
}

/*
 * BeginInternalSubTransaction
 *		This is the same as DefineSavepoint except it allows TBLOCK_STARTED
 *		state, and therefore it can safely be used in a function that might
 *		be called when not inside a BEGIN block.  Also, we automatically
 *		cycle through CommitTransactionCommand/StartTransactionCommand
 *		instead of expecting the caller to do it.
 *
 * Optionally, name can be NULL to create an unnamed savepoint.
 */
void
BeginInternalSubTransaction(char *name)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
		case TBLOCK_STARTED:
		case TBLOCK_INPROGRESS:
		case TBLOCK_SUBINPROGRESS:
			/* Normal subtransaction start */
			PushTransaction();
			s = CurrentTransactionState;		/* changed by push */

			/*
			 * Note that we are allocating the savepoint name in the
			 * parent transaction's CurTransactionContext, since we don't
			 * yet have a transaction context for the new guy.
			 */
			if (name)
				s->name = MemoryContextStrdup(CurTransactionContext, name);
			s->blockState = TBLOCK_SUBBEGIN;
			break;

			/* These cases are invalid.  Reject them altogether. */
		case TBLOCK_DEFAULT:
		case TBLOCK_BEGIN:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_ABORT:
		case TBLOCK_SUBABORT:
		case TBLOCK_ENDABORT:
		case TBLOCK_END:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
		case TBLOCK_SUBEND:
			elog(FATAL, "BeginInternalSubTransaction: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	CommitTransactionCommand();
	StartTransactionCommand();
}

/*
 * ReleaseCurrentSubTransaction
 *
 * RELEASE (ie, commit) the innermost subtransaction, regardless of its
 * savepoint name (if any).
 * NB: do NOT use CommitTransactionCommand/StartTransactionCommand with this.
 */
void
ReleaseCurrentSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState != TBLOCK_SUBINPROGRESS)
		elog(ERROR, "ReleaseCurrentSubTransaction: unexpected state %s",
			 BlockStateAsString(s->blockState));
	Assert(s->state == TRANS_INPROGRESS);
	MemoryContextSwitchTo(CurTransactionContext);
	CommitSubTransaction();
	PopTransaction();
	s = CurrentTransactionState; /* changed by pop */
	Assert(s->state == TRANS_INPROGRESS);
}

/*
 * RollbackAndReleaseCurrentSubTransaction
 *
 * ROLLBACK and RELEASE (ie, abort) the innermost subtransaction, regardless
 * of its savepoint name (if any).
 * NB: do NOT use CommitTransactionCommand/StartTransactionCommand with this.
 */
void
RollbackAndReleaseCurrentSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	switch (s->blockState)
	{
			/* Must be in a subtransaction */
		case TBLOCK_SUBABORT:
		case TBLOCK_SUBINPROGRESS:
			break;

			/* these cases are invalid. */
		case TBLOCK_DEFAULT:
		case TBLOCK_STARTED:
		case TBLOCK_ABORT:
		case TBLOCK_INPROGRESS:
		case TBLOCK_BEGIN:
		case TBLOCK_END:
		case TBLOCK_ENDABORT:
		case TBLOCK_SUBEND:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
		case TBLOCK_SUBBEGIN:
			elog(FATAL, "RollbackAndReleaseCurrentSubTransaction: unexpected state %s",
				 BlockStateAsString(s->blockState));
			break;
	}

	/*
	 * Abort the current subtransaction, if needed.
	 */
	if (s->blockState == TBLOCK_SUBINPROGRESS)
		AbortSubTransaction();
	s->blockState = TBLOCK_SUBENDABORT_RELEASE;

	/* And clean it up, too */
	CleanupAbortedSubTransactions(false);
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
	 * Get out of any transaction or nested transaction
	 */
	do
	{
		switch (s->blockState)
		{
			case TBLOCK_DEFAULT:
				/* Not in a transaction, do nothing */
				break;
			case TBLOCK_STARTED:
			case TBLOCK_BEGIN:
			case TBLOCK_INPROGRESS:
			case TBLOCK_END:
				/* In a transaction, so clean up */
				AbortTransaction();
				CleanupTransaction();
				s->blockState = TBLOCK_DEFAULT;
				break;
			case TBLOCK_ABORT:
			case TBLOCK_ENDABORT:
				/* AbortTransaction already done, still need Cleanup */
				CleanupTransaction();
				s->blockState = TBLOCK_DEFAULT;
				break;
			case TBLOCK_SUBBEGIN:

				/*
				 * We didn't get as far as starting the subxact, so
				 * there's nothing to abort.  Just pop back to parent.
				 */
				PopTransaction();
				s = CurrentTransactionState;	/* changed by pop */
				break;
			case TBLOCK_SUBINPROGRESS:
			case TBLOCK_SUBEND:
			case TBLOCK_SUBABORT_PENDING:

				/*
				 * In a subtransaction, so clean it up and abort parent
				 * too
				 */
				AbortSubTransaction();
				CleanupSubTransaction();
				PopTransaction();
				s = CurrentTransactionState;	/* changed by pop */
				break;
			case TBLOCK_SUBABORT:
			case TBLOCK_SUBENDABORT_ALL:
			case TBLOCK_SUBENDABORT:
			case TBLOCK_SUBENDABORT_RELEASE:
				/* As above, but AbortSubTransaction already done */
				CleanupSubTransaction();
				PopTransaction();
				s = CurrentTransactionState;	/* changed by pop */
				break;
		}
	} while (s->blockState != TBLOCK_DEFAULT);

	/* Should be out of all subxacts now */
	Assert(s->parent == NULL);
}

/*
 * IsTransactionBlock --- are we within a transaction block?
 */
bool
IsTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_DEFAULT || s->blockState == TBLOCK_STARTED)
		return false;

	return true;
}

/*
 * IsTransactionOrTransactionBlock --- are we within either a transaction
 * or a transaction block?	(The backend is only really "idle" when this
 * returns false.)
 *
 * This should match up with IsTransactionBlock and IsTransactionState.
 */
bool
IsTransactionOrTransactionBlock(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->blockState == TBLOCK_DEFAULT)
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
		case TBLOCK_STARTED:
			return 'I';			/* idle --- not in transaction */
		case TBLOCK_BEGIN:
		case TBLOCK_INPROGRESS:
		case TBLOCK_END:
		case TBLOCK_SUBINPROGRESS:
		case TBLOCK_SUBBEGIN:
		case TBLOCK_SUBEND:
			return 'T';			/* in transaction */
		case TBLOCK_ABORT:
		case TBLOCK_ENDABORT:
		case TBLOCK_SUBABORT:
		case TBLOCK_SUBENDABORT_ALL:
		case TBLOCK_SUBENDABORT:
		case TBLOCK_SUBABORT_PENDING:
		case TBLOCK_SUBENDABORT_RELEASE:
			return 'E';			/* in failed transaction */
	}

	/* should never get here */
	elog(FATAL, "invalid transaction block state: %s",
		 BlockStateAsString(s->blockState));
	return 0;					/* keep compiler quiet */
}

/*
 * IsSubTransaction
 */
bool
IsSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->nestingLevel >= 2)
		return true;

	return false;
}

/*
 * StartSubTransaction
 */
static void
StartSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->state != TRANS_DEFAULT)
		elog(WARNING, "StartSubTransaction while in %s state",
			 TransStateAsString(s->state));

	s->state = TRANS_START;

	/*
	 * must initialize resource-management stuff first
	 */
	AtSubStart_Memory();
	AtSubStart_ResourceOwner();

	/*
	 * Generate a new Xid and record it in pg_subtrans.  NB: we must make
	 * the subtrans entry BEFORE the Xid appears anywhere in shared
	 * storage, such as in the lock table; because until it's made the Xid
	 * may not appear to be "running" to other backends. See
	 * GetNewTransactionId.
	 */
	s->transactionIdData = GetNewTransactionId(true);

	SubTransSetParent(s->transactionIdData, s->parent->transactionIdData);

	XactLockTableInsert(s->transactionIdData);

	/*
	 * Finish setup of other transaction state fields.
	 */
	s->currentUser = GetUserId();
	s->prevXactReadOnly = XactReadOnly;

	/*
	 * Initialize other subsystems for new subtransaction
	 */
	AtSubStart_Inval();
	AtSubStart_Notify();
	AfterTriggerBeginSubXact();

	s->state = TRANS_INPROGRESS;

	/*
	 * Call start-of-subxact callbacks
	 */
	CallXactCallbacks(XACT_EVENT_START_SUB, s->parent->transactionIdData);

	ShowTransactionState("StartSubTransaction");
}

/*
 * CommitSubTransaction
 */
static void
CommitSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	ShowTransactionState("CommitSubTransaction");

	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "CommitSubTransaction while in %s state",
			 TransStateAsString(s->state));

	/* Pre-commit processing goes here -- nothing to do at the moment */

	s->state = TRANS_COMMIT;

	/* Mark subtransaction as subcommitted */
	CommandCounterIncrement();
	RecordSubTransactionCommit();
	AtSubCommit_childXids();

	/* Post-commit cleanup */
	AfterTriggerEndSubXact(true);
	AtSubCommit_Portals(s->parent->transactionIdData,
						s->parent->curTransactionOwner);
	AtEOSubXact_LargeObject(true, s->transactionIdData,
							s->parent->transactionIdData);
	AtSubCommit_Notify();
	AtEOSubXact_UpdatePasswordFile(true, s->transactionIdData,
								   s->parent->transactionIdData);

	CallXactCallbacks(XACT_EVENT_COMMIT_SUB, s->parent->transactionIdData);

	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 true, false);
	AtEOSubXact_RelationCache(true, s->transactionIdData,
							  s->parent->transactionIdData);
	AtEOSubXact_Inval(true);
	AtSubCommit_smgr();
	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_LOCKS,
						 true, false);
	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 true, false);

	AtEOXact_GUC(true, true);
	AtEOSubXact_SPI(true, s->transactionIdData);
	AtEOSubXact_on_commit_actions(true, s->transactionIdData,
								  s->parent->transactionIdData);
	AtEOSubXact_Namespace(true, s->transactionIdData,
						  s->parent->transactionIdData);
	AtEOSubXact_Files(true, s->transactionIdData,
					  s->parent->transactionIdData);

	/*
	 * We need to restore the upper transaction's read-only state, in case
	 * the upper is read-write while the child is read-only; GUC will
	 * incorrectly think it should leave the child state in place.
	 */
	XactReadOnly = s->prevXactReadOnly;

	CurrentResourceOwner = s->parent->curTransactionOwner;
	CurTransactionResourceOwner = s->parent->curTransactionOwner;
	ResourceOwnerDelete(s->curTransactionOwner);
	s->curTransactionOwner = NULL;

	AtSubCommit_Memory();

	s->state = TRANS_DEFAULT;
}

/*
 * AbortSubTransaction
 */
static void
AbortSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	ShowTransactionState("AbortSubTransaction");

	if (s->state != TRANS_INPROGRESS)
		elog(WARNING, "AbortSubTransaction while in %s state",
			 TransStateAsString(s->state));

	HOLD_INTERRUPTS();

	s->state = TRANS_ABORT;

	/*
	 * Release any LW locks we might be holding as quickly as possible.
	 * (Regular locks, however, must be held till we finish aborting.)
	 * Releasing LW locks is critical since we might try to grab them
	 * again while cleaning up!
	 *
	 * FIXME This may be incorrect --- Are there some locks we should keep?
	 * Buffer locks, for example?  I don't think so but I'm not sure.
	 */
	LWLockReleaseAll();

	AbortBufferIO();
	UnlockBuffers();

	LockWaitCancel();

	/*
	 * do abort processing
	 */
	AtSubAbort_Memory();

	AfterTriggerEndSubXact(false);
	AtSubAbort_Portals(s->parent->transactionIdData,
					   s->parent->curTransactionOwner);
	AtEOSubXact_LargeObject(false, s->transactionIdData,
							s->parent->transactionIdData);
	AtSubAbort_Notify();
	AtEOSubXact_UpdatePasswordFile(false, s->transactionIdData,
								   s->parent->transactionIdData);

	/* Advertise the fact that we aborted in pg_clog. */
	RecordSubTransactionAbort();

	/* Post-abort cleanup */
	CallXactCallbacks(XACT_EVENT_ABORT_SUB, s->parent->transactionIdData);

	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_BEFORE_LOCKS,
						 false, false);
	AtEOSubXact_RelationCache(false, s->transactionIdData,
							  s->parent->transactionIdData);
	AtEOSubXact_Inval(false);
	AtSubAbort_smgr();
	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_LOCKS,
						 false, false);
	ResourceOwnerRelease(s->curTransactionOwner,
						 RESOURCE_RELEASE_AFTER_LOCKS,
						 false, false);

	AtEOXact_GUC(false, true);
	AtEOSubXact_SPI(false, s->transactionIdData);
	AtEOSubXact_on_commit_actions(false, s->transactionIdData,
								  s->parent->transactionIdData);
	AtEOSubXact_Namespace(false, s->transactionIdData,
						  s->parent->transactionIdData);
	AtEOSubXact_Files(false, s->transactionIdData,
					  s->parent->transactionIdData);

	/*
	 * Reset user id which might have been changed transiently.  Here we
	 * want to restore to the userid that was current at subxact entry.
	 * (As in AbortTransaction, we need not worry about the session
	 * userid.)
	 *
	 * Must do this after AtEOXact_GUC to handle the case where we entered
	 * the subxact inside a SECURITY DEFINER function (hence current and
	 * session userids were different) and then session auth was changed
	 * inside the subxact.	GUC will reset both current and session
	 * userids to the entry-time session userid.  This is right in every
	 * other scenario so it seems simplest to let GUC do that and fix it
	 * here.
	 */
	SetUserId(s->currentUser);

	/*
	 * Restore the upper transaction's read-only state, too.  This should
	 * be redundant with GUC's cleanup but we may as well do it for
	 * consistency with the commit case.
	 */
	XactReadOnly = s->prevXactReadOnly;

	RESUME_INTERRUPTS();
}

/*
 * CleanupSubTransaction
 */
static void
CleanupSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	ShowTransactionState("CleanupSubTransaction");

	if (s->state != TRANS_ABORT)
		elog(WARNING, "CleanupSubTransaction while in %s state",
			 TransStateAsString(s->state));

	AtSubCleanup_Portals();

	CurrentResourceOwner = s->parent->curTransactionOwner;
	CurTransactionResourceOwner = s->parent->curTransactionOwner;
	ResourceOwnerDelete(s->curTransactionOwner);
	s->curTransactionOwner = NULL;

	AtSubCleanup_Memory();

	s->state = TRANS_DEFAULT;
}

/*
 * StartAbortedSubTransaction
 *
 * This function is used to start a subtransaction and put it immediately
 * into aborted state.	The end result should be equivalent to
 * StartSubTransaction immediately followed by AbortSubTransaction.
 * The reason we don't implement it just that way is that many of the backend
 * modules aren't designed to handle starting a subtransaction when not
 * inside a valid transaction.	Rather than making them all capable of
 * doing that, we just omit the paired start and abort calls in this path.
 */
static void
StartAbortedSubTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->state != TRANS_DEFAULT)
		elog(WARNING, "StartAbortedSubTransaction while in %s state",
			 TransStateAsString(s->state));

	s->state = TRANS_START;

	/*
	 * We don't bother to generate a new Xid, so the end state is not
	 * *exactly* like we had done a full Start/AbortSubTransaction...
	 */
	s->transactionIdData = InvalidTransactionId;

	/* Make sure currentUser is reasonably valid */
	Assert(s->parent != NULL);
	s->currentUser = s->parent->currentUser;

	/*
	 * Initialize only what has to be there for CleanupSubTransaction to
	 * work.
	 */
	AtSubStart_Memory();
	AtSubStart_ResourceOwner();

	s->state = TRANS_ABORT;

	AtSubAbort_Memory();

	ShowTransactionState("StartAbortedSubTransaction");
}

/*
 * PushTransaction
 *		Set up transaction state for a subtransaction
 *
 *	The caller has to make sure to always reassign CurrentTransactionState
 *	if it has a local pointer to it after calling this function.
 */
static void
PushTransaction(void)
{
	TransactionState p = CurrentTransactionState;
	TransactionState s;

	/*
	 * We keep subtransaction state nodes in TopTransactionContext.
	 */
	s = (TransactionState)
		MemoryContextAllocZero(TopTransactionContext,
							   sizeof(TransactionStateData));
	s->parent = p;
	s->nestingLevel = p->nestingLevel + 1;
	s->savepointLevel = p->savepointLevel;
	s->state = TRANS_DEFAULT;
	s->blockState = TBLOCK_SUBBEGIN;

	/* Command IDs count in a continuous sequence through subtransactions */
	s->commandId = p->commandId;

	/*
	 * Copy down some other data so that we will have valid state until
	 * StartSubTransaction runs.
	 */
	s->transactionIdData = p->transactionIdData;
	s->curTransactionContext = p->curTransactionContext;
	s->curTransactionOwner = p->curTransactionOwner;
	s->currentUser = p->currentUser;

	CurrentTransactionState = s;
}

/*
 * PopTransaction
 *		Pop back to parent transaction state
 *
 *	The caller has to make sure to always reassign CurrentTransactionState
 *	if it has a local pointer to it after calling this function.
 */
static void
PopTransaction(void)
{
	TransactionState s = CurrentTransactionState;

	if (s->state != TRANS_DEFAULT)
		elog(WARNING, "PopTransaction while in %s state",
			 TransStateAsString(s->state));

	if (s->parent == NULL)
		elog(FATAL, "PopTransaction with no parent");

	/* Command IDs count in a continuous sequence through subtransactions */
	s->parent->commandId = s->commandId;

	CurrentTransactionState = s->parent;

	/* Let's just make sure CurTransactionContext is good */
	CurTransactionContext = s->parent->curTransactionContext;
	MemoryContextSwitchTo(CurTransactionContext);

	/* Ditto for ResourceOwner links */
	CurTransactionResourceOwner = s->parent->curTransactionOwner;
	CurrentResourceOwner = s->parent->curTransactionOwner;

	/* Free the old child structure */
	if (s->name)
		pfree(s->name);
	pfree(s);
}

/*
 * ShowTransactionState
 *		Debug support
 */
static void
ShowTransactionState(const char *str)
{
	/* skip work if message will definitely not be printed */
	if (log_min_messages <= DEBUG2 || client_min_messages <= DEBUG2)
	{
		elog(DEBUG2, "%s", str);
		ShowTransactionStateRec(CurrentTransactionState);
	}
}

/*
 * ShowTransactionStateRec
 *		Recursive subroutine for ShowTransactionState
 */
static void
ShowTransactionStateRec(TransactionState s)
{
	if (s->parent)
		ShowTransactionStateRec(s->parent);

	/* use ereport to suppress computation if msg will not be printed */
	ereport(DEBUG2,
			(errmsg_internal("name: %s; blockState: %13s; state: %7s, xid/cid: %u/%02u, nestlvl: %d, children: %s",
						   PointerIsValid(s->name) ? s->name : "unnamed",
							 BlockStateAsString(s->blockState),
							 TransStateAsString(s->state),
							 (unsigned int) s->transactionIdData,
							 (unsigned int) s->commandId,
							 s->nestingLevel,
							 nodeToString(s->childXids))));
}

/*
 * BlockStateAsString
 *		Debug support
 */
static const char *
BlockStateAsString(TBlockState blockState)
{
	switch (blockState)
	{
		case TBLOCK_DEFAULT:
			return "DEFAULT";
		case TBLOCK_STARTED:
			return "STARTED";
		case TBLOCK_BEGIN:
			return "BEGIN";
		case TBLOCK_INPROGRESS:
			return "INPROGRESS";
		case TBLOCK_END:
			return "END";
		case TBLOCK_ABORT:
			return "ABORT";
		case TBLOCK_ENDABORT:
			return "ENDABORT";
		case TBLOCK_SUBBEGIN:
			return "SUB BEGIN";
		case TBLOCK_SUBINPROGRESS:
			return "SUB INPROGRS";
		case TBLOCK_SUBEND:
			return "SUB END";
		case TBLOCK_SUBABORT:
			return "SUB ABORT";
		case TBLOCK_SUBENDABORT_ALL:
			return "SUB ENDAB ALL";
		case TBLOCK_SUBENDABORT:
			return "SUB ENDAB";
		case TBLOCK_SUBABORT_PENDING:
			return "SUB ABRT PEND";
		case TBLOCK_SUBENDABORT_RELEASE:
			return "SUB ENDAB REL";
	}
	return "UNRECOGNIZED";
}

/*
 * TransStateAsString
 *		Debug support
 */
static const char *
TransStateAsString(TransState state)
{
	switch (state)
	{
		case TRANS_DEFAULT:
			return "DEFAULT";
		case TRANS_START:
			return "START";
		case TRANS_COMMIT:
			return "COMMIT";
		case TRANS_ABORT:
			return "ABORT";
		case TRANS_INPROGRESS:
			return "INPROGR";
	}
	return "UNRECOGNIZED";
}

/*
 * xactGetCommittedChildren
 *
 * Gets the list of committed children of the current transaction.	The return
 * value is the number of child transactions.  *children is set to point to a
 * palloc'd array of TransactionIds.  If there are no subxacts, *children is
 * set to NULL.
 */
int
xactGetCommittedChildren(TransactionId **ptr)
{
	TransactionState s = CurrentTransactionState;
	int			nchildren;
	TransactionId *children;
	ListCell   *p;

	nchildren = list_length(s->childXids);
	if (nchildren == 0)
	{
		*ptr = NULL;
		return 0;
	}

	children = (TransactionId *) palloc(nchildren * sizeof(TransactionId));
	*ptr = children;

	foreach(p, s->childXids)
	{
		TransactionId child = lfirst_xid(p);

		*children++ = child;
	}

	return nchildren;
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
		xl_xact_commit *xlrec = (xl_xact_commit *) XLogRecGetData(record);
		TransactionId *sub_xids;
		TransactionId max_xid;
		int			i;

		TransactionIdCommit(record->xl_xid);

		/* Mark committed subtransactions as committed */
		sub_xids = (TransactionId *) &(xlrec->xnodes[xlrec->nrels]);
		TransactionIdCommitTree(xlrec->nsubxacts, sub_xids);

		/* Make sure nextXid is beyond any XID mentioned in the record */
		max_xid = record->xl_xid;
		for (i = 0; i < xlrec->nsubxacts; i++)
		{
			if (TransactionIdPrecedes(max_xid, sub_xids[i]))
				max_xid = sub_xids[i];
		}
		if (TransactionIdFollowsOrEquals(max_xid,
										 ShmemVariableCache->nextXid))
		{
			ShmemVariableCache->nextXid = max_xid;
			TransactionIdAdvance(ShmemVariableCache->nextXid);
		}

		/* Make sure files supposed to be dropped are dropped */
		for (i = 0; i < xlrec->nrels; i++)
		{
			XLogCloseRelation(xlrec->xnodes[i]);
			smgrdounlink(smgropen(xlrec->xnodes[i]), false, true);
		}
	}
	else if (info == XLOG_XACT_ABORT)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) XLogRecGetData(record);
		TransactionId *sub_xids;
		TransactionId max_xid;
		int			i;

		TransactionIdAbort(record->xl_xid);

		/* Mark subtransactions as aborted */
		sub_xids = (TransactionId *) &(xlrec->xnodes[xlrec->nrels]);
		TransactionIdAbortTree(xlrec->nsubxacts, sub_xids);

		/* Make sure nextXid is beyond any XID mentioned in the record */
		max_xid = record->xl_xid;
		for (i = 0; i < xlrec->nsubxacts; i++)
		{
			if (TransactionIdPrecedes(max_xid, sub_xids[i]))
				max_xid = sub_xids[i];
		}
		if (TransactionIdFollowsOrEquals(max_xid,
										 ShmemVariableCache->nextXid))
		{
			ShmemVariableCache->nextXid = max_xid;
			TransactionIdAdvance(ShmemVariableCache->nextXid);
		}

		/* Make sure files supposed to be dropped are dropped */
		for (i = 0; i < xlrec->nrels; i++)
		{
			XLogCloseRelation(xlrec->xnodes[i]);
			smgrdounlink(smgropen(xlrec->xnodes[i]), false, true);
		}
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
	int			i;

	if (info == XLOG_XACT_COMMIT)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) rec;
		struct tm  *tm = localtime(&xlrec->xtime);

		sprintf(buf + strlen(buf), "commit: %04u-%02u-%02u %02u:%02u:%02u",
				tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
				tm->tm_hour, tm->tm_min, tm->tm_sec);
		if (xlrec->nrels > 0)
		{
			sprintf(buf + strlen(buf), "; rels:");
			for (i = 0; i < xlrec->nrels; i++)
			{
				RelFileNode rnode = xlrec->xnodes[i];

				sprintf(buf + strlen(buf), " %u/%u/%u",
						rnode.spcNode, rnode.dbNode, rnode.relNode);
			}
		}
		if (xlrec->nsubxacts > 0)
		{
			TransactionId *xacts = (TransactionId *)
			&xlrec->xnodes[xlrec->nrels];

			sprintf(buf + strlen(buf), "; subxacts:");
			for (i = 0; i < xlrec->nsubxacts; i++)
				sprintf(buf + strlen(buf), " %u", xacts[i]);
		}
	}
	else if (info == XLOG_XACT_ABORT)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) rec;
		struct tm  *tm = localtime(&xlrec->xtime);

		sprintf(buf + strlen(buf), "abort: %04u-%02u-%02u %02u:%02u:%02u",
				tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
				tm->tm_hour, tm->tm_min, tm->tm_sec);
		if (xlrec->nrels > 0)
		{
			sprintf(buf + strlen(buf), "; rels:");
			for (i = 0; i < xlrec->nrels; i++)
			{
				RelFileNode rnode = xlrec->xnodes[i];

				sprintf(buf + strlen(buf), " %u/%u/%u",
						rnode.spcNode, rnode.dbNode, rnode.relNode);
			}
		}
		if (xlrec->nsubxacts > 0)
		{
			TransactionId *xacts = (TransactionId *)
			&xlrec->xnodes[xlrec->nrels];

			sprintf(buf + strlen(buf), "; subxacts:");
			for (i = 0; i < xlrec->nsubxacts; i++)
				sprintf(buf + strlen(buf), " %u", xacts[i]);
		}
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
