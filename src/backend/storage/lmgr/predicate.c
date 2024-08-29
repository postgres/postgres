/*-------------------------------------------------------------------------
 *
 * predicate.c
 *	  POSTGRES predicate locking
 *	  to support full serializable transaction isolation
 *
 *
 * The approach taken is to implement Serializable Snapshot Isolation (SSI)
 * as initially described in this paper:
 *
 *	Michael J. Cahill, Uwe Röhm, and Alan D. Fekete. 2008.
 *	Serializable isolation for snapshot databases.
 *	In SIGMOD '08: Proceedings of the 2008 ACM SIGMOD
 *	international conference on Management of data,
 *	pages 729-738, New York, NY, USA. ACM.
 *	http://doi.acm.org/10.1145/1376616.1376690
 *
 * and further elaborated in Cahill's doctoral thesis:
 *
 *	Michael James Cahill. 2009.
 *	Serializable Isolation for Snapshot Databases.
 *	Sydney Digital Theses.
 *	University of Sydney, School of Information Technologies.
 *	http://hdl.handle.net/2123/5353
 *
 *
 * Predicate locks for Serializable Snapshot Isolation (SSI) are SIREAD
 * locks, which are so different from normal locks that a distinct set of
 * structures is required to handle them.  They are needed to detect
 * rw-conflicts when the read happens before the write.  (When the write
 * occurs first, the reading transaction can check for a conflict by
 * examining the MVCC data.)
 *
 * (1)	Besides tuples actually read, they must cover ranges of tuples
 *		which would have been read based on the predicate.  This will
 *		require modelling the predicates through locks against database
 *		objects such as pages, index ranges, or entire tables.
 *
 * (2)	They must be kept in RAM for quick access.  Because of this, it
 *		isn't possible to always maintain tuple-level granularity -- when
 *		the space allocated to store these approaches exhaustion, a
 *		request for a lock may need to scan for situations where a single
 *		transaction holds many fine-grained locks which can be coalesced
 *		into a single coarser-grained lock.
 *
 * (3)	They never block anything; they are more like flags than locks
 *		in that regard; although they refer to database objects and are
 *		used to identify rw-conflicts with normal write locks.
 *
 * (4)	While they are associated with a transaction, they must survive
 *		a successful COMMIT of that transaction, and remain until all
 *		overlapping transactions complete.  This even means that they
 *		must survive termination of the transaction's process.  If a
 *		top level transaction is rolled back, however, it is immediately
 *		flagged so that it can be ignored, and its SIREAD locks can be
 *		released any time after that.
 *
 * (5)	The only transactions which create SIREAD locks or check for
 *		conflicts with them are serializable transactions.
 *
 * (6)	When a write lock for a top level transaction is found to cover
 *		an existing SIREAD lock for the same transaction, the SIREAD lock
 *		can be deleted.
 *
 * (7)	A write from a serializable transaction must ensure that an xact
 *		record exists for the transaction, with the same lifespan (until
 *		all concurrent transaction complete or the transaction is rolled
 *		back) so that rw-dependencies to that transaction can be
 *		detected.
 *
 * We use an optimization for read-only transactions. Under certain
 * circumstances, a read-only transaction's snapshot can be shown to
 * never have conflicts with other transactions.  This is referred to
 * as a "safe" snapshot (and one known not to be is "unsafe").
 * However, it can't be determined whether a snapshot is safe until
 * all concurrent read/write transactions complete.
 *
 * Once a read-only transaction is known to have a safe snapshot, it
 * can release its predicate locks and exempt itself from further
 * predicate lock tracking. READ ONLY DEFERRABLE transactions run only
 * on safe snapshots, waiting as necessary for one to be available.
 *
 *
 * Lightweight locks to manage access to the predicate locking shared
 * memory objects must be taken in this order, and should be released in
 * reverse order:
 *
 *	SerializableFinishedListLock
 *		- Protects the list of transactions which have completed but which
 *			may yet matter because they overlap still-active transactions.
 *
 *	SerializablePredicateListLock
 *		- Protects the linked list of locks held by a transaction.  Note
 *			that the locks themselves are also covered by the partition
 *			locks of their respective lock targets; this lock only affects
 *			the linked list connecting the locks related to a transaction.
 *		- All transactions share this single lock (with no partitioning).
 *		- There is never a need for a process other than the one running
 *			an active transaction to walk the list of locks held by that
 *			transaction, except parallel query workers sharing the leader's
 *			transaction.  In the parallel case, an extra per-sxact lock is
 *			taken; see below.
 *		- It is relatively infrequent that another process needs to
 *			modify the list for a transaction, but it does happen for such
 *			things as index page splits for pages with predicate locks and
 *			freeing of predicate locked pages by a vacuum process.  When
 *			removing a lock in such cases, the lock itself contains the
 *			pointers needed to remove it from the list.  When adding a
 *			lock in such cases, the lock can be added using the anchor in
 *			the transaction structure.  Neither requires walking the list.
 *		- Cleaning up the list for a terminated transaction is sometimes
 *			not done on a retail basis, in which case no lock is required.
 *		- Due to the above, a process accessing its active transaction's
 *			list always uses a shared lock, regardless of whether it is
 *			walking or maintaining the list.  This improves concurrency
 *			for the common access patterns.
 *		- A process which needs to alter the list of a transaction other
 *			than its own active transaction must acquire an exclusive
 *			lock.
 *
 *	SERIALIZABLEXACT's member 'perXactPredicateListLock'
 *		- Protects the linked list of predicate locks held by a transaction.
 *			Only needed for parallel mode, where multiple backends share the
 *			same SERIALIZABLEXACT object.  Not needed if
 *			SerializablePredicateListLock is held exclusively.
 *
 *	PredicateLockHashPartitionLock(hashcode)
 *		- The same lock protects a target, all locks on that target, and
 *			the linked list of locks on the target.
 *		- When more than one is needed, acquire in ascending address order.
 *		- When all are needed (rare), acquire in ascending index order with
 *			PredicateLockHashPartitionLockByIndex(index).
 *
 *	SerializableXactHashLock
 *		- Protects both PredXact and SerializableXidHash.
 *
 *	SerialControlLock
 *		- Protects SerialControlData members
 *
 *	SLRU per-bank locks
 *		- Protects SerialSlruCtl
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/lmgr/predicate.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 * housekeeping for setting up shared memory predicate lock structures
 *		PredicateLockShmemInit(void)
 *		PredicateLockShmemSize(void)
 *
 * predicate lock reporting
 *		GetPredicateLockStatusData(void)
 *		PageIsPredicateLocked(Relation relation, BlockNumber blkno)
 *
 * predicate lock maintenance
 *		GetSerializableTransactionSnapshot(Snapshot snapshot)
 *		SetSerializableTransactionSnapshot(Snapshot snapshot,
 *										   VirtualTransactionId *sourcevxid)
 *		RegisterPredicateLockingXid(void)
 *		PredicateLockRelation(Relation relation, Snapshot snapshot)
 *		PredicateLockPage(Relation relation, BlockNumber blkno,
 *						Snapshot snapshot)
 *		PredicateLockTID(Relation relation, ItemPointer tid, Snapshot snapshot,
 *						 TransactionId tuple_xid)
 *		PredicateLockPageSplit(Relation relation, BlockNumber oldblkno,
 *							   BlockNumber newblkno)
 *		PredicateLockPageCombine(Relation relation, BlockNumber oldblkno,
 *								 BlockNumber newblkno)
 *		TransferPredicateLocksToHeapRelation(Relation relation)
 *		ReleasePredicateLocks(bool isCommit, bool isReadOnlySafe)
 *
 * conflict detection (may also trigger rollback)
 *		CheckForSerializableConflictOut(Relation relation, TransactionId xid,
 *										Snapshot snapshot)
 *		CheckForSerializableConflictIn(Relation relation, ItemPointer tid,
 *									   BlockNumber blkno)
 *		CheckTableForSerializableConflictIn(Relation relation)
 *
 * final rollback checking
 *		PreCommit_CheckForSerializationFailure(void)
 *
 * two-phase commit support
 *		AtPrepare_PredicateLocks(void);
 *		PostPrepare_PredicateLocks(TransactionId xid);
 *		PredicateLockTwoPhaseFinish(TransactionId xid, bool isCommit);
 *		predicatelock_twophase_recover(TransactionId xid, uint16 info,
 *									   void *recdata, uint32 len);
 */

#include "postgres.h"

#include "access/parallel.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "port/pg_lfind.h"
#include "storage/predicate.h"
#include "storage/predicate_internals.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/guc_hooks.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* Uncomment the next line to test the graceful degradation code. */
/* #define TEST_SUMMARIZE_SERIAL */

/*
 * Test the most selective fields first, for performance.
 *
 * a is covered by b if all of the following hold:
 *	1) a.database = b.database
 *	2) a.relation = b.relation
 *	3) b.offset is invalid (b is page-granularity or higher)
 *	4) either of the following:
 *		4a) a.offset is valid (a is tuple-granularity) and a.page = b.page
 *	 or 4b) a.offset is invalid and b.page is invalid (a is
 *			page-granularity and b is relation-granularity
 */
#define TargetTagIsCoveredBy(covered_target, covering_target)			\
	((GET_PREDICATELOCKTARGETTAG_RELATION(covered_target) == /* (2) */	\
	  GET_PREDICATELOCKTARGETTAG_RELATION(covering_target))				\
	 && (GET_PREDICATELOCKTARGETTAG_OFFSET(covering_target) ==			\
		 InvalidOffsetNumber)								 /* (3) */	\
	 && (((GET_PREDICATELOCKTARGETTAG_OFFSET(covered_target) !=			\
		   InvalidOffsetNumber)								 /* (4a) */ \
		  && (GET_PREDICATELOCKTARGETTAG_PAGE(covering_target) ==		\
			  GET_PREDICATELOCKTARGETTAG_PAGE(covered_target)))			\
		 || ((GET_PREDICATELOCKTARGETTAG_PAGE(covering_target) ==		\
			  InvalidBlockNumber)							 /* (4b) */ \
			 && (GET_PREDICATELOCKTARGETTAG_PAGE(covered_target)		\
				 != InvalidBlockNumber)))								\
	 && (GET_PREDICATELOCKTARGETTAG_DB(covered_target) ==	 /* (1) */	\
		 GET_PREDICATELOCKTARGETTAG_DB(covering_target)))

/*
 * The predicate locking target and lock shared hash tables are partitioned to
 * reduce contention.  To determine which partition a given target belongs to,
 * compute the tag's hash code with PredicateLockTargetTagHashCode(), then
 * apply one of these macros.
 * NB: NUM_PREDICATELOCK_PARTITIONS must be a power of 2!
 */
#define PredicateLockHashPartition(hashcode) \
	((hashcode) % NUM_PREDICATELOCK_PARTITIONS)
#define PredicateLockHashPartitionLock(hashcode) \
	(&MainLWLockArray[PREDICATELOCK_MANAGER_LWLOCK_OFFSET + \
		PredicateLockHashPartition(hashcode)].lock)
#define PredicateLockHashPartitionLockByIndex(i) \
	(&MainLWLockArray[PREDICATELOCK_MANAGER_LWLOCK_OFFSET + (i)].lock)

#define NPREDICATELOCKTARGETENTS() \
	mul_size(max_predicate_locks_per_xact, add_size(MaxBackends, max_prepared_xacts))

#define SxactIsOnFinishedList(sxact) (!dlist_node_is_detached(&(sxact)->finishedLink))

/*
 * Note that a sxact is marked "prepared" once it has passed
 * PreCommit_CheckForSerializationFailure, even if it isn't using
 * 2PC. This is the point at which it can no longer be aborted.
 *
 * The PREPARED flag remains set after commit, so SxactIsCommitted
 * implies SxactIsPrepared.
 */
#define SxactIsCommitted(sxact) (((sxact)->flags & SXACT_FLAG_COMMITTED) != 0)
#define SxactIsPrepared(sxact) (((sxact)->flags & SXACT_FLAG_PREPARED) != 0)
#define SxactIsRolledBack(sxact) (((sxact)->flags & SXACT_FLAG_ROLLED_BACK) != 0)
#define SxactIsDoomed(sxact) (((sxact)->flags & SXACT_FLAG_DOOMED) != 0)
#define SxactIsReadOnly(sxact) (((sxact)->flags & SXACT_FLAG_READ_ONLY) != 0)
#define SxactHasSummaryConflictIn(sxact) (((sxact)->flags & SXACT_FLAG_SUMMARY_CONFLICT_IN) != 0)
#define SxactHasSummaryConflictOut(sxact) (((sxact)->flags & SXACT_FLAG_SUMMARY_CONFLICT_OUT) != 0)
/*
 * The following macro actually means that the specified transaction has a
 * conflict out *to a transaction which committed ahead of it*.  It's hard
 * to get that into a name of a reasonable length.
 */
#define SxactHasConflictOut(sxact) (((sxact)->flags & SXACT_FLAG_CONFLICT_OUT) != 0)
#define SxactIsDeferrableWaiting(sxact) (((sxact)->flags & SXACT_FLAG_DEFERRABLE_WAITING) != 0)
#define SxactIsROSafe(sxact) (((sxact)->flags & SXACT_FLAG_RO_SAFE) != 0)
#define SxactIsROUnsafe(sxact) (((sxact)->flags & SXACT_FLAG_RO_UNSAFE) != 0)
#define SxactIsPartiallyReleased(sxact) (((sxact)->flags & SXACT_FLAG_PARTIALLY_RELEASED) != 0)

/*
 * Compute the hash code associated with a PREDICATELOCKTARGETTAG.
 *
 * To avoid unnecessary recomputations of the hash code, we try to do this
 * just once per function, and then pass it around as needed.  Aside from
 * passing the hashcode to hash_search_with_hash_value(), we can extract
 * the lock partition number from the hashcode.
 */
#define PredicateLockTargetTagHashCode(predicatelocktargettag) \
	get_hash_value(PredicateLockTargetHash, predicatelocktargettag)

/*
 * Given a predicate lock tag, and the hash for its target,
 * compute the lock hash.
 *
 * To make the hash code also depend on the transaction, we xor the sxid
 * struct's address into the hash code, left-shifted so that the
 * partition-number bits don't change.  Since this is only a hash, we
 * don't care if we lose high-order bits of the address; use an
 * intermediate variable to suppress cast-pointer-to-int warnings.
 */
#define PredicateLockHashCodeFromTargetHashCode(predicatelocktag, targethash) \
	((targethash) ^ ((uint32) PointerGetDatum((predicatelocktag)->myXact)) \
	 << LOG2_NUM_PREDICATELOCK_PARTITIONS)


/*
 * The SLRU buffer area through which we access the old xids.
 */
static SlruCtlData SerialSlruCtlData;

#define SerialSlruCtl			(&SerialSlruCtlData)

#define SERIAL_PAGESIZE			BLCKSZ
#define SERIAL_ENTRYSIZE			sizeof(SerCommitSeqNo)
#define SERIAL_ENTRIESPERPAGE	(SERIAL_PAGESIZE / SERIAL_ENTRYSIZE)

/*
 * Set maximum pages based on the number needed to track all transactions.
 */
#define SERIAL_MAX_PAGE			(MaxTransactionId / SERIAL_ENTRIESPERPAGE)

#define SerialNextPage(page) (((page) >= SERIAL_MAX_PAGE) ? 0 : (page) + 1)

#define SerialValue(slotno, xid) (*((SerCommitSeqNo *) \
	(SerialSlruCtl->shared->page_buffer[slotno] + \
	((((uint32) (xid)) % SERIAL_ENTRIESPERPAGE) * SERIAL_ENTRYSIZE))))

#define SerialPage(xid)	(((uint32) (xid)) / SERIAL_ENTRIESPERPAGE)

typedef struct SerialControlData
{
	int64		headPage;		/* newest initialized page */
	TransactionId headXid;		/* newest valid Xid in the SLRU */
	TransactionId tailXid;		/* oldest xmin we might be interested in */
}			SerialControlData;

typedef struct SerialControlData *SerialControl;

static SerialControl serialControl;

/*
 * When the oldest committed transaction on the "finished" list is moved to
 * SLRU, its predicate locks will be moved to this "dummy" transaction,
 * collapsing duplicate targets.  When a duplicate is found, the later
 * commitSeqNo is used.
 */
static SERIALIZABLEXACT *OldCommittedSxact;


/*
 * These configuration variables are used to set the predicate lock table size
 * and to control promotion of predicate locks to coarser granularity in an
 * attempt to degrade performance (mostly as false positive serialization
 * failure) gracefully in the face of memory pressure.
 */
int			max_predicate_locks_per_xact;	/* in guc_tables.c */
int			max_predicate_locks_per_relation;	/* in guc_tables.c */
int			max_predicate_locks_per_page;	/* in guc_tables.c */

/*
 * This provides a list of objects in order to track transactions
 * participating in predicate locking.  Entries in the list are fixed size,
 * and reside in shared memory.  The memory address of an entry must remain
 * fixed during its lifetime.  The list will be protected from concurrent
 * update externally; no provision is made in this code to manage that.  The
 * number of entries in the list, and the size allowed for each entry is
 * fixed upon creation.
 */
static PredXactList PredXact;

/*
 * This provides a pool of RWConflict data elements to use in conflict lists
 * between transactions.
 */
static RWConflictPoolHeader RWConflictPool;

/*
 * The predicate locking hash tables are in shared memory.
 * Each backend keeps pointers to them.
 */
static HTAB *SerializableXidHash;
static HTAB *PredicateLockTargetHash;
static HTAB *PredicateLockHash;
static dlist_head *FinishedSerializableTransactions;

/*
 * Tag for a dummy entry in PredicateLockTargetHash. By temporarily removing
 * this entry, you can ensure that there's enough scratch space available for
 * inserting one entry in the hash table. This is an otherwise-invalid tag.
 */
static const PREDICATELOCKTARGETTAG ScratchTargetTag = {0, 0, 0, 0};
static uint32 ScratchTargetTagHash;
static LWLock *ScratchPartitionLock;

/*
 * The local hash table used to determine when to combine multiple fine-
 * grained locks into a single courser-grained lock.
 */
static HTAB *LocalPredicateLockHash = NULL;

/*
 * Keep a pointer to the currently-running serializable transaction (if any)
 * for quick reference. Also, remember if we have written anything that could
 * cause a rw-conflict.
 */
static SERIALIZABLEXACT *MySerializableXact = InvalidSerializableXact;
static bool MyXactDidWrite = false;

/*
 * The SXACT_FLAG_RO_UNSAFE optimization might lead us to release
 * MySerializableXact early.  If that happens in a parallel query, the leader
 * needs to defer the destruction of the SERIALIZABLEXACT until end of
 * transaction, because the workers still have a reference to it.  In that
 * case, the leader stores it here.
 */
static SERIALIZABLEXACT *SavedSerializableXact = InvalidSerializableXact;

/* local functions */

static SERIALIZABLEXACT *CreatePredXact(void);
static void ReleasePredXact(SERIALIZABLEXACT *sxact);

static bool RWConflictExists(const SERIALIZABLEXACT *reader, const SERIALIZABLEXACT *writer);
static void SetRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer);
static void SetPossibleUnsafeConflict(SERIALIZABLEXACT *roXact, SERIALIZABLEXACT *activeXact);
static void ReleaseRWConflict(RWConflict conflict);
static void FlagSxactUnsafe(SERIALIZABLEXACT *sxact);

static bool SerialPagePrecedesLogically(int64 page1, int64 page2);
static void SerialInit(void);
static void SerialAdd(TransactionId xid, SerCommitSeqNo minConflictCommitSeqNo);
static SerCommitSeqNo SerialGetMinConflictCommitSeqNo(TransactionId xid);
static void SerialSetActiveSerXmin(TransactionId xid);

static uint32 predicatelock_hash(const void *key, Size keysize);
static void SummarizeOldestCommittedSxact(void);
static Snapshot GetSafeSnapshot(Snapshot origSnapshot);
static Snapshot GetSerializableTransactionSnapshotInt(Snapshot snapshot,
													  VirtualTransactionId *sourcevxid,
													  int sourcepid);
static bool PredicateLockExists(const PREDICATELOCKTARGETTAG *targettag);
static bool GetParentPredicateLockTag(const PREDICATELOCKTARGETTAG *tag,
									  PREDICATELOCKTARGETTAG *parent);
static bool CoarserLockCovers(const PREDICATELOCKTARGETTAG *newtargettag);
static void RemoveScratchTarget(bool lockheld);
static void RestoreScratchTarget(bool lockheld);
static void RemoveTargetIfNoLongerUsed(PREDICATELOCKTARGET *target,
									   uint32 targettaghash);
static void DeleteChildTargetLocks(const PREDICATELOCKTARGETTAG *newtargettag);
static int	MaxPredicateChildLocks(const PREDICATELOCKTARGETTAG *tag);
static bool CheckAndPromotePredicateLockRequest(const PREDICATELOCKTARGETTAG *reqtag);
static void DecrementParentLocks(const PREDICATELOCKTARGETTAG *targettag);
static void CreatePredicateLock(const PREDICATELOCKTARGETTAG *targettag,
								uint32 targettaghash,
								SERIALIZABLEXACT *sxact);
static void DeleteLockTarget(PREDICATELOCKTARGET *target, uint32 targettaghash);
static bool TransferPredicateLocksToNewTarget(PREDICATELOCKTARGETTAG oldtargettag,
											  PREDICATELOCKTARGETTAG newtargettag,
											  bool removeOld);
static void PredicateLockAcquire(const PREDICATELOCKTARGETTAG *targettag);
static void DropAllPredicateLocksFromTable(Relation relation,
										   bool transfer);
static void SetNewSxactGlobalXmin(void);
static void ClearOldPredicateLocks(void);
static void ReleaseOneSerializableXact(SERIALIZABLEXACT *sxact, bool partial,
									   bool summarize);
