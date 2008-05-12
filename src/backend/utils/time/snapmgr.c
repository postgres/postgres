/*-------------------------------------------------------------------------
 * snapmgr.c
 *		PostgreSQL snapshot manager
 *
 * We keep track of snapshots in two ways: the "registered snapshots" list,
 * and the "active snapshot" stack.  All snapshots in any of them is supposed
 * to be in persistent memory.  When a snapshot is no longer in any of these
 * lists (tracked by separate refcounts of each snapshot), its memory can be
 * freed.
 *
 * These arrangements let us reset MyProc->xmin when there are no snapshots
 * referenced by this transaction.  (One possible improvement would be to be
 * able to advance Xmin when the snapshot with the earliest Xmin is no longer
 * referenced.  That's a bit harder though, it requires more locking, and
 * anyway it should be rather uncommon to keep snapshots referenced for too
 * long.)
 *
 * Note: parts of this code could probably be replaced by appropriate use
 * of resowner.c.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/time/snapmgr.c,v 1.2 2008/05/12 20:02:02 alvherre Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "access/transam.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"
#include "utils/memutils.h"


/*
 * CurrentSnapshot points to the only snapshot taken in a serializable
 * transaction, and to the latest one taken in a read-committed transaction.
 * SecondarySnapshot is a snapshot that's always up-to-date as of the current
 * instant, even on a serializable transaction.  It should only be used for
 * special-purpose code (say, RI checking.)
 *
 * These SnapshotData structs are static to simplify memory allocation
 * (see the hack in GetSnapshotData to avoid repeated malloc/free).
 */
static SnapshotData CurrentSnapshotData = {HeapTupleSatisfiesMVCC};
static SnapshotData SecondarySnapshotData = {HeapTupleSatisfiesMVCC};

/* Pointers to valid snapshots */
static Snapshot	CurrentSnapshot = NULL;
static Snapshot	SecondarySnapshot = NULL;

/*
 * These are updated by GetSnapshotData.  We initialize them this way
 * for the convenience of TransactionIdIsInProgress: even in bootstrap
 * mode, we don't want it to say that BootstrapTransactionId is in progress.
 */
TransactionId TransactionXmin = FirstNormalTransactionId;
TransactionId RecentXmin = FirstNormalTransactionId;
TransactionId RecentGlobalXmin = FirstNormalTransactionId;

/*
 * Elements of the list of registered snapshots.
 *
 * Note that we keep refcounts both here and in SnapshotData.  This is because
 * the same snapshot may be registered more than once in a subtransaction, and
 * if a subxact aborts we want to be able to substract the correct amount of
 * counts from SnapshotData.  (Another approach would be keeping one
 * RegdSnapshotElt each time a snapshot is registered, but that seems
 * unnecessary wastage.)
 *
 * NB: the code assumes that elements in this list are in non-increasing
 * order of s_level; also, the list must be NULL-terminated.
 */
typedef struct RegdSnapshotElt
{
	Snapshot	s_snap;
	uint32		s_count;
	int			s_level;
	struct RegdSnapshotElt	*s_next;
} RegdSnapshotElt;

/*
 * Elements of the active snapshot stack.
 *
 * It's not necessary to keep a refcount like we do for the registered list;
 * each element here accounts for exactly one active_count on SnapshotData.
 * We cannot condense them like we do for RegdSnapshotElt because it would mess
 * up the order of entries in the stack.
 *
 * NB: the code assumes that elements in this list are in non-increasing
 * order of as_level; also, the list must be NULL-terminated.
 */
typedef struct ActiveSnapshotElt
{
	Snapshot	as_snap;
	int			as_level;
	struct ActiveSnapshotElt *as_next;
} ActiveSnapshotElt;

/* Head of the list of registered snapshots */
static RegdSnapshotElt	   *RegisteredSnapshotList = NULL;

/* Top of the stack of active snapshots */
static ActiveSnapshotElt	*ActiveSnapshot = NULL;

/* first GetTransactionSnapshot call in a transaction? */
bool					FirstSnapshotSet = false;

/*
 * Remembers whether this transaction registered a serializable snapshot at
 * start.  We cannot trust FirstSnapshotSet in combination with
 * IsXactIsoLevelSerializable, because GUC may be reset before us.
 */
static bool				registered_serializable = false;


static Snapshot CopySnapshot(Snapshot snapshot);
static void FreeSnapshot(Snapshot snapshot);
static void	SnapshotResetXmin(void);


/*
 * GetTransactionSnapshot
 *		Get the appropriate snapshot for a new query in a transaction.
 *
 *
 * Note that the return value may point at static storage that will be modified
 * by future calls and by CommandCounterIncrement().  Callers should call
 * RegisterSnapshot or PushActiveSnapshot on the returned snap if it is to be
 * used very long.
 */
