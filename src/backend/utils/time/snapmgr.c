/*-------------------------------------------------------------------------
 * snapmgr.c
 *		PostgreSQL snapshot manager
 *
 * We keep track of snapshots in two ways: those "registered" by resowner.c,
 * and the "active snapshot" stack.  All snapshots in either of them live in
 * persistent memory.  When a snapshot is no longer in any of these lists
 * (tracked by separate refcounts on each snapshot), its memory can be freed.
 *
 * The FirstXactSnapshot, if any, is treated a bit specially: we increment its
 * regd_count and count it in RegisteredSnapshots, but this reference is not
 * tracked by a resource owner. We used to use the TopTransactionResourceOwner
 * to track this snapshot reference, but that introduces logical circularity
 * and thus makes it impossible to clean up in a sane fashion.  It's better to
 * handle this reference as an internally-tracked registration, so that this
 * module is entirely lower-level than ResourceOwners.
 *
 * Likewise, any snapshots that have been exported by pg_export_snapshot
 * have regd_count = 1 and are counted in RegisteredSnapshots, but are not
 * tracked by any resource owner.
 *
 * The same is true for historic snapshots used during logical decoding,
 * their lifetime is managed separately (as they life longer as one xact.c
 * transaction).
 *
 * These arrangements let us reset MyPgXact->xmin when there are no snapshots
 * referenced by this transaction.  (One possible improvement would be to be
 * able to advance Xmin when the snapshot with the earliest Xmin is no longer
 * referenced.  That's a bit harder though, it requires more locking, and
 * anyway it should be rather uncommon to keep temporary snapshots referenced
 * for too long.)
 *
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/time/snapmgr.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/transam.h"
#include "access/xact.h"
#include "lib/pairingheap.h"
#include "miscadmin.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/sinval.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resowner_private.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/*
 * CurrentSnapshot points to the only snapshot taken in transaction-snapshot
 * mode, and to the latest one taken in a read-committed transaction.
 * SecondarySnapshot is a snapshot that's always up-to-date as of the current
 * instant, even in transaction-snapshot mode.  It should only be used for
 * special-purpose code (say, RI checking.)  CatalogSnapshot points to an
 * MVCC snapshot intended to be used for catalog scans; we must refresh it
 * whenever a system catalog change occurs.
 *
 * These SnapshotData structs are static to simplify memory allocation
 * (see the hack in GetSnapshotData to avoid repeated malloc/free).
 */
static SnapshotData CurrentSnapshotData = {HeapTupleSatisfiesMVCC};
static SnapshotData SecondarySnapshotData = {HeapTupleSatisfiesMVCC};
SnapshotData CatalogSnapshotData = {HeapTupleSatisfiesMVCC};

/* Pointers to valid snapshots */
static Snapshot CurrentSnapshot = NULL;
static Snapshot SecondarySnapshot = NULL;
static Snapshot CatalogSnapshot = NULL;
static Snapshot HistoricSnapshot = NULL;

/*
 * Staleness detection for CatalogSnapshot.
 */
static bool CatalogSnapshotStale = true;

/*
 * These are updated by GetSnapshotData.  We initialize them this way
 * for the convenience of TransactionIdIsInProgress: even in bootstrap
 * mode, we don't want it to say that BootstrapTransactionId is in progress.
 *
 * RecentGlobalXmin and RecentGlobalDataXmin are initialized to
 * InvalidTransactionId, to ensure that no one tries to use a stale
 * value. Readers should ensure that it has been set to something else
 * before using it.
 */
TransactionId TransactionXmin = FirstNormalTransactionId;
TransactionId RecentXmin = FirstNormalTransactionId;
TransactionId RecentGlobalXmin = InvalidTransactionId;
TransactionId RecentGlobalDataXmin = InvalidTransactionId;

/* (table, ctid) => (cmin, cmax) mapping during timetravel */
static HTAB *tuplecid_data = NULL;

/*
 * Elements of the active snapshot stack.
 *
 * Each element here accounts for exactly one active_count on SnapshotData.
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

/* Top of the stack of active snapshots */
static ActiveSnapshotElt *ActiveSnapshot = NULL;

/*
 * Currently registered Snapshots.  Ordered in a heap by xmin, so that we can
 * quickly find the one with lowest xmin, to advance our MyPgXat->xmin.
 * resowner.c also tracks these.
 */
static int xmin_cmp(const pairingheap_node *a, const pairingheap_node *b,
		 void *arg);

static pairingheap RegisteredSnapshots = {&xmin_cmp, NULL, NULL};

/* first GetTransactionSnapshot call in a transaction? */
bool		FirstSnapshotSet = false;

/*
 * Remember the serializable transaction snapshot, if any.  We cannot trust
 * FirstSnapshotSet in combination with IsolationUsesXactSnapshot(), because
 * GUC may be reset before us, changing the value of IsolationUsesXactSnapshot.
 */
static Snapshot FirstXactSnapshot = NULL;

/* Define pathname of exported-snapshot files */
#define SNAPSHOT_EXPORT_DIR "pg_snapshots"
#define XactExportFilePath(path, xid, num, suffix) \
	snprintf(path, sizeof(path), SNAPSHOT_EXPORT_DIR "/%08X-%d%s", \
			 xid, num, suffix)

/* Current xact's exported snapshots (a list of Snapshot structs) */
static List *exportedSnapshots = NIL;

/* Prototypes for local functions */
static Snapshot CopySnapshot(Snapshot snapshot);
static void FreeSnapshot(Snapshot snapshot);
static void SnapshotResetXmin(void);

/*
 * Snapshot fields to be serialized.
 *
 * Only these fields need to be sent to the cooperating backend; the
 * remaining ones can (and must) set by the receiver upon restore.
 */
typedef struct SerializedSnapshotData
{
	TransactionId xmin;
	TransactionId xmax;
	uint32		xcnt;
	int32		subxcnt;
	bool		suboverflowed;
	bool		takenDuringRecovery;
	CommandId	curcid;
} SerializedSnapshotData;

