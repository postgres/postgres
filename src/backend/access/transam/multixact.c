/*-------------------------------------------------------------------------
 *
 * multixact.c
 *		PostgreSQL multi-transaction-log manager
 *
 * The pg_multixact manager is a pg_clog-like manager that stores an array
 * of TransactionIds for each MultiXactId.	It is a fundamental part of the
 * shared-row-lock implementation.	A share-locked tuple stores a
 * MultiXactId in its Xmax, and a transaction that needs to wait for the
 * tuple to be unlocked can sleep on the potentially-several TransactionIds
 * that compose the MultiXactId.
 *
 * We use two SLRU areas, one for storing the offsets at which the data
 * starts for each MultiXactId in the other one.  This trick allows us to
 * store variable length arrays of TransactionIds.	(We could alternatively
 * use one area containing counts and TransactionIds, with valid MultiXactId
 * values pointing at slots containing counts; but that way seems less robust
 * since it would get completely confused if someone inquired about a bogus
 * MultiXactId that pointed to an intermediate slot containing an XID.)
 *
 * XLOG interactions: this module generates an XLOG record whenever a new
 * OFFSETs or MEMBERs page is initialized to zeroes, as well as an XLOG record
 * whenever a new MultiXactId is defined.  This allows us to completely
 * rebuild the data entered since the last checkpoint during XLOG replay.
 * Because this is possible, we need not follow the normal rule of
 * "write WAL before data"; the only correctness guarantee needed is that
 * we flush and sync all dirty OFFSETs and MEMBERs pages to disk before a
 * checkpoint is considered complete.  If a page does make it to disk ahead
 * of corresponding WAL records, it will be forcibly zeroed before use anyway.
 * Therefore, we don't need to mark our pages with LSN information; we have
 * enough synchronization already.
 *
 * Like clog.c, and unlike subtrans.c, we have to preserve state across
 * crashes and ensure that MXID and offset numbering increases monotonically
 * across a crash.	We do this in the same way as it's done for transaction
 * IDs: the WAL record is guaranteed to contain evidence of every MXID we
 * could need to worry about, and we just make sure that at the end of
 * replay, the next-MXID and next-offset counters are at least as large as
 * anything we saw during replay.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/access/transam/multixact.c,v 1.22 2006/11/17 18:00:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xact.h"
#include "miscadmin.h"
#include "storage/backendid.h"
#include "storage/lmgr.h"
#include "utils/memutils.h"
#include "storage/procarray.h"


/*
 * Defines for MultiXactOffset page sizes.	A page is the same BLCKSZ as is
 * used everywhere else in Postgres.
 *
 * Note: because both MultiXactOffsets and TransactionIds are 32 bits and
 * wrap around at 0xFFFFFFFF, MultiXact page numbering also wraps around at
 * 0xFFFFFFFF/MULTIXACT_*_PER_PAGE, and segment numbering at
 * 0xFFFFFFFF/MULTIXACT_*_PER_PAGE/SLRU_SEGMENTS_PER_PAGE.	We need take no
 * explicit notice of that fact in this module, except when comparing segment
 * and page numbers in TruncateMultiXact
 * (see MultiXact{Offset,Member}PagePrecedes).
 */

/* We need four bytes per offset and also four bytes per member */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset))
#define MULTIXACT_MEMBERS_PER_PAGE (BLCKSZ / sizeof(TransactionId))

#define MultiXactIdToOffsetPage(xid) \
	((xid) / (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)
#define MultiXactIdToOffsetEntry(xid) \
	((xid) % (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)

#define MXOffsetToMemberPage(xid) \
	((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_PAGE)
#define MXOffsetToMemberEntry(xid) \
	((xid) % (TransactionId) MULTIXACT_MEMBERS_PER_PAGE)


/*
 * Links to shared-memory data structures for MultiXact control
 */
static SlruCtlData MultiXactOffsetCtlData;
static SlruCtlData MultiXactMemberCtlData;

#define MultiXactOffsetCtl	(&MultiXactOffsetCtlData)
#define MultiXactMemberCtl	(&MultiXactMemberCtlData)

/*
 * MultiXact state shared across all backends.	All this state is protected
 * by MultiXactGenLock.  (We also use MultiXactOffsetControlLock and
 * MultiXactMemberControlLock to guard accesses to the two sets of SLRU
 * buffers.  For concurrency's sake, we avoid holding more than one of these
 * locks at a time.)
 */
typedef struct MultiXactStateData
{
	/* next-to-be-assigned MultiXactId */
	MultiXactId nextMXact;

	/* next-to-be-assigned offset */
	MultiXactOffset nextOffset;

	/* the Offset SLRU area was last truncated at this MultiXactId */
	MultiXactId lastTruncationPoint;

	/*
	 * Per-backend data starts here.  We have two arrays stored in the area
	 * immediately following the MultiXactStateData struct. Each is indexed by
	 * BackendId.  (Note: valid BackendIds run from 1 to MaxBackends; element
	 * zero of each array is never used.)
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
	 * the checkpointer could truncate away such data at any instant.
	 *
	 * The checkpointer can compute the safe truncation point as the oldest
	 * valid value among all the OldestMemberMXactId[] and
	 * OldestVisibleMXactId[] entries, or nextMXact if none are valid.
	 * Clearly, it is not possible for any later-computed OldestVisibleMXactId
	 * value to be older than this, and so there is no risk of truncating data
	 * that is still needed.
	 */
	MultiXactId perBackendXactIds[1];	/* VARIABLE LENGTH ARRAY */
} MultiXactStateData;

/* Pointers to the state data in shared memory */
static MultiXactStateData *MultiXactState;
static MultiXactId *OldestMemberMXactId;
static MultiXactId *OldestVisibleMXactId;


/*
 * Definitions for the backend-local MultiXactId cache.
 *
 * We use this cache to store known MultiXacts, so we don't need to go to
 * SLRU areas everytime.
 *
 * The cache lasts for the duration of a single transaction, the rationale
 * for this being that most entries will contain our own TransactionId and
 * so they will be uninteresting by the time our next transaction starts.
 * (XXX not clear that this is correct --- other members of the MultiXact
 * could hang around longer than we did.  However, it's not clear what a
 * better policy for flushing old cache entries would be.)
 *
 * We allocate the cache entries in a memory context that is deleted at
 * transaction end, so we don't need to do retail freeing of entries.
 */
typedef struct mXactCacheEnt
{
	struct mXactCacheEnt *next;
	MultiXactId multi;
	int			nxids;
	TransactionId xids[1];		/* VARIABLE LENGTH ARRAY */
} mXactCacheEnt;

static mXactCacheEnt *MXactCache = NULL;
static MemoryContext MXactContext = NULL;


#ifdef MULTIXACT_DEBUG
#define debug_elog2(a,b) elog(a,b)
#define debug_elog3(a,b,c) elog(a,b,c)
#define debug_elog4(a,b,c,d) elog(a,b,c,d)
#define debug_elog5(a,b,c,d,e) elog(a,b,c,d,e)
#else
#define debug_elog2(a,b)
#define debug_elog3(a,b,c)
#define debug_elog4(a,b,c,d)
#define debug_elog5(a,b,c,d,e)
#endif

/* internal MultiXactId management */
static void MultiXactIdSetOldestVisible(void);
static MultiXactId CreateMultiXactId(int nxids, TransactionId *xids);
static void RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
				   int nxids, TransactionId *xids);
static MultiXactId GetNewMultiXactId(int nxids, MultiXactOffset *offset);

/* MultiXact cache management */
static MultiXactId mXactCacheGetBySet(int nxids, TransactionId *xids);
static int	mXactCacheGetById(MultiXactId multi, TransactionId **xids);
static void mXactCachePut(MultiXactId multi, int nxids, TransactionId *xids);
static int	xidComparator(const void *arg1, const void *arg2);

#ifdef MULTIXACT_DEBUG
static char *mxid_to_string(MultiXactId multi, int nxids, TransactionId *xids);
#endif

/* management of SLRU infrastructure */
static int	ZeroMultiXactOffsetPage(int pageno, bool writeXlog);
static int	ZeroMultiXactMemberPage(int pageno, bool writeXlog);
static bool MultiXactOffsetPagePrecedes(int page1, int page2);
static bool MultiXactMemberPagePrecedes(int page1, int page2);
static bool MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2);
static bool MultiXactOffsetPrecedes(MultiXactOffset offset1,
						MultiXactOffset offset2);
static void ExtendMultiXactOffset(MultiXactId multi);
static void ExtendMultiXactMember(MultiXactOffset offset, int nmembers);
static void TruncateMultiXact(void);
static void WriteMZeroPageXlogRec(int pageno, uint8 info);


/*
 * MultiXactIdCreate
 *		Construct a MultiXactId representing two TransactionIds.
 *
 * The two XIDs must be different.
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
MultiXactId
MultiXactIdCreate(TransactionId xid1, TransactionId xid2)
{
	MultiXactId newMulti;
	TransactionId xids[2];

	AssertArg(TransactionIdIsValid(xid1));
	AssertArg(TransactionIdIsValid(xid2));

	Assert(!TransactionIdEquals(xid1, xid2));

	/*
	 * Note: unlike MultiXactIdExpand, we don't bother to check that both XIDs
	 * are still running.  In typical usage, xid2 will be our own XID and the
	 * caller just did a check on xid1, so it'd be wasted effort.
	 */

	xids[0] = xid1;
	xids[1] = xid2;

	newMulti = CreateMultiXactId(2, xids);

	debug_elog5(DEBUG2, "Create: returning %u for %u, %u",
				newMulti, xid1, xid2);

	return newMulti;
}

