/*-------------------------------------------------------------------------
 *
 * xlogprefetcher.c
 *		Prefetching support for recovery.
 *
 * Portions Copyright (c) 2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		src/backend/access/transam/xlogprefetcher.c
 *
 * This module provides a drop-in replacement for an XLogReader that tries to
 * minimize I/O stalls by looking ahead in the WAL.  If blocks that will be
 * accessed in the near future are not already in the buffer pool, it initiates
 * I/Os that might complete before the caller eventually needs the data.  When
 * referenced blocks are found in the buffer pool already, the buffer is
 * recorded in the decoded record so that XLogReadBufferForRedo() can try to
 * avoid a second buffer mapping table lookup.
 *
 * Currently, only the main fork is considered for prefetching.  Currently,
 * prefetching is only effective on systems where BufferPrefetch() does
 * something useful (mainly Linux).
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlogprefetcher.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "catalog/pg_class.h"
#include "catalog/pg_control.h"
#include "catalog/storage_xlog.h"
#include "commands/dbcommands_xlog.h"
#include "utils/fmgrprotos.h"
#include "utils/timestamp.h"
#include "funcapi.h"
#include "pgstat.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "utils/guc.h"
#include "utils/hsearch.h"

/*
 * Every time we process this much WAL, we'll update the values in
 * pg_stat_recovery_prefetch.
 */
#define XLOGPREFETCHER_STATS_DISTANCE BLCKSZ

/*
 * To detect repeated access to the same block and skip useless extra system
 * calls, we remember a small window of recently prefetched blocks.
 */
#define XLOGPREFETCHER_SEQ_WINDOW_SIZE 4

/*
 * When maintenance_io_concurrency is not saturated, we're prepared to look
 * ahead up to N times that number of block references.
 */
#define XLOGPREFETCHER_DISTANCE_MULTIPLIER 4

/* Define to log internal debugging messages. */
/* #define XLOGPREFETCHER_DEBUG_LEVEL LOG */

/* GUCs */
int			recovery_prefetch = RECOVERY_PREFETCH_TRY;

#ifdef USE_PREFETCH
#define RecoveryPrefetchEnabled() \
		(recovery_prefetch != RECOVERY_PREFETCH_OFF && \
		 maintenance_io_concurrency > 0)
#else
#define RecoveryPrefetchEnabled() false
#endif

static int	XLogPrefetchReconfigureCount = 0;

/*
 * Enum used to report whether an IO should be started.
 */
typedef enum
{
	LRQ_NEXT_NO_IO,
	LRQ_NEXT_IO,
	LRQ_NEXT_AGAIN
} LsnReadQueueNextStatus;

/*
 * Type of callback that can decide which block to prefetch next.  For now
 * there is only one.
 */
typedef LsnReadQueueNextStatus (*LsnReadQueueNextFun) (uintptr_t lrq_private,
													   XLogRecPtr *lsn);

/*
 * A simple circular queue of LSNs, using to control the number of
 * (potentially) inflight IOs.  This stands in for a later more general IO
 * control mechanism, which is why it has the apparently unnecessary
 * indirection through a function pointer.
 */
typedef struct LsnReadQueue
{
	LsnReadQueueNextFun next;
	uintptr_t	lrq_private;
	uint32		max_inflight;
	uint32		inflight;
	uint32		completed;
	uint32		head;
	uint32		tail;
	uint32		size;
	struct
	{
		bool		io;
		XLogRecPtr	lsn;
	}			queue[FLEXIBLE_ARRAY_MEMBER];
} LsnReadQueue;

/*
 * A prefetcher.  This is a mechanism that wraps an XLogReader, prefetching
 * blocks that will be soon be referenced, to try to avoid IO stalls.
 */
struct XLogPrefetcher
{
	/* WAL reader and current reading state. */
	XLogReaderState *reader;
	DecodedXLogRecord *record;
	int			next_block_id;

	/* When to publish stats. */
	XLogRecPtr	next_stats_shm_lsn;

	/* Book-keeping to avoid accessing blocks that don't exist yet. */
	HTAB	   *filter_table;
	dlist_head	filter_queue;

	/* Book-keeping to avoid repeat prefetches. */
	RelFileNode recent_rnode[XLOGPREFETCHER_SEQ_WINDOW_SIZE];
	BlockNumber recent_block[XLOGPREFETCHER_SEQ_WINDOW_SIZE];
	int			recent_idx;

	/* Book-keeping to disable prefetching temporarily. */
	XLogRecPtr	no_readahead_until;

	/* IO depth manager. */
	LsnReadQueue *streaming_read;

	XLogRecPtr	begin_ptr;

	int			reconfigure_count;
};

