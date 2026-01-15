/*-------------------------------------------------------------------------
 *
 * bufmgr.h
 *	  POSTGRES buffer manager definitions.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/bufmgr.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUFMGR_H
#define BUFMGR_H

#include "port/pg_iovec.h"
#include "storage/aio_types.h"
#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"

typedef void *Block;

/*
 * Possible arguments for GetAccessStrategy().
 *
 * If adding a new BufferAccessStrategyType, also add a new IOContext so
 * IO statistics using this strategy are tracked.
 */
typedef enum BufferAccessStrategyType
{
	BAS_NORMAL,					/* Normal random access */
	BAS_BULKREAD,				/* Large read-only scan (hint bit updates are
								 * ok) */
	BAS_BULKWRITE,				/* Large multi-block write (e.g. COPY IN) */
	BAS_VACUUM,					/* VACUUM */
} BufferAccessStrategyType;

/* Possible modes for ReadBufferExtended() */
typedef enum
{
	RBM_NORMAL,					/* Normal read */
	RBM_ZERO_AND_LOCK,			/* Don't read from disk, caller will
								 * initialize. Also locks the page. */
	RBM_ZERO_AND_CLEANUP_LOCK,	/* Like RBM_ZERO_AND_LOCK, but locks the page
								 * in "cleanup" mode */
	RBM_ZERO_ON_ERROR,			/* Read, but return an all-zeros page on error */
	RBM_NORMAL_NO_LOG,			/* Don't log page as invalid during WAL
								 * replay; otherwise same as RBM_NORMAL */
} ReadBufferMode;

/*
 * Type returned by PrefetchBuffer().
 */
typedef struct PrefetchBufferResult
{
	Buffer		recent_buffer;	/* If valid, a hit (recheck needed!) */
	bool		initiated_io;	/* If true, a miss resulting in async I/O */
} PrefetchBufferResult;

/*
 * Flags influencing the behaviour of ExtendBufferedRel*
 */
typedef enum ExtendBufferedFlags
{
	/*
	 * Don't acquire extension lock. This is safe only if the relation isn't
	 * shared, an access exclusive lock is held or if this is the startup
	 * process.
	 */
	EB_SKIP_EXTENSION_LOCK = (1 << 0),

	/* Is this extension part of recovery? */
	EB_PERFORMING_RECOVERY = (1 << 1),

	/*
	 * Should the fork be created if it does not currently exist? This likely
	 * only ever makes sense for relation forks.
	 */
	EB_CREATE_FORK_IF_NEEDED = (1 << 2),

	/* Should the first (possibly only) return buffer be returned locked? */
	EB_LOCK_FIRST = (1 << 3),

	/* Should the smgr size cache be cleared? */
	EB_CLEAR_SIZE_CACHE = (1 << 4),

	/* internal flags follow */
	EB_LOCK_TARGET = (1 << 5),
}			ExtendBufferedFlags;

/* forward declared, to avoid including smgr.h here */
typedef struct SMgrRelationData *SMgrRelation;

/*
 * Some functions identify relations either by relation or smgr +
 * relpersistence, initialized via the BMR_REL()/BMR_SMGR() macros below.
 * This allows us to use the same function for both recovery and normal
 * operation.  When BMR_REL is used, it's not valid to cache its rd_smgr here,
 * because our pointer would be obsolete in case of relcache invalidation.
 * For simplicity, use BMR_GET_SMGR to read the smgr.
 */
typedef struct BufferManagerRelation
{
	Relation	rel;
	SMgrRelation smgr;
	char		relpersistence;
} BufferManagerRelation;

#define BMR_REL(p_rel) \
	((BufferManagerRelation){.rel = p_rel})
#define BMR_SMGR(p_smgr, p_relpersistence) \
	((BufferManagerRelation){.smgr = p_smgr, .relpersistence = p_relpersistence})
#define BMR_GET_SMGR(bmr) \
	(RelationIsValid((bmr).rel) ? RelationGetSmgr((bmr).rel) : (bmr).smgr)

/* Zero out page if reading fails. */
#define READ_BUFFERS_ZERO_ON_ERROR (1 << 0)
/* Call smgrprefetch() if I/O necessary. */
#define READ_BUFFERS_ISSUE_ADVICE (1 << 1)
/* Don't treat page as invalid due to checksum failures. */
#define READ_BUFFERS_IGNORE_CHECKSUM_FAILURES (1 << 2)
/* IO will immediately be waited for */
#define READ_BUFFERS_SYNCHRONOUSLY (1 << 3)