/*
 * GetTransactionSnapshot
 *		Get the appropriate snapshot for a new query in a transaction.
 *
 * Note that the return value may point at static storage that will be modified
 * by future calls and by CommandCounterIncrement().  Callers should call
 * RegisterSnapshot or PushActiveSnapshot on the returned snap if it is to be
 * used very long.
 */
Snapshot
GetTransactionSnapshot(void)
{
	/*
	 * Return historic snapshot if doing logical decoding. We'll never need a
	 * non-historic transaction snapshot in this (sub-)transaction, so there's
	 * no need to be careful to set one up for later calls to
	 * GetTransactionSnapshot().
	 */
	if (HistoricSnapshotActive())
	{
		Assert(!FirstSnapshotSet);
		return HistoricSnapshot;
	}

	/* First call in transaction? */
	if (!FirstSnapshotSet)
	{
		Assert(pairingheap_is_empty(&RegisteredSnapshots));
		Assert(FirstXactSnapshot == NULL);

		if (IsInParallelMode())
			elog(ERROR,
				 "cannot take query snapshot during a parallel operation");

		/*
		 * In transaction-snapshot mode, the first snapshot must live until
		 * end of xact regardless of what the caller does with it, so we must
		 * make a copy of it rather than returning CurrentSnapshotData
		 * directly.  Furthermore, if we're running in serializable mode,
		 * predicate.c needs to wrap the snapshot fetch in its own processing.
		 */
		if (IsolationUsesXactSnapshot())
		{
			/* First, create the snapshot in CurrentSnapshotData */
			if (IsolationIsSerializable())
				CurrentSnapshot = GetSerializableTransactionSnapshot(&CurrentSnapshotData);
			else
				CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);
			/* Make a saved copy */
			CurrentSnapshot = CopySnapshot(CurrentSnapshot);
			FirstXactSnapshot = CurrentSnapshot;
			/* Mark it as "registered" in FirstXactSnapshot */
			FirstXactSnapshot->regd_count++;
			pairingheap_add(&RegisteredSnapshots, &FirstXactSnapshot->ph_node);
		}
		else
			CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

		/* Don't allow catalog snapshot to be older than xact snapshot. */
		CatalogSnapshotStale = true;

		FirstSnapshotSet = true;
		return CurrentSnapshot;
	}

	if (IsolationUsesXactSnapshot())
		return CurrentSnapshot;

	/* Don't allow catalog snapshot to be older than xact snapshot. */
	CatalogSnapshotStale = true;

	CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

	return CurrentSnapshot;
}

/*
 * GetLatestSnapshot
 *		Get a snapshot that is up-to-date as of the current instant,
 *		even if we are executing in transaction-snapshot mode.
 */
Snapshot
GetLatestSnapshot(void)
{
	/*
	 * We might be able to relax this, but nothing that could otherwise work
	 * needs it.
	 */
	if (IsInParallelMode())
		elog(ERROR,
			 "cannot update SecondarySnapshot during a parallel operation");

	/*
	 * So far there are no cases requiring support for GetLatestSnapshot()
	 * during logical decoding, but it wouldn't be hard to add if required.
	 */
	Assert(!HistoricSnapshotActive());

	/* If first call in transaction, go ahead and set the xact snapshot */
	if (!FirstSnapshotSet)
		return GetTransactionSnapshot();

	SecondarySnapshot = GetSnapshotData(&SecondarySnapshotData);

	return SecondarySnapshot;
}

/*
 * GetCatalogSnapshot
 *		Get a snapshot that is sufficiently up-to-date for scan of the
 *		system catalog with the specified OID.
 */
Snapshot
GetCatalogSnapshot(Oid relid)
{
	/*
	 * Return historic snapshot while we're doing logical decoding, so we can
	 * see the appropriate state of the catalog.
	 *
	 * This is the primary reason for needing to reset the system caches after
	 * finishing decoding.
	 */
	if (HistoricSnapshotActive())
		return HistoricSnapshot;

	return GetNonHistoricCatalogSnapshot(relid);
}

/*
 * GetNonHistoricCatalogSnapshot
 *		Get a snapshot that is sufficiently up-to-date for scan of the system
 *		catalog with the specified OID, even while historic snapshots are set
 *		up.
 */
Snapshot
GetNonHistoricCatalogSnapshot(Oid relid)
{
	/*
	 * If the caller is trying to scan a relation that has no syscache, no
	 * catcache invalidations will be sent when it is updated.  For a few key
	 * relations, snapshot invalidations are sent instead.  If we're trying to
	 * scan a relation for which neither catcache nor snapshot invalidations
	 * are sent, we must refresh the snapshot every time.
	 */
	if (!CatalogSnapshotStale && !RelationInvalidatesSnapshotsOnly(relid) &&
		!RelationHasSysCache(relid))
		CatalogSnapshotStale = true;

	if (CatalogSnapshotStale)
	{
		/* Get new snapshot. */
		CatalogSnapshot = GetSnapshotData(&CatalogSnapshotData);

		/*
		 * Mark new snapshost as valid.  We must do this last, in case an
		 * ERROR occurs inside GetSnapshotData().
		 */
		CatalogSnapshotStale = false;
	}

	return CatalogSnapshot;
}

/*
 * Mark the current catalog snapshot as invalid.  We could change this API
 * to allow the caller to provide more fine-grained invalidation details, so
 * that a change to relation A wouldn't prevent us from using our cached
 * snapshot to scan relation B, but so far there's no evidence that the CPU
 * cycles we spent tracking such fine details would be well-spent.
 */
void
InvalidateCatalogSnapshot()
{
	CatalogSnapshotStale = true;
}

/*
 * SnapshotSetCommandId
 *		Propagate CommandCounterIncrement into the static snapshots, if set
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
 * SetTransactionSnapshot
 *		Set the transaction's snapshot from an imported MVCC snapshot.
 *
 * Note that this is very closely tied to GetTransactionSnapshot --- it
 * must take care of all the same considerations as the first-snapshot case
 * in GetTransactionSnapshot.
 */
