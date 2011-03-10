/*-------------------------------------------------------------------------
 *
 * syncrep.c
 *
 * Synchronous replication is new as of PostgreSQL 9.1.
 *
 * If requested, transaction commits wait until their commit LSN is
 * acknowledged by the sync standby.
 *
 * This module contains the code for waiting and release of backends.
 * All code in this module executes on the primary. The core streaming
 * replication transport remains within WALreceiver/WALsender modules.
 *
 * The essence of this design is that it isolates all logic about
 * waiting/releasing onto the primary. The primary defines which standbys
 * it wishes to wait for. The standby is completely unaware of the
 * durability requirements of transactions on the primary, reducing the
 * complexity of the code and streamlining both standby operations and
 * network bandwidth because there is no requirement to ship
 * per-transaction state information.
 *
 * Replication is either synchronous or not synchronous (async). If it is
 * async, we just fastpath out of here. If it is sync, then in 9.1 we wait
 * for the flush location on the standby before releasing the waiting backend.
 * Further complexity in that interaction is expected in later releases.
 *
 * The best performing way to manage the waiting backends is to have a
 * single ordered queue of waiting backends, so that we can avoid
 * searching the through all waiters each time we receive a reply.
 *
 * In 9.1 we support only a single synchronous standby, chosen from a
 * priority list of synchronous_standby_names. Before it can become the
 * synchronous standby it must have caught up with the primary; that may
 * take some time. Once caught up, the current highest priority standby
 * will release waiters from the queue.
 *
 * Portions Copyright (c) 2010-2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/syncrep.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "access/xlog_internal.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "replication/syncrep.h"
#include "replication/walsender.h"
#include "storage/latch.h"
#include "storage/ipc.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"

/* User-settable parameters for sync rep */
bool	sync_rep_mode = false;			/* Only set in user backends */
char 	*SyncRepStandbyNames;

static bool sync_standbys_defined = false;	/* Is there at least one name? */
static bool announce_next_takeover = true;

static void SyncRepQueueInsert(void);

static int SyncRepGetStandbyPriority(void);
#ifdef USE_ASSERT_CHECKING
static bool SyncRepQueueIsOrderedByLSN(void);
#endif

/*
 * ===========================================================
 * Synchronous Replication functions for normal user backends
 * ===========================================================
 */

/*
 * Wait for synchronous replication, if requested by user.
 */