static bool XidIsConcurrent(TransactionId xid);
static void CheckTargetForConflictsIn(PREDICATELOCKTARGETTAG *targettag);
static void FlagRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer);
static void OnConflict_CheckForSerializationFailure(const SERIALIZABLEXACT *reader,
													SERIALIZABLEXACT *writer);
static void CreateLocalPredicateLockHash(void);
static void ReleasePredicateLocksLocal(void);


/*------------------------------------------------------------------------*/

/*
 * Does this relation participate in predicate locking? Temporary and system
 * relations are exempt.
 */
static inline bool
PredicateLockingNeededForRelation(Relation relation)
{
	return !(relation->rd_id < FirstUnpinnedObjectId ||
			 RelationUsesLocalBuffers(relation));
}

/*
 * When a public interface method is called for a read, this is the test to
 * see if we should do a quick return.
 *
 * Note: this function has side-effects! If this transaction has been flagged
 * as RO-safe since the last call, we release all predicate locks and reset
 * MySerializableXact. That makes subsequent calls to return quickly.
 *
 * This is marked as 'inline' to eliminate the function call overhead in the
 * common case that serialization is not needed.
 */
static inline bool
SerializationNeededForRead(Relation relation, Snapshot snapshot)
{
	/* Nothing to do if this is not a serializable transaction */
	if (MySerializableXact == InvalidSerializableXact)
		return false;

	/*
	 * Don't acquire locks or conflict when scanning with a special snapshot.
	 * This excludes things like CLUSTER and REINDEX. They use the wholesale
	 * functions TransferPredicateLocksToHeapRelation() and
	 * CheckTableForSerializableConflictIn() to participate in serialization,
	 * but the scans involved don't need serialization.
	 */
	if (!IsMVCCSnapshot(snapshot))
		return false;

	/*
	 * Check if we have just become "RO-safe". If we have, immediately release
	 * all locks as they're not needed anymore. This also resets
	 * MySerializableXact, so that subsequent calls to this function can exit
	 * quickly.
	 *
	 * A transaction is flagged as RO_SAFE if all concurrent R/W transactions
	 * commit without having conflicts out to an earlier snapshot, thus
	 * ensuring that no conflicts are possible for this transaction.
	 */
	if (SxactIsROSafe(MySerializableXact))
	{
		ReleasePredicateLocks(false, true);
		return false;
	}

	/* Check if the relation doesn't participate in predicate locking */
	if (!PredicateLockingNeededForRelation(relation))
		return false;

	return true;				/* no excuse to skip predicate locking */
}

/*
 * Like SerializationNeededForRead(), but called on writes.
 * The logic is the same, but there is no snapshot and we can't be RO-safe.
 */
static inline bool
SerializationNeededForWrite(Relation relation)
{
	/* Nothing to do if this is not a serializable transaction */
	if (MySerializableXact == InvalidSerializableXact)
		return false;

	/* Check if the relation doesn't participate in predicate locking */
	if (!PredicateLockingNeededForRelation(relation))
		return false;

	return true;				/* no excuse to skip predicate locking */
}


/*------------------------------------------------------------------------*/

/*
 * These functions are a simple implementation of a list for this specific
 * type of struct.  If there is ever a generalized shared memory list, we
 * should probably switch to that.
 */
static SERIALIZABLEXACT *
CreatePredXact(void)
{
	SERIALIZABLEXACT *sxact;

	if (dlist_is_empty(&PredXact->availableList))
		return NULL;

	sxact = dlist_container(SERIALIZABLEXACT, xactLink,
							dlist_pop_head_node(&PredXact->availableList));
	dlist_push_tail(&PredXact->activeList, &sxact->xactLink);
	return sxact;
}

static void
ReleasePredXact(SERIALIZABLEXACT *sxact)
{
	Assert(ShmemAddrIsValid(sxact));

	dlist_delete(&sxact->xactLink);
	dlist_push_tail(&PredXact->availableList, &sxact->xactLink);
}

/*------------------------------------------------------------------------*/

/*
 * These functions manage primitive access to the RWConflict pool and lists.
 */
static bool
RWConflictExists(const SERIALIZABLEXACT *reader, const SERIALIZABLEXACT *writer)
{
	dlist_iter	iter;

	Assert(reader != writer);

	/* Check the ends of the purported conflict first. */
	if (SxactIsDoomed(reader)
		|| SxactIsDoomed(writer)
		|| dlist_is_empty(&reader->outConflicts)
		|| dlist_is_empty(&writer->inConflicts))
		return false;

	/*
	 * A conflict is possible; walk the list to find out.
	 *
	 * The unconstify is needed as we have no const version of
	 * dlist_foreach().
	 */
	dlist_foreach(iter, &unconstify(SERIALIZABLEXACT *, reader)->outConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, outLink, iter.cur);

		if (conflict->sxactIn == writer)
			return true;
	}

	/* No conflict found. */
	return false;
}

static void
SetRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer)
{
	RWConflict	conflict;

	Assert(reader != writer);
	Assert(!RWConflictExists(reader, writer));

	if (dlist_is_empty(&RWConflictPool->availableList))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough elements in RWConflictPool to record a read/write conflict"),
				 errhint("You might need to run fewer transactions at a time or increase \"max_connections\".")));

	conflict = dlist_head_element(RWConflictData, outLink, &RWConflictPool->availableList);
	dlist_delete(&conflict->outLink);

	conflict->sxactOut = reader;
	conflict->sxactIn = writer;
	dlist_push_tail(&reader->outConflicts, &conflict->outLink);
	dlist_push_tail(&writer->inConflicts, &conflict->inLink);
}

static void
SetPossibleUnsafeConflict(SERIALIZABLEXACT *roXact,
						  SERIALIZABLEXACT *activeXact)
{
	RWConflict	conflict;

	Assert(roXact != activeXact);
	Assert(SxactIsReadOnly(roXact));
	Assert(!SxactIsReadOnly(activeXact));

	if (dlist_is_empty(&RWConflictPool->availableList))
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough elements in RWConflictPool to record a potential read/write conflict"),
				 errhint("You might need to run fewer transactions at a time or increase \"max_connections\".")));

	conflict = dlist_head_element(RWConflictData, outLink, &RWConflictPool->availableList);
	dlist_delete(&conflict->outLink);

	conflict->sxactOut = activeXact;
	conflict->sxactIn = roXact;
	dlist_push_tail(&activeXact->possibleUnsafeConflicts, &conflict->outLink);
	dlist_push_tail(&roXact->possibleUnsafeConflicts, &conflict->inLink);
}

static void
ReleaseRWConflict(RWConflict conflict)
{
	dlist_delete(&conflict->inLink);
	dlist_delete(&conflict->outLink);
	dlist_push_tail(&RWConflictPool->availableList, &conflict->outLink);
}

static void
FlagSxactUnsafe(SERIALIZABLEXACT *sxact)
{
	dlist_mutable_iter iter;

	Assert(SxactIsReadOnly(sxact));
	Assert(!SxactIsROSafe(sxact));

	sxact->flags |= SXACT_FLAG_RO_UNSAFE;

	/*
	 * We know this isn't a safe snapshot, so we can stop looking for other
	 * potential conflicts.
	 */
	dlist_foreach_modify(iter, &sxact->possibleUnsafeConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, inLink, iter.cur);

		Assert(!SxactIsReadOnly(conflict->sxactOut));
		Assert(sxact == conflict->sxactIn);

		ReleaseRWConflict(conflict);
	}
}

/*------------------------------------------------------------------------*/

/*
 * Decide whether a Serial page number is "older" for truncation purposes.
 * Analogous to CLOGPagePrecedes().
 */
static bool
SerialPagePrecedesLogically(int64 page1, int64 page2)
{
	TransactionId xid1;
	TransactionId xid2;

	xid1 = ((TransactionId) page1) * SERIAL_ENTRIESPERPAGE;
	xid1 += FirstNormalTransactionId + 1;
	xid2 = ((TransactionId) page2) * SERIAL_ENTRIESPERPAGE;
	xid2 += FirstNormalTransactionId + 1;

	return (TransactionIdPrecedes(xid1, xid2) &&
			TransactionIdPrecedes(xid1, xid2 + SERIAL_ENTRIESPERPAGE - 1));
}

#ifdef USE_ASSERT_CHECKING
static void
SerialPagePrecedesLogicallyUnitTests(void)
{
	int			per_page = SERIAL_ENTRIESPERPAGE,
				offset = per_page / 2;
	int64		newestPage,
				oldestPage,
				headPage,
				targetPage;
	TransactionId newestXact,
				oldestXact;

	/* GetNewTransactionId() has assigned the last XID it can safely use. */
	newestPage = 2 * SLRU_PAGES_PER_SEGMENT - 1;	/* nothing special */
	newestXact = newestPage * per_page + offset;
	Assert(newestXact / per_page == newestPage);
	oldestXact = newestXact + 1;
	oldestXact -= 1U << 31;
	oldestPage = oldestXact / per_page;

	/*
	 * In this scenario, the SLRU headPage pertains to the last ~1000 XIDs
	 * assigned.  oldestXact finishes, ~2B XIDs having elapsed since it
	 * started.  Further transactions cause us to summarize oldestXact to
	 * tailPage.  Function must return false so SerialAdd() doesn't zero
	 * tailPage (which may contain entries for other old, recently-finished
	 * XIDs) and half the SLRU.  Reaching this requires burning ~2B XIDs in
	 * single-user mode, a negligible possibility.
	 */
	headPage = newestPage;
	targetPage = oldestPage;
	Assert(!SerialPagePrecedesLogically(headPage, targetPage));

	/*
	 * In this scenario, the SLRU headPage pertains to oldestXact.  We're
	 * summarizing an XID near newestXact.  (Assume few other XIDs used
	 * SERIALIZABLE, hence the minimal headPage advancement.  Assume
	 * oldestXact was long-running and only recently reached the SLRU.)
	 * Function must return true to make SerialAdd() create targetPage.
	 *
	 * Today's implementation mishandles this case, but it doesn't matter
	 * enough to fix.  Verify that the defect affects just one page by
	 * asserting correct treatment of its prior page.  Reaching this case
	 * requires burning ~2B XIDs in single-user mode, a negligible
	 * possibility.  Moreover, if it does happen, the consequence would be
	 * mild, namely a new transaction failing in SimpleLruReadPage().
	 */
	headPage = oldestPage;
	targetPage = newestPage;
	Assert(SerialPagePrecedesLogically(headPage, targetPage - 1));
#if 0
	Assert(SerialPagePrecedesLogically(headPage, targetPage));
#endif
}
#endif

/*
 * Initialize for the tracking of old serializable committed xids.
 */
static void
SerialInit(void)
{
	bool		found;

	/*
	 * Set up SLRU management of the pg_serial data.
	 */
	SerialSlruCtl->PagePrecedes = SerialPagePrecedesLogically;
	SimpleLruInit(SerialSlruCtl, "serializable",
				  serializable_buffers, 0, "pg_serial",
				  LWTRANCHE_SERIAL_BUFFER, LWTRANCHE_SERIAL_SLRU,
				  SYNC_HANDLER_NONE, false);
#ifdef USE_ASSERT_CHECKING
	SerialPagePrecedesLogicallyUnitTests();
#endif
	SlruPagePrecedesUnitTests(SerialSlruCtl, SERIAL_ENTRIESPERPAGE);

	/*
	 * Create or attach to the SerialControl structure.
	 */
	serialControl = (SerialControl)
		ShmemInitStruct("SerialControlData", sizeof(SerialControlData), &found);

	Assert(found == IsUnderPostmaster);
	if (!found)
	{
		/*
		 * Set control information to reflect empty SLRU.
		 */
		LWLockAcquire(SerialControlLock, LW_EXCLUSIVE);
		serialControl->headPage = -1;
		serialControl->headXid = InvalidTransactionId;
		serialControl->tailXid = InvalidTransactionId;
		LWLockRelease(SerialControlLock);
	}
}

/*
 * GUC check_hook for serializable_buffers
 */
bool
check_serial_buffers(int *newval, void **extra, GucSource source)
{
	return check_slru_buffers("serializable_buffers", newval);
}

/*
 * Record a committed read write serializable xid and the minimum
 * commitSeqNo of any transactions to which this xid had a rw-conflict out.
 * An invalid commitSeqNo means that there were no conflicts out from xid.
 */
static void
SerialAdd(TransactionId xid, SerCommitSeqNo minConflictCommitSeqNo)
{
	TransactionId tailXid;
	int64		targetPage;
	int			slotno;
	int64		firstZeroPage;
	bool		isNewPage;
	LWLock	   *lock;

	Assert(TransactionIdIsValid(xid));

	targetPage = SerialPage(xid);
	lock = SimpleLruGetBankLock(SerialSlruCtl, targetPage);

	/*
	 * In this routine, we must hold both SerialControlLock and the SLRU bank
	 * lock simultaneously while making the SLRU data catch up with the new
	 * state that we determine.
	 */
	LWLockAcquire(SerialControlLock, LW_EXCLUSIVE);

	/*
	 * If no serializable transactions are active, there shouldn't be anything
	 * to push out to the SLRU.  Hitting this assert would mean there's
	 * something wrong with the earlier cleanup logic.
	 */
	tailXid = serialControl->tailXid;
	Assert(TransactionIdIsValid(tailXid));

	/*
	 * If the SLRU is currently unused, zero out the whole active region from
	 * tailXid to headXid before taking it into use. Otherwise zero out only
	 * any new pages that enter the tailXid-headXid range as we advance
	 * headXid.
	 */
	if (serialControl->headPage < 0)
	{
		firstZeroPage = SerialPage(tailXid);
		isNewPage = true;
	}
	else
	{
		firstZeroPage = SerialNextPage(serialControl->headPage);
		isNewPage = SerialPagePrecedesLogically(serialControl->headPage,
												targetPage);
	}

	if (!TransactionIdIsValid(serialControl->headXid)
		|| TransactionIdFollows(xid, serialControl->headXid))
		serialControl->headXid = xid;
	if (isNewPage)
		serialControl->headPage = targetPage;

	if (isNewPage)
	{
		/* Initialize intervening pages; might involve trading locks */
		for (;;)
		{
			lock = SimpleLruGetBankLock(SerialSlruCtl, firstZeroPage);
			LWLockAcquire(lock, LW_EXCLUSIVE);
			slotno = SimpleLruZeroPage(SerialSlruCtl, firstZeroPage);
			if (firstZeroPage == targetPage)
				break;
			firstZeroPage = SerialNextPage(firstZeroPage);
			LWLockRelease(lock);
		}
	}
	else
	{
		LWLockAcquire(lock, LW_EXCLUSIVE);
		slotno = SimpleLruReadPage(SerialSlruCtl, targetPage, true, xid);
	}

	SerialValue(slotno, xid) = minConflictCommitSeqNo;
	SerialSlruCtl->shared->page_dirty[slotno] = true;

	LWLockRelease(lock);
	LWLockRelease(SerialControlLock);
}

/*
 * Get the minimum commitSeqNo for any conflict out for the given xid.  For
 * a transaction which exists but has no conflict out, InvalidSerCommitSeqNo
 * will be returned.
 */
static SerCommitSeqNo
SerialGetMinConflictCommitSeqNo(TransactionId xid)
{
	TransactionId headXid;
	TransactionId tailXid;
	SerCommitSeqNo val;
	int			slotno;

	Assert(TransactionIdIsValid(xid));

	LWLockAcquire(SerialControlLock, LW_SHARED);
	headXid = serialControl->headXid;
	tailXid = serialControl->tailXid;
	LWLockRelease(SerialControlLock);

	if (!TransactionIdIsValid(headXid))
		return 0;

	Assert(TransactionIdIsValid(tailXid));

	if (TransactionIdPrecedes(xid, tailXid)
		|| TransactionIdFollows(xid, headXid))
		return 0;

	/*
	 * The following function must be called without holding SLRU bank lock,
	 * but will return with that lock held, which must then be released.
	 */
	slotno = SimpleLruReadPage_ReadOnly(SerialSlruCtl,
										SerialPage(xid), xid);
	val = SerialValue(slotno, xid);
	LWLockRelease(SimpleLruGetBankLock(SerialSlruCtl, SerialPage(xid)));
	return val;
}

/*
 * Call this whenever there is a new xmin for active serializable
 * transactions.  We don't need to keep information on transactions which
 * precede that.  InvalidTransactionId means none active, so everything in
 * the SLRU can be discarded.
 */
static void
SerialSetActiveSerXmin(TransactionId xid)
{
	LWLockAcquire(SerialControlLock, LW_EXCLUSIVE);

	/*
	 * When no sxacts are active, nothing overlaps, set the xid values to
	 * invalid to show that there are no valid entries.  Don't clear headPage,
	 * though.  A new xmin might still land on that page, and we don't want to
	 * repeatedly zero out the same page.
	 */
	if (!TransactionIdIsValid(xid))
	{
		serialControl->tailXid = InvalidTransactionId;
		serialControl->headXid = InvalidTransactionId;
		LWLockRelease(SerialControlLock);
		return;
	}

	/*
	 * When we're recovering prepared transactions, the global xmin might move
	 * backwards depending on the order they're recovered. Normally that's not
	 * OK, but during recovery no serializable transactions will commit, so
	 * the SLRU is empty and we can get away with it.
	 */
	if (RecoveryInProgress())
	{
		Assert(serialControl->headPage < 0);
		if (!TransactionIdIsValid(serialControl->tailXid)
			|| TransactionIdPrecedes(xid, serialControl->tailXid))
		{
			serialControl->tailXid = xid;
		}
		LWLockRelease(SerialControlLock);
		return;
	}

	Assert(!TransactionIdIsValid(serialControl->tailXid)
		   || TransactionIdFollows(xid, serialControl->tailXid));

	serialControl->tailXid = xid;

	LWLockRelease(SerialControlLock);
}

/*
 * Perform a checkpoint --- either during shutdown, or on-the-fly
 *
 * We don't have any data that needs to survive a restart, but this is a
 * convenient place to truncate the SLRU.
 */
void
CheckPointPredicate(void)
{
	int64		truncateCutoffPage;

	LWLockAcquire(SerialControlLock, LW_EXCLUSIVE);

	/* Exit quickly if the SLRU is currently not in use. */
	if (serialControl->headPage < 0)
	{
		LWLockRelease(SerialControlLock);
		return;
	}

	if (TransactionIdIsValid(serialControl->tailXid))
	{
		int64		tailPage;

		tailPage = SerialPage(serialControl->tailXid);

		/*
		 * It is possible for the tailXid to be ahead of the headXid.  This
		 * occurs if we checkpoint while there are in-progress serializable
		 * transaction(s) advancing the tail but we are yet to summarize the
		 * transactions.  In this case, we cutoff up to the headPage and the
		 * next summary will advance the headXid.
		 */
		if (SerialPagePrecedesLogically(tailPage, serialControl->headPage))
		{
			/* We can truncate the SLRU up to the page containing tailXid */
			truncateCutoffPage = tailPage;
		}
		else
			truncateCutoffPage = serialControl->headPage;
	}
	else
	{
		/*----------
		 * The SLRU is no longer needed. Truncate to head before we set head
		 * invalid.
		 *
		 * XXX: It's possible that the SLRU is not needed again until XID
		 * wrap-around has happened, so that the segment containing headPage
		 * that we leave behind will appear to be new again. In that case it
		 * won't be removed until XID horizon advances enough to make it
		 * current again.
		 *
		 * XXX: This should happen in vac_truncate_clog(), not in checkpoints.
		 * Consider this scenario, starting from a system with no in-progress
		 * transactions and VACUUM FREEZE having maximized oldestXact:
		 * - Start a SERIALIZABLE transaction.
		 * - Start, finish, and summarize a SERIALIZABLE transaction, creating
		 *   one SLRU page.
		 * - Consume XIDs to reach xidStopLimit.
		 * - Finish all transactions.  Due to the long-running SERIALIZABLE
		 *   transaction, earlier checkpoints did not touch headPage.  The
		 *   next checkpoint will change it, but that checkpoint happens after
		 *   the end of the scenario.
		 * - VACUUM to advance XID limits.
		 * - Consume ~2M XIDs, crossing the former xidWrapLimit.
		 * - Start, finish, and summarize a SERIALIZABLE transaction.
		 *   SerialAdd() declines to create the targetPage, because headPage
		 *   is not regarded as in the past relative to that targetPage.  The
		 *   transaction instigating the summarize fails in
		 *   SimpleLruReadPage().
		 */
		truncateCutoffPage = serialControl->headPage;
		serialControl->headPage = -1;
	}

	LWLockRelease(SerialControlLock);

	/*
	 * Truncate away pages that are no longer required.  Note that no
	 * additional locking is required, because this is only called as part of
	 * a checkpoint, and the validity limits have already been determined.
	 */
	SimpleLruTruncate(SerialSlruCtl, truncateCutoffPage);

	/*
	 * Write dirty SLRU pages to disk
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely as a debugging aid.
	 *
	 * We're doing this after the truncation to avoid writing pages right
	 * before deleting the file in which they sit, which would be completely
	 * pointless.
	 */
	SimpleLruWriteAll(SerialSlruCtl, true);
}

/*------------------------------------------------------------------------*/

/*
 * PredicateLockShmemInit -- Initialize the predicate locking data structures.
 *
 * This is called from CreateSharedMemoryAndSemaphores(), which see for
 * more comments.  In the normal postmaster case, the shared hash tables
 * are created here.  Backends inherit the pointers
 * to the shared tables via fork().  In the EXEC_BACKEND case, each
 * backend re-executes this code to obtain pointers to the already existing
 * shared hash tables.
 */
