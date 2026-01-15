/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  Internal definitions for buffer manager and the buffer replacement
 *	  strategy.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "pgstat.h"
#include "port/atomics.h"
#include "storage/aio_types.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/condition_variable.h"
#include "storage/lwlock.h"
#include "storage/procnumber.h"
#include "storage/proclist_types.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/relcache.h"
#include "utils/resowner.h"

/*
 * Buffer state is a single 64-bit variable where following data is combined.
 *
 * State of the buffer itself (in order):
 * - 18 bits refcount
 * - 4 bits usage count
 * - 12 bits of flags
 * - 18 bits share-lock count
 * - 1 bit share-exclusive locked
 * - 1 bit exclusive locked
 *
 * Combining these values allows to perform some operations without locking
 * the buffer header, by modifying them together with a CAS loop.
 *
 * The definition of buffer state components is below.
 */
#define BUF_REFCOUNT_BITS 18
#define BUF_USAGECOUNT_BITS 4
#define BUF_FLAG_BITS 12
#define BUF_LOCK_BITS (18+2)

StaticAssertDecl(BUF_REFCOUNT_BITS + BUF_USAGECOUNT_BITS + BUF_FLAG_BITS + BUF_LOCK_BITS <= 64,
				 "parts of buffer state space need to be <= 64");

/* refcount related definitions */
#define BUF_REFCOUNT_ONE 1
#define BUF_REFCOUNT_MASK \
	((UINT64CONST(1) << BUF_REFCOUNT_BITS) - 1)

/* usage count related definitions */
#define BUF_USAGECOUNT_SHIFT \
	BUF_REFCOUNT_BITS
#define BUF_USAGECOUNT_MASK \
	(((UINT64CONST(1) << BUF_USAGECOUNT_BITS) - 1) << (BUF_USAGECOUNT_SHIFT))
#define BUF_USAGECOUNT_ONE \
	(UINT64CONST(1) << BUF_REFCOUNT_BITS)

/* flags related definitions */
#define BUF_FLAG_SHIFT \
	(BUF_REFCOUNT_BITS + BUF_USAGECOUNT_BITS)
#define BUF_FLAG_MASK \
	(((UINT64CONST(1) << BUF_FLAG_BITS) - 1) << BUF_FLAG_SHIFT)

/* lock state related definitions */
#define BM_LOCK_SHIFT \
	(BUF_FLAG_SHIFT + BUF_FLAG_BITS)
#define BM_LOCK_VAL_SHARED \
	(UINT64CONST(1) << (BM_LOCK_SHIFT))
#define BM_LOCK_VAL_SHARE_EXCLUSIVE \
	(UINT64CONST(1) << (BM_LOCK_SHIFT + MAX_BACKENDS_BITS))
#define BM_LOCK_VAL_EXCLUSIVE \
	(UINT64CONST(1) << (BM_LOCK_SHIFT + MAX_BACKENDS_BITS + 1))
#define BM_LOCK_MASK \
	((((uint64) MAX_BACKENDS) << BM_LOCK_SHIFT) | BM_LOCK_VAL_SHARE_EXCLUSIVE | BM_LOCK_VAL_EXCLUSIVE)


/* Get refcount and usagecount from buffer state */
#define BUF_STATE_GET_REFCOUNT(state) \
	((uint32)((state) & BUF_REFCOUNT_MASK))
#define BUF_STATE_GET_USAGECOUNT(state) \
	((uint32)(((state) & BUF_USAGECOUNT_MASK) >> BUF_USAGECOUNT_SHIFT))

/*
 * Flags for buffer descriptors
 *
 * Note: BM_TAG_VALID essentially means that there is a buffer hashtable
 * entry associated with the buffer's tag.
 */

#define BUF_DEFINE_FLAG(flagno)	\
	(UINT64CONST(1) << (BUF_FLAG_SHIFT + (flagno)))