static void
SetTransactionSnapshot(Snapshot sourcesnap, TransactionId sourcexid,
					   PGPROC *sourceproc)
{
	/* Caller should have checked this already */
	Assert(!FirstSnapshotSet);

	Assert(pairingheap_is_empty(&RegisteredSnapshots));
	Assert(FirstXactSnapshot == NULL);
	Assert(!HistoricSnapshotActive());

	/*
	 * Even though we are not going to use the snapshot it computes, we must
	 * call GetSnapshotData, for two reasons: (1) to be sure that
	 * CurrentSnapshotData's XID arrays have been allocated, and (2) to update
	 * RecentXmin and RecentGlobalXmin.  (We could alternatively include those
	 * two variables in exported snapshot files, but it seems better to have
	 * snapshot importers compute reasonably up-to-date values for them.)
	 */
	CurrentSnapshot = GetSnapshotData(&CurrentSnapshotData);

	/*
	 * Now copy appropriate fields from the source snapshot.
	 */
	CurrentSnapshot->xmin = sourcesnap->xmin;
	CurrentSnapshot->xmax = sourcesnap->xmax;
	CurrentSnapshot->xcnt = sourcesnap->xcnt;
	Assert(sourcesnap->xcnt <= GetMaxSnapshotXidCount());
	memcpy(CurrentSnapshot->xip, sourcesnap->xip,
		   sourcesnap->xcnt * sizeof(TransactionId));
	CurrentSnapshot->subxcnt = sourcesnap->subxcnt;
	Assert(sourcesnap->subxcnt <= GetMaxSnapshotSubxidCount());
	memcpy(CurrentSnapshot->subxip, sourcesnap->subxip,
		   sourcesnap->subxcnt * sizeof(TransactionId));
	CurrentSnapshot->suboverflowed = sourcesnap->suboverflowed;
	CurrentSnapshot->takenDuringRecovery = sourcesnap->takenDuringRecovery;
	/* NB: curcid should NOT be copied, it's a local matter */

	/*
	 * Now we have to fix what GetSnapshotData did with MyPgXact->xmin and
	 * TransactionXmin.  There is a race condition: to make sure we are not
	 * causing the global xmin to go backwards, we have to test that the
	 * source transaction is still running, and that has to be done
	 * atomically. So let procarray.c do it.
	 *
	 * Note: in serializable mode, predicate.c will do this a second time. It
	 * doesn't seem worth contorting the logic here to avoid two calls,
	 * especially since it's not clear that predicate.c *must* do this.
	 */
	if (sourceproc != NULL)
	{
		if (!ProcArrayInstallRestoredXmin(CurrentSnapshot->xmin, sourceproc))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("could not import the requested snapshot"),
			   errdetail("The source transaction is not running anymore.")));
	}
	else if (!ProcArrayInstallImportedXmin(CurrentSnapshot->xmin, sourcexid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not import the requested snapshot"),
			   errdetail("The source transaction %u is not running anymore.",
						 sourcexid)));

	/*
	 * In transaction-snapshot mode, the first snapshot must live until end of
	 * xact, so we must make a copy of it.  Furthermore, if we're running in
	 * serializable mode, predicate.c needs to do its own processing.
	 */
	if (IsolationUsesXactSnapshot())
	{
		if (IsolationIsSerializable())
			SetSerializableTransactionSnapshot(CurrentSnapshot, sourcexid);
		/* Make a saved copy */
		CurrentSnapshot = CopySnapshot(CurrentSnapshot);
		FirstXactSnapshot = CurrentSnapshot;
		/* Mark it as "registered" in FirstXactSnapshot */
		FirstXactSnapshot->regd_count++;
		pairingheap_add(&RegisteredSnapshots, &FirstXactSnapshot->ph_node);
	}

	FirstSnapshotSet = true;
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

	/*
	 * Setup subXID array. Don't bother to copy it if it had overflowed,
	 * though, because it's not used anywhere in that case. Except if it's a
	 * snapshot taken during recovery; all the top-level XIDs are in subxip as
	 * well in that case, so we mustn't lose them.
	 */
	if (snapshot->subxcnt > 0 &&
		(!snapshot->suboverflowed || snapshot->takenDuringRecovery))
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
	Assert(snapshot->copied);

	pfree(snapshot);
}

/*
 * PushActiveSnapshot
 *		Set the given snapshot as the current active snapshot
 *
 * If the passed snapshot is a statically-allocated one, or it is possibly
 * subject to a future command counter update, create a new long-lived copy
 * with active refcount=1.  Otherwise, only increment the refcount.
 */
void
PushActiveSnapshot(Snapshot snap)
{
	ActiveSnapshotElt *newactive;

	Assert(snap != InvalidSnapshot);

	newactive = MemoryContextAlloc(TopTransactionContext, sizeof(ActiveSnapshotElt));

	/*
	 * Checking SecondarySnapshot is probably useless here, but it seems
	 * better to be sure.
	 */
	if (snap == CurrentSnapshot || snap == SecondarySnapshot || !snap->copied)
		newactive->as_snap = CopySnapshot(snap);
	else
		newactive->as_snap = snap;

	newactive->as_next = ActiveSnapshot;
	newactive->as_level = GetCurrentTransactionNestLevel();

	newactive->as_snap->active_count++;

	ActiveSnapshot = newactive;
}

/*
 * PushCopiedSnapshot
 *		As above, except forcibly copy the presented snapshot.
 *
 * This should be used when the ActiveSnapshot has to be modifiable, for
 * example if the caller intends to call UpdateActiveSnapshotCommandId.
 * The new snapshot will be released when popped from the stack.
 */
void
PushCopiedSnapshot(Snapshot snapshot)
{
	PushActiveSnapshot(CopySnapshot(snapshot));
}

/*
 * UpdateActiveSnapshotCommandId
 *
 * Update the current CID of the active snapshot.  This can only be applied
 * to a snapshot that is not referenced elsewhere.
 */
