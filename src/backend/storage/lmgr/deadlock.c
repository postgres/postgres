/*-------------------------------------------------------------------------
 *
 * deadlock.c
 *	  POSTGRES deadlock detection code
 *
 * See src/backend/storage/lmgr/README for a description of the deadlock
 * detection and resolution algorithms.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/deadlock.c
 *
 *	Interface:
 *
 *	DeadLockCheck()
 *	DeadLockReport()
 *	RememberSimpleDeadLock()
 *	InitDeadLockChecking()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "utils/memutils.h"


/*
 * One edge in the waits-for graph.
 *
 * waiter and blocker may or may not be members of a lock group, but if either
 * is, it will be the leader rather than any other member of the lock group.
 * The group leaders act as representatives of the whole group even though
 * those particular processes need not be waiting at all.  There will be at
 * least one member of the waiter's lock group on the wait queue for the given
 * lock, maybe more.
 */
typedef struct
{
	PGPROC	   *waiter;			/* the leader of the waiting lock group */
	PGPROC	   *blocker;		/* the leader of the group it is waiting for */
	LOCK	   *lock;			/* the lock being waited for */
	int			pred;			/* workspace for TopoSort */
	int			link;			/* workspace for TopoSort */
} EDGE;

/* One potential reordering of a lock's wait queue */
typedef struct
{
	LOCK	   *lock;			/* the lock whose wait queue is described */
	PGPROC	  **procs;			/* array of PGPROC *'s in new wait order */
	int			nProcs;
} WAIT_ORDER;

/*
 * Information saved about each edge in a detected deadlock cycle.  This
 * is used to print a diagnostic message upon failure.
 *
 * Note: because we want to examine this info after releasing the lock
 * manager's partition locks, we can't just store LOCK and PGPROC pointers;
 * we must extract out all the info we want to be able to print.
 */
typedef struct
{
	LOCKTAG		locktag;		/* ID of awaited lock object */
	LOCKMODE	lockmode;		/* type of lock we're waiting for */
	int			pid;			/* PID of blocked backend */
} DEADLOCK_INFO;


static bool DeadLockCheckRecurse(PGPROC *proc);
static int	TestConfiguration(PGPROC *startProc);
static bool FindLockCycle(PGPROC *checkProc,
						  EDGE *softEdges, int *nSoftEdges);
static bool FindLockCycleRecurse(PGPROC *checkProc, int depth,
								 EDGE *softEdges, int *nSoftEdges);
static bool FindLockCycleRecurseMember(PGPROC *checkProc,
									   PGPROC *checkProcLeader,
									   int depth, EDGE *softEdges, int *nSoftEdges);
static bool ExpandConstraints(EDGE *constraints, int nConstraints);
static bool TopoSort(LOCK *lock, EDGE *constraints, int nConstraints,
					 PGPROC **ordering);

#ifdef DEBUG_DEADLOCK
static void PrintLockQueue(LOCK *lock, const char *info);
#endif


/*
 * Working space for the deadlock detector
 */

/* Workspace for FindLockCycle */
static PGPROC **visitedProcs;	/* Array of visited procs */
static int	nVisitedProcs;

/* Workspace for TopoSort */
static PGPROC **topoProcs;		/* Array of not-yet-output procs */
static int *beforeConstraints;	/* Counts of remaining before-constraints */
static int *afterConstraints;	/* List head for after-constraints */

/* Output area for ExpandConstraints */
static WAIT_ORDER *waitOrders;	/* Array of proposed queue rearrangements */
static int	nWaitOrders;
static PGPROC **waitOrderProcs; /* Space for waitOrders queue contents */

/* Current list of constraints being considered */
static EDGE *curConstraints;
static int	nCurConstraints;
static int	maxCurConstraints;

/* Storage space for results from FindLockCycle */
static EDGE *possibleConstraints;
static int	nPossibleConstraints;
static int	maxPossibleConstraints;
static DEADLOCK_INFO *deadlockDetails;
static int	nDeadlockDetails;

/* PGPROC pointer of any blocking autovacuum worker found */
static PGPROC *blocking_autovacuum_proc = NULL;


/*
 * InitDeadLockChecking -- initialize deadlock checker during backend startup
 *
 * This does per-backend initialization of the deadlock checker; primarily,
 * allocation of working memory for DeadLockCheck.  We do this per-backend
 * since there's no percentage in making the kernel do copy-on-write
 * inheritance of workspace from the postmaster.  We want to allocate the
 * space at startup because (a) the deadlock checker might be invoked when
 * there's no free memory left, and (b) the checker is normally run inside a
 * signal handler, which is a very dangerous place to invoke palloc from.
 */