/* buffer header is locked */
#define BM_LOCKED					BUF_DEFINE_FLAG( 0)
/* data needs writing */
#define BM_DIRTY					BUF_DEFINE_FLAG( 1)
/* data is valid */
#define BM_VALID					BUF_DEFINE_FLAG( 2)
/* tag is assigned */
#define BM_TAG_VALID				BUF_DEFINE_FLAG( 3)
/* read or write in progress */
#define BM_IO_IN_PROGRESS			BUF_DEFINE_FLAG( 4)
/* previous I/O failed */
#define BM_IO_ERROR					BUF_DEFINE_FLAG( 5)
/* dirtied since write started */
#define BM_JUST_DIRTIED				BUF_DEFINE_FLAG( 6)
/* have waiter for sole pin */
#define BM_PIN_COUNT_WAITER			BUF_DEFINE_FLAG( 7)
/* must write for checkpoint */
#define BM_CHECKPOINT_NEEDED		BUF_DEFINE_FLAG( 8)
/* permanent buffer (not unlogged, or init fork) */
#define BM_PERMANENT				BUF_DEFINE_FLAG( 9)
/* content lock has waiters */
#define BM_LOCK_HAS_WAITERS			BUF_DEFINE_FLAG(10)
/* waiter for content lock has been signalled but not yet run */
#define BM_LOCK_WAKE_IN_PROGRESS	BUF_DEFINE_FLAG(11)


StaticAssertDecl(MAX_BACKENDS_BITS <= BUF_REFCOUNT_BITS,
				 "MAX_BACKENDS_BITS needs to be <= BUF_REFCOUNT_BITS");
StaticAssertDecl(MAX_BACKENDS_BITS <= (BUF_LOCK_BITS - 2),
				 "MAX_BACKENDS_BITS needs to be <= BUF_LOCK_BITS - 2");


/*
 * The maximum allowed value of usage_count represents a tradeoff between
 * accuracy and speed of the clock-sweep buffer management algorithm.  A
 * large value (comparable to NBuffers) would approximate LRU semantics.
 * But it can take as many as BM_MAX_USAGE_COUNT+1 complete cycles of the
 * clock-sweep hand to find a free buffer, so in practice we don't want the
 * value to be very large.
 */
#define BM_MAX_USAGE_COUNT	5

StaticAssertDecl(BM_MAX_USAGE_COUNT < (UINT64CONST(1) << BUF_USAGECOUNT_BITS),
				 "BM_MAX_USAGE_COUNT doesn't fit in BUF_USAGECOUNT_BITS bits");

/*
 * Buffer tag identifies which disk block the buffer contains.
 *
 * Note: the BufferTag data must be sufficient to determine where to write the
 * block, without reference to pg_class or pg_tablespace entries.  It's
 * possible that the backend flushing the buffer doesn't even believe the
 * relation is visible yet (its xact may have started before the xact that
 * created the rel).  The storage manager must be able to cope anyway.
 *
 * Note: if there's any pad bytes in the struct, InitBufferTag will have
 * to be fixed to zero them, since this struct is used as a hash key.
 */
typedef struct buftag
{
	Oid			spcOid;			/* tablespace oid */
	Oid			dbOid;			/* database oid */
	RelFileNumber relNumber;	/* relation file number */
	ForkNumber	forkNum;		/* fork number */
	BlockNumber blockNum;		/* blknum relative to begin of reln */
} BufferTag;

static inline RelFileNumber
BufTagGetRelNumber(const BufferTag *tag)
{
	return tag->relNumber;
}

static inline ForkNumber
BufTagGetForkNum(const BufferTag *tag)
{
	return tag->forkNum;
}

static inline void
BufTagSetRelForkDetails(BufferTag *tag, RelFileNumber relnumber,
						ForkNumber forknum)
{
	tag->relNumber = relnumber;
	tag->forkNum = forknum;
}