Snapshot
GetTransactionSnapshot(void)
{
	/* First call in transaction? */
	if (!FirstSnapshotSet)
	{
		CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);
		FirstSnapshotSet = true;

		/*
		 * In serializable mode, the first snapshot must live until end of xact
		 * regardless of what the caller does with it, so we must register it
		 * internally here and unregister it at end of xact.
		 */
		if (IsXactIsoLevelSerializable)
		{
			CurrentSnapshot = RegisterSnapshot(CurrentSnapshot);
			registered_serializable = true;
		}

		return CurrentSnapshot;
	}

	if (IsXactIsoLevelSerializable)
		return CurrentSnapshot;

	CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

	return CurrentSnapshot;
}

/*
 * GetLatestSnapshot
 *		Get a snapshot that is up-to-date as of the current instant,
 *		even if we are executing in SERIALIZABLE mode.
 */
Snapshot
GetLatestSnapshot(void)
{
	/* Should not be first call in transaction */
	if (!FirstSnapshotSet)
		elog(ERROR, "no snapshot has been set");

	SecondarySnapshot = GetSnapshotData(&SecondarySnapshotData);

	return SecondarySnapshot;
}

/*
 * SnapshotSetCommandId
 * 		Propagate CommandCounterIncrement into the static snapshots, if set
 */
void
SnapshotSetCommandId(CommandId curcid)
{
	if (!FirstSnapshotSet)
		return;

	if (CurrentSnapshot)
		CurrentSnapshot->curcid = curcid;
	if (SecondarySnapshot)
		SecondarySnapshot->curcid = curcid;
}

/*
 * CopySnapshot
 *		Copy the given snapshot.
 *
 * The copy is palloc'd in TopTransactionContext and has initial refcounts set
 * to 0.  The returned snapshot has the copied flag set.
 */
static Snapshot
CopySnapshot(Snapshot snapshot)
{
	Snapshot	newsnap;
	Size		subxipoff;
	Size		size;

	Assert(snapshot != InvalidSnapshot);

	/* We allocate any XID arrays needed in the same palloc block. */
	size = subxipoff = sizeof(SnapshotData) +
		snapshot->xcnt * sizeof(TransactionId);
	if (snapshot->subxcnt > 0)
		size += snapshot->subxcnt * sizeof(TransactionId);

	newsnap = (Snapshot) MemoryContextAlloc(TopTransactionContext, size);
	memcpy(newsnap, snapshot, sizeof(SnapshotData));

	newsnap->regd_count = 0;
	newsnap->active_count = 0;
	newsnap->copied = true;

	/* setup XID array */
	if (snapshot->xcnt > 0)
	{
		newsnap->xip = (TransactionId *) (newsnap + 1);
		memcpy(newsnap->xip, snapshot->xip,
			   snapshot->xcnt * sizeof(TransactionId));
	}
	else
		newsnap->xip = NULL;

	/* setup subXID array */
	if (snapshot->subxcnt > 0)
	{
		newsnap->subxip = (TransactionId *) ((char *) newsnap + subxipoff);
		memcpy(newsnap->subxip, snapshot->subxip,
			   snapshot->subxcnt * sizeof(TransactionId));
	}
	else
		newsnap->subxip = NULL;

	return newsnap;
}

/*
 * FreeSnapshot
 *		Free the memory associated with a snapshot.
 */
static void
FreeSnapshot(Snapshot snapshot)
{
	Assert(snapshot->regd_count == 0);
	Assert(snapshot->active_count == 0);

	pfree(snapshot);
}

/*
 * PushActiveSnapshot
 * 		Set the given snapshot as the current active snapshot
 *
 * If this is the first use of this snapshot, create a new long-lived copy with
 * active refcount=1.  Otherwise, only increment the refcount.
 */
void
PushActiveSnapshot(Snapshot snap)
{
	ActiveSnapshotElt	*newactive;

	Assert(snap != InvalidSnapshot);

	newactive = MemoryContextAlloc(TopTransactionContext, sizeof(ActiveSnapshotElt));
	/* Static snapshot?  Create a persistent copy */
	newactive->as_snap = snap->copied ? snap : CopySnapshot(snap);
	newactive->as_next = ActiveSnapshot;
	newactive->as_level = GetCurrentTransactionNestLevel();

	newactive->as_snap->active_count++;

	ActiveSnapshot = newactive;
}

/*
 * PushUpdatedSnapshot
 * 		As above, except we set the snapshot's CID to the current CID.
 */
