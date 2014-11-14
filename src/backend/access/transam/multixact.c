/*-------------------------------------------------------------------------
 *
 * multixact.c
 *		PostgreSQL multi-transaction-log manager
 *
 * The pg_multixact manager is a pg_clog-like manager that stores an array of
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
 * memory.  At checkpoint time, after the value is known flushed in WAL, any
 * files that correspond to multixacts older than that value are removed.
 * (These files are also removed when a restartpoint is executed.)
 *
 * When new multixactid values are to be created, care is taken that the
 * counter does not fall within the wraparound horizon considering the global
 * minimum value.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/multixact.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/multixact.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "funcapi.h"
#include "lib/ilist.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "postmaster/autovacuum.h"
#include "storage/lmgr.h"
#include "storage/pmsignal.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/*
 * Defines for MultiXactOffset page sizes.  A page is the same BLCKSZ as is
 * used everywhere else in Postgres.
 *
 * Note: because MultiXactOffsets are 32 bits and wrap around at 0xFFFFFFFF,
 * MultiXact page numbering also wraps around at
 * 0xFFFFFFFF/MULTIXACT_OFFSETS_PER_PAGE, and segment numbering at
 * 0xFFFFFFFF/MULTIXACT_OFFSETS_PER_PAGE/SLRU_PAGES_PER_SEGMENT.  We need
 * take no explicit notice of that fact in this module, except when comparing
 * segment and page numbers in TruncateMultiXact (see
 * MultiXactOffsetPagePrecedes).
 */

/* We need four bytes per offset */
#define MULTIXACT_OFFSETS_PER_PAGE (BLCKSZ / sizeof(MultiXactOffset))

#define MultiXactIdToOffsetPage(xid) \
	((xid) / (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)
#define MultiXactIdToOffsetEntry(xid) \
	((xid) % (MultiXactOffset) MULTIXACT_OFFSETS_PER_PAGE)

/*
 * The situation for members is a bit more complex: we store one byte of
 * additional flag bits for each TransactionId.  To do this without getting
 * into alignment issues, we store four bytes of flags, and then the
 * corresponding 4 Xids.  Each such 5-word (20-byte) set we call a "group", and
 * are stored as a whole in pages.  Thus, with 8kB BLCKSZ, we keep 409 groups
 * per page.  This wastes 12 bytes per page, but that's OK -- simplicity (and
 * performance) trumps space efficiency here.
 *
 * Note that the "offset" macros work with byte offset, not array indexes, so
 * arithmetic must be done using "char *" pointers.
 */
/* We need eight bits per xact, so one xact fits in a byte */
#define MXACT_MEMBER_BITS_PER_XACT			8
#define MXACT_MEMBER_FLAGS_PER_BYTE			1
#define MXACT_MEMBER_XACT_BITMASK	((1 << MXACT_MEMBER_BITS_PER_XACT) - 1)

/* how many full bytes of flags are there in a group? */
#define MULTIXACT_FLAGBYTES_PER_GROUP		4
#define MULTIXACT_MEMBERS_PER_MEMBERGROUP	\
	(MULTIXACT_FLAGBYTES_PER_GROUP * MXACT_MEMBER_FLAGS_PER_BYTE)
/* size in bytes of a complete group */
#define MULTIXACT_MEMBERGROUP_SIZE \
	(sizeof(TransactionId) * MULTIXACT_MEMBERS_PER_MEMBERGROUP + MULTIXACT_FLAGBYTES_PER_GROUP)
#define MULTIXACT_MEMBERGROUPS_PER_PAGE (BLCKSZ / MULTIXACT_MEMBERGROUP_SIZE)
#define MULTIXACT_MEMBERS_PER_PAGE	\
	(MULTIXACT_MEMBERGROUPS_PER_PAGE * MULTIXACT_MEMBERS_PER_MEMBERGROUP)

/*
 * Because the number of items per page is not a divisor of the last item
 * number (member 0xFFFFFFFF), the last segment does not use the maximum number
 * of pages, and moreover the last used page therein does not use the same
 * number of items as previous pages.  (Another way to say it is that the
 * 0xFFFFFFFF member is somewhere in the middle of the last page, so the page
 * has some empty space after that item.)
 *
 * This constant is the number of members in the last page of the last segment.
 */
#define MAX_MEMBERS_IN_LAST_MEMBERS_PAGE \
		((uint32) ((0xFFFFFFFF % MULTIXACT_MEMBERS_PER_PAGE) + 1))

/* page in which a member is to be found */
#define MXOffsetToMemberPage(xid) ((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_PAGE)

/* Location (byte offset within page) of flag word for a given member */
#define MXOffsetToFlagsOffset(xid) \
	((((xid) / (TransactionId) MULTIXACT_MEMBERS_PER_MEMBERGROUP) % \
	  (TransactionId) MULTIXACT_MEMBERGROUPS_PER_PAGE) * \
	 (TransactionId) MULTIXACT_MEMBERGROUP_SIZE)
#define MXOffsetToFlagsBitShift(xid) \
	(((xid) % (TransactionId) MULTIXACT_MEMBERS_PER_MEMBERGROUP) * \
	 MXACT_MEMBER_BITS_PER_XACT)

/* Location (byte offset within page) of TransactionId of given member */
#define MXOffsetToMemberOffset(xid) \
	(MXOffsetToFlagsOffset(xid) + MULTIXACT_FLAGBYTES_PER_GROUP + \
	 ((xid) % MULTIXACT_MEMBERS_PER_MEMBERGROUP) * sizeof(TransactionId))


/*
 * Links to shared-memory data structures for MultiXact control
 */
static SlruCtlData MultiXactOffsetCtlData;
static SlruCtlData MultiXactMemberCtlData;

#define MultiXactOffsetCtl	(&MultiXactOffsetCtlData)
#define MultiXactMemberCtl	(&MultiXactMemberCtlData)

/*
 * MultiXact state shared across all backends.  All this state is protected
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

	/*
	 * Oldest multixact that is still on disk.  Anything older than this
	 * should not be consulted.  These values are updated by vacuum.
	 */
	MultiXactId oldestMultiXactId;
	Oid			oldestMultiXactDB;

	/*
	 * This is what the previous checkpoint stored as the truncate position.
	 * This value is the oldestMultiXactId that was valid when a checkpoint
	 * was last executed.
	 */
	MultiXactId lastCheckpointedOldest;

	/* support for anti-wraparound measures */
	MultiXactId multiVacLimit;
	MultiXactId multiWarnLimit;
	MultiXactId multiStopLimit;
	MultiXactId multiWrapLimit;

	/*
	 * Per-backend data starts here.  We have two arrays stored in the area
	 * immediately following the MultiXactStateData struct. Each is indexed by
	 * BackendId.
	 *
	 * In both arrays, there's a slot for all normal backends (1..MaxBackends)
	 * followed by a slot for max_prepared_xacts prepared transactions. Valid
	 * BackendIds start from 1; element zero of each array is never used.
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
	 * The oldest valid value among all of the OldestMemberMXactId[] and
	 * OldestVisibleMXactId[] entries is considered by vacuum as the earliest
	 * possible value still having any live member transaction.  Subtracting
	 * vacuum_multixact_freeze_min_age from that value we obtain the freezing
	 * point for multixacts for that table.  Any value older than that is
	 * removed from tuple headers (or "frozen"; see FreezeMultiXactId.  Note
	 * that multis that have member xids that are older than the cutoff point
	 * for xids must also be frozen, even if the multis themselves are newer
	 * than the multixid cutoff point).  Whenever a full table vacuum happens,
	 * the freezing point so computed is used as the new pg_class.relminmxid
	 * value.  The minimum of all those values in a database is stored as
	 * pg_database.datminmxid.  In turn, the minimum of all of those values is
	 * stored in pg_control and used as truncation point for pg_multixact.  At
	 * checkpoint or restartpoint, unneeded segments are removed.
	 */
	MultiXactId perBackendXactIds[1];	/* VARIABLE LENGTH ARRAY */
} MultiXactStateData;