/*
 * MultiXactIdExpand
 *		Add a TransactionId to a pre-existing MultiXactId.
 *
 * If the TransactionId is already a member of the passed MultiXactId,
 * just return it as-is.
 *
 * Note that we do NOT actually modify the membership of a pre-existing
 * MultiXactId; instead we create a new one.  This is necessary to avoid
 * a race condition against MultiXactIdWait (see notes there).
 *
 * NB - we don't worry about our local MultiXactId cache here, because that
 * is handled by the lower-level routines.
 */
MultiXactId
MultiXactIdExpand(MultiXactId multi, TransactionId xid)
{
	MultiXactId newMulti;
	TransactionId *members;
	TransactionId *newMembers;
	int			nmembers;
	int			i;
	int			j;

	AssertArg(MultiXactIdIsValid(multi));
	AssertArg(TransactionIdIsValid(xid));

	debug_elog4(DEBUG2, "Expand: received multi %u, xid %u",
				multi, xid);

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers < 0)
	{
		/*
		 * The MultiXactId is obsolete.  This can only happen if all the
		 * MultiXactId members stop running between the caller checking and
		 * passing it to us.  It would be better to return that fact to the
		 * caller, but it would complicate the API and it's unlikely to happen
		 * too often, so just deal with it by creating a singleton MultiXact.
		 */
		newMulti = CreateMultiXactId(1, &xid);

		debug_elog4(DEBUG2, "Expand: %u has no members, create singleton %u",
					multi, newMulti);
		return newMulti;
	}

	/*
	 * If the TransactionId is already a member of the MultiXactId, just
	 * return the existing MultiXactId.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdEquals(members[i], xid))
		{
			debug_elog4(DEBUG2, "Expand: %u is already a member of %u",
						xid, multi);
			pfree(members);
			return multi;
		}
	}

	/*
	 * Determine which of the members of the MultiXactId are still running,
	 * and use them to create a new one.  (Removing dead members is just an
	 * optimization, but a useful one.	Note we have the same race condition
	 * here as above: j could be 0 at the end of the loop.)
	 */
	newMembers = (TransactionId *)
		palloc(sizeof(TransactionId) * (nmembers + 1));

	for (i = 0, j = 0; i < nmembers; i++)
	{
		if (TransactionIdIsInProgress(members[i]))
			newMembers[j++] = members[i];
	}

	newMembers[j++] = xid;
	newMulti = CreateMultiXactId(j, newMembers);

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
 */
bool
MultiXactIdIsRunning(MultiXactId multi)
{
	TransactionId *members;
	int			nmembers;
	int			i;

	debug_elog3(DEBUG2, "IsRunning %u?", multi);

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers < 0)
	{
		debug_elog2(DEBUG2, "IsRunning: no members");
		return false;
	}

	/*
	 * Checking for myself is cheap compared to looking in shared memory,
	 * so first do the equivalent of MultiXactIdIsCurrent().  This is not
	 * needed for correctness, it's just a fast path.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsCurrentTransactionId(members[i]))
		{
			debug_elog3(DEBUG2, "IsRunning: I (%d) am running!", i);
			pfree(members);
			return true;
		}
	}

	/*
	 * This could be made faster by having another entry point in procarray.c,
	 * walking the PGPROC array only once for all the members.	But in most
	 * cases nmembers should be small enough that it doesn't much matter.
	 */
	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsInProgress(members[i]))
		{
			debug_elog4(DEBUG2, "IsRunning: member %d (%u) is running",
						i, members[i]);
			pfree(members);
			return true;
		}
	}

	pfree(members);

	debug_elog3(DEBUG2, "IsRunning: %u is not running", multi);

	return false;
}

/*
 * MultiXactIdIsCurrent
 *		Returns true if the current transaction is a member of the MultiXactId.
 *
 * We return true if any live subtransaction of the current top-level
 * transaction is a member.  This is appropriate for the same reason that a
 * lock held by any such subtransaction is globally equivalent to a lock
 * held by the current subtransaction: no such lock could be released without
 * aborting this subtransaction, and hence releasing its locks.  So it's not
 * necessary to add the current subxact to the MultiXact separately.
 */