/*
 * A temporary filter used to track block ranges that haven't been created
 * yet, whole relations that haven't been created yet, and whole relations
 * that (we assume) have already been dropped, or will be created by bulk WAL
 * operators.
 */
typedef struct XLogPrefetcherFilter
{
	RelFileNode rnode;
	XLogRecPtr	filter_until_replayed;
	BlockNumber filter_from_block;
	dlist_node	link;
} XLogPrefetcherFilter;

/*
 * Counters exposed in shared memory for pg_stat_recovery_prefetch.
 */
typedef struct XLogPrefetchStats
{
	pg_atomic_uint64 reset_time;	/* Time of last reset. */
	pg_atomic_uint64 prefetch;	/* Prefetches initiated. */
	pg_atomic_uint64 hit;		/* Blocks already in cache. */
	pg_atomic_uint64 skip_init; /* Zero-inited blocks skipped. */
	pg_atomic_uint64 skip_new;	/* New/missing blocks filtered. */
	pg_atomic_uint64 skip_fpw;	/* FPWs skipped. */
	pg_atomic_uint64 skip_rep;	/* Repeat accesses skipped. */

	/* Dynamic values */
	int			wal_distance;	/* Number of WAL bytes ahead. */
	int			block_distance; /* Number of block references ahead. */
	int			io_depth;		/* Number of I/Os in progress. */
} XLogPrefetchStats;

static inline void XLogPrefetcherAddFilter(XLogPrefetcher *prefetcher,
										   RelFileNode rnode,
										   BlockNumber blockno,
										   XLogRecPtr lsn);
static inline bool XLogPrefetcherIsFiltered(XLogPrefetcher *prefetcher,
											RelFileNode rnode,
											BlockNumber blockno);
static inline void XLogPrefetcherCompleteFilters(XLogPrefetcher *prefetcher,
												 XLogRecPtr replaying_lsn);
static LsnReadQueueNextStatus XLogPrefetcherNextBlock(uintptr_t pgsr_private,
													  XLogRecPtr *lsn);

static XLogPrefetchStats *SharedStats;

static inline LsnReadQueue *
lrq_alloc(uint32 max_distance,
		  uint32 max_inflight,
		  uintptr_t lrq_private,
		  LsnReadQueueNextFun next)
{
	LsnReadQueue *lrq;
	uint32		size;

	Assert(max_distance >= max_inflight);

	size = max_distance + 1;	/* full ring buffer has a gap */
	lrq = palloc(offsetof(LsnReadQueue, queue) + sizeof(lrq->queue[0]) * size);
	lrq->lrq_private = lrq_private;
	lrq->max_inflight = max_inflight;
	lrq->size = size;
	lrq->next = next;
	lrq->head = 0;
	lrq->tail = 0;
	lrq->inflight = 0;
	lrq->completed = 0;

	return lrq;
}

static inline void
lrq_free(LsnReadQueue *lrq)
{
	pfree(lrq);
}

static inline uint32
lrq_inflight(LsnReadQueue *lrq)
{
	return lrq->inflight;
}

static inline uint32
lrq_completed(LsnReadQueue *lrq)
{
	return lrq->completed;
}

static inline void
lrq_prefetch(LsnReadQueue *lrq)
{
	/* Try to start as many IOs as we can within our limits. */
	while (lrq->inflight < lrq->max_inflight &&
		   lrq->inflight + lrq->completed < lrq->size - 1)
	{
		Assert(((lrq->head + 1) % lrq->size) != lrq->tail);
		switch (lrq->next(lrq->lrq_private, &lrq->queue[lrq->head].lsn))
		{
			case LRQ_NEXT_AGAIN:
				return;
			case LRQ_NEXT_IO:
				lrq->queue[lrq->head].io = true;
				lrq->inflight++;
				break;
			case LRQ_NEXT_NO_IO:
				lrq->queue[lrq->head].io = false;
				lrq->completed++;
				break;
		}
		lrq->head++;
		if (lrq->head == lrq->size)
			lrq->head = 0;
	}
}

static inline void
lrq_complete_lsn(LsnReadQueue *lrq, XLogRecPtr lsn)
{
	/*
	 * We know that LSNs before 'lsn' have been replayed, so we can now assume
	 * that any IOs that were started before then have finished.
	 */
	while (lrq->tail != lrq->head &&
		   lrq->queue[lrq->tail].lsn < lsn)
	{
		if (lrq->queue[lrq->tail].io)
			lrq->inflight--;
		else
			lrq->completed--;
		lrq->tail++;
		if (lrq->tail == lrq->size)
			lrq->tail = 0;
	}
	if (RecoveryPrefetchEnabled())
		lrq_prefetch(lrq);
}

size_t
XLogPrefetchShmemSize(void)
{
	return sizeof(XLogPrefetchStats);
}

