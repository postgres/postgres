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
 *	SerializablePredicateLockListLock
 *		- Protects the linked list of locks held by a transaction.  Note
 *			that the locks themselves are also covered by the partition
 *			locks of their respective lock targets; this lock only affects
 *			the linked list connecting the locks related to a transaction.
 *		- All transactions share this single lock (with no partitioning).
 *		- There is never a need for a process other than the one running
 *			an active transaction to walk the list of locks held by that
 *			transaction.
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
 *	FirstPredicateLockMgrLock based partition locks
 *		- The same lock protects a target, all locks on that target, and
 *			the linked list of locks on the target..
 *		- When more than one is needed, acquire in ascending order.
 *
 *	SerializableXactHashLock
 *		- Protects both PredXact and SerializableXidHash.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
 *		InitPredicateLocks(void)
 *		PredicateLockShmemSize(void)
 *
 * predicate lock reporting
 *		GetPredicateLockStatusData(void)
 *		PageIsPredicateLocked(Relation relation, BlockNumber blkno)
 *
 * predicate lock maintenance
 *		GetSerializableTransactionSnapshot(Snapshot snapshot)
 *		SetSerializableTransactionSnapshot(Snapshot snapshot,
 *										   TransactionId sourcexid)
 *		RegisterPredicateLockingXid(void)
 *		PredicateLockRelation(Relation relation, Snapshot snapshot)
 *		PredicateLockPage(Relation relation, BlockNumber blkno,
 *						Snapshot snapshot)
 *		PredicateLockTuple(Relation relation, HeapTuple tuple,
 *						Snapshot snapshot)
 *		PredicateLockPageSplit(Relation relation, BlockNumber oldblkno,
 *							   BlockNumber newblkno)
 *		PredicateLockPageCombine(Relation relation, BlockNumber oldblkno,
 *								 BlockNumber newblkno)
 *		TransferPredicateLocksToHeapRelation(Relation relation)
 *		ReleasePredicateLocks(bool isCommit)
 *
 * conflict detection (may also trigger rollback)
 *		CheckForSerializableConflictOut(bool visible, Relation relation,
 *										HeapTupleData *tup, Buffer buffer,
 *										Snapshot snapshot)
 *		CheckForSerializableConflictIn(Relation relation, HeapTupleData *tup,
 *									   Buffer buffer)
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

#include "access/htup_details.h"
#include "access/slru.h"
#include "access/subtrans.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/twophase_rmgr.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "storage/predicate_internals.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"

/* Uncomment the next line to test the graceful degradation code. */
/* #define TEST_OLDSERXID */

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

#define SxactIsOnFinishedList(sxact) (!SHMQueueIsDetached(&((sxact)->finishedLink)))

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
static SlruCtlData OldSerXidSlruCtlData;

#define OldSerXidSlruCtl			(&OldSerXidSlruCtlData)

#define OLDSERXID_PAGESIZE			BLCKSZ
#define OLDSERXID_ENTRYSIZE			sizeof(SerCommitSeqNo)
#define OLDSERXID_ENTRIESPERPAGE	(OLDSERXID_PAGESIZE / OLDSERXID_ENTRYSIZE)

/*
 * Set maximum pages based on the lesser of the number needed to track all
 * transactions and the maximum that SLRU supports.
 */
#define OLDSERXID_MAX_PAGE			Min(SLRU_PAGES_PER_SEGMENT * 0x10000 - 1, \
										(MaxTransactionId) / OLDSERXID_ENTRIESPERPAGE)

#define OldSerXidNextPage(page) (((page) >= OLDSERXID_MAX_PAGE) ? 0 : (page) + 1)

#define OldSerXidValue(slotno, xid) (*((SerCommitSeqNo *) \
	(OldSerXidSlruCtl->shared->page_buffer[slotno] + \
	((((uint32) (xid)) % OLDSERXID_ENTRIESPERPAGE) * OLDSERXID_ENTRYSIZE))))

#define OldSerXidPage(xid)	((((uint32) (xid)) / OLDSERXID_ENTRIESPERPAGE) % (OLDSERXID_MAX_PAGE + 1))
#define OldSerXidSegment(page)	((page) / SLRU_PAGES_PER_SEGMENT)

typedef struct OldSerXidControlData
{
	int			headPage;		/* newest initialized page */
	TransactionId headXid;		/* newest valid Xid in the SLRU */
	TransactionId tailXid;		/* oldest xmin we might be interested in */
	bool		warningIssued;	/* have we issued SLRU wrap-around warning? */
}	OldSerXidControlData;

typedef struct OldSerXidControlData *OldSerXidControl;

static OldSerXidControl oldSerXidControl;

/*
 * When the oldest committed transaction on the "finished" list is moved to
 * SLRU, its predicate locks will be moved to this "dummy" transaction,
 * collapsing duplicate targets.  When a duplicate is found, the later
 * commitSeqNo is used.
 */
static SERIALIZABLEXACT *OldCommittedSxact;


/* This configuration variable is used to set the predicate lock table size */
int			max_predicate_locks_per_xact;		/* set by guc.c */

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
static SHM_QUEUE *FinishedSerializableTransactions;

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

/* local functions */

static SERIALIZABLEXACT *CreatePredXact(void);
static void ReleasePredXact(SERIALIZABLEXACT *sxact);
static SERIALIZABLEXACT *FirstPredXact(void);
static SERIALIZABLEXACT *NextPredXact(SERIALIZABLEXACT *sxact);

static bool RWConflictExists(const SERIALIZABLEXACT *reader, const SERIALIZABLEXACT *writer);
static void SetRWConflict(SERIALIZABLEXACT *reader, SERIALIZABLEXACT *writer);
static void SetPossibleUnsafeConflict(SERIALIZABLEXACT *roXact, SERIALIZABLEXACT *activeXact);
static void ReleaseRWConflict(RWConflict conflict);
static void FlagSxactUnsafe(SERIALIZABLEXACT *sxact);

static bool OldSerXidPagePrecedesLogically(int p, int q);
static void OldSerXidInit(void);
static void OldSerXidAdd(TransactionId xid, SerCommitSeqNo minConflictCommitSeqNo);
static SerCommitSeqNo OldSerXidGetMinConflictCommitSeqNo(TransactionId xid);
static void OldSerXidSetActiveSerXmin(TransactionId xid);

static uint32 predicatelock_hash(const void *key, Size keysize);
static void SummarizeOldestCommittedSxact(void);
static Snapshot GetSafeSnapshot(Snapshot snapshot);
static Snapshot GetSerializableTransactionSnapshotInt(Snapshot snapshot,
									  TransactionId sourcexid);
static bool PredicateLockExists(const PREDICATELOCKTARGETTAG *targettag);
static bool GetParentPredicateLockTag(const PREDICATELOCKTARGETTAG *tag,
						  PREDICATELOCKTARGETTAG *parent);
static bool CoarserLockCovers(const PREDICATELOCKTARGETTAG *newtargettag);
static void RemoveScratchTarget(bool lockheld);
static void RestoreScratchTarget(bool lockheld);
static void RemoveTargetIfNoLongerUsed(PREDICATELOCKTARGET *target,
						   uint32 targettaghash);
static void DeleteChildTargetLocks(const PREDICATELOCKTARGETTAG *newtargettag);
static int	PredicateLockPromotionThreshold(const PREDICATELOCKTARGETTAG *tag);
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


/*------------------------------------------------------------------------*/

/*
 * Does this relation participate in predicate locking? Temporary and system
 * relations are exempt, as are materialized views.
 */
static inline bool
PredicateLockingNeededForRelation(Relation relation)
{
	return !(relation->rd_id < FirstBootstrapObjectId ||
			 RelationUsesLocalBuffers(relation) ||
			 relation->rd_rel->relkind == RELKIND_MATVIEW);
}

/*
 * When a public interface method is called for a read, this is the test to
 * see if we should do a quick return.
 *
 * Note: this function has side-effects! If this transaction has been flagged
 * as RO-safe since the last call, we release all predicate locks and reset
 * MySerializableXact. That makes subsequent calls to return quickly.
 *
 * This is marked as 'inline' to make to eliminate the function call overhead
 * in the common case that serialization is not needed.
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
		ReleasePredicateLocks(false);
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
	PredXactListElement ptle;

	ptle = (PredXactListElement)
		SHMQueueNext(&PredXact->availableList,
					 &PredXact->availableList,
					 offsetof(PredXactListElementData, link));
	if (!ptle)
		return NULL;

	SHMQueueDelete(&ptle->link);
	SHMQueueInsertBefore(&PredXact->activeList, &ptle->link);
	return &ptle->sxact;
}

static void
ReleasePredXact(SERIALIZABLEXACT *sxact)
{
	PredXactListElement ptle;

	Assert(ShmemAddrIsValid(sxact));

	ptle = (PredXactListElement)
		(((char *) sxact)
		 - offsetof(PredXactListElementData, sxact)
		 + offsetof(PredXactListElementData, link));
	SHMQueueDelete(&ptle->link);
	SHMQueueInsertBefore(&PredXact->availableList, &ptle->link);
}

static SERIALIZABLEXACT *
FirstPredXact(void)
{
	PredXactListElement ptle;

	ptle = (PredXactListElement)
		SHMQueueNext(&PredXact->activeList,
					 &PredXact->activeList,
					 offsetof(PredXactListElementData, link));
	if (!ptle)
		return NULL;

	return &ptle->sxact;
}

static SERIALIZABLEXACT *
NextPredXact(SERIALIZABLEXACT *sxact)
{
	PredXactListElement ptle;

	Assert(ShmemAddrIsValid(sxact));

	ptle = (PredXactListElement)
		(((char *) sxact)
		 - offsetof(PredXactListElementData, sxact)
		 + offsetof(PredXactListElementData, link));
	ptle = (PredXactListElement)
		SHMQueueNext(&PredXact->activeList,
					 &ptle->link,
					 offsetof(PredXactListElementData, link));
	if (!ptle)
		return NULL;

	return &ptle->sxact;
}

/*------------------------------------------------------------------------*/