void
InitDeadLockChecking(void)
{
	MemoryContext oldcxt;

	/* Make sure allocations are permanent */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * FindLockCycle needs at most MaxBackends entries in visitedProcs[] and
	 * deadlockDetails[].
	 */
	visitedProcs = (PGPROC **) palloc(MaxBackends * sizeof(PGPROC *));
	deadlockDetails = (DEADLOCK_INFO *) palloc(MaxBackends * sizeof(DEADLOCK_INFO));

	/*
	 * TopoSort needs to consider at most MaxBackends wait-queue entries, and
	 * it needn't run concurrently with FindLockCycle.
	 */
	topoProcs = visitedProcs;	/* re-use this space */
	beforeConstraints = (int *) palloc(MaxBackends * sizeof(int));
	afterConstraints = (int *) palloc(MaxBackends * sizeof(int));

	/*
	 * We need to consider rearranging at most MaxBackends/2 wait queues
	 * (since it takes at least two waiters in a queue to create a soft edge),
	 * and the expanded form of the wait queues can't involve more than
	 * MaxBackends total waiters.
	 */
	waitOrders = (WAIT_ORDER *)
		palloc((MaxBackends / 2) * sizeof(WAIT_ORDER));
	waitOrderProcs = (PGPROC **) palloc(MaxBackends * sizeof(PGPROC *));

	/*
	 * Allow at most MaxBackends distinct constraints in a configuration. (Is
	 * this enough?  In practice it seems it should be, but I don't quite see
	 * how to prove it.  If we run out, we might fail to find a workable wait
	 * queue rearrangement even though one exists.)  NOTE that this number
	 * limits the maximum recursion depth of DeadLockCheckRecurse. Making it
	 * really big might potentially allow a stack-overflow problem.
	 */
	maxCurConstraints = MaxBackends;
	curConstraints = (EDGE *) palloc(maxCurConstraints * sizeof(EDGE));

	/*
	 * Allow up to 3*MaxBackends constraints to be saved without having to
	 * re-run TestConfiguration.  (This is probably more than enough, but we
	 * can survive if we run low on space by doing excess runs of
	 * TestConfiguration to re-compute constraint lists each time needed.) The
	 * last MaxBackends entries in possibleConstraints[] are reserved as
	 * output workspace for FindLockCycle.
	 */
	maxPossibleConstraints = MaxBackends * 4;
	possibleConstraints =
		(EDGE *) palloc(maxPossibleConstraints * sizeof(EDGE));

	MemoryContextSwitchTo(oldcxt);
}

/*
 * DeadLockCheck -- Checks for deadlocks for a given process
 *
 * This code looks for deadlocks involving the given process.  If any
 * are found, it tries to rearrange lock wait queues to resolve the
 * deadlock.  If resolution is impossible, return DS_HARD_DEADLOCK ---
 * the caller is then expected to abort the given proc's transaction.
 *
 * Caller must already have locked all partitions of the lock tables.
 *
 * On failure, deadlock details are recorded in deadlockDetails[] for
 * subsequent printing by DeadLockReport().  That activity is separate
 * because (a) we don't want to do it while holding all those LWLocks,
 * and (b) we are typically invoked inside a signal handler.
 */
DeadLockState
DeadLockCheck(PGPROC *proc)
{
	/* Initialize to "no constraints" */
	nCurConstraints = 0;
	nPossibleConstraints = 0;
	nWaitOrders = 0;

	/* Initialize to not blocked by an autovacuum worker */
	blocking_autovacuum_proc = NULL;

	/* Search for deadlocks and possible fixes */
	if (DeadLockCheckRecurse(proc))
	{
		/*
		 * Call FindLockCycle one more time, to record the correct
		 * deadlockDetails[] for the basic state with no rearrangements.
		 */
		int			nSoftEdges;

		TRACE_POSTGRESQL_DEADLOCK_FOUND();

		nWaitOrders = 0;
		if (!FindLockCycle(proc, possibleConstraints, &nSoftEdges))
			elog(FATAL, "deadlock seems to have disappeared");

		return DS_HARD_DEADLOCK;	/* cannot find a non-deadlocked state */
	}

	/* Apply any needed rearrangements of wait queues */
	for (int i = 0; i < nWaitOrders; i++)
	{
		LOCK	   *lock = waitOrders[i].lock;
		PGPROC	  **procs = waitOrders[i].procs;
		int			nProcs = waitOrders[i].nProcs;
		dclist_head *waitQueue = &lock->waitProcs;

		Assert(nProcs == dclist_count(waitQueue));

#ifdef DEBUG_DEADLOCK
		PrintLockQueue(lock, "DeadLockCheck:");
#endif

		/* Reset the queue and re-add procs in the desired order */
		dclist_init(waitQueue);
		for (int j = 0; j < nProcs; j++)
			dclist_push_tail(waitQueue, &procs[j]->links);

#ifdef DEBUG_DEADLOCK
		PrintLockQueue(lock, "rearranged to:");
#endif

		/* See if any waiters for the lock can be woken up now */
		ProcLockWakeup(GetLocksMethodTable(lock), lock);
	}

	/* Return code tells caller if we had to escape a deadlock or not */
	if (nWaitOrders > 0)
		return DS_SOFT_DEADLOCK;
	else if (blocking_autovacuum_proc != NULL)
		return DS_BLOCKED_BY_AUTOVACUUM;
	else
		return DS_NO_DEADLOCK;
}