void
SyncRepWaitForLSN(XLogRecPtr XactCommitLSN)
{
	char 		*new_status = NULL;
	const char *old_status;

	/*
	 * Fast exit if user has not requested sync replication, or
	 * there are no sync replication standby names defined.
	 * Note that those standbys don't need to be connected.
	 */
	if (!SyncRepRequested() || !sync_standbys_defined)
		return;

	Assert(SHMQueueIsDetached(&(MyProc->syncRepLinks)));

	/*
	 * Wait for specified LSN to be confirmed.
	 *
	 * Each proc has its own wait latch, so we perform a normal latch
	 * check/wait loop here.
	 */
	for (;;)
	{
		ResetLatch(&MyProc->waitLatch);

		/*
		 * Synchronous Replication state machine within user backend
		 *
		 * Initially backends start in state SYNC_REP_NOT_WAITING and then
		 * change that state to SYNC_REP_WAITING before adding ourselves
		 * to the wait queue. During SyncRepWakeQueue() a WALSender changes
		 * the state to SYNC_REP_WAIT_COMPLETE once replication is confirmed.
		 * This backend then resets its state to SYNC_REP_NOT_WAITING when
		 * we exit normally, or SYNC_REP_MUST_DISCONNECT in abnormal cases.
		 *
		 * We read MyProc->syncRepState without SyncRepLock, which
		 * assumes that read access is atomic.
		 */
		switch (MyProc->syncRepState)
		{
			case SYNC_REP_NOT_WAITING:
				/*
				 * Set our waitLSN so WALSender will know when to wake us.
				 * We set this before we add ourselves to queue, so that
				 * any proc on the queue can be examined freely without
				 * taking a lock on each process in the queue, as long as
				 * they hold SyncRepLock.
				 */
				MyProc->waitLSN = XactCommitLSN;
				MyProc->syncRepState = SYNC_REP_WAITING;

				/*
				 * Add to queue while holding lock.
				 */
				LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
				SyncRepQueueInsert();
				Assert(SyncRepQueueIsOrderedByLSN());
				LWLockRelease(SyncRepLock);

				/*
				 * Alter ps display to show waiting for sync rep.
				 */
				if (update_process_title)
				{
					int			len;

					old_status = get_ps_display(&len);
					new_status = (char *) palloc(len + 32 + 1);
					memcpy(new_status, old_status, len);
					sprintf(new_status + len, " waiting for %X/%X",
						 XactCommitLSN.xlogid, XactCommitLSN.xrecoff);
					set_ps_display(new_status, false);
					new_status[len] = '\0'; /* truncate off " waiting ..." */
				}

				break;

			case SYNC_REP_WAITING:
				/*
				 * Check for conditions that would cause us to leave the
				 * wait state before the LSN has been reached.
				 */
				if (!PostmasterIsAlive(true))
				{
					LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
					SHMQueueDelete(&(MyProc->syncRepLinks));
					LWLockRelease(SyncRepLock);

					MyProc->syncRepState = SYNC_REP_MUST_DISCONNECT;
					return;
				}

				/*
				 * We don't receive SIGHUPs at this point, so resetting
				 * synchronous_standby_names has no effect on waiters.
				 */

				/* Continue waiting */

				break;

			case SYNC_REP_WAIT_COMPLETE:
				/*
				 * WalSender has checked our LSN and has removed us from
				 * queue. Cleanup local state and leave.
				 */
				Assert(SHMQueueIsDetached(&(MyProc->syncRepLinks)));

				MyProc->syncRepState = SYNC_REP_NOT_WAITING;
				MyProc->waitLSN.xlogid = 0;
				MyProc->waitLSN.xrecoff = 0;

				if (new_status)
				{
					/* Reset ps display */
					set_ps_display(new_status, false);
					pfree(new_status);
				}

				return;

			case SYNC_REP_MUST_DISCONNECT:
				return;

			default:
				elog(FATAL, "invalid syncRepState");
		}

		/*
		 * Wait on latch for up to 60 seconds. This allows us to
		 * check for postmaster death regularly while waiting.
		 * Note that timeout here does not necessarily release from loop.
		 */
		WaitLatch(&MyProc->waitLatch, 60000000L);
	}
}

/*
 * Insert MyProc into SyncRepQueue, maintaining sorted invariant.
 *
 * Usually we will go at tail of queue, though its possible that we arrive
 * here out of order, so start at tail and work back to insertion point.
 */
static void
SyncRepQueueInsert(void)
{
	PGPROC	*proc;

	proc = (PGPROC *) SHMQueuePrev(&(WalSndCtl->SyncRepQueue),
								   &(WalSndCtl->SyncRepQueue),
								   offsetof(PGPROC, syncRepLinks));

	while (proc)
	{
		/*
		 * Stop at the queue element that we should after to
		 * ensure the queue is ordered by LSN.
		 */
		if (XLByteLT(proc->waitLSN, MyProc->waitLSN))
			break;

		proc = (PGPROC *) SHMQueuePrev(&(WalSndCtl->SyncRepQueue),
									   &(proc->syncRepLinks),
									   offsetof(PGPROC, syncRepLinks));
	}

	if (proc)
		SHMQueueInsertAfter(&(proc->syncRepLinks), &(MyProc->syncRepLinks));
	else
		SHMQueueInsertAfter(&(WalSndCtl->SyncRepQueue), &(MyProc->syncRepLinks));
}

void
SyncRepCleanupAtProcExit(int code, Datum arg)
{
	if (!SHMQueueIsDetached(&(MyProc->syncRepLinks)))
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
		SHMQueueDelete(&(MyProc->syncRepLinks));
		LWLockRelease(SyncRepLock);
	}

	DisownLatch(&MyProc->waitLatch);
}

/*
 * ===========================================================
 * Synchronous Replication functions for wal sender processes
 * ===========================================================
 */

/*
 * Take any action required to initialise sync rep state from config
 * data. Called at WALSender startup and after each SIGHUP.
 */