/*
 * These functions manage primitive access to the RWConflict pool and lists.
 */
static bool
RWConflictExists(const SERIALIZABLEXACT *reader, const SERIALIZABLEXACT *writer)
{
	RWConflict	conflict;

	Assert(reader != writer);

	/* Check the ends of the purported conflict first. */
	if (SxactIsDoomed(reader)
		|| SxactIsDoomed(writer)
		|| SHMQueueEmpty(&reader->outConflicts)
		|| SHMQueueEmpty(&writer->inConflicts))
		return false;

	/* A conflict is possible; walk the list to find out. */
	conflict = (RWConflict)
		SHMQueueNext(&reader->outConflicts,
					 &reader->outConflicts,
					 offsetof(RWConflictData, outLink));
	while (conflict)
	{
		if (conflict->sxactIn == writer)
			return true;
		conflict = (RWConflict)
			SHMQueueNext(&reader->outConflicts,
						 &conflict->outLink,
						 offsetof(RWConflictData, outLink));
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

	conflict = (RWConflict)
		SHMQueueNext(&RWConflictPool->availableList,
					 &RWConflictPool->availableList,
					 offsetof(RWConflictData, outLink));
	if (!conflict)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough elements in RWConflictPool to record a read/write conflict"),
				 errhint("You might need to run fewer transactions at a time or increase max_connections.")));

	SHMQueueDelete(&conflict->outLink);

	conflict->sxactOut = reader;
	conflict->sxactIn = writer;
	SHMQueueInsertBefore(&reader->outConflicts, &conflict->outLink);
	SHMQueueInsertBefore(&writer->inConflicts, &conflict->inLink);
}

static void
SetPossibleUnsafeConflict(SERIALIZABLEXACT *roXact,
						  SERIALIZABLEXACT *activeXact)
{
	RWConflict	conflict;

	Assert(roXact != activeXact);
	Assert(SxactIsReadOnly(roXact));
	Assert(!SxactIsReadOnly(activeXact));

	conflict = (RWConflict)
		SHMQueueNext(&RWConflictPool->availableList,
					 &RWConflictPool->availableList,
					 offsetof(RWConflictData, outLink));
	if (!conflict)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("not enough elements in RWConflictPool to record a potential read/write conflict"),
				 errhint("You might need to run fewer transactions at a time or increase max_connections.")));

	SHMQueueDelete(&conflict->outLink);

	conflict->sxactOut = activeXact;
	conflict->sxactIn = roXact;
	SHMQueueInsertBefore(&activeXact->possibleUnsafeConflicts,
						 &conflict->outLink);
	SHMQueueInsertBefore(&roXact->possibleUnsafeConflicts,
						 &conflict->inLink);
}

static void
ReleaseRWConflict(RWConflict conflict)
{
	SHMQueueDelete(&conflict->inLink);
	SHMQueueDelete(&conflict->outLink);
	SHMQueueInsertBefore(&RWConflictPool->availableList, &conflict->outLink);
}

static void
FlagSxactUnsafe(SERIALIZABLEXACT *sxact)
{
	RWConflict	conflict,
				nextConflict;

	Assert(SxactIsReadOnly(sxact));
	Assert(!SxactIsROSafe(sxact));

	sxact->flags |= SXACT_FLAG_RO_UNSAFE;

	/*
	 * We know this isn't a safe snapshot, so we can stop looking for other
	 * potential conflicts.
	 */
	conflict = (RWConflict)
		SHMQueueNext(&sxact->possibleUnsafeConflicts,
					 &sxact->possibleUnsafeConflicts,
					 offsetof(RWConflictData, inLink));
	while (conflict)
	{
		nextConflict = (RWConflict)
			SHMQueueNext(&sxact->possibleUnsafeConflicts,
						 &conflict->inLink,
						 offsetof(RWConflictData, inLink));

		Assert(!SxactIsReadOnly(conflict->sxactOut));
		Assert(sxact == conflict->sxactIn);

		ReleaseRWConflict(conflict);

		conflict = nextConflict;
	}
}

/*------------------------------------------------------------------------*/

/*
 * We will work on the page range of 0..OLDSERXID_MAX_PAGE.
 * Compares using wraparound logic, as is required by slru.c.
 */
static bool
OldSerXidPagePrecedesLogically(int p, int q)
{
	int			diff;

	/*
	 * We have to compare modulo (OLDSERXID_MAX_PAGE+1)/2.  Both inputs should
	 * be in the range 0..OLDSERXID_MAX_PAGE.
	 */
	Assert(p >= 0 && p <= OLDSERXID_MAX_PAGE);
	Assert(q >= 0 && q <= OLDSERXID_MAX_PAGE);

	diff = p - q;
	if (diff >= ((OLDSERXID_MAX_PAGE + 1) / 2))
		diff -= OLDSERXID_MAX_PAGE + 1;
	else if (diff < -((int) (OLDSERXID_MAX_PAGE + 1) / 2))
		diff += OLDSERXID_MAX_PAGE + 1;
	return diff < 0;
}

/*
 * Initialize for the tracking of old serializable committed xids.
 */
static void
OldSerXidInit(void)
{
	bool		found;

	/*
	 * Set up SLRU management of the pg_serial data.
	 */
	OldSerXidSlruCtl->PagePrecedes = OldSerXidPagePrecedesLogically;
	SimpleLruInit(OldSerXidSlruCtl, "oldserxid",
				  NUM_OLDSERXID_BUFFERS, 0, OldSerXidLock, "pg_serial",
				  LWTRANCHE_OLDSERXID_BUFFERS);
	/* Override default assumption that writes should be fsync'd */
	OldSerXidSlruCtl->do_fsync = false;

	/*
	 * Create or attach to the OldSerXidControl structure.
	 */
	oldSerXidControl = (OldSerXidControl)
		ShmemInitStruct("OldSerXidControlData", sizeof(OldSerXidControlData), &found);

	if (!found)
	{
		/*
		 * Set control information to reflect empty SLRU.
		 */
		oldSerXidControl->headPage = -1;
		oldSerXidControl->headXid = InvalidTransactionId;
		oldSerXidControl->tailXid = InvalidTransactionId;
		oldSerXidControl->warningIssued = false;
	}
}

/*
 * Record a committed read write serializable xid and the minimum
 * commitSeqNo of any transactions to which this xid had a rw-conflict out.
 * An invalid seqNo means that there were no conflicts out from xid.
 */
static void
OldSerXidAdd(TransactionId xid, SerCommitSeqNo minConflictCommitSeqNo)
{
	TransactionId tailXid;
	int			targetPage;
	int			slotno;
	int			firstZeroPage;
	bool		isNewPage;

	Assert(TransactionIdIsValid(xid));

	targetPage = OldSerXidPage(xid);

	LWLockAcquire(OldSerXidLock, LW_EXCLUSIVE);

	/*
	 * If no serializable transactions are active, there shouldn't be anything
	 * to push out to the SLRU.  Hitting this assert would mean there's
	 * something wrong with the earlier cleanup logic.
	 */
	tailXid = oldSerXidControl->tailXid;
	Assert(TransactionIdIsValid(tailXid));

	/*
	 * If the SLRU is currently unused, zero out the whole active region from
	 * tailXid to headXid before taking it into use. Otherwise zero out only
	 * any new pages that enter the tailXid-headXid range as we advance
	 * headXid.
	 */
	if (oldSerXidControl->headPage < 0)
	{
		firstZeroPage = OldSerXidPage(tailXid);
		isNewPage = true;
	}
	else
	{
		firstZeroPage = OldSerXidNextPage(oldSerXidControl->headPage);
		isNewPage = OldSerXidPagePrecedesLogically(oldSerXidControl->headPage,
												   targetPage);
	}

	if (!TransactionIdIsValid(oldSerXidControl->headXid)
		|| TransactionIdFollows(xid, oldSerXidControl->headXid))
		oldSerXidControl->headXid = xid;
	if (isNewPage)
		oldSerXidControl->headPage = targetPage;

	/*
	 * Give a warning if we're about to run out of SLRU pages.
	 *
	 * slru.c has a maximum of 64k segments, with 32 (SLRU_PAGES_PER_SEGMENT)
	 * pages each. We need to store a 64-bit integer for each Xid, and with
	 * default 8k block size, 65536*32 pages is only enough to cover 2^30
	 * XIDs. If we're about to hit that limit and wrap around, warn the user.
	 *
	 * To avoid spamming the user, we only give one warning when we've used 1
	 * billion XIDs, and stay silent until the situation is fixed and the
	 * number of XIDs used falls below 800 million again.
	 *
	 * XXX: We have no safeguard to actually *prevent* the wrap-around,
	 * though. All you get is a warning.
	 */
	if (oldSerXidControl->warningIssued)
	{
		TransactionId lowWatermark;

		lowWatermark = tailXid + 800000000;
		if (lowWatermark < FirstNormalTransactionId)
			lowWatermark = FirstNormalTransactionId;
		if (TransactionIdPrecedes(xid, lowWatermark))
			oldSerXidControl->warningIssued = false;
	}
	else
	{
		TransactionId highWatermark;

		highWatermark = tailXid + 1000000000;
		if (highWatermark < FirstNormalTransactionId)
			highWatermark = FirstNormalTransactionId;
		if (TransactionIdFollows(xid, highWatermark))
		{
			oldSerXidControl->warningIssued = true;
			ereport(WARNING,
					(errmsg("memory for serializable conflict tracking is nearly exhausted"),
					 errhint("There might be an idle transaction or a forgotten prepared transaction causing this.")));
		}
	}

	if (isNewPage)
	{
		/* Initialize intervening pages. */
		while (firstZeroPage != targetPage)
		{
			(void) SimpleLruZeroPage(OldSerXidSlruCtl, firstZeroPage);
			firstZeroPage = OldSerXidNextPage(firstZeroPage);
		}
		slotno = SimpleLruZeroPage(OldSerXidSlruCtl, targetPage);
	}
	else
		slotno = SimpleLruReadPage(OldSerXidSlruCtl, targetPage, true, xid);

	OldSerXidValue(slotno, xid) = minConflictCommitSeqNo;
	OldSerXidSlruCtl->shared->page_dirty[slotno] = true;

	LWLockRelease(OldSerXidLock);
}