void
PredicateLockShmemInit(void)
{
	HASHCTL		info;
	long		max_table_size;
	Size		requestSize;
	bool		found;

#ifndef EXEC_BACKEND
	Assert(!IsUnderPostmaster);
#endif

	/*
	 * Compute size of predicate lock target hashtable. Note these
	 * calculations must agree with PredicateLockShmemSize!
	 */
	max_table_size = NPREDICATELOCKTARGETENTS();

	/*
	 * Allocate hash table for PREDICATELOCKTARGET structs.  This stores
	 * per-predicate-lock-target information.
	 */
	info.keysize = sizeof(PREDICATELOCKTARGETTAG);
	info.entrysize = sizeof(PREDICATELOCKTARGET);
	info.num_partitions = NUM_PREDICATELOCK_PARTITIONS;

	PredicateLockTargetHash = ShmemInitHash("PREDICATELOCKTARGET hash",
											max_table_size,
											max_table_size,
											&info,
											HASH_ELEM | HASH_BLOBS |
											HASH_PARTITION | HASH_FIXED_SIZE);

	/*
	 * Reserve a dummy entry in the hash table; we use it to make sure there's
	 * always one entry available when we need to split or combine a page,
	 * because running out of space there could mean aborting a
	 * non-serializable transaction.
	 */
	if (!IsUnderPostmaster)
	{
		(void) hash_search(PredicateLockTargetHash, &ScratchTargetTag,
						   HASH_ENTER, &found);
		Assert(!found);
	}

	/* Pre-calculate the hash and partition lock of the scratch entry */
	ScratchTargetTagHash = PredicateLockTargetTagHashCode(&ScratchTargetTag);
	ScratchPartitionLock = PredicateLockHashPartitionLock(ScratchTargetTagHash);

	/*
	 * Allocate hash table for PREDICATELOCK structs.  This stores per
	 * xact-lock-of-a-target information.
	 */
	info.keysize = sizeof(PREDICATELOCKTAG);
	info.entrysize = sizeof(PREDICATELOCK);
	info.hash = predicatelock_hash;
	info.num_partitions = NUM_PREDICATELOCK_PARTITIONS;

	/* Assume an average of 2 xacts per target */
	max_table_size *= 2;

	PredicateLockHash = ShmemInitHash("PREDICATELOCK hash",
									  max_table_size,
									  max_table_size,
									  &info,
									  HASH_ELEM | HASH_FUNCTION |
									  HASH_PARTITION | HASH_FIXED_SIZE);

	/*
	 * Compute size for serializable transaction hashtable. Note these
	 * calculations must agree with PredicateLockShmemSize!
	 */
	max_table_size = (MaxBackends + max_prepared_xacts);

	/*
	 * Allocate a list to hold information on transactions participating in
	 * predicate locking.
	 *
	 * Assume an average of 10 predicate locking transactions per backend.
	 * This allows aggressive cleanup while detail is present before data must
	 * be summarized for storage in SLRU and the "dummy" transaction.
	 */
	max_table_size *= 10;

	PredXact = ShmemInitStruct("PredXactList",
							   PredXactListDataSize,
							   &found);
	Assert(found == IsUnderPostmaster);
	if (!found)
	{
		int			i;

		dlist_init(&PredXact->availableList);
		dlist_init(&PredXact->activeList);
		PredXact->SxactGlobalXmin = InvalidTransactionId;
		PredXact->SxactGlobalXminCount = 0;
		PredXact->WritableSxactCount = 0;
		PredXact->LastSxactCommitSeqNo = FirstNormalSerCommitSeqNo - 1;
		PredXact->CanPartialClearThrough = 0;
		PredXact->HavePartialClearedThrough = 0;
		requestSize = mul_size((Size) max_table_size,
							   sizeof(SERIALIZABLEXACT));
		PredXact->element = ShmemAlloc(requestSize);
		/* Add all elements to available list, clean. */
		memset(PredXact->element, 0, requestSize);
		for (i = 0; i < max_table_size; i++)
		{
			LWLockInitialize(&PredXact->element[i].perXactPredicateListLock,
							 LWTRANCHE_PER_XACT_PREDICATE_LIST);
			dlist_push_tail(&PredXact->availableList, &PredXact->element[i].xactLink);
		}
		PredXact->OldCommittedSxact = CreatePredXact();
		SetInvalidVirtualTransactionId(PredXact->OldCommittedSxact->vxid);
		PredXact->OldCommittedSxact->prepareSeqNo = 0;
		PredXact->OldCommittedSxact->commitSeqNo = 0;
		PredXact->OldCommittedSxact->SeqNo.lastCommitBeforeSnapshot = 0;
		dlist_init(&PredXact->OldCommittedSxact->outConflicts);
		dlist_init(&PredXact->OldCommittedSxact->inConflicts);
		dlist_init(&PredXact->OldCommittedSxact->predicateLocks);
		dlist_node_init(&PredXact->OldCommittedSxact->finishedLink);
		dlist_init(&PredXact->OldCommittedSxact->possibleUnsafeConflicts);
		PredXact->OldCommittedSxact->topXid = InvalidTransactionId;
		PredXact->OldCommittedSxact->finishedBefore = InvalidTransactionId;
		PredXact->OldCommittedSxact->xmin = InvalidTransactionId;
		PredXact->OldCommittedSxact->flags = SXACT_FLAG_COMMITTED;
		PredXact->OldCommittedSxact->pid = 0;
		PredXact->OldCommittedSxact->pgprocno = INVALID_PROC_NUMBER;
	}
	/* This never changes, so let's keep a local copy. */
	OldCommittedSxact = PredXact->OldCommittedSxact;

	/*
	 * Allocate hash table for SERIALIZABLEXID structs.  This stores per-xid
	 * information for serializable transactions which have accessed data.
	 */
	info.keysize = sizeof(SERIALIZABLEXIDTAG);
	info.entrysize = sizeof(SERIALIZABLEXID);

	SerializableXidHash = ShmemInitHash("SERIALIZABLEXID hash",
										max_table_size,
										max_table_size,
										&info,
										HASH_ELEM | HASH_BLOBS |
										HASH_FIXED_SIZE);

	/*
	 * Allocate space for tracking rw-conflicts in lists attached to the
	 * transactions.
	 *
	 * Assume an average of 5 conflicts per transaction.  Calculations suggest
	 * that this will prevent resource exhaustion in even the most pessimal
	 * loads up to max_connections = 200 with all 200 connections pounding the
	 * database with serializable transactions.  Beyond that, there may be
	 * occasional transactions canceled when trying to flag conflicts. That's
	 * probably OK.
	 */
	max_table_size *= 5;

	RWConflictPool = ShmemInitStruct("RWConflictPool",
									 RWConflictPoolHeaderDataSize,
									 &found);
	Assert(found == IsUnderPostmaster);
	if (!found)
	{
		int			i;

		dlist_init(&RWConflictPool->availableList);
		requestSize = mul_size((Size) max_table_size,
							   RWConflictDataSize);
		RWConflictPool->element = ShmemAlloc(requestSize);
		/* Add all elements to available list, clean. */
		memset(RWConflictPool->element, 0, requestSize);
		for (i = 0; i < max_table_size; i++)
		{
			dlist_push_tail(&RWConflictPool->availableList,
							&RWConflictPool->element[i].outLink);
		}
	}

	/*
	 * Create or attach to the header for the list of finished serializable
	 * transactions.
	 */
	FinishedSerializableTransactions = (dlist_head *)
		ShmemInitStruct("FinishedSerializableTransactions",
						sizeof(dlist_head),
						&found);
	Assert(found == IsUnderPostmaster);
	if (!found)
		dlist_init(FinishedSerializableTransactions);

	/*
	 * Initialize the SLRU storage for old committed serializable
	 * transactions.
	 */
	SerialInit();
}

/*
 * Estimate shared-memory space used for predicate lock table
 */
Size
PredicateLockShmemSize(void)
{
	Size		size = 0;
	long		max_table_size;

	/* predicate lock target hash table */
	max_table_size = NPREDICATELOCKTARGETENTS();
	size = add_size(size, hash_estimate_size(max_table_size,
											 sizeof(PREDICATELOCKTARGET)));

	/* predicate lock hash table */
	max_table_size *= 2;
	size = add_size(size, hash_estimate_size(max_table_size,
											 sizeof(PREDICATELOCK)));

	/*
	 * Since NPREDICATELOCKTARGETENTS is only an estimate, add 10% safety
	 * margin.
	 */
	size = add_size(size, size / 10);

	/* transaction list */
	max_table_size = MaxBackends + max_prepared_xacts;
	max_table_size *= 10;
	size = add_size(size, PredXactListDataSize);
	size = add_size(size, mul_size((Size) max_table_size,
								   sizeof(SERIALIZABLEXACT)));

	/* transaction xid table */
	size = add_size(size, hash_estimate_size(max_table_size,
											 sizeof(SERIALIZABLEXID)));

	/* rw-conflict pool */
	max_table_size *= 5;
	size = add_size(size, RWConflictPoolHeaderDataSize);
	size = add_size(size, mul_size((Size) max_table_size,
								   RWConflictDataSize));

	/* Head for list of finished serializable transactions. */
	size = add_size(size, sizeof(dlist_head));

	/* Shared memory structures for SLRU tracking of old committed xids. */
	size = add_size(size, sizeof(SerialControlData));
	size = add_size(size, SimpleLruShmemSize(serializable_buffers, 0));

	return size;
}


/*
 * Compute the hash code associated with a PREDICATELOCKTAG.
 *
 * Because we want to use just one set of partition locks for both the
 * PREDICATELOCKTARGET and PREDICATELOCK hash tables, we have to make sure
 * that PREDICATELOCKs fall into the same partition number as their
 * associated PREDICATELOCKTARGETs.  dynahash.c expects the partition number
 * to be the low-order bits of the hash code, and therefore a
 * PREDICATELOCKTAG's hash code must have the same low-order bits as the
 * associated PREDICATELOCKTARGETTAG's hash code.  We achieve this with this
 * specialized hash function.
 */
static uint32
predicatelock_hash(const void *key, Size keysize)
{
	const PREDICATELOCKTAG *predicatelocktag = (const PREDICATELOCKTAG *) key;
	uint32		targethash;

	Assert(keysize == sizeof(PREDICATELOCKTAG));

	/* Look into the associated target object, and compute its hash code */
	targethash = PredicateLockTargetTagHashCode(&predicatelocktag->myTarget->tag);

	return PredicateLockHashCodeFromTargetHashCode(predicatelocktag, targethash);
}


/*
 * GetPredicateLockStatusData
 *		Return a table containing the internal state of the predicate
 *		lock manager for use in pg_lock_status.
 *
 * Like GetLockStatusData, this function tries to hold the partition LWLocks
 * for as short a time as possible by returning two arrays that simply
 * contain the PREDICATELOCKTARGETTAG and SERIALIZABLEXACT for each lock
 * table entry. Multiple copies of the same PREDICATELOCKTARGETTAG and
 * SERIALIZABLEXACT will likely appear.
 */
PredicateLockData *
GetPredicateLockStatusData(void)
{
	PredicateLockData *data;
	int			i;
	int			els,
				el;
	HASH_SEQ_STATUS seqstat;
	PREDICATELOCK *predlock;

	data = (PredicateLockData *) palloc(sizeof(PredicateLockData));

	/*
	 * To ensure consistency, take simultaneous locks on all partition locks
	 * in ascending order, then SerializableXactHashLock.
	 */
	for (i = 0; i < NUM_PREDICATELOCK_PARTITIONS; i++)
		LWLockAcquire(PredicateLockHashPartitionLockByIndex(i), LW_SHARED);
	LWLockAcquire(SerializableXactHashLock, LW_SHARED);

	/* Get number of locks and allocate appropriately-sized arrays. */
	els = hash_get_num_entries(PredicateLockHash);
	data->nelements = els;
	data->locktags = (PREDICATELOCKTARGETTAG *)
		palloc(sizeof(PREDICATELOCKTARGETTAG) * els);
	data->xacts = (SERIALIZABLEXACT *)
		palloc(sizeof(SERIALIZABLEXACT) * els);


	/* Scan through PredicateLockHash and copy contents */
	hash_seq_init(&seqstat, PredicateLockHash);

	el = 0;

	while ((predlock = (PREDICATELOCK *) hash_seq_search(&seqstat)))
	{
		data->locktags[el] = predlock->tag.myTarget->tag;
		data->xacts[el] = *predlock->tag.myXact;
		el++;
	}

	Assert(el == els);

	/* Release locks in reverse order */
	LWLockRelease(SerializableXactHashLock);
	for (i = NUM_PREDICATELOCK_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(PredicateLockHashPartitionLockByIndex(i));

	return data;
}

/*
 * Free up shared memory structures by pushing the oldest sxact (the one at
 * the front of the SummarizeOldestCommittedSxact queue) into summary form.
 * Each call will free exactly one SERIALIZABLEXACT structure and may also
 * free one or more of these structures: SERIALIZABLEXID, PREDICATELOCK,
 * PREDICATELOCKTARGET, RWConflictData.
 */
static void
SummarizeOldestCommittedSxact(void)
{
	SERIALIZABLEXACT *sxact;

	LWLockAcquire(SerializableFinishedListLock, LW_EXCLUSIVE);

	/*
	 * This function is only called if there are no sxact slots available.
	 * Some of them must belong to old, already-finished transactions, so
	 * there should be something in FinishedSerializableTransactions list that
	 * we can summarize. However, there's a race condition: while we were not
	 * holding any locks, a transaction might have ended and cleaned up all
	 * the finished sxact entries already, freeing up their sxact slots. In
	 * that case, we have nothing to do here. The caller will find one of the
	 * slots released by the other backend when it retries.
	 */
	if (dlist_is_empty(FinishedSerializableTransactions))
	{
		LWLockRelease(SerializableFinishedListLock);
		return;
	}

	/*
	 * Grab the first sxact off the finished list -- this will be the earliest
	 * commit.  Remove it from the list.
	 */
	sxact = dlist_head_element(SERIALIZABLEXACT, finishedLink,
							   FinishedSerializableTransactions);
	dlist_delete_thoroughly(&sxact->finishedLink);

	/* Add to SLRU summary information. */
	if (TransactionIdIsValid(sxact->topXid) && !SxactIsReadOnly(sxact))
		SerialAdd(sxact->topXid, SxactHasConflictOut(sxact)
				  ? sxact->SeqNo.earliestOutConflictCommit : InvalidSerCommitSeqNo);

	/* Summarize and release the detail. */
	ReleaseOneSerializableXact(sxact, false, true);

	LWLockRelease(SerializableFinishedListLock);
}

/*
 * GetSafeSnapshot
 *		Obtain and register a snapshot for a READ ONLY DEFERRABLE
 *		transaction. Ensures that the snapshot is "safe", i.e. a
 *		read-only transaction running on it can execute serializably
 *		without further checks. This requires waiting for concurrent
 *		transactions to complete, and retrying with a new snapshot if
 *		one of them could possibly create a conflict.
 *
 *		As with GetSerializableTransactionSnapshot (which this is a subroutine
 *		for), the passed-in Snapshot pointer should reference a static data
 *		area that can safely be passed to GetSnapshotData.
 */
static Snapshot
GetSafeSnapshot(Snapshot origSnapshot)
{
	Snapshot	snapshot;

	Assert(XactReadOnly && XactDeferrable);

	while (true)
	{
		/*
		 * GetSerializableTransactionSnapshotInt is going to call
		 * GetSnapshotData, so we need to provide it the static snapshot area
		 * our caller passed to us.  The pointer returned is actually the same
		 * one passed to it, but we avoid assuming that here.
		 */
		snapshot = GetSerializableTransactionSnapshotInt(origSnapshot,
														 NULL, InvalidPid);

		if (MySerializableXact == InvalidSerializableXact)
			return snapshot;	/* no concurrent r/w xacts; it's safe */

		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

		/*
		 * Wait for concurrent transactions to finish. Stop early if one of
		 * them marked us as conflicted.
		 */
		MySerializableXact->flags |= SXACT_FLAG_DEFERRABLE_WAITING;
		while (!(dlist_is_empty(&MySerializableXact->possibleUnsafeConflicts) ||
				 SxactIsROUnsafe(MySerializableXact)))
		{
			LWLockRelease(SerializableXactHashLock);
			ProcWaitForSignal(WAIT_EVENT_SAFE_SNAPSHOT);
			LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
		}
		MySerializableXact->flags &= ~SXACT_FLAG_DEFERRABLE_WAITING;

		if (!SxactIsROUnsafe(MySerializableXact))
		{
			LWLockRelease(SerializableXactHashLock);
			break;				/* success */
		}

		LWLockRelease(SerializableXactHashLock);

		/* else, need to retry... */
		ereport(DEBUG2,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg_internal("deferrable snapshot was unsafe; trying a new one")));
		ReleasePredicateLocks(false, false);
	}

	/*
	 * Now we have a safe snapshot, so we don't need to do any further checks.
	 */
	Assert(SxactIsROSafe(MySerializableXact));
	ReleasePredicateLocks(false, true);

	return snapshot;
}

/*
 * GetSafeSnapshotBlockingPids
 *		If the specified process is currently blocked in GetSafeSnapshot,
 *		write the process IDs of all processes that it is blocked by
 *		into the caller-supplied buffer output[].  The list is truncated at
 *		output_size, and the number of PIDs written into the buffer is
 *		returned.  Returns zero if the given PID is not currently blocked
 *		in GetSafeSnapshot.
 */
int
GetSafeSnapshotBlockingPids(int blocked_pid, int *output, int output_size)
{
	int			num_written = 0;
	dlist_iter	iter;
	SERIALIZABLEXACT *blocking_sxact = NULL;

	LWLockAcquire(SerializableXactHashLock, LW_SHARED);

	/* Find blocked_pid's SERIALIZABLEXACT by linear search. */
	dlist_foreach(iter, &PredXact->activeList)
	{
		SERIALIZABLEXACT *sxact =
			dlist_container(SERIALIZABLEXACT, xactLink, iter.cur);

		if (sxact->pid == blocked_pid)
		{
			blocking_sxact = sxact;
			break;
		}
	}

	/* Did we find it, and is it currently waiting in GetSafeSnapshot? */
	if (blocking_sxact != NULL && SxactIsDeferrableWaiting(blocking_sxact))
	{
		/* Traverse the list of possible unsafe conflicts collecting PIDs. */
		dlist_foreach(iter, &blocking_sxact->possibleUnsafeConflicts)
		{
			RWConflict	possibleUnsafeConflict =
				dlist_container(RWConflictData, inLink, iter.cur);

			output[num_written++] = possibleUnsafeConflict->sxactOut->pid;

			if (num_written >= output_size)
				break;
		}
	}

	LWLockRelease(SerializableXactHashLock);

	return num_written;
}

/*
 * Acquire a snapshot that can be used for the current transaction.
 *
 * Make sure we have a SERIALIZABLEXACT reference in MySerializableXact.
 * It should be current for this process and be contained in PredXact.
 *
 * The passed-in Snapshot pointer should reference a static data area that
 * can safely be passed to GetSnapshotData.  The return value is actually
 * always this same pointer; no new snapshot data structure is allocated
 * within this function.
 */
Snapshot
GetSerializableTransactionSnapshot(Snapshot snapshot)
{
	Assert(IsolationIsSerializable());

	/*
	 * Can't use serializable mode while recovery is still active, as it is,
	 * for example, on a hot standby.  We could get here despite the check in
	 * check_transaction_isolation() if default_transaction_isolation is set
	 * to serializable, so phrase the hint accordingly.
	 */
	if (RecoveryInProgress())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use serializable mode in a hot standby"),
				 errdetail("\"default_transaction_isolation\" is set to \"serializable\"."),
				 errhint("You can use \"SET default_transaction_isolation = 'repeatable read'\" to change the default.")));

	/*
	 * A special optimization is available for SERIALIZABLE READ ONLY
	 * DEFERRABLE transactions -- we can wait for a suitable snapshot and
	 * thereby avoid all SSI overhead once it's running.
	 */
	if (XactReadOnly && XactDeferrable)
		return GetSafeSnapshot(snapshot);

	return GetSerializableTransactionSnapshotInt(snapshot,
												 NULL, InvalidPid);
}

/*
 * Import a snapshot to be used for the current transaction.
 *
 * This is nearly the same as GetSerializableTransactionSnapshot, except that
 * we don't take a new snapshot, but rather use the data we're handed.
 *
 * The caller must have verified that the snapshot came from a serializable
 * transaction; and if we're read-write, the source transaction must not be
 * read-only.
 */
void
SetSerializableTransactionSnapshot(Snapshot snapshot,
								   VirtualTransactionId *sourcevxid,
								   int sourcepid)
{
	Assert(IsolationIsSerializable());

	/*
	 * If this is called by parallel.c in a parallel worker, we don't want to
	 * create a SERIALIZABLEXACT just yet because the leader's
	 * SERIALIZABLEXACT will be installed with AttachSerializableXact().  We
	 * also don't want to reject SERIALIZABLE READ ONLY DEFERRABLE in this
	 * case, because the leader has already determined that the snapshot it
	 * has passed us is safe.  So there is nothing for us to do.
	 */
	if (IsParallelWorker())
		return;

	/*
	 * We do not allow SERIALIZABLE READ ONLY DEFERRABLE transactions to
	 * import snapshots, since there's no way to wait for a safe snapshot when
	 * we're using the snap we're told to.  (XXX instead of throwing an error,
	 * we could just ignore the XactDeferrable flag?)
	 */
	if (XactReadOnly && XactDeferrable)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a snapshot-importing transaction must not be READ ONLY DEFERRABLE")));

	(void) GetSerializableTransactionSnapshotInt(snapshot, sourcevxid,
												 sourcepid);
}

/*
 * Guts of GetSerializableTransactionSnapshot
 *
 * If sourcevxid is valid, this is actually an import operation and we should
 * skip calling GetSnapshotData, because the snapshot contents are already
 * loaded up.  HOWEVER: to avoid race conditions, we must check that the
 * source xact is still running after we acquire SerializableXactHashLock.
 * We do that by calling ProcArrayInstallImportedXmin.
 */