/*
 * Last element of OldestMemberMXactID and OldestVisibleMXactId arrays.
 * Valid elements are (1..MaxOldestSlot); element 0 is never used.
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
static dlist_head MXactCache = DLIST_STATIC_INIT(MXactCache);
static int	MXactCacheMembers = 0;
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

static char *mxstatus_to_string(MultiXactStatus status);

/* management of SLRU infrastructure */
static int	ZeroMultiXactOffsetPage(int pageno, bool writeXlog);
static int	ZeroMultiXactMemberPage(int pageno, bool writeXlog);
static bool MultiXactOffsetPagePrecedes(int page1, int page2);
static bool MultiXactMemberPagePrecedes(int page1, int page2);
static bool MultiXactOffsetPrecedes(MultiXactOffset offset1,
						MultiXactOffset offset2);
static void ExtendMultiXactOffset(MultiXactId multi);
static void ExtendMultiXactMember(MultiXactOffset offset, int nmembers);
static void WriteMZeroPageXlogRec(int pageno, uint8 info);


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

	AssertArg(TransactionIdIsValid(xid1));
	AssertArg(TransactionIdIsValid(xid2));

	Assert(!TransactionIdEquals(xid1, xid2) || (status1 != status2));

	/* MultiXactIdSetOldestMember() must have been called already. */
	Assert(MultiXactIdIsValid(OldestMemberMXactId[MyBackendId]));

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

	AssertArg(MultiXactIdIsValid(multi));
	AssertArg(TransactionIdIsValid(xid));

	/* MultiXactIdSetOldestMember() must have been called already. */
	Assert(MultiXactIdIsValid(OldestMemberMXactId[MyBackendId]));

	debug_elog5(DEBUG2, "Expand: received multi %u, xid %u status %s",
				multi, xid, mxstatus_to_string(status));

	/*
	 * Note: we don't allow for old multis here.  The reason is that the only
	 * caller of this function does a check that the multixact is no longer
	 * running.
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false);

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
MultiXactIdIsRunning(MultiXactId multi)
{
	MultiXactMember *members;
	int			nmembers;
	int			i;

	debug_elog3(DEBUG2, "IsRunning %u?", multi);

	/*
	 * "false" here means we assume our callers have checked that the given
	 * multi cannot possibly come from a pg_upgraded database.
	 */
	nmembers = GetMultiXactIdMembers(multi, &members, false);

	if (nmembers < 0)
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
 * an OldestMemberMXactId[] entry older than what we compute here.  Therefore
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

		for (i = 1; i <= MaxOldestSlot; i++)
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
	XLogRecData rdata[2];
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
					elog(ERROR, "new multixact has more than one updating member");
				has_update = true;
			}
		}
	}

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

	/*
	 * Make an XLOG entry describing the new MXID.
	 *
	 * Note: we need not flush this XLOG entry to disk before proceeding. The
	 * only way for the MXID to be referenced from any data page is for
	 * heap_lock_tuple() to have put it there, and heap_lock_tuple() generates
	 * an XLOG record that must follow ours.  The normal LSN interlock between
	 * the data page and that XLOG record will ensure that our XLOG record
	 * reaches disk first.  If the SLRU members/offsets data reaches disk
	 * sooner than the XLOG record, we do not care because we'll overwrite it
	 * with zeroes unless the XLOG record is there too; see notes at top of
	 * this file.
	 */
	xlrec.mid = multi;
	xlrec.moff = offset;
	xlrec.nmembers = nmembers;

	/*
	 * XXX Note: there's a lot of padding space in MultiXactMember.  We could
	 * find a more compact representation of this Xlog record -- perhaps all
	 * the status flags in one XLogRecData, then all the xids in another one?
	 * Not clear that it's worth the trouble though.
	 */
	rdata[0].data = (char *) (&xlrec);
	rdata[0].len = SizeOfMultiXactCreate;
	rdata[0].buffer = InvalidBuffer;
	rdata[0].next = &(rdata[1]);

	rdata[1].data = (char *) members;
	rdata[1].len = nmembers * sizeof(MultiXactMember);
	rdata[1].buffer = InvalidBuffer;
	rdata[1].next = NULL;

	(void) XLogInsert(RM_MULTIXACT_ID, XLOG_MULTIXACT_CREATE_ID, rdata);

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
	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
	offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
	offptr += entryno;

	*offptr = offset;

	MultiXactOffsetCtl->shared->page_dirty[slotno] = true;

	/* Exchange our lock */
	LWLockRelease(MultiXactOffsetControlLock);

	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	prev_pageno = -1;

	for (i = 0; i < nmembers; i++, offset++)
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

	LWLockRelease(MultiXactMemberControlLock);
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

	/* safety check, we should never get this far in a HS slave */
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
	 * If we're past multiVacLimit, start trying to force autovacuum cycles.
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

		/*
		 * To avoid swamping the postmaster with signals, we issue the autovac
		 * request only once per 64K transaction starts.  This still gives
		 * plenty of chances before we get into real trouble.
		 */
		if (IsUnderPostmaster && (result % 65536) == 0)
			SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

		if (IsUnderPostmaster &&
			!MultiXactIdPrecedes(result, multiStopLimit))
		{
			char	   *oldest_datname = get_database_name(oldest_datoid);

			/* complain even if that DB has disappeared */
			if (oldest_datname)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands that generate new MultiXactIds to avoid wraparound data loss in database \"%s\"",
								oldest_datname),
				 errhint("Execute a database-wide VACUUM in that database.\n"
						 "You might also need to commit or roll back old prepared transactions.")));
			else
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("database is not accepting commands that generate new MultiXactIds to avoid wraparound data loss in database with OID %u",
								oldest_datoid),
				 errhint("Execute a database-wide VACUUM in that database.\n"
						 "You might also need to commit or roll back old prepared transactions.")));
		}
		else if (!MultiXactIdPrecedes(result, multiWarnLimit))
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
						 "You might also need to commit or roll back old prepared transactions.")));
			else
				ereport(WARNING,
						(errmsg_plural("database with OID %u must be vacuumed before %u more MultiXactId is used",
									   "database with OID %u must be vacuumed before %u more MultiXactIds are used",
									   multiWrapLimit - result,
									   oldest_datoid,
									   multiWrapLimit - result),
				 errhint("Execute a database-wide VACUUM in that database.\n"
						 "You might also need to commit or roll back old prepared transactions.")));
		}

		/* Re-acquire lock and start over */
		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
		result = MultiXactState->nextMXact;
		if (result < FirstMultiXactId)
			result = FirstMultiXactId;
	}

	/* Make sure there is room for the MXID in the file.  */
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
		nmembers++;				/* allocate member slot 0 too */
	}
	else
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
	 * with either case.  Similarly, nextOffset may be zero, but we won't use
	 * that as the actual start offset of the next multixact.
	 */
	(MultiXactState->nextMXact)++;

	MultiXactState->nextOffset += nmembers;

	LWLockRelease(MultiXactGenLock);

	debug_elog4(DEBUG2, "GetNew: returning %u offset %u", result, *offset);
	return result;
}