void
PushUpdatedSnapshot(Snapshot snapshot)
{
	Snapshot	newsnap;

	/*
	 * We cannot risk modifying a snapshot that's possibly already used
	 * elsewhere, so make a new copy to scribble on.
	 */
	newsnap = CopySnapshot(snapshot);
	newsnap->curcid = GetCurrentCommandId(false);

	PushActiveSnapshot(newsnap);
}

/*
 * PopActiveSnapshot
 *
 * Remove the topmost snapshot from the active snapshot stack, decrementing the
 * reference count, and free it if this was the last reference.
 */
void
PopActiveSnapshot(void)
{
	ActiveSnapshotElt	*newstack;

	newstack = ActiveSnapshot->as_next;

	Assert(ActiveSnapshot->as_snap->active_count > 0);

	ActiveSnapshot->as_snap->active_count--;

	if (ActiveSnapshot->as_snap->active_count == 0 &&
		ActiveSnapshot->as_snap->regd_count == 0)
		FreeSnapshot(ActiveSnapshot->as_snap);

	pfree(ActiveSnapshot);
	ActiveSnapshot = newstack;

	SnapshotResetXmin();
}

/*
 * GetActiveSnapshot
 * 		Return the topmost snapshot in the Active stack.
 */
Snapshot
GetActiveSnapshot(void)
{
	Assert(ActiveSnapshot != NULL);

	return ActiveSnapshot->as_snap;
}

/*
 * ActiveSnapshotSet
 * 		Return whether there is at least one snapsho in the Active stack
 */
bool
ActiveSnapshotSet(void)
{
	return ActiveSnapshot != NULL;
}

/*
 * RegisterSnapshot
 * 		Register a snapshot as being in use
 *
 * If InvalidSnapshot is passed, it is not registered.
 */
Snapshot
RegisterSnapshot(Snapshot snapshot)
{
	RegdSnapshotElt	*elt;
	RegdSnapshotElt	*newhead;
	int		level;

	if (snapshot == InvalidSnapshot)
		return InvalidSnapshot;

	level = GetCurrentTransactionNestLevel();

	/*
	 * If there's already an item in the list for the same snapshot and the
	 * same subxact nest level, increment its refcounts.  Otherwise create a
	 * new one.
	 */
	for (elt = RegisteredSnapshotList; elt != NULL; elt = elt->s_next)
	{
		if (elt->s_level < level)
			break;

		if (elt->s_snap == snapshot && elt->s_level == level)
		{
			elt->s_snap->regd_count++;
			elt->s_count++;

			return elt->s_snap;
		}
	}

	/*
	 * Create the new list element.  If it's not been copied into persistent
	 * memory already, we must do so; otherwise we can just increment the
	 * reference count.
	 */
	newhead = MemoryContextAlloc(TopTransactionContext, sizeof(RegdSnapshotElt));
	newhead->s_next = RegisteredSnapshotList;
	/* Static snapshot?  Create a persistent copy */
	newhead->s_snap = snapshot->copied ? snapshot : CopySnapshot(snapshot);
	newhead->s_level = level;
	newhead->s_count = 1;

	newhead->s_snap->regd_count++;

	RegisteredSnapshotList = newhead;

	return RegisteredSnapshotList->s_snap;
}

/*
 * UnregisterSnapshot
 * 		Signals that a snapshot is no longer necessary
 *
 * If both reference counts fall to zero, the snapshot memory is released.
 * If only the registered list refcount falls to zero, just the list element is
 * freed.
 */
void
UnregisterSnapshot(Snapshot snapshot)
{
	RegdSnapshotElt	*prev = NULL;
	RegdSnapshotElt	*elt;
	bool		found = false;

	if (snapshot == InvalidSnapshot)
		return;

	for (elt = RegisteredSnapshotList; elt != NULL; elt = elt->s_next)
	{
		if (elt->s_snap == snapshot)
		{
			Assert(elt->s_snap->regd_count > 0);
			Assert(elt->s_count > 0);

			elt->s_snap->regd_count--;
			elt->s_count--;
			found = true;

			if (elt->s_count == 0)
			{
				/* delink it from the registered snapshot list */
				if (prev)
					prev->s_next = elt->s_next;
				else
					RegisteredSnapshotList = elt->s_next;

				/* free the snapshot itself if it's no longer relevant */
				if (elt->s_snap->regd_count == 0 && elt->s_snap->active_count == 0)
					FreeSnapshot(elt->s_snap);

				/* and free the list element */
				pfree(elt);
			}

			break;
		}

		prev = elt;
	}

	if (!found)
		elog(WARNING, "unregistering failed for snapshot %p", snapshot);

	SnapshotResetXmin();
}