/*
 * Return the PGPROC of the autovacuum that's blocking a process.
 *
 * We reset the saved pointer as soon as we pass it back.
 */
PGPROC *
GetBlockingAutoVacuumPgproc(void)
{
	PGPROC	   *ptr;

	ptr = blocking_autovacuum_proc;
	blocking_autovacuum_proc = NULL;

	return ptr;
}

/*
 * DeadLockCheckRecurse -- recursively search for valid orderings
 *
 * curConstraints[] holds the current set of constraints being considered
 * by an outer level of recursion.  Add to this each possible solution
 * constraint for any cycle detected at this level.
 *
 * Returns true if no solution exists.  Returns false if a deadlock-free
 * state is attainable, in which case waitOrders[] shows the required
 * rearrangements of lock wait queues (if any).
 */
static bool
DeadLockCheckRecurse(PGPROC *proc)
{
	int			nEdges;
	int			oldPossibleConstraints;
	bool		savedList;
	int			i;

	nEdges = TestConfiguration(proc);
	if (nEdges < 0)
		return true;			/* hard deadlock --- no solution */
	if (nEdges == 0)
		return false;			/* good configuration found */
	if (nCurConstraints >= maxCurConstraints)
		return true;			/* out of room for active constraints? */
	oldPossibleConstraints = nPossibleConstraints;
	if (nPossibleConstraints + nEdges + MaxBackends <= maxPossibleConstraints)
	{
		/* We can save the edge list in possibleConstraints[] */
		nPossibleConstraints += nEdges;
		savedList = true;
	}
	else
	{
		/* Not room; will need to regenerate the edges on-the-fly */
		savedList = false;
	}

	/*
	 * Try each available soft edge as an addition to the configuration.
	 */
	for (i = 0; i < nEdges; i++)
	{
		if (!savedList && i > 0)
		{
			/* Regenerate the list of possible added constraints */
			if (nEdges != TestConfiguration(proc))
				elog(FATAL, "inconsistent results during deadlock check");
		}
		curConstraints[nCurConstraints] =
			possibleConstraints[oldPossibleConstraints + i];
		nCurConstraints++;
		if (!DeadLockCheckRecurse(proc))
			return false;		/* found a valid solution! */
		/* give up on that added constraint, try again */
		nCurConstraints--;
	}
	nPossibleConstraints = oldPossibleConstraints;
	return true;				/* no solution found */
}


/*--------------------
 * Test a configuration (current set of constraints) for validity.
 *
 * Returns:
 *		0: the configuration is good (no deadlocks)
 *	   -1: the configuration has a hard deadlock or is not self-consistent
 *		>0: the configuration has one or more soft deadlocks
 *
 * In the soft-deadlock case, one of the soft cycles is chosen arbitrarily
 * and a list of its soft edges is returned beginning at
 * possibleConstraints+nPossibleConstraints.  The return value is the
 * number of soft edges.
 *--------------------
 */
static int
TestConfiguration(PGPROC *startProc)
{
	int			softFound = 0;
	EDGE	   *softEdges = possibleConstraints + nPossibleConstraints;
	int			nSoftEdges;
	int			i;

	/*
	 * Make sure we have room for FindLockCycle's output.
	 */
	if (nPossibleConstraints + MaxBackends > maxPossibleConstraints)
		return -1;

	/*
	 * Expand current constraint set into wait orderings.  Fail if the
	 * constraint set is not self-consistent.
	 */
	if (!ExpandConstraints(curConstraints, nCurConstraints))
		return -1;

	/*
	 * Check for cycles involving startProc or any of the procs mentioned in
	 * constraints.  We check startProc last because if it has a soft cycle
	 * still to be dealt with, we want to deal with that first.
	 */
	for (i = 0; i < nCurConstraints; i++)
	{
		if (FindLockCycle(curConstraints[i].waiter, softEdges, &nSoftEdges))
		{
			if (nSoftEdges == 0)
				return -1;		/* hard deadlock detected */
			softFound = nSoftEdges;
		}
		if (FindLockCycle(curConstraints[i].blocker, softEdges, &nSoftEdges))
		{
			if (nSoftEdges == 0)
				return -1;		/* hard deadlock detected */
			softFound = nSoftEdges;
		}
	}
	if (FindLockCycle(startProc, softEdges, &nSoftEdges))
	{
		if (nSoftEdges == 0)
			return -1;			/* hard deadlock detected */
		softFound = nSoftEdges;
	}
	return softFound;
}