void
SyncRepInitConfig(void)
{
	int priority;

	/*
	 * Determine if we are a potential sync standby and remember the result
	 * for handling replies from standby.
	 */
	priority = SyncRepGetStandbyPriority();
	if (MyWalSnd->sync_standby_priority != priority)
	{
		LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);
		MyWalSnd->sync_standby_priority = priority;
		LWLockRelease(SyncRepLock);
		ereport(DEBUG1,
				(errmsg("standby \"%s\" now has synchronous standby priority %u",
						application_name, priority)));
	}
}

/*
 * Update the LSNs on each queue based upon our latest state. This
 * implements a simple policy of first-valid-standby-releases-waiter.
 *
 * Other policies are possible, which would change what we do here and what
 * perhaps also which information we store as well.
 */
void
SyncRepReleaseWaiters(void)
{
	volatile WalSndCtlData *walsndctl = WalSndCtl;
	volatile WalSnd *syncWalSnd = NULL;
	int 		numprocs = 0;
	int			priority = 0;
	int			i;

	/*
	 * If this WALSender is serving a standby that is not on the list of
	 * potential standbys then we have nothing to do. If we are still
	 * starting up or still running base backup, then leave quickly also.
	 */
	if (MyWalSnd->sync_standby_priority == 0 ||
		MyWalSnd->state < WALSNDSTATE_STREAMING)
		return;

	/*
	 * We're a potential sync standby. Release waiters if we are the
	 * highest priority standby. If there are multiple standbys with
	 * same priorities then we use the first mentioned standby.
	 * If you change this, also change pg_stat_get_wal_senders().
	 */
	LWLockAcquire(SyncRepLock, LW_EXCLUSIVE);

	for (i = 0; i < max_wal_senders; i++)
	{
		/* use volatile pointer to prevent code rearrangement */
		volatile WalSnd *walsnd = &walsndctl->walsnds[i];

		if (walsnd->pid != 0 &&
			walsnd->sync_standby_priority > 0 &&
			(priority == 0 ||
			 priority > walsnd->sync_standby_priority))
		{
			 priority = walsnd->sync_standby_priority;
			 syncWalSnd = walsnd;
		}
	}

	/*
	 * We should have found ourselves at least.
	 */
	Assert(syncWalSnd);

	/*
	 * If we aren't managing the highest priority standby then just leave.
	 */
	if (syncWalSnd != MyWalSnd)
	{
		LWLockRelease(SyncRepLock);
		announce_next_takeover = true;
		return;
	}

	if (XLByteLT(walsndctl->lsn, MyWalSnd->flush))
	{
		/*
		 * Set the lsn first so that when we wake backends they will
		 * release up to this location.
		 */
		walsndctl->lsn = MyWalSnd->flush;
		numprocs = SyncRepWakeQueue(false);
	}

	LWLockRelease(SyncRepLock);

	elog(DEBUG3, "released %d procs up to %X/%X",
					numprocs,
					MyWalSnd->flush.xlogid,
					MyWalSnd->flush.xrecoff);

	/*
	 * If we are managing the highest priority standby, though we weren't
	 * prior to this, then announce we are now the sync standby.
	 */
	if (announce_next_takeover)
	{
		announce_next_takeover = false;
		ereport(LOG,
				(errmsg("standby \"%s\" is now the synchronous standby with priority %u",
						application_name, MyWalSnd->sync_standby_priority)));
	}
}

/*
 * Check if we are in the list of sync standbys, and if so, determine
 * priority sequence. Return priority if set, or zero to indicate that
 * we are not a potential sync standby.
 *
 * Compare the parameter SyncRepStandbyNames against the application_name
 * for this WALSender, or allow any name if we find a wildcard "*".
 */
static int
SyncRepGetStandbyPriority(void)
{
	char	   *rawstring;
	List	   *elemlist;
	ListCell   *l;
	int			priority = 0;
	bool		found = false;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(SyncRepStandbyNames);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		pfree(rawstring);
		list_free(elemlist);
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("invalid list syntax for parameter \"synchronous_standby_names\"")));
		return 0;
	}

	foreach(l, elemlist)
	{
		char	   *standby_name = (char *) lfirst(l);

		priority++;

		if (pg_strcasecmp(standby_name, application_name) == 0 ||
			pg_strcasecmp(standby_name, "*") == 0)
		{
			found = true;
			break;
		}
	}

	pfree(rawstring);
	list_free(elemlist);

	return (found ? priority : 0);
}