bool
MultiXactIdIsCurrent(MultiXactId multi)
{
	bool		result = false;
	TransactionId *members;
	int			nmembers;
	int			i;

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers < 0)
		return false;

	for (i = 0; i < nmembers; i++)
	{
		if (TransactionIdIsCurrentTransactionId(members[i]))
		{
			result = true;
			break;
		}
	}

	pfree(members);

	return result;
}

/*
 * MultiXactIdSetOldestMember
 *		Save the oldest MultiXactId this transaction could be a member of.
 *
 * We set the OldestMemberMXactId for a given transaction the first time
 * it's going to acquire a shared lock.  We need to do this even if we end
 * up using a TransactionId instead of a MultiXactId, because there is a
 * chance that another transaction would add our XID to a MultiXactId.
 *
 * The value to set is the next-to-be-assigned MultiXactId, so this is meant
 * to be called just before acquiring a shared lock.
 */
void
MultiXactIdSetOldestMember(void)
{
	if (!MultiXactIdIsValid(OldestMemberMXactId[MyBackendId]))
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
		 */
		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

		/*
		 * We have to beware of the possibility that nextMXact is in the
		 * wrapped-around state.  We don't fix the counter itself here, but we
		 * must be sure to store a valid value in our array entry.
		 */
		nextMXact = MultiXactState->nextMXact;
		if (nextMXact < FirstMultiXactId)
			nextMXact = FirstMultiXactId;

		OldestMemberMXactId[MyBackendId] = nextMXact;

		LWLockRelease(MultiXactGenLock);

		debug_elog4(DEBUG2, "MultiXact: setting OldestMember[%d] = %u",
					MyBackendId, nextMXact);
	}
}

/*
 * MultiXactIdSetOldestVisible
 *		Save the oldest MultiXactId this transaction considers possibly live.
 *
 * We set the OldestVisibleMXactId for a given transaction the first time
 * it's going to inspect any MultiXactId.  Once we have set this, we are
 * guaranteed that the checkpointer won't truncate off SLRU data for
 * MultiXactIds at or after our OldestVisibleMXactId.
 *
 * The value to set is the oldest of nextMXact and all the valid per-backend
 * OldestMemberMXactId[] entries.  Because of the locking we do, we can be
 * certain that no subsequent call to MultiXactIdSetOldestMember can set
 * an OldestMemberMXactId[] entry older than what we compute here.	Therefore
 * there is no live transaction, now or later, that can be a member of any
 * MultiXactId older than the OldestVisibleMXactId we compute here.
 */
static void
MultiXactIdSetOldestVisible(void)
{
	if (!MultiXactIdIsValid(OldestVisibleMXactId[MyBackendId]))
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

		for (i = 1; i <= MaxBackends; i++)
		{
			MultiXactId thisoldest = OldestMemberMXactId[i];

			if (MultiXactIdIsValid(thisoldest) &&
				MultiXactIdPrecedes(thisoldest, oldestMXact))
				oldestMXact = thisoldest;
		}

		OldestVisibleMXactId[MyBackendId] = oldestMXact;

		LWLockRelease(MultiXactGenLock);

		debug_elog4(DEBUG2, "MultiXact: setting OldestVisible[%d] = %u",
					MyBackendId, oldestMXact);
	}
}

/*
 * MultiXactIdWait
 *		Sleep on a MultiXactId.
 *
 * We do this by sleeping on each member using XactLockTableWait.  Any
 * members that belong to the current backend are *not* waited for, however;
 * this would not merely be useless but would lead to Assert failure inside
 * XactLockTableWait.  By the time this returns, it is certain that all
 * transactions *of other backends* that were members of the MultiXactId
 * are dead (and no new ones can have been added, since it is not legal
 * to add members to an existing MultiXactId).
 *
 * But by the time we finish sleeping, someone else may have changed the Xmax
 * of the containing tuple, so the caller needs to iterate on us somehow.
 */
void
MultiXactIdWait(MultiXactId multi)
{
	TransactionId *members;
	int			nmembers;

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId member = members[i];

			debug_elog4(DEBUG2, "MultiXactIdWait: waiting for %d (%u)",
						i, member);
			if (!TransactionIdIsCurrentTransactionId(member))
				XactLockTableWait(member);
		}

		pfree(members);
	}
}

/*
 * ConditionalMultiXactIdWait
 *		As above, but only lock if we can get the lock without blocking.
 */
bool
ConditionalMultiXactIdWait(MultiXactId multi)
{
	bool		result = true;
	TransactionId *members;
	int			nmembers;

	nmembers = GetMultiXactIdMembers(multi, &members);

	if (nmembers >= 0)
	{
		int			i;

		for (i = 0; i < nmembers; i++)
		{
			TransactionId member = members[i];

			debug_elog4(DEBUG2, "ConditionalMultiXactIdWait: trying %d (%u)",
						i, member);
			if (!TransactionIdIsCurrentTransactionId(member))
			{
				result = ConditionalXactLockTableWait(member);
				if (!result)
					break;
			}
		}

		pfree(members);
	}

	return result;
}

/*
 * CreateMultiXactId
 *		Make a new MultiXactId
 *
 * Make XLOG, SLRU and cache entries for a new MultiXactId, recording the
 * given TransactionIds as members.  Returns the newly created MultiXactId.
 *
 * NB: the passed xids[] array will be sorted in-place.
 */