/*
 * GetMultiXactIdMembers
 *		Returns the set of MultiXactMembers that make up a MultiXactId
 *
 * If the given MultiXactId is older than the value we know to be oldest, we
 * return -1.  The caller is expected to allow that only in permissible cases,
 * i.e. when the infomask lets it presuppose that the tuple had been
 * share-locked before a pg_upgrade; this means that the HEAP_XMAX_LOCK_ONLY
 * needs to be set, but HEAP_XMAX_KEYSHR_LOCK and HEAP_XMAX_EXCL_LOCK are not
 * set.
 *
 * Other border conditions, such as trying to read a value that's larger than
 * the value currently known as the next to assign, raise an error.  Previously
 * these also returned -1, but since this can lead to the wrong visibility
 * results, it is dangerous to do that.
 */
int
GetMultiXactIdMembers(MultiXactId multi, MultiXactMember **members,
					  bool allow_old)
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
	MultiXactId oldestMXact;
	MultiXactId nextMXact;
	MultiXactId tmpMXact;
	MultiXactOffset nextOffset;
	MultiXactMember *ptr;

	debug_elog3(DEBUG2, "GetMembers: asked for %u", multi);

	if (!MultiXactIdIsValid(multi))
		return -1;

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
	 * We check known limits on MultiXact before resorting to the SLRU area.
	 *
	 * An ID older than MultiXactState->oldestMultiXactId cannot possibly be
	 * useful; it has already been removed, or will be removed shortly, by
	 * truncation.  Returning the wrong values could lead
	 * to an incorrect visibility result.  However, to support pg_upgrade we
	 * need to allow an empty set to be returned regardless, if the caller is
	 * willing to accept it; the caller is expected to check that it's an
	 * allowed condition (such as ensuring that the infomask bits set on the
	 * tuple are consistent with the pg_upgrade scenario).  If the caller is
	 * expecting this to be called only on recently created multis, then we
	 * raise an error.
	 *
	 * Conversely, an ID >= nextMXact shouldn't ever be seen here; if it is
	 * seen, it implies undetected ID wraparound has occurred.  This raises a
	 * hard error.
	 *
	 * Shared lock is enough here since we aren't modifying any global state.
	 * Acquire it just long enough to grab the current counter values.  We may
	 * need both nextMXact and nextOffset; see below.
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);

	oldestMXact = MultiXactState->oldestMultiXactId;
	nextMXact = MultiXactState->nextMXact;
	nextOffset = MultiXactState->nextOffset;

	LWLockRelease(MultiXactGenLock);

	if (MultiXactIdPrecedes(multi, oldestMXact))
	{
		ereport(allow_old ? DEBUG1 : ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
		 errmsg("MultiXactId %u does no longer exist -- apparent wraparound",
				multi)));
		return -1;
	}

	if (!MultiXactIdPrecedes(multi, nextMXact))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("MultiXactId %u has not been created yet -- apparent wraparound",
						multi)));

	/*
	 * Find out the offset at which we need to start reading MultiXactMembers
	 * and the number of members in the multixact.  We determine the latter as
	 * the difference between this multixact's starting offset and the next
	 * one's.  However, there are some corner cases to worry about:
	 *
	 * 1. This multixact may be the latest one created, in which case there is
	 * no next one to look at.  In this case the nextOffset value we just
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
	 * have this case.  We sleep for a bit and try again.
	 *
	 * 3. Because GetNewMultiXactId increments offset zero to offset one to
	 * handle case #2, there is an ambiguity near the point of offset
	 * wraparound.  If we see next multixact's offset is one, is that our
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

	slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
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
			slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, tmpMXact);

		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;
		nextMXOffset = *offptr;

		if (nextMXOffset == 0)
		{
			/* Corner case 2: next multixact is still being filled in */
			LWLockRelease(MultiXactOffsetControlLock);
			CHECK_FOR_INTERRUPTS();
			pg_usleep(1000L);
			goto retry;
		}

		length = nextMXOffset - offset;
	}

	LWLockRelease(MultiXactOffsetControlLock);

	ptr = (MultiXactMember *) palloc(length * sizeof(MultiXactMember));
	*members = ptr;

	/* Now get the members themselves. */
	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	truelength = 0;
	prev_pageno = -1;
	for (i = 0; i < length; i++, offset++)
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
			slotno = SimpleLruReadPage(MultiXactMemberCtl, pageno, true, multi);
			prev_pageno = pageno;
		}

		xactptr = (TransactionId *)
			(MultiXactMemberCtl->shared->page_buffer[slotno] + memberoff);

		if (!TransactionIdIsValid(*xactptr))
		{
			/* Corner case 3: we must be looking at unused slot zero */
			Assert(offset == 0);
			continue;
		}

		flagsoff = MXOffsetToFlagsOffset(offset);
		bshift = MXOffsetToFlagsBitShift(offset);
		flagsptr = (uint32 *) (MultiXactMemberCtl->shared->page_buffer[slotno] + flagsoff);

		ptr[truelength].xid = *xactptr;
		ptr[truelength].status = (*flagsptr >> bshift) & MXACT_MEMBER_XACT_BITMASK;
		truelength++;
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
 * MultiXactHasRunningRemoteMembers
 *		Does the given multixact have still-live members from
 *		transactions other than our own?
 */