/*
 * FindLockCycle -- basic check for deadlock cycles
 *
 * Scan outward from the given proc to see if there is a cycle in the
 * waits-for graph that includes this proc.  Return true if a cycle
 * is found, else false.  If a cycle is found, we return a list of
 * the "soft edges", if any, included in the cycle.  These edges could
 * potentially be eliminated by rearranging wait queues.  We also fill
 * deadlockDetails[] with information about the detected cycle; this info
 * is not used by the deadlock algorithm itself, only to print a useful
 * message after failing.
 *
 * Since we need to be able to check hypothetical configurations that would
 * exist after wait queue rearrangement, the routine pays attention to the
 * table of hypothetical queue orders in waitOrders[].  These orders will
 * be believed in preference to the actual ordering seen in the locktable.
 */
static bool
FindLockCycle(PGPROC *checkProc,
			  EDGE *softEdges,	/* output argument */
			  int *nSoftEdges)	/* output argument */
{
	nVisitedProcs = 0;
	nDeadlockDetails = 0;
	*nSoftEdges = 0;
	return FindLockCycleRecurse(checkProc, 0, softEdges, nSoftEdges);
}

static bool
FindLockCycleRecurse(PGPROC *checkProc,
					 int depth,
					 EDGE *softEdges,	/* output argument */
					 int *nSoftEdges)	/* output argument */
{
	int			i;
	dlist_iter	iter;

	/*
	 * If this process is a lock group member, check the leader instead. (Note
	 * that we might be the leader, in which case this is a no-op.)
	 */
	if (checkProc->lockGroupLeader != NULL)
		checkProc = checkProc->lockGroupLeader;

	/*
	 * Have we already seen this proc?
	 */
	for (i = 0; i < nVisitedProcs; i++)
	{
		if (visitedProcs[i] == checkProc)
		{
			/* If we return to starting point, we have a deadlock cycle */
			if (i == 0)
			{
				/*
				 * record total length of cycle --- outer levels will now fill
				 * deadlockDetails[]
				 */
				Assert(depth <= MaxBackends);
				nDeadlockDetails = depth;

				return true;
			}

			/*
			 * Otherwise, we have a cycle but it does not include the start
			 * point, so say "no deadlock".
			 */
			return false;
		}
	}
	/* Mark proc as seen */
	Assert(nVisitedProcs < MaxBackends);
	visitedProcs[nVisitedProcs++] = checkProc;

	/*
	 * If the process is waiting, there is an outgoing waits-for edge to each
	 * process that blocks it.
	 */
	if (checkProc->links.next != NULL && checkProc->waitLock != NULL &&
		FindLockCycleRecurseMember(checkProc, checkProc, depth, softEdges,
								   nSoftEdges))
		return true;

	/*
	 * If the process is not waiting, there could still be outgoing waits-for
	 * edges if it is part of a lock group, because other members of the lock
	 * group might be waiting even though this process is not.  (Given lock
	 * groups {A1, A2} and {B1, B2}, if A1 waits for B1 and B2 waits for A2,
	 * that is a deadlock even neither of B1 and A2 are waiting for anything.)
	 */
	dlist_foreach(iter, &checkProc->lockGroupMembers)
	{
		PGPROC	   *memberProc;

		memberProc = dlist_container(PGPROC, lockGroupLink, iter.cur);

		if (memberProc->links.next != NULL && memberProc->waitLock != NULL &&
			memberProc != checkProc &&
			FindLockCycleRecurseMember(memberProc, checkProc, depth, softEdges,
									   nSoftEdges))
			return true;
	}

	return false;
}