struct ReadBuffersOperation
{
	/* The following members should be set by the caller. */
	Relation	rel;			/* optional */
	SMgrRelation smgr;
	char		persistence;
	ForkNumber	forknum;
	BufferAccessStrategy strategy;

	/*
	 * The following private members are private state for communication
	 * between StartReadBuffers() and WaitReadBuffers(), initialized only if
	 * an actual read is required, and should not be modified.
	 */
	Buffer	   *buffers;
	BlockNumber blocknum;
	int			flags;
	int16		nblocks;
	int16		nblocks_done;
	PgAioWaitRef io_wref;
	PgAioReturn io_return;
};

typedef struct ReadBuffersOperation ReadBuffersOperation;

/* to avoid having to expose buf_internals.h here */
typedef struct WritebackContext WritebackContext;

/* in globals.c ... this duplicates miscadmin.h */
extern PGDLLIMPORT int NBuffers;

/* in bufmgr.c */
extern PGDLLIMPORT bool zero_damaged_pages;
extern PGDLLIMPORT int bgwriter_lru_maxpages;
extern PGDLLIMPORT double bgwriter_lru_multiplier;
extern PGDLLIMPORT bool track_io_timing;

#define DEFAULT_EFFECTIVE_IO_CONCURRENCY 16
#define DEFAULT_MAINTENANCE_IO_CONCURRENCY 16
extern PGDLLIMPORT int effective_io_concurrency;
extern PGDLLIMPORT int maintenance_io_concurrency;

#define MAX_IO_COMBINE_LIMIT PG_IOV_MAX
#define DEFAULT_IO_COMBINE_LIMIT Min(MAX_IO_COMBINE_LIMIT, (128 * 1024) / BLCKSZ)
extern PGDLLIMPORT int io_combine_limit;	/* min of the two GUCs below */
extern PGDLLIMPORT int io_combine_limit_guc;
extern PGDLLIMPORT int io_max_combine_limit;

extern PGDLLIMPORT int checkpoint_flush_after;
extern PGDLLIMPORT int backend_flush_after;
extern PGDLLIMPORT int bgwriter_flush_after;

extern PGDLLIMPORT const PgAioHandleCallbacks aio_shared_buffer_readv_cb;
extern PGDLLIMPORT const PgAioHandleCallbacks aio_local_buffer_readv_cb;

/* in buf_init.c */
extern PGDLLIMPORT char *BufferBlocks;

/* in localbuf.c */
extern PGDLLIMPORT int NLocBuffer;
extern PGDLLIMPORT Block *LocalBufferBlockPointers;
extern PGDLLIMPORT int32 *LocalRefCount;

/* upper limit for effective_io_concurrency */
#define MAX_IO_CONCURRENCY 1000

/* special block number for ReadBuffer() */
#define P_NEW	InvalidBlockNumber	/* grow the file to get a new page */

/*
 * Buffer content lock modes (mode argument for LockBuffer())
 */
typedef enum BufferLockMode
{
	BUFFER_LOCK_UNLOCK,

	/*
	 * A share lock conflicts with exclusive locks.
	 */
	BUFFER_LOCK_SHARE,

	/*
	 * A share-exclusive lock conflicts with itself and exclusive locks.
	 */
	BUFFER_LOCK_SHARE_EXCLUSIVE,

	/*
	 * An exclusive lock conflicts with every other lock type.
	 */
	BUFFER_LOCK_EXCLUSIVE,
} BufferLockMode;


/*
 * prototypes for functions in bufmgr.c
 */
extern PrefetchBufferResult PrefetchSharedBuffer(SMgrRelation smgr_reln,
												 ForkNumber forkNum,
												 BlockNumber blockNum);
extern PrefetchBufferResult PrefetchBuffer(Relation reln, ForkNumber forkNum,
										   BlockNumber blockNum);
extern bool ReadRecentBuffer(RelFileLocator rlocator, ForkNumber forkNum,
							 BlockNumber blockNum, Buffer recent_buffer);
extern Buffer ReadBuffer(Relation reln, BlockNumber blockNum);
extern Buffer ReadBufferExtended(Relation reln, ForkNumber forkNum,
								 BlockNumber blockNum, ReadBufferMode mode,
								 BufferAccessStrategy strategy);
extern Buffer ReadBufferWithoutRelcache(RelFileLocator rlocator,
										ForkNumber forkNum, BlockNumber blockNum,
										ReadBufferMode mode, BufferAccessStrategy strategy,
										bool permanent);

