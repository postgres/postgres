/*-------------------------------------------------------------------------
 *
 * deadlock.c
 *	  POSTGRES deadlock detection code
 *
 * See src/backend/storage/lmgr/README for a description of the deadlock
 * detection and resolution algorithms.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/lmgr/deadlock.c,v 1.25 2003/09/25 06:58:02 petere Exp $
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

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/memutils.h"


/* One edge in the waits-for graph */
typedef struct
{
	PGPROC	   *waiter;			/* the waiting process */
	PGPROC	   *blocker;		/* the process it is waiting for */
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
 * Information saved about each edge in a detected deadlock cycle.	This
 * is used to print a diagnostic message upon failure.
 *
 * Note: because we want to examine this info after releasing the LockMgrLock,
 * we can't just store LOCK and PGPROC pointers; we must extract out all the
 * info we want to be able to print.
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


/*
 * InitDeadLockChecking -- initialize deadlock checker during backend startup
 *
 * This does per-backend initialization of the deadlock checker; primarily,
 * allocation of working memory for DeadLockCheck.	We do this per-backend
 * since there's no percentage in making the kernel do copy-on-write
 * inheritance of workspace from the postmaster.  We want to allocate the
 * space at startup because the deadlock checker might be invoked when there's
 * no free memory left.
 */
void
InitDeadLockChecking(void)
{
	MemoryContext oldcxt;

	/* Make sure allocations are permanent */
	oldcxt = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * FindLockCycle needs at most MaxBackends entries in visitedProcs[]
	 * and deadlockDetails[].
	 */
	visitedProcs = (PGPROC **) palloc(MaxBackends * sizeof(PGPROC *));
	deadlockDetails = (DEADLOCK_INFO *) palloc(MaxBackends * sizeof(DEADLOCK_INFO));

	/*
	 * TopoSort needs to consider at most MaxBackends wait-queue entries,
	 * and it needn't run concurrently with FindLockCycle.
	 */
	topoProcs = visitedProcs;	/* re-use this space */
	beforeConstraints = (int *) palloc(MaxBackends * sizeof(int));
	afterConstraints = (int *) palloc(MaxBackends * sizeof(int));

	/*
	 * We need to consider rearranging at most MaxBackends/2 wait queues
	 * (since it takes at least two waiters in a queue to create a soft
	 * edge), and the expanded form of the wait queues can't involve more
	 * than MaxBackends total waiters.	(But avoid palloc(0) if
	 * MaxBackends = 1.)
	 */
	waitOrders = (WAIT_ORDER *)
		palloc(((MaxBackends + 1) / 2) * sizeof(WAIT_ORDER));
	waitOrderProcs = (PGPROC **) palloc(MaxBackends * sizeof(PGPROC *));

	/*
	 * Allow at most MaxBackends distinct constraints in a configuration.
	 * (Is this enough?  In practice it seems it should be, but I don't
	 * quite see how to prove it.  If we run out, we might fail to find a
	 * workable wait queue rearrangement even though one exists.)  NOTE
	 * that this number limits the maximum recursion depth of
	 * DeadLockCheckRecurse. Making it really big might potentially allow
	 * a stack-overflow problem.
	 */
	maxCurConstraints = MaxBackends;
	curConstraints = (EDGE *) palloc(maxCurConstraints * sizeof(EDGE));

	/*
	 * Allow up to 3*MaxBackends constraints to be saved without having to
	 * re-run TestConfiguration.  (This is probably more than enough, but
	 * we can survive if we run low on space by doing excess runs of
	 * TestConfiguration to re-compute constraint lists each time needed.)
	 * The last MaxBackends entries in possibleConstraints[] are reserved
	 * as output workspace for FindLockCycle.
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
 * deadlock.  If resolution is impossible, return TRUE --- the caller
 * is then expected to abort the given proc's transaction.
 *
 * We can't block on user locks, so no sense testing for deadlock
 * because there is no blocking, and no timer for the block.  So,
 * only look at regular locks.
 *
 * We must have already locked the master lock before being called.
 * NOTE: although the lockmethod structure appears to allow each lock
 * table to have a different masterLock, all locks that can block had
 * better use the same LWLock, else this code will not be adequately
 * interlocked!
 *
 * On failure, deadlock details are recorded in deadlockDetails[] for
 * subsequent printing by DeadLockReport().  That activity is separate
 * because we don't want to do it while holding the master lock.
 */
bool
DeadLockCheck(PGPROC *proc)
{
	int			i,
				j;

	/* Initialize to "no constraints" */
	nCurConstraints = 0;
	nPossibleConstraints = 0;
	nWaitOrders = 0;

	/* Search for deadlocks and possible fixes */
	if (DeadLockCheckRecurse(proc))
	{
		/*
		 * Call FindLockCycle one more time, to record the correct
		 * deadlockDetails[] for the basic state with no rearrangements.
		 */
		int			nSoftEdges;

		nWaitOrders = 0;
		if (!FindLockCycle(proc, possibleConstraints, &nSoftEdges))
			elog(FATAL, "deadlock seems to have disappeared");

		return true;			/* cannot find a non-deadlocked state */
	}

	/* Apply any needed rearrangements of wait queues */
	for (i = 0; i < nWaitOrders; i++)
	{
		LOCK	   *lock = waitOrders[i].lock;
		PGPROC	  **procs = waitOrders[i].procs;
		int			nProcs = waitOrders[i].nProcs;
		PROC_QUEUE *waitQueue = &(lock->waitProcs);

		Assert(nProcs == waitQueue->size);

#ifdef DEBUG_DEADLOCK
		PrintLockQueue(lock, "DeadLockCheck:");
#endif

		/* Reset the queue and re-add procs in the desired order */
		ProcQueueInit(waitQueue);
		for (j = 0; j < nProcs; j++)
		{
			SHMQueueInsertBefore(&(waitQueue->links), &(procs[j]->links));
			waitQueue->size++;
		}

#ifdef DEBUG_DEADLOCK
		PrintLockQueue(lock, "rearranged to:");
#endif

		/* See if any waiters for the lock can be woken up now */
		ProcLockWakeup(GetLocksMethodTable(lock), lock);
	}
	return false;
}

/*
 * DeadLockCheckRecurse -- recursively search for valid orderings
 *
 * curConstraints[] holds the current set of constraints being considered
 * by an outer level of recursion.	Add to this each possible solution
 * constraint for any cycle detected at this level.
 *
 * Returns TRUE if no solution exists.	Returns FALSE if a deadlock-free
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
	 * Check for cycles involving startProc or any of the procs mentioned
	 * in constraints.	We check startProc last because if it has a soft
	 * cycle still to be dealt with, we want to deal with that first.
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
 * waits-for graph that includes this proc.  Return TRUE if a cycle
 * is found, else FALSE.  If a cycle is found, we return a list of
 * the "soft edges", if any, included in the cycle.  These edges could
 * potentially be eliminated by rearranging wait queues.  We also fill
 * deadlockDetails[] with information about the detected cycle; this info
 * is not used by the deadlock algorithm itself, only to print a useful
 * message after failing.
 *
 * Since we need to be able to check hypothetical configurations that would
 * exist after wait queue rearrangement, the routine pays attention to the
 * table of hypothetical queue orders in waitOrders[].	These orders will
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
	PGPROC	   *proc;
	LOCK	   *lock;
	PROCLOCK   *proclock;
	SHM_QUEUE  *lockHolders;
	LOCKMETHODTABLE *lockMethodTable;
	PROC_QUEUE *waitQueue;
	int			queue_size;
	int			conflictMask;
	int			i;
	int			numLockModes,
				lm;

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
				 * record total length of cycle --- outer levels will now
				 * fill deadlockDetails[]
				 */
				Assert(depth <= MaxBackends);
				nDeadlockDetails = depth;

				return true;
			}

			/*
			 * Otherwise, we have a cycle but it does not include the
			 * start point, so say "no deadlock".
			 */
			return false;
		}
	}
	/* Mark proc as seen */
	Assert(nVisitedProcs < MaxBackends);
	visitedProcs[nVisitedProcs++] = checkProc;

	/*
	 * If the proc is not waiting, we have no outgoing waits-for edges.
	 */
	if (checkProc->links.next == INVALID_OFFSET)
		return false;
	lock = checkProc->waitLock;
	if (lock == NULL)
		return false;
	lockMethodTable = GetLocksMethodTable(lock);
	numLockModes = lockMethodTable->numLockModes;
	conflictMask = lockMethodTable->conflictTab[checkProc->waitLockMode];

	/*
	 * Scan for procs that already hold conflicting locks.	These are
	 * "hard" edges in the waits-for graph.
	 */
	lockHolders = &(lock->lockHolders);

	proclock = (PROCLOCK *) SHMQueueNext(lockHolders, lockHolders,
										 offsetof(PROCLOCK, lockLink));

	while (proclock)
	{
		proc = (PGPROC *) MAKE_PTR(proclock->tag.proc);

		/* A proc never blocks itself */
		if (proc != checkProc)
		{
			for (lm = 1; lm <= numLockModes; lm++)
			{
				if (proclock->holding[lm] > 0 &&
					((1 << lm) & conflictMask) != 0)
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
					/* If no deadlock, we're done looking at this proclock */
					break;
				}
			}
		}

		proclock = (PROCLOCK *) SHMQueueNext(lockHolders, &proclock->lockLink,
										   offsetof(PROCLOCK, lockLink));
	}

	/*
	 * Scan for procs that are ahead of this one in the lock's wait queue.
	 * Those that have conflicting requests soft-block this one.  This
	 * must be done after the hard-block search, since if another proc
	 * both hard- and soft-blocks this one, we want to call it a hard
	 * edge.
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

		queue_size = waitOrders[i].nProcs;

		for (i = 0; i < queue_size; i++)
		{
			proc = procs[i];

			/* Done when we reach the target proc */
			if (proc == checkProc)
				break;

			/* Is there a conflict with this guy's request? */
			if (((1 << proc->waitLockMode) & conflictMask) != 0)
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
					 * Add this edge to the list of soft edges in the
					 * cycle
					 */
					Assert(*nSoftEdges < MaxBackends);
					softEdges[*nSoftEdges].waiter = checkProc;
					softEdges[*nSoftEdges].blocker = proc;
					(*nSoftEdges)++;
					return true;
				}
			}
		}
	}
	else
	{
		/* Use the true lock wait queue order */
		waitQueue = &(lock->waitProcs);
		queue_size = waitQueue->size;

		proc = (PGPROC *) MAKE_PTR(waitQueue->links.next);

		while (queue_size-- > 0)
		{
			/* Done when we reach the target proc */
			if (proc == checkProc)
				break;

			/* Is there a conflict with this guy's request? */
			if (((1 << proc->waitLockMode) & conflictMask) != 0)
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
					 * Add this edge to the list of soft edges in the
					 * cycle
					 */
					Assert(*nSoftEdges < MaxBackends);
					softEdges[*nSoftEdges].waiter = checkProc;
					softEdges[*nSoftEdges].blocker = proc;
					(*nSoftEdges)++;
					return true;
				}
			}

			proc = (PGPROC *) MAKE_PTR(proc->links.next);
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
 * Returns TRUE if able to build an ordering that satisfies all the
 * constraints, FALSE if not (there are contradictory constraints).
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
	 * Scan constraint list backwards.	This is because the last-added
	 * constraint is the only one that could fail, and so we want to test
	 * it for inconsistency first.
	 */
	for (i = nConstraints; --i >= 0;)
	{
		PGPROC	   *proc = constraints[i].waiter;
		LOCK	   *lock = proc->waitLock;

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
		waitOrders[nWaitOrders].nProcs = lock->waitProcs.size;
		nWaitOrderProcs += lock->waitProcs.size;
		Assert(nWaitOrderProcs <= MaxBackends);

		/*
		 * Do the topo sort.  TopoSort need not examine constraints after
		 * this one, since they must be for different locks.
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
 * The partial order is specified by an array of EDGE structs.	Each EDGE
 * is one that we need to reverse, therefore the "waiter" must appear before
 * the "blocker" in the output array.  The EDGE array may well contain
 * edges associated with other locks; these should be ignored.
 *
 * Returns TRUE if able to build an ordering that satisfies all the
 * constraints, FALSE if not (there are contradictory constraints).
 */
static bool
TopoSort(LOCK *lock,
		 EDGE *constraints,
		 int nConstraints,
		 PGPROC **ordering)		/* output argument */
{
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	int			queue_size = waitQueue->size;
	PGPROC	   *proc;
	int			i,
				j,
				k,
				last;

	/* First, fill topoProcs[] array with the procs in their current order */
	proc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
	for (i = 0; i < queue_size; i++)
	{
		topoProcs[i] = proc;
		proc = (PGPROC *) MAKE_PTR(proc->links.next);
	}

	/*
	 * Scan the constraints, and for each proc in the array, generate a
	 * count of the number of constraints that say it must be before
	 * something else, plus a list of the constraints that say it must be
	 * after something else. The count for the j'th proc is stored in
	 * beforeConstraints[j], and the head of its list in
	 * afterConstraints[j].  Each constraint stores its list link in
	 * constraints[i].link (note any constraint will be in just one list).
	 * The array index for the before-proc of the i'th constraint is
	 * remembered in constraints[i].pred.
	 */
	MemSet(beforeConstraints, 0, queue_size * sizeof(int));
	MemSet(afterConstraints, 0, queue_size * sizeof(int));
	for (i = 0; i < nConstraints; i++)
	{
		proc = constraints[i].waiter;
		/* Ignore constraint if not for this lock */
		if (proc->waitLock != lock)
			continue;
		/* Find the waiter proc in the array */
		for (j = queue_size; --j >= 0;)
		{
			if (topoProcs[j] == proc)
				break;
		}
		Assert(j >= 0);			/* should have found a match */
		/* Find the blocker proc in the array */
		proc = constraints[i].blocker;
		for (k = queue_size; --k >= 0;)
		{
			if (topoProcs[k] == proc)
				break;
		}
		Assert(k >= 0);			/* should have found a match */
		beforeConstraints[j]++; /* waiter must come before */
		/* add this constraint to list of after-constraints for blocker */
		constraints[i].pred = j;
		constraints[i].link = afterConstraints[k];
		afterConstraints[k] = i + 1;
	}
	/*--------------------
	 * Now scan the topoProcs array backwards.	At each step, output the
	 * last proc that has no remaining before-constraints, and decrease
	 * the beforeConstraints count of each of the procs it was constrained
	 * against.
	 * i = index of ordering[] entry we want to output this time
	 * j = search index for topoProcs[]
	 * k = temp for scanning constraint list for proc j
	 * last = last non-null index in topoProcs (avoid redundant searches)
	 *--------------------
	 */
	last = queue_size - 1;
	for (i = queue_size; --i >= 0;)
	{
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
		/* Output candidate, and mark it done by zeroing topoProcs[] entry */
		ordering[i] = topoProcs[j];
		topoProcs[j] = NULL;
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
	PROC_QUEUE *waitQueue = &(lock->waitProcs);
	int			queue_size = waitQueue->size;
	PGPROC	   *proc;
	int			i;

	printf("%s lock %lx queue ", info, MAKE_OFFSET(lock));
	proc = (PGPROC *) MAKE_PTR(waitQueue->links.next);
	for (i = 0; i < queue_size; i++)
	{
		printf(" %d", proc->pid);
		proc = (PGPROC *) MAKE_PTR(proc->links.next);
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
	StringInfoData buf;
	int			i;

	initStringInfo(&buf);
	for (i = 0; i < nDeadlockDetails; i++)
	{
		DEADLOCK_INFO *info = &deadlockDetails[i];
		int			nextpid;

		/* The last proc waits for the first one... */
		if (i < nDeadlockDetails - 1)
			nextpid = info[1].pid;
		else
			nextpid = deadlockDetails[0].pid;

		if (i > 0)
			appendStringInfoChar(&buf, '\n');

		if (info->locktag.relId == XactLockTableId && info->locktag.dbId == 0)
		{
			/* Lock is for transaction ID */
			appendStringInfo(&buf,
							 gettext("Process %d waits for %s on transaction %u; blocked by process %d."),
							 info->pid,
							 GetLockmodeName(info->lockmode),
							 info->locktag.objId.xid,
							 nextpid);
		}
		else
		{
			/* Lock is for a relation */
			appendStringInfo(&buf,
							 gettext("Process %d waits for %s on relation %u of database %u; blocked by process %d."),
							 info->pid,
							 GetLockmodeName(info->lockmode),
							 info->locktag.relId,
							 info->locktag.dbId,
							 nextpid);
		}
	}
	ereport(ERROR,
			(errcode(ERRCODE_T_R_DEADLOCK_DETECTED),
			 errmsg("deadlock detected"),
			 errdetail("%s", buf.data)));
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