/*
 * Reset all counters to zero.
 */
void
XLogPrefetchResetStats(void)
{
	pg_atomic_write_u64(&SharedStats->reset_time, GetCurrentTimestamp());
	pg_atomic_write_u64(&SharedStats->prefetch, 0);
	pg_atomic_write_u64(&SharedStats->hit, 0);
	pg_atomic_write_u64(&SharedStats->skip_init, 0);
	pg_atomic_write_u64(&SharedStats->skip_new, 0);
	pg_atomic_write_u64(&SharedStats->skip_fpw, 0);
	pg_atomic_write_u64(&SharedStats->skip_rep, 0);
}

void
XLogPrefetchShmemInit(void)
{
	bool		found;

	SharedStats = (XLogPrefetchStats *)
		ShmemInitStruct("XLogPrefetchStats",
						sizeof(XLogPrefetchStats),
						&found);

	if (!found)
	{
		pg_atomic_init_u64(&SharedStats->reset_time, GetCurrentTimestamp());
		pg_atomic_init_u64(&SharedStats->prefetch, 0);
		pg_atomic_init_u64(&SharedStats->hit, 0);
		pg_atomic_init_u64(&SharedStats->skip_init, 0);
		pg_atomic_init_u64(&SharedStats->skip_new, 0);
		pg_atomic_init_u64(&SharedStats->skip_fpw, 0);
		pg_atomic_init_u64(&SharedStats->skip_rep, 0);
	}
}

/*
 * Called when any GUC is changed that affects prefetching.
 */
void
XLogPrefetchReconfigure(void)
{
	XLogPrefetchReconfigureCount++;
}

/*
 * Increment a counter in shared memory.  This is equivalent to *counter++ on a
 * plain uint64 without any memory barrier or locking, except on platforms
 * where readers can't read uint64 without possibly observing a torn value.
 */
static inline void
XLogPrefetchIncrement(pg_atomic_uint64 *counter)
{
	Assert(AmStartupProcess() || !IsUnderPostmaster);
	pg_atomic_write_u64(counter, pg_atomic_read_u64(counter) + 1);
}

/*
 * Create a prefetcher that is ready to begin prefetching blocks referenced by
 * WAL records.
 */
XLogPrefetcher *
XLogPrefetcherAllocate(XLogReaderState *reader)
{
	XLogPrefetcher *prefetcher;
	static HASHCTL hash_table_ctl = {
		.keysize = sizeof(RelFileNode),
		.entrysize = sizeof(XLogPrefetcherFilter)
	};

	prefetcher = palloc0(sizeof(XLogPrefetcher));

	prefetcher->reader = reader;
	prefetcher->filter_table = hash_create("XLogPrefetcherFilterTable", 1024,
										   &hash_table_ctl,
										   HASH_ELEM | HASH_BLOBS);
	dlist_init(&prefetcher->filter_queue);

	SharedStats->wal_distance = 0;
	SharedStats->block_distance = 0;
	SharedStats->io_depth = 0;

	/* First usage will cause streaming_read to be allocated. */
	prefetcher->reconfigure_count = XLogPrefetchReconfigureCount - 1;

	return prefetcher;
}

/*
 * Destroy a prefetcher and release all resources.
 */
void
XLogPrefetcherFree(XLogPrefetcher *prefetcher)
{
	lrq_free(prefetcher->streaming_read);
	hash_destroy(prefetcher->filter_table);
	pfree(prefetcher);
}

/*
 * Provide access to the reader.
 */
XLogReaderState *
XLogPrefetcherGetReader(XLogPrefetcher *prefetcher)
{
	return prefetcher->reader;
}

/*
 * Update the statistics visible in the pg_stat_recovery_prefetch view.
 */
void
XLogPrefetcherComputeStats(XLogPrefetcher *prefetcher)
{
	uint32		io_depth;
	uint32		completed;
	int64		wal_distance;


	/* How far ahead of replay are we now? */
	if (prefetcher->reader->decode_queue_tail)
	{
		wal_distance =
			prefetcher->reader->decode_queue_tail->lsn -
			prefetcher->reader->decode_queue_head->lsn;
	}
	else
	{
		wal_distance = 0;
	}

	/* How many IOs are currently in flight and completed? */
	io_depth = lrq_inflight(prefetcher->streaming_read);
	completed = lrq_completed(prefetcher->streaming_read);

	/* Update the instantaneous stats visible in pg_stat_recovery_prefetch. */
	SharedStats->io_depth = io_depth;
	SharedStats->block_distance = io_depth + completed;
	SharedStats->wal_distance = wal_distance;

	prefetcher->next_stats_shm_lsn =
		prefetcher->reader->ReadRecPtr + XLOGPREFETCHER_STATS_DISTANCE;
}