/*
 * SnapshotResetXmin
 *
 * If there are no more snapshots, we can reset our PGPROC->xmin to InvalidXid.
 * Note we can do this without locking because we assume that storing an Xid
 * is atomic.
 */
static void
SnapshotResetXmin(void)
{
	if (RegisteredSnapshotList == NULL && ActiveSnapshot == NULL)
		MyProc->xmin = InvalidTransactionId;
}

/*
 * AtSubCommit_Snapshot
 */
void
AtSubCommit_Snapshot(int level)
{
	ActiveSnapshotElt	*active;
	RegdSnapshotElt	*regd;

	/*
	 * Relabel the active snapshots set in this subtransaction as though they
	 * are owned by the parent subxact.
	 */
	for (active = ActiveSnapshot; active != NULL; active = active->as_next)
	{
		if (active->as_level < level)
			break;
		active->as_level = level - 1;
	}

	/*
	 * Reassign all registered snapshots to the parent subxact.
	 *
	 * Note: this code is somewhat bogus in that we could end up with multiple
	 * entries for the same snapshot and the same subxact level (my parent's
	 * level).  Cleaning that up is more trouble than it's currently worth,
	 * however.
	 */
	for (regd = RegisteredSnapshotList; regd != NULL; regd = regd->s_next)
	{
		if (regd->s_level == level)
			regd->s_level--;
	}
}

/*
 * AtSubAbort_Snapshot
 * 		Clean up snapshots after a subtransaction abort
 */
void
AtSubAbort_Snapshot(int level)
{
	RegdSnapshotElt	*prev;
	RegdSnapshotElt	*regd;

	/* Forget the active snapshots set by this subtransaction */
	while (ActiveSnapshot && ActiveSnapshot->as_level >= level)
	{
		ActiveSnapshotElt	*next;

		next = ActiveSnapshot->as_next;

		/*
		 * Decrement the snapshot's active count.  If it's still registered or
		 * marked as active by an outer subtransaction, we can't free it yet.
		 */
		Assert(ActiveSnapshot->as_snap->active_count >= 1);
		ActiveSnapshot->as_snap->active_count -= 1;

		if (ActiveSnapshot->as_snap->active_count == 0 &&
			ActiveSnapshot->as_snap->regd_count == 0)
			FreeSnapshot(ActiveSnapshot->as_snap);

		/* and free the stack element */
		pfree(ActiveSnapshot);

		ActiveSnapshot = next;
	}

	/* Unregister all snapshots registered during this subtransaction */
	prev = NULL;
	for (regd = RegisteredSnapshotList; regd != NULL; )
	{
		if (regd->s_level >= level)
		{
			RegdSnapshotElt	*tofree;

			if (prev)
				prev->s_next = regd->s_next;
			else
				RegisteredSnapshotList = regd->s_next;

			tofree = regd;
			regd = regd->s_next;

			tofree->s_snap->regd_count -= tofree->s_count;

			/* free the snapshot if possible */
			if (tofree->s_snap->regd_count == 0 &&
				tofree->s_snap->active_count == 0)
				FreeSnapshot(tofree->s_snap);

			/* and free the list element */
			pfree(tofree);
		}
		else
		{
			prev = regd;
			regd = regd->s_next;
		}
	}

	SnapshotResetXmin();
}

/*
 * AtEOXact_Snapshot
 * 		Snapshot manager's cleanup function for end of transaction
 */
void
AtEOXact_Snapshot(bool isCommit)
{
	/* On commit, complain about leftover snapshots */
	if (isCommit)
	{
		ActiveSnapshotElt	*active;
		RegdSnapshotElt	*regd;

		/*
		 * On a serializable snapshot we must first unregister our private
		 * refcount to the serializable snapshot.
		 */
		if (registered_serializable)
			UnregisterSnapshot(CurrentSnapshot);

		/* complain about unpopped active snapshots */
		for (active = ActiveSnapshot; active != NULL; active = active->as_next)
		{
			ereport(WARNING,
					(errmsg("snapshot %p still active", active)));
		}

		/* complain about any unregistered snapshot */
		for (regd = RegisteredSnapshotList; regd != NULL; regd = regd->s_next)
		{
			ereport(WARNING,
					(errmsg("snapshot %p not destroyed at commit (%d regd refs, %d active refs)",
							regd->s_snap, regd->s_snap->regd_count,
							regd->s_snap->active_count)));
		}
	}

	/*
	 * And reset our state.  We don't need to free the memory explicitely --
	 * it'll go away with TopTransactionContext.
	 */
	ActiveSnapshot = NULL;
	RegisteredSnapshotList = NULL;

	CurrentSnapshot = NULL;
	SecondarySnapshot = NULL;

	FirstSnapshotSet = false;
	registered_serializable = false;
}