bool
MultiXactHasRunningRemoteMembers(MultiXactId multi)
{
	MultiXactMember *members;
	int			nmembers;
	int			i;

	nmembers = GetMultiXactIdMembers(multi, &members, true);
	if (nmembers <= 0)
		return false;

	for (i = 0; i < nmembers; i++)
	{
		/* not interested in our own members */
		if (TransactionIdIsCurrentTransactionId(members[i].xid))
			continue;

		if (TransactionIdIsInProgress(members[i].xid))
		{
			pfree(members);
			return true;
		}
	}

	pfree(members);
	return false;
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

	dlist_foreach(iter, &MXactCache)
	{
		mXactCacheEnt *entry = dlist_container(mXactCacheEnt, node, iter.cur);

		if (entry->nmembers != nmembers)
			continue;

		/*
		 * We assume the cache entries are sorted, and that the unused bits in
		 * "status" are zeroed.
		 */
		if (memcmp(members, entry->members, nmembers * sizeof(MultiXactMember)) == 0)
		{
			debug_elog3(DEBUG2, "CacheGet: found %u", entry->multi);
			dlist_move_head(&MXactCache, iter.cur);
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

	dlist_foreach(iter, &MXactCache)
	{
		mXactCacheEnt *entry = dlist_container(mXactCacheEnt, node, iter.cur);

		if (entry->multi == multi)
		{
			MultiXactMember *ptr;
			Size		size;

			size = sizeof(MultiXactMember) * entry->nmembers;
			ptr = (MultiXactMember *) palloc(size);
			*members = ptr;

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
			dlist_move_head(&MXactCache, iter.cur);

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
											 "MultiXact Cache Context",
											 ALLOCSET_SMALL_MINSIZE,
											 ALLOCSET_SMALL_INITSIZE,
											 ALLOCSET_SMALL_MAXSIZE);
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

	dlist_push_head(&MXactCache, &entry->node);
	if (MXactCacheMembers++ >= MAX_CACHE_ENTRIES)
	{
		dlist_node *node;
		mXactCacheEnt *entry;

		node = dlist_tail_node(&MXactCache);
		dlist_delete(node);
		MXactCacheMembers--;

		entry = dlist_container(mXactCacheEnt, node, node);
		debug_elog3(DEBUG2, "CachePut: pruning cached multi %u",
					entry->multi);

		pfree(entry);
	}
}

static char *
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
	OldestMemberMXactId[MyBackendId] = InvalidMultiXactId;
	OldestVisibleMXactId[MyBackendId] = InvalidMultiXactId;

	/*
	 * Discard the local MultiXactId cache.  Since MXactContext was created as
	 * a child of TopTransactionContext, we needn't delete it explicitly.
	 */
	MXactContext = NULL;
	dlist_init(&MXactCache);
	MXactCacheMembers = 0;
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
	MultiXactId myOldestMember = OldestMemberMXactId[MyBackendId];

	if (MultiXactIdIsValid(myOldestMember))
		RegisterTwoPhaseRecord(TWOPHASE_RM_MULTIXACT_ID, 0,
							   &myOldestMember, sizeof(MultiXactId));
}

/*
 * PostPrepare_MultiXact
 *		Clean up after successful PREPARE TRANSACTION
 */
void
PostPrepare_MultiXact(TransactionId xid)
{
	MultiXactId myOldestMember;

	/*
	 * Transfer our OldestMemberMXactId value to the slot reserved for the
	 * prepared transaction.
	 */
	myOldestMember = OldestMemberMXactId[MyBackendId];
	if (MultiXactIdIsValid(myOldestMember))
	{
		BackendId	dummyBackendId = TwoPhaseGetDummyBackendId(xid);

		/*
		 * Even though storing MultiXactId is atomic, acquire lock to make
		 * sure others see both changes, not just the reset of the slot of the
		 * current backend. Using a volatile pointer might suffice, but this
		 * isn't a hot spot.
		 */
		LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);

		OldestMemberMXactId[dummyBackendId] = myOldestMember;
		OldestMemberMXactId[MyBackendId] = InvalidMultiXactId;

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
	OldestVisibleMXactId[MyBackendId] = InvalidMultiXactId;

	/*
	 * Discard the local MultiXactId cache like in AtEOX_MultiXact
	 */
	MXactContext = NULL;
	dlist_init(&MXactCache);
	MXactCacheMembers = 0;
}

/*
 * multixact_twophase_recover
 *		Recover the state of a prepared transaction at startup
 */
void
multixact_twophase_recover(TransactionId xid, uint16 info,
						   void *recdata, uint32 len)
{
	BackendId	dummyBackendId = TwoPhaseGetDummyBackendId(xid);
	MultiXactId oldestMember;

	/*
	 * Get the oldest member XID from the state file record, and set it in the
	 * OldestMemberMXactId slot reserved for this prepared transaction.
	 */
	Assert(len == sizeof(MultiXactId));
	oldestMember = *((MultiXactId *) recdata);

	OldestMemberMXactId[dummyBackendId] = oldestMember;
}

/*
 * multixact_twophase_postcommit
 *		Similar to AtEOX_MultiXact but for COMMIT PREPARED
 */
void
multixact_twophase_postcommit(TransactionId xid, uint16 info,
							  void *recdata, uint32 len)
{
	BackendId	dummyBackendId = TwoPhaseGetDummyBackendId(xid);

	Assert(len == sizeof(MultiXactId));

	OldestMemberMXactId[dummyBackendId] = InvalidMultiXactId;
}

/*
 * multixact_twophase_postabort
 *		This is actually just the same as the COMMIT case.
 */
void
multixact_twophase_postabort(TransactionId xid, uint16 info,
							 void *recdata, uint32 len)
{
	multixact_twophase_postcommit(xid, info, recdata, len);
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

#define SHARED_MULTIXACT_STATE_SIZE \
	add_size(sizeof(MultiXactStateData), \
			 mul_size(sizeof(MultiXactId) * 2, MaxOldestSlot))

	size = SHARED_MULTIXACT_STATE_SIZE;
	size = add_size(size, SimpleLruShmemSize(NUM_MXACTOFFSET_BUFFERS, 0));
	size = add_size(size, SimpleLruShmemSize(NUM_MXACTMEMBER_BUFFERS, 0));

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
				  "MultiXactOffset Ctl", NUM_MXACTOFFSET_BUFFERS, 0,
				  MultiXactOffsetControlLock, "pg_multixact/offsets");
	SimpleLruInit(MultiXactMemberCtl,
				  "MultiXactMember Ctl", NUM_MXACTMEMBER_BUFFERS, 0,
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
	 * since we only use indexes 1..MaxOldestSlot in each array.
	 */
	OldestMemberMXactId = MultiXactState->perBackendXactIds;
	OldestVisibleMXactId = OldestMemberMXactId + MaxOldestSlot;
}

/*
 * This func must be called ONCE on system install.  It creates the initial
 * MultiXact segments.  (The MultiXacts directories are assumed to have been
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
	SimpleLruWritePage(MultiXactOffsetCtl, slotno);
	Assert(!MultiXactOffsetCtl->shared->page_dirty[slotno]);

	LWLockRelease(MultiXactOffsetControlLock);

	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	/* Create and zero the first page of the members log */
	slotno = ZeroMultiXactMemberPage(0, false);

	/* Make sure it's written out */
	SimpleLruWritePage(MultiXactMemberCtl, slotno);
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
 * MaybeExtendOffsetSlru
 *		Extend the offsets SLRU area, if necessary
 *
 * After a binary upgrade from <= 9.2, the pg_multixact/offset SLRU area might
 * contain files that are shorter than necessary; this would occur if the old
 * installation had used multixacts beyond the first page (files cannot be
 * copied, because the on-disk representation is different).  pg_upgrade would
 * update pg_control to set the next offset value to be at that position, so
 * that tuples marked as locked by such MultiXacts would be seen as visible
 * without having to consult multixact.  However, trying to create and use a
 * new MultiXactId would result in an error because the page on which the new
 * value would reside does not exist.  This routine is in charge of creating
 * such pages.
 */
static void
MaybeExtendOffsetSlru(void)
{
	int			pageno;

	pageno = MultiXactIdToOffsetPage(MultiXactState->nextMXact);

	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	if (!SimpleLruDoesPhysicalPageExist(MultiXactOffsetCtl, pageno))
	{
		int			slotno;

		/*
		 * Fortunately for us, SimpleLruWritePage is already prepared to deal
		 * with creating a new segment file even if the page we're writing is
		 * not the first in it, so this is enough.
		 */
		slotno = ZeroMultiXactOffsetPage(pageno, false);
		SimpleLruWritePage(MultiXactOffsetCtl, slotno);
	}

	LWLockRelease(MultiXactOffsetControlLock);
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
	int			pageno;

	/*
	 * Initialize offset's idea of the latest page number.
	 */
	pageno = MultiXactIdToOffsetPage(multi);
	MultiXactOffsetCtl->shared->latest_page_number = pageno;

	/*
	 * Initialize member's idea of the latest page number.
	 */
	pageno = MXOffsetToMemberPage(offset);
	MultiXactMemberCtl->shared->latest_page_number = pageno;
}

/*
 * This must be called ONCE at the end of startup/recovery.
 *
 * We don't need any locks here, really; the SLRU locks are taken only because
 * slru.c expects to be called with locks held.
 */
void
TrimMultiXact(void)
{
	MultiXactId multi = MultiXactState->nextMXact;
	MultiXactOffset offset = MultiXactState->nextOffset;
	int			pageno;
	int			entryno;
	int			flagsoff;

	/*
	 * During a binary upgrade, make sure that the offsets SLRU is large
	 * enough to contain the next value that would be created. It's fine to do
	 * this here and not in StartupMultiXact() since binary upgrades should
	 * never need crash recovery.
	 */
	if (IsBinaryUpgrade)
		MaybeExtendOffsetSlru();

	/* Clean up offsets state */
	LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

	/*
	 * (Re-)Initialize our idea of the latest page number for offsets.
	 */
	pageno = MultiXactIdToOffsetPage(multi);
	MultiXactOffsetCtl->shared->latest_page_number = pageno;

	/*
	 * Zero out the remainder of the current offsets page.  See notes in
	 * TrimCLOG() for motivation.
	 */
	entryno = MultiXactIdToOffsetEntry(multi);
	if (entryno != 0)
	{
		int			slotno;
		MultiXactOffset *offptr;

		slotno = SimpleLruReadPage(MultiXactOffsetCtl, pageno, true, multi);
		offptr = (MultiXactOffset *) MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;

		MemSet(offptr, 0, BLCKSZ - (entryno * sizeof(MultiXactOffset)));

		MultiXactOffsetCtl->shared->page_dirty[slotno] = true;
	}

	LWLockRelease(MultiXactOffsetControlLock);

	/* And the same for members */
	LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

	/*
	 * (Re-)Initialize our idea of the latest page number for members.
	 */
	pageno = MXOffsetToMemberPage(offset);
	MultiXactMemberCtl->shared->latest_page_number = pageno;

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
	}

	LWLockRelease(MultiXactMemberControlLock);
}

/*
 * This must be called ONCE during postmaster or standalone-backend shutdown
 */
void
ShutdownMultiXact(void)
{
	/* Flush dirty MultiXact pages to disk */
	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_START(false);
	SimpleLruFlush(MultiXactOffsetCtl, false);
	SimpleLruFlush(MultiXactMemberCtl, false);
	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_DONE(false);
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
				"MultiXact: checkpoint is nextMulti %u, nextOffset %u, oldestMulti %u in DB %u",
				*nextMulti, *nextMultiOffset, *oldestMulti, *oldestMultiDB);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 */
void
CheckPointMultiXact(void)
{
	TRACE_POSTGRESQL_MULTIXACT_CHECKPOINT_START(true);

	/* Flush dirty MultiXact pages to disk */
	SimpleLruFlush(MultiXactOffsetCtl, true);
	SimpleLruFlush(MultiXactMemberCtl, true);

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
	debug_elog4(DEBUG2, "MultiXact: setting next multi to %u offset %u",
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
	 * Since multixacts wrap differently from transaction IDs, this logic is
	 * not entirely correct: in some scenarios we could go for longer than 2
	 * billion multixacts without seeing any data loss, and in some others we
	 * could get in trouble before that if the new pg_multixact/members data
	 * stomps on the previous cycle's data.  For lack of a better mechanism we
	 * use the same logic as for transaction IDs, that is, start taking action
	 * halfway around the oldest potentially-existing multixact.
	 */
	multiWrapLimit = oldest_datminmxid + (MaxMultiXactId >> 1);
	if (multiWrapLimit < FirstMultiXactId)
		multiWrapLimit += FirstMultiXactId;

	/*
	 * We'll refuse to continue assigning MultiXactIds once we get within 100
	 * multi of data loss.
	 *
	 * Note: This differs from the magic number used in
	 * SetTransactionIdLimit() since vacuum itself will never generate new
	 * multis.
	 */
	multiStopLimit = multiWrapLimit - 100;
	if (multiStopLimit < FirstMultiXactId)
		multiStopLimit -= FirstMultiXactId;

	/*
	 * We'll start complaining loudly when we get within 10M multis of the
	 * stop point.   This is kind of arbitrary, but if you let your gas gauge
	 * get down to 1% of full, would you be looking for the next gas station?
	 * We need to be fairly liberal about this number because there are lots
	 * of scenarios where most transactions are done by automatic clients that
	 * won't pay attention to warnings. (No, we're not gonna make this
	 * configurable.  If you know enough to configure it, you know enough to
	 * not get in this kind of trouble in the first place.)
	 */
	multiWarnLimit = multiStopLimit - 10000000;
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
	 (errmsg("MultiXactId wrap limit is %u, limited by database with OID %u",
			 multiWrapLimit, oldest_datoid)));

	/*
	 * If past the autovacuum force point, immediately signal an autovac
	 * request.  The reason for this is that autovac only processes one
	 * database per invocation.  Once it's finished cleaning up the oldest
	 * database, it'll call here, and we'll signal the postmaster to start
	 * another iteration immediately if there are still any old databases.
	 */
	if (MultiXactIdPrecedes(multiVacLimit, curMulti) &&
		IsUnderPostmaster && !InRecovery)
		SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_LAUNCHER);

	/* Give an immediate warning if past the wrap warn point */
	if (MultiXactIdPrecedes(multiWarnLimit, curMulti) && !InRecovery)
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
					 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions.")));
		else
			ereport(WARNING,
					(errmsg_plural("database with OID %u must be vacuumed before %u more MultiXactId is used",
								   "database with OID %u must be vacuumed before %u more MultiXactIds are used",
								   multiWrapLimit - curMulti,
								   oldest_datoid,
								   multiWrapLimit - curMulti),
					 errhint("To avoid a database shutdown, execute a database-wide VACUUM in that database.\n"
							 "You might also need to commit or roll back old prepared transactions.")));
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
	if (MultiXactOffsetPrecedes(MultiXactState->nextOffset, minMultiOffset))
	{
		debug_elog3(DEBUG2, "MultiXact: setting next offset to %u",
					minMultiOffset);
		MultiXactState->nextOffset = minMultiOffset;
	}
	LWLockRelease(MultiXactGenLock);
}