static MultiXactId
CreateMultiXactId(int nxids, TransactionId *xids)
{
	MultiXactId multi;
	MultiXactOffset offset;
	XLogRecData rdata[2];
	xl_multixact_create xlrec;

	debug_elog3(DEBUG2, "Create: %s",
				mxid_to_string(InvalidMultiXactId, nxids, xids));

	/*
	 * See if the same set of XIDs already exists in our cache; if so, just
	 * re-use that MultiXactId.  (Note: it might seem that looking in our
	 * cache is insufficient, and we ought to search disk to see if a
	 * duplicate definition already exists.  But since we only ever create
	 * MultiXacts containing our own XID, in most cases any such MultiXacts
	 * were in fact created by us, and so will be in our cache.  There are
	 * corner cases where someone else added us to a MultiXact without our
	 * knowledge, but it's not worth checking for.)
	 */
	multi = mXactCacheGetBySet(nxids, xids);
	if (MultiXactIdIsValid(multi))
	{
		debug_elog2(DEBUG2, "Create: in cache!");
		return multi;
	}

	/*
	 * Assign the MXID and offsets range to use, and make sure there is space
	 * in the OFFSETs and MEMBERs files.  NB: this routine does
	 * START_CRIT_SECTION().
	 */
	multi = GetNewMultiXactId(nxids, &offset);

	/*
	 * Make an XLOG entry describing the new MXID.
	 *
	 * Note: we need not flush this XLOG entry to disk before proceeding. The
	 * only way for the MXID to be referenced from any data page is for
	 * heap_lock_tuple() to have put it there, and heap_lock_tuple() generates
	 * an XLOG record that must follow ours.  The normal LSN interlock between
	 * the data page and that XLOG record will ensure that our XLOG record
	 * reaches disk first.	If the SLRU members/offsets data reaches disk
	 * sooner than the XLOG record, we do not care because we'll overwrite it
	 * with zeroes unless the XLOG record is there too; see notes at top of
	 * this file.
	 */
	xlrec.mid = multi;
	xlrec.moff = offset;
	xlrec.nxids = nxids;

	rdata[0].data = (char *) (&xlrec);
	rdata[0].len = MinSizeOfMultiXactCreate;
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);
	rdata[1].data = (char *) xids;
	rdata[1].len = nxids * sizeof(TransactionId);
	rdata[1].buffer = InvalidBuffer;
	rdata[1].next = NULL;

	(void) XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_CREATE_ID, rdata);

	/* Now enter the information into the OFFSETs and MEMBERs logs */
	RecordNewMultiXact(multi, offset, nxids, xids);

	/* Done with critical section */
	END_CRIT_SECTION();

	/* Store the new MultiXactId in the local cache, too */
	mXactCachePut(multi, nxids, xids);

	debug_elog2(DEBUG2, "Create: all done");

	return multi;
}

/*
 * RecordNewMultiXact
 *		Write info about a new multixact into the offsets and members files
 *
 * This is broken out of CreateMultiXactId so that xlog replay can use it.
 */
static void
RecordNewMultiXact(MultiXactId multi, MultiXactOffset offset,
				   int nxids, TransactionId *xids)
{
	int			pageno;
	int			prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	int			i;

	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	/*
	 * Note: we pass the MultiXactId to SimpleLruReadPage as the "transaction"
	 * to complain about if there's any I/O error.  This is kinda bogus, but
	 * since the errors will always give the full pathname, it should be clear
	 * enough that a MultiXactId is really involved.  Perhaps someday we'll
	 * take the trouble to generalize the slru.c error reporting code.
	 */
	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;

	*offptr = offset;

	MultiXactOffsetCtl->shared->page_dirty[slotno] = true;

	/* Exchange our lock */
	LWLockRelease(MultiXactOffsetControlLock);

	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	prev_pageno = -1;

	for (i = 0; i < nxids; i++, offset++)
	{
		TransactionId *memberptr;

		pageno = MXOffsetToMemberPage(offset);
		entryno = MXOffsetToMemberEntry(offset);

		if (pageno != prev_pageno)
		{
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, multi);
			prev_pageno = pageno;
		}

		memberptr = (TransactionId *)
			MultiXactMemberCtl->shared->page_buffer[slotno];
		memberptr += entryno;

		*memberptr = xids[i];

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactMemberControlLock);
}

/*
 * GetNewMultiXactId
 *		Get the next MultiXactId.
 *
 * Also, reserve the needed amount of space in the "members" area.	The
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
GetNewMultiXactId(int nxids, MultiXactOffset *offset)
{
	MultiXactId result;
	MultiXactOffset nextOffset;

	debug_elog3(DEBUG2, "GetNew: for %d xids", nxids);

	/* MultiXactIdSetOldestMember() must have been called already */
	Assert(MultiXactIdIsValid(OldestMemberMXactId[MyBackendId]));

	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

	/* Handle wraparound of the nextMXact counter */
	if (MultiXactState->nextMXact < FirstMultiXactId)
		MultiXactState->nextMXact = FirstMultiXactId;

	/*
	 * Assign the MXID, and make sure there is room for it in the file.
	 */
	result = MultiXactState->nextMXact;

	ExtendMultiXactOffset(result);

	/*
	 * Reserve the members space, similarly to above.  Also, be careful not to
	 * return zero as the starting offset for any multixact. See
	 * GetMultiXactIdMembers() for motivation.
	 */
	nextOffset = MultiXactState->nextOffset;
	if (nextOffset == 0)
	{
		*offset = 1;
		nxids++;				/* allocate member slot 0 too */
	}
	else
		*offset = nextOffset;

	ExtendMultiXactMember(nextOffset, nxids);

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
	 * the next iteration.	But note that nextMXact may be InvalidMultiXactId
	 * after this routine exits, so anyone else looking at the variable must
	 * be prepared to deal with that.  Similarly, nextOffset may be zero, but
	 * we won't use that as the actual start offset of the next multixact.
	 */
	(MultiXactState->nextMXact)++;

	MultiXactState->nextOffset += nxids;

	LWLockRelease(MultiXactGenLock);

	debug_elog4(DEBUG2, "GetNew: returning %u offset %u", result, *offset);
	return result;
}

/*
 * GetMultiXactIdMembers
 *		Returns the set of TransactionIds that make up a MultiXactId
 *
 * We return -1 if the MultiXactId is too old to possibly have any members
 * still running; in that case we have not actually looked them up, and
 * *xids is not set.
 */
