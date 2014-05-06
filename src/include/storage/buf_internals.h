/*-------------------------------------------------------------------------
 *
 * buf_internals.h
 *	  Internal definitions for buffer manager and the buffer replacement
 *	  strategy.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/buf_internals.h,v 1.104 2010/01/02 16:58:08 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_INTERNALS_H
#define BUFMGR_INTERNALS_H

#include "storage/buf.h"
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
	((LWLockId) (FirstBufMappingLock + BufTableHashPartition(hashcode)))

/*
 *	BufferDesc -- shared descriptor/state data for a single shared buffer.
 *
 * Note: buf_hdr_lock must be held to examine or change the tag, flags,
 * usage_count, refcount, or wait_backend_pid fields.  buf_id field never
 * changes after initialization, so does not need locking.  freeNext is
 * protected by the BufFreelistLock not buf_hdr_lock.  The LWLocks can take
 * care of themselves.  The buf_hdr_lock is *not* used to control access to
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
 */
typedef struct sbufdesc
{
	BufferTag	tag;			/* ID of page contained in buffer */
	BufFlags	flags;			/* see bit definitions above */
	uint16		usage_count;	/* usage counter for clock sweep code */
	unsigned	refcount;		/* # of backends holding pins on buffer */
	int			wait_backend_pid;		/* backend PID of pin-count waiter */

	slock_t		buf_hdr_lock;	/* protects the above fields */

	int			buf_id;			/* buffer's index number (from 0) */
	int			freeNext;		/* link in freelist chain */

	LWLockId	io_in_progress_lock;	/* to wait for I/O to complete */
	LWLockId	content_lock;	/* to lock access to buffer contents */
} BufferDesc;

#define BufferDescriptorGetBuffer(bdesc) ((bdesc)->buf_id + 1)

/*
 * The freeNext field is either the index of the next freelist entry,
 * or one of these special values:
 */
#define FREENEXT_END_OF_LIST	(-1)
#define FREENEXT_NOT_IN_LIST	(-2)

/*
 * Macros for acquiring/releasing a shared buffer header's spinlock.
 * Do not apply these to local buffers!
 *
 * Note: as a general coding rule, if you are using these then you probably
 * need to be using a volatile-qualified pointer to the buffer header, to
 * ensure that the compiler doesn't rearrange accesses to the header to
 * occur before or after the spinlock is acquired/released.
 */
#define LockBufHdr(bufHdr)		SpinLockAcquire(&(bufHdr)->buf_hdr_lock)
#define UnlockBufHdr(bufHdr)	SpinLockRelease(&(bufHdr)->buf_hdr_lock)


/* in buf_init.c */
extern PGDLLIMPORT BufferDesc *BufferDescriptors;

/* in localbuf.c */
extern BufferDesc *LocalBufferDescriptors;


/*
 * Internal routines: only called by bufmgr
 */

/* freelist.c */
extern volatile BufferDesc *StrategyGetBuffer(BufferAccessStrategy strategy,
				  bool *lock_held);
extern void StrategyFreeBuffer(volatile BufferDesc *buf);
extern bool StrategyRejectBuffer(BufferAccessStrategy strategy,
					 volatile BufferDesc *buf);

extern int	StrategySyncStart(uint32 *complete_passes, uint32 *num_buf_alloc);
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
extern void AtEOXact_LocalBuffers(bool isCommit);

#endif   /* BUFMGR_INTERNALS_H */