/*
 * Walk queue from head setting setting the state of any backends that
 * need to be woken, remove them from the queue and then wake them.
 * Set all = true to wake whole queue, or just up to LSN.
 *
 * Must hold SyncRepLock.
 */
int
SyncRepWakeQueue(bool all)
{
	volatile WalSndCtlData *walsndctl = WalSndCtl;
	PGPROC	*proc = NULL;
	PGPROC	*thisproc = NULL;
	int		numprocs = 0;

	Assert(SyncRepQueueIsOrderedByLSN());

	proc = (PGPROC *) SHMQueueNext(&(WalSndCtl->SyncRepQueue),
								   &(WalSndCtl->SyncRepQueue),
								   offsetof(PGPROC, syncRepLinks));

	while (proc)
	{
		/*
		 * Assume the queue is ordered by LSN
		 */
		if (!all && XLByteLT(walsndctl->lsn, proc->waitLSN))
			return numprocs;

		/*
		 * Move to next proc, so we can delete thisproc from the queue.
		 * thisproc is valid, proc may be NULL after this.
		 */
		thisproc = proc;
		proc = (PGPROC *) SHMQueueNext(&(WalSndCtl->SyncRepQueue),
									   &(proc->syncRepLinks),
									   offsetof(PGPROC, syncRepLinks));

		/*
		 * Set state to complete; see SyncRepWaitForLSN() for discussion
		 * of the various states.
		 */
		thisproc->syncRepState = SYNC_REP_WAIT_COMPLETE;

		/*
		 * Remove thisproc from queue.
		 */
		SHMQueueDelete(&(thisproc->syncRepLinks));

		/*
		 * Wake only when we have set state and removed from queue.
		 */
		Assert(SHMQueueIsDetached(&(thisproc->syncRepLinks)));
		Assert(thisproc->syncRepState == SYNC_REP_WAIT_COMPLETE);
		SetLatch(&(thisproc->waitLatch));

		numprocs++;
	}

	return numprocs;
}

#ifdef USE_ASSERT_CHECKING
static bool
SyncRepQueueIsOrderedByLSN(void)
{
	PGPROC	*proc = NULL;
	XLogRecPtr lastLSN;

	lastLSN.xlogid = 0;
	lastLSN.xrecoff = 0;

	proc = (PGPROC *) SHMQueueNext(&(WalSndCtl->SyncRepQueue),
								   &(WalSndCtl->SyncRepQueue),
								   offsetof(PGPROC, syncRepLinks));

	while (proc)
	{
		/*
		 * Check the queue is ordered by LSN and that multiple
		 * procs don't have matching LSNs
		 */
		if (XLByteLE(proc->waitLSN, lastLSN))
			return false;

		lastLSN = proc->waitLSN;

		proc = (PGPROC *) SHMQueueNext(&(WalSndCtl->SyncRepQueue),
									   &(proc->syncRepLinks),
									   offsetof(PGPROC, syncRepLinks));
	}

	return true;
}
#endif

/*
 * ===========================================================
 * Synchronous Replication functions executed by any process
 * ===========================================================
 */

const char *
assign_synchronous_standby_names(const char *newval, bool doit, GucSource source)
{
	char	   *rawstring;
	List	   *elemlist;

	/* Need a modifiable copy of string */
	rawstring = pstrdup(newval);

	/* Parse string into list of identifiers */
	if (!SplitIdentifierString(rawstring, ',', &elemlist))
	{
		/* syntax error in list */
		pfree(rawstring);
		list_free(elemlist);
		ereport(FATAL,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg("invalid list syntax for parameter \"synchronous_standby_names\"")));
		return NULL;
	}

	/*
	 * Is there at least one sync standby? If so cache this knowledge to
	 * improve performance of SyncRepWaitForLSN() for all-async configs.
	 */
	if (doit && list_length(elemlist) > 0)
		sync_standbys_defined = true;

	/*
	 * Any additional validation of standby names should go here.
	 *
	 * Don't attempt to set WALSender priority because this is executed by
	 * postmaster at startup, not WALSender, so the application_name is
	 * not yet correctly set.
	 */

	pfree(rawstring);
	list_free(elemlist);

	return newval;
}
