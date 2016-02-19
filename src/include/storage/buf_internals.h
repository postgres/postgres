/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  Internal definitions for buffer manager and the buffer replacement
 *	  strategy.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf_internals.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/spin.h"
#include "utils/relcache.h"


/*
 * Flags for buffer descriptors
 *
 * Note: TAG_VALID essentially means that there is a buffer hashtable
 * entry associated with the buffer's tag.
 */
#define BM_DIRTY				(1 << 0)		/* data needs writing */
#define BM_VALID				(1 << 1)		/* data is valid */
#define BM_TAG_VALID			(1 << 2)		/* tag is assigned */
#define BM_IO_IN_PROGRESS		(1 << 3)		/* read or write in progress */
#define BM_IO_ERROR				(1 << 4)		/* previous I/O failed */
#define BM_JUST_DIRTIED			(1 << 5)		/* dirtied since write started */
#define BM_PIN_COUNT_WAITER		(1 << 6)		/* have waiter for sole pin */
#define BM_CHECKPOINT_NEEDED	(1 << 7)		/* must write for checkpoint */
#define BM_PERMANENT			(1 << 8)		/* permanent relation (not
												 * unlogged) */

typedef bits16 BufFlags;

/*
 * The maximum allowed value of usage_count represents a tradeoff between
 * accuracy and speed of the clock-sweep buffer management algorithm.  A
 * large value (comparable to NBuffers) would approximate LRU semantics.
 * But it can take as many as BM_MAX_USAGE_COUNT+1 complete cycles of
 * clock sweeps to find a free buffer, so in practice we don't want the
 * value to be very large.
 */
#define BM_MAX_USAGE_COUNT	5

/*
 * Buffer tag identifies which disk block the buffer contains.
 *
 * Note: the BufferTag data must be sufficient to determine where to write the
 * block, without reference to pg_class or pg_tablespace entries.  It's
 * possible that the backend flushing the buffer doesn't even believe the
 * relation is visible yet (its xact may have started before the xact that
 * created the rel).  The storage manager must be able to cope anyway.
 *
 * Note: if there's any pad bytes in the struct, INIT_BUFFERTAG will have
 * to be fixed to zero them, since this struct is used as a hash key.
 */
typedef struct buftag
{
	RelFileNode rnode;			/* physical relation identifier */
	ForkNumber	forkNum;
	BlockNumber blockNum;		/* blknum relative to begin of reln */
} BufferTag;

#define CLEAR_BUFFERTAG(a) \
( \
	(a).rnode.spcNode = InvalidOid, \
	(a).rnode.dbNode = InvalidOid, \
	(a).rnode.relNode = InvalidOid, \
	(a).forkNum = InvalidForkNumber, \
	(a).blockNum = InvalidBlockNumber \
)

#define INIT_BUFFERTAG(a,xx_rnode,xx_forkNum,xx_blockNum) \
( \
	(a).rnode = (xx_rnode), \
	(a).forkNum = (xx_forkNum), \
	(a).blockNum = (xx_blockNum) \
)

#define BUFFERTAGS_EQUAL(a,b) \
( \
	RelFileNodeEquals((a).rnode, (b).rnode) && \
	(a).blockNum == (b).blockNum && \
	(a).forkNum == (b).forkNum \
)

/*
 * The shared buffer mapping table is partitioned to reduce contention.
 * To determine which partition lock a given tag requires, compute the tag's
 * hash code with BufTableHashCode(), then apply BufMappingPartitionLock().
 * NB: NUM_BUFFER_PARTITIONS must be a power of 2!
 */
#define BufTableHashPartition(hashcode) \
	((hashcode) % NUM_BUFFER_PARTITIONS)
#define BufMappingPartitionLock(hashcode) \
	(&MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET + \
		BufTableHashPartition(hashcode)].lock)
#define BufMappingPartitionLockByIndex(i) \
	(&MainLWLockArray[BUFFER_MAPPING_LWLOCK_OFFSET + (i)].lock)