/*
 * Get the minimum commitSeqNo for any conflict out for the given xid.  For
 * a transaction which exists but has no conflict out, InvalidSerCommitSeqNo
 * will be returned.
 */
static SerCommitSeqNo
OldSerXidGetMinConflictCommitSeqNo(TransactionId xid)
{
	TransactionId headXid;
	TransactionId tailXid;
	SerCommitSeqNo val;
	int			slotno;

	Assert(TransactionIdIsValid(xid));

	LWLockAcquire(OldSerXidLock, LW_SHARED);
	headXid = oldSerXidControl->headXid;
	tailXid = oldSerXidControl->tailXid;
	LWLockRelease(OldSerXidLock);

	if (!TransactionIdIsValid(headXid))
		return 0;

	Assert(TransactionIdIsValid(tailXid));

	if (TransactionIdPrecedes(xid, tailXid)
		|| TransactionIdFollows(xid, headXid))
		return 0;

	/*
	 * The following function must be called without holding OldSerXidLock,
	 * but will return with that lock held, which must then be released.
	 */
	slotno = SimpleLruReadPage_ReadOnly(OldSerXidSlruCtl,
										OldSerXidPage(xid), xid);
	val = OldSerXidValue(slotno, xid);
	LWLockRelease(OldSerXidLock);
	return val;
}

/*
 * Call this whenever there is a new xmin for active serializable
 * transactions.  We don't need to keep information on transactions which
 * precede that.  InvalidTransactionId means none active, so everything in
 * the SLRU can be discarded.
 */
static void
OldSerXidSetActiveSerXmin(TransactionId xid)
{
	LWLockAcquire(OldSerXidLock, LW_EXCLUSIVE);

	/*
	 * When no sxacts are active, nothing overlaps, set the xid values to
	 * invalid to show that there are no valid entries.  Don't clear headPage,
	 * though.  A new xmin might still land on that page, and we don't want to
	 * repeatedly zero out the same page.
	 */
	if (!TransactionIdIsValid(xid))
	{
		oldSerXidControl->tailXid = InvalidTransactionId;
		oldSerXidControl->headXid = InvalidTransactionId;
		LWLockRelease(OldSerXidLock);
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
		Assert(oldSerXidControl->headPage < 0);
		if (!TransactionIdIsValid(oldSerXidControl->tailXid)
			|| TransactionIdPrecedes(xid, oldSerXidControl->tailXid))
		{
			oldSerXidControl->tailXid = xid;
		}
		LWLockRelease(OldSerXidLock);
		return;
	}

	Assert(!TransactionIdIsValid(oldSerXidControl->tailXid)
		   || TransactionIdFollows(xid, oldSerXidControl->tailXid));

	oldSerXidControl->tailXid = xid;

	LWLockRelease(OldSerXidLock);
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
	int			tailPage;

	LWLockAcquire(OldSerXidLock, LW_EXCLUSIVE);

	/* Exit quickly if the SLRU is currently not in use. */
	if (oldSerXidControl->headPage < 0)
	{
		LWLockRelease(OldSerXidLock);
		return;
	}

	if (TransactionIdIsValid(oldSerXidControl->tailXid))
	{
		/* We can truncate the SLRU up to the page containing tailXid */
		tailPage = OldSerXidPage(oldSerXidControl->tailXid);
	}
	else
	{
		/*
		 * The SLRU is no longer needed. Truncate to head before we set head
		 * invalid.
		 *
		 * XXX: It's possible that the SLRU is not needed again until XID
		 * wrap-around has happened, so that the segment containing headPage
		 * that we leave behind will appear to be new again. In that case it
		 * won't be removed until XID horizon advances enough to make it
		 * current again.
		 */
		tailPage = oldSerXidControl->headPage;
		oldSerXidControl->headPage = -1;
	}

	LWLockRelease(OldSerXidLock);

	/* Truncate away pages that are no longer required */
	SimpleLruTruncate(OldSerXidSlruCtl, tailPage);

	/*
	 * Flush dirty SLRU pages to disk
	 *
	 * This is not actually necessary from a correctness point of view. We do
	 * it merely as a debugging aid.
	 *
	 * We're doing this after the truncation to avoid writing pages right
	 * before deleting the file in which they sit, which would be completely
	 * pointless.
	 */
	SimpleLruFlush(OldSerXidSlruCtl, true);
}

/*------------------------------------------------------------------------*/

/*
 * InitPredicateLocks -- Initialize the predicate locking data structures.
 *
 * This is called from CreateSharedMemoryAndSemaphores(), which see for
 * more comments.  In the normal postmaster case, the shared hash tables
 * are created here.  Backends inherit the pointers
 * to the shared tables via fork().  In the EXEC_BACKEND case, each
 * backend re-executes this code to obtain pointers to the already existing
 * shared hash tables.
 */