static inline RelFileLocator
BufTagGetRelFileLocator(const BufferTag *tag)
{
	RelFileLocator rlocator;

	rlocator.spcOid = tag->spcOid;
	rlocator.dbOid = tag->dbOid;
	rlocator.relNumber = BufTagGetRelNumber(tag);

	return rlocator;
}

static inline void
ClearBufferTag(BufferTag *tag)
{
	tag->spcOid = InvalidOid;
	tag->dbOid = InvalidOid;
	BufTagSetRelForkDetails(tag, InvalidRelFileNumber, InvalidForkNumber);
	tag->blockNum = InvalidBlockNumber;
}

static inline void
InitBufferTag(BufferTag *tag, const RelFileLocator *rlocator,
			  ForkNumber forkNum, BlockNumber blockNum)
{
	tag->spcOid = rlocator->spcOid;
	tag->dbOid = rlocator->dbOid;
	BufTagSetRelForkDetails(tag, rlocator->relNumber, forkNum);
	tag->blockNum = blockNum;
}

static inline bool
BufferTagsEqual(const BufferTag *tag1, const BufferTag *tag2)
{
	return (tag1->spcOid == tag2->spcOid) &&
		(tag1->dbOid == tag2->dbOid) &&
		(tag1->relNumber == tag2->relNumber) &&
		(tag1->blockNum == tag2->blockNum) &&
		(tag1->forkNum == tag2->forkNum);
}

static inline bool
BufTagMatchesRelFileLocator(const BufferTag *tag,
							const RelFileLocator *rlocator)
{
	return (tag->spcOid == rlocator->spcOid) &&
		(tag->dbOid == rlocator->dbOid) &&
		(BufTagGetRelNumber(tag) == rlocator->relNumber);
}


/*
 * The shared buffer mapping table is partitioned to reduce contention.
 * To determine which partition lock a given tag requires, compute the tag's
 * hash code with BufTableHashCode(), then apply BufMappingPartitionLock().
 * NB: NUM_BUFFER_PARTITIONS must be a power of 2!
 */
static inline uint32
BufTableHashPartition(uint32 hashcode)
{
	return hashcode % NUM_BUFFER_PARTITIONS;
}

static inline LWLock *
BufMappingPartitionLock(uint32 hashcode)
{
	return &MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET +
							BufTableHashPartition(hashcode)].lock;
}

static inline LWLock *
BufMappingPartitionLockByIndex(uint32 index)
{
	return &MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET + index].lock;
}

/*
 *	BufferDesc -- shared descriptor/state data for a single shared buffer.
 *
 * The state of the buffer is controlled by the, drumroll, state variable. It
 * only may be modified using atomic operations.  The state variable combines
 * various flags, the buffer's refcount and usage count. See comment above
 * BUF_REFCOUNT_BITS for details about the division.  This layout allow us to
 * do some operations in a single atomic operation, without actually acquiring
 * and releasing the spinlock; for instance, increasing or decreasing the
 * refcount.
 *
 * One of the aforementioned flags is BM_LOCKED, used to implement the buffer
 * header lock. See the following paragraphs, as well as the documentation for
 * individual fields, for more details.
 *
 * The identity of the buffer (BufferDesc.tag) can only be changed by the
 * backend holding the buffer header lock.
 *
 * If the lock is held by another backend, neither additional buffer pins may
 * be established (we would like to relax this eventually), nor can flags be
 * set/cleared. These operations either need to acquire the buffer header
 * spinlock, or need to use a CAS loop, waiting for the lock to be released if
 * it is held.  However, existing buffer pins may be released while the buffer
 * header spinlock is held, using an atomic subtraction.
 *
 * If we have the buffer pinned, its tag can't change underneath us, so we can
 * examine the tag without locking the buffer header.  Also, in places we do
 * one-time reads of the flags without bothering to lock the buffer header;
 * this is generally for situations where we don't expect the flag bit being
 * tested to be changing.
 *
 * We can't physically remove items from a disk page if another backend has
 * the buffer pinned.  Hence, a backend may need to wait for all other pins
 * to go away.  This is signaled by storing its own pgprocno into
 * wait_backend_pgprocno and setting flag bit BM_PIN_COUNT_WAITER.  At present,
 * there can be only one such waiter per buffer.
 *
 * The content of buffers is protected via the buffer content lock,
 * implemented as part of the buffer state. Note that the buffer header lock
 * is *not* used to control access to the data in the buffer! We used to use
 * an LWLock to implement the content lock, but having a dedicated
 * implementation of content locks allows us to implement some otherwise hard
 * things (e.g. race-freely checking if AIO is in progress before locking a
 * buffer exclusively) and enables otherwise impossible optimizations
 * (e.g. unlocking and unpinning a buffer in one atomic operation).
 *
 * We use this same struct for local buffer headers, but the locks are not
 * used and not all of the flag bits are useful either. To avoid unnecessary
 * overhead, manipulations of the state field should be done without actual
 * atomic operations (i.e. only pg_atomic_read_u64() and
 * pg_atomic_unlocked_write_u64()).
 *
 * Be careful to avoid increasing the size of the struct when adding or
 * reordering members.  Keeping it below 64 bytes (the most common CPU
 * cache line size) is fairly important for performance.
 *
 * Per-buffer I/O condition variables are currently kept outside this struct in
 * a separate array.  They could be moved in here and still fit within that
 * limit on common systems, but for now that is not done.
 */