static Snapshot
GetSerializableTransactionSnapshotInt(Snapshot snapshot,
									  VirtualTransactionId *sourcevxid,
									  int sourcepid)
{
	PGPROC	   *proc;
	VirtualTransactionId vxid;
	SERIALIZABLEXACT *sxact,
			   *othersxact;

	/* We only do this for serializable transactions.  Once. */
	Assert(MySerializableXact == InvalidSerializableXact);

	Assert(!RecoveryInProgress());

	/*
	 * Since all parts of a serializable transaction must use the same
	 * snapshot, it is too late to establish one after a parallel operation
	 * has begun.
	 */
	if (IsInParallelMode())
		elog(ERROR, "cannot establish serializable snapshot during a parallel operation");

	proc = MyProc;
	Assert(proc != NULL);
	GET_VXID_FROM_PGPROC(vxid, *proc);

	/*
	 * First we get the sxact structure, which may involve looping and access
	 * to the "finished" list to free a structure for use.
	 *
	 * We must hold SerializableXactHashLock when taking/checking the snapshot
	 * to avoid race conditions, for much the same reasons that
	 * GetSnapshotData takes the ProcArrayLock.  Since we might have to
	 * release SerializableXactHashLock to call SummarizeOldestCommittedSxact,
	 * this means we have to create the sxact first, which is a bit annoying
	 * (in particular, an elog(ERROR) in procarray.c would cause us to leak
	 * the sxact).  Consider refactoring to avoid this.
	 */
#ifdef TEST_SUMMARIZE_SERIAL
	SummarizeOldestCommittedSxact();
#endif
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
	do
	{
		sxact = CreatePredXact();
		/* If null, push out committed sxact to SLRU summary & retry. */
		if (!sxact)
		{
			LWLockRelease(SerializableXactHashLock);
			SummarizeOldestCommittedSxact();
			LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
		}
	} while (!sxact);

	/* Get the snapshot, or check that it's safe to use */
	if (!sourcevxid)
		snapshot = GetSnapshotData(snapshot);
	else if (!ProcArrayInstallImportedXmin(snapshot->xmin, sourcevxid))
	{
		ReleasePredXact(sxact);
		LWLockRelease(SerializableXactHashLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not import the requested snapshot"),
				 errdetail("The source process with PID %d is not running anymore.",
						   sourcepid)));
	}

	/*
	 * If there are no serializable transactions which are not read-only, we
	 * can "opt out" of predicate locking and conflict checking for a
	 * read-only transaction.
	 *
	 * The reason this is safe is that a read-only transaction can only become
	 * part of a dangerous structure if it overlaps a writable transaction
	 * which in turn overlaps a writable transaction which committed before
	 * the read-only transaction started.  A new writable transaction can
	 * overlap this one, but it can't meet the other condition of overlapping
	 * a transaction which committed before this one started.
	 */
	if (XactReadOnly && PredXact->WritableSxactCount == 0)
	{
		ReleasePredXact(sxact);
		LWLockRelease(SerializableXactHashLock);
		return snapshot;
	}

	/* Initialize the structure. */
	sxact->vxid = vxid;
	sxact->SeqNo.lastCommitBeforeSnapshot = PredXact->LastSxactCommitSeqNo;
	sxact->prepareSeqNo = InvalidSerCommitSeqNo;
	sxact->commitSeqNo = InvalidSerCommitSeqNo;
	dlist_init(&(sxact->outConflicts));
	dlist_init(&(sxact->inConflicts));
	dlist_init(&(sxact->possibleUnsafeConflicts));
	sxact->topXid = GetTopTransactionIdIfAny();
	sxact->finishedBefore = InvalidTransactionId;
	sxact->xmin = snapshot->xmin;
	sxact->pid = MyProcPid;
	sxact->pgprocno = MyProcNumber;
	dlist_init(&sxact->predicateLocks);
	dlist_node_init(&sxact->finishedLink);
	sxact->flags = 0;
	if (XactReadOnly)
	{
		dlist_iter	iter;

		sxact->flags |= SXACT_FLAG_READ_ONLY;

		/*
		 * Register all concurrent r/w transactions as possible conflicts; if
		 * all of them commit without any outgoing conflicts to earlier
		 * transactions then this snapshot can be deemed safe (and we can run
		 * without tracking predicate locks).
		 */
		dlist_foreach(iter, &PredXact->activeList)
		{
			othersxact = dlist_container(SERIALIZABLEXACT, xactLink, iter.cur);

			if (!SxactIsCommitted(othersxact)
				&& !SxactIsDoomed(othersxact)
				&& !SxactIsReadOnly(othersxact))
			{
				SetPossibleUnsafeConflict(sxact, othersxact);
			}
		}

		/*
		 * If we didn't find any possibly unsafe conflicts because every
		 * uncommitted writable transaction turned out to be doomed, then we
		 * can "opt out" immediately.  See comments above the earlier check
		 * for PredXact->WritableSxactCount == 0.
		 */
		if (dlist_is_empty(&sxact->possibleUnsafeConflicts))
		{
			ReleasePredXact(sxact);
			LWLockRelease(SerializableXactHashLock);
			return snapshot;
		}
	}
	else
	{
		++(PredXact->WritableSxactCount);
		Assert(PredXact->WritableSxactCount <=
			   (MaxBackends + max_prepared_xacts));
	}

	/* Maintain serializable global xmin info. */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
	{
		Assert(PredXact->SxactGlobalXminCount == 0);
		PredXact->SxactGlobalXmin = snapshot->xmin;
		PredXact->SxactGlobalXminCount = 1;
		SerialSetActiveSerXmin(snapshot->xmin);
	}
	else if (TransactionIdEquals(snapshot->xmin, PredXact->SxactGlobalXmin))
	{
		Assert(PredXact->SxactGlobalXminCount > 0);
		PredXact->SxactGlobalXminCount++;
	}
	else
	{
		Assert(TransactionIdFollows(snapshot->xmin, PredXact->SxactGlobalXmin));
	}

	MySerializableXact = sxact;
	MyXactDidWrite = false;		/* haven't written anything yet */

	LWLockRelease(SerializableXactHashLock);

	CreateLocalPredicateLockHash();

	return snapshot;
}

static void
CreateLocalPredicateLockHash(void)
{
	HASHCTL		hash_ctl;

	/* Initialize the backend-local hash table of parent locks */
	Assert(LocalPredicateLockHash == NULL);
	hash_ctl.keysize = sizeof(PREDICATELOCKTARGETTAG);
	hash_ctl.entrysize = sizeof(LOCALPREDICATELOCK);
	LocalPredicateLockHash = hash_create("Local predicate lock",
										 max_predicate_locks_per_xact,
										 &hash_ctl,
										 HASH_ELEM | HASH_BLOBS);
}

/*
 * Register the top level XID in SerializableXidHash.
 * Also store it for easy reference in MySerializableXact.
 */
void
RegisterPredicateLockingXid(TransactionId xid)
{
	SERIALIZABLEXIDTAG sxidtag;
	SERIALIZABLEXID *sxid;
	bool		found;

	/*
	 * If we're not tracking predicate lock data for this transaction, we
	 * should ignore the request and return quickly.
	 */
	if (MySerializableXact == InvalidSerializableXact)
		return;

	/* We should have a valid XID and be at the top level. */
	Assert(TransactionIdIsValid(xid));

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* This should only be done once per transaction. */
	Assert(MySerializableXact->topXid == InvalidTransactionId);

	MySerializableXact->topXid = xid;

	sxidtag.xid = xid;
	sxid = (SERIALIZABLEXID *) hash_search(SerializableXidHash,
										   &sxidtag,
										   HASH_ENTER, &found);
	Assert(!found);

	/* Initialize the structure. */
	sxid->myXact = MySerializableXact;
	LWLockRelease(SerializableXactHashLock);
}


/*
 * Check whether there are any predicate locks held by any transaction
 * for the page at the given block number.
 *
 * Note that the transaction may be completed but not yet subject to
 * cleanup due to overlapping serializable transactions.  This must
 * return valid information regardless of transaction isolation level.
 *
 * Also note that this doesn't check for a conflicting relation lock,
 * just a lock specifically on the given page.
 *
 * One use is to support proper behavior during GiST index vacuum.
 */
bool
PageIsPredicateLocked(Relation relation, BlockNumber blkno)
{
	PREDICATELOCKTARGETTAG targettag;
	uint32		targettaghash;
	LWLock	   *partitionLock;
	PREDICATELOCKTARGET *target;

	SET_PREDICATELOCKTARGETTAG_PAGE(targettag,
									relation->rd_locator.dbOid,
									relation->rd_id,
									blkno);

	targettaghash = PredicateLockTargetTagHashCode(&targettag);
	partitionLock = PredicateLockHashPartitionLock(targettaghash);
	LWLockAcquire(partitionLock, LW_SHARED);
	target = (PREDICATELOCKTARGET *)
		hash_search_with_hash_value(PredicateLockTargetHash,
									&targettag, targettaghash,
									HASH_FIND, NULL);
	LWLockRelease(partitionLock);

	return (target != NULL);
}


/*
 * Check whether a particular lock is held by this transaction.
 *
 * Important note: this function may return false even if the lock is
 * being held, because it uses the local lock table which is not
 * updated if another transaction modifies our lock list (e.g. to
 * split an index page). It can also return true when a coarser
 * granularity lock that covers this target is being held. Be careful
 * to only use this function in circumstances where such errors are
 * acceptable!
 */
static bool
PredicateLockExists(const PREDICATELOCKTARGETTAG *targettag)
{
	LOCALPREDICATELOCK *lock;

	/* check local hash table */
	lock = (LOCALPREDICATELOCK *) hash_search(LocalPredicateLockHash,
											  targettag,
											  HASH_FIND, NULL);

	if (!lock)
		return false;

	/*
	 * Found entry in the table, but still need to check whether it's actually
	 * held -- it could just be a parent of some held lock.
	 */
	return lock->held;
}

/*
 * Return the parent lock tag in the lock hierarchy: the next coarser
 * lock that covers the provided tag.
 *
 * Returns true and sets *parent to the parent tag if one exists,
 * returns false if none exists.
 */
static bool
GetParentPredicateLockTag(const PREDICATELOCKTARGETTAG *tag,
						  PREDICATELOCKTARGETTAG *parent)
{
	switch (GET_PREDICATELOCKTARGETTAG_TYPE(*tag))
	{
		case PREDLOCKTAG_RELATION:
			/* relation locks have no parent lock */
			return false;

		case PREDLOCKTAG_PAGE:
			/* parent lock is relation lock */
			SET_PREDICATELOCKTARGETTAG_RELATION(*parent,
												GET_PREDICATELOCKTARGETTAG_DB(*tag),
												GET_PREDICATELOCKTARGETTAG_RELATION(*tag));

			return true;

		case PREDLOCKTAG_TUPLE:
			/* parent lock is page lock */
			SET_PREDICATELOCKTARGETTAG_PAGE(*parent,
											GET_PREDICATELOCKTARGETTAG_DB(*tag),
											GET_PREDICATELOCKTARGETTAG_RELATION(*tag),
											GET_PREDICATELOCKTARGETTAG_PAGE(*tag));
			return true;
	}

	/* not reachable */
	Assert(false);
	return false;
}

/*
 * Check whether the lock we are considering is already covered by a
 * coarser lock for our transaction.
 *
 * Like PredicateLockExists, this function might return a false
 * negative, but it will never return a false positive.
 */
static bool
CoarserLockCovers(const PREDICATELOCKTARGETTAG *newtargettag)
{
	PREDICATELOCKTARGETTAG targettag,
				parenttag;

	targettag = *newtargettag;

	/* check parents iteratively until no more */
	while (GetParentPredicateLockTag(&targettag, &parenttag))
	{
		targettag = parenttag;
		if (PredicateLockExists(&targettag))
			return true;
	}

	/* no more parents to check; lock is not covered */
	return false;
}

/*
 * Remove the dummy entry from the predicate lock target hash, to free up some
 * scratch space. The caller must be holding SerializablePredicateListLock,
 * and must restore the entry with RestoreScratchTarget() before releasing the
 * lock.
 *
 * If lockheld is true, the caller is already holding the partition lock
 * of the partition containing the scratch entry.
 */
static void
RemoveScratchTarget(bool lockheld)
{
	bool		found;

	Assert(LWLockHeldByMe(SerializablePredicateListLock));

	if (!lockheld)
		LWLockAcquire(ScratchPartitionLock, LW_EXCLUSIVE);
	hash_search_with_hash_value(PredicateLockTargetHash,
								&ScratchTargetTag,
								ScratchTargetTagHash,
								HASH_REMOVE, &found);
	Assert(found);
	if (!lockheld)
		LWLockRelease(ScratchPartitionLock);
}

/*
 * Re-insert the dummy entry in predicate lock target hash.
 */
static void
RestoreScratchTarget(bool lockheld)
{
	bool		found;

	Assert(LWLockHeldByMe(SerializablePredicateListLock));

	if (!lockheld)
		LWLockAcquire(ScratchPartitionLock, LW_EXCLUSIVE);
	hash_search_with_hash_value(PredicateLockTargetHash,
								&ScratchTargetTag,
								ScratchTargetTagHash,
								HASH_ENTER, &found);
	Assert(!found);
	if (!lockheld)
		LWLockRelease(ScratchPartitionLock);
}

/*
 * Check whether the list of related predicate locks is empty for a
 * predicate lock target, and remove the target if it is.
 */
static void
RemoveTargetIfNoLongerUsed(PREDICATELOCKTARGET *target, uint32 targettaghash)
{
	PREDICATELOCKTARGET *rmtarget PG_USED_FOR_ASSERTS_ONLY;

	Assert(LWLockHeldByMe(SerializablePredicateListLock));

	/* Can't remove it until no locks at this target. */
	if (!dlist_is_empty(&target->predicateLocks))
		return;

	/* Actually remove the target. */
	rmtarget = hash_search_with_hash_value(PredicateLockTargetHash,
										   &target->tag,
										   targettaghash,
										   HASH_REMOVE, NULL);
	Assert(rmtarget == target);
}

/*
 * Delete child target locks owned by this process.
 * This implementation is assuming that the usage of each target tag field
 * is uniform.  No need to make this hard if we don't have to.
 *
 * We acquire an LWLock in the case of parallel mode, because worker
 * backends have access to the leader's SERIALIZABLEXACT.  Otherwise,
 * we aren't acquiring LWLocks for the predicate lock or lock
 * target structures associated with this transaction unless we're going
 * to modify them, because no other process is permitted to modify our
 * locks.
 */
static void
DeleteChildTargetLocks(const PREDICATELOCKTARGETTAG *newtargettag)
{
	SERIALIZABLEXACT *sxact;
	PREDICATELOCK *predlock;
	dlist_mutable_iter iter;

	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
	sxact = MySerializableXact;
	if (IsInParallelMode())
		LWLockAcquire(&sxact->perXactPredicateListLock, LW_EXCLUSIVE);

	dlist_foreach_modify(iter, &sxact->predicateLocks)
	{
		PREDICATELOCKTAG oldlocktag;
		PREDICATELOCKTARGET *oldtarget;
		PREDICATELOCKTARGETTAG oldtargettag;

		predlock = dlist_container(PREDICATELOCK, xactLink, iter.cur);

		oldlocktag = predlock->tag;
		Assert(oldlocktag.myXact == sxact);
		oldtarget = oldlocktag.myTarget;
		oldtargettag = oldtarget->tag;

		if (TargetTagIsCoveredBy(oldtargettag, *newtargettag))
		{
			uint32		oldtargettaghash;
			LWLock	   *partitionLock;
			PREDICATELOCK *rmpredlock PG_USED_FOR_ASSERTS_ONLY;

			oldtargettaghash = PredicateLockTargetTagHashCode(&oldtargettag);
			partitionLock = PredicateLockHashPartitionLock(oldtargettaghash);

			LWLockAcquire(partitionLock, LW_EXCLUSIVE);

			dlist_delete(&predlock->xactLink);
			dlist_delete(&predlock->targetLink);
			rmpredlock = hash_search_with_hash_value
				(PredicateLockHash,
				 &oldlocktag,
				 PredicateLockHashCodeFromTargetHashCode(&oldlocktag,
														 oldtargettaghash),
				 HASH_REMOVE, NULL);
			Assert(rmpredlock == predlock);

			RemoveTargetIfNoLongerUsed(oldtarget, oldtargettaghash);

			LWLockRelease(partitionLock);

			DecrementParentLocks(&oldtargettag);
		}
	}
	if (IsInParallelMode())
		LWLockRelease(&sxact->perXactPredicateListLock);
	LWLockRelease(SerializablePredicateListLock);
}

/*
 * Returns the promotion limit for a given predicate lock target.  This is the
 * max number of descendant locks allowed before promoting to the specified
 * tag. Note that the limit includes non-direct descendants (e.g., both tuples
 * and pages for a relation lock).
 *
 * Currently the default limit is 2 for a page lock, and half of the value of
 * max_pred_locks_per_transaction - 1 for a relation lock, to match behavior
 * of earlier releases when upgrading.
 *
 * TODO SSI: We should probably add additional GUCs to allow a maximum ratio
 * of page and tuple locks based on the pages in a relation, and the maximum
 * ratio of tuple locks to tuples in a page.  This would provide more
 * generally "balanced" allocation of locks to where they are most useful,
 * while still allowing the absolute numbers to prevent one relation from
 * tying up all predicate lock resources.
 */
static int
MaxPredicateChildLocks(const PREDICATELOCKTARGETTAG *tag)
{
	switch (GET_PREDICATELOCKTARGETTAG_TYPE(*tag))
	{
		case PREDLOCKTAG_RELATION:
			return max_predicate_locks_per_relation < 0
				? (max_predicate_locks_per_xact
				   / (-max_predicate_locks_per_relation)) - 1
				: max_predicate_locks_per_relation;

		case PREDLOCKTAG_PAGE:
			return max_predicate_locks_per_page;

		case PREDLOCKTAG_TUPLE:

			/*
			 * not reachable: nothing is finer-granularity than a tuple, so we
			 * should never try to promote to it.
			 */
			Assert(false);
			return 0;
	}

	/* not reachable */
	Assert(false);
	return 0;
}

/*
 * For all ancestors of a newly-acquired predicate lock, increment
 * their child count in the parent hash table. If any of them have
 * more descendants than their promotion threshold, acquire the
 * coarsest such lock.
 *
 * Returns true if a parent lock was acquired and false otherwise.
 */
static bool
CheckAndPromotePredicateLockRequest(const PREDICATELOCKTARGETTAG *reqtag)
{
	PREDICATELOCKTARGETTAG targettag,
				nexttag,
				promotiontag;
	LOCALPREDICATELOCK *parentlock;
	bool		found,
				promote;

	promote = false;

	targettag = *reqtag;

	/* check parents iteratively */
	while (GetParentPredicateLockTag(&targettag, &nexttag))
	{
		targettag = nexttag;
		parentlock = (LOCALPREDICATELOCK *) hash_search(LocalPredicateLockHash,
														&targettag,
														HASH_ENTER,
														&found);
		if (!found)
		{
			parentlock->held = false;
			parentlock->childLocks = 1;
		}
		else
			parentlock->childLocks++;

		if (parentlock->childLocks >
			MaxPredicateChildLocks(&targettag))
		{
			/*
			 * We should promote to this parent lock. Continue to check its
			 * ancestors, however, both to get their child counts right and to
			 * check whether we should just go ahead and promote to one of
			 * them.
			 */
			promotiontag = targettag;
			promote = true;
		}
	}

	if (promote)
	{
		/* acquire coarsest ancestor eligible for promotion */
		PredicateLockAcquire(&promotiontag);
		return true;
	}
	else
		return false;
}

/*
 * When releasing a lock, decrement the child count on all ancestor
 * locks.
 *
 * This is called only when releasing a lock via
 * DeleteChildTargetLocks (i.e. when a lock becomes redundant because
 * we've acquired its parent, possibly due to promotion) or when a new
 * MVCC write lock makes the predicate lock unnecessary. There's no
 * point in calling it when locks are released at transaction end, as
 * this information is no longer needed.
 */
static void
DecrementParentLocks(const PREDICATELOCKTARGETTAG *targettag)
{
	PREDICATELOCKTARGETTAG parenttag,
				nexttag;

	parenttag = *targettag;

	while (GetParentPredicateLockTag(&parenttag, &nexttag))
	{
		uint32		targettaghash;
		LOCALPREDICATELOCK *parentlock,
				   *rmlock PG_USED_FOR_ASSERTS_ONLY;

		parenttag = nexttag;
		targettaghash = PredicateLockTargetTagHashCode(&parenttag);
		parentlock = (LOCALPREDICATELOCK *)
			hash_search_with_hash_value(LocalPredicateLockHash,
										&parenttag, targettaghash,
										HASH_FIND, NULL);

		/*
		 * There's a small chance the parent lock doesn't exist in the lock
		 * table. This can happen if we prematurely removed it because an
		 * index split caused the child refcount to be off.
		 */
		if (parentlock == NULL)
			continue;

		parentlock->childLocks--;

		/*
		 * Under similar circumstances the parent lock's refcount might be
		 * zero. This only happens if we're holding that lock (otherwise we
		 * would have removed the entry).
		 */
		if (parentlock->childLocks < 0)
		{
			Assert(parentlock->held);
			parentlock->childLocks = 0;
		}

		if ((parentlock->childLocks == 0) && (!parentlock->held))
		{
			rmlock = (LOCALPREDICATELOCK *)
				hash_search_with_hash_value(LocalPredicateLockHash,
											&parenttag, targettaghash,
											HASH_REMOVE, NULL);
			Assert(rmlock == parentlock);
		}
	}
}