void
InitPredicateLocks(void)
{
	HASHCTL		info;
	long		max_table_size;
	Size		requestSize;
	bool		found;

	/*
	 * Compute size of predicate lock target hashtable. Note these
	 * calculations must agree with PredicateLockShmemSize!
	 */
	max_table_size = NPREDICATELOCKTARGETENTS();

	/*
	 * Allocate hash table for PREDICATELOCKTARGET structs.  This stores
	 * per-predicate-lock-target information.
	 */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(PREDICATELOCKTARGETTAG);
	info.entrysize = sizeof(PREDICATELOCKTARGET);
	info.num_partitions = NUM_PREDICATELOCK_PARTITIONS;

	PredicateLockTargetHash = ShmemInitHash("PREDICATELOCKTARGET hash",
											max_table_size,
											max_table_size,
											&info,
											HASH_ELEM | HASH_BLOBS |
											HASH_PARTITION | HASH_FIXED_SIZE);

	/* Assume an average of 2 xacts per target */
	max_table_size *= 2;

	/*
	 * Reserve a dummy entry in the hash table; we use it to make sure there's
	 * always one entry available when we need to split or combine a page,
	 * because running out of space there could mean aborting a
	 * non-serializable transaction.
	 */
	hash_search(PredicateLockTargetHash, &ScratchTargetTag, HASH_ENTER, NULL);

	/*
	 * Allocate hash table for PREDICATELOCK structs.  This stores per
	 * xact-lock-of-a-target information.
	 */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(PREDICATELOCKTAG);
	info.entrysize = sizeof(PREDICATELOCK);
	info.hash = predicatelock_hash;
	info.num_partitions = NUM_PREDICATELOCK_PARTITIONS;

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
	if (!found)
	{
		int			i;

		SHMQueueInit(&PredXact->availableList);
		SHMQueueInit(&PredXact->activeList);
		PredXact->SxactGlobalXmin = InvalidTransactionId;
		PredXact->SxactGlobalXminCount = 0;
		PredXact->WritableSxactCount = 0;
		PredXact->LastSxactCommitSeqNo = FirstNormalSerCommitSeqNo - 1;
		PredXact->CanPartialClearThrough = 0;
		PredXact->HavePartialClearedThrough = 0;
		requestSize = mul_size((Size) max_table_size,
							   PredXactListElementDataSize);
		PredXact->element = ShmemAlloc(requestSize);
		/* Add all elements to available list, clean. */
		memset(PredXact->element, 0, requestSize);
		for (i = 0; i < max_table_size; i++)
		{
			SHMQueueInsertBefore(&(PredXact->availableList),
								 &(PredXact->element[i].link));
		}
		PredXact->OldCommittedSxact = CreatePredXact();
		SetInvalidVirtualTransactionId(PredXact->OldCommittedSxact->vxid);
		PredXact->OldCommittedSxact->prepareSeqNo = 0;
		PredXact->OldCommittedSxact->commitSeqNo = 0;
		PredXact->OldCommittedSxact->SeqNo.lastCommitBeforeSnapshot = 0;
		SHMQueueInit(&PredXact->OldCommittedSxact->outConflicts);
		SHMQueueInit(&PredXact->OldCommittedSxact->inConflicts);
		SHMQueueInit(&PredXact->OldCommittedSxact->predicateLocks);
		SHMQueueInit(&PredXact->OldCommittedSxact->finishedLink);
		SHMQueueInit(&PredXact->OldCommittedSxact->possibleUnsafeConflicts);
		PredXact->OldCommittedSxact->topXid = InvalidTransactionId;
		PredXact->OldCommittedSxact->finishedBefore = InvalidTransactionId;
		PredXact->OldCommittedSxact->xmin = InvalidTransactionId;
		PredXact->OldCommittedSxact->flags = SXACT_FLAG_COMMITTED;
		PredXact->OldCommittedSxact->pid = 0;
	}
	/* This never changes, so let's keep a local copy. */
	OldCommittedSxact = PredXact->OldCommittedSxact;

	/*
	 * Allocate hash table for SERIALIZABLEXID structs.  This stores per-xid
	 * information for serializable transactions which have accessed data.
	 */
	MemSet(&info, 0, sizeof(info));
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
	if (!found)
	{
		int			i;

		SHMQueueInit(&RWConflictPool->availableList);
		requestSize = mul_size((Size) max_table_size,
							   RWConflictDataSize);
		RWConflictPool->element = ShmemAlloc(requestSize);
		/* Add all elements to available list, clean. */
		memset(RWConflictPool->element, 0, requestSize);
		for (i = 0; i < max_table_size; i++)
		{
			SHMQueueInsertBefore(&(RWConflictPool->availableList),
								 &(RWConflictPool->element[i].outLink));
		}
	}

	/*
	 * Create or attach to the header for the list of finished serializable
	 * transactions.
	 */
	FinishedSerializableTransactions = (SHM_QUEUE *)
		ShmemInitStruct("FinishedSerializableTransactions",
						sizeof(SHM_QUEUE),
						&found);
	if (!found)
		SHMQueueInit(FinishedSerializableTransactions);

	/*
	 * Initialize the SLRU storage for old committed serializable
	 * transactions.
	 */
	OldSerXidInit();

	/* Pre-calculate the hash and partition lock of the scratch entry */
	ScratchTargetTagHash = PredicateLockTargetTagHashCode(&ScratchTargetTag);
	ScratchPartitionLock = PredicateLockHashPartitionLock(ScratchTargetTagHash);
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
								   PredXactListElementDataSize));

	/* transaction xid table */
	size = add_size(size, hash_estimate_size(max_table_size,
											 sizeof(SERIALIZABLEXID)));

	/* rw-conflict pool */
	max_table_size *= 5;
	size = add_size(size, RWConflictPoolHeaderDataSize);
	size = add_size(size, mul_size((Size) max_table_size,
								   RWConflictDataSize));

	/* Head for list of finished serializable transactions. */
	size = add_size(size, sizeof(SHM_QUEUE));

	/* Shared memory structures for SLRU tracking of old committed xids. */
	size = add_size(size, sizeof(OldSerXidControlData));
	size = add_size(size, SimpleLruShmemSize(NUM_OLDSERXID_BUFFERS, 0));

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
	if (SHMQueueEmpty(FinishedSerializableTransactions))
	{
		LWLockRelease(SerializableFinishedListLock);
		return;
	}

	/*
	 * Grab the first sxact off the finished list -- this will be the earliest
	 * commit.  Remove it from the list.
	 */
	sxact = (SERIALIZABLEXACT *)
		SHMQueueNext(FinishedSerializableTransactions,
					 FinishedSerializableTransactions,
					 offsetof(SERIALIZABLEXACT, finishedLink));
	SHMQueueDelete(&(sxact->finishedLink));

	/* Add to SLRU summary information. */
	if (TransactionIdIsValid(sxact->topXid) && !SxactIsReadOnly(sxact))
		OldSerXidAdd(sxact->topXid, SxactHasConflictOut(sxact)
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
													   InvalidTransactionId);

		if (MySerializableXact == InvalidSerializableXact)
			return snapshot;	/* no concurrent r/w xacts; it's safe */

		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

		/*
		 * Wait for concurrent transactions to finish. Stop early if one of
		 * them marked us as conflicted.
		 */
		MySerializableXact->flags |= SXACT_FLAG_DEFERRABLE_WAITING;
		while (!(SHMQueueEmpty(&MySerializableXact->possibleUnsafeConflicts) ||
				 SxactIsROUnsafe(MySerializableXact)))
		{
			LWLockRelease(SerializableXactHashLock);
			ProcWaitForSignal();
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
				 errmsg("deferrable snapshot was unsafe; trying a new one")));
		ReleasePredicateLocks(false);
	}

	/*
	 * Now we have a safe snapshot, so we don't need to do any further checks.
	 */
	Assert(SxactIsROSafe(MySerializableXact));
	ReleasePredicateLocks(false);

	return snapshot;
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
	 * check_XactIsoLevel() if default_transaction_isolation is set to
	 * serializable, so phrase the hint accordingly.
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
												 InvalidTransactionId);
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
								   TransactionId sourcexid)
{
	Assert(IsolationIsSerializable());

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

	(void) GetSerializableTransactionSnapshotInt(snapshot, sourcexid);
}

/*
 * Guts of GetSerializableTransactionSnapshot
 *
 * If sourcexid is valid, this is actually an import operation and we should
 * skip calling GetSnapshotData, because the snapshot contents are already
 * loaded up.  HOWEVER: to avoid race conditions, we must check that the
 * source xact is still running after we acquire SerializableXactHashLock.
 * We do that by calling ProcArrayInstallImportedXmin.
 */