typedef struct BufferDesc
{
	/*
	 * ID of page contained in buffer. The buffer header spinlock needs to be
	 * held to modify this field.
	 */
	BufferTag	tag;

	/*
	 * Buffer's index number (from 0). The field never changes after
	 * initialization, so does not need locking.
	 */
	int			buf_id;

	/*
	 * State of the buffer, containing flags, refcount and usagecount. See
	 * BUF_* and BM_* defines at the top of this file.
	 */
	pg_atomic_uint64 state;

	/*
	 * Backend of pin-count waiter. The buffer header spinlock needs to be
	 * held to modify this field.
	 */
	int			wait_backend_pgprocno;

	PgAioWaitRef io_wref;		/* set iff AIO is in progress */

	/*
	 * List of PGPROCs waiting for the buffer content lock. Protected by the
	 * buffer header spinlock.
	 */
	proclist_head lock_waiters;
} BufferDesc;

/*
 * Concurrent access to buffer headers has proven to be more efficient if
 * they're cache line aligned. So we force the start of the BufferDescriptors
 * array to be on a cache line boundary and force the elements to be cache
 * line sized.
 *
 * XXX: As this is primarily matters in highly concurrent workloads which
 * probably all are 64bit these days, and the space wastage would be a bit
 * more noticeable on 32bit systems, we don't force the stride to be cache
 * line sized on those. If somebody does actual performance testing, we can
 * reevaluate.
 *
 * Note that local buffer descriptors aren't forced to be aligned - as there's
 * no concurrent access to those it's unlikely to be beneficial.
 *
 * We use a 64-byte cache line size here, because that's the most common
 * size. Making it bigger would be a waste of memory. Even if running on a
 * platform with either 32 or 128 byte line sizes, it's good to align to
 * boundaries and avoid false sharing.
 */
#define BUFFERDESC_PAD_TO_SIZE	(SIZEOF_VOID_P == 8 ? 64 : 1)

typedef union BufferDescPadded
{
	BufferDesc	bufferdesc;
	char		pad[BUFFERDESC_PAD_TO_SIZE];
} BufferDescPadded;

/*
 * The PendingWriteback & WritebackContext structure are used to keep
 * information about pending flush requests to be issued to the OS.
 */
typedef struct PendingWriteback
{
	/* could store different types of pending flushes here */
	BufferTag	tag;
} PendingWriteback;