/*
 * Indicate that a predicate lock on the given target is held by the
 * specified transaction. Has no effect if the lock is already held.
 *
 * This updates the lock table and the sxact's lock list, and creates
 * the lock target if necessary, but does *not* do anything related to
 * granularity promotion or the local lock table. See
 * PredicateLockAcquire for that.
 */
static void
CreatePredicateLock(const PREDICATELOCKTARGETTAG *targettag,
					uint32 targettaghash,
					SERIALIZABLEXACT *sxact)
{
	PREDICATELOCKTARGET *target;
	PREDICATELOCKTAG locktag;
	PREDICATELOCK *lock;
	LWLock	   *partitionLock;
	bool		found;

	partitionLock = PredicateLockHashPartitionLock(targettaghash);

	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
	if (IsInParallelMode())
		LWLockAcquire(&sxact->perXactPredicateListLock, LW_EXCLUSIVE);
	LWLockAcquire(partitionLock, LW_EXCLUSIVE);

	/* Make sure that the target is represented. */
	target = (PREDICATELOCKTARGET *)
		hash_search_with_hash_value(PredicateLockTargetHash,
									targettag, targettaghash,
									HASH_ENTER_NULL, &found);
	if (!target)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You might need to increase \"%s\".", "max_pred_locks_per_transaction")));
	if (!found)
		dlist_init(&target->predicateLocks);

	/* We've got the sxact and target, make sure they're joined. */
	locktag.myTarget = target;
	locktag.myXact = sxact;
	lock = (PREDICATELOCK *)
		hash_search_with_hash_value(PredicateLockHash, &locktag,
									PredicateLockHashCodeFromTargetHashCode(&locktag, targettaghash),
									HASH_ENTER_NULL, &found);
	if (!lock)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of shared memory"),
				 errhint("You might need to increase \"%s\".", "max_pred_locks_per_transaction")));

	if (!found)
	{
		dlist_push_tail(&target->predicateLocks, &lock->targetLink);
		dlist_push_tail(&sxact->predicateLocks, &lock->xactLink);
		lock->commitSeqNo = InvalidSerCommitSeqNo;
	}

	LWLockRelease(partitionLock);
	if (IsInParallelMode())
		LWLockRelease(&sxact->perXactPredicateListLock);
	LWLockRelease(SerializablePredicateListLock);
}

/*
 * Acquire a predicate lock on the specified target for the current
 * connection if not already held. This updates the local lock table
 * and uses it to implement granularity promotion. It will consolidate
 * multiple locks into a coarser lock if warranted, and will release
 * any finer-grained locks covered by the new one.
 */
static void
PredicateLockAcquire(const PREDICATELOCKTARGETTAG *targettag)
{
	uint32		targettaghash;
	bool		found;
	LOCALPREDICATELOCK *locallock;

	/* Do we have the lock already, or a covering lock? */
	if (PredicateLockExists(targettag))
		return;

	if (CoarserLockCovers(targettag))
		return;

	/* the same hash and LW lock apply to the lock target and the local lock. */
	targettaghash = PredicateLockTargetTagHashCode(targettag);

	/* Acquire lock in local table */
	locallock = (LOCALPREDICATELOCK *)
		hash_search_with_hash_value(LocalPredicateLockHash,
									targettag, targettaghash,
									HASH_ENTER, &found);
	locallock->held = true;
	if (!found)
		locallock->childLocks = 0;

	/* Actually create the lock */
	CreatePredicateLock(targettag, targettaghash, MySerializableXact);

	/*
	 * Lock has been acquired. Check whether it should be promoted to a
	 * coarser granularity, or whether there are finer-granularity locks to
	 * clean up.
	 */
	if (CheckAndPromotePredicateLockRequest(targettag))
	{
		/*
		 * Lock request was promoted to a coarser-granularity lock, and that
		 * lock was acquired. It will delete this lock and any of its
		 * children, so we're done.
		 */
	}
	else
	{
		/* Clean up any finer-granularity locks */
		if (GET_PREDICATELOCKTARGETTAG_TYPE(*targettag) != PREDLOCKTAG_TUPLE)
			DeleteChildTargetLocks(targettag);
	}
}


/*
 *		PredicateLockRelation
 *
 * Gets a predicate lock at the relation level.
 * Skip if not in full serializable transaction isolation level.
 * Skip if this is a temporary table.
 * Clear any finer-grained predicate locks this session has on the relation.
 */
void
PredicateLockRelation(Relation relation, Snapshot snapshot)
{
	PREDICATELOCKTARGETTAG tag;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	SET_PREDICATELOCKTARGETTAG_RELATION(tag,
										relation->rd_locator.dbOid,
										relation->rd_id);
	PredicateLockAcquire(&tag);
}

/*
 *		PredicateLockPage
 *
 * Gets a predicate lock at the page level.
 * Skip if not in full serializable transaction isolation level.
 * Skip if this is a temporary table.
 * Skip if a coarser predicate lock already covers this page.
 * Clear any finer-grained predicate locks this session has on the relation.
 */
void
PredicateLockPage(Relation relation, BlockNumber blkno, Snapshot snapshot)
{
	PREDICATELOCKTARGETTAG tag;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	SET_PREDICATELOCKTARGETTAG_PAGE(tag,
									relation->rd_locator.dbOid,
									relation->rd_id,
									blkno);
	PredicateLockAcquire(&tag);
}

/*
 *		PredicateLockTID
 *
 * Gets a predicate lock at the tuple level.
 * Skip if not in full serializable transaction isolation level.
 * Skip if this is a temporary table.
 */
void
PredicateLockTID(Relation relation, ItemPointer tid, Snapshot snapshot,
				 TransactionId tuple_xid)
{
	PREDICATELOCKTARGETTAG tag;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	/*
	 * Return if this xact wrote it.
	 */
	if (relation->rd_index == NULL)
	{
		/* If we wrote it; we already have a write lock. */
		if (TransactionIdIsCurrentTransactionId(tuple_xid))
			return;
	}

	/*
	 * Do quick-but-not-definitive test for a relation lock first.  This will
	 * never cause a return when the relation is *not* locked, but will
	 * occasionally let the check continue when there really *is* a relation
	 * level lock.
	 */
	SET_PREDICATELOCKTARGETTAG_RELATION(tag,
										relation->rd_locator.dbOid,
										relation->rd_id);
	if (PredicateLockExists(&tag))
		return;

	SET_PREDICATELOCKTARGETTAG_TUPLE(tag,
									 relation->rd_locator.dbOid,
									 relation->rd_id,
									 ItemPointerGetBlockNumber(tid),
									 ItemPointerGetOffsetNumber(tid));
	PredicateLockAcquire(&tag);
}


/*
 *		DeleteLockTarget
 *
 * Remove a predicate lock target along with any locks held for it.
 *
 * Caller must hold SerializablePredicateListLock and the
 * appropriate hash partition lock for the target.
 */
static void
DeleteLockTarget(PREDICATELOCKTARGET *target, uint32 targettaghash)
{
	dlist_mutable_iter iter;

	Assert(LWLockHeldByMeInMode(SerializablePredicateListLock,
								LW_EXCLUSIVE));
	Assert(LWLockHeldByMe(PredicateLockHashPartitionLock(targettaghash)));

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	dlist_foreach_modify(iter, &target->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, targetLink, iter.cur);
		bool		found;

		dlist_delete(&(predlock->xactLink));
		dlist_delete(&(predlock->targetLink));

		hash_search_with_hash_value
			(PredicateLockHash,
			 &predlock->tag,
			 PredicateLockHashCodeFromTargetHashCode(&predlock->tag,
													 targettaghash),
			 HASH_REMOVE, &found);
		Assert(found);
	}
	LWLockRelease(SerializableXactHashLock);

	/* Remove the target itself, if possible. */
	RemoveTargetIfNoLongerUsed(target, targettaghash);
}


/*
 *		TransferPredicateLocksToNewTarget
 *
 * Move or copy all the predicate locks for a lock target, for use by
 * index page splits/combines and other things that create or replace
 * lock targets. If 'removeOld' is true, the old locks and the target
 * will be removed.
 *
 * Returns true on success, or false if we ran out of shared memory to
 * allocate the new target or locks. Guaranteed to always succeed if
 * removeOld is set (by using the scratch entry in PredicateLockTargetHash
 * for scratch space).
 *
 * Warning: the "removeOld" option should be used only with care,
 * because this function does not (indeed, can not) update other
 * backends' LocalPredicateLockHash. If we are only adding new
 * entries, this is not a problem: the local lock table is used only
 * as a hint, so missing entries for locks that are held are
 * OK. Having entries for locks that are no longer held, as can happen
 * when using "removeOld", is not in general OK. We can only use it
 * safely when replacing a lock with a coarser-granularity lock that
 * covers it, or if we are absolutely certain that no one will need to
 * refer to that lock in the future.
 *
 * Caller must hold SerializablePredicateListLock exclusively.
 */
static bool
TransferPredicateLocksToNewTarget(PREDICATELOCKTARGETTAG oldtargettag,
								  PREDICATELOCKTARGETTAG newtargettag,
								  bool removeOld)
{
	uint32		oldtargettaghash;
	LWLock	   *oldpartitionLock;
	PREDICATELOCKTARGET *oldtarget;
	uint32		newtargettaghash;
	LWLock	   *newpartitionLock;
	bool		found;
	bool		outOfShmem = false;

	Assert(LWLockHeldByMeInMode(SerializablePredicateListLock,
								LW_EXCLUSIVE));

	oldtargettaghash = PredicateLockTargetTagHashCode(&oldtargettag);
	newtargettaghash = PredicateLockTargetTagHashCode(&newtargettag);
	oldpartitionLock = PredicateLockHashPartitionLock(oldtargettaghash);
	newpartitionLock = PredicateLockHashPartitionLock(newtargettaghash);

	if (removeOld)
	{
		/*
		 * Remove the dummy entry to give us scratch space, so we know we'll
		 * be able to create the new lock target.
		 */
		RemoveScratchTarget(false);
	}

	/*
	 * We must get the partition locks in ascending sequence to avoid
	 * deadlocks. If old and new partitions are the same, we must request the
	 * lock only once.
	 */
	if (oldpartitionLock < newpartitionLock)
	{
		LWLockAcquire(oldpartitionLock,
					  (removeOld ? LW_EXCLUSIVE : LW_SHARED));
		LWLockAcquire(newpartitionLock, LW_EXCLUSIVE);
	}
	else if (oldpartitionLock > newpartitionLock)
	{
		LWLockAcquire(newpartitionLock, LW_EXCLUSIVE);
		LWLockAcquire(oldpartitionLock,
					  (removeOld ? LW_EXCLUSIVE : LW_SHARED));
	}
	else
		LWLockAcquire(newpartitionLock, LW_EXCLUSIVE);

	/*
	 * Look for the old target.  If not found, that's OK; no predicate locks
	 * are affected, so we can just clean up and return. If it does exist,
	 * walk its list of predicate locks and move or copy them to the new
	 * target.
	 */
	oldtarget = hash_search_with_hash_value(PredicateLockTargetHash,
											&oldtargettag,
											oldtargettaghash,
											HASH_FIND, NULL);

	if (oldtarget)
	{
		PREDICATELOCKTARGET *newtarget;
		PREDICATELOCKTAG newpredlocktag;
		dlist_mutable_iter iter;

		newtarget = hash_search_with_hash_value(PredicateLockTargetHash,
												&newtargettag,
												newtargettaghash,
												HASH_ENTER_NULL, &found);

		if (!newtarget)
		{
			/* Failed to allocate due to insufficient shmem */
			outOfShmem = true;
			goto exit;
		}

		/* If we created a new entry, initialize it */
		if (!found)
			dlist_init(&newtarget->predicateLocks);

		newpredlocktag.myTarget = newtarget;

		/*
		 * Loop through all the locks on the old target, replacing them with
		 * locks on the new target.
		 */
		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

		dlist_foreach_modify(iter, &oldtarget->predicateLocks)
		{
			PREDICATELOCK *oldpredlock =
				dlist_container(PREDICATELOCK, targetLink, iter.cur);
			PREDICATELOCK *newpredlock;
			SerCommitSeqNo oldCommitSeqNo = oldpredlock->commitSeqNo;

			newpredlocktag.myXact = oldpredlock->tag.myXact;

			if (removeOld)
			{
				dlist_delete(&(oldpredlock->xactLink));
				dlist_delete(&(oldpredlock->targetLink));

				hash_search_with_hash_value
					(PredicateLockHash,
					 &oldpredlock->tag,
					 PredicateLockHashCodeFromTargetHashCode(&oldpredlock->tag,
															 oldtargettaghash),
					 HASH_REMOVE, &found);
				Assert(found);
			}

			newpredlock = (PREDICATELOCK *)
				hash_search_with_hash_value(PredicateLockHash,
											&newpredlocktag,
											PredicateLockHashCodeFromTargetHashCode(&newpredlocktag,
																					newtargettaghash),
											HASH_ENTER_NULL,
											&found);
			if (!newpredlock)
			{
				/* Out of shared memory. Undo what we've done so far. */
				LWLockRelease(SerializableXactHashLock);
				DeleteLockTarget(newtarget, newtargettaghash);
				outOfShmem = true;
				goto exit;
			}
			if (!found)
			{
				dlist_push_tail(&(newtarget->predicateLocks),
								&(newpredlock->targetLink));
				dlist_push_tail(&(newpredlocktag.myXact->predicateLocks),
								&(newpredlock->xactLink));
				newpredlock->commitSeqNo = oldCommitSeqNo;
			}
			else
			{
				if (newpredlock->commitSeqNo < oldCommitSeqNo)
					newpredlock->commitSeqNo = oldCommitSeqNo;
			}

			Assert(newpredlock->commitSeqNo != 0);
			Assert((newpredlock->commitSeqNo == InvalidSerCommitSeqNo)
				   || (newpredlock->tag.myXact == OldCommittedSxact));
		}
		LWLockRelease(SerializableXactHashLock);

		if (removeOld)
		{
			Assert(dlist_is_empty(&oldtarget->predicateLocks));
			RemoveTargetIfNoLongerUsed(oldtarget, oldtargettaghash);
		}
	}


exit:
	/* Release partition locks in reverse order of acquisition. */
	if (oldpartitionLock < newpartitionLock)
	{
		LWLockRelease(newpartitionLock);
		LWLockRelease(oldpartitionLock);
	}
	else if (oldpartitionLock > newpartitionLock)
	{
		LWLockRelease(oldpartitionLock);
		LWLockRelease(newpartitionLock);
	}
	else
		LWLockRelease(newpartitionLock);

	if (removeOld)
	{
		/* We shouldn't run out of memory if we're moving locks */
		Assert(!outOfShmem);

		/* Put the scratch entry back */
		RestoreScratchTarget(false);
	}

	return !outOfShmem;
}

/*
 * Drop all predicate locks of any granularity from the specified relation,
 * which can be a heap relation or an index relation.  If 'transfer' is true,
 * acquire a relation lock on the heap for any transactions with any lock(s)
 * on the specified relation.
 *
 * This requires grabbing a lot of LW locks and scanning the entire lock
 * target table for matches.  That makes this more expensive than most
 * predicate lock management functions, but it will only be called for DDL
 * type commands that are expensive anyway, and there are fast returns when
 * no serializable transactions are active or the relation is temporary.
 *
 * We don't use the TransferPredicateLocksToNewTarget function because it
 * acquires its own locks on the partitions of the two targets involved,
 * and we'll already be holding all partition locks.
 *
 * We can't throw an error from here, because the call could be from a
 * transaction which is not serializable.
 *
 * NOTE: This is currently only called with transfer set to true, but that may
 * change.  If we decide to clean up the locks from a table on commit of a
 * transaction which executed DROP TABLE, the false condition will be useful.
 */