static bool
FindLockCycleRecurseMember(PGPROC *checkProc,
						   PGPROC *checkProcLeader,
						   int depth,
						   EDGE *softEdges, /* output argument */
						   int *nSoftEdges) /* output argument */
{
	PGPROC	   *proc;
	LOCK	   *lock = checkProc->waitLock;
	dlist_iter	proclock_iter;
	LockMethod	lockMethodTable;
	int			conflictMask;
	int			i;
	int			numLockModes,
				lm;

	/*
	 * The relation extension lock can never participate in actual deadlock
	 * cycle.  See Assert in LockAcquireExtended.  So, there is no advantage
	 * in checking wait edges from it.
	 */
	if (LOCK_LOCKTAG(*lock) == LOCKTAG_RELATION_EXTEND)
		return false;

	lockMethodTable = GetLocksMethodTable(lock);
	numLockModes = lockMethodTable->numLockModes;
	conflictMask = lockMethodTable->conflictTab[checkProc->waitLockMode];

	/*
	 * Scan for procs that already hold conflicting locks.  These are "hard"
	 * edges in the waits-for graph.
	 */
	dlist_foreach(proclock_iter, &lock->procLocks)
	{
		PROCLOCK   *proclock = dlist_container(PROCLOCK, lockLink, proclock_iter.cur);
		PGPROC	   *leader;

		proc = proclock->tag.myProc;
		leader = proc->lockGroupLeader == NULL ? proc : proc->lockGroupLeader;

		/* A proc never blocks itself or any other lock group member */
		if (leader != checkProcLeader)
		{
			for (lm = 1; lm <= numLockModes; lm++)
			{
				if ((proclock->holdMask & LOCKBIT_ON(lm)) &&
					(conflictMask & LOCKBIT_ON(lm)))
				{
					/* This proc hard-blocks checkProc */
					if (FindLockCycleRecurse(proc, depth + 1,
											 softEdges, nSoftEdges))
					{
						/* fill deadlockDetails[] */
						DEADLOCK_INFO *info = &deadlockDetails[depth];

						info->locktag = lock->tag;
						info->lockmode = checkProc->waitLockMode;
						info->pid = checkProc->pid;

						return true;
					}

					/*
					 * No deadlock here, but see if this proc is an autovacuum
					 * that is directly hard-blocking our own proc.  If so,
					 * report it so that the caller can send a cancel signal
					 * to it, if appropriate.  If there's more than one such
					 * proc, it's indeterminate which one will be reported.
					 *
					 * We don't touch autovacuums that are indirectly blocking
					 * us; it's up to the direct blockee to take action.  This
					 * rule simplifies understanding the behavior and ensures
					 * that an autovacuum won't be canceled with less than
					 * deadlock_timeout grace period.
					 *
					 * Note we read statusFlags without any locking.  This is
					 * OK only for checking the PROC_IS_AUTOVACUUM flag,
					 * because that flag is set at process start and never
					 * reset.  There is logic elsewhere to avoid canceling an
					 * autovacuum that is working to prevent XID wraparound
					 * problems (which needs to read a different statusFlags
					 * bit), but we don't do that here to avoid grabbing
					 * ProcArrayLock.
					 */
					if (checkProc == MyProc &&
						proc->statusFlags & PROC_IS_AUTOVACUUM)
						blocking_autovacuum_proc = proc;

					/* We're done looking at this proclock */
					break;
				}
			}
		}
	}

	/*
	 * Scan for procs that are ahead of this one in the lock's wait queue.
	 * Those that have conflicting requests soft-block this one.  This must be
	 * done after the hard-block search, since if another proc both hard- and
	 * soft-blocks this one, we want to call it a hard edge.
	 *
	 * If there is a proposed re-ordering of the lock's wait order, use that
	 * rather than the current wait order.
	 */
	for (i = 0; i < nWaitOrders; i++)
	{
		if (waitOrders[i].lock == lock)
			break;
	}

	if (i < nWaitOrders)
	{
		/* Use the given hypothetical wait queue order */
		PGPROC	  **procs = waitOrders[i].procs;
		int			queue_size = waitOrders[i].nProcs;

		for (i = 0; i < queue_size; i++)
		{
			PGPROC	   *leader;

			proc = procs[i];
			leader = proc->lockGroupLeader == NULL ? proc :
				proc->lockGroupLeader;

			/*
			 * TopoSort will always return an ordering with group members
			 * adjacent to each other in the wait queue (see comments
			 * therein). So, as soon as we reach a process in the same lock
			 * group as checkProc, we know we've found all the conflicts that
			 * precede any member of the lock group lead by checkProcLeader.
			 */
			if (leader == checkProcLeader)
				break;

			/* Is there a conflict with this guy's request? */
			if ((LOCKBIT_ON(proc->waitLockMode) & conflictMask) != 0)
			{
				/* This proc soft-blocks checkProc */
				if (FindLockCycleRecurse(proc, depth + 1,
										 softEdges, nSoftEdges))
				{
					/* fill deadlockDetails[] */
					DEADLOCK_INFO *info = &deadlockDetails[depth];

					info->locktag = lock->tag;
					info->lockmode = checkProc->waitLockMode;
					info->pid = checkProc->pid;

					/*
					 * Add this edge to the list of soft edges in the cycle
					 */
					Assert(*nSoftEdges < MaxBackends);
					softEdges[*nSoftEdges].waiter = checkProcLeader;
					softEdges[*nSoftEdges].blocker = leader;
					softEdges[*nSoftEdges].lock = lock;
					(*nSoftEdges)++;
					return true;
				}
			}
		}
	}
	else
	{
		PGPROC	   *lastGroupMember = NULL;
		dlist_iter	proc_iter;
		dclist_head *waitQueue;

		/* Use the true lock wait queue order */
		waitQueue = &lock->waitProcs;

		/*
		 * Find the last member of the lock group that is present in the wait
		 * queue.  Anything after this is not a soft lock conflict. If group
		 * locking is not in use, then we know immediately which process we're
		 * looking for, but otherwise we've got to search the wait queue to
		 * find the last process actually present.
		 */
		if (checkProc->lockGroupLeader == NULL)
			lastGroupMember = checkProc;
		else
		{
			dclist_foreach(proc_iter, waitQueue)
			{
				proc = dlist_container(PGPROC, links, proc_iter.cur);

				if (proc->lockGroupLeader == checkProcLeader)
					lastGroupMember = proc;
			}
			Assert(lastGroupMember != NULL);
		}

		/*
		 * OK, now rescan (or scan) the queue to identify the soft conflicts.
		 */
		dclist_foreach(proc_iter, waitQueue)
		{
			PGPROC	   *leader;

			proc = dlist_container(PGPROC, links, proc_iter.cur);

			leader = proc->lockGroupLeader == NULL ? proc :
				proc->lockGroupLeader;

			/* Done when we reach the target proc */
			if (proc == lastGroupMember)
				break;

			/* Is there a conflict with this guy's request? */
			if ((LOCKBIT_ON(proc->waitLockMode) & conflictMask) != 0 &&
				leader != checkProcLeader)
			{
				/* This proc soft-blocks checkProc */
				if (FindLockCycleRecurse(proc, depth + 1,
										 softEdges, nSoftEdges))
				{
					/* fill deadlockDetails[] */
					DEADLOCK_INFO *info = &deadlockDetails[depth];

					info->locktag = lock->tag;
					info->lockmode = checkProc->waitLockMode;
					info->pid = checkProc->pid;

					/*
					 * Add this edge to the list of soft edges in the cycle
					 */
					Assert(*nSoftEdges < MaxBackends);
					softEdges[*nSoftEdges].waiter = checkProcLeader;
					softEdges[*nSoftEdges].blocker = leader;
					softEdges[*nSoftEdges].lock = lock;
					(*nSoftEdges)++;
					return true;
				}
			}
		}
	}

	/*
	 * No conflict detected here.
	 */
	return false;
}


