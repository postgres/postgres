/*-------------------------------------------------------------------------
 *
 * multixact.c
 *		PostgreSQL multi-transaction-log manager
 *
 * The pg_multixact manager is a pg_xact-like manager that stores an array of
 * MultiXactMember for each MultiXactId.  It is a fundamental part of the
 * shared-row-lock implementation.  Each MultiXactMember is comprised of a
 * TransactionId and a set of flag bits.  The name is a bit historical:
 * originally, a MultiXactId consisted of more than one TransactionId (except
 * in rare corner cases), hence "multi".  Nowadays, however, it's perfectly
 * legitimate to have MultiXactIds that only include a single Xid.
 *
 * The meaning of the flag bits is opaque to this module, but they are mostly
 * used in heapam.c to identify lock modes that each of the member transactions
 * is holding on any given tuple.  This module just contains support to store
 * and retrieve the arrays.
 *
 * We use two SLRU areas, one for storing the offsets at which the data
 * starts for each MultiXactId in the other one.  This trick allows us to
 * store variable length arrays of TransactionIds.  (We could alternatively
 * use one area containing counts and TransactionIds, with valid MultiXactId
 * values pointing at slots containing counts; but that way seems less robust
 * since it would get completely confused if someone inquired about a bogus
 * MultiXactId that pointed to an intermediate slot containing an XID.)
 *
 * XLOG interactions: this module generates a record whenever a new OFFSETs or
 * MEMBERs page is initialized to zeroes, as well as an
 * XLOG_MULTIXACT_CREATE_ID record whenever a new MultiXactId is defined.
 * This module ignores the WAL rule "write xlog before data," because it
 * suffices that actions recording a MultiXactId in a heap xmax do follow that
 * rule.  The only way for the MXID to be referenced from any data page is for
 * heap_lock_tuple() or heap_update() to have put it there, and each generates
 * an XLOG record that must follow ours.  The normal LSN interlock between the
 * data page and that XLOG record will ensure that our XLOG record reaches
 * disk first.  If the SLRU members/offsets data reaches disk sooner than the
 * XLOG records, we do not care; after recovery, no xmax will refer to it.  On
 * the flip side, to ensure that all referenced entries _do_ reach disk, this
 * module's XLOG records completely rebuild the data entered since the last
 * checkpoint.  We flush and sync all dirty OFFSETs and MEMBERs pages to disk
 * before each checkpoint is considered complete.
 *
 * Like clog.c, and unlike subtrans.c, we have to preserve state across
 * crashes and ensure that MXID and offset numbering increases monotonically
 * across a crash.  We do this in the same way as it's done for transaction
 * IDs: the WAL record is guaranteed to contain evidence of every MXID we
 * could need to worry about, and we just make sure that at the end of
 * replay, the next-MXID and next-offset counters are at least as large as
 * anything we saw during replay.
 *
 * We are able to remove segments no longer necessary by carefully tracking
 * each table's used values: during vacuum, any multixact older than a certain
 * value is removed; the cutoff value is stored in pg_class.  The minimum value
 * across all tables in each database is stored in pg_database, and the global
 * minimum across all databases is part of pg_control and is kept in shared
 * memory.  Whenever that minimum is advanced, the SLRUs are truncated.
 *
 * When new multixactid values are to be created, care is taken that the
 * counter does not fall within the wraparound horizon considering the global
 * minimum value.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/multixact.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/multixact_internal.h"
#include "access/slru.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/pmsignal.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"


/*
 * Thresholds used to keep members disk usage in check when multixids have a
 * lot of members.  When MULTIXACT_MEMBER_LOW_THRESHOLD is reached, vacuum
 * starts freezing multixids more aggressively, even if the normal multixid
 * age limits haven't been reached yet.
 */
#define MULTIXACT_MEMBER_LOW_THRESHOLD		UINT64CONST(2000000000)
#define MULTIXACT_MEMBER_HIGH_THRESHOLD		UINT64CONST(4000000000)

static inline MultiXactId
PreviousMultiXactId(MultiXactId multi)
{
	return multi == FirstMultiXactId ? MaxMultiXactId : multi - 1;
}

/*
 * Links to shared-memory data structures for MultiXact control
 */
static SlruCtlData MultiXactOffsetCtlData;
static SlruCtlData MultiXactMemberCtlData;

#define MultiXactOffsetCtl	(&MultiXactOffsetCtlData)
#define MultiXactMemberCtl	(&MultiXactMemberCtlData)

/*
 * MultiXact state shared across all backends.  All this state is protected
 * by MultiXactGenLock.  (We also use SLRU bank's lock of MultiXactOffset and
 * MultiXactMember to guard accesses to the two sets of SLRU buffers.  For
 * concurrency's sake, we avoid holding more than one of these locks at a
 * time.)
 */
typedef struct MultiXactStateData
{
	/* next-to-be-assigned MultiXactId */
	MultiXactId nextMXact;

	/* next-to-be-assigned offset */
	MultiXactOffset nextOffset;

	/* Have we completed multixact startup? */
	bool		finishedStartup;

	/*
	 * Oldest multixact that is still potentially referenced by a relation.
	 * Anything older than this should not be consulted.  These values are
	 * updated by vacuum.
	 */
	MultiXactId oldestMultiXactId;
	Oid			oldestMultiXactDB;

	/*
	 * Oldest multixact offset that is potentially referenced by a multixact
	 * referenced by a relation.
	 */
	MultiXactOffset oldestOffset;

	/* support for anti-wraparound measures */
	MultiXactId multiVacLimit;
	MultiXactId multiWarnLimit;
	MultiXactId multiStopLimit;
	MultiXactId multiWrapLimit;

	/*
	 * Per-backend data starts here.  We have two arrays stored in the area
	 * immediately following the MultiXactStateData struct. Each is indexed by
	 * ProcNumber.
	 *
	 * In both arrays, there's a slot for all normal backends
	 * (0..MaxBackends-1) followed by a slot for max_prepared_xacts prepared
	 * transactions.
	 *
	 * OldestMemberMXactId[k] is the oldest MultiXactId each backend's current
	 * transaction(s) could possibly be a member of, or InvalidMultiXactId
	 * when the backend has no live transaction that could possibly be a
	 * member of a MultiXact.  Each backend sets its entry to the current
	 * nextMXact counter just before first acquiring a shared lock in a given
	 * transaction, and clears it at transaction end. (This works because only
	 * during or after acquiring a shared lock could an XID possibly become a
	 * member of a MultiXact, and that MultiXact would have to be created
	 * during or after the lock acquisition.)
	 *
	 * OldestVisibleMXactId[k] is the oldest MultiXactId each backend's
	 * current transaction(s) think is potentially live, or InvalidMultiXactId
	 * when not in a transaction or not in a transaction that's paid any
	 * attention to MultiXacts yet.  This is computed when first needed in a
	 * given transaction, and cleared at transaction end.  We can compute it
	 * as the minimum of the valid OldestMemberMXactId[] entries at the time
	 * we compute it (using nextMXact if none are valid).  Each backend is
	 * required not to attempt to access any SLRU data for MultiXactIds older
	 * than its own OldestVisibleMXactId[] setting; this is necessary because
	 * the relevant SLRU data can be concurrently truncated away.
	 *
	 * The oldest valid value among all of the OldestMemberMXactId[] and
	 * OldestVisibleMXactId[] entries is considered by vacuum as the earliest
	 * possible value still having any live member transaction -- OldestMxact.
	 * Any value older than that is typically removed from tuple headers, or
	 * "frozen" via being replaced with a new xmax.  VACUUM can sometimes even
	 * remove an individual MultiXact xmax whose value is >= its OldestMxact
	 * cutoff, though typically only when no individual member XID is still
	 * running.  See FreezeMultiXactId for full details.
	 *
	 * Whenever VACUUM advances relminmxid, then either its OldestMxact cutoff
	 * or the oldest extant Multi remaining in the table is used as the new
	 * pg_class.relminmxid value (whichever is earlier).  The minimum of all
	 * relminmxid values in each database is stored in pg_database.datminmxid.
	 * In turn, the minimum of all of those values is stored in pg_control.
	 * This is used as the truncation point for pg_multixact when unneeded
	 * segments get removed by vac_truncate_clog() during vacuuming.
	 */
	MultiXactId perBackendXactIds[FLEXIBLE_ARRAY_MEMBER];
} MultiXactStateData;

/*
 * Size of OldestMemberMXactId and OldestVisibleMXactId arrays.
 */
#define MaxOldestSlot	(MaxBackends + max_prepared_xacts)

/* Pointers to the state data in shared memory */
static MultiXactStateData *MultiXactState;
static MultiXactId *OldestMemberMXactId;
static MultiXactId *OldestVisibleMXactId;


/*
 * Definitions for the backend-local MultiXactId cache.
 *
 * We use this cache to store known MultiXacts, so we don't need to go to
 * SLRU areas every time.
 *
 * The cache lasts for the duration of a single transaction, the rationale
 * for this being that most entries will contain our own TransactionId and
 * so they will be uninteresting by the time our next transaction starts.
 * (XXX not clear that this is correct --- other members of the MultiXact
 * could hang around longer than we did.  However, it's not clear what a
 * better policy for flushing old cache entries would be.)	FIXME actually
 * this is plain wrong now that multixact's may contain update Xids.
 *
 * We allocate the cache entries in a memory context that is deleted at
 * transaction end, so we don't need to do retail freeing of entries.
 */
typedef struct mXactCacheEnt
{
	MultiXactId multi;
	int			nmembers;
	dlist_node	node;
	MultiXactMember members[FLEXIBLE_ARRAY_MEMBER];
} mXactCacheEnt;

#define MAX_CACHE_ENTRIES	256
static dclist_head MXactCache = DCLIST_STATIC_INIT(MXactCache);
static MemoryContext MXactContext = NULL;

#ifdef MULTIXACT_DEBUG
#define debug_elog2(a,b) elog(a,b)
#define debug_elog3(a,b,c) elog(a,b,c)
#define debug_elog4(a,b,c,d) elog(a,b,c,d)
#define debug_elog5(a,b,c,d,e) elog(a,b,c,d,e)
#define debug_elog6(a,b,c,d,e,f) elog(a,b,c,d,e,f)
#else
#define debug_elog2(a,b)
#define debug_elog3(a,b,c)
#define debug_elog4(a,b,c,d)
#define debug_elog5(a,b,c,d,e)
#define debug_elog6(a,b,c,d,e,f)
#endif

/* internal MultiXactId management */
static void MultiXactIdSetOldestVisible(void);
static void RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
							   int nmembers, MultiXactMember *members);
static MultiXactId GetNewMultiXactId(int nmembers, MultiXactOffset *offset);

/* MultiXact cache management */
static int	mxactMemberComparator(const void *arg1, const void *arg2);
static MultiXactId mXactCacheGetBySet(int nmembers, MultiXactMember *members);
static int	mXactCacheGetById(MultiXactId multi, MultiXactMember **members);
static void mXactCachePut(MultiXactId multi, int nmembers,
						  MultiXactMember *members);

/* management of SLRU infrastructure */
static bool MultiXactOffsetPagePrecedes(int64 page1, int64 page2);
static bool MultiXactMemberPagePrecedes(int64 page1, int64 page2);
static void ExtendMultiXactOffset(MultiXactId multi);
static void ExtendMultiXactMember(MultiXactOffset offset, int nmembers);
static void SetOldestOffset(void);
static bool find_multixact_start(MultiXactId multi, MultiXactOffset *result);
static void WriteMTruncateXlogRec(Oid oldestMultiDB,
								  MultiXactId startTruncOff,
								  MultiXactId endTruncOff,
								  MultiXactOffset startTruncMemb,
								  MultiXactOffset endTruncMemb);


/*
 * MultiXactIdCreate
 *		Construct a MultiXactId representing two TransactionIds.
 *
 * The two XIDs must be different, or be requesting different statuses.
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
MultiXactId
MultiXactIdCreate(TransactionId xid1, MultiXactStatus status1,
				  TransactionId xid2, MultiXactStatus status2)
{
	MultiXactId newMulti;
	MultiXactMember members[2];

	Assert(TransactionIdIsValid(xid1));
	Assert(TransactionIdIsValid(xid2));

	Assert(!TransactionIdEquals(xid1, xid2) || (status1 != status2));

	/* MultiXactIdSetOldestMember() must have been called already. */
	Assert(MultiXactIdIsValid(OldestMemberMXactId[MyProcNumber]));

	/*
	 * Note: unlike MultiXactIdExpand, we don't bother to check that both XIDs
	 * are still running.  In typical usage, xid2 will be our own XID and the
	 * caller just did a check on xid1, so it'd be wasted effort.
	 */

	members[0].xid = xid1;
	members[0].status = status1;
	members[1].xid = xid2;
	members[1].status = status2;

	newMulti = MultiXactIdCreateFromMembers(2, members);

	debug_elog3(DEBUG2, "Create: %s",
				mxid_to_string(newMulti, 2, members));

	return newMulti;
}