/*
 * A callback that examines the next block reference in the WAL, and possibly
 * starts an IO so that a later read will be fast.
 *
 * Returns LRQ_NEXT_AGAIN if no more WAL data is available yet.
 *
 * Returns LRQ_NEXT_IO if the next block reference is for a main fork block
 * that isn't in the buffer pool, and the kernel has been asked to start
 * reading it to make a future read system call faster. An LSN is written to
 * *lsn, and the I/O will be considered to have completed once that LSN is
 * replayed.
 *
 * Returns LRQ_NO_IO if we examined the next block reference and found that it
 * was already in the buffer pool, or we decided for various reasons not to
 * prefetch.
 */
static LsnReadQueueNextStatus
XLogPrefetcherNextBlock(uintptr_t pgsr_private, XLogRecPtr *lsn)
{
	XLogPrefetcher *prefetcher = (XLogPrefetcher *) pgsr_private;
	XLogReaderState *reader = prefetcher->reader;
	XLogRecPtr	replaying_lsn = reader->ReadRecPtr;

	/*
	 * We keep track of the record and block we're up to between calls with
	 * prefetcher->record and prefetcher->next_block_id.
	 */
	for (;;)
	{
		DecodedXLogRecord *record;

		/* Try to read a new future record, if we don't already have one. */
		if (prefetcher->record == NULL)
		{
			bool		nonblocking;

			/*
			 * If there are already records or an error queued up that could
			 * be replayed, we don't want to block here.  Otherwise, it's OK
			 * to block waiting for more data: presumably the caller has
			 * nothing else to do.
			 */
			nonblocking = XLogReaderHasQueuedRecordOrError(reader);

			/* Readahead is disabled until we replay past a certain point. */
			if (nonblocking && replaying_lsn <= prefetcher->no_readahead_until)
				return LRQ_NEXT_AGAIN;

			record = XLogReadAhead(prefetcher->reader, nonblocking);
			if (record == NULL)
			{
				/*
				 * We can't read any more, due to an error or lack of data in
				 * nonblocking mode.  Don't try to read ahead again until
				 * we've replayed everything already decoded.
				 */
				if (nonblocking && prefetcher->reader->decode_queue_tail)
					prefetcher->no_readahead_until =
						prefetcher->reader->decode_queue_tail->lsn;

				return LRQ_NEXT_AGAIN;
			}

			/*
			 * If prefetching is disabled, we don't need to analyze the record
			 * or issue any prefetches.  We just need to cause one record to
			 * be decoded.
			 */
			if (!RecoveryPrefetchEnabled())
			{
				*lsn = InvalidXLogRecPtr;
				return LRQ_NEXT_NO_IO;
			}

			/* We have a new record to process. */
			prefetcher->record = record;
			prefetcher->next_block_id = 0;
		}
		else
		{
			/* Continue to process from last call, or last loop. */
			record = prefetcher->record;
		}

		/*
		 * Check for operations that require us to filter out block ranges, or
		 * pause readahead completely.
		 */
		if (replaying_lsn < record->lsn)
		{
			uint8		rmid = record->header.xl_rmid;
			uint8		record_type = record->header.xl_info & ~XLR_INFO_MASK;

			if (rmid == RM_XLOG_ID)
			{
				if (record_type == XLOG_CHECKPOINT_SHUTDOWN ||
					record_type == XLOG_END_OF_RECOVERY)
				{
					/*
					 * These records might change the TLI.  Avoid potential
					 * bugs if we were to allow "read TLI" and "replay TLI" to
					 * differ without more analysis.
					 */
					prefetcher->no_readahead_until = record->lsn;

#ifdef XLOGPREFETCHER_DEBUG_LEVEL
					elog(XLOGPREFETCHER_DEBUG_LEVEL,
						 "suppressing all readahead until %X/%X is replayed due to possible TLI change",
						 LSN_FORMAT_ARGS(record->lsn));
#endif

					/* Fall through so we move past this record. */
				}
			}
			else if (rmid == RM_DBASE_ID)
			{
				/*
				 * When databases are created with the file-copy strategy,
				 * there are no WAL records to tell us about the creation of
				 * individual relations.
				 */
				if (record_type == XLOG_DBASE_CREATE_FILE_COPY)
				{
					xl_dbase_create_file_copy_rec *xlrec =
					(xl_dbase_create_file_copy_rec *) record->main_data;
					RelFileNode rnode = {InvalidOid, xlrec->db_id, InvalidOid};

					/*
					 * Don't try to prefetch anything in this database until
					 * it has been created, or we might confuse the blocks of
					 * different generations, if a database OID or relfilenode
					 * is reused.  It's also more efficient than discovering
					 * that relations don't exist on disk yet with ENOENT
					 * errors.
					 */
					XLogPrefetcherAddFilter(prefetcher, rnode, 0, record->lsn);

#ifdef XLOGPREFETCHER_DEBUG_LEVEL
					elog(XLOGPREFETCHER_DEBUG_LEVEL,
						 "suppressing prefetch in database %u until %X/%X is replayed due to raw file copy",
						 rnode.dbNode,
						 LSN_FORMAT_ARGS(record->lsn));
#endif
				}
			}
			else if (rmid == RM_SMGR_ID)
			{
				if (record_type == XLOG_SMGR_CREATE)
				{
					xl_smgr_create *xlrec = (xl_smgr_create *)
					record->main_data;

					if (xlrec->forkNum == MAIN_FORKNUM)
					{
						/*
						 * Don't prefetch anything for this whole relation
						 * until it has been created.  Otherwise we might
						 * confuse the blocks of different generations, if a
						 * relfilenode is reused.  This also avoids the need
						 * to discover the problem via extra syscalls that
						 * report ENOENT.
						 */
						XLogPrefetcherAddFilter(prefetcher, xlrec->rnode, 0,
												record->lsn);

#ifdef XLOGPREFETCHER_DEBUG_LEVEL
						elog(XLOGPREFETCHER_DEBUG_LEVEL,
							 "suppressing prefetch in relation %u/%u/%u until %X/%X is replayed, which creates the relation",
							 xlrec->rnode.spcNode,
							 xlrec->rnode.dbNode,
							 xlrec->rnode.relNode,
							 LSN_FORMAT_ARGS(record->lsn));
#endif
					}
				}
				else if (record_type == XLOG_SMGR_TRUNCATE)
				{
					xl_smgr_truncate *xlrec = (xl_smgr_truncate *)
					record->main_data;

					/*
					 * Don't consider prefetching anything in the truncated
					 * range until the truncation has been performed.
					 */
					XLogPrefetcherAddFilter(prefetcher, xlrec->rnode,
											xlrec->blkno,
											record->lsn);

#ifdef XLOGPREFETCHER_DEBUG_LEVEL
					elog(XLOGPREFETCHER_DEBUG_LEVEL,
						 "suppressing prefetch in relation %u/%u/%u from block %u until %X/%X is replayed, which truncates the relation",
						 xlrec->rnode.spcNode,
						 xlrec->rnode.dbNode,
						 xlrec->rnode.relNode,
						 xlrec->blkno,
						 LSN_FORMAT_ARGS(record->lsn));
#endif
				}
			}
		}

		/* Scan the block references, starting where we left off last time. */
		while (prefetcher->next_block_id <= record->max_block_id)
		{
			int			block_id = prefetcher->next_block_id++;
			DecodedBkpBlock *block = &record->blocks[block_id];
			SMgrRelation reln;
			PrefetchBufferResult result;

			if (!block->in_use)
				continue;

			Assert(!BufferIsValid(block->prefetch_buffer));;

			/*
			 * Record the LSN of this record.  When it's replayed,
			 * LsnReadQueue will consider any IOs submitted for earlier LSNs
			 * to be finished.
			 */
			*lsn = record->lsn;

			/* We don't try to prefetch anything but the main fork for now. */
			if (block->forknum != MAIN_FORKNUM)
			{
				return LRQ_NEXT_NO_IO;
			}

			/*
			 * If there is a full page image attached, we won't be reading the
			 * page, so don't bother trying to prefetch.
			 */
			if (block->has_image)
			{
				XLogPrefetchIncrement(&SharedStats->skip_fpw);
				return LRQ_NEXT_NO_IO;
			}

			/* There is no point in reading a page that will be zeroed. */
			if (block->flags & BKPBLOCK_WILL_INIT)
			{
				XLogPrefetchIncrement(&SharedStats->skip_init);
				return LRQ_NEXT_NO_IO;
			}

			/* Should we skip prefetching this block due to a filter? */
			if (XLogPrefetcherIsFiltered(prefetcher, block->rnode, block->blkno))
			{
				XLogPrefetchIncrement(&SharedStats->skip_new);
				return LRQ_NEXT_NO_IO;
			}

			/* There is no point in repeatedly prefetching the same block. */
			for (int i = 0; i < XLOGPREFETCHER_SEQ_WINDOW_SIZE; ++i)
			{
				if (block->blkno == prefetcher->recent_block[i] &&
					RelFileNodeEquals(block->rnode, prefetcher->recent_rnode[i]))
				{
					/*
					 * XXX If we also remembered where it was, we could set
					 * recent_buffer so that recovery could skip smgropen()
					 * and a buffer table lookup.
					 */
					XLogPrefetchIncrement(&SharedStats->skip_rep);
					return LRQ_NEXT_NO_IO;
				}
			}
			prefetcher->recent_rnode[prefetcher->recent_idx] = block->rnode;
			prefetcher->recent_block[prefetcher->recent_idx] = block->blkno;
			prefetcher->recent_idx =
				(prefetcher->recent_idx + 1) % XLOGPREFETCHER_SEQ_WINDOW_SIZE;

			/*
			 * We could try to have a fast path for repeated references to the
			 * same relation (with some scheme to handle invalidations
			 * safely), but for now we'll call smgropen() every time.
			 */
			reln = smgropen(block->rnode, InvalidBackendId);

			/*
			 * If the relation file doesn't exist on disk, for example because
			 * we're replaying after a crash and the file will be created and
			 * then unlinked by WAL that hasn't been replayed yet, suppress
			 * further prefetching in the relation until this record is
			 * replayed.
			 */
			if (!smgrexists(reln, MAIN_FORKNUM))
			{
#ifdef XLOGPREFETCHER_DEBUG_LEVEL
				elog(XLOGPREFETCHER_DEBUG_LEVEL,
					 "suppressing all prefetch in relation %u/%u/%u until %X/%X is replayed, because the relation does not exist on disk",
					 reln->smgr_rnode.node.spcNode,
					 reln->smgr_rnode.node.dbNode,
					 reln->smgr_rnode.node.relNode,
					 LSN_FORMAT_ARGS(record->lsn));
#endif
				XLogPrefetcherAddFilter(prefetcher, block->rnode, 0,
										record->lsn);
				XLogPrefetchIncrement(&SharedStats->skip_new);
				return LRQ_NEXT_NO_IO;
			}

			/*
			 * If the relation isn't big enough to contain the referenced
			 * block yet, suppress prefetching of this block and higher until
			 * this record is replayed.
			 */
			if (block->blkno >= smgrnblocks(reln, block->forknum))
			{
#ifdef XLOGPREFETCHER_DEBUG_LEVEL
				elog(XLOGPREFETCHER_DEBUG_LEVEL,
					 "suppressing prefetch in relation %u/%u/%u from block %u until %X/%X is replayed, because the relation is too small",
					 reln->smgr_rnode.node.spcNode,
					 reln->smgr_rnode.node.dbNode,
					 reln->smgr_rnode.node.relNode,
					 block->blkno,
					 LSN_FORMAT_ARGS(record->lsn));
#endif
				XLogPrefetcherAddFilter(prefetcher, block->rnode, block->blkno,
										record->lsn);
				XLogPrefetchIncrement(&SharedStats->skip_new);
				return LRQ_NEXT_NO_IO;
			}

			/* Try to initiate prefetching. */
			result = PrefetchSharedBuffer(reln, block->forknum, block->blkno);
			if (BufferIsValid(result.recent_buffer))
			{
				/* Cache hit, nothing to do. */
				XLogPrefetchIncrement(&SharedStats->hit);
				block->prefetch_buffer = result.recent_buffer;
				return LRQ_NEXT_NO_IO;
			}
			else if (result.initiated_io)
			{
				/* Cache miss, I/O (presumably) started. */
				XLogPrefetchIncrement(&SharedStats->prefetch);
				block->prefetch_buffer = InvalidBuffer;
				return LRQ_NEXT_IO;
			}
			else
			{
				/*
				 * This shouldn't be possible, because we already determined
				 * that the relation exists on disk and is big enough.
				 * Something is wrong with the cache invalidation for
				 * smgrexists(), smgrnblocks(), or the file was unlinked or
				 * truncated beneath our feet?
				 */
				elog(ERROR,
					 "could not prefetch relation %u/%u/%u block %u",
					 reln->smgr_rnode.node.spcNode,
					 reln->smgr_rnode.node.dbNode,
					 reln->smgr_rnode.node.relNode,
					 block->blkno);
			}
		}

		/*
		 * Several callsites need to be able to read exactly one record
		 * without any internal readahead.  Examples: xlog.c reading
		 * checkpoint records with emode set to PANIC, which might otherwise
		 * cause XLogPageRead() to panic on some future page, and xlog.c
		 * determining where to start writing WAL next, which depends on the
		 * contents of the reader's internal buffer after reading one record.
		 * Therefore, don't even think about prefetching until the first
		 * record after XLogPrefetcherBeginRead() has been consumed.
		 */
		if (prefetcher->reader->decode_queue_tail &&
			prefetcher->reader->decode_queue_tail->lsn == prefetcher->begin_ptr)
			return LRQ_NEXT_AGAIN;

		/* Advance to the next record. */
		prefetcher->record = NULL;
	}
	pg_unreachable();
}