/*
 * ExpandConstraints -- expand a list of constraints into a set of
 *		specific new orderings for affected wait queues
 *
 * Input is a list of soft edges to be reversed.  The output is a list
 * of nWaitOrders WAIT_ORDER structs in waitOrders[], with PGPROC array
 * workspace in waitOrderProcs[].
 *
 * Returns true if able to build an ordering that satisfies all the
 * constraints, false if not (there are contradictory constraints).
 */
static bool
ExpandConstraints(EDGE *constraints,
				  int nConstraints)
{
	int			nWaitOrderProcs = 0;
	int			i,
				j;

	nWaitOrders = 0;

	/*
	 * Scan constraint list backwards.  This is because the last-added
	 * constraint is the only one that could fail, and so we want to test it
	 * for inconsistency first.
	 */
	for (i = nConstraints; --i >= 0;)
	{
		LOCK	   *lock = constraints[i].lock;

		/* Did we already make a list for this lock? */
		for (j = nWaitOrders; --j >= 0;)
		{
			if (waitOrders[j].lock == lock)
				break;
		}
		if (j >= 0)
			continue;
		/* No, so allocate a new list */
		waitOrders[nWaitOrders].lock = lock;
		waitOrders[nWaitOrders].procs = waitOrderProcs + nWaitOrderProcs;
		waitOrders[nWaitOrders].nProcs = dclist_count(&lock->waitProcs);
		nWaitOrderProcs += dclist_count(&lock->waitProcs);
		Assert(nWaitOrderProcs <= MaxBackends);

		/*
		 * Do the topo sort.  TopoSort need not examine constraints after this
		 * one, since they must be for different locks.
		 */
		if (!TopoSort(lock, constraints, i + 1,
					  waitOrders[nWaitOrders].procs))
			return false;
		nWaitOrders++;
	}
	return true;
}


/*
 * TopoSort -- topological sort of a wait queue
 *
 * Generate a re-ordering of a lock's wait queue that satisfies given
 * constraints about certain procs preceding others.  (Each such constraint
 * is a fact of a partial ordering.)  Minimize rearrangement of the queue
 * not needed to achieve the partial ordering.
 *
 * This is a lot simpler and slower than, for example, the topological sort
 * algorithm shown in Knuth's Volume 1.  However, Knuth's method doesn't
 * try to minimize the damage to the existing order.  In practice we are
 * not likely to be working with more than a few constraints, so the apparent
 * slowness of the algorithm won't really matter.
 *
 * The initial queue ordering is taken directly from the lock's wait queue.
 * The output is an array of PGPROC pointers, of length equal to the lock's
 * wait queue length (the caller is responsible for providing this space).
 * The partial order is specified by an array of EDGE structs.  Each EDGE
 * is one that we need to reverse, therefore the "waiter" must appear before
 * the "blocker" in the output array.  The EDGE array may well contain
 * edges associated with other locks; these should be ignored.
 *
 * Returns true if able to build an ordering that satisfies all the
 * constraints, false if not (there are contradictory constraints).
 */