/*
 * Update our oldestMultiXactId value, but only if it's more recent than
 * what we had.
 */
void
MultiXactAdvanceOldest(MultiXactId oldestMulti, Oid oldestMultiDB)
{
	if (MultiXactIdPrecedes(MultiXactState->oldestMultiXactId, oldestMulti))
		SetMultiXactIdLimit(oldestMulti, oldestMultiDB);
}

/*
 * Update the "safe truncation point".  This is the newest value of oldestMulti
 * that is known to be flushed as part of a checkpoint record.
 */
void
MultiXactSetSafeTruncate(MultiXactId safeTruncateMulti)
{
	LWLockAcquire(MultiXactGenLock, LW_EXCLUSIVE);
	MultiXactState->lastCheckpointedOldest = safeTruncateMulti;
	LWLockRelease(MultiXactGenLock);
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
			int			pageno;

			pageno = MXOffsetToMemberPage(offset);

			LWLockAcquire(MultiXactMemberControlLock, LW_EXCLUSIVE);

			/* Zero the page and make an XLOG entry about it */
			ZeroMultiXactMemberPage(pageno, true);

			LWLockRelease(MultiXactMemberControlLock);
		}

		/*
		 * Compute the number of items till end of current page.  Careful: if
		 * addition of unsigned ints wraps around, we're at the last page of
		 * the last segment; since that page holds a different number of items
		 * than other pages, we need to do it differently.
		 */
		if (offset + MAX_MEMBERS_IN_LAST_MEMBERS_PAGE < offset)
		{
			/*
			 * This is the last page of the last segment; we can compute the
			 * number of items left to allocate in it without modulo
			 * arithmetic.
			 */
			difference = MaxMultiXactOffset - offset + 1;
		}
		else
			difference = MULTIXACT_MEMBERS_PER_PAGE - offset % MULTIXACT_MEMBERS_PER_PAGE;

		/*
		 * Advance to next page, taking care to properly handle the wraparound
		 * case.  OK if nmembers goes negative.
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
 * this function; however, it can be used by a full-table vacuum to set the
 * point at which it will be possible to truncate SLRU for that table.
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
	for (i = 1; i <= MaxOldestSlot; i++)
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
 * SlruScanDirectory callback.
 *		This callback deletes segments that are outside the range determined by
 *		the given page numbers.
 *
 * Both range endpoints are exclusive (that is, segments containing any of
 * those pages are kept.)
 */