extern bool StartReadBuffer(ReadBuffersOperation *operation,
							Buffer *buffer,
							BlockNumber blocknum,
							int flags);
extern bool StartReadBuffers(ReadBuffersOperation *operation,
							 Buffer *buffers,
							 BlockNumber blockNum,
							 int *nblocks,
							 int flags);
extern void WaitReadBuffers(ReadBuffersOperation *operation);

extern void ReleaseBuffer(Buffer buffer);
extern void UnlockReleaseBuffer(Buffer buffer);
extern bool BufferIsLockedByMe(Buffer buffer);
extern bool BufferIsLockedByMeInMode(Buffer buffer, BufferLockMode mode);
extern bool BufferIsDirty(Buffer buffer);
extern void MarkBufferDirty(Buffer buffer);
extern void IncrBufferRefCount(Buffer buffer);
extern void CheckBufferIsPinnedOnce(Buffer buffer);
extern Buffer ReleaseAndReadBuffer(Buffer buffer, Relation relation,
								   BlockNumber blockNum);

extern Buffer ExtendBufferedRel(BufferManagerRelation bmr,
								ForkNumber forkNum,
								BufferAccessStrategy strategy,
								uint32 flags);
extern BlockNumber ExtendBufferedRelBy(BufferManagerRelation bmr,
									   ForkNumber fork,
									   BufferAccessStrategy strategy,
									   uint32 flags,
									   uint32 extend_by,
									   Buffer *buffers,
									   uint32 *extended_by);
extern Buffer ExtendBufferedRelTo(BufferManagerRelation bmr,
								  ForkNumber fork,
								  BufferAccessStrategy strategy,
								  uint32 flags,
								  BlockNumber extend_to,
								  ReadBufferMode mode);

extern void InitBufferManagerAccess(void);
extern void AtEOXact_Buffers(bool isCommit);
#ifdef USE_ASSERT_CHECKING
extern void AssertBufferLocksPermitCatalogRead(void);
#endif
extern char *DebugPrintBufferRefcount(Buffer buffer);
extern void CheckPointBuffers(int flags);
extern BlockNumber BufferGetBlockNumber(Buffer buffer);
extern BlockNumber RelationGetNumberOfBlocksInFork(Relation relation,
												   ForkNumber forkNum);
extern void FlushOneBuffer(Buffer buffer);
extern void FlushRelationBuffers(Relation rel);
extern void FlushRelationsAllBuffers(SMgrRelation *smgrs, int nrels);
extern void CreateAndCopyRelationData(RelFileLocator src_rlocator,
									  RelFileLocator dst_rlocator,
									  bool permanent);
extern void FlushDatabaseBuffers(Oid dbid);
extern void DropRelationBuffers(SMgrRelation smgr_reln,
								ForkNumber *forkNum,
								int nforks, BlockNumber *firstDelBlock);
extern void DropRelationsAllBuffers(SMgrRelation *smgr_reln,
									int nlocators);
extern void DropDatabaseBuffers(Oid dbid);

#define RelationGetNumberOfBlocks(reln) \
	RelationGetNumberOfBlocksInFork(reln, MAIN_FORKNUM)

extern bool BufferIsPermanent(Buffer buffer);
extern XLogRecPtr BufferGetLSNAtomic(Buffer buffer);
extern void BufferGetTag(Buffer buffer, RelFileLocator *rlocator,
						 ForkNumber *forknum, BlockNumber *blknum);

extern void MarkBufferDirtyHint(Buffer buffer, bool buffer_std);

extern void UnlockBuffers(void);
extern void UnlockBuffer(Buffer buffer);
extern void LockBufferInternal(Buffer buffer, BufferLockMode mode);

/*
 * Handling BUFFER_LOCK_UNLOCK in bufmgr.c leads to sufficiently worse branch
 * prediction to impact performance. Therefore handle that switch here, where
 * most of the time `mode` will be a constant and thus can be optimized out by
 * the compiler.
 */
static inline void
LockBuffer(Buffer buffer, BufferLockMode mode)
{
	if (mode == BUFFER_LOCK_UNLOCK)
		UnlockBuffer(buffer);
	else
		LockBufferInternal(buffer, mode);
}

extern bool ConditionalLockBuffer(Buffer buffer);
extern void LockBufferForCleanup(Buffer buffer);
extern bool ConditionalLockBufferForCleanup(Buffer buffer);
extern bool IsBufferCleanupOK(Buffer buffer);
extern bool HoldingBufferPinThatDelaysRecovery(void);