/*
 * MultiXactIdExpand
 *		Add a TransactionId to a pre-existing MultiXactId.
 *
 * If the TransactionId is already a member of the passed MultiXactId with the
 * same status, just return it as-is.
 *
 * Note that we do NOT actually modify the membership of a pre-existing
 * MultiXactId; instead we create a new one.  This is necessary to avoid
 * a race condition against code trying to wait for one MultiXactId to finish;
 * see notes in heapam.c.
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 *
 * Note: It is critical that MultiXactIds that come from an old cluster (i.e.
 * one upgraded by pg_upgrade from a cluster older than this feature) are not
 * passed in.
 */
MultiXactId
MultiXactIdExpand(MultiXactId multi, TransactionId xid, MultiXactStatus status)
{
	MultiXactId newMulti;
	MultiXactMember *members;
	MultiXactMember *newMembers;
	int			nmembers;
	int			i;
	int			j;

	Assert(MultiXactIdIsValid(multi));
	Assert(TransactionIdIsValid(xid));

	/* MultiXactIdSetOldestMember() must have been called already. */
	Assert(MultiXactIdIsValid(OldestMemberMXactId[MyProcNumber]));

	debug_elog5(DEBUG2, "Expand: received multi %u, xid %u status %s",
				multi, xid, mxstatus_to_string(status));

	/*
	 * Note: we don't allow for old multis here.  The reason is that the only
	 * caller of this function does a check that the multixact is no longer
	 * running.
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false, false);

	if (nmembers < 0)
	{
		MultiXactMember member;

		/*
		 * The MultiXactId is obsolete.  This can only happen if all the
		 * MultiXactId members stop running between the caller checking and
		 * passing it to us.  It would be better to return that fact to the
		 * caller, but it would complicate the API and it's unlikely to happen
		 * too often, so just deal with it by creating a singleton MultiXact.
		 */
		member.xid = xid;
		member.status = status;
		newMulti = MultiXactIdCreateFromMembers(1, &member);

		debug_elog4(DEBUG2, "Expand: %u has no members, create singleton %u",
					multi, newMulti);
		return newMulti;
	}

	/*
	 * If the TransactionId is already a member of the MultiXactId with the
	 * same status, just return the existing MultiXactId.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdEquals(members[i].xid, xid) &&
			(members[i].status == status))
		{
			debug_elog4(DEBUG2, "Expand: %u is already a member of %u",
						xid, multi);
			pfree(members);
			return multi;
		}
	}

	/*
	 * Determine which of the members of the MultiXactId are still of
	 * interest. This is any running transaction, and also any transaction
	 * that grabbed something stronger than just a lock and was committed. (An
	 * update that aborted is of no interest here; and having more than one
	 * update Xid in a multixact would cause errors elsewhere.)
	 *
	 * Removing dead members is not just an optimization: freezing of tuples
	 * whose Xmax are multis depends on this behavior.
	 *
	 * Note we have the same race condition here as above: j could be 0 at the
	 * end of the loop.
	 */
	newMembers = (MultiXactMember *)
		palloc(sizeof(MultiXactMember) * (nmembers + 1));

	for (i = 0, j = 0; i < nmembers; i++)
	{
		if (TransactionIdIsInProgress(members[i].xid) ||
			(ISUPDATE_from_mxstatus(members[i].status) &&
			 TransactionIdDidCommit(members[i].xid)))
		{
			newMembers[j].xid = members[i].xid;
			newMembers[j++].status = members[i].status;
		}
	}

	newMembers[j].xid = xid;
	newMembers[j++].status = status;
	newMulti = MultiXactIdCreateFromMembers(j, newMembers);

	pfree(members);
	pfree(newMembers);

	debug_elog3(DEBUG2, "Expand: returning new multi %u", newMulti);

	return newMulti;
}

/*
 * MultiXactIdIsRunning
 *		Returns whether a MultiXactId is "running".
 *
 * We return true if at least one member of the given MultiXactId is still
 * running.  Note that a "false" result is certain not to change,
 * because it is not legal to add members to an existing MultiXactId.
 *
 * Caller is expected to have verified that the multixact does not come from
 * a pg_upgraded share-locked tuple.
 */
bool
MultiXactIdIsRunning(MultiXactId multi, bool isLockOnly)
{
	MultiXactMember *members;
	int			nmembers;
	int			i;

	debug_elog3(DEBUG2, "IsRunning %u?", multi);

	/*
	 * "false" here means we assume our callers have checked that the given
	 * multi cannot possibly come from a pg_upgraded database.
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false, isLockOnly);

	if (nmembers <= 0)
	{
		debug_elog2(DEBUG2, "IsRunning: no members");
		return false;
	}

	/*
	 * Checking for myself is cheap compared to looking in shared memory;
	 * return true if any live subtransaction of the current top-level
	 * transaction is a member.
	 *
	 * This is not needed for correctness, it's just a fast path.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsCurrentTransactionId(members[i].xid))
		{
			debug_elog3(DEBUG2, "IsRunning: I (%d) am running!", i);
			pfree(members);
			return true;
		}
	}

	/*
	 * This could be made faster by having another entry point in procarray.c,
	 * walking the PGPROC array only once for all the members.  But in most
	 * cases nmembers should be small enough that it doesn't much matter.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsInProgress(members[i].xid))
		{
			debug_elog4(DEBUG2, "IsRunning: member %d (%u) is running",
						i, members[i].xid);
			pfree(members);
			return true;
		}
	}

	pfree(members);

	debug_elog3(DEBUG2, "IsRunning: %u is not running", multi);

	return false;
}

/*
 * MultiXactIdSetOldestMember
 *		Save the oldest MultiXactId this transaction could be a member of.
 *
 * We set the OldestMemberMXactId for a given transaction the first time it's
 * going to do some operation that might require a MultiXactId (tuple lock,
 * update or delete).  We need to do this even if we end up using a
 * TransactionId instead of a MultiXactId, because there is a chance that
 * another transaction would add our XID to a MultiXactId.
 *
 * The value to set is the next-to-be-assigned MultiXactId, so this is meant to
 * be called just before doing any such possibly-MultiXactId-able operation.
 */
void
MultiXactIdSetOldestMember(void)
{
	if (!MultiXactIdIsValid(OldestMemberMXactId[MyProcNumber]))
	{
		MultiXactId nextMXact;

		/*
		 * You might think we don't need to acquire a lock here, since
		 * fetching and storing of TransactionIds is probably atomic, but in
		 * fact we do: suppose we pick up nextMXact and then lose the CPU for
		 * a long time.  Someone else could advance nextMXact, and then
		 * another someone else could compute an OldestVisibleMXactId that
		 * would be after the value we are going to store when we get control
		 * back.  Which would be wrong.
		 *
		 * Note that a shared lock is sufficient, because it's enough to stop
		 * someone from advancing nextMXact; and nobody else could be trying
		 * to write to our OldestMember entry, only reading (and we assume
		 * storing it is atomic.)
		 */
		LWLockAcquire(MultiXactGenLock, LW_SHARED);

		/*
		 * We have to beware of the possibility that nextMXact is in the
		 * wrapped-around state.  We don't fix the counter itself here, but we
		 * must be sure to store a valid value in our array entry.
		 */
		nextMXact = MultiXactState->nextMXact;
		if (nextMXact < FirstMultiXactId)
			nextMXact = FirstMultiXactId;

		OldestMemberMXactId[MyProcNumber] = nextMXact;

		LWLockRelease(MultiXactGenLock);

		debug_elog4(DEBUG2, "MultiXact: setting OldestMember[%d] = %u",
					MyProcNumber, nextMXact);
	}
}

/*
 * MultiXactIdSetOldestVisible
 *		Save the oldest MultiXactId this transaction considers possibly live.
 *
 * We set the OldestVisibleMXactId for a given transaction the first time
 * it's going to inspect any MultiXactId.  Once we have set this, we are
 * guaranteed that SLRU data for MultiXactIds >= our own OldestVisibleMXactId
 * won't be truncated away.
 *
 * The value to set is the oldest of nextMXact and all the valid per-backend
 * OldestMemberMXactId[] entries.  Because of the locking we do, we can be
 * certain that no subsequent call to MultiXactIdSetOldestMember can set
 * an OldestMemberMXactId[] entry older than what we compute here.  Therefore
 * there is no live transaction, now or later, that can be a member of any
 * MultiXactId older than the OldestVisibleMXactId we compute here.
 */
static void
MultiXactIdSetOldestVisible(void)
{
	if (!MultiXactIdIsValid(OldestVisibleMXactId[MyProcNumber]))
	{
		MultiXactId oldestMXact;
		int			i;

		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

		/*
		 * We have to beware of the possibility that nextMXact is in the
		 * wrapped-around state.  We don't fix the counter itself here, but we
		 * must be sure to store a valid value in our array entry.
		 */
		oldestMXact = MultiXactState->nextMXact;
		if (oldestMXact < FirstMultiXactId)
			oldestMXact = FirstMultiXactId;

		for (i = 0; i < MaxOldestSlot; i++)
		{
			MultiXactId thisoldest = OldestMemberMXactId[i];

			if (MultiXactIdIsValid(thisoldest) &&
				MultiXactIdPrecedes(thisoldest, oldestMXact))
				oldestMXact = thisoldest;
		}

		OldestVisibleMXactId[MyProcNumber] = oldestMXact;

		LWLockRelease(MultiXactGenLock);

		debug_elog4(DEBUG2, "MultiXact: setting OldestVisible[%d] = %u",
					MyProcNumber, oldestMXact);
	}
}

/*
 * ReadNextMultiXactId
 *		Return the next MultiXactId to be assigned, but don't allocate it
 */
MultiXactId
ReadNextMultiXactId(void)
{
	MultiXactId mxid;

	/* XXX we could presumably do this without a lock. */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	mxid = MultiXactState->nextMXact;
	LWLockRelease(MultiXactGenLock);

	if (mxid < FirstMultiXactId)
		mxid = FirstMultiXactId;

	return mxid;
}

/*
 * ReadMultiXactIdRange
 *		Get the range of IDs that may still be referenced by a relation.
 */
void
ReadMultiXactIdRange(MultiXactId *oldest, MultiXactId *next)
{
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	*oldest = MultiXactState->oldestMultiXactId;
	*next = MultiXactState->nextMXact;
	LWLockRelease(MultiXactGenLock);

	if (*oldest < FirstMultiXactId)
		*oldest = FirstMultiXactId;
	if (*next < FirstMultiXactId)
		*next = FirstMultiXactId;
}


/*
 * MultiXactIdCreateFromMembers
 *		Make a new MultiXactId from the specified set of members
 *
 * Make XLOG, SLRU and cache entries for a new MultiXactId, recording the
 * given TransactionIds as members.  Returns the newly created MultiXactId.
 *
 * NB: the passed members[] array will be sorted in-place.
 */
MultiXactId
MultiXactIdCreateFromMembers(int nmembers, MultiXactMember *members)
{
	MultiXactId multi;
	MultiXactOffset offset;
	xl_multixact_create xlrec;

	debug_elog3(DEBUG2, "Create: %s",
				mxid_to_string(InvalidMultiXactId, nmembers, members));

	/*
	 * See if the same set of members already exists in our cache; if so, just
	 * re-use that MultiXactId.  (Note: it might seem that looking in our
	 * cache is insufficient, and we ought to search disk to see if a
	 * duplicate definition already exists.  But since we only ever create
	 * MultiXacts containing our own XID, in most cases any such MultiXacts
	 * were in fact created by us, and so will be in our cache.  There are
	 * corner cases where someone else added us to a MultiXact without our
	 * knowledge, but it's not worth checking for.)
	 */
	multi = mXactCacheGetBySet(nmembers, members);
	if (MultiXactIdIsValid(multi))
	{
		debug_elog2(DEBUG2, "Create: in cache!");
		return multi;
	}

	/* Verify that there is a single update Xid among the given members. */
	{
		int			i;
		bool		has_update = false;

		for (i = 0; i < nmembers; i++)
		{
			if (ISUPDATE_from_mxstatus(members[i].status))
			{
				if (has_update)
					elog(ERROR, "new multixact has more than one updating member: %s",
						 mxid_to_string(InvalidMultiXactId, nmembers, members));
				has_update = true;
			}
		}
	}

	/* Load the injection point before entering the critical section */
	INJECTION_POINT_LOAD("multixact-create-from-members");

	/*
	 * Assign the MXID and offsets range to use, and make sure there is space
	 * in the OFFSETs and MEMBERs files.  NB: this routine does
	 * START_CRIT_SECTION().
	 *
	 * Note: unlike MultiXactIdCreate and MultiXactIdExpand, we do not check
	 * that we've called MultiXactIdSetOldestMember here.  This is because
	 * this routine is used in some places to create new MultiXactIds of which
	 * the current backend is not a member, notably during freezing of multis
	 * in vacuum.  During vacuum, in particular, it would be unacceptable to
	 * keep OldestMulti set, in case it runs for long.
	 */
	multi = GetNewMultiXactId(nmembers, &offset);

	INJECTION_POINT_CACHED("multixact-create-from-members", NULL);

	/* Make an XLOG entry describing the new MXID. */
	xlrec.mid = multi;
	xlrec.moff = offset;
	xlrec.nmembers = nmembers;

	/*
	 * XXX Note: there's a lot of padding space in MultiXactMember.  We could
	 * find a more compact representation of this Xlog record -- perhaps all
	 * the status flags in one XLogRecData, then all the xids in another one?
	 * Not clear that it's worth the trouble though.
	 */
	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfMultiXactCreate);
	XLogRegisterData(members, nmembers * sizeof(MultiXactMember));

	(void) XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_CREATE_ID);

	/* Now enter the information into the OFFSETs and MEMBERs logs */
	RecordNewMultiXact(multi, offset, nmembers, members);

	/* Done with critical section */
	END_CRIT_SECTION();

	/* Store the new MultiXactId in the local cache, too */
	mXactCachePut(multi, nmembers, members);

	debug_elog2(DEBUG2, "Create: all done");

	return multi;
}