/*
 * Expose statistics about recovery prefetching.
 */
Datum
pg_stat_get_recovery_prefetch(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_RECOVERY_PREFETCH_COLS 10
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Datum		values[PG_STAT_GET_RECOVERY_PREFETCH_COLS];
	bool		nulls[PG_STAT_GET_RECOVERY_PREFETCH_COLS];

	SetSingleFuncCall(fcinfo, 0);

	for (int i = 0; i < PG_STAT_GET_RECOVERY_PREFETCH_COLS; ++i)
		nulls[i] = false;

	values[0] = TimestampTzGetDatum(pg_atomic_read_u64(&SharedStats->reset_time));
	values[1] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->prefetch));
	values[2] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->hit));
	values[3] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_init));
	values[4] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_new));
	values[5] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_fpw));
	values[6] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_rep));
	values[7] = Int32GetDatum(SharedStats->wal_distance);
	values[8] = Int32GetDatum(SharedStats->block_distance);
	values[9] = Int32GetDatum(SharedStats->io_depth);
	tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);

	return (Datum) 0;
}

/*
 * Don't prefetch any blocks >= 'blockno' from a given 'rnode', until 'lsn'
 * has been replayed.
 */
static inline void
XLogPrefetcherAddFilter(XLogPrefetcher *prefetcher, RelFileNode rnode,
						BlockNumber blockno, XLogRecPtr lsn)
{
	XLogPrefetcherFilter *filter;
	bool		found;

	filter = hash_search(prefetcher->filter_table, &rnode, HASH_ENTER, &found);
	if (!found)
	{
		/*
		 * Don't allow any prefetching of this block or higher until replayed.
		 */
		filter->filter_until_replayed = lsn;
		filter->filter_from_block = blockno;
		dlist_push_head(&prefetcher->filter_queue, &filter->link);
	}
	else
	{
		/*
		 * We were already filtering this rnode.  Extend the filter's lifetime
		 * to cover this WAL record, but leave the lower of the block numbers
		 * there because we don't want to have to track individual blocks.
		 */
		filter->filter_until_replayed = lsn;
		dlist_delete(&filter->link);
		dlist_push_head(&prefetcher->filter_queue, &filter->link);
		filter->filter_from_block = Min(filter->filter_from_block, blockno);
	}
}