void
UpdateActiveSnapshotCommandId(void)
{
	CommandId	save_curcid,
				curcid;

	Assert(ActiveSnapshot != NULL);
	Assert(ActiveSnapshot->as_snap->active_count == 1);
	Assert(ActiveSnapshot->as_snap->regd_count == 0);

	/*
	 * Don't allow modification of the active snapshot during parallel
	 * operation.  We share the snapshot to worker backends at beginning of
	 * parallel operation, so any change to snapshot can lead to
	 * inconsistencies.  We have other defenses against
	 * CommandCounterIncrement, but there are a few places that call this
	 * directly, so we put an additional guard here.
	 */
	save_curcid = ActiveSnapshot->as_snap->curcid;
	curcid = GetCurrentCommandId(false);
	if (IsInParallelMode() && save_curcid != curcid)
		elog(ERROR, "cannot modify commandid in active snapshot during a parallel operation");
	ActiveSnapshot->as_snap->curcid = curcid;
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
	ActiveSnapshotElt *newstack;

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
 *		Return the topmost snapshot in the Active stack.
 */
Snapshot
GetActiveSnapshot(void)
{
	Assert(ActiveSnapshot != NULL);

	return ActiveSnapshot->as_snap;
}

/*
 * ActiveSnapshotSet
 *		Return whether there is at least one snapshot in the Active stack
 */
bool
ActiveSnapshotSet(void)
{
	return ActiveSnapshot != NULL;
}

/*
 * RegisterSnapshot
 *		Register a snapshot as being in use by the current resource owner
 *
 * If InvalidSnapshot is passed, it is not registered.
 */
Snapshot
RegisterSnapshot(Snapshot snapshot)
{
	if (snapshot == InvalidSnapshot)
		return InvalidSnapshot;

	return RegisterSnapshotOnOwner(snapshot, CurrentResourceOwner);
}

/*
 * RegisterSnapshotOnOwner
 *		As above, but use the specified resource owner
 */
Snapshot
RegisterSnapshotOnOwner(Snapshot snapshot, ResourceOwner owner)
{
	Snapshot	snap;

	if (snapshot == InvalidSnapshot)
		return InvalidSnapshot;

	/* Static snapshot?  Create a persistent copy */
	snap = snapshot->copied ? snapshot : CopySnapshot(snapshot);

	/* and tell resowner.c about it */
	ResourceOwnerEnlargeSnapshots(owner);
	snap->regd_count++;
	ResourceOwnerRememberSnapshot(owner, snap);

	if (snap->regd_count == 1)
		pairingheap_add(&RegisteredSnapshots, &snap->ph_node);

	return snap;
}

/*
 * UnregisterSnapshot
 *
 * Decrement the reference count of a snapshot, remove the corresponding
 * reference from CurrentResourceOwner, and free the snapshot if no more
 * references remain.
 */
void
UnregisterSnapshot(Snapshot snapshot)
{
	if (snapshot == NULL)
		return;

	UnregisterSnapshotFromOwner(snapshot, CurrentResourceOwner);
}

/*
 * UnregisterSnapshotFromOwner
 *		As above, but use the specified resource owner
 */
void
UnregisterSnapshotFromOwner(Snapshot snapshot, ResourceOwner owner)
{
	if (snapshot == NULL)
		return;

	Assert(snapshot->regd_count > 0);
	Assert(!pairingheap_is_empty(&RegisteredSnapshots));

	ResourceOwnerForgetSnapshot(owner, snapshot);

	snapshot->regd_count--;
	if (snapshot->regd_count == 0)
		pairingheap_remove(&RegisteredSnapshots, &snapshot->ph_node);

	if (snapshot->regd_count == 0 && snapshot->active_count == 0)
	{
		FreeSnapshot(snapshot);
		SnapshotResetXmin();
	}
}

/*
 * Comparison function for RegisteredSnapshots heap.  Snapshots are ordered
 * by xmin, so that the snapshot with smallest xmin is at the top.
 */
static int
xmin_cmp(const pairingheap_node *a, const pairingheap_node *b, void *arg)
{
	const SnapshotData *asnap = pairingheap_const_container(SnapshotData, ph_node, a);
	const SnapshotData *bsnap = pairingheap_const_container(SnapshotData, ph_node, b);

	if (TransactionIdPrecedes(asnap->xmin, bsnap->xmin))
		return 1;
	else if (TransactionIdFollows(asnap->xmin, bsnap->xmin))
		return -1;
	else
		return 0;
}

/*
 * SnapshotResetXmin
 *
 * If there are no more snapshots, we can reset our PGXACT->xmin to InvalidXid.
 * Note we can do this without locking because we assume that storing an Xid
 * is atomic.
 *
 * Even if there are some remaining snapshots, we may be able to advance our
 * PGXACT->xmin to some degree.  This typically happens when a portal is
 * dropped.  For efficiency, we only consider recomputing PGXACT->xmin when
 * the active snapshot stack is empty.
 */
static void
SnapshotResetXmin(void)
{
	Snapshot	minSnapshot;

	if (ActiveSnapshot != NULL)
		return;

	if (pairingheap_is_empty(&RegisteredSnapshots))
	{
		MyPgXact->xmin = InvalidTransactionId;
		return;
	}

	minSnapshot = pairingheap_container(SnapshotData, ph_node,
									pairingheap_first(&RegisteredSnapshots));

	if (TransactionIdPrecedes(MyPgXact->xmin, minSnapshot->xmin))
		MyPgXact->xmin = minSnapshot->xmin;
}

/*
 * AtSubCommit_Snapshot
 */
void
AtSubCommit_Snapshot(int level)
{
	ActiveSnapshotElt *active;

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
}

/*
 * AtSubAbort_Snapshot
 *		Clean up snapshots after a subtransaction abort
 */
void
AtSubAbort_Snapshot(int level)
{
	/* Forget the active snapshots set by this subtransaction */
	while (ActiveSnapshot && ActiveSnapshot->as_level >= level)
	{
		ActiveSnapshotElt *next;

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

	SnapshotResetXmin();
}

/*
 * AtEOXact_Snapshot
 *		Snapshot manager's cleanup function for end of transaction
 */
void
AtEOXact_Snapshot(bool isCommit)
{
	/*
	 * In transaction-snapshot mode we must release our privately-managed
	 * reference to the transaction snapshot.  We must decrement
	 * RegisteredSnapshots to keep the check below happy.  But we don't bother
	 * to do FreeSnapshot, for two reasons: the memory will go away with
	 * TopTransactionContext anyway, and if someone has left the snapshot
	 * stacked as active, we don't want the code below to be chasing through a
	 * dangling pointer.
	 */
	if (FirstXactSnapshot != NULL)
	{
		Assert(FirstXactSnapshot->regd_count > 0);
		Assert(!pairingheap_is_empty(&RegisteredSnapshots));
		pairingheap_remove(&RegisteredSnapshots, &FirstXactSnapshot->ph_node);
	}
	FirstXactSnapshot = NULL;

	/*
	 * If we exported any snapshots, clean them up.
	 */
	if (exportedSnapshots != NIL)
	{
		TransactionId myxid = GetTopTransactionId();
		int			i;
		char		buf[MAXPGPATH];
		ListCell   *lc;

		/*
		 * Get rid of the files.  Unlink failure is only a WARNING because (1)
		 * it's too late to abort the transaction, and (2) leaving a leaked
		 * file around has little real consequence anyway.
		 */
		for (i = 1; i <= list_length(exportedSnapshots); i++)
		{
			XactExportFilePath(buf, myxid, i, "");
			if (unlink(buf))
				elog(WARNING, "could not unlink file \"%s\": %m", buf);
		}

		/*
		 * As with the FirstXactSnapshot, we needn't spend any effort on
		 * cleaning up the per-snapshot data structures, but we do need to
		 * unlink them from RegisteredSnapshots to prevent a warning below.
		 */
		foreach(lc, exportedSnapshots)
		{
			Snapshot	snap = (Snapshot) lfirst(lc);

			pairingheap_remove(&RegisteredSnapshots, &snap->ph_node);
		}

		exportedSnapshots = NIL;
	}

	/* On commit, complain about leftover snapshots */
	if (isCommit)
	{
		ActiveSnapshotElt *active;

		if (!pairingheap_is_empty(&RegisteredSnapshots))
			elog(WARNING, "registered snapshots seem to remain after cleanup");

		/* complain about unpopped active snapshots */
		for (active = ActiveSnapshot; active != NULL; active = active->as_next)
			elog(WARNING, "snapshot %p still active", active);
	}

	/*
	 * And reset our state.  We don't need to free the memory explicitly --
	 * it'll go away with TopTransactionContext.
	 */
	ActiveSnapshot = NULL;
	pairingheap_reset(&RegisteredSnapshots);

	CurrentSnapshot = NULL;
	SecondarySnapshot = NULL;

	FirstSnapshotSet = false;

	SnapshotResetXmin();
}


/*
 * ExportSnapshot
 *		Export the snapshot to a file so that other backends can import it.
 *		Returns the token (the file name) that can be used to import this
 *		snapshot.
 */
char *
ExportSnapshot(Snapshot snapshot)
{
	TransactionId topXid;
	TransactionId *children;
	int			nchildren;
	int			addTopXid;
	StringInfoData buf;
	FILE	   *f;
	int			i;
	MemoryContext oldcxt;
	char		path[MAXPGPATH];
	char		pathtmp[MAXPGPATH];

	/*
	 * It's tempting to call RequireTransactionChain here, since it's not very
	 * useful to export a snapshot that will disappear immediately afterwards.
	 * However, we haven't got enough information to do that, since we don't
	 * know if we're at top level or not.  For example, we could be inside a
	 * plpgsql function that is going to fire off other transactions via
	 * dblink.  Rather than disallow perfectly legitimate usages, don't make a
	 * check.
	 *
	 * Also note that we don't make any restriction on the transaction's
	 * isolation level; however, importers must check the level if they are
	 * serializable.
	 */

	/*
	 * This will assign a transaction ID if we do not yet have one.
	 */
	topXid = GetTopTransactionId();

	/*
	 * We cannot export a snapshot from a subtransaction because there's no
	 * easy way for importers to verify that the same subtransaction is still
	 * running.
	 */
	if (IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot export a snapshot from a subtransaction")));

	/*
	 * We do however allow previous committed subtransactions to exist.
	 * Importers of the snapshot must see them as still running, so get their
	 * XIDs to add them to the snapshot.
	 */
	nchildren = xactGetCommittedChildren(&children);

	/*
	 * Copy the snapshot into TopTransactionContext, add it to the
	 * exportedSnapshots list, and mark it pseudo-registered.  We do this to
	 * ensure that the snapshot's xmin is honored for the rest of the
	 * transaction.
	 */
	snapshot = CopySnapshot(snapshot);

	oldcxt = MemoryContextSwitchTo(TopTransactionContext);
	exportedSnapshots = lappend(exportedSnapshots, snapshot);
	MemoryContextSwitchTo(oldcxt);

	snapshot->regd_count++;
	pairingheap_add(&RegisteredSnapshots, &snapshot->ph_node);

	/*
	 * Fill buf with a text serialization of the snapshot, plus identification
	 * data about this transaction.  The format expected by ImportSnapshot is
	 * pretty rigid: each line must be fieldname:value.
	 */
	initStringInfo(&buf);

	appendStringInfo(&buf, "xid:%u\n", topXid);
	appendStringInfo(&buf, "dbid:%u\n", MyDatabaseId);
	appendStringInfo(&buf, "iso:%d\n", XactIsoLevel);
	appendStringInfo(&buf, "ro:%d\n", XactReadOnly);

	appendStringInfo(&buf, "xmin:%u\n", snapshot->xmin);
	appendStringInfo(&buf, "xmax:%u\n", snapshot->xmax);

	/*
	 * We must include our own top transaction ID in the top-xid data, since
	 * by definition we will still be running when the importing transaction
	 * adopts the snapshot, but GetSnapshotData never includes our own XID in
	 * the snapshot.  (There must, therefore, be enough room to add it.)
	 *
	 * However, it could be that our topXid is after the xmax, in which case
	 * we shouldn't include it because xip[] members are expected to be before
	 * xmax.  (We need not make the same check for subxip[] members, see
	 * snapshot.h.)
	 */
	addTopXid = TransactionIdPrecedes(topXid, snapshot->xmax) ? 1 : 0;
	appendStringInfo(&buf, "xcnt:%d\n", snapshot->xcnt + addTopXid);
	for (i = 0; i < snapshot->xcnt; i++)
		appendStringInfo(&buf, "xip:%u\n", snapshot->xip[i]);
	if (addTopXid)
		appendStringInfo(&buf, "xip:%u\n", topXid);

	/*
	 * Similarly, we add our subcommitted child XIDs to the subxid data. Here,
	 * we have to cope with possible overflow.
	 */
	if (snapshot->suboverflowed ||
		snapshot->subxcnt + nchildren > GetMaxSnapshotSubxidCount())
		appendStringInfoString(&buf, "sof:1\n");
	else
	{
		appendStringInfoString(&buf, "sof:0\n");
		appendStringInfo(&buf, "sxcnt:%d\n", snapshot->subxcnt + nchildren);
		for (i = 0; i < snapshot->subxcnt; i++)
			appendStringInfo(&buf, "sxp:%u\n", snapshot->subxip[i]);
		for (i = 0; i < nchildren; i++)
			appendStringInfo(&buf, "sxp:%u\n", children[i]);
	}
	appendStringInfo(&buf, "rec:%u\n", snapshot->takenDuringRecovery);

	/*
	 * Now write the text representation into a file.  We first write to a
	 * ".tmp" filename, and rename to final filename if no error.  This
	 * ensures that no other backend can read an incomplete file
	 * (ImportSnapshot won't allow it because of its valid-characters check).
	 */
	XactExportFilePath(pathtmp, topXid, list_length(exportedSnapshots), ".tmp");
	if (!(f = AllocateFile(pathtmp, PG_BINARY_W)))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create file \"%s\": %m", pathtmp)));

	if (fwrite(buf.data, buf.len, 1, f) != 1)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", pathtmp)));

	/* no fsync() since file need not survive a system crash */

	if (FreeFile(f))
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write to file \"%s\": %m", pathtmp)));

	/*
	 * Now that we have written everything into a .tmp file, rename the file
	 * to remove the .tmp suffix.
	 */
	XactExportFilePath(path, topXid, list_length(exportedSnapshots), "");

	if (rename(pathtmp, path) < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not rename file \"%s\" to \"%s\": %m",
						pathtmp, path)));

	/*
	 * The basename of the file is what we return from pg_export_snapshot().
	 * It's already in path in a textual format and we know that the path
	 * starts with SNAPSHOT_EXPORT_DIR.  Skip over the prefix and the slash
	 * and pstrdup it so as not to return the address of a local variable.
	 */
	return pstrdup(path + strlen(SNAPSHOT_EXPORT_DIR) + 1);
}