/*
 * RecordNewMultiXact
 *		Write info about a new multixact into the offsets and members files
 *
 * This is broken out of MultiXactIdCreateFromMembers so that xlog replay can
 * use it.
 */
static void
RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
				   int nmembers, MultiXactMember *members)
{
	int64		pageno;
	int64		prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	MultiXactId next;
	int64		next_pageno;
	int			next_entryno;
	MultiXactOffset *next_offptr;
	MultiXactOffset next_offset;
	LWLock	   *lock;
	LWLock	   *prevlock = NULL;

	/* position of this multixid in the offsets SLRU area  */
	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/* position of the next multixid */
	next = multi + 1;
	if (next < FirstMultiXactId)
		next = FirstMultiXactId;
	next_pageno = MultiXactIdToOffsetPage(next);
	next_entryno = MultiXactIdToOffsetEntry(next);

	/*
	 * Set the starting offset of this multixid's members.
	 *
	 * In the common case, it was already be set by the previous
	 * RecordNewMultiXact call, as this was the next multixid of the previous
	 * multixid.  But if multiple backends are generating multixids
	 * concurrently, we might race ahead and get called before the previous
	 * multixid.
	 */
	lock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);
	LWLockAcquire(lock, LW_EXCLUSIVE);

	/*
	 * Note: we pass the MultiXactId to SimpleLruReadPage as the "transaction"
	 * to complain about if there's any I/O error.  This is kinda bogus, but
	 * since the errors will always give the full pathname, it should be clear
	 * enough that a MultiXactId is really involved.  Perhaps someday we'll
	 * take the trouble to generalize the slru.c error reporting code.
	 */
	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;

	if (*offptr != offset)
	{
		/* should already be set to the correct value, or not at all */
		Assert(*offptr == 0);
		*offptr = offset;
		MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
	}

	/*
	 * Set the next multixid's offset to the end of this multixid's members.
	 */
	if (next_pageno == pageno)
	{
		next_offptr = offptr + 1;
	}
	else
	{
		/* must be the first entry on the page */
		Assert(next_entryno == 0 || next == FirstMultiXactId);

		/* Swap the lock for a lock on the next page */
		LWLockRelease(lock);
		lock = SimpleLruGetBankLock(MultiXactOffsetCtl, next_pageno);
		LWLockAcquire(lock, LW_EXCLUSIVE);

		slotno = SimpleLruReadPage(MultiXactOffsetCtl, next_pageno, true, next);
		next_offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		next_offptr += next_entryno;
	}

	/* Like in GetNewMultiXactId(), skip over offset 0 */
	next_offset = offset + nmembers;
	if (next_offset == 0)
		next_offset = 1;
	if (*next_offptr != next_offset)
	{
		/* should already be set to the correct value, or not at all */
		Assert(*next_offptr == 0);
		*next_offptr = next_offset;
		MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
	}

	/* Release MultiXactOffset SLRU lock. */
	LWLockRelease(lock);

	prev_pageno = -1;

	for (int i = 0; i < nmembers; i++, offset++)
	{
		TransactionId *memberptr;
		uint32	   *flagsptr;
		uint32		flagsval;
		int			bshift;
		int			flagsoff;
		int			memberoff;

		Assert(members[i].status <= MultiXactStatusUpdate);

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);
		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);

		if (pageno != prev_pageno)
		{
			/*
			 * MultiXactMember SLRU page is changed so check if this new page
			 * fall into the different SLRU bank then release the old bank's
			 * lock and acquire lock on the new bank.
			 */
			lock = SimpleLruGetBankLock(MultiXactMemberCtl, pageno);
			if (lock != prevlock)
			{
				if (prevlock != NULL)
					LWLockRelease(prevlock);

				LWLockAcquire(lock, LW_EXCLUSIVE);
				prevlock = lock;
			}
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		memberptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		*memberptr = members[i].xid;

		flagsptr = (uint32 *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		flagsval = *flagsptr;
		flagsval &= ~(((1 << MXACT_MEMBER_BITS_PER_XACT) - 1) << bshift);
		flagsval |= (members[i].status << bshift);
		*flagsptr = flagsval;

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}

	if (prevlock != NULL)
		LWLockRelease(prevlock);
}

/*
 * GetNewMultiXactId
 *		Get the next MultiXactId.
 *
 * Also, reserve the needed amount of space in the "members" area.  The
 * starting offset of the reserved space is returned in *offset.
 *
 * This may generate XLOG records for expansion of the offsets and/or members
 * files.  Unfortunately, we have to do that while holding MultiXactGenLock
 * to avoid race conditions --- the XLOG record for zeroing a page must appear
 * before any backend can possibly try to store data in that page!
 *
 * We start a critical section before advancing the shared counters.  The
 * caller must end the critical section after writing SLRU data.
 */
static MultiXactId
GetNewMultiXactId(int nmembers, MultiXactOffset *offset)
{
	MultiXactId result;
	MultiXactOffset nextOffset;

	debug_elog3(DEBUG2, "GetNew: for %d xids", nmembers);

	/* safety check, we should never get this far in a HS standby */
	if (RecoveryInProgress())
		elog(ERROR, "cannot assign MultiXactIds during recovery");

	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

	/* Handle wraparound of the nextMXact counter */
	if (MultiXactState->nextMXact < FirstMultiXactId)
		MultiXactState->nextMXact = FirstMultiXactId;

	/* Assign the MXID */
	result = MultiXactState->nextMXact;

	/*----------
	 * Check to see if it's safe to assign another MultiXactId.  This protects
	 * against catastrophic data loss due to multixact wraparound.  The basic
	 * rules are:
	 *
	 * If we're past multiVacLimit or the safe threshold for member storage
	 * space, or we don't know what the safe threshold for member storage is,
	 * start trying to force autovacuum cycles.
	 * If we're past multiWarnLimit, start issuing warnings.
	 * If we're past multiStopLimit, refuse to create new MultiXactIds.
	 *
	 * Note these are pretty much the same protections in GetNewTransactionId.
	 *----------
	 */
	if (!MultiXactIdPrecedes(result, MultiXactState->multiVacLimit))
	{
		/*
		 * For safety's sake, we release MultiXactGenLock while sending
		 * signals, warnings, etc.  This is not so much because we care about
		 * preserving concurrency in this situation, as to avoid any
		 * possibility of deadlock while doing get_database_name(). First,
		 * copy all the shared values we'll need in this path.
		 */
		MultiXactId multiWarnLimit = MultiXactState->multiWarnLimit;
		MultiXactId multiStopLimit = MultiXactState->multiStopLimit;
		MultiXactId multiWrapLimit = MultiXactState->multiWrapLimit;
		Oid			oldest_datoid = MultiXactState->oldestMultiXactDB;

		LWLockRelease(MultiXactGenLock);

		if (IsUnderPostmaster &&
			!MultiXactIdPrecedes(result, multiStopLimit))
		{
			char	   *oldest_datname = get_database_name(oldest_datoid);

			/*
			 * Immediately kick autovacuum into action as we're already in
			 * ERROR territory.
			 */
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

			/* complain even if that DB has disappeared */
			if (oldest_datname)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands that assign new MultiXactIds to avoid wraparound data loss in database \"%s\"",
								oldest_datname),
						 errhint("Execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands that assign new MultiXactIds to avoid wraparound data loss in database with OID %u",
								oldest_datoid),
						 errhint("Execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		}

		/*
		 * To avoid swamping the postmaster with signals, we issue the autovac
		 * request only once per 64K multis generated.  This still gives
		 * plenty of chances before we get into real trouble.
		 */
		if (IsUnderPostmaster && (result % 65536) == 0)
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

		if (!MultiXactIdPrecedes(result, multiWarnLimit))
		{
			char	   *oldest_datname = get_database_name(oldest_datoid);

			/* complain even if that DB has disappeared */
			if (oldest_datname)
				ereport(WARNING,
						(errmsg_plural("database \"%s\" must be vacuumed before %u more MultiXactId is used",
									   "database \"%s\" must be vacuumed before %u more MultiXactIds are used",
									   multiWrapLimit - result,
									   oldest_datname,
									   multiWrapLimit - result),
						 errhint("Execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
			else
				ereport(WARNING,
						(errmsg_plural("database with OID %u must be vacuumed before %u more MultiXactId is used",
									   "database with OID %u must be vacuumed before %u more MultiXactIds are used",
									   multiWrapLimit - result,
									   oldest_datoid,
									   multiWrapLimit - result),
						 errhint("Execute a database-wide VACUUM in that database.\n"
								 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		}

		/* Re-acquire lock and start over */
		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
		result = MultiXactState->nextMXact;
		if (result < FirstMultiXactId)
			result = FirstMultiXactId;
	}

	/*
	 * Make sure there is room for the next MXID in the file.  Assigning this
	 * MXID sets the next MXID's offset already.
	 */
	ExtendMultiXactOffset(result + 1);

	/*
	 * Reserve the members space, similarly to above.
	 */
	nextOffset = MultiXactState->nextOffset;

	/*
	 * Offsets are 64-bit integers and will never wrap around.  Firstly, it
	 * would take an unrealistic amount of time and resources to consume 2^64
	 * offsets.  Secondly, multixid creation is WAL-logged, so you would run
	 * out of LSNs before reaching offset wraparound.  Nevertheless, check for
	 * wraparound as a sanity check.
	 */
	if (nextOffset + nmembers < nextOffset)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("MultiXact members would wrap around")));
	*offset = nextOffset;

	ExtendMultiXactMember(nextOffset, nmembers);

	/*
	 * Critical section from here until caller has written the data into the
	 * just-reserved SLRU space; we don't want to error out with a partly
	 * written MultiXact structure.  (In particular, failing to write our
	 * start offset after advancing nextMXact would effectively corrupt the
	 * previous MultiXact.)
	 */
	START_CRIT_SECTION();

	/*
	 * Advance counters.  As in GetNewTransactionId(), this must not happen
	 * until after file extension has succeeded!
	 *
	 * We don't care about MultiXactId wraparound here; it will be handled by
	 * the next iteration.  But note that nextMXact may be InvalidMultiXactId
	 * or the first value on a segment-beginning page after this routine
	 * exits, so anyone else looking at the variable must be prepared to deal
	 * with either case.
	 */
	(MultiXactState->nextMXact)++;

	MultiXactState->nextOffset += nmembers;

	LWLockRelease(MultiXactGenLock);

	debug_elog4(DEBUG2, "GetNew: returning %u offset %" PRIu64,
				result, *offset);
	return result;
}

/*
 * GetMultiXactIdMembers
 *		Return the set of MultiXactMembers that make up a MultiXactId
 *
 * Return value is the number of members found, or -1 if there are none,
 * and *members is set to a newly palloc'ed array of members.  It's the
 * caller's responsibility to free it when done with it.
 *
 * from_pgupgrade must be passed as true if and only if only the multixact
 * corresponds to a value from a tuple that was locked in a 9.2-or-older
 * installation and later pg_upgrade'd (that is, the infomask is
 * HEAP_LOCKED_UPGRADED).  In this case, we know for certain that no members
 * can still be running, so we return -1 just like for an empty multixact
 * without any further checking.  It would be wrong to try to resolve such a
 * multixact: either the multixact is within the current valid multixact
 * range, in which case the returned result would be bogus, or outside that
 * range, in which case an error would be raised.
 *
 * In all other cases, the passed multixact must be within the known valid
 * range, that is, greater than or equal to oldestMultiXactId, and less than
 * nextMXact.  Otherwise, an error is raised.
 *
 * isLockOnly must be set to true if caller is certain that the given multi
 * is used only to lock tuples; can be false without loss of correctness,
 * but passing a true means we can return quickly without checking for
 * old updates.
 */
int
GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **members,
					  bool from_pgupgrade, bool isLockOnly)
{
	int64		pageno;
	int64		prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	MultiXactOffset offset;
	int			length;
	MultiXactId oldestMXact;
	MultiXactId nextMXact;
	MultiXactMember *ptr;
	LWLock	   *lock;

	debug_elog3(DEBUG2, "GetMembers: asked for %u", multi);

	if (!MultiXactIdIsValid(multi) || from_pgupgrade)
	{
		*members = NULL;
		return -1;
	}

	/* See if the MultiXactId is in the local cache */
	length = mXactCacheGetById(multi, members);
	if (length >= 0)
	{
		debug_elog3(DEBUG2, "GetMembers: found %s in the cache",
					mxid_to_string(multi, length, *members));
		return length;
	}

	/* Set our OldestVisibleMXactId[] entry if we didn't already */
	MultiXactIdSetOldestVisible();

	/*
	 * If we know the multi is used only for locking and not for updates, then
	 * we can skip checking if the value is older than our oldest visible
	 * multi.  It cannot possibly still be running.
	 */
	if (isLockOnly &&
		MultiXactIdPrecedes(multi, OldestVisibleMXactId[MyProcNumber]))
	{
		debug_elog2(DEBUG2, "GetMembers: a locker-only multi is too old");
		*members = NULL;
		return -1;
	}

	/*
	 * We check known limits on MultiXact before resorting to the SLRU area.
	 *
	 * An ID older than MultiXactState->oldestMultiXactId cannot possibly be
	 * useful; it has already been removed, or will be removed shortly, by
	 * truncation.  If one is passed, an error is raised.
	 *
	 * Also, an ID >= nextMXact shouldn't ever be seen here; if it is seen, it
	 * implies undetected ID wraparound has occurred.  This raises a hard
	 * error.
	 *
	 * Shared lock is enough here since we aren't modifying any global state.
	 * Acquire it just long enough to grab the current counter values.
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	oldestMXact = MultiXactState->oldestMultiXactId;
	nextMXact = MultiXactState->nextMXact;

	LWLockRelease(MultiXactGenLock);

	if (MultiXactIdPrecedes(multi, oldestMXact))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("MultiXactId %u does no longer exist -- apparent wraparound",
						multi)));

	if (!MultiXactIdPrecedes(multi, nextMXact))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("MultiXactId %u has not been created yet -- apparent wraparound",
						multi)));

	/*
	 * Find out the offset at which we need to start reading MultiXactMembers
	 * and the number of members in the multixact.  We determine the latter as
	 * the difference between this multixact's starting offset and the next
	 * one's.
	 */
	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/* Acquire the bank lock for the page we need. */
	lock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);
	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* read this multi's offset */
	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;
	offset = *offptr;

	Assert(offset != 0);

	/* read next multi's offset */
	{
		MultiXactId tmpMXact;
		MultiXactOffset nextMXOffset;

		/* handle wraparound if needed */
		tmpMXact = multi + 1;
		if (tmpMXact < FirstMultiXactId)
			tmpMXact = FirstMultiXactId;

		prev_pageno = pageno;

		pageno = MultiXactIdToOffsetPage(tmpMXact);
		entryno = MultiXactIdToOffsetEntry(tmpMXact);

		if (pageno != prev_pageno)
		{
			LWLock	   *newlock;

			/*
			 * Since we're going to access a different SLRU page, if this page
			 * falls under a different bank, release the old bank's lock and
			 * acquire the lock of the new bank.
			 */
			newlock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);
			if (newlock != lock)
			{
				LWLockRelease(lock);
				LWLockAcquire(newlock, LW_EXCLUSIVE);
				lock = newlock;
			}
			slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, tmpMXact);
		}

		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;
		nextMXOffset = *offptr;

		if (nextMXOffset == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("MultiXact %u has invalid next offset",
							multi)));

		length = nextMXOffset - offset;
	}

	LWLockRelease(lock);
	lock = NULL;

	/* A multixid with zero members should not happen */
	Assert(length > 0);

	/* read the members */
	ptr = (MultiXactMember *) palloc(length * sizeof(MultiXactMember));
	prev_pageno = -1;
	for (int i = 0; i < length; i++, offset++)
	{
		TransactionId *xactptr;
		uint32	   *flagsptr;
		int			flagsoff;
		int			bshift;
		int			memberoff;

		pageno = MXOffsetToMemberPage(offset);
		memberoff = MXOffsetToMemberOffset(offset);

		if (pageno != prev_pageno)
		{
			LWLock	   *newlock;

			/*
			 * Since we're going to access a different SLRU page, if this page
			 * falls under a different bank, release the old bank's lock and
			 * acquire the lock of the new bank.
			 */
			newlock = SimpleLruGetBankLock(MultiXactMemberCtl, pageno);
			if (newlock != lock)
			{
				if (lock)
					LWLockRelease(lock);
				LWLockAcquire(newlock, LW_EXCLUSIVE);
				lock = newlock;
			}

			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		xactptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);
		Assert(TransactionIdIsValid(*xactptr));

		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);
		flagsptr = (uint32 *) (MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		ptr[i].xid = *xactptr;
		ptr[i].status = (*flagsptr >> bshift) & MXACT_MEMBER_XACT_BITMASK;
	}

	LWLockRelease(lock);

	/*
	 * Copy the result into the local cache.
	 */
	mXactCachePut(multi, length, ptr);

	debug_elog3(DEBUG2, "GetMembers: no cache for %s",
				mxid_to_string(multi, length, ptr));
	*members = ptr;
	return length;
}