static Snapshot
GetSerializableTransactionSnapshotInt(Snapshot snapshot,
									  TransactionId sourcexid)
{
	PGPROC	   *proc;
	VirtualTransactionId vxid;
	SERIALIZABLEXACT *sxact,
			   *othersxact;
	HASHCTL		hash_ctl;

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
#ifdef TEST_OLDSERXID
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
	if (!TransactionIdIsValid(sourcexid))
		snapshot = GetSnapshotData(snapshot);
	else if (!ProcArrayInstallImportedXmin(snapshot->xmin, sourcexid))
	{
		ReleasePredXact(sxact);
		LWLockRelease(SerializableXactHashLock);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("could not import the requested snapshot"),
			   errdetail("The source transaction %u is not running anymore.",
						 sourcexid)));
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

	/* Maintain serializable global xmin info. */
	if (!TransactionIdIsValid(PredXact->SxactGlobalXmin))
	{
		Assert(PredXact->SxactGlobalXminCount == 0);
		PredXact->SxactGlobalXmin = snapshot->xmin;
		PredXact->SxactGlobalXminCount = 1;
		OldSerXidSetActiveSerXmin(snapshot->xmin);
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

	/* Initialize the structure. */
	sxact->vxid = vxid;
	sxact->SeqNo.lastCommitBeforeSnapshot = PredXact->LastSxactCommitSeqNo;
	sxact->prepareSeqNo = InvalidSerCommitSeqNo;
	sxact->commitSeqNo = InvalidSerCommitSeqNo;
	SHMQueueInit(&(sxact->outConflicts));
	SHMQueueInit(&(sxact->inConflicts));
	SHMQueueInit(&(sxact->possibleUnsafeConflicts));
	sxact->topXid = GetTopTransactionIdIfAny();
	sxact->finishedBefore = InvalidTransactionId;
	sxact->xmin = snapshot->xmin;
	sxact->pid = MyProcPid;
	SHMQueueInit(&(sxact->predicateLocks));
	SHMQueueElemInit(&(sxact->finishedLink));
	sxact->flags = 0;
	if (XactReadOnly)
	{
		sxact->flags |= SXACT_FLAG_READ_ONLY;

		/*
		 * Register all concurrent r/w transactions as possible conflicts; if
		 * all of them commit without any outgoing conflicts to earlier
		 * transactions then this snapshot can be deemed safe (and we can run
		 * without tracking predicate locks).
		 */
		for (othersxact = FirstPredXact();
			 othersxact != NULL;
			 othersxact = NextPredXact(othersxact))
		{
			if (!SxactIsCommitted(othersxact)
				&& !SxactIsDoomed(othersxact)
				&& !SxactIsReadOnly(othersxact))
			{
				SetPossibleUnsafeConflict(sxact, othersxact);
			}
		}
	}
	else
	{
		++(PredXact->WritableSxactCount);
		Assert(PredXact->WritableSxactCount <=
			   (MaxBackends + max_prepared_xacts));
	}

	MySerializableXact = sxact;
	MyXactDidWrite = false;		/* haven't written anything yet */

	LWLockRelease(SerializableXactHashLock);

	/* Initialize the backend-local hash table of parent locks */
	Assert(LocalPredicateLockHash == NULL);
	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(PREDICATELOCKTARGETTAG);
	hash_ctl.entrysize = sizeof(LOCALPREDICATELOCK);
	LocalPredicateLockHash = hash_create("Local predicate lock",
										 max_predicate_locks_per_xact,
										 &hash_ctl,
										 HASH_ELEM | HASH_BLOBS);

	return snapshot;
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
									relation->rd_node.dbNode,
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
 * scratch space. The caller must be holding SerializablePredicateLockListLock,
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

	Assert(LWLockHeldByMe(SerializablePredicateLockListLock));

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

	Assert(LWLockHeldByMe(SerializablePredicateLockListLock));

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

	Assert(LWLockHeldByMe(SerializablePredicateLockListLock));

	/* Can't remove it until no locks at this target. */
	if (!SHMQueueEmpty(&target->predicateLocks))
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
 * We aren't acquiring lightweight locks for the predicate lock or lock
 * target structures associated with this transaction unless we're going
 * to modify them, because no other process is permitted to modify our
 * locks.
 */
static void
DeleteChildTargetLocks(const PREDICATELOCKTARGETTAG *newtargettag)
{
	SERIALIZABLEXACT *sxact;
	PREDICATELOCK *predlock;

	LWLockAcquire(SerializablePredicateLockListLock, LW_SHARED);
	sxact = MySerializableXact;
	predlock = (PREDICATELOCK *)
		SHMQueueNext(&(sxact->predicateLocks),
					 &(sxact->predicateLocks),
					 offsetof(PREDICATELOCK, xactLink));
	while (predlock)
	{
		SHM_QUEUE  *predlocksxactlink;
		PREDICATELOCK *nextpredlock;
		PREDICATELOCKTAG oldlocktag;
		PREDICATELOCKTARGET *oldtarget;
		PREDICATELOCKTARGETTAG oldtargettag;

		predlocksxactlink = &(predlock->xactLink);
		nextpredlock = (PREDICATELOCK *)
			SHMQueueNext(&(sxact->predicateLocks),
						 predlocksxactlink,
						 offsetof(PREDICATELOCK, xactLink));

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

			SHMQueueDelete(predlocksxactlink);
			SHMQueueDelete(&(predlock->targetLink));
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

		predlock = nextpredlock;
	}
	LWLockRelease(SerializablePredicateLockListLock);
}

/*
 * Returns the promotion threshold for a given predicate lock
 * target. This is the number of descendant locks required to promote
 * to the specified tag. Note that the threshold includes non-direct
 * descendants, e.g. both tuples and pages for a relation lock.
 *
 * TODO SSI: We should do something more intelligent about what the
 * thresholds are, either making it proportional to the number of
 * tuples in a page & pages in a relation, or at least making it a
 * GUC. Currently the threshold is 3 for a page lock, and
 * max_pred_locks_per_transaction/2 for a relation lock, chosen
 * entirely arbitrarily (and without benchmarking).
 */
static int
PredicateLockPromotionThreshold(const PREDICATELOCKTARGETTAG *tag)
{
	switch (GET_PREDICATELOCKTARGETTAG_TYPE(*tag))
	{
		case PREDLOCKTAG_RELATION:
			return max_predicate_locks_per_xact / 2;

		case PREDLOCKTAG_PAGE:
			return 3;

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

		if (parentlock->childLocks >=
			PredicateLockPromotionThreshold(&targettag))
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

	LWLockAcquire(SerializablePredicateLockListLock, LW_SHARED);
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
				 errhint("You might need to increase max_pred_locks_per_transaction.")));
	if (!found)
		SHMQueueInit(&(target->predicateLocks));

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
				 errhint("You might need to increase max_pred_locks_per_transaction.")));

	if (!found)
	{
		SHMQueueInsertBefore(&(target->predicateLocks), &(lock->targetLink));
		SHMQueueInsertBefore(&(sxact->predicateLocks),
							 &(lock->xactLink));
		lock->commitSeqNo = InvalidSerCommitSeqNo;
	}

	LWLockRelease(partitionLock);
	LWLockRelease(SerializablePredicateLockListLock);
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
										relation->rd_node.dbNode,
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
									relation->rd_node.dbNode,
									relation->rd_id,
									blkno);
	PredicateLockAcquire(&tag);
}

/*
 *		PredicateLockTuple
 *
 * Gets a predicate lock at the tuple level.
 * Skip if not in full serializable transaction isolation level.
 * Skip if this is a temporary table.
 */