int
GetMultiXactIdMembers(MultiXactId multi, TransactionId **xids)
{
	int			pageno;
	int			prev_pageno;
	int			entryno;
	int			slotno;
	MultiXactOffset *offptr;
	MultiXactOffset offset;
	int			length;
	int			truelength;
	int			i;
	MultiXactId nextMXact;
	MultiXactId tmpMXact;
	MultiXactOffset nextOffset;
	TransactionId *ptr;

	debug_elog3(DEBUG2, "GetMembers: asked for %u", multi);

	Assert(MultiXactIdIsValid(multi));

	/* See if the MultiXactId is in the local cache */
	length = mXactCacheGetById(multi, xids);
	if (length >= 0)
	{
		debug_elog3(DEBUG2, "GetMembers: found %s in the cache",
					mxid_to_string(multi, length, *xids));
		return length;
	}

	/* Set our OldestVisibleMXactId[] entry if we didn't already */
	MultiXactIdSetOldestVisible();

	/*
	 * We check known limits on MultiXact before resorting to the SLRU area.
	 *
	 * An ID older than our OldestVisibleMXactId[] entry can't possibly still
	 * be running, and we'd run the risk of trying to read already-truncated
	 * SLRU data if we did try to examine it.
	 *
	 * Conversely, an ID >= nextMXact shouldn't ever be seen here; if it is
	 * seen, it implies undetected ID wraparound has occurred.	We just
	 * silently assume that such an ID is no longer running.
	 *
	 * Shared lock is enough here since we aren't modifying any global state.
	 * Also, we can examine our own OldestVisibleMXactId without the lock,
	 * since no one else is allowed to change it.
	 */
	if (MultiXactIdPrecedes(multi, OldestVisibleMXactId[MyBackendId]))
	{
		debug_elog2(DEBUG2, "GetMembers: it's too old");
		*xids = NULL;
		return -1;
	}

	/*
	 * Acquire the shared lock just long enough to grab the current counter
	 * values.	We may need both nextMXact and nextOffset; see below.
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	nextMXact = MultiXactState->nextMXact;
	nextOffset = MultiXactState->nextOffset;

	LWLockRelease(MultiXactGenLock);

	if (!MultiXactIdPrecedes(multi, nextMXact))
	{
		debug_elog2(DEBUG2, "GetMembers: it's too new!");
		*xids = NULL;
		return -1;
	}

	/*
	 * Find out the offset at which we need to start reading MultiXactMembers
	 * and the number of members in the multixact.	We determine the latter as
	 * the difference between this multixact's starting offset and the next
	 * one's.  However, there are some corner cases to worry about:
	 *
	 * 1. This multixact may be the latest one created, in which case there is
	 * no next one to look at.	In this case the nextOffset value we just
	 * saved is the correct endpoint.
	 *
	 * 2. The next multixact may still be in process of being filled in: that
	 * is, another process may have done GetNewMultiXactId but not yet written
	 * the offset entry for that ID.  In that scenario, it is guaranteed that
	 * the offset entry for that multixact exists (because GetNewMultiXactId
	 * won't release MultiXactGenLock until it does) but contains zero
	 * (because we are careful to pre-zero offset pages). Because
	 * GetNewMultiXactId will never return zero as the starting offset for a
	 * multixact, when we read zero as the next multixact's offset, we know we
	 * have this case.	We sleep for a bit and try again.
	 *
	 * 3. Because GetNewMultiXactId increments offset zero to offset one to
	 * handle case #2, there is an ambiguity near the point of offset
	 * wraparound.	If we see next multixact's offset is one, is that our
	 * multixact's actual endpoint, or did it end at zero with a subsequent
	 * increment?  We handle this using the knowledge that if the zero'th
	 * member slot wasn't filled, it'll contain zero, and zero isn't a valid
	 * transaction ID so it can't be a multixact member.  Therefore, if we
	 * read a zero from the members array, just ignore it.
	 *
	 * This is all pretty messy, but the mess occurs only in infrequent corner
	 * cases, so it seems better than holding the MultiXactGenLock for a long
	 * time on every multixact creation.
	 */
retry:
	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	pageno = MultiXactIdToOffsetPage(multi);
	entryno = MultiXactIdToOffsetEntry(multi);

	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;
	offset = *offptr;

	Assert(offset != 0);

	/*
	 * Use the same increment rule as GetNewMultiXactId(), that is, don't
	 * handle wraparound explicitly until needed.
	 */
	tmpMXact = multi + 1;

	if (nextMXact == tmpMXact)
	{
		/* Corner case 1: there is no next multixact */
		length = nextOffset - offset;
	}
	else
	{
		MultiXactOffset nextMXOffset;

		/* handle wraparound if needed */
		if (tmpMXact < FirstMultiXactId)
			tmpMXact = FirstMultiXactId;

		prev_pageno = pageno;

		pageno = MultiXactIdToOffsetPage(tmpMXact);
		entryno = MultiXactIdToOffsetEntry(tmpMXact);

		if (pageno != prev_pageno)
			slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, tmpMXact);

		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;
		nextMXOffset = *offptr;

		if (nextMXOffset == 0)
		{
			/* Corner case 2: next multixact is still being filled in */
			LWLockRelease(MultiXactOffsetControlLock);
			pg_usleep(1000L);
			goto retry;
		}

		length = nextMXOffset - offset;
	}

	LWLockRelease(MultiXactOffsetControlLock);

	ptr = (TransactionId *) palloc(length * sizeof(TransactionId));
	*xids = ptr;

	/* Now get the members themselves. */
	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	truelength = 0;
	prev_pageno = -1;
	for (i = 0; i < length; i++, offset++)
	{
		TransactionId *xactptr;

		pageno = MXOffsetToMemberPage(offset);
		entryno = MXOffsetToMemberEntry(offset);

		if (pageno != prev_pageno)
		{
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, multi);
			prev_pageno = pageno;
		}

		xactptr = (TransactionId *)
			MultiXactMemberCtl->shared->page_buffer[slotno];
		xactptr += entryno;

		if (!TransactionIdIsValid(*xactptr))
		{
			/* Corner case 3: we must be looking at unused slot zero */
			Assert(offset == 0);
			continue;
		}

		ptr[truelength++] = *xactptr;
	}

	LWLockRelease(MultiXactMemberControlLock);

	/*
	 * Copy the result into the local cache.
	 */
	mXactCachePut(multi, truelength, ptr);

	debug_elog3(DEBUG2, "GetMembers: no cache for %s",
				mxid_to_string(multi, truelength, ptr));
	return truelength;
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
 * NB: the passed xids[] array will be sorted in-place.
 */
static MultiXactId
mXactCacheGetBySet(int nxids, TransactionId *xids)
{
	mXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CacheGet: looking for %s",
				mxid_to_string(InvalidMultiXactId, nxids, xids));

	/* sort the array so comparison is easy */
	qsort(xids, nxids, sizeof(TransactionId), xidComparator);

	for (entry = MXactCache; entry != NULL; entry = entry->next)
	{
		if (entry->nxids != nxids)
			continue;

		/* We assume the cache entries are sorted */
		if (memcmp(xids, entry->xids, nxids * sizeof(TransactionId)) == 0)
		{
			debug_elog3(DEBUG2, "CacheGet: found %u", entry->multi);
			return entry->multi;
		}
	}

	debug_elog2(DEBUG2, "CacheGet: not found :-(");
	return InvalidMultiXactId;
}

/*
 * mXactCacheGetById
 *		returns the composing TransactionId set from the cache for a
 *		given MultiXactId, if present.
 *
 * If successful, *xids is set to the address of a palloc'd copy of the
 * TransactionId set.  Return value is number of members, or -1 on failure.
 */