static void
DropAllPredicateLocksFromTable(Relation relation, bool transfer)
{
	HASH_SEQ_STATUS seqstat;
	PREDICATELOCKTARGET *oldtarget;
	PREDICATELOCKTARGET *heaptarget;
	Oid			dbId;
	Oid			relId;
	Oid			heapId;
	int			i;
	bool		isIndex;
	bool		found;
	uint32		heaptargettaghash;

	/*
	 * Bail out quickly if there are no serializable transactions running.
	 * It's safe to check this without taking locks because the caller is
	 * holding an ACCESS EXCLUSIVE lock on the relation.  No new locks which
	 * would matter here can be acquired while that is held.
	 */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
		return;

	if (!PredicateLockingNeededForRelation(relation))
		return;

	dbId = relation->rd_locator.dbOid;
	relId = relation->rd_id;
	if (relation->rd_index == NULL)
	{
		isIndex = false;
		heapId = relId;
	}
	else
	{
		isIndex = true;
		heapId = relation->rd_index->indrelid;
	}
	Assert(heapId != InvalidOid);
	Assert(transfer || !isIndex);	/* index OID only makes sense with
									 * transfer */

	/* Retrieve first time needed, then keep. */
	heaptargettaghash = 0;
	heaptarget = NULL;

	/* Acquire locks on all lock partitions */
	LWLockAcquire(SerializablePredicateListLock, LW_EXCLUSIVE);
	for (i = 0; i < NUM_PREDICATELOCK_PARTITIONS; i++)
		LWLockAcquire(PredicateLockHashPartitionLockByIndex(i), LW_EXCLUSIVE);
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/*
	 * Remove the dummy entry to give us scratch space, so we know we'll be
	 * able to create the new lock target.
	 */
	if (transfer)
		RemoveScratchTarget(true);

	/* Scan through target map */
	hash_seq_init(&seqstat, PredicateLockTargetHash);

	while ((oldtarget = (PREDICATELOCKTARGET *) hash_seq_search(&seqstat)))
	{
		dlist_mutable_iter iter;

		/*
		 * Check whether this is a target which needs attention.
		 */
		if (GET_PREDICATELOCKTARGETTAG_RELATION(oldtarget->tag) != relId)
			continue;			/* wrong relation id */
		if (GET_PREDICATELOCKTARGETTAG_DB(oldtarget->tag) != dbId)
			continue;			/* wrong database id */
		if (transfer && !isIndex
			&& GET_PREDICATELOCKTARGETTAG_TYPE(oldtarget->tag) == PREDLOCKTAG_RELATION)
			continue;			/* already the right lock */

		/*
		 * If we made it here, we have work to do.  We make sure the heap
		 * relation lock exists, then we walk the list of predicate locks for
		 * the old target we found, moving all locks to the heap relation lock
		 * -- unless they already hold that.
		 */

		/*
		 * First make sure we have the heap relation target.  We only need to
		 * do this once.
		 */
		if (transfer && heaptarget == NULL)
		{
			PREDICATELOCKTARGETTAG heaptargettag;

			SET_PREDICATELOCKTARGETTAG_RELATION(heaptargettag, dbId, heapId);
			heaptargettaghash = PredicateLockTargetTagHashCode(&heaptargettag);
			heaptarget = hash_search_with_hash_value(PredicateLockTargetHash,
													 &heaptargettag,
													 heaptargettaghash,
													 HASH_ENTER, &found);
			if (!found)
				dlist_init(&heaptarget->predicateLocks);
		}

		/*
		 * Loop through all the locks on the old target, replacing them with
		 * locks on the new target.
		 */
		dlist_foreach_modify(iter, &oldtarget->predicateLocks)
		{
			PREDICATELOCK *oldpredlock =
				dlist_container(PREDICATELOCK, targetLink, iter.cur);
			PREDICATELOCK *newpredlock;
			SerCommitSeqNo oldCommitSeqNo;
			SERIALIZABLEXACT *oldXact;

			/*
			 * Remove the old lock first. This avoids the chance of running
			 * out of lock structure entries for the hash table.
			 */
			oldCommitSeqNo = oldpredlock->commitSeqNo;
			oldXact = oldpredlock->tag.myXact;

			dlist_delete(&(oldpredlock->xactLink));

			/*
			 * No need for retail delete from oldtarget list, we're removing
			 * the whole target anyway.
			 */
			hash_search(PredicateLockHash,
						&oldpredlock->tag,
						HASH_REMOVE, &found);
			Assert(found);

			if (transfer)
			{
				PREDICATELOCKTAG newpredlocktag;

				newpredlocktag.myTarget = heaptarget;
				newpredlocktag.myXact = oldXact;
				newpredlock = (PREDICATELOCK *)
					hash_search_with_hash_value(PredicateLockHash,
												&newpredlocktag,
												PredicateLockHashCodeFromTargetHashCode(&newpredlocktag,
																						heaptargettaghash),
												HASH_ENTER,
												&found);
				if (!found)
				{
					dlist_push_tail(&(heaptarget->predicateLocks),
									&(newpredlock->targetLink));
					dlist_push_tail(&(newpredlocktag.myXact->predicateLocks),
									&(newpredlock->xactLink));
					newpredlock->commitSeqNo = oldCommitSeqNo;
				}
				else
				{
					if (newpredlock->commitSeqNo < oldCommitSeqNo)
						newpredlock->commitSeqNo = oldCommitSeqNo;
				}

				Assert(newpredlock->commitSeqNo != 0);
				Assert((newpredlock->commitSeqNo == InvalidSerCommitSeqNo)
					   || (newpredlock->tag.myXact == OldCommittedSxact));
			}
		}

		hash_search(PredicateLockTargetHash, &oldtarget->tag, HASH_REMOVE,
					&found);
		Assert(found);
	}

	/* Put the scratch entry back */
	if (transfer)
		RestoreScratchTarget(true);

	/* Release locks in reverse order */
	LWLockRelease(SerializableXactHashLock);
	for (i = NUM_PREDICATELOCK_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(PredicateLockHashPartitionLockByIndex(i));
	LWLockRelease(SerializablePredicateListLock);
}

/*
 * TransferPredicateLocksToHeapRelation
 *		For all transactions, transfer all predicate locks for the given
 *		relation to a single relation lock on the heap.
 */
void
TransferPredicateLocksToHeapRelation(Relation relation)
{
	DropAllPredicateLocksFromTable(relation, true);
}


/*
 *		PredicateLockPageSplit
 *
 * Copies any predicate locks for the old page to the new page.
 * Skip if this is a temporary table or toast table.
 *
 * NOTE: A page split (or overflow) affects all serializable transactions,
 * even if it occurs in the context of another transaction isolation level.
 *
 * NOTE: This currently leaves the local copy of the locks without
 * information on the new lock which is in shared memory.  This could cause
 * problems if enough page splits occur on locked pages without the processes
 * which hold the locks getting in and noticing.
 */
void
PredicateLockPageSplit(Relation relation, BlockNumber oldblkno,
					   BlockNumber newblkno)
{
	PREDICATELOCKTARGETTAG oldtargettag;
	PREDICATELOCKTARGETTAG newtargettag;
	bool		success;

	/*
	 * Bail out quickly if there are no serializable transactions running.
	 *
	 * It's safe to do this check without taking any additional locks. Even if
	 * a serializable transaction starts concurrently, we know it can't take
	 * any SIREAD locks on the page being split because the caller is holding
	 * the associated buffer page lock. Memory reordering isn't an issue; the
	 * memory barrier in the LWLock acquisition guarantees that this read
	 * occurs while the buffer page lock is held.
	 */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
		return;

	if (!PredicateLockingNeededForRelation(relation))
		return;

	Assert(oldblkno != newblkno);
	Assert(BlockNumberIsValid(oldblkno));
	Assert(BlockNumberIsValid(newblkno));

	SET_PREDICATELOCKTARGETTAG_PAGE(oldtargettag,
									relation->rd_locator.dbOid,
									relation->rd_id,
									oldblkno);
	SET_PREDICATELOCKTARGETTAG_PAGE(newtargettag,
									relation->rd_locator.dbOid,
									relation->rd_id,
									newblkno);

	LWLockAcquire(SerializablePredicateListLock, LW_EXCLUSIVE);

	/*
	 * Try copying the locks over to the new page's tag, creating it if
	 * necessary.
	 */
	success = TransferPredicateLocksToNewTarget(oldtargettag,
												newtargettag,
												false);

	if (!success)
	{
		/*
		 * No more predicate lock entries are available. Failure isn't an
		 * option here, so promote the page lock to a relation lock.
		 */

		/* Get the parent relation lock's lock tag */
		success = GetParentPredicateLockTag(&oldtargettag,
											&newtargettag);
		Assert(success);

		/*
		 * Move the locks to the parent. This shouldn't fail.
		 *
		 * Note that here we are removing locks held by other backends,
		 * leading to a possible inconsistency in their local lock hash table.
		 * This is OK because we're replacing it with a lock that covers the
		 * old one.
		 */
		success = TransferPredicateLocksToNewTarget(oldtargettag,
													newtargettag,
													true);
		Assert(success);
	}

	LWLockRelease(SerializablePredicateListLock);
}

/*
 *		PredicateLockPageCombine
 *
 * Combines predicate locks for two existing pages.
 * Skip if this is a temporary table or toast table.
 *
 * NOTE: A page combine affects all serializable transactions, even if it
 * occurs in the context of another transaction isolation level.
 */
void
PredicateLockPageCombine(Relation relation, BlockNumber oldblkno,
						 BlockNumber newblkno)
{
	/*
	 * Page combines differ from page splits in that we ought to be able to
	 * remove the locks on the old page after transferring them to the new
	 * page, instead of duplicating them. However, because we can't edit other
	 * backends' local lock tables, removing the old lock would leave them
	 * with an entry in their LocalPredicateLockHash for a lock they're not
	 * holding, which isn't acceptable. So we wind up having to do the same
	 * work as a page split, acquiring a lock on the new page and keeping the
	 * old page locked too. That can lead to some false positives, but should
	 * be rare in practice.
	 */
	PredicateLockPageSplit(relation, oldblkno, newblkno);
}

/*
 * Walk the list of in-progress serializable transactions and find the new
 * xmin.
 */
static void
SetNewSxactGlobalXmin(void)
{
	dlist_iter	iter;

	Assert(LWLockHeldByMe(SerializableXactHashLock));

	PredXact->SxactGlobalXmin = InvalidTransactionId;
	PredXact->SxactGlobalXminCount = 0;

	dlist_foreach(iter, &PredXact->activeList)
	{
		SERIALIZABLEXACT *sxact =
			dlist_container(SERIALIZABLEXACT, xactLink, iter.cur);

		if (!SxactIsRolledBack(sxact)
			&& !SxactIsCommitted(sxact)
			&& sxact != OldCommittedSxact)
		{
			Assert(sxact->xmin != InvalidTransactionId);
			if (!TransactionIdIsValid(PredXact->SxactGlobalXmin)
				|| TransactionIdPrecedes(sxact->xmin,
										 PredXact->SxactGlobalXmin))
			{
				PredXact->SxactGlobalXmin = sxact->xmin;
				PredXact->SxactGlobalXminCount = 1;
			}
			else if (TransactionIdEquals(sxact->xmin,
										 PredXact->SxactGlobalXmin))
				PredXact->SxactGlobalXminCount++;
		}
	}

	SerialSetActiveSerXmin(PredXact->SxactGlobalXmin);
}

/*
 *		ReleasePredicateLocks
 *
 * Releases predicate locks based on completion of the current transaction,
 * whether committed or rolled back.  It can also be called for a read only
 * transaction when it becomes impossible for the transaction to become
 * part of a dangerous structure.
 *
 * We do nothing unless this is a serializable transaction.
 *
 * This method must ensure that shared memory hash tables are cleaned
 * up in some relatively timely fashion.
 *
 * If this transaction is committing and is holding any predicate locks,
 * it must be added to a list of completed serializable transactions still
 * holding locks.
 *
 * If isReadOnlySafe is true, then predicate locks are being released before
 * the end of the transaction because MySerializableXact has been determined
 * to be RO_SAFE.  In non-parallel mode we can release it completely, but it
 * in parallel mode we partially release the SERIALIZABLEXACT and keep it
 * around until the end of the transaction, allowing each backend to clear its
 * MySerializableXact variable and benefit from the optimization in its own
 * time.
 */
void
ReleasePredicateLocks(bool isCommit, bool isReadOnlySafe)
{
	bool		partiallyReleasing = false;
	bool		needToClear;
	SERIALIZABLEXACT *roXact;
	dlist_mutable_iter iter;

	/*
	 * We can't trust XactReadOnly here, because a transaction which started
	 * as READ WRITE can show as READ ONLY later, e.g., within
	 * subtransactions.  We want to flag a transaction as READ ONLY if it
	 * commits without writing so that de facto READ ONLY transactions get the
	 * benefit of some RO optimizations, so we will use this local variable to
	 * get some cleanup logic right which is based on whether the transaction
	 * was declared READ ONLY at the top level.
	 */
	bool		topLevelIsDeclaredReadOnly;

	/* We can't be both committing and releasing early due to RO_SAFE. */
	Assert(!(isCommit && isReadOnlySafe));

	/* Are we at the end of a transaction, that is, a commit or abort? */
	if (!isReadOnlySafe)
	{
		/*
		 * Parallel workers mustn't release predicate locks at the end of
		 * their transaction.  The leader will do that at the end of its
		 * transaction.
		 */
		if (IsParallelWorker())
		{
			ReleasePredicateLocksLocal();
			return;
		}

		/*
		 * By the time the leader in a parallel query reaches end of
		 * transaction, it has waited for all workers to exit.
		 */
		Assert(!ParallelContextActive());

		/*
		 * If the leader in a parallel query earlier stashed a partially
		 * released SERIALIZABLEXACT for final clean-up at end of transaction
		 * (because workers might still have been accessing it), then it's
		 * time to restore it.
		 */
		if (SavedSerializableXact != InvalidSerializableXact)
		{
			Assert(MySerializableXact == InvalidSerializableXact);
			MySerializableXact = SavedSerializableXact;
			SavedSerializableXact = InvalidSerializableXact;
			Assert(SxactIsPartiallyReleased(MySerializableXact));
		}
	}

	if (MySerializableXact == InvalidSerializableXact)
	{
		Assert(LocalPredicateLockHash == NULL);
		return;
	}

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/*
	 * If the transaction is committing, but it has been partially released
	 * already, then treat this as a roll back.  It was marked as rolled back.
	 */
	if (isCommit && SxactIsPartiallyReleased(MySerializableXact))
		isCommit = false;

	/*
	 * If we're called in the middle of a transaction because we discovered
	 * that the SXACT_FLAG_RO_SAFE flag was set, then we'll partially release
	 * it (that is, release the predicate locks and conflicts, but not the
	 * SERIALIZABLEXACT itself) if we're the first backend to have noticed.
	 */
	if (isReadOnlySafe && IsInParallelMode())
	{
		/*
		 * The leader needs to stash a pointer to it, so that it can
		 * completely release it at end-of-transaction.
		 */
		if (!IsParallelWorker())
			SavedSerializableXact = MySerializableXact;

		/*
		 * The first backend to reach this condition will partially release
		 * the SERIALIZABLEXACT.  All others will just clear their
		 * backend-local state so that they stop doing SSI checks for the rest
		 * of the transaction.
		 */
		if (SxactIsPartiallyReleased(MySerializableXact))
		{
			LWLockRelease(SerializableXactHashLock);
			ReleasePredicateLocksLocal();
			return;
		}
		else
		{
			MySerializableXact->flags |= SXACT_FLAG_PARTIALLY_RELEASED;
			partiallyReleasing = true;
			/* ... and proceed to perform the partial release below. */
		}
	}
	Assert(!isCommit || SxactIsPrepared(MySerializableXact));
	Assert(!isCommit || !SxactIsDoomed(MySerializableXact));
	Assert(!SxactIsCommitted(MySerializableXact));
	Assert(SxactIsPartiallyReleased(MySerializableXact)
		   || !SxactIsRolledBack(MySerializableXact));

	/* may not be serializable during COMMIT/ROLLBACK PREPARED */
	Assert(MySerializableXact->pid == 0 || IsolationIsSerializable());

	/* We'd better not already be on the cleanup list. */
	Assert(!SxactIsOnFinishedList(MySerializableXact));

	topLevelIsDeclaredReadOnly = SxactIsReadOnly(MySerializableXact);

	/*
	 * We don't hold XidGenLock lock here, assuming that TransactionId is
	 * atomic!
	 *
	 * If this value is changing, we don't care that much whether we get the
	 * old or new value -- it is just used to determine how far
	 * SxactGlobalXmin must advance before this transaction can be fully
	 * cleaned up.  The worst that could happen is we wait for one more
	 * transaction to complete before freeing some RAM; correctness of visible
	 * behavior is not affected.
	 */
	MySerializableXact->finishedBefore = XidFromFullTransactionId(TransamVariables->nextXid);

	/*
	 * If it's not a commit it's either a rollback or a read-only transaction
	 * flagged SXACT_FLAG_RO_SAFE, and we can clear our locks immediately.
	 */
	if (isCommit)
	{
		MySerializableXact->flags |= SXACT_FLAG_COMMITTED;
		MySerializableXact->commitSeqNo = ++(PredXact->LastSxactCommitSeqNo);
		/* Recognize implicit read-only transaction (commit without write). */
		if (!MyXactDidWrite)
			MySerializableXact->flags |= SXACT_FLAG_READ_ONLY;
	}
	else
	{
		/*
		 * The DOOMED flag indicates that we intend to roll back this
		 * transaction and so it should not cause serialization failures for
		 * other transactions that conflict with it. Note that this flag might
		 * already be set, if another backend marked this transaction for
		 * abort.
		 *
		 * The ROLLED_BACK flag further indicates that ReleasePredicateLocks
		 * has been called, and so the SerializableXact is eligible for
		 * cleanup. This means it should not be considered when calculating
		 * SxactGlobalXmin.
		 */
		MySerializableXact->flags |= SXACT_FLAG_DOOMED;
		MySerializableXact->flags |= SXACT_FLAG_ROLLED_BACK;

		/*
		 * If the transaction was previously prepared, but is now failing due
		 * to a ROLLBACK PREPARED or (hopefully very rare) error after the
		 * prepare, clear the prepared flag.  This simplifies conflict
		 * checking.
		 */
		MySerializableXact->flags &= ~SXACT_FLAG_PREPARED;
	}

	if (!topLevelIsDeclaredReadOnly)
	{
		Assert(PredXact->WritableSxactCount > 0);
		if (--(PredXact->WritableSxactCount) == 0)
		{
			/*
			 * Release predicate locks and rw-conflicts in for all committed
			 * transactions.  There are no longer any transactions which might
			 * conflict with the locks and no chance for new transactions to
			 * overlap.  Similarly, existing conflicts in can't cause pivots,
			 * and any conflicts in which could have completed a dangerous
			 * structure would already have caused a rollback, so any
			 * remaining ones must be benign.
			 */
			PredXact->CanPartialClearThrough = PredXact->LastSxactCommitSeqNo;
		}
	}
	else
	{
		/*
		 * Read-only transactions: clear the list of transactions that might
		 * make us unsafe. Note that we use 'inLink' for the iteration as
		 * opposed to 'outLink' for the r/w xacts.
		 */
		dlist_foreach_modify(iter, &MySerializableXact->possibleUnsafeConflicts)
		{
			RWConflict	possibleUnsafeConflict =
				dlist_container(RWConflictData, inLink, iter.cur);

			Assert(!SxactIsReadOnly(possibleUnsafeConflict->sxactOut));
			Assert(MySerializableXact == possibleUnsafeConflict->sxactIn);

			ReleaseRWConflict(possibleUnsafeConflict);
		}
	}

	/* Check for conflict out to old committed transactions. */
	if (isCommit
		&& !SxactIsReadOnly(MySerializableXact)
		&& SxactHasSummaryConflictOut(MySerializableXact))
	{
		/*
		 * we don't know which old committed transaction we conflicted with,
		 * so be conservative and use FirstNormalSerCommitSeqNo here
		 */
		MySerializableXact->SeqNo.earliestOutConflictCommit =
			FirstNormalSerCommitSeqNo;
		MySerializableXact->flags |= SXACT_FLAG_CONFLICT_OUT;
	}

	/*
	 * Release all outConflicts to committed transactions.  If we're rolling
	 * back clear them all.  Set SXACT_FLAG_CONFLICT_OUT if any point to
	 * previously committed transactions.
	 */
	dlist_foreach_modify(iter, &MySerializableXact->outConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, outLink, iter.cur);

		if (isCommit
			&& !SxactIsReadOnly(MySerializableXact)
			&& SxactIsCommitted(conflict->sxactIn))
		{
			if ((MySerializableXact->flags & SXACT_FLAG_CONFLICT_OUT) == 0
				|| conflict->sxactIn->prepareSeqNo < MySerializableXact->SeqNo.earliestOutConflictCommit)
				MySerializableXact->SeqNo.earliestOutConflictCommit = conflict->sxactIn->prepareSeqNo;
			MySerializableXact->flags |= SXACT_FLAG_CONFLICT_OUT;
		}

		if (!isCommit
			|| SxactIsCommitted(conflict->sxactIn)
			|| (conflict->sxactIn->SeqNo.lastCommitBeforeSnapshot >= PredXact->LastSxactCommitSeqNo))
			ReleaseRWConflict(conflict);
	}

	/*
	 * Release all inConflicts from committed and read-only transactions. If
	 * we're rolling back, clear them all.
	 */
	dlist_foreach_modify(iter, &MySerializableXact->inConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, inLink, iter.cur);

		if (!isCommit
			|| SxactIsCommitted(conflict->sxactOut)
			|| SxactIsReadOnly(conflict->sxactOut))
			ReleaseRWConflict(conflict);
	}

	if (!topLevelIsDeclaredReadOnly)
	{
		/*
		 * Remove ourselves from the list of possible conflicts for concurrent
		 * READ ONLY transactions, flagging them as unsafe if we have a
		 * conflict out. If any are waiting DEFERRABLE transactions, wake them
		 * up if they are known safe or known unsafe.
		 */
		dlist_foreach_modify(iter, &MySerializableXact->possibleUnsafeConflicts)
		{
			RWConflict	possibleUnsafeConflict =
				dlist_container(RWConflictData, outLink, iter.cur);

			roXact = possibleUnsafeConflict->sxactIn;
			Assert(MySerializableXact == possibleUnsafeConflict->sxactOut);
			Assert(SxactIsReadOnly(roXact));

			/* Mark conflicted if necessary. */
			if (isCommit
				&& MyXactDidWrite
				&& SxactHasConflictOut(MySerializableXact)
				&& (MySerializableXact->SeqNo.earliestOutConflictCommit
					<= roXact->SeqNo.lastCommitBeforeSnapshot))
			{
				/*
				 * This releases possibleUnsafeConflict (as well as all other
				 * possible conflicts for roXact)
				 */
				FlagSxactUnsafe(roXact);
			}
			else
			{
				ReleaseRWConflict(possibleUnsafeConflict);

				/*
				 * If we were the last possible conflict, flag it safe. The
				 * transaction can now safely release its predicate locks (but
				 * that transaction's backend has to do that itself).
				 */
				if (dlist_is_empty(&roXact->possibleUnsafeConflicts))
					roXact->flags |= SXACT_FLAG_RO_SAFE;
			}

			/*
			 * Wake up the process for a waiting DEFERRABLE transaction if we
			 * now know it's either safe or conflicted.
			 */
			if (SxactIsDeferrableWaiting(roXact) &&
				(SxactIsROUnsafe(roXact) || SxactIsROSafe(roXact)))
				ProcSendSignal(roXact->pgprocno);
		}
	}

	/*
	 * Check whether it's time to clean up old transactions. This can only be
	 * done when the last serializable transaction with the oldest xmin among
	 * serializable transactions completes.  We then find the "new oldest"
	 * xmin and purge any transactions which finished before this transaction
	 * was launched.
	 *
	 * For parallel queries in read-only transactions, it might run twice. We
	 * only release the reference on the first call.
	 */
	needToClear = false;
	if ((partiallyReleasing ||
		 !SxactIsPartiallyReleased(MySerializableXact)) &&
		TransactionIdEquals(MySerializableXact->xmin,
							PredXact->SxactGlobalXmin))
	{
		Assert(PredXact->SxactGlobalXminCount > 0);
		if (--(PredXact->SxactGlobalXminCount) == 0)
		{
			SetNewSxactGlobalXmin();
			needToClear = true;
		}
	}

	LWLockRelease(SerializableXactHashLock);

	LWLockAcquire(SerializableFinishedListLock, LW_EXCLUSIVE);

	/* Add this to the list of transactions to check for later cleanup. */
	if (isCommit)
		dlist_push_tail(FinishedSerializableTransactions,
						&MySerializableXact->finishedLink);

	/*
	 * If we're releasing a RO_SAFE transaction in parallel mode, we'll only
	 * partially release it.  That's necessary because other backends may have
	 * a reference to it.  The leader will release the SERIALIZABLEXACT itself
	 * at the end of the transaction after workers have stopped running.
	 */
	if (!isCommit)
		ReleaseOneSerializableXact(MySerializableXact,
								   isReadOnlySafe && IsInParallelMode(),
								   false);

	LWLockRelease(SerializableFinishedListLock);

	if (needToClear)
		ClearOldPredicateLocks();

	ReleasePredicateLocksLocal();
}

static void
ReleasePredicateLocksLocal(void)
{
	MySerializableXact = InvalidSerializableXact;
	MyXactDidWrite = false;

	/* Delete per-transaction lock table */
	if (LocalPredicateLockHash != NULL)
	{
		hash_destroy(LocalPredicateLockHash);
		LocalPredicateLockHash = NULL;
	}
}

/*
 * Clear old predicate locks, belonging to committed transactions that are no
 * longer interesting to any in-progress transaction.
 */