typedef struct MembersLiveRange
{
	int			rangeStart;
	int			rangeEnd;
} MembersLiveRange;

static bool
SlruScanDirCbRemoveMembers(SlruCtl ctl, char *filename, int segpage,
						   void *data)
{
	MembersLiveRange *range = (MembersLiveRange *) data;
	MultiXactOffset nextOffset;

	if ((segpage == range->rangeStart) ||
		(segpage == range->rangeEnd))
		return false;			/* easy case out */

	/*
	 * To ensure that no segment is spuriously removed, we must keep track of
	 * new segments added since the start of the directory scan; to do this,
	 * we update our end-of-range point as we run.
	 *
	 * As an optimization, we can skip looking at shared memory if we know for
	 * certain that the current segment must be kept.  This is so because
	 * nextOffset never decreases, and we never increase rangeStart during any
	 * one run.
	 */
	if (!((range->rangeStart > range->rangeEnd &&
		   segpage > range->rangeEnd && segpage < range->rangeStart) ||
		  (range->rangeStart < range->rangeEnd &&
		   (segpage < range->rangeStart || segpage > range->rangeEnd))))
		return false;

	/*
	 * Update our idea of the end of the live range.
	 */
	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	nextOffset = MultiXactState->nextOffset;
	LWLockRelease(MultiXactGenLock);
	range->rangeEnd = MXOffsetToMemberPage(nextOffset);

	/* Recheck the deletion condition.  If it still holds, perform deletion */
	if ((range->rangeStart > range->rangeEnd &&
		 segpage > range->rangeEnd && segpage < range->rangeStart) ||
		(range->rangeStart < range->rangeEnd &&
		 (segpage < range->rangeStart || segpage > range->rangeEnd)))
		SlruDeleteSegment(ctl, filename);

	return false;				/* keep going */
}