/*
 * pg_export_snapshot
 *		SQL-callable wrapper for ExportSnapshot.
 */
Datum
pg_export_snapshot(PG_FUNCTION_ARGS)
{
	char	   *snapshotName;

	snapshotName = ExportSnapshot(GetActiveSnapshot());
	PG_RETURN_TEXT_P(cstring_to_text(snapshotName));
}


/*
 * Parsing subroutines for ImportSnapshot: parse a line with the given
 * prefix followed by a value, and advance *s to the next line.  The
 * filename is provided for use in error messages.
 */
static int
parseIntFromText(const char *prefix, char **s, const char *filename)
{
	char	   *ptr = *s;
	int			prefixlen = strlen(prefix);
	int			val;

	if (strncmp(ptr, prefix, prefixlen) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr += prefixlen;
	if (sscanf(ptr, "%d", &val) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr = strchr(ptr, '\n');
	if (!ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	*s = ptr + 1;
	return val;
}

static TransactionId
parseXidFromText(const char *prefix, char **s, const char *filename)
{
	char	   *ptr = *s;
	int			prefixlen = strlen(prefix);
	TransactionId val;

	if (strncmp(ptr, prefix, prefixlen) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr += prefixlen;
	if (sscanf(ptr, "%u", &val) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	ptr = strchr(ptr, '\n');
	if (!ptr)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", filename)));
	*s = ptr + 1;
	return val;
}

/*
 * ImportSnapshot
 *		Import a previously exported snapshot.  The argument should be a
 *		filename in SNAPSHOT_EXPORT_DIR.  Load the snapshot from that file.
 *		This is called by "SET TRANSACTION SNAPSHOT 'foo'".
 */
void
ImportSnapshot(const char *idstr)
{
	char		path[MAXPGPATH];
	FILE	   *f;
	struct stat stat_buf;
	char	   *filebuf;
	int			xcnt;
	int			i;
	TransactionId src_xid;
	Oid			src_dbid;
	int			src_isolevel;
	bool		src_readonly;
	SnapshotData snapshot;

	/*
	 * Must be at top level of a fresh transaction.  Note in particular that
	 * we check we haven't acquired an XID --- if we have, it's conceivable
	 * that the snapshot would show it as not running, making for very screwy
	 * behavior.
	 */
	if (FirstSnapshotSet ||
		GetTopTransactionIdIfAny() != InvalidTransactionId ||
		IsSubTransaction())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
		errmsg("SET TRANSACTION SNAPSHOT must be called before any query")));

	/*
	 * If we are in read committed mode then the next query would execute with
	 * a new snapshot thus making this function call quite useless.
	 */
	if (!IsolationUsesXactSnapshot())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a snapshot-importing transaction must have isolation level SERIALIZABLE or REPEATABLE READ")));

	/*
	 * Verify the identifier: only 0-9, A-F and hyphens are allowed.  We do
	 * this mainly to prevent reading arbitrary files.
	 */
	if (strspn(idstr, "0123456789ABCDEF-") != strlen(idstr))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid snapshot identifier: \"%s\"", idstr)));

	/* OK, read the file */
	snprintf(path, MAXPGPATH, SNAPSHOT_EXPORT_DIR "/%s", idstr);

	f = AllocateFile(path, PG_BINARY_R);
	if (!f)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid snapshot identifier: \"%s\"", idstr)));

	/* get the size of the file so that we know how much memory we need */
	if (fstat(fileno(f), &stat_buf))
		elog(ERROR, "could not stat file \"%s\": %m", path);

	/* and read the file into a palloc'd string */
	filebuf = (char *) palloc(stat_buf.st_size + 1);
	if (fread(filebuf, stat_buf.st_size, 1, f) != 1)
		elog(ERROR, "could not read file \"%s\": %m", path);

	filebuf[stat_buf.st_size] = '\0';

	FreeFile(f);

	/*
	 * Construct a snapshot struct by parsing the file content.
	 */
	memset(&snapshot, 0, sizeof(snapshot));

	src_xid = parseXidFromText("xid:", &filebuf, path);
	/* we abuse parseXidFromText a bit here ... */
	src_dbid = parseXidFromText("dbid:", &filebuf, path);
	src_isolevel = parseIntFromText("iso:", &filebuf, path);
	src_readonly = parseIntFromText("ro:", &filebuf, path);

	snapshot.xmin = parseXidFromText("xmin:", &filebuf, path);
	snapshot.xmax = parseXidFromText("xmax:", &filebuf, path);

	snapshot.xcnt = xcnt = parseIntFromText("xcnt:", &filebuf, path);

	/* sanity-check the xid count before palloc */
	if (xcnt < 0 || xcnt > GetMaxSnapshotXidCount())
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", path)));

	snapshot.xip = (TransactionId *) palloc(xcnt * sizeof(TransactionId));
	for (i = 0; i < xcnt; i++)
		snapshot.xip[i] = parseXidFromText("xip:", &filebuf, path);

	snapshot.suboverflowed = parseIntFromText("sof:", &filebuf, path);

	if (!snapshot.suboverflowed)
	{
		snapshot.subxcnt = xcnt = parseIntFromText("sxcnt:", &filebuf, path);

		/* sanity-check the xid count before palloc */
		if (xcnt < 0 || xcnt > GetMaxSnapshotSubxidCount())
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("invalid snapshot data in file \"%s\"", path)));

		snapshot.subxip = (TransactionId *) palloc(xcnt * sizeof(TransactionId));
		for (i = 0; i < xcnt; i++)
			snapshot.subxip[i] = parseXidFromText("sxp:", &filebuf, path);
	}
	else
	{
		snapshot.subxcnt = 0;
		snapshot.subxip = NULL;
	}

	snapshot.takenDuringRecovery = parseIntFromText("rec:", &filebuf, path);

	/*
	 * Do some additional sanity checking, just to protect ourselves.  We
	 * don't trouble to check the array elements, just the most critical
	 * fields.
	 */
	if (!TransactionIdIsNormal(src_xid) ||
		!OidIsValid(src_dbid) ||
		!TransactionIdIsNormal(snapshot.xmin) ||
		!TransactionIdIsNormal(snapshot.xmax))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid snapshot data in file \"%s\"", path)));

	/*
	 * If we're serializable, the source transaction must be too, otherwise
	 * predicate.c has problems (SxactGlobalXmin could go backwards).  Also, a
	 * non-read-only transaction can't adopt a snapshot from a read-only
	 * transaction, as predicate.c handles the cases very differently.
	 */
	if (IsolationIsSerializable())
	{
		if (src_isolevel != XACT_SERIALIZABLE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a serializable transaction cannot import a snapshot from a non-serializable transaction")));
		if (src_readonly && !XactReadOnly)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("a non-read-only serializable transaction cannot import a snapshot from a read-only transaction")));
	}

	/*
	 * We cannot import a snapshot that was taken in a different database,
	 * because vacuum calculates OldestXmin on a per-database basis; so the
	 * source transaction's xmin doesn't protect us from data loss.  This
	 * restriction could be removed if the source transaction were to mark its
	 * xmin as being globally applicable.  But that would require some
	 * additional syntax, since that has to be known when the snapshot is
	 * initially taken.  (See pgsql-hackers discussion of 2011-10-21.)
	 */
	if (src_dbid != MyDatabaseId)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			  errmsg("cannot import a snapshot from a different database")));

	/* OK, install the snapshot */
	SetTransactionSnapshot(&snapshot, src_xid, NULL);
}