void
PredicateLockTuple(Relation relation, HeapTuple tuple, Snapshot snapshot)
{
	PREDICATELOCKTARGETTAG tag;
	ItemPointer tid;
	TransactionId targetxmin;

	if (!SerializationNeededForRead(relation, snapshot))
		return;

	/*
	 * If it's a heap tuple, return if this xact wrote it.
	 */
	if (relation->rd_index == NULL)
	{
		TransactionId myxid;

		targetxmin = HeapTupleHeaderGetXmin(tuple->t_data);

		myxid = GetTopTransactionIdIfAny();
		if (TransactionIdIsValid(myxid))
		{
			if (TransactionIdFollowsOrEquals(targetxmin, TransactionXmin))
			{
				TransactionId xid = SubTransGetTopmostTransaction(targetxmin);

				if (TransactionIdEquals(xid, myxid))
				{
					/* We wrote it; we already have a write lock. */
					return;
				}
			}
		}
	}

	/*
	 * Do quick-but-not-definitive test for a relation lock first.  This will
	 * never cause a return when the relation is *not* locked, but will
	 * occasionally let the check continue when there really *is* a relation
	 * level lock.
	 */
	SET_PREDICATELOCKTARGETTAG_RELATION(tag,
										relation->rd_node.dbNode,
										relation->rd_id);
	if (PredicateLockExists(&tag))
		return;

	tid = &(tuple->t_self);
	SET_PREDICATELOCKTARGETTAG_TUPLE(tag,
									 relation->rd_node.dbNode,
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
 * Caller must hold SerializablePredicateLockListLock and the
 * appropriate hash partition lock for the target.
 */
static void
DeleteLockTarget(PREDICATELOCKTARGET *target, uint32 targettaghash)
{
	PREDICATELOCK *predlock;
	SHM_QUEUE  *predlocktargetlink;
	PREDICATELOCK *nextpredlock;
	bool		found;

	Assert(LWLockHeldByMe(SerializablePredicateLockListLock));
	Assert(LWLockHeldByMe(PredicateLockHashPartitionLock(targettaghash)));

	predlock = (PREDICATELOCK *)
		SHMQueueNext(&(target->predicateLocks),
					 &(target->predicateLocks),
					 offsetof(PREDICATELOCK, targetLink));
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
	while (predlock)
	{
		predlocktargetlink = &(predlock->targetLink);
		nextpredlock = (PREDICATELOCK *)
			SHMQueueNext(&(target->predicateLocks),
						 predlocktargetlink,
						 offsetof(PREDICATELOCK, targetLink));

		SHMQueueDelete(&(predlock->xactLink));
		SHMQueueDelete(&(predlock->targetLink));

		hash_search_with_hash_value
			(PredicateLockHash,
			 &predlock->tag,
			 PredicateLockHashCodeFromTargetHashCode(&predlock->tag,
													 targettaghash),
			 HASH_REMOVE, &found);
		Assert(found);

		predlock = nextpredlock;
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
 * Caller must hold SerializablePredicateLockListLock.
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

	Assert(LWLockHeldByMe(SerializablePredicateLockListLock));

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
		PREDICATELOCK *oldpredlock;
		PREDICATELOCKTAG newpredlocktag;

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
			SHMQueueInit(&(newtarget->predicateLocks));

		newpredlocktag.myTarget = newtarget;

		/*
		 * Loop through all the locks on the old target, replacing them with
		 * locks on the new target.
		 */
		oldpredlock = (PREDICATELOCK *)
			SHMQueueNext(&(oldtarget->predicateLocks),
						 &(oldtarget->predicateLocks),
						 offsetof(PREDICATELOCK, targetLink));
		LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);
		while (oldpredlock)
		{
			SHM_QUEUE  *predlocktargetlink;
			PREDICATELOCK *nextpredlock;
			PREDICATELOCK *newpredlock;
			SerCommitSeqNo oldCommitSeqNo = oldpredlock->commitSeqNo;

			predlocktargetlink = &(oldpredlock->targetLink);
			nextpredlock = (PREDICATELOCK *)
				SHMQueueNext(&(oldtarget->predicateLocks),
							 predlocktargetlink,
							 offsetof(PREDICATELOCK, targetLink));
			newpredlocktag.myXact = oldpredlock->tag.myXact;

			if (removeOld)
			{
				SHMQueueDelete(&(oldpredlock->xactLink));
				SHMQueueDelete(&(oldpredlock->targetLink));

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
				SHMQueueInsertBefore(&(newtarget->predicateLocks),
									 &(newpredlock->targetLink));
				SHMQueueInsertBefore(&(newpredlocktag.myXact->predicateLocks),
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

			oldpredlock = nextpredlock;
		}
		LWLockRelease(SerializableXactHashLock);

		if (removeOld)
		{
			Assert(SHMQueueEmpty(&oldtarget->predicateLocks));
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

		/* Put the scrach entry back */
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

	dbId = relation->rd_node.dbNode;
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
	Assert(transfer || !isIndex);		/* index OID only makes sense with
										 * transfer */

	/* Retrieve first time needed, then keep. */
	heaptargettaghash = 0;
	heaptarget = NULL;

	/* Acquire locks on all lock partitions */
	LWLockAcquire(SerializablePredicateLockListLock, LW_EXCLUSIVE);
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
		PREDICATELOCK *oldpredlock;

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
				SHMQueueInit(&heaptarget->predicateLocks);
		}

		/*
		 * Loop through all the locks on the old target, replacing them with
		 * locks on the new target.
		 */
		oldpredlock = (PREDICATELOCK *)
			SHMQueueNext(&(oldtarget->predicateLocks),
						 &(oldtarget->predicateLocks),
						 offsetof(PREDICATELOCK, targetLink));
		while (oldpredlock)
		{
			PREDICATELOCK *nextpredlock;
			PREDICATELOCK *newpredlock;
			SerCommitSeqNo oldCommitSeqNo;
			SERIALIZABLEXACT *oldXact;

			nextpredlock = (PREDICATELOCK *)
				SHMQueueNext(&(oldtarget->predicateLocks),
							 &(oldpredlock->targetLink),
							 offsetof(PREDICATELOCK, targetLink));

			/*
			 * Remove the old lock first. This avoids the chance of running
			 * out of lock structure entries for the hash table.
			 */
			oldCommitSeqNo = oldpredlock->commitSeqNo;
			oldXact = oldpredlock->tag.myXact;

			SHMQueueDelete(&(oldpredlock->xactLink));

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
					SHMQueueInsertBefore(&(heaptarget->predicateLocks),
										 &(newpredlock->targetLink));
					SHMQueueInsertBefore(&(newpredlocktag.myXact->predicateLocks),
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

			oldpredlock = nextpredlock;
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
	LWLockRelease(SerializablePredicateLockListLock);
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
									relation->rd_node.dbNode,
									relation->rd_id,
									oldblkno);
	SET_PREDICATELOCKTARGETTAG_PAGE(newtargettag,
									relation->rd_node.dbNode,
									relation->rd_id,
									newblkno);

	LWLockAcquire(SerializablePredicateLockListLock, LW_EXCLUSIVE);

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

	LWLockRelease(SerializablePredicateLockListLock);
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
	SERIALIZABLEXACT *sxact;

	Assert(LWLockHeldByMe(SerializableXactHashLock));

	PredXact->SxactGlobalXmin = InvalidTransactionId;
	PredXact->SxactGlobalXminCount = 0;

	for (sxact = FirstPredXact(); sxact != NULL; sxact = NextPredXact(sxact))
	{
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

	OldSerXidSetActiveSerXmin(PredXact->SxactGlobalXmin);
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
 */
void
ReleasePredicateLocks(bool isCommit)
{
	bool		needToClear;
	RWConflict	conflict,
				nextConflict,
				possibleUnsafeConflict;
	SERIALIZABLEXACT *roXact;

	/*
	 * We can't trust XactReadOnly here, because a transaction which started
	 * as READ WRITE can show as READ ONLY later, e.g., within
	 * substransactions.  We want to flag a transaction as READ ONLY if it
	 * commits without writing so that de facto READ ONLY transactions get the
	 * benefit of some RO optimizations, so we will use this local variable to
	 * get some cleanup logic right which is based on whether the transaction
	 * was declared READ ONLY at the top level.
	 */
	bool		topLevelIsDeclaredReadOnly;

	if (MySerializableXact == InvalidSerializableXact)
	{
		Assert(LocalPredicateLockHash == NULL);
		return;
	}

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	Assert(!isCommit || SxactIsPrepared(MySerializableXact));
	Assert(!isCommit || !SxactIsDoomed(MySerializableXact));
	Assert(!SxactIsCommitted(MySerializableXact));
	Assert(!SxactIsRolledBack(MySerializableXact));

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
	 * GlobalSerializableXmin must advance before this transaction can be
	 * fully cleaned up.  The worst that could happen is we wait for one more
	 * transaction to complete before freeing some RAM; correctness of visible
	 * behavior is not affected.
	 */
	MySerializableXact->finishedBefore = ShmemVariableCache->nextXid;

	/*
	 * If it's not a commit it's a rollback, and we can clear our locks
	 * immediately.
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
		possibleUnsafeConflict = (RWConflict)
			SHMQueueNext(&MySerializableXact->possibleUnsafeConflicts,
						 &MySerializableXact->possibleUnsafeConflicts,
						 offsetof(RWConflictData, inLink));
		while (possibleUnsafeConflict)
		{
			nextConflict = (RWConflict)
				SHMQueueNext(&MySerializableXact->possibleUnsafeConflicts,
							 &possibleUnsafeConflict->inLink,
							 offsetof(RWConflictData, inLink));

			Assert(!SxactIsReadOnly(possibleUnsafeConflict->sxactOut));
			Assert(MySerializableXact == possibleUnsafeConflict->sxactIn);

			ReleaseRWConflict(possibleUnsafeConflict);

			possibleUnsafeConflict = nextConflict;
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
	conflict = (RWConflict)
		SHMQueueNext(&MySerializableXact->outConflicts,
					 &MySerializableXact->outConflicts,
					 offsetof(RWConflictData, outLink));
	while (conflict)
	{
		nextConflict = (RWConflict)
			SHMQueueNext(&MySerializableXact->outConflicts,
						 &conflict->outLink,
						 offsetof(RWConflictData, outLink));

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

		conflict = nextConflict;
	}

	/*
	 * Release all inConflicts from committed and read-only transactions. If
	 * we're rolling back, clear them all.
	 */
	conflict = (RWConflict)
		SHMQueueNext(&MySerializableXact->inConflicts,
					 &MySerializableXact->inConflicts,
					 offsetof(RWConflictData, inLink));
	while (conflict)
	{
		nextConflict = (RWConflict)
			SHMQueueNext(&MySerializableXact->inConflicts,
						 &conflict->inLink,
						 offsetof(RWConflictData, inLink));

		if (!isCommit
			|| SxactIsCommitted(conflict->sxactOut)
			|| SxactIsReadOnly(conflict->sxactOut))
			ReleaseRWConflict(conflict);

		conflict = nextConflict;
	}

	if (!topLevelIsDeclaredReadOnly)
	{
		/*
		 * Remove ourselves from the list of possible conflicts for concurrent
		 * READ ONLY transactions, flagging them as unsafe if we have a
		 * conflict out. If any are waiting DEFERRABLE transactions, wake them
		 * up if they are known safe or known unsafe.
		 */
		possibleUnsafeConflict = (RWConflict)
			SHMQueueNext(&MySerializableXact->possibleUnsafeConflicts,
						 &MySerializableXact->possibleUnsafeConflicts,
						 offsetof(RWConflictData, outLink));
		while (possibleUnsafeConflict)
		{
			nextConflict = (RWConflict)
				SHMQueueNext(&MySerializableXact->possibleUnsafeConflicts,
							 &possibleUnsafeConflict->outLink,
							 offsetof(RWConflictData, outLink));

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
				if (SHMQueueEmpty(&roXact->possibleUnsafeConflicts))
					roXact->flags |= SXACT_FLAG_RO_SAFE;
			}

			/*
			 * Wake up the process for a waiting DEFERRABLE transaction if we
			 * now know it's either safe or conflicted.
			 */
			if (SxactIsDeferrableWaiting(roXact) &&
				(SxactIsROUnsafe(roXact) || SxactIsROSafe(roXact)))
				ProcSendSignal(roXact->pid);

			possibleUnsafeConflict = nextConflict;
		}
	}

	/*
	 * Check whether it's time to clean up old transactions. This can only be
	 * done when the last serializable transaction with the oldest xmin among
	 * serializable transactions completes.  We then find the "new oldest"
	 * xmin and purge any transactions which finished before this transaction
	 * was launched.
	 */
	needToClear = false;
	if (TransactionIdEquals(MySerializableXact->xmin, PredXact->SxactGlobalXmin))
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
		SHMQueueInsertBefore(FinishedSerializableTransactions,
							 &MySerializableXact->finishedLink);

	if (!isCommit)
		ReleaseOneSerializableXact(MySerializableXact, false, false);

	LWLockRelease(SerializableFinishedListLock);

	if (needToClear)
		ClearOldPredicateLocks();

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
	SERIALIZABLEXACT *finishedSxact;
	PREDICATELOCK *predlock;

	/*
	 * Loop through finished transactions. They are in commit order, so we can
	 * stop as soon as we find one that's still interesting.
	 */
	LWLockAcquire(SerializableFinishedListLock, LW_EXCLUSIVE);
	finishedSxact = (SERIALIZABLEXACT *)
		SHMQueueNext(FinishedSerializableTransactions,
					 FinishedSerializableTransactions,
					 offsetof(SERIALIZABLEXACT, finishedLink));
	LWLockAcquire(SerializableXactHashLock, LW_SHARED);
	while (finishedSxact)
	{
		SERIALIZABLEXACT *nextSxact;

		nextSxact = (SERIALIZABLEXACT *)
			SHMQueueNext(FinishedSerializableTransactions,
						 &(finishedSxact->finishedLink),
						 offsetof(SERIALIZABLEXACT, finishedLink));
		if (!TransactionIdIsValid(PredXact->SxactGlobalXmin)
			|| TransactionIdPrecedesOrEquals(finishedSxact->finishedBefore,
											 PredXact->SxactGlobalXmin))
		{
			/*
			 * This transaction committed before any in-progress transaction
			 * took its snapshot. It's no longer interesting.
			 */
			LWLockRelease(SerializableXactHashLock);
			SHMQueueDelete(&(finishedSxact->finishedLink));
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
				SHMQueueDelete(&(finishedSxact->finishedLink));
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
		finishedSxact = nextSxact;
	}
	LWLockRelease(SerializableXactHashLock);

	/*
	 * Loop through predicate locks on dummy transaction for summarized data.
	 */
	LWLockAcquire(SerializablePredicateLockListLock, LW_SHARED);
	predlock = (PREDICATELOCK *)
		SHMQueueNext(&OldCommittedSxact->predicateLocks,
					 &OldCommittedSxact->predicateLocks,
					 offsetof(PREDICATELOCK, xactLink));
	while (predlock)
	{
		PREDICATELOCK *nextpredlock;
		bool		canDoPartialCleanup;

		nextpredlock = (PREDICATELOCK *)
			SHMQueueNext(&OldCommittedSxact->predicateLocks,
						 &predlock->xactLink,
						 offsetof(PREDICATELOCK, xactLink));

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

			SHMQueueDelete(&(predlock->targetLink));
			SHMQueueDelete(&(predlock->xactLink));

			hash_search_with_hash_value(PredicateLockHash, &tag,
								PredicateLockHashCodeFromTargetHashCode(&tag,
															  targettaghash),
										HASH_REMOVE, NULL);
			RemoveTargetIfNoLongerUsed(target, targettaghash);

			LWLockRelease(partitionLock);
		}

		predlock = nextpredlock;
	}

	LWLockRelease(SerializablePredicateLockListLock);
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
	PREDICATELOCK *predlock;
	SERIALIZABLEXIDTAG sxidtag;
	RWConflict	conflict,
				nextConflict;

	Assert(sxact != NULL);
	Assert(SxactIsRolledBack(sxact) || SxactIsCommitted(sxact));
	Assert(partial || !SxactIsOnFinishedList(sxact));
	Assert(LWLockHeldByMe(SerializableFinishedListLock));

	/*
	 * First release all the predicate locks held by this xact (or transfer
	 * them to OldCommittedSxact if summarize is true)
	 */
	LWLockAcquire(SerializablePredicateLockListLock, LW_SHARED);
	predlock = (PREDICATELOCK *)
		SHMQueueNext(&(sxact->predicateLocks),
					 &(sxact->predicateLocks),
					 offsetof(PREDICATELOCK, xactLink));
	while (predlock)
	{
		PREDICATELOCK *nextpredlock;
		PREDICATELOCKTAG tag;
		SHM_QUEUE  *targetLink;
		PREDICATELOCKTARGET *target;
		PREDICATELOCKTARGETTAG targettag;
		uint32		targettaghash;
		LWLock	   *partitionLock;

		nextpredlock = (PREDICATELOCK *)
			SHMQueueNext(&(sxact->predicateLocks),
						 &(predlock->xactLink),
						 offsetof(PREDICATELOCK, xactLink));

		tag = predlock->tag;
		targetLink = &(predlock->targetLink);
		target = tag.myTarget;
		targettag = target->tag;
		targettaghash = PredicateLockTargetTagHashCode(&targettag);
		partitionLock = PredicateLockHashPartitionLock(targettaghash);

		LWLockAcquire(partitionLock, LW_EXCLUSIVE);

		SHMQueueDelete(targetLink);

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
						 errhint("You might need to increase max_pred_locks_per_transaction.")));
			if (found)
			{
				Assert(predlock->commitSeqNo != 0);
				Assert(predlock->commitSeqNo != InvalidSerCommitSeqNo);
				if (predlock->commitSeqNo < sxact->commitSeqNo)
					predlock->commitSeqNo = sxact->commitSeqNo;
			}
			else
			{
				SHMQueueInsertBefore(&(target->predicateLocks),
									 &(predlock->targetLink));
				SHMQueueInsertBefore(&(OldCommittedSxact->predicateLocks),
									 &(predlock->xactLink));
				predlock->commitSeqNo = sxact->commitSeqNo;
			}
		}
		else
			RemoveTargetIfNoLongerUsed(target, targettaghash);

		LWLockRelease(partitionLock);

		predlock = nextpredlock;
	}

	/*
	 * Rather than retail removal, just re-init the head after we've run
	 * through the list.
	 */
	SHMQueueInit(&sxact->predicateLocks);

	LWLockRelease(SerializablePredicateLockListLock);

	sxidtag.xid = sxact->topXid;
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* Release all outConflicts (unless 'partial' is true) */
	if (!partial)
	{
		conflict = (RWConflict)
			SHMQueueNext(&sxact->outConflicts,
						 &sxact->outConflicts,
						 offsetof(RWConflictData, outLink));
		while (conflict)
		{
			nextConflict = (RWConflict)
				SHMQueueNext(&sxact->outConflicts,
							 &conflict->outLink,
							 offsetof(RWConflictData, outLink));
			if (summarize)
				conflict->sxactIn->flags |= SXACT_FLAG_SUMMARY_CONFLICT_IN;
			ReleaseRWConflict(conflict);
			conflict = nextConflict;
		}
	}

	/* Release all inConflicts. */
	conflict = (RWConflict)
		SHMQueueNext(&sxact->inConflicts,
					 &sxact->inConflicts,
					 offsetof(RWConflictData, inLink));
	while (conflict)
	{
		nextConflict = (RWConflict)
			SHMQueueNext(&sxact->inConflicts,
						 &conflict->inLink,
						 offsetof(RWConflictData, inLink));
		if (summarize)
			conflict->sxactOut->flags |= SXACT_FLAG_SUMMARY_CONFLICT_OUT;
		ReleaseRWConflict(conflict);
		conflict = nextConflict;
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
	uint32		i;

	Assert(TransactionIdIsValid(xid));
	Assert(!TransactionIdEquals(xid, GetTopTransactionIdIfAny()));

	snap = GetTransactionSnapshot();

	if (TransactionIdPrecedes(xid, snap->xmin))
		return false;

	if (TransactionIdFollowsOrEquals(xid, snap->xmax))
		return true;

	for (i = 0; i < snap->xcnt; i++)
	{
		if (xid == snap->xip[i])
			return true;
	}

	return false;
}

/*
 * CheckForSerializableConflictOut
 *		We are reading a tuple which has been modified.  If it is visible to
 *		us but has been deleted, that indicates a rw-conflict out.  If it's
 *		not visible and was created by a concurrent (overlapping)
 *		serializable transaction, that is also a rw-conflict out,
 *
 * We will determine the top level xid of the writing transaction with which
 * we may be in conflict, and check for overlap with our own transaction.
 * If the transactions overlap (i.e., they cannot see each other's writes),
 * then we have a conflict out.
 *
 * This function should be called just about anywhere in heapam.c where a
 * tuple has been read. The caller must hold at least a shared lock on the
 * buffer, because this function might set hint bits on the tuple. There is
 * currently no known reason to call this function from an index AM.
 */
void
CheckForSerializableConflictOut(bool visible, Relation relation,
								HeapTuple tuple, Buffer buffer,
								Snapshot snapshot)
{
	TransactionId xid;
	SERIALIZABLEXIDTAG sxidtag;
	SERIALIZABLEXID *sxid;
	SERIALIZABLEXACT *sxact;
	HTSV_Result htsvResult;

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

	/*
	 * Check to see whether the tuple has been written to by a concurrent
	 * transaction, either to create it not visible to us, or to delete it
	 * while it is visible to us.  The "visible" bool indicates whether the
	 * tuple is visible to us, while HeapTupleSatisfiesVacuum checks what else
	 * is going on with it.
	 */
	htsvResult = HeapTupleSatisfiesVacuum(tuple, TransactionXmin, buffer);
	switch (htsvResult)
	{
		case HEAPTUPLE_LIVE:
			if (visible)
				return;
			xid = HeapTupleHeaderGetXmin(tuple->t_data);
			break;
		case HEAPTUPLE_RECENTLY_DEAD:
			if (!visible)
				return;
			xid = HeapTupleHeaderGetUpdateXid(tuple->t_data);
			break;
		case HEAPTUPLE_DELETE_IN_PROGRESS:
			xid = HeapTupleHeaderGetUpdateXid(tuple->t_data);
			break;
		case HEAPTUPLE_INSERT_IN_PROGRESS:
			xid = HeapTupleHeaderGetXmin(tuple->t_data);
			break;
		case HEAPTUPLE_DEAD:
			return;
		default:

			/*
			 * The only way to get to this default clause is if a new value is
			 * added to the enum type without adding it to this switch
			 * statement.  That's a bug, so elog.
			 */
			elog(ERROR, "unrecognized return value from HeapTupleSatisfiesVacuum: %u", htsvResult);

			/*
			 * In spite of having all enum values covered and calling elog on
			 * this default, some compilers think this is a code path which
			 * allows xid to be used below without initialization. Silence
			 * that warning.
			 */
			xid = InvalidTransactionId;
	}
	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdFollowsOrEquals(xid, TransactionXmin));

	/*
	 * Find top level xid.  Bail out if xid is too early to be a conflict, or
	 * if it's our own xid.
	 */
	if (TransactionIdEquals(xid, GetTopTransactionIdIfAny()))
		return;
	xid = SubTransGetTopmostTransaction(xid);
	if (TransactionIdPrecedes(xid, TransactionXmin))
		return;
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

		conflictCommitSeqNo = OldSerXidGetMinConflictCommitSeqNo(xid);
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
				|| !SHMQueueEmpty(&MySerializableXact->inConflicts))
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
	PREDICATELOCK *predlock;
	PREDICATELOCK *mypredlock = NULL;
	PREDICATELOCKTAG mypredlocktag;

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
	predlock = (PREDICATELOCK *)
		SHMQueueNext(&(target->predicateLocks),
					 &(target->predicateLocks),
					 offsetof(PREDICATELOCK, targetLink));
	LWLockAcquire(SerializableXactHashLock, LW_SHARED);
	while (predlock)
	{
		SHM_QUEUE  *predlocktargetlink;
		PREDICATELOCK *nextpredlock;
		SERIALIZABLEXACT *sxact;

		predlocktargetlink = &(predlock->targetLink);
		nextpredlock = (PREDICATELOCK *)
			SHMQueueNext(&(target->predicateLocks),
						 predlocktargetlink,
						 offsetof(PREDICATELOCK, targetLink));

		sxact = predlock->tag.myXact;
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

		predlock = nextpredlock;
	}
	LWLockRelease(SerializableXactHashLock);
	LWLockRelease(partitionLock);

	/*
	 * If we found one of our own SIREAD locks to remove, remove it now.
	 *
	 * At this point our transaction already has an ExclusiveRowLock on the
	 * relation, so we are OK to drop the predicate lock on the tuple, if
	 * found, without fearing that another write against the tuple will occur
	 * before the MVCC information makes it to the buffer.
	 */
	if (mypredlock != NULL)
	{
		uint32		predlockhashcode;
		PREDICATELOCK *rmpredlock;

		LWLockAcquire(SerializablePredicateLockListLock, LW_SHARED);
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

			SHMQueueDelete(&(mypredlock->targetLink));
			SHMQueueDelete(&(mypredlock->xactLink));

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
		LWLockRelease(SerializablePredicateLockListLock);

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
CheckForSerializableConflictIn(Relation relation, HeapTuple tuple,
							   Buffer buffer)
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
	if (tuple != NULL)
	{
		SET_PREDICATELOCKTARGETTAG_TUPLE(targettag,
										 relation->rd_node.dbNode,
										 relation->rd_id,
								 ItemPointerGetBlockNumber(&(tuple->t_self)),
							   ItemPointerGetOffsetNumber(&(tuple->t_self)));
		CheckTargetForConflictsIn(&targettag);
	}

	if (BufferIsValid(buffer))
	{
		SET_PREDICATELOCKTARGETTAG_PAGE(targettag,
										relation->rd_node.dbNode,
										relation->rd_id,
										BufferGetBlockNumber(buffer));
		CheckTargetForConflictsIn(&targettag);
	}

	SET_PREDICATELOCKTARGETTAG_RELATION(targettag,
										relation->rd_node.dbNode,
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

	dbId = relation->rd_node.dbNode;
	heapId = relation->rd_id;

	LWLockAcquire(SerializablePredicateLockListLock, LW_EXCLUSIVE);
	for (i = 0; i < NUM_PREDICATELOCK_PARTITIONS; i++)
		LWLockAcquire(PredicateLockHashPartitionLockByIndex(i), LW_SHARED);
	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* Scan through target list */
	hash_seq_init(&seqstat, PredicateLockTargetHash);

	while ((target = (PREDICATELOCKTARGET *) hash_seq_search(&seqstat)))
	{
		PREDICATELOCK *predlock;

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
		predlock = (PREDICATELOCK *)
			SHMQueueNext(&(target->predicateLocks),
						 &(target->predicateLocks),
						 offsetof(PREDICATELOCK, targetLink));
		while (predlock)
		{
			PREDICATELOCK *nextpredlock;

			nextpredlock = (PREDICATELOCK *)
				SHMQueueNext(&(target->predicateLocks),
							 &(predlock->targetLink),
							 offsetof(PREDICATELOCK, targetLink));

			if (predlock->tag.myXact != MySerializableXact
			  && !RWConflictExists(predlock->tag.myXact, MySerializableXact))
			{
				FlagRWConflict(predlock->tag.myXact, MySerializableXact);
			}

			predlock = nextpredlock;
		}
	}

	/* Release locks in reverse order */
	LWLockRelease(SerializableXactHashLock);
	for (i = NUM_PREDICATELOCK_PARTITIONS - 1; i >= 0; i--)
		LWLockRelease(PredicateLockHashPartitionLockByIndex(i));
	LWLockRelease(SerializablePredicateLockListLock);
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
	RWConflict	conflict;

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
	if (!failure)
	{
		if (SxactHasSummaryConflictOut(writer))
		{
			failure = true;
			conflict = NULL;
		}
		else
			conflict = (RWConflict)
				SHMQueueNext(&writer->outConflicts,
							 &writer->outConflicts,
							 offsetof(RWConflictData, outLink));
		while (conflict)
		{
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
			conflict = (RWConflict)
				SHMQueueNext(&writer->outConflicts,
							 &conflict->outLink,
							 offsetof(RWConflictData, outLink));
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
			conflict = NULL;
		}
		else
			conflict = (RWConflict)
				SHMQueueNext(&reader->inConflicts,
							 &reader->inConflicts,
							 offsetof(RWConflictData, inLink));
		while (conflict)
		{
			SERIALIZABLEXACT *t0 = conflict->sxactOut;

			if (!SxactIsDoomed(t0)
				&& (!SxactIsCommitted(t0)
					|| t0->commitSeqNo >= writer->prepareSeqNo)
				&& (!SxactIsReadOnly(t0)
			  || t0->SeqNo.lastCommitBeforeSnapshot >= writer->prepareSeqNo))
			{
				failure = true;
				break;
			}
			conflict = (RWConflict)
				SHMQueueNext(&reader->inConflicts,
							 &conflict->inLink,
							 offsetof(RWConflictData, inLink));
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
 * PreCommit_CheckForSerializableConflicts
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
 * that we flail without ever making progress.  This transaction is
 * committing writes, so letting it commit ensures progress.  If we
 * canceled the far conflict, it might immediately fail again on retry.
 */
void
PreCommit_CheckForSerializationFailure(void)
{
	RWConflict	nearConflict;

	if (MySerializableXact == InvalidSerializableXact)
		return;

	Assert(IsolationIsSerializable());

	LWLockAcquire(SerializableXactHashLock, LW_EXCLUSIVE);

	/* Check if someone else has already decided that we need to die */
	if (SxactIsDoomed(MySerializableXact))
	{
		LWLockRelease(SerializableXactHashLock);
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to read/write dependencies among transactions"),
				 errdetail_internal("Reason code: Canceled on identification as a pivot, during commit attempt."),
				 errhint("The transaction might succeed if retried.")));
	}

	nearConflict = (RWConflict)
		SHMQueueNext(&MySerializableXact->inConflicts,
					 &MySerializableXact->inConflicts,
					 offsetof(RWConflictData, inLink));
	while (nearConflict)
	{
		if (!SxactIsCommitted(nearConflict->sxactOut)
			&& !SxactIsDoomed(nearConflict->sxactOut))
		{
			RWConflict	farConflict;

			farConflict = (RWConflict)
				SHMQueueNext(&nearConflict->sxactOut->inConflicts,
							 &nearConflict->sxactOut->inConflicts,
							 offsetof(RWConflictData, inLink));
			while (farConflict)
			{
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
				farConflict = (RWConflict)
					SHMQueueNext(&nearConflict->sxactOut->inConflicts,
								 &farConflict->inLink,
								 offsetof(RWConflictData, inLink));
			}
		}

		nearConflict = (RWConflict)
			SHMQueueNext(&MySerializableXact->inConflicts,
						 &nearConflict->inLink,
						 offsetof(RWConflictData, inLink));
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
	PREDICATELOCK *predlock;
	SERIALIZABLEXACT *sxact;
	TwoPhasePredicateRecord record;
	TwoPhasePredicateXactRecord *xactRecord;
	TwoPhasePredicateLockRecord *lockRecord;

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
	LWLockAcquire(SerializablePredicateLockListLock, LW_SHARED);

	predlock = (PREDICATELOCK *)
		SHMQueueNext(&(sxact->predicateLocks),
					 &(sxact->predicateLocks),
					 offsetof(PREDICATELOCK, xactLink));

	while (predlock != NULL)
	{
		record.type = TWOPHASEPREDICATERECORD_LOCK;
		lockRecord->target = predlock->tag.myTarget->tag;

		RegisterTwoPhaseRecord(TWOPHASE_RM_PREDICATELOCK_ID, 0,
							   &record, sizeof(record));

		predlock = (PREDICATELOCK *)
			SHMQueueNext(&(sxact->predicateLocks),
						 &(predlock->xactLink),
						 offsetof(PREDICATELOCK, xactLink));
	}

	LWLockRelease(SerializablePredicateLockListLock);
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
	ReleasePredicateLocks(isCommit);
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

		/* vxid for a prepared xact is InvalidBackendId/xid; no pid */
		sxact->vxid.backendId = InvalidBackendId;
		sxact->vxid.localTransactionId = (LocalTransactionId) xid;
		sxact->pid = 0;

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
		SHMQueueInit(&(sxact->possibleUnsafeConflicts));

		SHMQueueInit(&(sxact->predicateLocks));
		SHMQueueElemInit(&(sxact->finishedLink));

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
		SHMQueueInit(&(sxact->outConflicts));
		SHMQueueInit(&(sxact->inConflicts));
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
			OldSerXidSetActiveSerXmin(sxact->xmin);
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