typedef struct mxtruncinfo
{
	int			earliestExistingPage;
} mxtruncinfo;

/*
 * SlruScanDirectory callback
 *		This callback determines the earliest existing page number.
 */
static bool
SlruScanDirCbFindEarliest(SlruCtl ctl, char *filename, int segpage, void *data)
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
 * Remove all MultiXactOffset and MultiXactMember segments before the oldest
 * ones still of interest.
 *
 * On a primary, this is called by the checkpointer process after a checkpoint
 * has been flushed; during crash recovery, it's called from
 * CreateRestartPoint().  In the latter case, we rely on the fact that
 * xlog_redo() will already have called MultiXactAdvanceOldest().  Our
 * latest_page_number will already have been initialized by StartupMultiXact()
 * and kept up to date as new pages are zeroed.
 */
void
TruncateMultiXact(void)
{
	MultiXactId		oldestMXact;
	MultiXactOffset oldestOffset;
	MultiXactOffset nextOffset;
	mxtruncinfo trunc;
	MultiXactId earliest;
	MembersLiveRange range;

	Assert(AmCheckpointerProcess() || AmStartupProcess() ||
		   !IsPostmasterEnvironment);

	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	oldestMXact = MultiXactState->lastCheckpointedOldest;
	LWLockRelease(MultiXactGenLock);
	Assert(MultiXactIdIsValid(oldestMXact));

	/*
	 * Note we can't just plow ahead with the truncation; it's possible that
	 * there are no segments to truncate, which is a problem because we are
	 * going to attempt to read the offsets page to determine where to
	 * truncate the members SLRU.  So we first scan the directory to determine
	 * the earliest offsets page number that we can read without error.
	 */
	trunc.earliestExistingPage = -1;
	SlruScanDirectory(MultiXactOffsetCtl, SlruScanDirCbFindEarliest, &trunc);
	earliest = trunc.earliestExistingPage * MULTIXACT_OFFSETS_PER_PAGE;
	if (earliest < FirstMultiXactId)
		earliest = FirstMultiXactId;

	/* nothing to do */
	if (MultiXactIdPrecedes(oldestMXact, earliest))
		return;

	/*
	 * First, compute the safe truncation point for MultiXactMember. This is
	 * the starting offset of the oldest multixact.
	 */
	{
		int			pageno;
		int			slotno;
		int			entryno;
		MultiXactOffset *offptr;

		/* lock is acquired by SimpleLruReadPage_ReadOnly */

		pageno = MultiXactIdToOffsetPage(oldestMXact);
		entryno = MultiXactIdToOffsetEntry(oldestMXact);

		slotno = SimpleLruReadPage_ReadOnly(MultiXactOffsetCtl, pageno,
											oldestMXact);
		offptr = (MultiXactOffset *)
			MultiXactOffsetCtl->shared->page_buffer[slotno];
		offptr += entryno;
		oldestOffset = *offptr;

		LWLockRelease(MultiXactOffsetControlLock);
	}

	/*
	 * To truncate MultiXactMembers, we need to figure out the active page
	 * range and delete all files outside that range.  The start point is the
	 * start of the segment containing the oldest offset; an end point of the
	 * segment containing the next offset to use is enough.  The end point is
	 * updated as MultiXactMember gets extended concurrently, elsewhere.
	 */
	range.rangeStart = MXOffsetToMemberPage(oldestOffset);
	range.rangeStart -= range.rangeStart % SLRU_PAGES_PER_SEGMENT;

	LWLockAcquire(MultiXactGenLock, LW_SHARED);
	nextOffset = MultiXactState->nextOffset;
	LWLockRelease(MultiXactGenLock);

	range.rangeEnd = MXOffsetToMemberPage(nextOffset);

	SlruScanDirectory(MultiXactMemberCtl, SlruScanDirCbRemoveMembers, &range);

	/* Now we can truncate MultiXactOffset */
	SimpleLruTruncate(MultiXactOffsetCtl,
					  MultiXactIdToOffsetPage(oldestMXact));

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
 */
static void
WriteMZeroPageXlogRec(int pageno, uint8 info)
{
	XLogRecData rdata;

	rdata.data = (char *) (&pageno);
	rdata.len = sizeof(int);
	rdata.buffer = InvalidBuffer;
	rdata.next = NULL;
	(void) XLogInsert(RM_MULTIXACT_ID, info, &rdata);
}

/*
 * MULTIXACT resource manager's routines
 */
void
multixact_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	/* Backup blocks are not used in multixact records */
	Assert(!(record->xl_info & XLR_BKP_BLOCK_MASK));

	if (info == XLOG_MULTIXACT_ZERO_OFF_PAGE)
	{
		int			pageno;
		int			slotno;

		memcpy(&pageno, XLogRecGetData(record), sizeof(int));

		LWLockAcquire(MultiXactOffsetControlLock, LW_EXCLUSIVE);

		slotno = ZeroMultiXactOffsetPage(pageno, false);
		SimpleLruWritePage(MultiXactOffsetCtl, slotno);
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
		SimpleLruWritePage(MultiXactMemberCtl, slotno);
		Assert(!MultiXactMemberCtl->shared->page_dirty[slotno]);

		LWLockRelease(MultiXactMemberControlLock);
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
		max_xid = record->xl_xid;
		for (i = 0; i < xlrec->nmembers; i++)
		{
			if (TransactionIdPrecedes(max_xid, xlrec->members[i].xid))
				max_xid = xlrec->members[i].xid;
		}

		/*
		 * We don't expect anyone else to modify nextXid, hence startup
		 * process doesn't need to hold a lock while checking this. We still
		 * acquire the lock to modify it, though.
		 */
		if (TransactionIdFollowsOrEquals(max_xid,
										 ShmemVariableCache->nextXid))
		{
			LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
			ShmemVariableCache->nextXid = max_xid;
			TransactionIdAdvance(ShmemVariableCache->nextXid);
			LWLockRelease(XidGenLock);
		}
	}
	else
		elog(PANIC, "multixact_redo: unknown op code %u", info);
}