static int
mXactCacheGetById(MultiXactId multi, TransactionId **xids)
{
	mXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CacheGet: looking for %u", multi);

	for (entry = MXactCache; entry != NULL; entry = entry->next)
	{
		if (entry->multi == multi)
		{
			TransactionId *ptr;
			Size		size;

			size = sizeof(TransactionId) * entry->nxids;
			ptr = (TransactionId *) palloc(size);
			*xids = ptr;

			memcpy(ptr, entry->xids, size);

			debug_elog3(DEBUG2, "CacheGet: found %s",
						mxid_to_string(multi, entry->nxids, entry->xids));
			return entry->nxids;
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
mXactCachePut(MultiXactId multi, int nxids, TransactionId *xids)
{
	mXactCacheEnt *entry;

	debug_elog3(DEBUG2, "CachePut: storing %s",
				mxid_to_string(multi, nxids, xids));

	if (MXactContext == NULL)
	{
		/* The cache only lives as long as the current transaction */
		debug_elog2(DEBUG2, "CachePut: initializing memory context");
		MXactContext = AllocSetContextCreate(TopTransactionContext,
											 "MultiXact Cache Context",
											 ALLOCSET_SMALL_MINSIZE,
											 ALLOCSET_SMALL_INITSIZE,
											 ALLOCSET_SMALL_MAXSIZE);
	}

	entry = (mXactCacheEnt *)
		MemoryContextAlloc(MXactContext,
						   offsetof(mXactCacheEnt, xids) +
						   nxids * sizeof(TransactionId));

	entry->multi = multi;
	entry->nxids = nxids;
	memcpy(entry->xids, xids, nxids * sizeof(TransactionId));

	/* mXactCacheGetBySet assumes the entries are sorted, so sort them */
	qsort(entry->xids, nxids, sizeof(TransactionId), xidComparator);

	entry->next = MXactCache;
	MXactCache = entry;
}

/*
 * xidComparator
 *		qsort comparison function for XIDs
 *
 * We don't need to use wraparound comparison for XIDs, and indeed must
 * not do so since that does not respect the triangle inequality!  Any
 * old sort order will do.
 */
static int
xidComparator(const void *arg1, const void *arg2)
{
	TransactionId xid1 = *(const TransactionId *) arg1;
	TransactionId xid2 = *(const TransactionId *) arg2;

	if (xid1 > xid2)
		return 1;
	if (xid1 < xid2)
		return -1;
	return 0;
}

#ifdef MULTIXACT_DEBUG
static char *
mxid_to_string(MultiXactId multi, int nxids, TransactionId *xids)
{
	char	   *str = palloc(15 * (nxids + 1) + 4);
	int			i;

	snprintf(str, 47, "%u %d[%u", multi, nxids, xids[0]);

	for (i = 1; i < nxids; i++)
		snprintf(str + strlen(str), 17, ", %u", xids[i]);

	strcat(str, "]");
	return str;
}
#endif

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
	OldestMemberMXactId[MyBackendId] = InvalidMultiXactId;
	OldestVisibleMXactId[MyBackendId] = InvalidMultiXactId;

	/*
	 * Discard the local MultiXactId cache.  Since MXactContext was created as
	 * a child of TopTransactionContext, we needn't delete it explicitly.
	 */
	MXactContext = NULL;
	MXactCache = NULL;
}

/*
 * Initialization of shared memory for MultiXact.  We use two SLRU areas,
 * thus double memory.	Also, reserve space for the shared MultiXactState
 * struct and the per-backend MultiXactId arrays (two of those, too).
 */
Size
MultiXactShmemSize(void)
{
	Size		size;

#define SHARED_MULTIXACT_STATE_SIZE \
	add_size(sizeof(MultiXactStateData), \
			 mul_size(sizeof(MultiXactId) * 2, MaxBackends))

	size = SHARED_MULTIXACT_STATE_SIZE;
	size = add_size(size, SimpleLruShmemSize(NUM_MXACTOFFSET_BUFFERS));
	size = add_size(size, SimpleLruShmemSize(NUM_MXACTMEMBER_BUFFERS));

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
				  "MultiXactOffset Ctl", NUM_MXACTOFFSET_BUFFERS,
				  MultiXactOffsetControlLock, "pg_multixact/offsets");
	SimpleLruInit(MultiXactMemberCtl,
				  "MultiXactMember Ctl", NUM_MXACTMEMBER_BUFFERS,
				  MultiXactMemberControlLock, "pg_multixact/members");

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
	 * Set up array pointers.  Note that perBackendXactIds[0] is wasted space
	 * since we only use indexes 1..MaxBackends in each array.
	 */
	OldestMemberMXactId = MultiXactState->perBackendXactIds;
	OldestVisibleMXactId = OldestMemberMXactId + MaxBackends;
}

/*
 * This func must be called ONCE on system install.  It creates the initial
 * MultiXact segments.	(The MultiXacts directories are assumed to have been
 * created by initdb, and MultiXactShmemInit must have been called already.)
 */
void
BootStrapMultiXact(void)
{
	int			slotno;

	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the offsets log */
	slotno = ZeroMultiXactOffsetPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(MultiXactOffsetCtl, slotno, NULL);
	Assert(!MultiXactOffsetCtl->shared->page_dirty[slotno]);

	LWLockRelease(MultiXactOffsetControlLock);

	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the members log */
	slotno = ZeroMultiXactMemberPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(MultiXactMemberCtl, slotno, NULL);
	Assert(!MultiXactMemberCtl->shared->page_dirty[slotno]);

	LWLockRelease(MultiXactMemberControlLock);
}

/*
 * Initialize (or reinitialize) a page of MultiXactOffset to zeroes.
 * If writeXlog is TRUE, also emit an XLOG record saying we did this.
 *
 * The page is not actually written, just set up in shared memory.
 * The slot number of the new page is returned.
 *
 * Control lock must be held at entry, and will be held at exit.
 */
static int
ZeroMultiXactOffsetPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(MultiXactOffsetCtl, pageno);

	if (writeXlog)
		WriteMZeroPageXlogRec(pageno, XLOG_MULTIXACT_ZERO_OFF_PAGE);

	return slotno;
}

/*
 * Ditto, for MultiXactMember
 */
static int
ZeroMultiXactMemberPage(int pageno, bool writeXlog)
{
	int			slotno;

	slotno = SimpleLruZeroPage(MultiXactMemberCtl, pageno);

	if (writeXlog)
		WriteMZeroPageXlogRec(pageno, XLOG_MULTIXACT_ZERO_MEM_PAGE);

	return slotno;
}

/*
 * This must be called ONCE during postmaster or standalone-backend startup.
 *
 * StartupXLOG has already established nextMXact/nextOffset by calling
 * MultiXactSetNextMXact and/or MultiXactAdvanceNextMXact.	Note that we
 * may already have replayed WAL data into the SLRU files.
 *
 * We don't need any locks here, really; the SLRU locks are taken
 * only because slru.c expects to be called with locks held.
 */