extern bool BgBufferSync(WritebackContext *wb_context);

extern uint32 GetPinLimit(void);
extern uint32 GetLocalPinLimit(void);
extern uint32 GetAdditionalPinLimit(void);
extern uint32 GetAdditionalLocalPinLimit(void);
extern void LimitAdditionalPins(uint32 *additional_pins);
extern void LimitAdditionalLocalPins(uint32 *additional_pins);

extern bool EvictUnpinnedBuffer(Buffer buf, bool *buffer_flushed);
extern void EvictAllUnpinnedBuffers(int32 *buffers_evicted,
									int32 *buffers_flushed,
									int32 *buffers_skipped);
extern void EvictRelUnpinnedBuffers(Relation rel,
									int32 *buffers_evicted,
									int32 *buffers_flushed,
									int32 *buffers_skipped);
extern bool MarkDirtyUnpinnedBuffer(Buffer buf, bool *buffer_already_dirty);
extern void MarkDirtyRelUnpinnedBuffers(Relation rel,
										int32 *buffers_dirtied,
										int32 *buffers_already_dirty,
										int32 *buffers_skipped);
extern void MarkDirtyAllUnpinnedBuffers(int32 *buffers_dirtied,
										int32 *buffers_already_dirty,
										int32 *buffers_skipped);

/* in buf_init.c */
extern void BufferManagerShmemInit(void);
extern Size BufferManagerShmemSize(void);

/* in localbuf.c */
extern void AtProcExit_LocalBuffers(void);

/* in freelist.c */

extern BufferAccessStrategy GetAccessStrategy(BufferAccessStrategyType btype);
extern BufferAccessStrategy GetAccessStrategyWithSize(BufferAccessStrategyType btype,
													  int ring_size_kb);
extern int	GetAccessStrategyBufferCount(BufferAccessStrategy strategy);
extern int	GetAccessStrategyPinLimit(BufferAccessStrategy strategy);

extern void FreeAccessStrategy(BufferAccessStrategy strategy);


/* inline functions */

/*
 * Although this header file is nominally backend-only, certain frontend
 * programs like pg_waldump include it.  For compilers that emit static
 * inline functions even when they're unused, that leads to unsatisfied
 * external references; hence hide these with #ifndef FRONTEND.
 */

#ifndef FRONTEND

/*
 * BufferIsValid
 *		True iff the given buffer number is valid (either as a shared
 *		or local buffer).
 *
 * Note: For a long time this was defined the same as BufferIsPinned,
 * that is it would say False if you didn't hold a pin on the buffer.
 * I believe this was bogus and served only to mask logic errors.
 * Code should always know whether it has a buffer reference,
 * independently of the pin state.
 *
 * Note: For a further long time this was not quite the inverse of the
 * BufferIsInvalid() macro, in that it also did sanity checks to verify
 * that the buffer number was in range.  Most likely, this macro was
 * originally intended only to be used in assertions, but its use has
 * since expanded quite a bit, and the overhead of making those checks
 * even in non-assert-enabled builds can be significant.  Thus, we've
 * now demoted the range checks to assertions within the macro itself.
 */
static inline bool
BufferIsValid(Buffer bufnum)
{
	Assert(bufnum <= NBuffers);
	Assert(bufnum >= -NLocBuffer);

	return bufnum != InvalidBuffer;
}

/*
 * BufferGetBlock
 *		Returns a reference to a disk page image associated with a buffer.
 *
 * Note:
 *		Assumes buffer is valid.
 */
static inline Block
BufferGetBlock(Buffer buffer)
{
	Assert(BufferIsValid(buffer));

	if (BufferIsLocal(buffer))
		return LocalBufferBlockPointers[-buffer - 1];
	else
		return (Block) (BufferBlocks + ((Size) (buffer - 1)) * BLCKSZ);
}

/*
 * BufferGetPageSize
 *		Returns the page size within a buffer.
 *
 * Notes:
 *		Assumes buffer is valid.
 *
 *		The buffer can be a raw disk block and need not contain a valid
 *		(formatted) disk page.
 */
/* XXX should dig out of buffer descriptor */
static inline Size
BufferGetPageSize(Buffer buffer)
{
	Assert(BufferIsValid(buffer));
	return (Size) BLCKSZ;
}

/*
 * BufferGetPage
 *		Returns the page associated with a buffer.
 */
static inline Page
BufferGetPage(Buffer buffer)
{
	return (Page) BufferGetBlock(buffer);
}

#endif							/* FRONTEND */

#endif							/* BUFMGR_H */