/*
 * XactHasExportedSnapshots
 *		Test whether current transaction has exported any snapshots.
 */
bool
XactHasExportedSnapshots(void)
{
	return (exportedSnapshots != NIL);
}

/*
 * DeleteAllExportedSnapshotFiles
 *		Clean up any files that have been left behind by a crashed backend
 *		that had exported snapshots before it died.
 *
 * This should be called during database startup or crash recovery.
 */
void
DeleteAllExportedSnapshotFiles(void)
{
	char		buf[MAXPGPATH];
	DIR		   *s_dir;
	struct dirent *s_de;

	if (!(s_dir = AllocateDir(SNAPSHOT_EXPORT_DIR)))
	{
		/*
		 * We really should have that directory in a sane cluster setup. But
		 * then again if we don't, it's not fatal enough to make it FATAL.
		 * Since we're running in the postmaster, LOG is our best bet.
		 */
		elog(LOG, "could not open directory \"%s\": %m", SNAPSHOT_EXPORT_DIR);
		return;
	}

	while ((s_de = ReadDir(s_dir, SNAPSHOT_EXPORT_DIR)) != NULL)
	{
		if (strcmp(s_de->d_name, ".") == 0 ||
			strcmp(s_de->d_name, "..") == 0)
			continue;

		snprintf(buf, MAXPGPATH, SNAPSHOT_EXPORT_DIR "/%s", s_de->d_name);
		/* Again, unlink failure is not worthy of FATAL */
		if (unlink(buf))
			elog(LOG, "could not unlink file \"%s\": %m", buf);
	}

	FreeDir(s_dir);
}