static void
ClearOldPredicateLocks(void)
{
	dlist_mutable_iter iter;

	/*
	 * Loop through finished transactions. They are in commit order, so we can
	 * stop as soon as we find one that's still interesting.
	 */
	LWLockAcquire(SerializableFinishedListLock, LW_EXCLUSIVE);
	LWLockAcquire(SerializableXactHashLock, LW_SHARED);
	dlist_foreach_modify(iter, FinishedSerializableTransactions)
	{
		SERIALIZABLEXACT *finishedSxact =
			dlist_container(SERIALIZABLEXACT, finishedLink, iter.cur);

		if (!TransactionIdIsValid(PredXact->SxactGlobalXmin)
			|| TransactionIdPrecedesOrEquals(finishedSxact->finishedBefore,
											 PredXact->SxactGlobalXmin))
		{
			/*
			 * This transaction committed before any in-progress transaction
			 * took its snapshot. It's no longer interesting.
			 */
			LWLockRelease(SerializableXactHashLock);
			dlist_delete_thoroughly(&finishedSxact->finishedLink);
			ReleaseOneSerializableXact(finishedSxact, false, false);
			LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		}
		else if (finishedSxact->commitSeqNo > PredXact->HavePartialClearedThrough
				 && finishedSxact->commitSeqNo <= PredXact->CanPartialClearThrough)
		{
			/*
			 * Any active transactions that took their snapshot before this
			 * transaction committed are read-only, so we can clear part of
			 * its state.
			 */
			LWLockRelease(SerializableXactHashLock);

			if (SxactIsReadOnly(finishedSxact))
			{
				/* A read-only transaction can be removed entirely */
				dlist_delete_thoroughly(&(finishedSxact->finishedLink));
				ReleaseOneSerializableXact(finishedSxact, false, false);
			}
			else
			{
				/*
				 * A read-write transaction can only be partially cleared. We
				 * need to keep the SERIALIZABLEXACT but can release the
				 * SIREAD locks and conflicts in.
				 */
				ReleaseOneSerializableXact(finishedSxact, true, false);
			}

			PredXact->HavePartialClearedThrough = finishedSxact->commitSeqNo;
			LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		}
		else
		{
			/* Still interesting. */
			break;
		}
	}
	LWLockRelease(SerializableXactHashLock);

	/*
	 * Loop through predicate locks on dummy transaction for summarized data.
	 */
	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
	dlist_foreach_modify(iter, &OldCommittedSxact->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, xactLink, iter.cur);
		bool		canDoPartialCleanup;

		LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		Assert(predlock->commitSeqNo != 0);
		Assert(predlock->commitSeqNo != InvalidSerCommitSeqNo);
		canDoPartialCleanup = (predlock->commitSeqNo <= PredXact->CanPartialClearThrough);
		LWLockRelease(SerializableXactHashLock);

		/*
		 * If this lock originally belonged to an old enough transaction, we
		 * can release it.
		 */
		if (canDoPartialCleanup)
		{
			PREDICATELOCKTAG tag;
			PREDICATELOCKTARGET *target;
			PREDICATELOCKTARGETTAG targettag;
			uint32		targettaghash;
			LWLock	   *partitionLock;

			tag = predlock->tag;
			target = tag.myTarget;
			targettag = target->tag;
			targettaghash = PredicateLockTargetTagHashCode(&targettag);
			partitionLock = PredicateLockHashPartitionLock(targettaghash);

			LWLockAcquire(partitionLock, LW_EXCLUSIVE);

			dlist_delete(&(predlock->targetLink));
			dlist_delete(&(predlock->xactLink));

			hash_search_with_hash_value(PredicateLockHash, &tag,
										PredicateLockHashCodeFromTargetHashCode(&tag,
																				targettaghash),
										HASH_REMOVE, NULL);
			RemoveTargetIfNoLongerUsed(target, targettaghash);

			LWLockRelease(partitionLock);
		}
	}

	LWLockRelease(SerializablePredicateListLock);
	LWLockRelease(SerializableFinishedListLock);
}

/*
 * This is the normal way to delete anything from any of the predicate
 * locking hash tables.  Given a transaction which we know can be deleted:
 * delete all predicate locks held by that transaction and any predicate
 * lock targets which are now unreferenced by a lock; delete all conflicts
 * for the transaction; delete all xid values for the transaction; then
 * delete the transaction.
 *
 * When the partial flag is set, we can release all predicate locks and
 * in-conflict information -- we've established that there are no longer
 * any overlapping read write transactions for which this transaction could
 * matter -- but keep the transaction entry itself and any outConflicts.
 *
 * When the summarize flag is set, we've run short of room for sxact data
 * and must summarize to the SLRU.  Predicate locks are transferred to a
 * dummy "old" transaction, with duplicate locks on a single target
 * collapsing to a single lock with the "latest" commitSeqNo from among
 * the conflicting locks..
 */
static void
ReleaseOneSerializableXact(SERIALIZABLEXACT *sxact, bool partial,
						   bool summarize)
{
	SERIALIZABLEXIDTAG sxidtag;
	dlist_mutable_iter iter;

	Assert(sxact != NULL);
	Assert(SxactIsRolledBack(sxact) || SxactIsCommitted(sxact));
	Assert(partial || !SxactIsOnFinishedList(sxact));
	Assert(LWLockHeldByMe(SerializableFinishedListLock));

	/*
	 * First release all the predicate locks held by this xact (or transfer
	 * them to OldCommittedSxact if summarize is true)
	 */
	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
	if (IsInParallelMode())
		LWLockAcquire(&sxact->perXactPredicateListLock, LW_EXCLUSIVE);
	dlist_foreach_modify(iter, &sxact->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, xactLink, iter.cur);
		PREDICATELOCKTAG tag;
		PREDICATELOCKTARGET *target;
		PREDICATELOCKTARGETTAG targettag;
		uint32		targettaghash;
		LWLock	   *partitionLock;

		tag = predlock->tag;
		target = tag.myTarget;
		targettag = target->tag;
		targettaghash = PredicateLockTargetTagHashCode(&targettag);
		partitionLock = PredicateLockHashPartitionLock(targettaghash);

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		dlist_delete(&predlock->targetLink);

		hash_search_with_hash_value(PredicateLockHash, &tag,
									PredicateLockHashCodeFromTargetHashCode(&tag,
																			targettaghash),
									HASH_REMOVE, NULL);
		if (summarize)
		{
			bool		found;

			/* Fold into dummy transaction list. */
			tag.myXact = OldCommittedSxact;
			predlock = hash_search_with_hash_value(PredicateLockHash, &tag,
												   PredicateLockHashCodeFromTargetHashCode(&tag,
																						   targettaghash),
												   HASH_ENTER_NULL, &found);
			if (!predlock)
				ereport(ERROR,
						(errcode(ERRCODE_OUT_OF_MEMORY),
						 errmsg("out of shared memory"),
						 errhint("You might need to increase \"%s\".", "max_pred_locks_per_transaction")));
			if (found)
			{
				Assert(predlock->commitSeqNo != 0);
				Assert(predlock->commitSeqNo != InvalidSerCommitSeqNo);
				if (predlock->commitSeqNo < sxact->commitSeqNo)
					predlock->commitSeqNo = sxact->commitSeqNo;
			}
			else
			{
				dlist_push_tail(&target->predicateLocks,
								&predlock->targetLink);
				dlist_push_tail(&OldCommittedSxact->predicateLocks,
								&predlock->xactLink);
				predlock->commitSeqNo = sxact->commitSeqNo;
			}
		}
		else
			RemoveTargetIfNoLongerUsed(target, targettaghash);

		LWLockRelease(partitionLock);
	}

	/*
	 * Rather than retail removal, just re-init the head after we've run
	 * through the list.
	 */
	dlist_init(&sxact->predicateLocks);

	if (IsInParallelMode())
		LWLockRelease(&sxact->perXactPredicateListLock);
	LWLockRelease(SerializablePredicateListLock);

	sxidtag.xid = sxact->topXid;
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* Release all outConflicts (unless 'partial' is true) */
	if (!partial)
	{
		dlist_foreach_modify(iter, &sxact->outConflicts)
		{
			RWConflict	conflict =
				dlist_container(RWConflictData, outLink, iter.cur);

			if (summarize)
				conflict->sxactIn->flags |= SXACT_FLAG_SUMMARY_CONFLICT_IN;
			ReleaseRWConflict(conflict);
		}
	}

	/* Release all inConflicts. */
	dlist_foreach_modify(iter, &sxact->inConflicts)
	{
		RWConflict	conflict =
			dlist_container(RWConflictData, inLink, iter.cur);

		if (summarize)
			conflict->sxactOut->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;
		ReleaseRWConflict(conflict);
	}

	/* Finally, get rid of the xid and the record of the transaction itself. */
	if (!partial)
	{
		if (sxidtag.xid != InvalidTransactionId)
			hash_search(SerializableXidHash, &sxidtag, HASH_REMOVE, NULL);
		ReleasePredXact(sxact);
	}

	LWLockRelease(SerializableXactHashLock);
}

/*
 * Tests whether the given top level transaction is concurrent with
 * (overlaps) our current transaction.
 *
 * We need to identify the top level transaction for SSI, anyway, so pass
 * that to this function to save the overhead of checking the snapshot's
 * subxip array.
 */
static bool
XidIsConcurrent(TransactionId xid)
{
	Snapshot	snap;

	Assert(TransactionIdIsValid(xid));
	Assert(!TransactionIdEquals(xid, GetTopTransactionIdIfAny()));

	snap = GetTransactionSnapshot();

	if (TransactionIdPrecedes(xid, snap->xmin))
		return false;

	if (TransactionIdFollowsOrEquals(xid, snap->xmax))
		return true;

	return pg_lfind32(xid, snap->xip, snap->xcnt);
}

bool
CheckForSerializableConflictOutNeeded(Relation relation, Snapshot snapshot)
{
	if (!SerializationNeededForRead(relation, snapshot))
		return false;

	/* Check if someone else has already decided that we need to die */
	if (SxactIsDoomed(MySerializableXact))
	{
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during conflict out checking."),
				 errhint("The transaction might succeed if retried.")));
	}

	return true;
}

/*
 * CheckForSerializableConflictOut
 *		A table AM is reading a tuple that has been modified.  If it determines
 *		that the tuple version it is reading is not visible to us, it should
 *		pass in the top level xid of the transaction that created it.
 *		Otherwise, if it determines that it is visible to us but it has been
 *		deleted or there is a newer version available due to an update, it
 *		should pass in the top level xid of the modifying transaction.
 *
 * This function will check for overlap with our own transaction.  If the given
 * xid is also serializable and the transactions overlap (i.e., they cannot see
 * each other's writes), then we have a conflict out.
 */
void
CheckForSerializableConflictOut(Relation relation, TransactionId xid, Snapshot snapshot)
{
	SERIALIZABLEXIDTAG sxidtag;
	SERIALIZABLEXID *sxid;
	SERIALIZABLEXACT *sxact;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	/* Check if someone else has already decided that we need to die */
	if (SxactIsDoomed(MySerializableXact))
	{
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during conflict out checking."),
				 errhint("The transaction might succeed if retried.")));
	}
	Assert(TransactionIdIsValid(xid));

	if (TransactionIdEquals(xid, GetTopTransactionIdIfAny()))
		return;

	/*
	 * Find sxact or summarized info for the top level xid.
	 */
	sxidtag.xid = xid;
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
	sxid = (SERIALIZABLEXID *)
		hash_search(SerializableXidHash, &sxidtag, HASH_FIND, NULL);
	if (!sxid)
	{
		/*
		 * Transaction not found in "normal" SSI structures.  Check whether it
		 * got pushed out to SLRU storage for "old committed" transactions.
		 */
		SerCommitSeqNo conflictCommitSeqNo;

		conflictCommitSeqNo = SerialGetMinConflictCommitSeqNo(xid);
		if (conflictCommitSeqNo != 0)
		{
			if (conflictCommitSeqNo != InvalidSerCommitSeqNo
				&& (!SxactIsReadOnly(MySerializableXact)
					|| conflictCommitSeqNo
					<= MySerializableXact->SeqNo.lastCommitBeforeSnapshot))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to read/write dependencies among transactions"),
						 errdetail_internal("Reason code: Canceled on conflict out to old pivot %u.", xid),
						 errhint("The transaction might succeed if retried.")));

			if (SxactHasSummaryConflictIn(MySerializableXact)
				|| !dlist_is_empty(&MySerializableXact->inConflicts))
				ereport(ERROR,
						(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
						 errmsg("could not serialize access due to read/write dependencies among transactions"),
						 errdetail_internal("Reason code: Canceled on identification as a pivot, with conflict out to old committed transaction %u.", xid),
						 errhint("The transaction might succeed if retried.")));

			MySerializableXact->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;
		}

		/* It's not serializable or otherwise not important. */
		LWLockRelease(SerializableXactHashLock);
		return;
	}
	sxact = sxid->myXact;
	Assert(TransactionIdEquals(sxact->topXid, xid));
	if (sxact == MySerializableXact || SxactIsDoomed(sxact))
	{
		/* Can't conflict with ourself or a transaction that will roll back. */
		LWLockRelease(SerializableXactHashLock);
		return;
	}

	/*
	 * We have a conflict out to a transaction which has a conflict out to a
	 * summarized transaction.  That summarized transaction must have
	 * committed first, and we can't tell when it committed in relation to our
	 * snapshot acquisition, so something needs to be canceled.
	 */
	if (SxactHasSummaryConflictOut(sxact))
	{
		if (!SxactIsPrepared(sxact))
		{
			sxact->flags |= SXACT_FLAG_DOOMED;
			LWLockRelease(SerializableXactHashLock);
			return;
		}
		else
		{
			LWLockRelease(SerializableXactHashLock);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to read/write dependencies among transactions"),
					 errdetail_internal("Reason code: Canceled on conflict out to old pivot."),
					 errhint("The transaction might succeed if retried.")));
		}
	}

	/*
	 * If this is a read-only transaction and the writing transaction has
	 * committed, and it doesn't have a rw-conflict to a transaction which
	 * committed before it, no conflict.
	 */
	if (SxactIsReadOnly(MySerializableXact)
		&& SxactIsCommitted(sxact)
		&& !SxactHasSummaryConflictOut(sxact)
		&& (!SxactHasConflictOut(sxact)
			|| MySerializableXact->SeqNo.lastCommitBeforeSnapshot < sxact->SeqNo.earliestOutConflictCommit))
	{
		/* Read-only transaction will appear to run first.  No conflict. */
		LWLockRelease(SerializableXactHashLock);
		return;
	}

	if (!XidIsConcurrent(xid))
	{
		/* This write was already in our snapshot; no conflict. */
		LWLockRelease(SerializableXactHashLock);
		return;
	}

	if (RWConflictExists(MySerializableXact, sxact))
	{
		/* We don't want duplicate conflict records in the list. */
		LWLockRelease(SerializableXactHashLock);
		return;
	}

	/*
	 * Flag the conflict.  But first, if this conflict creates a dangerous
	 * structure, ereport an error.
	 */
	FlagRWConflict(MySerializableXact, sxact);
	LWLockRelease(SerializableXactHashLock);
}

/*
 * Check a particular target for rw-dependency conflict in. A subroutine of
 * CheckForSerializableConflictIn().
 */
static void
CheckTargetForConflictsIn(PREDICATELOCKTARGETTAG *targettag)
{
	uint32		targettaghash;
	LWLock	   *partitionLock;
	PREDICATELOCKTARGET *target;
	PREDICATELOCK *mypredlock = NULL;
	PREDICATELOCKTAG mypredlocktag;
	dlist_mutable_iter iter;

	Assert(MySerializableXact != InvalidSerializableXact);

	/*
	 * The same hash and LW lock apply to the lock target and the lock itself.
	 */
	targettaghash = PredicateLockTargetTagHashCode(targettag);
	partitionLock = PredicateLockHashPartitionLock(targettaghash);
	LWLockAcquire(partitionLock, LW_SHARED);
	target = (PREDICATELOCKTARGET *)
		hash_search_with_hash_value(PredicateLockTargetHash,
									targettag, targettaghash,
									HASH_FIND, NULL);
	if (!target)
	{
		/* Nothing has this target locked; we're done here. */
		LWLockRelease(partitionLock);
		return;
	}

	/*
	 * Each lock for an overlapping transaction represents a conflict: a
	 * rw-dependency in to this transaction.
	 */
	LWLockAcquire(SerializableXactHashLock, LW_SHARED);

	dlist_foreach_modify(iter, &target->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, targetLink, iter.cur);
		SERIALIZABLEXACT *sxact = predlock->tag.myXact;

		if (sxact == MySerializableXact)
		{
			/*
			 * If we're getting a write lock on a tuple, we don't need a
			 * predicate (SIREAD) lock on the same tuple. We can safely remove
			 * our SIREAD lock, but we'll defer doing so until after the loop
			 * because that requires upgrading to an exclusive partition lock.
			 *
			 * We can't use this optimization within a subtransaction because
			 * the subtransaction could roll back, and we would be left
			 * without any lock at the top level.
			 */
			if (!IsSubTransaction()
				&& GET_PREDICATELOCKTARGETTAG_OFFSET(*targettag))
			{
				mypredlock = predlock;
				mypredlocktag = predlock->tag;
			}
		}
		else if (!SxactIsDoomed(sxact)
				 && (!SxactIsCommitted(sxact)
					 || TransactionIdPrecedes(GetTransactionSnapshot()->xmin,
											  sxact->finishedBefore))
				 && !RWConflictExists(sxact, MySerializableXact))
		{
			LWLockRelease(SerializableXactHashLock);
			LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

			/*
			 * Re-check after getting exclusive lock because the other
			 * transaction may have flagged a conflict.
			 */
			if (!SxactIsDoomed(sxact)
				&& (!SxactIsCommitted(sxact)
					|| TransactionIdPrecedes(GetTransactionSnapshot()->xmin,
											 sxact->finishedBefore))
				&& !RWConflictExists(sxact, MySerializableXact))
			{
				FlagRWConflict(sxact, MySerializableXact);
			}

			LWLockRelease(SerializableXactHashLock);
			LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		}
	}
	LWLockRelease(SerializableXactHashLock);
	LWLockRelease(partitionLock);

	/*
	 * If we found one of our own SIREAD locks to remove, remove it now.
	 *
	 * At this point our transaction already has a RowExclusiveLock on the
	 * relation, so we are OK to drop the predicate lock on the tuple, if
	 * found, without fearing that another write against the tuple will occur
	 * before the MVCC information makes it to the buffer.
	 */
	if (mypredlock != NULL)
	{
		uint32		predlockhashcode;
		PREDICATELOCK *rmpredlock;

		LWLockAcquire(SerializablePredicateListLock, LW_SHARED);
		if (IsInParallelMode())
			LWLockAcquire(&MySerializableXact->perXactPredicateListLock, LW_EXCLUSIVE);
		LWLockAcquire(partitionLock, LW_EXCLUSIVE);
		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

		/*
		 * Remove the predicate lock from shared memory, if it wasn't removed
		 * while the locks were released.  One way that could happen is from
		 * autovacuum cleaning up an index.
		 */
		predlockhashcode = PredicateLockHashCodeFromTargetHashCode
			(&mypredlocktag, targettaghash);
		rmpredlock = (PREDICATELOCK *)
			hash_search_with_hash_value(PredicateLockHash,
										&mypredlocktag,
										predlockhashcode,
										HASH_FIND, NULL);
		if (rmpredlock != NULL)
		{
			Assert(rmpredlock == mypredlock);

			dlist_delete(&(mypredlock->targetLink));
			dlist_delete(&(mypredlock->xactLink));

			rmpredlock = (PREDICATELOCK *)
				hash_search_with_hash_value(PredicateLockHash,
											&mypredlocktag,
											predlockhashcode,
											HASH_REMOVE, NULL);
			Assert(rmpredlock == mypredlock);

			RemoveTargetIfNoLongerUsed(target, targettaghash);
		}

		LWLockRelease(SerializableXactHashLock);
		LWLockRelease(partitionLock);
		if (IsInParallelMode())
			LWLockRelease(&MySerializableXact->perXactPredicateListLock);
		LWLockRelease(SerializablePredicateListLock);

		if (rmpredlock != NULL)
		{
			/*
			 * Remove entry in local lock table if it exists. It's OK if it
			 * doesn't exist; that means the lock was transferred to a new
			 * target by a different backend.
			 */
			hash_search_with_hash_value(LocalPredicateLockHash,
										targettag, targettaghash,
										HASH_REMOVE, NULL);

			DecrementParentLocks(targettag);
		}
	}
}

/*
 * CheckForSerializableConflictIn
 *		We are writing the given tuple.  If that indicates a rw-conflict
 *		in from another serializable transaction, take appropriate action.
 *
 * Skip checking for any granularity for which a parameter is missing.
 *
 * A tuple update or delete is in conflict if we have a predicate lock
 * against the relation or page in which the tuple exists, or against the
 * tuple itself.
 */
void
CheckForSerializableConflictIn(Relation relation, ItemPointer tid, BlockNumber blkno)
{
	PREDICATELOCKTARGETTAG targettag;

	if (!SerializationNeededForWrite(relation))
		return;

	/* Check if someone else has already decided that we need to die */
	if (SxactIsDoomed(MySerializableXact))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during conflict in checking."),
				 errhint("The transaction might succeed if retried.")));

	/*
	 * We're doing a write which might cause rw-conflicts now or later.
	 * Memorize that fact.
	 */
	MyXactDidWrite = true;

	/*
	 * It is important that we check for locks from the finest granularity to
	 * the coarsest granularity, so that granularity promotion doesn't cause
	 * us to miss a lock.  The new (coarser) lock will be acquired before the
	 * old (finer) locks are released.
	 *
	 * It is not possible to take and hold a lock across the checks for all
	 * granularities because each target could be in a separate partition.
	 */
	if (tid != NULL)
	{
		SET_PREDICATELOCKTARGETTAG_TUPLE(targettag,
										 relation->rd_locator.dbOid,
										 relation->rd_id,
										 ItemPointerGetBlockNumber(tid),
										 ItemPointerGetOffsetNumber(tid));
		CheckTargetForConflictsIn(&targettag);
	}

	if (blkno != InvalidBlockNumber)
	{
		SET_PREDICATELOCKTARGETTAG_PAGE(targettag,
										relation->rd_locator.dbOid,
										relation->rd_id,
										blkno);
		CheckTargetForConflictsIn(&targettag);
	}

	SET_PREDICATELOCKTARGETTAG_RELATION(targettag,
										relation->rd_locator.dbOid,
										relation->rd_id);
	CheckTargetForConflictsIn(&targettag);
}

/*
 * CheckTableForSerializableConflictIn
 *		The entire table is going through a DDL-style logical mass delete
 *		like TRUNCATE or DROP TABLE.  If that causes a rw-conflict in from
 *		another serializable transaction, take appropriate action.
 *
 * While these operations do not operate entirely within the bounds of
 * snapshot isolation, they can occur inside a serializable transaction, and
 * will logically occur after any reads which saw rows which were destroyed
 * by these operations, so we do what we can to serialize properly under
 * SSI.
 *
 * The relation passed in must be a heap relation. Any predicate lock of any
 * granularity on the heap will cause a rw-conflict in to this transaction.
 * Predicate locks on indexes do not matter because they only exist to guard
 * against conflicting inserts into the index, and this is a mass *delete*.
 * When a table is truncated or dropped, the index will also be truncated
 * or dropped, and we'll deal with locks on the index when that happens.
 *
 * Dropping or truncating a table also needs to drop any existing predicate
 * locks on heap tuples or pages, because they're about to go away. This
 * should be done before altering the predicate locks because the transaction
 * could be rolled back because of a conflict, in which case the lock changes
 * are not needed. (At the moment, we don't actually bother to drop the
 * existing locks on a dropped or truncated table at the moment. That might
 * lead to some false positives, but it doesn't seem worth the trouble.)
 */