/*
 *	BufferDesc -- shared descriptor/state data for a single shared buffer.
 *
 * Note: buf_hdr_lock must be held to examine or change the tag, flags,
 * usage_count, refcount, or wait_backend_pid fields.  buf_id field never
 * changes after initialization, so does not need locking.  freeNext is
 * protected by the buffer_strategy_lock not buf_hdr_lock.  The LWLock can
 * take care of itself.  The buf_hdr_lock is *not* used to control access to
 * the data in the buffer!
 *
 * An exception is that if we have the buffer pinned, its tag can't change
 * underneath us, so we can examine the tag without locking the spinlock.
 * Also, in places we do one-time reads of the flags without bothering to
 * lock the spinlock; this is generally for situations where we don't expect
 * the flag bit being tested to be changing.
 *
 * We can't physically remove items from a disk page if another backend has
 * the buffer pinned.  Hence, a backend may need to wait for all other pins
 * to go away.  This is signaled by storing its own PID into
 * wait_backend_pid and setting flag bit BM_PIN_COUNT_WAITER.  At present,
 * there can be only one such waiter per buffer.
 *
 * We use this same struct for local buffer headers, but the lock fields
 * are not used and not all of the flag bits are useful either.
 *
 * Be careful to avoid increasing the size of the struct when adding or
 * reordering members.  Keeping it below 64 bytes (the most common CPU
 * cache line size) is fairly important for performance.
 */
typedef struct BufferDesc
{
	BufferTag	tag;			/* ID of page contained in buffer */
	BufFlags	flags;			/* see bit definitions above */
	uint8		usage_count;	/* usage counter for clock sweep code */
	slock_t		buf_hdr_lock;	/* protects a subset of fields, see above */
	unsigned	refcount;		/* # of backends holding pins on buffer */
	int			wait_backend_pid;		/* backend PID of pin-count waiter */

	int			buf_id;			/* buffer's index number (from 0) */
	int			freeNext;		/* link in freelist chain */

	LWLock		content_lock;	/* to lock access to buffer contents */
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
 * We use 64bit as the cache line size here, because that's the most common
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

#define GetBufferDescriptor(id) (&BufferDescriptors[(id)].bufferdesc)
#define GetLocalBufferDescriptor(id) (&LocalBufferDescriptors[(id)])

#define BufferDescriptorGetBuffer(bdesc) ((bdesc)->buf_id + 1)

#define BufferDescriptorGetIOLock(bdesc) \
	(&(BufferIOLWLockArray[(bdesc)->buf_id]).lock)
#define BufferDescriptorGetContentLock(bdesc) \
	((LWLock*) (&(bdesc)->content_lock))

extern PGDLLIMPORT LWLockMinimallyPadded *BufferIOLWLockArray;

/*
 * The freeNext field is either the index of the next freelist entry,
 * or one of these special values:
 */
#define FREENEXT_END_OF_LIST	(-1)
#define FREENEXT_NOT_IN_LIST	(-2)

/*
 * Macros for acquiring/releasing a shared buffer header's spinlock.
 * Do not apply these to local buffers!
 */
#define LockBufHdr(bufHdr)		SpinLockAcquire(&(bufHdr)->buf_hdr_lock)
#define UnlockBufHdr(bufHdr)	SpinLockRelease(&(bufHdr)->buf_hdr_lock)


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
extern PGDLLIMPORT WritebackContext BackendWritebackContext;

/* in localbuf.c */
extern BufferDesc *LocalBufferDescriptors;

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
	Oid			relNode;
	ForkNumber	forkNum;
	BlockNumber blockNum;
	int			buf_id;
} CkptSortItem;

extern CkptSortItem *CkptBufferIds;

/*
 * Internal buffer management routines
 */
/* bufmgr.c */
extern void WritebackContextInit(WritebackContext *context, int *max_coalesce);
extern void IssuePendingWritebacks(WritebackContext *context);
extern void ScheduleBufferTagForWriteback(WritebackContext *context, BufferTag *tag);

/* freelist.c */
extern BufferDesc *StrategyGetBuffer(BufferAccessStrategy strategy);
extern void StrategyFreeBuffer(BufferDesc *buf);
extern bool StrategyRejectBuffer(BufferAccessStrategy strategy,
					 BufferDesc *buf);

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
extern void LocalPrefetchBuffer(SMgrRelation smgr, ForkNumber forkNum,
					BlockNumber blockNum);
extern BufferDesc *LocalBufferAlloc(SMgrRelation smgr, ForkNumber forkNum,
				 BlockNumber blockNum, bool *foundPtr);
extern void MarkLocalBufferDirty(Buffer buffer);
extern void DropRelFileNodeLocalBuffers(RelFileNode rnode, ForkNumber forkNum,
							BlockNumber firstDelBlock);
extern void DropRelFileNodeAllLocalBuffers(RelFileNode rnode);
extern void AtEOXact_LocalBuffers(bool isCommit);

#endif   /* BUFMGR_INTERNALS_H */