/*
 * mxactMemberComparator
 *		qsort comparison function for MultiXactMember
 *
 * We can't use wraparound comparison for XIDs because that does not respect
 * the triangle inequality!  Any old sort order will do.
 */
static int
mxactMemberComparator(const void *arg1, const void *arg2)
{
	MultiXactMember member1 = *(const MultiXactMember *) arg1;
	MultiXactMember member2 = *(const MultiXactMember *) arg2;

	if (member1.xid > member2.xid)
		return 1;
	if (member1.xid < member2.xid)
		return -1;
	if (member1.status > member2.status)
		return 1;
	if (member1.status < member2.status)
		return -1;
	return 0;
}

/*
 * mXactCacheGetBySet
 *		returns a MultiXactId from the cache based on the set of
 *		TransactionIds that compose it, or InvalidMultiXactId if
 *		none matches.
 *
 * This is helpful, for example, if two transactions want to lock a huge
 * table.  By using the cache, the second will use the same MultiXactId
 * for the majority of tuples, thus keeping MultiXactId usage low (saving
 * both I/O and wraparound issues).
 *
 * NB: the passed members array will be sorted in-place.
 */
static MultiXactId
mXactCacheGetBySet(int nmembers, MultiXactMember *members)
{
	dlist_iter	iter;

	debug_elog3(DEBUG2, "CacheGet: looking for %s",
				mxid_to_string(InvalidMultiXactId, nmembers, members));

	/* sort the array so comparison is easy */
	qsort(members, nmembers, sizeof(MultiXactMember), mxactMemberComparator);

	dclist_foreach(iter, &MXactCache)
	{
		mXactCacheEnt *entry = dclist_container(mXactCacheEnt, node,
												iter.cur);

		if (entry->nmembers != nmembers)
			continue;

		/*
		 * We assume the cache entries are sorted, and that the unused bits in
		 * "status" are zeroed.
		 */
		if (memcmp(members, entry->members, nmembers * sizeof(MultiXactMember)) == 0)
		{
			debug_elog3(DEBUG2, "CacheGet: found %u", entry->multi);
			dclist_move_head(&MXactCache, iter.cur);
			return entry->multi;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found :-(");
	return InvalidMultiXactId;
}

/*
 * mXactCacheGetById
 *		returns the composing MultiXactMember set from the cache for a
 *		given MultiXactId, if present.
 *
 * If successful, *xids is set to the address of a palloc'd copy of the
 * MultiXactMember set.  Return value is number of members, or -1 on failure.
 */
static int
mXactCacheGetById(MultiXactId multi, MultiXactMember **members)
{
	dlist_iter	iter;

	debug_elog3(DEBUG2, "CacheGet: looking for %u", multi);

	dclist_foreach(iter, &MXactCache)
	{
		mXactCacheEnt *entry = dclist_container(mXactCacheEnt, node,
												iter.cur);

		if (entry->multi == multi)
		{
			MultiXactMember *ptr;
			Size		size;

			size = sizeof(MultiXactMember) * entry->nmembers;
			ptr = (MultiXactMember *) palloc(size);

			memcpy(ptr, entry->members, size);

			debug_elog3(DEBUG2, "CacheGet: found %s",
						mxid_to_string(multi,
									   entry->nmembers,
									   entry->members));

			/*
			 * Note we modify the list while not using a modifiable iterator.
			 * This is acceptable only because we exit the iteration
			 * immediately afterwards.
			 */
			dclist_move_head(&MXactCache, iter.cur);

			*members = ptr;
			return entry->nmembers;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found");
	return -1;
}

/*
 * mXactCachePut
 *		Add a new MultiXactId and its composing set into the local cache.
 */
static void
mXactCachePut(MultiXactId multi, int nmembers, MultiXactMember *members)
{
	mXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CachePut: storing %s",
				mxid_to_string(multi, nmembers, members));

	if (MXactContext == NULL)
	{
		/* The cache only lives as long as the current transaction */
		debug_elog2(DEBUG2, "CachePut: initializing memory context");
		MXactContext = AllocSetContextCreate(TopTransactionContext,
											 "MultiXact cache context",
											 ALLOCSET_SMALL_SIZES);
	}

	entry = (mXactCacheEnt *)
		MemoryContextAlloc(MXactContext,
						   offsetof(mXactCacheEnt, members) +
						   nmembers * sizeof(MultiXactMember));

	entry->multi = multi;
	entry->nmembers = nmembers;
	memcpy(entry->members, members, nmembers * sizeof(MultiXactMember));

	/* mXactCacheGetBySet assumes the entries are sorted, so sort them */
	qsort(entry->members, nmembers, sizeof(MultiXactMember), mxactMemberComparator);

	dclist_push_head(&MXactCache, &entry->node);
	if (dclist_count(&MXactCache) > MAX_CACHE_ENTRIES)
	{
		dlist_node *node;

		node = dclist_tail_node(&MXactCache);
		dclist_delete_from(&MXactCache, node);

		entry = dclist_container(mXactCacheEnt, node, node);
		debug_elog3(DEBUG2, "CachePut: pruning cached multi %u",
					entry->multi);

		pfree(entry);
	}
}

char *
mxstatus_to_string(MultiXactStatus status)
{
	switch (status)
	{
		case MultiXactStatusForKeyShare:
			return "keysh";
		case MultiXactStatusForShare:
			return "sh";
		case MultiXactStatusForNoKeyUpdate:
			return "fornokeyupd";
		case MultiXactStatusForUpdate:
			return "forupd";
		case MultiXactStatusNoKeyUpdate:
			return "nokeyupd";
		case MultiXactStatusUpdate:
			return "upd";
		default:
			elog(ERROR, "unrecognized multixact status %d", status);
			return "";
	}
}

char *
mxid_to_string(MultiXactId multi, int nmembers, MultiXactMember *members)
{
	static char *str = NULL;
	StringInfoData buf;
	int			i;

	if (str != NULL)
		pfree(str);

	initStringInfo(&buf);

	appendStringInfo(&buf, "%u %d[%u (%s)", multi, nmembers, members[0].xid,
					 mxstatus_to_string(members[0].status));

	for (i = 1; i < nmembers; i++)
		appendStringInfo(&buf, ", %u (%s)", members[i].xid,
						 mxstatus_to_string(members[i].status));

	appendStringInfoChar(&buf, ']');
	str = MemoryContextStrdup(TopMemoryContext, buf.data);
	pfree(buf.data);
	return str;
}

/*
 * AtEOXact_MultiXact
 *		Handle transaction end for MultiXact
 *
 * This is called at top transaction commit or abort (we don't care which).
 */
void
AtEOXact_MultiXact(void)
{
	/*
	 * Reset our OldestMemberMXactId and OldestVisibleMXactId values, both of
	 * which should only be valid while within a transaction.
	 *
	 * We assume that storing a MultiXactId is atomic and so we need not take
	 * MultiXactGenLock to do this.
	 */
	OldestMemberMXactId[MyProcNumber] = InvalidMultiXactId;
	OldestVisibleMXactId[MyProcNumber] = InvalidMultiXactId;

	/*
	 * Discard the local MultiXactId cache.  Since MXactContext was created as
	 * a child of TopTransactionContext, we needn't delete it explicitly.
	 */
	MXactContext = NULL;
	dclist_init(&MXactCache);
}

/*
 * AtPrepare_MultiXact
 *		Save multixact state at 2PC transaction prepare
 *
 * In this phase, we only store our OldestMemberMXactId value in the two-phase
 * state file.
 */
void
AtPrepare_MultiXact(void)
{
	MultiXactId myOldestMember = OldestMemberMXactId[MyProcNumber];

	if (MultiXactIdIsValid(myOldestMember))
		RegisterTwoPhaseRecord(TWOPHASE_RM_MULTIXACT_ID, 0,
							   &myOldestMember, sizeof(MultiXactId));
}

/*
 * PostPrepare_MultiXact
 *		Clean up after successful PREPARE TRANSACTION
 */
void
PostPrepare_MultiXact(FullTransactionId fxid)
{
	MultiXactId myOldestMember;

	/*
	 * Transfer our OldestMemberMXactId value to the slot reserved for the
	 * prepared transaction.
	 */
	myOldestMember = OldestMemberMXactId[MyProcNumber];
	if (MultiXactIdIsValid(myOldestMember))
	{
		ProcNumber	dummyProcNumber = TwoPhaseGetDummyProcNumber(fxid, false);

		/*
		 * Even though storing MultiXactId is atomic, acquire lock to make
		 * sure others see both changes, not just the reset of the slot of the
		 * current backend. Using a volatile pointer might suffice, but this
		 * isn't a hot spot.
		 */
		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

		OldestMemberMXactId[dummyProcNumber] = myOldestMember;
		OldestMemberMXactId[MyProcNumber] = InvalidMultiXactId;

		LWLockRelease(MultiXactGenLock);
	}

	/*
	 * We don't need to transfer OldestVisibleMXactId value, because the
	 * transaction is not going to be looking at any more multixacts once it's
	 * prepared.
	 *
	 * We assume that storing a MultiXactId is atomic and so we need not take
	 * MultiXactGenLock to do this.
	 */
	OldestVisibleMXactId[MyProcNumber] = InvalidMultiXactId;

	/*
	 * Discard the local MultiXactId cache like in AtEOXact_MultiXact.
	 */
	MXactContext = NULL;
	dclist_init(&MXactCache);
}

/*
 * multixact_twophase_recover
 *		Recover the state of a prepared transaction at startup
 */
void
multixact_twophase_recover(FullTransactionId fxid, uint16 info,
						   void *recdata, uint32 len)
{
	ProcNumber	dummyProcNumber = TwoPhaseGetDummyProcNumber(fxid, false);
	MultiXactId oldestMember;

	/*
	 * Get the oldest member XID from the state file record, and set it in the
	 * OldestMemberMXactId slot reserved for this prepared transaction.
	 */
	Assert(len == sizeof(MultiXactId));
	oldestMember = *((MultiXactId *) recdata);

	OldestMemberMXactId[dummyProcNumber] = oldestMember;
}

/*
 * multixact_twophase_postcommit
 *		Similar to AtEOXact_MultiXact but for COMMIT PREPARED
 */
void
multixact_twophase_postcommit(FullTransactionId fxid, uint16 info,
							  void *recdata, uint32 len)
{
	ProcNumber	dummyProcNumber = TwoPhaseGetDummyProcNumber(fxid, true);

	Assert(len == sizeof(MultiXactId));

	OldestMemberMXactId[dummyProcNumber] = InvalidMultiXactId;
}

/*
 * multixact_twophase_postabort
 *		This is actually just the same as the COMMIT case.
 */
void
multixact_twophase_postabort(FullTransactionId fxid, uint16 info,
							 void *recdata, uint32 len)
{
	multixact_twophase_postcommit(fxid, info, recdata, len);
}

/*
 * Initialization of shared memory for MultiXact.  We use two SLRU areas,
 * thus double memory.  Also, reserve space for the shared MultiXactState
 * struct and the per-backend MultiXactId arrays (two of those, too).
 */
Size
MultiXactShmemSize(void)
{
	Size		size;

	/* We need 2*MaxOldestSlot perBackendXactIds[] entries */
#define SHARED_MULTIXACT_STATE_SIZE \
	add_size(offsetof(MultiXactStateData, perBackendXactIds), \
			 mul_size(sizeof(MultiXactId) * 2, MaxOldestSlot))

	size = SHARED_MULTIXACT_STATE_SIZE;
	size = add_size(size, SimpleLruShmemSize(multixact_offset_buffers, 0));
	size = add_size(size, SimpleLruShmemSize(multixact_member_buffers, 0));

	return size;
}

void
MultiXactShmemInit(void)
{
	bool		found;

	debug_elog2(DEBUG2, "Shared Memory Init for MultiXact");

	MultiXactOffsetCtl->PagePrecedes = MultiXactOffsetPagePrecedes;
	MultiXactMemberCtl->PagePrecedes = MultiXactMemberPagePrecedes;

	SimpleLruInit(MultiXactOffsetCtl,
				  "multixact_offset", multixact_offset_buffers, 0,
				  "pg_multixact/offsets", LWTRANCHE_MULTIXACTOFFSET_BUFFER,
				  LWTRANCHE_MULTIXACTOFFSET_SLRU,
				  SYNC_HANDLER_MULTIXACT_OFFSET,
				  false);
	SlruPagePrecedesUnitTests(MultiXactOffsetCtl, MULTIXACT_OFFSETS_PER_PAGE);
	SimpleLruInit(MultiXactMemberCtl,
				  "multixact_member", multixact_member_buffers, 0,
				  "pg_multixact/members", LWTRANCHE_MULTIXACTMEMBER_BUFFER,
				  LWTRANCHE_MULTIXACTMEMBER_SLRU,
				  SYNC_HANDLER_MULTIXACT_MEMBER,
				  true);
	/* doesn't call SimpleLruTruncate() or meet criteria for unit tests */

	/* Initialize our shared state struct */
	MultiXactState = ShmemInitStruct("Shared MultiXact State",
									 SHARED_MULTIXACT_STATE_SIZE,
									 &found);
	if (!IsUnderPostmaster)
	{
		Assert(!found);

		/* Make sure we zero out the per-backend state */
		MemSet(MultiXactState, 0, SHARED_MULTIXACT_STATE_SIZE);
	}
	else
		Assert(found);

	/*
	 * Set up array pointers.
	 */
	OldestMemberMXactId = MultiXactState->perBackendXactIds;
	OldestVisibleMXactId = OldestMemberMXactId + MaxOldestSlot;
}

/*
 * GUC check_hook for multixact_offset_buffers
 */
bool
check_multixact_offset_buffers(int *newval, void **extra, GucSource source)
{
	return check_slru_buffers("multixact_offset_buffers", newval);
}

/*
 * GUC check_hook for multixact_member_buffers
 */
bool
check_multixact_member_buffers(int *newval, void **extra, GucSource source)
{
	return check_slru_buffers("multixact_member_buffers", newval);
}

/*
 * This func must be called ONCE on system install.  It creates the initial
 * MultiXact segments.  (The MultiXacts directories are assumed to have been
 * created by initdb, and MultiXactShmemInit must have been called already.)
 */
void
BootStrapMultiXact(void)
{
	/* Zero the initial pages and flush them to disk */
	SimpleLruZeroAndWritePage(MultiXactOffsetCtl, 0);
	SimpleLruZeroAndWritePage(MultiXactMemberCtl, 0);
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup.
 *
 * StartupXLOG has already established nextMXact/nextOffset by calling
 * MultiXactSetNextMXact and/or MultiXactAdvanceNextMXact, and the oldestMulti
 * info from pg_control and/or MultiXactAdvanceOldest, but we haven't yet
 * replayed WAL.
 */
void
StartupMultiXact(void)
{
	MultiXactId multi = MultiXactState->nextMXact;
	MultiXactOffset offset = MultiXactState->nextOffset;
	int64		pageno;

	/*
	 * Initialize offset's idea of the latest page number.
	 */
	pageno = MultiXactIdToOffsetPage(multi);
	pg_atomic_write_u64(&MultiXactOffsetCtl->shared->latest_page_number,
						pageno);

	/*
	 * Initialize member's idea of the latest page number.
	 */
	pageno = MXOffsetToMemberPage(offset);
	pg_atomic_write_u64(&MultiXactMemberCtl->shared->latest_page_number,
						pageno);
}

/*
 * This must be called ONCE at the end of startup/recovery.
 */
void
TrimMultiXact(void)
{
	MultiXactId nextMXact;
	MultiXactOffset offset;
	MultiXactId oldestMXact;
	Oid			oldestMXactDB;
	int64		pageno;
	int			entryno;
	int			flagsoff;

	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	nextMXact = MultiXactState->nextMXact;
	offset = MultiXactState->nextOffset;
	oldestMXact = MultiXactState->oldestMultiXactId;
	oldestMXactDB = MultiXactState->oldestMultiXactDB;
	LWLockRelease(MultiXactGenLock);

	/* Clean up offsets state */

	/*
	 * (Re-)Initialize our idea of the latest page number for offsets.
	 */
	pageno = MultiXactIdToOffsetPage(nextMXact);
	pg_atomic_write_u64(&MultiXactOffsetCtl->shared->latest_page_number,
						pageno);

	/*
	 * Set the offset of nextMXact on the offsets page.  This is normally done
	 * in RecordNewMultiXact() of the previous multixact, but let's be sure
	 * the next page exists, if the nextMXact was reset with pg_resetwal for
	 * example.
	 *
	 * Zero out the remainder of the page.  See notes in TrimCLOG() for
	 * background.  Unlike CLOG, some WAL record covers every pg_multixact
	 * SLRU mutation.  Since, also unlike CLOG, we ignore the WAL rule "write
	 * xlog before data," nextMXact successors may carry obsolete, nonzero
	 * offset values.
	 */
	entryno = MultiXactIdToOffsetEntry(nextMXact);
	{
		int			slotno;
		MultiXactOffset *offptr;
		LWLock	   *lock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);

		LWLockAcquire(lock, LW_EXCLUSIVE);
		if (entryno == 0)
			slotno = SimpleLruZeroPage(MultiXactOffsetCtl, pageno);
		else
			slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, nextMXact);
		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;

		*offptr = offset;
		if (entryno != 0 && (entryno + 1) * sizeof(MultiXactOffset) != BLCKSZ)
			MemSet(offptr + 1, 0, BLCKSZ - (entryno + 1) * sizeof(MultiXactOffset));

		MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
		LWLockRelease(lock);
	}

	/*
	 * And the same for members.
	 *
	 * (Re-)Initialize our idea of the latest page number for members.
	 */
	pageno = MXOffsetToMemberPage(offset);
	pg_atomic_write_u64(&MultiXactMemberCtl->shared->latest_page_number,
						pageno);

	/*
	 * Zero out the remainder of the current members page.  See notes in
	 * TrimCLOG() for motivation.
	 */
	flagsoff = MXOffsetToFlagsOffset(offset);
	if (flagsoff != 0)
	{
		int			slotno;
		TransactionId *xidptr;
		int			memberoff;
		LWLock	   *lock = SimpleLruGetBankLock(MultiXactMemberCtl, pageno);

		LWLockAcquire(lock, LW_EXCLUSIVE);
		memberoff = MXOffsetToMemberOffset(offset);
		slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, offset);
		xidptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		MemSet(xidptr, 0, BLCKSZ - memberoff);

		/*
		 * Note: we don't need to zero out the flag bits in the remaining
		 * members of the current group, because they are always reset before
		 * writing.
		 */

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
		LWLockRelease(lock);
	}

	/* signal that we're officially up */
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->finishedStartup = true;
	LWLockRelease(MultiXactGenLock);

	/* Now compute how far away the next multixid wraparound is. */
	SetMultiXactIdLimit(oldestMXact, oldestMXactDB);
}

/*
 * Get the MultiXact data to save in a checkpoint record
 */
void
MultiXactGetCheckptMulti(bool is_shutdown,
						 MultiXactId *nextMulti,
						 MultiXactOffset *nextMultiOffset,
						 MultiXactId *oldestMulti,
						 Oid *oldestMultiDB)
{
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	*nextMulti = MultiXactState->nextMXact;
	*nextMultiOffset = MultiXactState->nextOffset;
	*oldestMulti = MultiXactState->oldestMultiXactId;
	*oldestMultiDB = MultiXactState->oldestMultiXactDB;
	LWLockRelease(MultiXactGenLock);

	debug_elog6(DEBUG2,
				"MultiXact: checkpoint is nextMulti %u, nextOffset %" PRIu64 ", oldestMulti %u in DB %u",
				*nextMulti, *nextMultiOffset, *oldestMulti, *oldestMultiDB);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointMultiXact(void)
{
	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_START(true);

	/*
	 * Write dirty MultiXact pages to disk.  This may result in sync requests
	 * queued for later handling by ProcessSyncRequests(), as part of the
	 * checkpoint.
	 */
	SimpleLruWriteAll(MultiXactOffsetCtl, true);
	SimpleLruWriteAll(MultiXactMemberCtl, true);

	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_DONE(true);
}

/*
 * Set the next-to-be-assigned MultiXactId and offset
 *
 * This is used when we can determine the correct next ID/offset exactly
 * from a checkpoint record.  Although this is only called during bootstrap
 * and XLog replay, we take the lock in case any hot-standby backends are
 * examining the values.
 */
void
MultiXactSetNextMXact(MultiXactId nextMulti,
					  MultiXactOffset nextMultiOffset)
{
	debug_elog4(DEBUG2, "MultiXact: setting next multi to %u offset %" PRIu64,
				nextMulti, nextMultiOffset);
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->nextMXact = nextMulti;
	MultiXactState->nextOffset = nextMultiOffset;
	LWLockRelease(MultiXactGenLock);
}

/*
 * Determine the last safe MultiXactId to allocate given the currently oldest
 * datminmxid (ie, the oldest MultiXactId that might exist in any database
 * of our cluster), and the OID of the (or a) database with that value.
 *
 * This also updates MultiXactState->oldestOffset, by looking up the offset of
 * MultiXactState->oldestMultiXactId.
 */
void
SetMultiXactIdLimit(MultiXactId oldest_datminmxid, Oid oldest_datoid)
{
	MultiXactId multiVacLimit;
	MultiXactId multiWarnLimit;
	MultiXactId multiStopLimit;
	MultiXactId multiWrapLimit;
	MultiXactId curMulti;

	Assert(MultiXactIdIsValid(oldest_datminmxid));

	/*
	 * We pretend that a wrap will happen halfway through the multixact ID
	 * space, but that's not really true, because multixacts wrap differently
	 * from transaction IDs.
	 */
	multiWrapLimit = oldest_datminmxid + (MaxMultiXactId >> 1);
	if (multiWrapLimit < FirstMultiXactId)
		multiWrapLimit += FirstMultiXactId;

	/*
	 * We'll refuse to continue assigning MultiXactIds once we get within 3M
	 * multi of data loss.  See SetTransactionIdLimit.
	 */
	multiStopLimit = multiWrapLimit - 3000000;
	if (multiStopLimit < FirstMultiXactId)
		multiStopLimit -= FirstMultiXactId;

	/*
	 * We'll start complaining loudly when we get within 40M multis of data
	 * loss.  This is kind of arbitrary, but if you let your gas gauge get
	 * down to 2% of full, would you be looking for the next gas station?  We
	 * need to be fairly liberal about this number because there are lots of
	 * scenarios where most transactions are done by automatic clients that
	 * won't pay attention to warnings.  (No, we're not gonna make this
	 * configurable.  If you know enough to configure it, you know enough to
	 * not get in this kind of trouble in the first place.)
	 */
	multiWarnLimit = multiWrapLimit - 40000000;
	if (multiWarnLimit < FirstMultiXactId)
		multiWarnLimit -= FirstMultiXactId;

	/*
	 * We'll start trying to force autovacuums when oldest_datminmxid gets to
	 * be more than autovacuum_multixact_freeze_max_age mxids old.
	 *
	 * Note: autovacuum_multixact_freeze_max_age is a PGC_POSTMASTER parameter
	 * so that we don't have to worry about dealing with on-the-fly changes in
	 * its value.  See SetTransactionIdLimit.
	 */
	multiVacLimit = oldest_datminmxid + autovacuum_multixact_freeze_max_age;
	if (multiVacLimit < FirstMultiXactId)
		multiVacLimit += FirstMultiXactId;

	/* Grab lock for just long enough to set the new limit values */
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->oldestMultiXactId = oldest_datminmxid;
	MultiXactState->oldestMultiXactDB = oldest_datoid;
	MultiXactState->multiVacLimit = multiVacLimit;
	MultiXactState->multiWarnLimit = multiWarnLimit;
	MultiXactState->multiStopLimit = multiStopLimit;
	MultiXactState->multiWrapLimit = multiWrapLimit;
	curMulti = MultiXactState->nextMXact;
	LWLockRelease(MultiXactGenLock);

	/* Log the info */
	ereport(DEBUG1,
			(errmsg_internal("MultiXactId wrap limit is %u, limited by database with OID %u",
							 multiWrapLimit, oldest_datoid)));

	/*
	 * Computing the actual limits is only possible once the data directory is
	 * in a consistent state. There's no need to compute the limits while
	 * still replaying WAL - no decisions about new multis are made even
	 * though multixact creations might be replayed. So we'll only do further
	 * checks after TrimMultiXact() has been called.
	 */
	if (!MultiXactState->finishedStartup)
		return;

	Assert(!InRecovery);

	/*
	 * Offsets are 64-bits wide and never wrap around, so we don't need to
	 * consider them for emergency autovacuum purposes.  But now that we're in
	 * a consistent state, determine MultiXactState->oldestOffset.  It will be
	 * used to adjust the freezing cutoff, to keep the offsets disk usage in
	 * check.
	 */
	SetOldestOffset();

	/*
	 * If past the autovacuum force point, immediately signal an autovac
	 * request.  The reason for this is that autovac only processes one
	 * database per invocation.  Once it's finished cleaning up the oldest
	 * database, it'll call here, and we'll signal the postmaster to start
	 * another iteration immediately if there are still any old databases.
	 */
	if (MultiXactIdPrecedes(multiVacLimit, curMulti) && IsUnderPostmaster)
		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

	/* Give an immediate warning if past the wrap warn point */
	if (MultiXactIdPrecedes(multiWarnLimit, curMulti))
	{
		char	   *oldest_datname;

		/*
		 * We can be called when not inside a transaction, for example during
		 * StartupXLOG().  In such a case we cannot do database access, so we
		 * must just report the oldest DB's OID.
		 *
		 * Note: it's also possible that get_database_name fails and returns
		 * NULL, for example because the database just got dropped.  We'll
		 * still warn, even though the warning might now be unnecessary.
		 */
		if (IsTransactionState())
			oldest_datname = get_database_name(oldest_datoid);
		else
			oldest_datname = NULL;

		if (oldest_datname)
			ereport(WARNING,
					(errmsg_plural("database \"%s\" must be vacuumed before %u more MultiXactId is used",
								   "database \"%s\" must be vacuumed before %u more MultiXactIds are used",
								   multiWrapLimit - curMulti,
								   oldest_datname,
								   multiWrapLimit - curMulti),
					 errhint("To avoid MultiXactId assignment failures, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
		else
			ereport(WARNING,
					(errmsg_plural("database with OID %u must be vacuumed before %u more MultiXactId is used",
								   "database with OID %u must be vacuumed before %u more MultiXactIds are used",
								   multiWrapLimit - curMulti,
								   oldest_datoid,
								   multiWrapLimit - curMulti),
					 errhint("To avoid MultiXactId assignment failures, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions, or drop stale replication slots.")));
	}
}

/*
 * Ensure the next-to-be-assigned MultiXactId is at least minMulti,
 * and similarly nextOffset is at least minMultiOffset.
 *
 * This is used when we can determine minimum safe values from an XLog
 * record (either an on-line checkpoint or an mxact creation log entry).
 * Although this is only called during XLog replay, we take the lock in case
 * any hot-standby backends are examining the values.
 */
void
MultiXactAdvanceNextMXact(MultiXactId minMulti,
						  MultiXactOffset minMultiOffset)
{
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	if (MultiXactIdPrecedes(MultiXactState->nextMXact, minMulti))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next multi to %u", minMulti);
		MultiXactState->nextMXact = minMulti;
	}
	if (MultiXactState->nextOffset < minMultiOffset)
	{
		debug_elog3(DEBUG2, "MultiXact: setting next offset to %" PRIU64,
					minMultiOffset);
		MultiXactState->nextOffset = minMultiOffset;
	}
	LWLockRelease(MultiXactGenLock);
}

/*
 * Update our oldestMultiXactId value, but only if it's more recent than what
 * we had.
 *
 * This may only be called during WAL replay.
 */
void
MultiXactAdvanceOldest(MultiXactId oldestMulti, Oid oldestMultiDB)
{
	Assert(InRecovery);

	if (MultiXactIdPrecedes(MultiXactState->oldestMultiXactId, oldestMulti))
		SetMultiXactIdLimit(oldestMulti, oldestMultiDB);
}

/*
 * Make sure that MultiXactOffset has room for a newly-allocated MultiXactId.
 *
 * NB: this is called while holding MultiXactGenLock.  We want it to be very
 * fast most of the time; even when it's not so fast, no actual I/O need
 * happen unless we're forced to write out a dirty log or xlog page to make
 * room in shared memory.
 */
static void
ExtendMultiXactOffset(MultiXactId multi)
{
	int64		pageno;
	LWLock	   *lock;

	/*
	 * No work except at first MultiXactId of a page.  But beware: just after
	 * wraparound, the first MultiXactId of page zero is FirstMultiXactId.
	 */
	if (MultiXactIdToOffsetEntry(multi) != 0 &&
		multi != FirstMultiXactId)
		return;

	pageno = MultiXactIdToOffsetPage(multi);
	lock = SimpleLruGetBankLock(MultiXactOffsetCtl, pageno);

	LWLockAcquire(lock, LW_EXCLUSIVE);

	/* Zero the page and make a WAL entry about it */
	SimpleLruZeroPage(MultiXactOffsetCtl, pageno);
	XLogSimpleInsertInt64(RM_MULTIXACT_ID, XLOG_MULTIXACT_ZERO_OFF_PAGE,
						  pageno);

	LWLockRelease(lock);
}

/*
 * Make sure that MultiXactMember has room for the members of a newly-
 * allocated MultiXactId.
 *
 * Like the above routine, this is called while holding MultiXactGenLock;
 * same comments apply.
 */
static void
ExtendMultiXactMember(MultiXactOffset offset, int nmembers)
{
	/*
	 * It's possible that the members span more than one page of the members
	 * file, so we loop to ensure we consider each page.  The coding is not
	 * optimal if the members span several pages, but that seems unusual
	 * enough to not worry much about.
	 */
	while (nmembers > 0)
	{
		int			flagsoff;
		int			flagsbit;
		uint32		difference;

		/*
		 * Only zero when at first entry of a page.
		 */
		flagsoff = MXOffsetToFlagsOffset(offset);
		flagsbit = MXOffsetToFlagsBitShift(offset);
		if (flagsoff == 0 && flagsbit == 0)
		{
			int64		pageno;
			LWLock	   *lock;

			pageno = MXOffsetToMemberPage(offset);
			lock = SimpleLruGetBankLock(MultiXactMemberCtl, pageno);

			LWLockAcquire(lock, LW_EXCLUSIVE);

			/* Zero the page and make a WAL entry about it */
			SimpleLruZeroPage(MultiXactMemberCtl, pageno);
			XLogSimpleInsertInt64(RM_MULTIXACT_ID,
								  XLOG_MULTIXACT_ZERO_MEM_PAGE, pageno);

			LWLockRelease(lock);
		}

		/* Compute the number of items till end of current page. */
		difference = MULTIXACT_MEMBERS_PER_PAGE - offset % MULTIXACT_MEMBERS_PER_PAGE;

		/*
		 * Advance to next page.  OK if nmembers goes negative.
		 */
		nmembers -= difference;
		offset += difference;
	}
}

/*
 * GetOldestMultiXactId
 *
 * Return the oldest MultiXactId that's still possibly still seen as live by
 * any running transaction.  Older ones might still exist on disk, but they no
 * longer have any running member transaction.
 *
 * It's not safe to truncate MultiXact SLRU segments on the value returned by
 * this function; however, it can be set as the new relminmxid for any table
 * that VACUUM knows has no remaining MXIDs < the same value.  It is only safe
 * to truncate SLRUs when no table can possibly still have a referencing MXID.
 */
MultiXactId
GetOldestMultiXactId(void)
{
	MultiXactId oldestMXact;
	MultiXactId nextMXact;
	int			i;

	/*
	 * This is the oldest valid value among all the OldestMemberMXactId[] and
	 * OldestVisibleMXactId[] entries, or nextMXact if none are valid.
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	/*
	 * We have to beware of the possibility that nextMXact is in the
	 * wrapped-around state.  We don't fix the counter itself here, but we
	 * must be sure to use a valid value in our calculation.
	 */
	nextMXact = MultiXactState->nextMXact;
	if (nextMXact < FirstMultiXactId)
		nextMXact = FirstMultiXactId;

	oldestMXact = nextMXact;
	for (i = 0; i < MaxOldestSlot; i++)
	{
		MultiXactId thisoldest;

		thisoldest = OldestMemberMXactId[i];
		if (MultiXactIdIsValid(thisoldest) &&
			MultiXactIdPrecedes(thisoldest, oldestMXact))
			oldestMXact = thisoldest;
		thisoldest = OldestVisibleMXactId[i];
		if (MultiXactIdIsValid(thisoldest) &&
			MultiXactIdPrecedes(thisoldest, oldestMXact))
			oldestMXact = thisoldest;
	}

	LWLockRelease(MultiXactGenLock);

	return oldestMXact;
}

/*
 * Calculate the oldest member offset and install it in MultiXactState, where
 * it can be used to adjust multixid freezing cutoffs.
 */
static void
SetOldestOffset(void)
{
	MultiXactId oldestMultiXactId;
	MultiXactId nextMXact;
	MultiXactOffset oldestOffset = 0;	/* placate compiler */
	MultiXactOffset nextOffset;
	bool		oldestOffsetKnown = false;

	/*
	 * NB: Have to prevent concurrent truncation, we might otherwise try to
	 * lookup an oldestMulti that's concurrently getting truncated away.
	 */
	LWLockAcquire(MultiXactTruncationLock, LW_SHARED);

	/* Read relevant fields from shared memory. */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	oldestMultiXactId = MultiXactState->oldestMultiXactId;
	nextMXact = MultiXactState->nextMXact;
	nextOffset = MultiXactState->nextOffset;
	Assert(MultiXactState->finishedStartup);
	LWLockRelease(MultiXactGenLock);

	/*
	 * Determine the offset of the oldest multixact.  Normally, we can read
	 * the offset from the multixact itself, but there's an important special
	 * case: if there are no multixacts in existence at all, oldestMXact
	 * obviously can't point to one.  It will instead point to the multixact
	 * ID that will be assigned the next time one is needed.
	 */
	if (oldestMultiXactId == nextMXact)
	{
		/*
		 * When the next multixact gets created, it will be stored at the next
		 * offset.
		 */
		oldestOffset = nextOffset;
		oldestOffsetKnown = true;
	}
	else
	{
		/*
		 * Look up the offset at which the oldest existing multixact's members
		 * are stored.  If we cannot find it, be careful not to fail, and
		 * leave oldestOffset unchanged.  oldestOffset is initialized to zero
		 * at system startup, which prevents truncating members until a proper
		 * value is calculated.
		 *
		 * (We had bugs in early releases of PostgreSQL 9.3.X and 9.4.X where
		 * the supposedly-earliest multixact might not really exist.  Those
		 * should be long gone by now, so this should not fail, but let's
		 * still be defensive.)
		 */
		oldestOffsetKnown =
			find_multixact_start(oldestMultiXactId, &oldestOffset);

		if (oldestOffsetKnown)
			ereport(DEBUG1,
					(errmsg_internal("oldest MultiXactId member is at offset %" PRIu64,
									 oldestOffset)));
		else
			ereport(LOG,
					(errmsg("MultiXact member truncation is disabled because oldest checkpointed MultiXact %u does not exist on disk",
							oldestMultiXactId)));
	}

	LWLockRelease(MultiXactTruncationLock);

	/* Install the computed value */
	if (oldestOffsetKnown)
	{
		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
		MultiXactState->oldestOffset = oldestOffset;
		LWLockRelease(MultiXactGenLock);
	}
}

/*
 * Find the starting offset of the given MultiXactId.
 *
 * Returns false if the file containing the multi does not exist on disk.
 * Otherwise, returns true and sets *result to the starting member offset.
 *
 * This function does not prevent concurrent truncation, so if that's
 * required, the caller has to protect against that.
 */
static bool
find_multixact_start(MultiXactId multi, MultiXactOffset *result)
{
	MultiXactOffset offset;
	int64		pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;

	Assert(MultiXactState->finishedStartup);

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/*
	 * Write out dirty data, so PhysicalPageExists can work correctly.
	 */
	SimpleLruWriteAll(MultiXactOffsetCtl, true);
	SimpleLruWriteAll(MultiXactMemberCtl, true);

	if (!SimpleLruDoesPhysicalPageExist(MultiXactOffsetCtl, pageno))
		return false;

	/* lock is acquired by SimpleLruReadPage_ReadOnly */
	slotno = SimpleLruReadPage_ReadOnly(MultiXactOffsetCtl, pageno, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;
	offset = *offptr;
	LWLockRelease(SimpleLruGetBankLock(MultiXactOffsetCtl, pageno));

	*result = offset;
	return true;
}

/*
 * GetMultiXactInfo
 *
 * Returns information about the current MultiXact state, as of:
 * multixacts: Number of MultiXacts (nextMultiXactId - oldestMultiXactId)
 * members: Number of member entries (nextOffset - oldestOffset)
 * oldestMultiXactId: Oldest MultiXact ID still in use
 * oldestOffset: Oldest offset still in use
 */
void
GetMultiXactInfo(uint32 *multixacts, MultiXactOffset *members,
				 MultiXactId *oldestMultiXactId, MultiXactOffset *oldestOffset)
{
	MultiXactOffset nextOffset;
	MultiXactId nextMultiXactId;

	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	nextOffset = MultiXactState->nextOffset;
	*oldestMultiXactId = MultiXactState->oldestMultiXactId;
	nextMultiXactId = MultiXactState->nextMXact;
	*oldestOffset = MultiXactState->oldestOffset;
	LWLockRelease(MultiXactGenLock);

	*members = nextOffset - *oldestOffset;
	*multixacts = nextMultiXactId - *oldestMultiXactId;
}

/*
 * Multixact members can be removed once the multixacts that refer to them
 * are older than every datminmxid.  autovacuum_multixact_freeze_max_age and
 * vacuum_multixact_freeze_table_age work together to make sure we never have
 * too many multixacts; we hope that, at least under normal circumstances,
 * this will also be sufficient to keep us from using too many offsets.
 * However, if the average multixact has many members, we might accumulate a
 * large amount of members, consuming disk space, while still using few enough
 * multixids that the multixid limits fail to trigger relminmxid advancement
 * by VACUUM.
 *
 * To prevent that, if the members space usage exceeds a threshold
 * (MULTIXACT_MEMBER_LOW_THRESHOLD), we effectively reduce
 * autovacuum_multixact_freeze_max_age to a value just less than the number of
 * multixacts in use.  We hope that this will quickly trigger autovacuuming on
 * the table or tables with the oldest relminmxid, thus allowing datminmxid
 * values to advance and removing some members.
 *
 * As the amount of the member space in use grows, we become more aggressive
 * in clamping this value.  That not only causes autovacuum to ramp up, but
 * also makes any manual vacuums the user issues more aggressive.  This
 * happens because vacuum_get_cutoffs() will clamp the freeze table and the
 * minimum freeze age cutoffs based on the effective
 * autovacuum_multixact_freeze_max_age this function returns.  At the extreme,
 * when the members usage reaches MULTIXACT_MEMBER_HIGH_THRESHOLD, we clamp
 * freeze_max_age to zero, and every vacuum of any table will freeze every
 * multixact.
 */
int
MultiXactMemberFreezeThreshold(void)
{
	MultiXactOffset members;
	uint32		multixacts;
	uint32		victim_multixacts;
	double		fraction;
	int			result;
	MultiXactId oldestMultiXactId;
	MultiXactOffset oldestOffset;

	/* Read the current offsets and members usage. */
	GetMultiXactInfo(&multixacts, &members, &oldestMultiXactId, &oldestOffset);

	/* If member space utilization is low, no special action is required. */
	if (members <= MULTIXACT_MEMBER_LOW_THRESHOLD)
		return autovacuum_multixact_freeze_max_age;

	/*
	 * Compute a target for relminmxid advancement.  The number of multixacts
	 * we try to eliminate from the system is based on how far we are past
	 * MULTIXACT_MEMBER_LOW_THRESHOLD.
	 *
	 * The way this formula works is that when members is exactly at the low
	 * threshold, fraction = 0.0, and we set freeze_max_age equal to
	 * mxid_age(oldestMultiXactId).  As members grows further, towards the
	 * high threshold, fraction grows linearly from 0.0 to 1.0, and the result
	 * shrinks from mxid_age(oldestMultiXactId) to 0.  Beyond the high
	 * threshold, fraction > 1.0 and the result is clamped to 0.
	 */
	fraction = (double) (members - MULTIXACT_MEMBER_LOW_THRESHOLD) /
		(MULTIXACT_MEMBER_HIGH_THRESHOLD - MULTIXACT_MEMBER_LOW_THRESHOLD);

	/* fraction could be > 1.0, but lowest possible freeze age is zero */
	if (fraction >= 1.0)
		return 0;

	victim_multixacts = multixacts * fraction;
	result = multixacts - victim_multixacts;

	/*
	 * Clamp to autovacuum_multixact_freeze_max_age, so that we never make
	 * autovacuum less aggressive than it would otherwise be.
	 */
	return Min(result, autovacuum_multixact_freeze_max_age);
}

typedef struct mxtruncinfo
{
	int64		earliestExistingPage;
} mxtruncinfo;

/*
 * SlruScanDirectory callback
 *		This callback determines the earliest existing page number.
 */
static bool
SlruScanDirCbFindEarliest(SlruCtl ctl, char *filename, int64 segpage, void *data)
{
	mxtruncinfo *trunc = (mxtruncinfo *) data;

	if (trunc->earliestExistingPage == -1 ||
		ctl->PagePrecedes(segpage, trunc->earliestExistingPage))
	{
		trunc->earliestExistingPage = segpage;
	}

	return false;				/* keep going */
}


/*
 * Delete members segments [oldest, newOldest)
 */
static void
PerformMembersTruncation(MultiXactOffset oldestOffset, MultiXactOffset newOldestOffset)
{
	SimpleLruTruncate(MultiXactMemberCtl,
					  MXOffsetToMemberPage(newOldestOffset));
}

/*
 * Delete offsets segments [oldest, newOldest)
 */
static void
PerformOffsetsTruncation(MultiXactId oldestMulti, MultiXactId newOldestMulti)
{
	/*
	 * We step back one multixact to avoid passing a cutoff page that hasn't
	 * been created yet in the rare case that oldestMulti would be the first
	 * item on a page and oldestMulti == nextMulti.  In that case, if we
	 * didn't subtract one, we'd trigger SimpleLruTruncate's wraparound
	 * detection.
	 */
	SimpleLruTruncate(MultiXactOffsetCtl,
					  MultiXactIdToOffsetPage(PreviousMultiXactId(newOldestMulti)));
}

/*
 * Remove all MultiXactOffset and MultiXactMember segments before the oldest
 * ones still of interest.
 *
 * This is only called on a primary as part of vacuum (via
 * vac_truncate_clog()). During recovery truncation is done by replaying
 * truncation WAL records logged here.
 *
 * newOldestMulti is the oldest currently required multixact, newOldestMultiDB
 * is one of the databases preventing newOldestMulti from increasing.
 */
void
TruncateMultiXact(MultiXactId newOldestMulti, Oid newOldestMultiDB)
{
	MultiXactId oldestMulti;
	MultiXactId nextMulti;
	MultiXactOffset newOldestOffset;
	MultiXactOffset oldestOffset;
	MultiXactOffset nextOffset;
	mxtruncinfo trunc;
	MultiXactId earliest;

	Assert(!RecoveryInProgress());
	Assert(MultiXactState->finishedStartup);

	/*
	 * We can only allow one truncation to happen at once. Otherwise parts of
	 * members might vanish while we're doing lookups or similar. There's no
	 * need to have an interlock with creating new multis or such, since those
	 * are constrained by the limits (which only grow, never shrink).
	 */
	LWLockAcquire(MultiXactTruncationLock, LW_EXCLUSIVE);

	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	nextMulti = MultiXactState->nextMXact;
	nextOffset = MultiXactState->nextOffset;
	oldestMulti = MultiXactState->oldestMultiXactId;
	LWLockRelease(MultiXactGenLock);
	Assert(MultiXactIdIsValid(oldestMulti));

	/*
	 * Make sure to only attempt truncation if there's values to truncate
	 * away. In normal processing values shouldn't go backwards, but there's
	 * some corner cases (due to bugs) where that's possible.
	 */
	if (MultiXactIdPrecedesOrEquals(newOldestMulti, oldestMulti))
	{
		LWLockRelease(MultiXactTruncationLock);
		return;
	}

	/*
	 * Note we can't just plow ahead with the truncation; it's possible that
	 * there are no segments to truncate, which is a problem because we are
	 * going to attempt to read the offsets page to determine where to
	 * truncate the members SLRU.  So we first scan the directory to determine
	 * the earliest offsets page number that we can read without error.
	 *
	 * When nextMXact is less than one segment away from multiWrapLimit,
	 * SlruScanDirCbFindEarliest can find some early segment other than the
	 * actual earliest.  (MultiXactOffsetPagePrecedes(EARLIEST, LATEST)
	 * returns false, because not all pairs of entries have the same answer.)
	 * That can also arise when an earlier truncation attempt failed unlink()
	 * or returned early from this function.  The only consequence is
	 * returning early, which wastes space that we could have liberated.
	 *
	 * NB: It's also possible that the page that oldestMulti is on has already
	 * been truncated away, and we crashed before updating oldestMulti.
	 */
	trunc.earliestExistingPage = -1;
	SlruScanDirectory(MultiXactOffsetCtl, SlruScanDirCbFindEarliest, &trunc);
	earliest = trunc.earliestExistingPage * MULTIXACT_OFFSETS_PER_PAGE;
	if (earliest < FirstMultiXactId)
		earliest = FirstMultiXactId;

	/* If there's nothing to remove, we can bail out early. */
	if (MultiXactIdPrecedes(oldestMulti, earliest))
	{
		LWLockRelease(MultiXactTruncationLock);
		return;
	}

	/*
	 * First, compute the safe truncation point for MultiXactMember. This is
	 * the starting offset of the oldest multixact.
	 *
	 * Hopefully, find_multixact_start will always work here, because we've
	 * already checked that it doesn't precede the earliest MultiXact on disk.
	 * But if it fails, don't truncate anything, and log a message.
	 */
	if (oldestMulti == nextMulti)
	{
		/* there are NO MultiXacts */
		oldestOffset = nextOffset;
	}
	else if (!find_multixact_start(oldestMulti, &oldestOffset))
	{
		ereport(LOG,
				(errmsg("oldest MultiXact %u not found, earliest MultiXact %u, skipping truncation",
						oldestMulti, earliest)));
		LWLockRelease(MultiXactTruncationLock);
		return;
	}

	/*
	 * Secondly compute up to where to truncate. Lookup the corresponding
	 * member offset for newOldestMulti for that.
	 */
	if (newOldestMulti == nextMulti)
	{
		/* there are NO MultiXacts */
		newOldestOffset = nextOffset;
	}
	else if (!find_multixact_start(newOldestMulti, &newOldestOffset))
	{
		ereport(LOG,
				(errmsg("cannot truncate up to MultiXact %u because it does not exist on disk, skipping truncation",
						newOldestMulti)));
		LWLockRelease(MultiXactTruncationLock);
		return;
	}

	elog(DEBUG1, "performing multixact truncation: "
		 "offsets [%u, %u), offsets segments [%" PRIx64 ", %" PRIx64 "), "
		 "members [%" PRIu64 ", %" PRIu64 "), members segments [%" PRIx64 ", %" PRIx64 ")",
		 oldestMulti, newOldestMulti,
		 MultiXactIdToOffsetSegment(oldestMulti),
		 MultiXactIdToOffsetSegment(newOldestMulti),
		 oldestOffset, newOldestOffset,
		 MXOffsetToMemberSegment(oldestOffset),
		 MXOffsetToMemberSegment(newOldestOffset));

	/*
	 * Do truncation, and the WAL logging of the truncation, in a critical
	 * section. That way offsets/members cannot get out of sync anymore, i.e.
	 * once consistent the newOldestMulti will always exist in members, even
	 * if we crashed in the wrong moment.
	 */
	START_CRIT_SECTION();

	/*
	 * Prevent checkpoints from being scheduled concurrently. This is critical
	 * because otherwise a truncation record might not be replayed after a
	 * crash/basebackup, even though the state of the data directory would
	 * require it.
	 */
	Assert((MyProc->delayChkptFlags & DELAY_CHKPT_START) == 0);
	MyProc->delayChkptFlags |= DELAY_CHKPT_START;

	/* WAL log truncation */
	WriteMTruncateXlogRec(newOldestMultiDB,
						  oldestMulti, newOldestMulti,
						  oldestOffset, newOldestOffset);

	/*
	 * Update in-memory limits before performing the truncation, while inside
	 * the critical section: Have to do it before truncation, to prevent
	 * concurrent lookups of those values. Has to be inside the critical
	 * section as otherwise a future call to this function would error out,
	 * while looking up the oldest member in offsets, if our caller crashes
	 * before updating the limits.
	 */
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->oldestMultiXactId = newOldestMulti;
	MultiXactState->oldestMultiXactDB = newOldestMultiDB;
	MultiXactState->oldestOffset = newOldestOffset;
	LWLockRelease(MultiXactGenLock);

	/* First truncate members */
	PerformMembersTruncation(oldestOffset, newOldestOffset);

	/* Then offsets */
	PerformOffsetsTruncation(oldestMulti, newOldestMulti);

	MyProc->delayChkptFlags &= ~DELAY_CHKPT_START;

	END_CRIT_SECTION();
	LWLockRelease(MultiXactTruncationLock);
}

/*
 * Decide whether a MultiXactOffset page number is "older" for truncation
 * purposes.  Analogous to CLOGPagePrecedes().
 *
 * Offsetting the values is optional, because MultiXactIdPrecedes() has
 * translational symmetry.
 */
static bool
MultiXactOffsetPagePrecedes(int64 page1, int64 page2)
{
	MultiXactId multi1;
	MultiXactId multi2;

	multi1 = ((MultiXactId) page1) * MULTIXACT_OFFSETS_PER_PAGE;
	multi1 += FirstMultiXactId + 1;
	multi2 = ((MultiXactId) page2) * MULTIXACT_OFFSETS_PER_PAGE;
	multi2 += FirstMultiXactId + 1;

	return (MultiXactIdPrecedes(multi1, multi2) &&
			MultiXactIdPrecedes(multi1,
								multi2 + MULTIXACT_OFFSETS_PER_PAGE - 1));
}

/*
 * Decide whether a MultiXactMember page number is "older" for truncation
 * purposes.  There is no "invalid offset number" and members never wrap
 * around, so use the numbers verbatim.
 */
static bool
MultiXactMemberPagePrecedes(int64 page1, int64 page2)
{
	return page1 < page2;
}

/*
 * Decide which of two MultiXactIds is earlier.
 *
 * XXX do we need to do something special for InvalidMultiXactId?
 * (Doesn't look like it.)
 */
bool
MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2)
{
	int32		diff = (int32) (multi1 - multi2);

	return (diff < 0);
}

/*
 * MultiXactIdPrecedesOrEquals -- is multi1 logically <= multi2?
 *
 * XXX do we need to do something special for InvalidMultiXactId?
 * (Doesn't look like it.)
 */
bool
MultiXactIdPrecedesOrEquals(MultiXactId multi1, MultiXactId multi2)
{
	int32		diff = (int32) (multi1 - multi2);

	return (diff <= 0);
}


/*
 * Write a TRUNCATE xlog record
 *
 * We must flush the xlog record to disk before returning --- see notes in
 * TruncateCLOG().
 */
static void
WriteMTruncateXlogRec(Oid oldestMultiDB,
					  MultiXactId startTruncOff, MultiXactId endTruncOff,
					  MultiXactOffset startTruncMemb, MultiXactOffset endTruncMemb)
{
	XLogRecPtr	recptr;
	xl_multixact_truncate xlrec;

	xlrec.oldestMultiDB = oldestMultiDB;

	xlrec.startTruncOff = startTruncOff;
	xlrec.endTruncOff = endTruncOff;

	xlrec.startTruncMemb = startTruncMemb;
	xlrec.endTruncMemb = endTruncMemb;

	XLogBeginInsert();
	XLogRegisterData(&xlrec, SizeOfMultiXactTruncate);
	recptr = XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_TRUNCATE_ID);
	XLogFlush(recptr);
}

/*
 * MULTIXACT resource manager's routines
 */
void
multixact_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/* Backup blocks are not used in multixact records */
	Assert(!XLogRecHasAnyBlockRefs(record));

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE)
	{
		int64		pageno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(pageno));
		SimpleLruZeroAndWritePage(MultiXactOffsetCtl, pageno);
	}
	else if (info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
	{
		int64		pageno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(pageno));
		SimpleLruZeroAndWritePage(MultiXactMemberCtl, pageno);
	}
	else if (info == XLOG_MULTIXACT_CREATE_ID)
	{
		xl_multixact_create *xlrec =
			(xl_multixact_create *) XLogRecGetData(record);
		TransactionId max_xid;
		int			i;

		/* Store the data back into the SLRU files */
		RecordNewMultiXact(xlrec->mid, xlrec->moff, xlrec->nmembers,
						   xlrec->members);

		/* Make sure nextMXact/nextOffset are beyond what this record has */
		MultiXactAdvanceNextMXact(xlrec->mid + 1,
								  xlrec->moff + xlrec->nmembers);

		/*
		 * Make sure nextXid is beyond any XID mentioned in the record. This
		 * should be unnecessary, since any XID found here ought to have other
		 * evidence in the XLOG, but let's be safe.
		 */
		max_xid = XLogRecGetXid(record);
		for (i = 0; i < xlrec->nmembers; i++)
		{
			if (TransactionIdPrecedes(max_xid, xlrec->members[i].xid))
				max_xid = xlrec->members[i].xid;
		}

		AdvanceNextFullTransactionIdPastXid(max_xid);
	}
	else if (info == XLOG_MULTIXACT_TRUNCATE_ID)
	{
		xl_multixact_truncate xlrec;
		int64		pageno;

		memcpy(&xlrec, XLogRecGetData(record),
			   SizeOfMultiXactTruncate);

		elog(DEBUG1, "replaying multixact truncation: "
			 "offsets [%u, %u), offsets segments [%" PRIx64 ", %" PRIx64 "), "
			 "members [%" PRIu64 ", %" PRIu64 "), members segments [%" PRIx64 ", %" PRIx64 ")",
			 xlrec.startTruncOff, xlrec.endTruncOff,
			 MultiXactIdToOffsetSegment(xlrec.startTruncOff),
			 MultiXactIdToOffsetSegment(xlrec.endTruncOff),
			 xlrec.startTruncMemb, xlrec.endTruncMemb,
			 MXOffsetToMemberSegment(xlrec.startTruncMemb),
			 MXOffsetToMemberSegment(xlrec.endTruncMemb));

		/* should not be required, but more than cheap enough */
		LWLockAcquire(MultiXactTruncationLock, LW_EXCLUSIVE);

		/*
		 * Advance the horizon values, so they're current at the end of
		 * recovery.
		 */
		SetMultiXactIdLimit(xlrec.endTruncOff, xlrec.oldestMultiDB);

		PerformMembersTruncation(xlrec.startTruncMemb, xlrec.endTruncMemb);

		/*
		 * During XLOG replay, latest_page_number isn't necessarily set up
		 * yet; insert a suitable value to bypass the sanity test in
		 * SimpleLruTruncate.
		 */
		pageno = MultiXactIdToOffsetPage(xlrec.endTruncOff);
		pg_atomic_write_u64(&MultiXactOffsetCtl->shared->latest_page_number,
							pageno);
		PerformOffsetsTruncation(xlrec.startTruncOff, xlrec.endTruncOff);

		LWLockRelease(MultiXactTruncationLock);
	}
	else
		elog(PANIC, "multixact_redo: unknown op code %u", info);
}

/*
 * Entrypoint for sync.c to sync offsets files.
 */
int
multixactoffsetssyncfiletag(const FileTag *ftag, char *path)
{
	return SlruSyncFileTag(MultiXactOffsetCtl, ftag, path);
}

/*
 * Entrypoint for sync.c to sync members files.
 */
int
multixactmemberssyncfiletag(const FileTag *ftag, char *path)
{
	return SlruSyncFileTag(MultiXactMemberCtl, ftag, path);
}