void
CheckTableForSerializableConflictIn(Relation relation)
{
	HASH_SEQ_STATUS seqstat;
	PREDICATELOCKTARGET *target;
	Oid			dbId;
	Oid			heapId;
	int			i;

	/*
	 * Bail out quickly if there are no serializable transactions running.
	 * It's safe to check this without taking locks because the caller is
	 * holding an ACCESS EXCLUSIVE lock on the relation.  No new locks which
	 * would matter here can be acquired while that is held.
	 */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
		return;

	if (!SerializationNeededForWrite(relation))
		return;

	/*
	 * We're doing a write which might cause rw-conflicts now or later.
	 * Memorize that fact.
	 */
	MyXactDidWrite = true;

	Assert(relation->rd_index == NULL); /* not an index relation */

	dbId = relation->rd_locator.dbOid;
	heapId = relation->rd_id;

	LWLockAcquire(SerializablePredicateListLock, LW_EXCLUSIVE);
	for (i = 0; i < NUM_PREDICATELOCK_PARTITIONS; i++)
		LWLockAcquire(PredicateLockHashPartitionLockByIndex(i), LW_SHARED);
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* Scan through target list */
	hash_seq_init(&seqstat, PredicateLockTargetHash);

	while ((target = (PREDICATELOCKTARGET *) hash_seq_search(&seqstat)))
	{
		dlist_mutable_iter iter;

		/*
		 * Check whether this is a target which needs attention.
		 */
		if (GET_PREDICATELOCKTARGETTAG_RELATION(target->tag) != heapId)
			continue;			/* wrong relation id */
		if (GET_PREDICATELOCKTARGETTAG_DB(target->tag) != dbId)
			continue;			/* wrong database id */

		/*
		 * Loop through locks for this target and flag conflicts.
		 */
		dlist_foreach_modify(iter, &target->predicateLocks)
		{
			PREDICATELOCK *predlock =
				dlist_container(PREDICATELOCK, targetLink, iter.cur);

			if (predlock->tag.myXact != MySerializableXact
				&& !RWConflictExists(predlock->tag.myXact, MySerializableXact))
			{
				FlagRWConflict(predlock->tag.myXact, MySerializableXact);
			}
		}
	}

	/* Release locks in reverse order */
	LWLockRelease(SerializableXactHashLock);
	for (i = NUM_PREDICATELOCK_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(PredicateLockHashPartitionLockByIndex(i));
	LWLockRelease(SerializablePredicateListLock);
}


/*
 * Flag a rw-dependency between two serializable transactions.
 *
 * The caller is responsible for ensuring that we have a LW lock on
 * the transaction hash table.
 */
static void
FlagRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer)
{
	Assert(reader != writer);

	/* First, see if this conflict causes failure. */
	OnConflict_CheckForSerializationFailure(reader, writer);

	/* Actually do the conflict flagging. */
	if (reader == OldCommittedSxact)
		writer->flags |= SXACT_FLAG_SUMMARY_CONFLICT_IN;
	else if (writer == OldCommittedSxact)
		reader->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;
	else
		SetRWConflict(reader, writer);
}

/*----------------------------------------------------------------------------
 * We are about to add a RW-edge to the dependency graph - check that we don't
 * introduce a dangerous structure by doing so, and abort one of the
 * transactions if so.
 *
 * A serialization failure can only occur if there is a dangerous structure
 * in the dependency graph:
 *
 *		Tin ------> Tpivot ------> Tout
 *			  rw			 rw
 *
 * Furthermore, Tout must commit first.
 *
 * One more optimization is that if Tin is declared READ ONLY (or commits
 * without writing), we can only have a problem if Tout committed before Tin
 * acquired its snapshot.
 *----------------------------------------------------------------------------
 */
static void
OnConflict_CheckForSerializationFailure(const SERIALIZABLEXACT *reader,
										SERIALIZABLEXACT *writer)
{
	bool		failure;

	Assert(LWLockHeldByMe(SerializableXactHashLock));

	failure = false;

	/*------------------------------------------------------------------------
	 * Check for already-committed writer with rw-conflict out flagged
	 * (conflict-flag on W means that T2 committed before W):
	 *
	 *		R ------> W ------> T2
	 *			rw		  rw
	 *
	 * That is a dangerous structure, so we must abort. (Since the writer
	 * has already committed, we must be the reader)
	 *------------------------------------------------------------------------
	 */
	if (SxactIsCommitted(writer)
		&& (SxactHasConflictOut(writer) || SxactHasSummaryConflictOut(writer)))
		failure = true;

	/*------------------------------------------------------------------------
	 * Check whether the writer has become a pivot with an out-conflict
	 * committed transaction (T2), and T2 committed first:
	 *
	 *		R ------> W ------> T2
	 *			rw		  rw
	 *
	 * Because T2 must've committed first, there is no anomaly if:
	 * - the reader committed before T2
	 * - the writer committed before T2
	 * - the reader is a READ ONLY transaction and the reader was concurrent
	 *	 with T2 (= reader acquired its snapshot before T2 committed)
	 *
	 * We also handle the case that T2 is prepared but not yet committed
	 * here. In that case T2 has already checked for conflicts, so if it
	 * commits first, making the above conflict real, it's too late for it
	 * to abort.
	 *------------------------------------------------------------------------
	 */
	if (!failure && SxactHasSummaryConflictOut(writer))
		failure = true;
	else if (!failure)
	{
		dlist_iter	iter;

		dlist_foreach(iter, &writer->outConflicts)
		{
			RWConflict	conflict =
				dlist_container(RWConflictData, outLink, iter.cur);
			SERIALIZABLEXACT *t2 = conflict->sxactIn;

			if (SxactIsPrepared(t2)
				&& (!SxactIsCommitted(reader)
					|| t2->prepareSeqNo <= reader->commitSeqNo)
				&& (!SxactIsCommitted(writer)
					|| t2->prepareSeqNo <= writer->commitSeqNo)
				&& (!SxactIsReadOnly(reader)
					|| t2->prepareSeqNo <= reader->SeqNo.lastCommitBeforeSnapshot))
			{
				failure = true;
				break;
			}
		}
	}

	/*------------------------------------------------------------------------
	 * Check whether the reader has become a pivot with a writer
	 * that's committed (or prepared):
	 *
	 *		T0 ------> R ------> W
	 *			 rw		   rw
	 *
	 * Because W must've committed first for an anomaly to occur, there is no
	 * anomaly if:
	 * - T0 committed before the writer
	 * - T0 is READ ONLY, and overlaps the writer
	 *------------------------------------------------------------------------
	 */
	if (!failure && SxactIsPrepared(writer) && !SxactIsReadOnly(reader))
	{
		if (SxactHasSummaryConflictIn(reader))
		{
			failure = true;
		}
		else
		{
			dlist_iter	iter;

			/*
			 * The unconstify is needed as we have no const version of
			 * dlist_foreach().
			 */
			dlist_foreach(iter, &unconstify(SERIALIZABLEXACT *, reader)->inConflicts)
			{
				const RWConflict conflict =
					dlist_container(RWConflictData, inLink, iter.cur);
				const SERIALIZABLEXACT *t0 = conflict->sxactOut;

				if (!SxactIsDoomed(t0)
					&& (!SxactIsCommitted(t0)
						|| t0->commitSeqNo >= writer->prepareSeqNo)
					&& (!SxactIsReadOnly(t0)
						|| t0->SeqNo.lastCommitBeforeSnapshot >= writer->prepareSeqNo))
				{
					failure = true;
					break;
				}
			}
		}
	}

	if (failure)
	{
		/*
		 * We have to kill a transaction to avoid a possible anomaly from
		 * occurring. If the writer is us, we can just ereport() to cause a
		 * transaction abort. Otherwise we flag the writer for termination,
		 * causing it to abort when it tries to commit. However, if the writer
		 * is a prepared transaction, already prepared, we can't abort it
		 * anymore, so we have to kill the reader instead.
		 */
		if (MySerializableXact == writer)
		{
			LWLockRelease(SerializableXactHashLock);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to read/write dependencies among transactions"),
					 errdetail_internal("Reason code: Canceled on identification as a pivot, during write."),
					 errhint("The transaction might succeed if retried.")));
		}
		else if (SxactIsPrepared(writer))
		{
			LWLockRelease(SerializableXactHashLock);

			/* if we're not the writer, we have to be the reader */
			Assert(MySerializableXact == reader);
			ereport(ERROR,
					(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
					 errmsg("could not serialize access due to read/write dependencies among transactions"),
					 errdetail_internal("Reason code: Canceled on conflict out to pivot %u, during read.", writer->topXid),
					 errhint("The transaction might succeed if retried.")));
		}
		writer->flags |= SXACT_FLAG_DOOMED;
	}
}

/*
 * PreCommit_CheckForSerializationFailure
 *		Check for dangerous structures in a serializable transaction
 *		at commit.
 *
 * We're checking for a dangerous structure as each conflict is recorded.
 * The only way we could have a problem at commit is if this is the "out"
 * side of a pivot, and neither the "in" side nor the pivot has yet
 * committed.
 *
 * If a dangerous structure is found, the pivot (the near conflict) is
 * marked for death, because rolling back another transaction might mean
 * that we fail without ever making progress.  This transaction is
 * committing writes, so letting it commit ensures progress.  If we
 * canceled the far conflict, it might immediately fail again on retry.
 */
void
PreCommit_CheckForSerializationFailure(void)
{
	dlist_iter	near_iter;

	if (MySerializableXact == InvalidSerializableXact)
		return;

	Assert(IsolationIsSerializable());

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/*
	 * Check if someone else has already decided that we need to die.  Since
	 * we set our own DOOMED flag when partially releasing, ignore in that
	 * case.
	 */
	if (SxactIsDoomed(MySerializableXact) &&
		!SxactIsPartiallyReleased(MySerializableXact))
	{
		LWLockRelease(SerializableXactHashLock);
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during commit attempt."),
				 errhint("The transaction might succeed if retried.")));
	}

	dlist_foreach(near_iter, &MySerializableXact->inConflicts)
	{
		RWConflict	nearConflict =
			dlist_container(RWConflictData, inLink, near_iter.cur);

		if (!SxactIsCommitted(nearConflict->sxactOut)
			&& !SxactIsDoomed(nearConflict->sxactOut))
		{
			dlist_iter	far_iter;

			dlist_foreach(far_iter, &nearConflict->sxactOut->inConflicts)
			{
				RWConflict	farConflict =
					dlist_container(RWConflictData, inLink, far_iter.cur);

				if (farConflict->sxactOut == MySerializableXact
					|| (!SxactIsCommitted(farConflict->sxactOut)
						&& !SxactIsReadOnly(farConflict->sxactOut)
						&& !SxactIsDoomed(farConflict->sxactOut)))
				{
					/*
					 * Normally, we kill the pivot transaction to make sure we
					 * make progress if the failing transaction is retried.
					 * However, we can't kill it if it's already prepared, so
					 * in that case we commit suicide instead.
					 */
					if (SxactIsPrepared(nearConflict->sxactOut))
					{
						LWLockRelease(SerializableXactHashLock);
						ereport(ERROR,
								(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
								 errmsg("could not serialize access due to read/write dependencies among transactions"),
								 errdetail_internal("Reason code: Canceled on commit attempt with conflict in from prepared pivot."),
								 errhint("The transaction might succeed if retried.")));
					}
					nearConflict->sxactOut->flags |= SXACT_FLAG_DOOMED;
					break;
				}
			}
		}
	}

	MySerializableXact->prepareSeqNo = ++(PredXact->LastSxactCommitSeqNo);
	MySerializableXact->flags |= SXACT_FLAG_PREPARED;

	LWLockRelease(SerializableXactHashLock);
}

/*------------------------------------------------------------------------*/

/*
 * Two-phase commit support
 */

/*
 * AtPrepare_Locks
 *		Do the preparatory work for a PREPARE: make 2PC state file
 *		records for all predicate locks currently held.
 */
void
AtPrepare_PredicateLocks(void)
{
	SERIALIZABLEXACT *sxact;
	TwoPhasePredicateRecord record;
	TwoPhasePredicateXactRecord *xactRecord;
	TwoPhasePredicateLockRecord *lockRecord;
	dlist_iter	iter;

	sxact = MySerializableXact;
	xactRecord = &(record.data.xactRecord);
	lockRecord = &(record.data.lockRecord);

	if (MySerializableXact == InvalidSerializableXact)
		return;

	/* Generate an xact record for our SERIALIZABLEXACT */
	record.type = TWOPHASEPREDICATERECORD_XACT;
	xactRecord->xmin = MySerializableXact->xmin;
	xactRecord->flags = MySerializableXact->flags;

	/*
	 * Note that we don't include the list of conflicts in our out in the
	 * statefile, because new conflicts can be added even after the
	 * transaction prepares. We'll just make a conservative assumption during
	 * recovery instead.
	 */

	RegisterTwoPhaseRecord(TWOPHASE_RM_PREDICATELOCK_ID, 0,
						   &record, sizeof(record));

	/*
	 * Generate a lock record for each lock.
	 *
	 * To do this, we need to walk the predicate lock list in our sxact rather
	 * than using the local predicate lock table because the latter is not
	 * guaranteed to be accurate.
	 */
	LWLockAcquire(SerializablePredicateListLock, LW_SHARED);

	/*
	 * No need to take sxact->perXactPredicateListLock in parallel mode
	 * because there cannot be any parallel workers running while we are
	 * preparing a transaction.
	 */
	Assert(!IsParallelWorker() && !ParallelContextActive());

	dlist_foreach(iter, &sxact->predicateLocks)
	{
		PREDICATELOCK *predlock =
			dlist_container(PREDICATELOCK, xactLink, iter.cur);

		record.type = TWOPHASEPREDICATERECORD_LOCK;
		lockRecord->target = predlock->tag.myTarget->tag;

		RegisterTwoPhaseRecord(TWOPHASE_RM_PREDICATELOCK_ID, 0,
							   &record, sizeof(record));
	}

	LWLockRelease(SerializablePredicateListLock);
}

/*
 * PostPrepare_Locks
 *		Clean up after successful PREPARE. Unlike the non-predicate
 *		lock manager, we do not need to transfer locks to a dummy
 *		PGPROC because our SERIALIZABLEXACT will stay around
 *		anyway. We only need to clean up our local state.
 */
void
PostPrepare_PredicateLocks(TransactionId xid)
{
	if (MySerializableXact == InvalidSerializableXact)
		return;

	Assert(SxactIsPrepared(MySerializableXact));

	MySerializableXact->pid = 0;
	MySerializableXact->pgprocno = INVALID_PROC_NUMBER;

	hash_destroy(LocalPredicateLockHash);
	LocalPredicateLockHash = NULL;

	MySerializableXact = InvalidSerializableXact;
	MyXactDidWrite = false;
}

/*
 * PredicateLockTwoPhaseFinish
 *		Release a prepared transaction's predicate locks once it
 *		commits or aborts.
 */
void
PredicateLockTwoPhaseFinish(TransactionId xid, bool isCommit)
{
	SERIALIZABLEXID *sxid;
	SERIALIZABLEXIDTAG sxidtag;

	sxidtag.xid = xid;

	LWLockAcquire(SerializableXactHashLock, LW_SHARED);
	sxid = (SERIALIZABLEXID *)
		hash_search(SerializableXidHash, &sxidtag, HASH_FIND, NULL);
	LWLockRelease(SerializableXactHashLock);

	/* xid will not be found if it wasn't a serializable transaction */
	if (sxid == NULL)
		return;

	/* Release its locks */
	MySerializableXact = sxid->myXact;
	MyXactDidWrite = true;		/* conservatively assume that we wrote
								 * something */
	ReleasePredicateLocks(isCommit, false);
}

/*
 * Re-acquire a predicate lock belonging to a transaction that was prepared.
 */
void
predicatelock_twophase_recover(TransactionId xid, uint16 info,
							   void *recdata, uint32 len)
{
	TwoPhasePredicateRecord *record;

	Assert(len == sizeof(TwoPhasePredicateRecord));

	record = (TwoPhasePredicateRecord *) recdata;

	Assert((record->type == TWOPHASEPREDICATERECORD_XACT) ||
		   (record->type == TWOPHASEPREDICATERECORD_LOCK));

	if (record->type == TWOPHASEPREDICATERECORD_XACT)
	{
		/* Per-transaction record. Set up a SERIALIZABLEXACT. */
		TwoPhasePredicateXactRecord *xactRecord;
		SERIALIZABLEXACT *sxact;
		SERIALIZABLEXID *sxid;
		SERIALIZABLEXIDTAG sxidtag;
		bool		found;

		xactRecord = (TwoPhasePredicateXactRecord *) &record->data.xactRecord;

		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
		sxact = CreatePredXact();
		if (!sxact)
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("out of shared memory")));

		/* vxid for a prepared xact is INVALID_PROC_NUMBER/xid; no pid */
		sxact->vxid.procNumber = INVALID_PROC_NUMBER;
		sxact->vxid.localTransactionId = (LocalTransactionId) xid;
		sxact->pid = 0;
		sxact->pgprocno = INVALID_PROC_NUMBER;

		/* a prepared xact hasn't committed yet */
		sxact->prepareSeqNo = RecoverySerCommitSeqNo;
		sxact->commitSeqNo = InvalidSerCommitSeqNo;
		sxact->finishedBefore = InvalidTransactionId;

		sxact->SeqNo.lastCommitBeforeSnapshot = RecoverySerCommitSeqNo;

		/*
		 * Don't need to track this; no transactions running at the time the
		 * recovered xact started are still active, except possibly other
		 * prepared xacts and we don't care whether those are RO_SAFE or not.
		 */
		dlist_init(&(sxact->possibleUnsafeConflicts));

		dlist_init(&(sxact->predicateLocks));
		dlist_node_init(&sxact->finishedLink);

		sxact->topXid = xid;
		sxact->xmin = xactRecord->xmin;
		sxact->flags = xactRecord->flags;
		Assert(SxactIsPrepared(sxact));
		if (!SxactIsReadOnly(sxact))
		{
			++(PredXact->WritableSxactCount);
			Assert(PredXact->WritableSxactCount <=
				   (MaxBackends + max_prepared_xacts));
		}

		/*
		 * We don't know whether the transaction had any conflicts or not, so
		 * we'll conservatively assume that it had both a conflict in and a
		 * conflict out, and represent that with the summary conflict flags.
		 */
		dlist_init(&(sxact->outConflicts));
		dlist_init(&(sxact->inConflicts));
		sxact->flags |= SXACT_FLAG_SUMMARY_CONFLICT_IN;
		sxact->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;

		/* Register the transaction's xid */
		sxidtag.xid = xid;
		sxid = (SERIALIZABLEXID *) hash_search(SerializableXidHash,
											   &sxidtag,
											   HASH_ENTER, &found);
		Assert(sxid != NULL);
		Assert(!found);
		sxid->myXact = (SERIALIZABLEXACT *) sxact;

		/*
		 * Update global xmin. Note that this is a special case compared to
		 * registering a normal transaction, because the global xmin might go
		 * backwards. That's OK, because until recovery is over we're not
		 * going to complete any transactions or create any non-prepared
		 * transactions, so there's no danger of throwing away.
		 */
		if ((!TransactionIdIsValid(PredXact->SxactGlobalXmin)) ||
			(TransactionIdFollows(PredXact->SxactGlobalXmin, sxact->xmin)))
		{
			PredXact->SxactGlobalXmin = sxact->xmin;
			PredXact->SxactGlobalXminCount = 1;
			SerialSetActiveSerXmin(sxact->xmin);
		}
		else if (TransactionIdEquals(sxact->xmin, PredXact->SxactGlobalXmin))
		{
			Assert(PredXact->SxactGlobalXminCount > 0);
			PredXact->SxactGlobalXminCount++;
		}

		LWLockRelease(SerializableXactHashLock);
	}
	else if (record->type == TWOPHASEPREDICATERECORD_LOCK)
	{
		/* Lock record. Recreate the PREDICATELOCK */
		TwoPhasePredicateLockRecord *lockRecord;
		SERIALIZABLEXID *sxid;
		SERIALIZABLEXACT *sxact;
		SERIALIZABLEXIDTAG sxidtag;
		uint32		targettaghash;

		lockRecord = (TwoPhasePredicateLockRecord *) &record->data.lockRecord;
		targettaghash = PredicateLockTargetTagHashCode(&lockRecord->target);

		LWLockAcquire(SerializableXactHashLock, LW_SHARED);
		sxidtag.xid = xid;
		sxid = (SERIALIZABLEXID *)
			hash_search(SerializableXidHash, &sxidtag, HASH_FIND, NULL);
		LWLockRelease(SerializableXactHashLock);

		Assert(sxid != NULL);
		sxact = sxid->myXact;
		Assert(sxact != InvalidSerializableXact);

		CreatePredicateLock(&lockRecord->target, targettaghash, sxact);
	}
}

/*
 * Prepare to share the current SERIALIZABLEXACT with parallel workers.
 * Return a handle object that can be used by AttachSerializableXact() in a
 * parallel worker.
 */
SerializableXactHandle
ShareSerializableXact(void)
{
	return MySerializableXact;
}

/*
 * Allow parallel workers to import the leader's SERIALIZABLEXACT.
 */
void
AttachSerializableXact(SerializableXactHandle handle)
{

	Assert(MySerializableXact == InvalidSerializableXact);

	MySerializableXact = (SERIALIZABLEXACT *) handle;
	if (MySerializableXact != InvalidSerializableXact)
		CreateLocalPredicateLockHash();
}