bool
ThereAreNoPriorRegisteredSnapshots(void)
{
	if (pairingheap_is_empty(&RegisteredSnapshots) ||
		pairingheap_is_singular(&RegisteredSnapshots))
		return true;

	return false;
}

/*
 * Setup a snapshot that replaces normal catalog snapshots that allows catalog
 * access to behave just like it did at a certain point in the past.
 *
 * Needed for logical decoding.
 */
void
SetupHistoricSnapshot(Snapshot historic_snapshot, HTAB *tuplecids)
{
	Assert(historic_snapshot != NULL);

	/* setup the timetravel snapshot */
	HistoricSnapshot = historic_snapshot;

	/* setup (cmin, cmax) lookup hash */
	tuplecid_data = tuplecids;
}


/*
 * Make catalog snapshots behave normally again.
 */
void
TeardownHistoricSnapshot(bool is_error)
{
	HistoricSnapshot = NULL;
	tuplecid_data = NULL;
}

bool
HistoricSnapshotActive(void)
{
	return HistoricSnapshot != NULL;
}

HTAB *
HistoricSnapshotGetTupleCids(void)
{
	Assert(HistoricSnapshotActive());
	return tuplecid_data;
}

/*
 * EstimateSnapshotSpace
 *		Returns the size need to store the given snapshot.
 *
 * We are exporting only required fields from the Snapshot, stored in
 * SerializedSnapshotData.
 */