void
StartupMultiXact(void)
{
	MultiXactId multi = MultiXactState->nextMXact;
	MultiXactOffset offset = MultiXactState->nextOffset;
	int			pageno;
	int			entryno;

	/* Clean up offsets state */
	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	/*
	 * Initialize our idea of the latest page number.
	 */
	pageno = MultiXactIdToOffsetPage(multi);
	MultiXactOffsetCtl->shared->latest_page_number = pageno;

	/*
	 * Zero out the remainder of the current offsets page.	See notes in
	 * StartupCLOG() for motivation.
	 */
	entryno = MultiXactIdToOffsetEntry(multi);
	if (entryno != 0)
	{
		int			slotno;
		MultiXactOffset *offptr;

		slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, multi);
		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;

		MemSet(offptr, 0, BLCKSZ - (entryno * sizeof(MultiXactOffset)));

		MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactOffsetControlLock);

	/* And the same for members */
	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	/*
	 * Initialize our idea of the latest page number.
	 */
	pageno = MXOffsetToMemberPage(offset);
	MultiXactMemberCtl->shared->latest_page_number = pageno;

	/*
	 * Zero out the remainder of the current members page.	See notes in
	 * StartupCLOG() for motivation.
	 */
	entryno = MXOffsetToMemberEntry(offset);
	if (entryno != 0)
	{
		int			slotno;
		TransactionId *xidptr;

		slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, offset);
		xidptr = (TransactionId *) MultiXactMemberCtl->shared->page_buffer[slotno];
		xidptr += entryno;

		MemSet(xidptr, 0, BLCKSZ - (entryno * sizeof(TransactionId)));

		MultiXactMemberCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactMemberControlLock);

	/*
	 * Initialize lastTruncationPoint to invalid, ensuring that the first
	 * checkpoint will try to do truncation.
	 */
	MultiXactState->lastTruncationPoint = InvalidMultiXactId;
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownMultiXact(void)
{
	/* Flush dirty MultiXact pages to disk */
	SimpleLruFlush(MultiXactOffsetCtl, false);
	SimpleLruFlush(MultiXactMemberCtl, false);
}

/*
 * Get the next MultiXactId and offset to save in a checkpoint record
 */
void
MultiXactGetCheckptMulti(bool is_shutdown,
						 MultiXactId *nextMulti,
						 MultiXactOffset *nextMultiOffset)
{
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	*nextMulti = MultiXactState->nextMXact;
	*nextMultiOffset = MultiXactState->nextOffset;

	LWLockRelease(MultiXactGenLock);

	debug_elog4(DEBUG2, "MultiXact: checkpoint is nextMulti %u, nextOffset %u",
				*nextMulti, *nextMultiOffset);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointMultiXact(void)
{
	/* Flush dirty MultiXact pages to disk */
	SimpleLruFlush(MultiXactOffsetCtl, true);
	SimpleLruFlush(MultiXactMemberCtl, true);

	/*
	 * Truncate the SLRU files.  This could be done at any time, but
	 * checkpoint seems a reasonable place for it.	There is one exception: if
	 * we are called during xlog recovery, then shared->latest_page_number
	 * isn't valid (because StartupMultiXact hasn't been called yet) and so
	 * SimpleLruTruncate would get confused.  It seems best not to risk
	 * removing any data during recovery anyway, so don't truncate.
	 */
	if (!InRecovery)
		TruncateMultiXact();
}

/*
 * Set the next-to-be-assigned MultiXactId and offset
 *
 * This is used when we can determine the correct next ID/offset exactly
 * from a checkpoint record.  We need no locking since it is only called
 * during bootstrap and XLog replay.
 */
void
MultiXactSetNextMXact(MultiXactId nextMulti,
					  MultiXactOffset nextMultiOffset)
{
	debug_elog4(DEBUG2, "MultiXact: setting next multi to %u offset %u",
				nextMulti, nextMultiOffset);
	MultiXactState->nextMXact = nextMulti;
	MultiXactState->nextOffset = nextMultiOffset;
}

/*
 * Ensure the next-to-be-assigned MultiXactId is at least minMulti,
 * and similarly nextOffset is at least minMultiOffset
 *
 * This is used when we can determine minimum safe values from an XLog
 * record (either an on-line checkpoint or an mxact creation log entry).
 * We need no locking since it is only called during XLog replay.
 */
void
MultiXactAdvanceNextMXact(MultiXactId minMulti,
						  MultiXactOffset minMultiOffset)
{
	if (MultiXactIdPrecedes(MultiXactState->nextMXact, minMulti))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next multi to %u", minMulti);
		MultiXactState->nextMXact = minMulti;
	}
	if (MultiXactOffsetPrecedes(MultiXactState->nextOffset, minMultiOffset))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next offset to %u",
					minMultiOffset);
		MultiXactState->nextOffset = minMultiOffset;
	}
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
	int			pageno;

	/*
	 * No work except at first MultiXactId of a page.  But beware: just after
	 * wraparound, the first MultiXactId of page zero is FirstMultiXactId.
	 */
	if (MultiXactIdToOffsetEntry(multi) != 0 &&
		multi != FirstMultiXactId)
		return;

	pageno = MultiXactIdToOffsetPage(multi);

	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	/* Zero the page and make an XLOG entry about it */
	ZeroMultiXactOffsetPage(pageno, true);

	LWLockRelease(MultiXactOffsetControlLock);
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
		int			entryno;

		/*
		 * Only zero when at first entry of a page.
		 */
		entryno = MXOffsetToMemberEntry(offset);
		if (entryno == 0)
		{
			int			pageno;

			pageno = MXOffsetToMemberPage(offset);

			LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

			/* Zero the page and make an XLOG entry about it */
			ZeroMultiXactMemberPage(pageno, true);

			LWLockRelease(MultiXactMemberControlLock);
		}

		/* Advance to next page (OK if nmembers goes negative) */
		offset += (MULTIXACT_MEMBERS_PER_PAGE - entryno);
		nmembers -= (MULTIXACT_MEMBERS_PER_PAGE - entryno);
	}
}

/*
 * Remove all MultiXactOffset and MultiXactMember segments before the oldest
 * ones still of interest.
 *
 * This is called only during checkpoints.	We assume no more than one
 * backend does this at a time.
 *
 * XXX do we have any issues with needing to checkpoint here?
 */