Datum
pg_get_multixact_members(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		MultiXactMember *members;
		int			nmembers;
		int			iter;
	} mxact;
	MultiXactId mxid = PG_GETARG_UINT32(0);
	mxact	   *multi;
	FuncCallContext *funccxt;

	if (mxid < FirstMultiXactId)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid MultiXactId: %u", mxid)));

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcxt;
		TupleDesc	tupdesc;

		funccxt = SRF_FIRSTCALL_INIT();
		oldcxt = MemoryContextSwitchTo(funccxt->multi_call_memory_ctx);

		multi = palloc(sizeof(mxact));
		/* no need to allow for old values here */
		multi->nmembers = GetMultiXactIdMembers(mxid, &multi->members, false);
		multi->iter = 0;

		tupdesc = CreateTemplateTupleDesc(2, false);
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "xid",
						   XIDOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "mode",
						   TEXTOID, -1, 0);

		funccxt->attinmeta = TupleDescGetAttInMetadata(tupdesc);
		funccxt->user_fctx = multi;

		MemoryContextSwitchTo(oldcxt);
	}

	funccxt = SRF_PERCALL_SETUP();
	multi = (mxact *) funccxt->user_fctx;

	while (multi->iter < multi->nmembers)
	{
		HeapTuple	tuple;
		char	   *values[2];

		values[0] = psprintf("%u", multi->members[multi->iter].xid);
		values[1] = mxstatus_to_string(multi->members[multi->iter].status);

		tuple = BuildTupleFromCStrings(funccxt->attinmeta, values);

		multi->iter++;
		pfree(values[0]);
		SRF_RETURN_NEXT(funccxt, HeapTupleGetDatum(tuple));
	}

	if (multi->nmembers > 0)
		pfree(multi->members);
	pfree(multi);

	SRF_RETURN_DONE(funccxt);
}