Size
EstimateSnapshotSpace(Snapshot snap)
{
	Size		size;

	Assert(snap != InvalidSnapshot);
	Assert(snap->satisfies == HeapTupleSatisfiesMVCC);

	/* We allocate any XID arrays needed in the same palloc block. */
	size = add_size(sizeof(SerializedSnapshotData),
					mul_size(snap->xcnt, sizeof(TransactionId)));
	if (snap->subxcnt > 0 &&
		(!snap->suboverflowed || snap->takenDuringRecovery))
		size = add_size(size,
						mul_size(snap->subxcnt, sizeof(TransactionId)));

	return size;
}

/*
 * SerializeSnapshot
 *		Dumps the serialized snapshot (extracted from given snapshot) onto the
 *		memory location at start_address.
 */
void
SerializeSnapshot(Snapshot snapshot, char *start_address)
{
	SerializedSnapshotData *serialized_snapshot;

	Assert(snapshot->subxcnt >= 0);

	serialized_snapshot = (SerializedSnapshotData *) start_address;

	/* Copy all required fields */
	serialized_snapshot->xmin = snapshot->xmin;
	serialized_snapshot->xmax = snapshot->xmax;
	serialized_snapshot->xcnt = snapshot->xcnt;
	serialized_snapshot->subxcnt = snapshot->subxcnt;
	serialized_snapshot->suboverflowed = snapshot->suboverflowed;
	serialized_snapshot->takenDuringRecovery = snapshot->takenDuringRecovery;
	serialized_snapshot->curcid = snapshot->curcid;

	/*
	 * Ignore the SubXID array if it has overflowed, unless the snapshot was
	 * taken during recovey - in that case, top-level XIDs are in subxip as
	 * well, and we mustn't lose them.
	 */
	if (serialized_snapshot->suboverflowed && !snapshot->takenDuringRecovery)
		serialized_snapshot->subxcnt = 0;

	/* Copy XID array */
	if (snapshot->xcnt > 0)
		memcpy((TransactionId *) (serialized_snapshot + 1),
			   snapshot->xip, snapshot->xcnt * sizeof(TransactionId));

	/*
	 * Copy SubXID array. Don't bother to copy it if it had overflowed,
	 * though, because it's not used anywhere in that case. Except if it's a
	 * snapshot taken during recovery; all the top-level XIDs are in subxip as
	 * well in that case, so we mustn't lose them.
	 */
	if (serialized_snapshot->subxcnt > 0)
	{
		Size		subxipoff = sizeof(SerializedSnapshotData) +
		snapshot->xcnt * sizeof(TransactionId);

		memcpy((TransactionId *) ((char *) serialized_snapshot + subxipoff),
			   snapshot->subxip, snapshot->subxcnt * sizeof(TransactionId));
	}
}

/*
 * RestoreSnapshot
 *		Restore a serialized snapshot from the specified address.
 *
 * The copy is palloc'd in TopTransactionContext and has initial refcounts set
 * to 0.  The returned snapshot has the copied flag set.
 */
Snapshot
RestoreSnapshot(char *start_address)
{
	SerializedSnapshotData *serialized_snapshot;
	Size		size;
	Snapshot	snapshot;
	TransactionId *serialized_xids;

	serialized_snapshot = (SerializedSnapshotData *) start_address;
	serialized_xids = (TransactionId *)
		(start_address + sizeof(SerializedSnapshotData));

	/* We allocate any XID arrays needed in the same palloc block. */
	size = sizeof(SnapshotData)
		+ serialized_snapshot->xcnt * sizeof(TransactionId)
		+ serialized_snapshot->subxcnt * sizeof(TransactionId);

	/* Copy all required fields */
	snapshot = (Snapshot) MemoryContextAlloc(TopTransactionContext, size);
	snapshot->satisfies = HeapTupleSatisfiesMVCC;
	snapshot->xmin = serialized_snapshot->xmin;
	snapshot->xmax = serialized_snapshot->xmax;
	snapshot->xip = NULL;
	snapshot->xcnt = serialized_snapshot->xcnt;
	snapshot->subxip = NULL;
	snapshot->subxcnt = serialized_snapshot->subxcnt;
	snapshot->suboverflowed = serialized_snapshot->suboverflowed;
	snapshot->takenDuringRecovery = serialized_snapshot->takenDuringRecovery;
	snapshot->curcid = serialized_snapshot->curcid;

	/* Copy XIDs, if present. */
	if (serialized_snapshot->xcnt > 0)
	{
		snapshot->xip = (TransactionId *) (snapshot + 1);
		memcpy(snapshot->xip, serialized_xids,
			   serialized_snapshot->xcnt * sizeof(TransactionId));
	}

	/* Copy SubXIDs, if present. */
	if (serialized_snapshot->subxcnt > 0)
	{
		snapshot->subxip = ((TransactionId *) (snapshot + 1)) +
			serialized_snapshot->xcnt;
		memcpy(snapshot->subxip, serialized_xids + serialized_snapshot->xcnt,
			   serialized_snapshot->subxcnt * sizeof(TransactionId));
	}

	/* Set the copied flag so that the caller will set refcounts correctly. */
	snapshot->regd_count = 0;
	snapshot->active_count = 0;
	snapshot->copied = true;

	return snapshot;
}

/*
 * Install a restored snapshot as the transaction snapshot.
 *
 * The second argument is of type void * so that snapmgr.h need not include
 * the declaration for PGPROC.
 */
void
RestoreTransactionSnapshot(Snapshot snapshot, void *master_pgproc)
{
	SetTransactionSnapshot(snapshot, InvalidTransactionId, master_pgproc);
}