static void
TruncateMultiXact(void)
{
	MultiXactId nextMXact;
	MultiXactOffset nextOffset;
	MultiXactId oldestMXact;
	MultiXactOffset oldestOffset;
	int			cutoffPage;
	int			i;

	/*
	 * First, compute where we can safely truncate.  Per notes above, this is
	 * the oldest valid value among all the OldestMemberMXactId[] and
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
	for (i = 1; i <= MaxBackends; i++)
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

	/* Save the current nextOffset too */
	nextOffset = MultiXactState->nextOffset;

	LWLockRelease(MultiXactGenLock);

	debug_elog3(DEBUG2, "MultiXact: truncation point = %u", oldestMXact);

	/*
	 * If we already truncated at this point, do nothing.  This saves time
	 * when no MultiXacts are getting used, which is probably not uncommon.
	 */
	if (MultiXactState->lastTruncationPoint == oldestMXact)
		return;

	/*
	 * We need to determine where to truncate MultiXactMember.	If we found a
	 * valid oldest MultiXactId, read its starting offset; otherwise we use
	 * the nextOffset value we saved above.
	 */
	if (oldestMXact == nextMXact)
		oldestOffset = nextOffset;
	else
	{
		int			pageno;
		int			slotno;
		int			entryno;
		MultiXactOffset *offptr;

		/* lock is acquired by SimpleLruReadPage_ReadOnly */

		pageno = MultiXactIdToOffsetPage(oldestMXact);
		entryno = MultiXactIdToOffsetEntry(oldestMXact);

		slotno = SimpleLruReadPage_ReadOnly(MultiXactOffsetCtl, pageno, oldestMXact);
		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;
		oldestOffset = *offptr;

		LWLockRelease(MultiXactOffsetControlLock);
	}

	/*
	 * The cutoff point is the start of the segment containing oldestMXact. We
	 * pass the *page* containing oldestMXact to SimpleLruTruncate.
	 */
	cutoffPage = MultiXactIdToOffsetPage(oldestMXact);

	SimpleLruTruncate(MultiXactOffsetCtl, cutoffPage);

	/*
	 * Also truncate MultiXactMember at the previously determined offset.
	 */
	cutoffPage = MXOffsetToMemberPage(oldestOffset);

	SimpleLruTruncate(MultiXactMemberCtl, cutoffPage);

	/*
	 * Set the last known truncation point.  We don't need a lock for this
	 * since only one backend does checkpoints at a time.
	 */
	MultiXactState->lastTruncationPoint = oldestMXact;
}

/*
 * Decide which of two MultiXactOffset page numbers is "older" for truncation
 * purposes.
 *
 * We need to use comparison of MultiXactId here in order to do the right
 * thing with wraparound.  However, if we are asked about page number zero, we
 * don't want to hand InvalidMultiXactId to MultiXactIdPrecedes: it'll get
 * weird.  So, offset both multis by FirstMultiXactId to avoid that.
 * (Actually, the current implementation doesn't do anything weird with
 * InvalidMultiXactId, but there's no harm in leaving this code like this.)
 */
static bool
MultiXactOffsetPagePrecedes(int page1, int page2)
{
	MultiXactId multi1;
	MultiXactId multi2;

	multi1 = ((MultiXactId) page1) * MULTIXACT_OFFSETS_PER_PAGE;
	multi1 += FirstMultiXactId;
	multi2 = ((MultiXactId) page2) * MULTIXACT_OFFSETS_PER_PAGE;
	multi2 += FirstMultiXactId;

	return MultiXactIdPrecedes(multi1, multi2);
}

/*
 * Decide which of two MultiXactMember page numbers is "older" for truncation
 * purposes.  There is no "invalid offset number" so use the numbers verbatim.
 */
static bool
MultiXactMemberPagePrecedes(int page1, int page2)
{
	MultiXactOffset offset1;
	MultiXactOffset offset2;

	offset1 = ((MultiXactOffset) page1) * MULTIXACT_MEMBERS_PER_PAGE;
	offset2 = ((MultiXactOffset) page2) * MULTIXACT_MEMBERS_PER_PAGE;

	return MultiXactOffsetPrecedes(offset1, offset2);
}

/*
 * Decide which of two MultiXactIds is earlier.
 *
 * XXX do we need to do something special for InvalidMultiXactId?
 * (Doesn't look like it.)
 */
static bool
MultiXactIdPrecedes(MultiXactId multi1, MultiXactId multi2)
{
	int32		diff = (int32) (multi1 - multi2);

	return (diff < 0);
}

/*
 * Decide which of two offsets is earlier.
 */
static bool
MultiXactOffsetPrecedes(MultiXactOffset offset1, MultiXactOffset offset2)
{
	int32		diff = (int32) (offset1 - offset2);

	return (diff < 0);
}


/*
 * Write an xlog record reflecting the zeroing of either a MEMBERs or
 * OFFSETs page (info shows which)
 *
 * Note: xlog record is marked as outside transaction control, since we
 * want it to be redone whether the invoking transaction commits or not.
 */
static void
WriteMZeroPageXlogRec(int pageno, uint8 info)
{
	XLogRecData rdata;

	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	(void) XLogInsert(RM_MULTIXACT_ID, info | XLOG_NO_TRAN, &rdata);
}

/*
 * MULTIXACT resource manager's routines
 */
void
multixact_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

		slotno = ZeroMultiXactOffsetPage(pageno, false);
		SimpleLruWritePage(MultiXactOffsetCtl, slotno, NULL);
		Assert(!MultiXactOffsetCtl->shared->page_dirty[slotno]);

		LWLockRelease(MultiXactOffsetControlLock);
	}
	else if (info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

		slotno = ZeroMultiXactMemberPage(pageno, false);
		SimpleLruWritePage(MultiXactMemberCtl, slotno, NULL);
		Assert(!MultiXactMemberCtl->shared->page_dirty[slotno]);

		LWLockRelease(MultiXactMemberControlLock);
	}
	else if (info == XLOG_MULTIXACT_CREATE_ID)
	{
		xl_multixact_create *xlrec = (xl_multixact_create *) XLogRecGetData(record);
		TransactionId *xids = xlrec->xids;
		TransactionId max_xid;
		int			i;

		/* Store the data back into the SLRU files */
		RecordNewMultiXact(xlrec->mid, xlrec->moff, xlrec->nxids, xids);

		/* Make sure nextMXact/nextOffset are beyond what this record has */
		MultiXactAdvanceNextMXact(xlrec->mid + 1, xlrec->moff + xlrec->nxids);

		/*
		 * Make sure nextXid is beyond any XID mentioned in the record. This
		 * should be unnecessary, since any XID found here ought to have other
		 * evidence in the XLOG, but let's be safe.
		 */
		max_xid = record->xl_xid;
		for (i = 0; i < xlrec->nxids; i++)
		{
			if (TransactionIdPrecedes(max_xid, xids[i]))
				max_xid = xids[i];
		}
		if (TransactionIdFollowsOrEquals(max_xid,
										 ShmemVariableCache->nextXid))
		{
			ShmemVariableCache->nextXid = max_xid;
			TransactionIdAdvance(ShmemVariableCache->nextXid);
		}
	}
	else
		elog(PANIC, "multixact_redo: unknown op code %u", info);
}

void
multixact_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		appendStringInfo(buf, "zero offsets page: %d", pageno);
	}
	else if (info == XLOG_MULTIXACT_ZERO_MEM_PAGE)
	{
		int			pageno;

		memcpy(&pageno, rec, sizeof(int));
		appendStringInfo(buf, "zero members page: %d", pageno);
	}
	else if (info == XLOG_MULTIXACT_CREATE_ID)
	{
		xl_multixact_create *xlrec = (xl_multixact_create *) rec;
		int			i;

		appendStringInfo(buf, "create multixact %u offset %u:",
						 xlrec->mid, xlrec->moff);
		for (i = 0; i < xlrec->nxids; i++)
			appendStringInfo(buf, " %u", xlrec->xids[i]);
	}
	else
		appendStringInfo(buf, "UNKNOWN");
}