/*
 * Have we replayed any records that caused us to begin filtering a block
 * range?  That means that relations should have been created, extended or
 * dropped as required, so we can stop filtering out accesses to a given
 * relfilenode.
 */
static inline void
XLogPrefetcherCompleteFilters(XLogPrefetcher *prefetcher, XLogRecPtr replaying_lsn)
{
	while (unlikely(!dlist_is_empty(&prefetcher->filter_queue)))
	{
		XLogPrefetcherFilter *filter = dlist_tail_element(XLogPrefetcherFilter,
														  link,
														  &prefetcher->filter_queue);

		if (filter->filter_until_replayed >= replaying_lsn)
			break;

		dlist_delete(&filter->link);
		hash_search(prefetcher->filter_table, filter, HASH_REMOVE, NULL);
	}
}

/*
 * Check if a given block should be skipped due to a filter.
 */
static inline bool
XLogPrefetcherIsFiltered(XLogPrefetcher *prefetcher, RelFileNode rnode,
						 BlockNumber blockno)
{
	/*
	 * Test for empty queue first, because we expect it to be empty most of
	 * the time and we can avoid the hash table lookup in that case.
	 */
	if (unlikely(!dlist_is_empty(&prefetcher->filter_queue)))
	{
		XLogPrefetcherFilter *filter;

		/* See if the block range is filtered. */
		filter = hash_search(prefetcher->filter_table, &rnode, HASH_FIND, NULL);
		if (filter && filter->filter_from_block <= blockno)
		{
#ifdef XLOGPREFETCHER_DEBUG_LEVEL
			elog(XLOGPREFETCHER_DEBUG_LEVEL,
				 "prefetch of %u/%u/%u block %u suppressed; filtering until LSN %X/%X is replayed (blocks >= %u filtered)",
				 rnode.spcNode, rnode.dbNode, rnode.relNode, blockno,
				 LSN_FORMAT_ARGS(filter->filter_until_replayed),
				 filter->filter_from_block);
#endif
			return true;
		}

		/* See if the whole database is filtered. */
		rnode.relNode = InvalidOid;
		rnode.spcNode = InvalidOid;
		filter = hash_search(prefetcher->filter_table, &rnode, HASH_FIND, NULL);
		if (filter)
		{
#ifdef XLOGPREFETCHER_DEBUG_LEVEL
			elog(XLOGPREFETCHER_DEBUG_LEVEL,
				 "prefetch of %u/%u/%u block %u suppressed; filtering until LSN %X/%X is replayed (whole database)",
				 rnode.spcNode, rnode.dbNode, rnode.relNode, blockno,
				 LSN_FORMAT_ARGS(filter->filter_until_replayed));
#endif
			return true;
		}
	}

	return false;
}