/* struct forward declared in bufmgr.h */
typedef struct WritebackContext
{
	/* pointer to the max number of writeback requests to coalesce */
	int		   *max_pending;

	/* current number of pending writeback requests */
	int			nr_pending;

	/* pending requests */
	PendingWriteback pending_writebacks[WRITEBACK_MAX_PENDING_FLUSHES];
} WritebackContext;

/* in buf_init.c */
extern PGDLLIMPORT BufferDescPadded *BufferDescriptors;
extern PGDLLIMPORT ConditionVariableMinimallyPadded *BufferIOCVArray;
extern PGDLLIMPORT WritebackContext BackendWritebackContext;

/* in localbuf.c */
extern PGDLLIMPORT BufferDesc *LocalBufferDescriptors;


static inline BufferDesc *
GetBufferDescriptor(uint32 id)
{
	return &(BufferDescriptors[id]).bufferdesc;
}

static inline BufferDesc *
GetLocalBufferDescriptor(uint32 id)
{
	return &LocalBufferDescriptors[id];
}

static inline Buffer
BufferDescriptorGetBuffer(const BufferDesc *bdesc)
{
	return (Buffer) (bdesc->buf_id + 1);
}

static inline ConditionVariable *
BufferDescriptorGetIOCV(const BufferDesc *bdesc)
{
	return &(BufferIOCVArray[bdesc->buf_id]).cv;
}

/*
 * Functions for acquiring/releasing a shared buffer header's spinlock.  Do
 * not apply these to local buffers!
 */
extern uint64 LockBufHdr(BufferDesc *desc);

/*
 * Unlock the buffer header.
 *
 * This can only be used if the caller did not modify BufferDesc.state. To
 * set/unset flag bits or change the refcount use UnlockBufHdrExt().
 */
static inline void
UnlockBufHdr(BufferDesc *desc)
{
	Assert(pg_atomic_read_u64(&desc->state) & BM_LOCKED);

	pg_atomic_fetch_sub_u64(&desc->state, BM_LOCKED);
}

/*
 * Unlock the buffer header, while atomically adding the flags in set_bits,
 * unsetting the ones in unset_bits and changing the refcount by
 * refcount_change.
 *
 * Note that this approach would not work for usagecount, since we need to cap
 * the usagecount at BM_MAX_USAGE_COUNT.
 */
static inline uint64
UnlockBufHdrExt(BufferDesc *desc, uint64 old_buf_state,
				uint64 set_bits, uint64 unset_bits,
				int refcount_change)
{
	for (;;)
	{
		uint64		buf_state = old_buf_state;

		Assert(buf_state & BM_LOCKED);

		buf_state |= set_bits;
		buf_state &= ~unset_bits;
		buf_state &= ~BM_LOCKED;

		if (refcount_change != 0)
			buf_state += BUF_REFCOUNT_ONE * refcount_change;

		if (pg_atomic_compare_exchange_u64(&desc->state, &old_buf_state,
										   buf_state))
		{
			return old_buf_state;
		}
	}
}

extern uint64 WaitBufHdrUnlocked(BufferDesc *buf);

/* in bufmgr.c */

/*
 * Structure to sort buffers per file on checkpoints.
 *
 * This structure is allocated per buffer in shared memory, so it should be
 * kept as small as possible.
 */
typedef struct CkptSortItem
{
	Oid			tsId;
	RelFileNumber relNumber;
	ForkNumber	forkNum;
	BlockNumber blockNum;
	int			buf_id;
} CkptSortItem;

extern PGDLLIMPORT CkptSortItem *CkptBufferIds;

/* ResourceOwner callbacks to hold buffer I/Os and pins */
extern PGDLLIMPORT const ResourceOwnerDesc buffer_io_resowner_desc;
extern PGDLLIMPORT const ResourceOwnerDesc buffer_resowner_desc;