static bool
TopoSort(LOCK *lock,
		 EDGE *constraints,
		 int nConstraints,
		 PGPROC **ordering)		/* output argument */
{
	dclist_head *waitQueue = &lock->waitProcs;
	int			queue_size = dclist_count(waitQueue);
	PGPROC	   *proc;
	int			i,
				j,
				jj,
				k,
				kk,
				last;
	dlist_iter	proc_iter;

	/* First, fill topoProcs[] array with the procs in their current order */
	i = 0;
	dclist_foreach(proc_iter, waitQueue)
	{
		proc = dlist_container(PGPROC, links, proc_iter.cur);
		topoProcs[i++] = proc;
	}
	Assert(i == queue_size);

	/*
	 * Scan the constraints, and for each proc in the array, generate a count
	 * of the number of constraints that say it must be before something else,
	 * plus a list of the constraints that say it must be after something
	 * else. The count for the j'th proc is stored in beforeConstraints[j],
	 * and the head of its list in afterConstraints[j].  Each constraint
	 * stores its list link in constraints[i].link (note any constraint will
	 * be in just one list). The array index for the before-proc of the i'th
	 * constraint is remembered in constraints[i].pred.
	 *
	 * Note that it's not necessarily the case that every constraint affects
	 * this particular wait queue.  Prior to group locking, a process could be
	 * waiting for at most one lock.  But a lock group can be waiting for
	 * zero, one, or multiple locks.  Since topoProcs[] is an array of the
	 * processes actually waiting, while constraints[] is an array of group
	 * leaders, we've got to scan through topoProcs[] for each constraint,
	 * checking whether both a waiter and a blocker for that group are
	 * present.  If so, the constraint is relevant to this wait queue; if not,
	 * it isn't.
	 */
	MemSet(beforeConstraints, 0, queue_size * sizeof(int));
	MemSet(afterConstraints, 0, queue_size * sizeof(int));
	for (i = 0; i < nConstraints; i++)
	{
		/*
		 * Find a representative process that is on the lock queue and part of
		 * the waiting lock group.  This may or may not be the leader, which
		 * may or may not be waiting at all.  If there are any other processes
		 * in the same lock group on the queue, set their number of
		 * beforeConstraints to -1 to indicate that they should be emitted
		 * with their groupmates rather than considered separately.
		 *
		 * In this loop and the similar one just below, it's critical that we
		 * consistently select the same representative member of any one lock
		 * group, so that all the constraints are associated with the same
		 * proc, and the -1's are only associated with not-representative
		 * members.  We select the last one in the topoProcs array.
		 */
		proc = constraints[i].waiter;
		Assert(proc != NULL);
		jj = -1;
		for (j = queue_size; --j >= 0;)
		{
			PGPROC	   *waiter = topoProcs[j];

			if (waiter == proc || waiter->lockGroupLeader == proc)
			{
				Assert(waiter->waitLock == lock);
				if (jj == -1)
					jj = j;
				else
				{
					Assert(beforeConstraints[j] <= 0);
					beforeConstraints[j] = -1;
				}
			}
		}

		/* If no matching waiter, constraint is not relevant to this lock. */
		if (jj < 0)
			continue;

		/*
		 * Similarly, find a representative process that is on the lock queue
		 * and waiting for the blocking lock group.  Again, this could be the
		 * leader but does not need to be.
		 */
		proc = constraints[i].blocker;
		Assert(proc != NULL);
		kk = -1;
		for (k = queue_size; --k >= 0;)
		{
			PGPROC	   *blocker = topoProcs[k];

			if (blocker == proc || blocker->lockGroupLeader == proc)
			{
				Assert(blocker->waitLock == lock);
				if (kk == -1)
					kk = k;
				else
				{
					Assert(beforeConstraints[k] <= 0);
					beforeConstraints[k] = -1;
				}
			}
		}

		/* If no matching blocker, constraint is not relevant to this lock. */
		if (kk < 0)
			continue;

		Assert(beforeConstraints[jj] >= 0);
		beforeConstraints[jj]++;	/* waiter must come before */
		/* add this constraint to list of after-constraints for blocker */
		constraints[i].pred = jj;
		constraints[i].link = afterConstraints[kk];
		afterConstraints[kk] = i + 1;
	}

	/*--------------------
	 * Now scan the topoProcs array backwards.  At each step, output the
	 * last proc that has no remaining before-constraints plus any other
	 * members of the same lock group; then decrease the beforeConstraints
	 * count of each of the procs it was constrained against.
	 * i = index of ordering[] entry we want to output this time
	 * j = search index for topoProcs[]
	 * k = temp for scanning constraint list for proc j
	 * last = last non-null index in topoProcs (avoid redundant searches)
	 *--------------------
	 */
	last = queue_size - 1;
	for (i = queue_size - 1; i >= 0;)
	{
		int			c;
		int			nmatches = 0;

		/* Find next candidate to output */
		while (topoProcs[last] == NULL)
			last--;
		for (j = last; j >= 0; j--)
		{
			if (topoProcs[j] != NULL && beforeConstraints[j] == 0)
				break;
		}

		/* If no available candidate, topological sort fails */
		if (j < 0)
			return false;

		/*
		 * Output everything in the lock group.  There's no point in
		 * outputting an ordering where members of the same lock group are not
		 * consecutive on the wait queue: if some other waiter is between two
		 * requests that belong to the same group, then either it conflicts
		 * with both of them and is certainly not a solution; or it conflicts
		 * with at most one of them and is thus isomorphic to an ordering
		 * where the group members are consecutive.
		 */
		proc = topoProcs[j];
		if (proc->lockGroupLeader != NULL)
			proc = proc->lockGroupLeader;
		Assert(proc != NULL);
		for (c = 0; c <= last; ++c)
		{
			if (topoProcs[c] == proc || (topoProcs[c] != NULL &&
										 topoProcs[c]->lockGroupLeader == proc))
			{
				ordering[i - nmatches] = topoProcs[c];
				topoProcs[c] = NULL;
				++nmatches;
			}
		}
		Assert(nmatches > 0);
		i -= nmatches;

		/* Update beforeConstraints counts of its predecessors */
		for (k = afterConstraints[j]; k > 0; k = constraints[k - 1].link)
			beforeConstraints[constraints[k - 1].pred]--;
	}

	/* Done */
	return true;
}