/*
 * A wrapper for XLogBeginRead() that also resets the prefetcher.
 */
void
XLogPrefetcherBeginRead(XLogPrefetcher *prefetcher, XLogRecPtr recPtr)
{
	/* This will forget about any in-flight IO. */
	prefetcher->reconfigure_count--;

	/* Book-keeping to avoid readahead on first read. */
	prefetcher->begin_ptr = recPtr;

	prefetcher->no_readahead_until = 0;

	/* This will forget about any queued up records in the decoder. */
	XLogBeginRead(prefetcher->reader, recPtr);
}

/*
 * A wrapper for XLogReadRecord() that provides the same interface, but also
 * tries to initiate I/O for blocks referenced in future WAL records.
 */
XLogRecord *
XLogPrefetcherReadRecord(XLogPrefetcher *prefetcher, char **errmsg)
{
	DecodedXLogRecord *record;
	XLogRecPtr	replayed_up_to;

	/*
	 * See if it's time to reset the prefetching machinery, because a relevant
	 * GUC was changed.
	 */
	if (unlikely(XLogPrefetchReconfigureCount != prefetcher->reconfigure_count))
	{
		uint32		max_distance;
		uint32		max_inflight;

		if (prefetcher->streaming_read)
			lrq_free(prefetcher->streaming_read);

		if (RecoveryPrefetchEnabled())
		{
			Assert(maintenance_io_concurrency > 0);
			max_inflight = maintenance_io_concurrency;
			max_distance = max_inflight * XLOGPREFETCHER_DISTANCE_MULTIPLIER;
		}
		else
		{
			max_inflight = 1;
			max_distance = 1;
		}

		prefetcher->streaming_read = lrq_alloc(max_distance,
											   max_inflight,
											   (uintptr_t) prefetcher,
											   XLogPrefetcherNextBlock);

		prefetcher->reconfigure_count = XLogPrefetchReconfigureCount;
	}

	/*
	 * Release last returned record, if there is one, as it's now been
	 * replayed.
	 */
	replayed_up_to = XLogReleasePreviousRecord(prefetcher->reader);

	/*
	 * Can we drop any filters yet?  If we were waiting for a relation to be
	 * created or extended, it is now OK to access blocks in the covered
	 * range.
	 */
	XLogPrefetcherCompleteFilters(prefetcher, replayed_up_to);

	/*
	 * All IO initiated by earlier WAL is now completed.  This might trigger
	 * further prefetching.
	 */
	lrq_complete_lsn(prefetcher->streaming_read, replayed_up_to);

	/*
	 * If there's nothing queued yet, then start prefetching to cause at least
	 * one record to be queued.
	 */
	if (!XLogReaderHasQueuedRecordOrError(prefetcher->reader))
	{
		Assert(lrq_inflight(prefetcher->streaming_read) == 0);
		Assert(lrq_completed(prefetcher->streaming_read) == 0);
		lrq_prefetch(prefetcher->streaming_read);
	}

	/* Read the next record. */
	record = XLogNextRecord(prefetcher->reader, errmsg);
	if (!record)
		return NULL;

	/*
	 * The record we just got is the "current" one, for the benefit of the
	 * XLogRecXXX() macros.
	 */
	Assert(record == prefetcher->reader->record);

	/*
	 * If maintenance_io_concurrency is set very low, we might have started
	 * prefetching some but not all of the blocks referenced in the record
	 * we're about to return.  Forget about the rest of the blocks in this
	 * record by dropping the prefetcher's reference to it.
	 */
	if (record == prefetcher->record)
		prefetcher->record = NULL;

	/*
	 * See if it's time to compute some statistics, because enough WAL has
	 * been processed.
	 */
	if (unlikely(record->lsn >= prefetcher->next_stats_shm_lsn))
		XLogPrefetcherComputeStats(prefetcher);

	Assert(record == prefetcher->reader->record);

	return &record->header;
}

bool
check_recovery_prefetch(int *new_value, void **extra, GucSource source)
{
#ifndef USE_PREFETCH
	if (*new_value == RECOVERY_PREFETCH_ON)
	{
		GUC_check_errdetail("recovery_prefetch is not supported on platforms that lack posix_fadvise().");
		return false;
	}
#endif

	return true;
}

void
assign_recovery_prefetch(int new_value, void *extra)
{
	/* Reconfigure prefetching, because a setting it depends on changed. */
	recovery_prefetch = new_value;
	if (AmStartupProcess())
		XLogPrefetchReconfigure();
}