/* Convenience wrappers over ResourceOwnerRemember/Forget */
static inline void
ResourceOwnerRememberBuffer(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerRemember(owner, Int32GetDatum(buffer), &buffer_resowner_desc);
}
static inline void
ResourceOwnerForgetBuffer(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerForget(owner, Int32GetDatum(buffer), &buffer_resowner_desc);
}
static inline void
ResourceOwnerRememberBufferIO(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerRemember(owner, Int32GetDatum(buffer), &buffer_io_resowner_desc);
}
static inline void
ResourceOwnerForgetBufferIO(ResourceOwner owner, Buffer buffer)
{
	ResourceOwnerForget(owner, Int32GetDatum(buffer), &buffer_io_resowner_desc);
}

/*
 * Internal buffer management routines
 */
/* bufmgr.c */
extern void WritebackContextInit(WritebackContext *context, int *max_pending);
extern void IssuePendingWritebacks(WritebackContext *wb_context, IOContext io_context);
extern void ScheduleBufferTagForWriteback(WritebackContext *wb_context,
										  IOContext io_context, BufferTag *tag);

extern void TrackNewBufferPin(Buffer buf);

/* solely to make it easier to write tests */
extern bool StartBufferIO(BufferDesc *buf, bool forInput, bool nowait);
extern void TerminateBufferIO(BufferDesc *buf, bool clear_dirty, uint64 set_flag_bits,
							  bool forget_owner, bool release_aio);


/* freelist.c */
extern IOContext IOContextForStrategy(BufferAccessStrategy strategy);
extern BufferDesc *StrategyGetBuffer(BufferAccessStrategy strategy,
									 uint64 *buf_state, bool *from_ring);
extern bool StrategyRejectBuffer(BufferAccessStrategy strategy,
								 BufferDesc *buf, bool from_ring);

extern int	StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc);
extern void StrategyNotifyBgWriter(int bgwprocno);

extern Size StrategyShmemSize(void);
extern void StrategyInitialize(bool init);

/* buf_table.c */
extern Size BufTableShmemSize(int size);
extern void InitBufTable(int size);
extern uint32 BufTableHashCode(BufferTag *tagPtr);
extern int	BufTableLookup(BufferTag *tagPtr, uint32 hashcode);
extern int	BufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id);
extern void BufTableDelete(BufferTag *tagPtr, uint32 hashcode);

/* localbuf.c */
extern bool PinLocalBuffer(BufferDesc *buf_hdr, bool adjust_usagecount);
extern void UnpinLocalBuffer(Buffer buffer);
extern void UnpinLocalBufferNoOwner(Buffer buffer);
extern PrefetchBufferResult PrefetchLocalBuffer(SMgrRelation smgr,
												ForkNumber forkNum,
												BlockNumber blockNum);
extern BufferDesc *LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum,
									BlockNumber blockNum, bool *foundPtr);
extern BlockNumber ExtendBufferedRelLocal(BufferManagerRelation bmr,
										  ForkNumber fork,
										  uint32 flags,
										  uint32 extend_by,
										  BlockNumber extend_upto,
										  Buffer *buffers,
										  uint32 *extended_by);
extern void MarkLocalBufferDirty(Buffer buffer);
extern void TerminateLocalBufferIO(BufferDesc *bufHdr, bool clear_dirty,
								   uint64 set_flag_bits, bool release_aio);
extern bool StartLocalBufferIO(BufferDesc *bufHdr, bool forInput, bool nowait);
extern void FlushLocalBuffer(BufferDesc *bufHdr, SMgrRelation reln);
extern void InvalidateLocalBuffer(BufferDesc *bufHdr, bool check_unreferenced);
extern void DropRelationLocalBuffers(RelFileLocator rlocator,
									 ForkNumber *forkNum, int nforks,
									 BlockNumber *firstDelBlock);
extern void DropRelationAllLocalBuffers(RelFileLocator rlocator);
extern void AtEOXact_LocalBuffers(bool isCommit);

#endif							/* BUFMGR_INTERNALS_H */