#ifdef DEBUG_DEADLOCK
static void
PrintLockQueue(LOCK *lock, const char *info)
{
	dclist_head *waitQueue = &lock->waitProcs;
	dlist_iter	proc_iter;

	printf("%s lock %p queue ", info, lock);

	dclist_foreach(proc_iter, waitQueue)
	{
		PGPROC	   *proc = dlist_container(PGPROC, links, proc_iter.cur);

		printf(" %d", proc->pid);
	}
	printf("\n");
	fflush(stdout);
}
#endif

/*
 * Report a detected deadlock, with available details.
 */
void
DeadLockReport(void)
{
	StringInfoData clientbuf;	/* errdetail for client */
	StringInfoData logbuf;		/* errdetail for server log */
	StringInfoData locktagbuf;
	int			i;

	initStringInfo(&clientbuf);
	initStringInfo(&logbuf);
	initStringInfo(&locktagbuf);

	/* Generate the "waits for" lines sent to the client */
	for (i = 0; i < nDeadlockDetails; i++)
	{
		DEADLOCK_INFO *info = &deadlockDetails[i];
		int			nextpid;

		/* The last proc waits for the first one... */
		if (i < nDeadlockDetails - 1)
			nextpid = info[1].pid;
		else
			nextpid = deadlockDetails[0].pid;

		/* reset locktagbuf to hold next object description */
		resetStringInfo(&locktagbuf);

		DescribeLockTag(&locktagbuf, &info->locktag);

		if (i > 0)
			appendStringInfoChar(&clientbuf, '\n');

		appendStringInfo(&clientbuf,
						 _("Process %d waits for %s on %s; blocked by process %d."),
						 info->pid,
						 GetLockmodeName(info->locktag.locktag_lockmethodid,
										 info->lockmode),
						 locktagbuf.data,
						 nextpid);
	}

	/* Duplicate all the above for the server ... */
	appendBinaryStringInfo(&logbuf, clientbuf.data, clientbuf.len);

	/* ... and add info about query strings */
	for (i = 0; i < nDeadlockDetails; i++)
	{
		DEADLOCK_INFO *info = &deadlockDetails[i];

		appendStringInfoChar(&logbuf, '\n');

		appendStringInfo(&logbuf,
						 _("Process %d: %s"),
						 info->pid,
						 pgstat_get_backend_current_activity(info->pid, false));
	}

	pgstat_report_deadlock();

	ereport(ERROR,
			(errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
			 errmsg("deadlock detected"),
			 errdetail_internal("%s", clientbuf.data),
			 errdetail_log("%s", logbuf.data),
			 errhint("See server log for query details.")));
}

/*
 * RememberSimpleDeadLock: set up info for DeadLockReport when ProcSleep
 * detects a trivial (two-way) deadlock.  proc1 wants to block for lockmode
 * on lock, but proc2 is already waiting and would be blocked by proc1.
 */
void
RememberSimpleDeadLock(PGPROC *proc1,
					   LOCKMODE lockmode,
					   LOCK *lock,
					   PGPROC *proc2)
{
	DEADLOCK_INFO *info = &deadlockDetails[0];

	info->locktag = lock->tag;
	info->lockmode = lockmode;
	info->pid = proc1->pid;
	info++;
	info->locktag = proc2->waitLock->tag;
	info->lockmode = proc2->waitLockMode;
	info->pid = proc2->pid;
	nDeadlockDetails = 2;
}
