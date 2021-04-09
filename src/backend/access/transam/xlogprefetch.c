/*-------------------------------------------------------------------------
 *
 * xlogprefetch.c
 *		Prefetching support for recovery.
 *
 * Portions Copyright (c) 2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *		src/backend/access/transam/xlogprefetch.c
 *
 * The goal of this module is to read future WAL records and issue
 * PrefetchSharedBuffer() calls for referenced blocks, so that we avoid I/O
 * stalls in the main recovery loop.
 *
 * When examining a WAL record from the future, we need to consider that a
 * referenced block or segment file might not exist on disk until this record
 * or some earlier record has been replayed.  After a crash, a file might also
 * be missing because it was dropped by a later WAL record; in that case, it
 * will be recreated when this record is replayed.  These cases are handled by
 * recognizing them and adding a "filter" that prevents all prefetching of a
 * certain block range until the present WAL record has been replayed.  Blocks
 * skipped for these reasons are counted as "skip_new" (that is, cases where we
 * didn't try to prefetch "new" blocks).
 *
 * Blocks found in the buffer pool already are counted as "skip_hit".
 * Repeated access to the same buffer is detected and skipped, and this is
 * counted with "skip_seq".  Blocks that were logged with FPWs are skipped if
 * recovery_prefetch_fpw is off, since on most systems there will be no I/O
 * stall; this is counted with "skip_fpw".
 *
 * The only way we currently have to know that an I/O initiated with
 * PrefetchSharedBuffer() has completed is to wait for the corresponding call
 * to XLogReadBufferInRedo() to return.  Therefore, we track the number of
 * potentially in-flight I/Os by using a circular buffer of LSNs.  When it's
 * full, we have to wait for recovery to replay enough records to remove some
 * LSNs, and only then can we initiate more prefetching.  Ideally, this keeps
 * us just the right distance ahead to respect maintenance_io_concurrency,
 * though in practice it errs on the side of being too conservative because
 * many I/Os complete sooner than we know.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlogprefetch.h"
#include "access/xlogreader.h"
#include "access/xlogutils.h"
#include "catalog/storage_xlog.h"
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
 * Sample the queue depth and distance every time we replay this much WAL.
 * This is used to compute avg_queue_depth and avg_distance for the log
 * message that appears at the end of crash recovery.  It's also used to send
 * messages periodically to the stats collector, to save the counters on disk.
 */
#define XLOGPREFETCHER_SAMPLE_DISTANCE 0x40000

/* GUCs */
bool		recovery_prefetch = false;
bool		recovery_prefetch_fpw = false;

int			XLogPrefetchReconfigureCount;

/*
 * A prefetcher object.  There is at most one of these in existence at a time,
 * recreated whenever there is a configuration change.
 */
struct XLogPrefetcher
{
	/* Reader and current reading state. */
	XLogReaderState *reader;
	DecodedXLogRecord *record;
	int			next_block_id;
	bool		shutdown;

	/* Details of last prefetch to skip repeats and seq scans. */
	SMgrRelation last_reln;
	RelFileNode last_rnode;
	BlockNumber last_blkno;

	/* Online averages. */
	uint64		samples;
	double		avg_queue_depth;
	double		avg_distance;
	XLogRecPtr	next_sample_lsn;

	/* Book-keeping required to avoid accessing non-existing blocks. */
	HTAB	   *filter_table;
	dlist_head	filter_queue;

	/* Book-keeping required to limit concurrent prefetches. */
	int			prefetch_head;
	int			prefetch_tail;
	int			prefetch_queue_size;
	XLogRecPtr	prefetch_queue[MAX_IO_CONCURRENCY + 1];
};

/*
 * A temporary filter used to track block ranges that haven't been created
 * yet, whole relations that haven't been created yet, and whole relations
 * that we must assume have already been dropped.
 */
typedef struct XLogPrefetcherFilter
{
	RelFileNode rnode;
	XLogRecPtr	filter_until_replayed;
	BlockNumber filter_from_block;
	dlist_node	link;
} XLogPrefetcherFilter;

/*
 * Counters exposed in shared memory for pg_stat_prefetch_recovery.
 */
typedef struct XLogPrefetchStats
{
	pg_atomic_uint64 reset_time;	/* Time of last reset. */
	pg_atomic_uint64 prefetch;	/* Prefetches initiated. */
	pg_atomic_uint64 skip_hit;	/* Blocks already buffered. */
	pg_atomic_uint64 skip_new;	/* New/missing blocks filtered. */
	pg_atomic_uint64 skip_fpw;	/* FPWs skipped. */
	pg_atomic_uint64 skip_seq;	/* Repeat blocks skipped. */
	float		avg_distance;
	float		avg_queue_depth;

	/* Reset counters */
	pg_atomic_uint32 reset_request;
	uint32		reset_handled;

	/* Dynamic values */
	int			distance;		/* Number of bytes ahead in the WAL. */
	int			queue_depth;	/* Number of I/Os possibly in progress. */
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
static inline void XLogPrefetcherInitiatedIO(XLogPrefetcher *prefetcher,
											 XLogRecPtr prefetching_lsn);
static inline void XLogPrefetcherCompletedIO(XLogPrefetcher *prefetcher,
											 XLogRecPtr replaying_lsn);
static inline bool XLogPrefetcherSaturated(XLogPrefetcher *prefetcher);
static bool XLogPrefetcherScanRecords(XLogPrefetcher *prefetcher,
									  XLogRecPtr replaying_lsn);
static bool XLogPrefetcherScanBlocks(XLogPrefetcher *prefetcher);
static void XLogPrefetchSaveStats(void);
static void XLogPrefetchRestoreStats(void);

static XLogPrefetchStats *SharedStats;

size_t
XLogPrefetchShmemSize(void)
{
	return sizeof(XLogPrefetchStats);
}

static void
XLogPrefetchResetStats(void)
{
	pg_atomic_write_u64(&SharedStats->reset_time, GetCurrentTimestamp());
	pg_atomic_write_u64(&SharedStats->prefetch, 0);
	pg_atomic_write_u64(&SharedStats->skip_hit, 0);
	pg_atomic_write_u64(&SharedStats->skip_new, 0);
	pg_atomic_write_u64(&SharedStats->skip_fpw, 0);
	pg_atomic_write_u64(&SharedStats->skip_seq, 0);
	SharedStats->avg_distance = 0;
	SharedStats->avg_queue_depth = 0;
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
		pg_atomic_init_u32(&SharedStats->reset_request, 0);
		SharedStats->reset_handled = 0;

		pg_atomic_init_u64(&SharedStats->reset_time, GetCurrentTimestamp());
		pg_atomic_init_u64(&SharedStats->prefetch, 0);
		pg_atomic_init_u64(&SharedStats->skip_hit, 0);
		pg_atomic_init_u64(&SharedStats->skip_new, 0);
		pg_atomic_init_u64(&SharedStats->skip_fpw, 0);
		pg_atomic_init_u64(&SharedStats->skip_seq, 0);
		SharedStats->avg_distance = 0;
		SharedStats->avg_queue_depth = 0;
		SharedStats->distance = 0;
		SharedStats->queue_depth = 0;
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
 * Called by any backend to request that the stats be reset.
 */
void
XLogPrefetchRequestResetStats(void)
{
	pg_atomic_fetch_add_u32(&SharedStats->reset_request, 1);
}

/*
 * Tell the stats collector to serialize the shared memory counters into the
 * stats file.
 */
static void
XLogPrefetchSaveStats(void)
{
	PgStat_RecoveryPrefetchStats serialized = {
		.prefetch = pg_atomic_read_u64(&SharedStats->prefetch),
		.skip_hit = pg_atomic_read_u64(&SharedStats->skip_hit),
		.skip_new = pg_atomic_read_u64(&SharedStats->skip_new),
		.skip_fpw = pg_atomic_read_u64(&SharedStats->skip_fpw),
		.skip_seq = pg_atomic_read_u64(&SharedStats->skip_seq),
		.stat_reset_timestamp = pg_atomic_read_u64(&SharedStats->reset_time)
	};

	pgstat_send_recoveryprefetch(&serialized);
}

/*
 * Try to restore the shared memory counters from the stats file.
 */
static void
XLogPrefetchRestoreStats(void)
{
	PgStat_RecoveryPrefetchStats *serialized = pgstat_fetch_recoveryprefetch();

	if (serialized->stat_reset_timestamp != 0)
	{
		pg_atomic_write_u64(&SharedStats->prefetch, serialized->prefetch);
		pg_atomic_write_u64(&SharedStats->skip_hit, serialized->skip_hit);
		pg_atomic_write_u64(&SharedStats->skip_new, serialized->skip_new);
		pg_atomic_write_u64(&SharedStats->skip_fpw, serialized->skip_fpw);
		pg_atomic_write_u64(&SharedStats->skip_seq, serialized->skip_seq);
		pg_atomic_write_u64(&SharedStats->reset_time, serialized->stat_reset_timestamp);
	}
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
 * Initialize an XLogPrefetchState object and restore the last saved
 * statistics from disk.
 */
void
XLogPrefetchBegin(XLogPrefetchState *state, XLogReaderState *reader)
{
	XLogPrefetchRestoreStats();

	/* We'll reconfigure on the first call to XLogPrefetch(). */
	state->reader = reader;
	state->prefetcher = NULL;
	state->reconfigure_count = XLogPrefetchReconfigureCount - 1;
}

/*
 * Shut down the prefetching infrastructure, if configured.
 */
void
XLogPrefetchEnd(XLogPrefetchState *state)
{
	XLogPrefetchSaveStats();

	if (state->prefetcher)
		XLogPrefetcherFree(state->prefetcher);
	state->prefetcher = NULL;

	SharedStats->queue_depth = 0;
	SharedStats->distance = 0;
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

	/*
	 * The size of the queue is based on the maintenance_io_concurrency
	 * setting.  In theory we might have a separate queue for each tablespace,
	 * but it's not clear how that should work, so for now we'll just use the
	 * general GUC to rate-limit all prefetching.  The queue has space for up
	 * the highest possible value of the GUC + 1, because our circular buffer
	 * has a gap between head and tail when full.
	 */
	prefetcher = palloc0(sizeof(XLogPrefetcher));
	prefetcher->prefetch_queue_size = maintenance_io_concurrency + 1;
	prefetcher->reader = reader;
	prefetcher->filter_table = hash_create("XLogPrefetcherFilterTable", 1024,
										   &hash_table_ctl,
										   HASH_ELEM | HASH_BLOBS);
	dlist_init(&prefetcher->filter_queue);

	SharedStats->queue_depth = 0;
	SharedStats->distance = 0;

	return prefetcher;
}

/*
 * Destroy a prefetcher and release all resources.
 */
void
XLogPrefetcherFree(XLogPrefetcher *prefetcher)
{
	/* Log final statistics. */
	ereport(LOG,
			(errmsg("recovery finished prefetching at %X/%X; "
					"prefetch = " UINT64_FORMAT ", "
					"skip_hit = " UINT64_FORMAT ", "
					"skip_new = " UINT64_FORMAT ", "
					"skip_fpw = " UINT64_FORMAT ", "
					"skip_seq = " UINT64_FORMAT ", "
					"avg_distance = %f, "
					"avg_queue_depth = %f",
					(uint32) (prefetcher->reader->EndRecPtr << 32),
					(uint32) (prefetcher->reader->EndRecPtr),
					pg_atomic_read_u64(&SharedStats->prefetch),
					pg_atomic_read_u64(&SharedStats->skip_hit),
					pg_atomic_read_u64(&SharedStats->skip_new),
					pg_atomic_read_u64(&SharedStats->skip_fpw),
					pg_atomic_read_u64(&SharedStats->skip_seq),
					SharedStats->avg_distance,
					SharedStats->avg_queue_depth)));
	hash_destroy(prefetcher->filter_table);
	pfree(prefetcher);
}

/*
 * Called when recovery is replaying a new LSN, to check if we can read ahead.
 */
bool
XLogPrefetcherReadAhead(XLogPrefetcher *prefetcher, XLogRecPtr replaying_lsn)
{
	uint32		reset_request;

	/* If an error has occurred or we've hit the end of the WAL, do nothing. */
	if (prefetcher->shutdown)
		return false;

	/*
	 * Have any in-flight prefetches definitely completed, judging by the LSN
	 * that is currently being replayed?
	 */
	XLogPrefetcherCompletedIO(prefetcher, replaying_lsn);

	/*
	 * Do we already have the maximum permitted number of I/Os running
	 * (according to the information we have)?  If so, we have to wait for at
	 * least one to complete, so give up early and let recovery catch up.
	 */
	if (XLogPrefetcherSaturated(prefetcher))
		return false;

	/*
	 * Can we drop any filters yet?  This happens when the LSN that is
	 * currently being replayed has moved past a record that prevents
	 * prefetching of a block range, such as relation extension.
	 */
	XLogPrefetcherCompleteFilters(prefetcher, replaying_lsn);

	/*
	 * Have we been asked to reset our stats counters?  This is checked with
	 * an unsynchronized memory read, but we'll see it eventually and we'll be
	 * accessing that cache line anyway.
	 */
	reset_request = pg_atomic_read_u32(&SharedStats->reset_request);
	if (reset_request != SharedStats->reset_handled)
	{
		XLogPrefetchResetStats();
		SharedStats->reset_handled = reset_request;

		prefetcher->avg_distance = 0;
		prefetcher->avg_queue_depth = 0;
		prefetcher->samples = 0;
	}

	/* OK, we can now try reading ahead. */
	return XLogPrefetcherScanRecords(prefetcher, replaying_lsn);
}

/*
 * Read ahead as far as we are allowed to, considering the LSN that recovery
 * is currently replaying.
 *
 * Return true if the xlogreader would like more data.
 */
static bool
XLogPrefetcherScanRecords(XLogPrefetcher *prefetcher, XLogRecPtr replaying_lsn)
{
	XLogReaderState *reader = prefetcher->reader;
	DecodedXLogRecord *record;

	Assert(!XLogPrefetcherSaturated(prefetcher));

	for (;;)
	{
		char	   *error;
		int64		distance;

		/* If we don't already have a record, then try to read one. */
		if (prefetcher->record == NULL)
		{
			switch (XLogReadAhead(reader, &record, &error))
			{
				case XLREAD_NEED_DATA:
					return true;
				case XLREAD_FAIL:
					if (error)
						ereport(LOG,
								(errmsg("recovery no longer prefetching: %s",
										error)));
					else
						ereport(LOG,
								(errmsg("recovery no longer prefetching")));
					prefetcher->shutdown = true;
					SharedStats->queue_depth = 0;
					SharedStats->distance = 0;

					return false;
				case XLREAD_FULL:
					return false;
				case XLREAD_SUCCESS:
					prefetcher->record = record;
					prefetcher->next_block_id = 0;
					break;
			}
		}
		else
		{
			/*
			 * We ran out of I/O queue while part way through a record.  We'll
			 * carry on where we left off, according to next_block_id.
			 */
			record = prefetcher->record;
		}

		/* How far ahead of replay are we now? */
		distance = record->lsn - replaying_lsn;

		/* Update distance shown in shm. */
		SharedStats->distance = distance;

		/* Periodically recompute some statistics. */
		if (unlikely(replaying_lsn >= prefetcher->next_sample_lsn))
		{
			/* Compute online averages. */
			prefetcher->samples++;
			if (prefetcher->samples == 1)
			{
				prefetcher->avg_distance = SharedStats->distance;
				prefetcher->avg_queue_depth = SharedStats->queue_depth;
			}
			else
			{
				prefetcher->avg_distance +=
					(SharedStats->distance - prefetcher->avg_distance) /
					prefetcher->samples;
				prefetcher->avg_queue_depth +=
					(SharedStats->queue_depth - prefetcher->avg_queue_depth) /
					prefetcher->samples;
			}

			/* Expose it in shared memory. */
			SharedStats->avg_distance = prefetcher->avg_distance;
			SharedStats->avg_queue_depth = prefetcher->avg_queue_depth;

			/* Also periodically save the simple counters. */
			XLogPrefetchSaveStats();

			prefetcher->next_sample_lsn =
				replaying_lsn + XLOGPREFETCHER_SAMPLE_DISTANCE;
		}

		/* Are we not far enough ahead? */
		if (distance <= 0)
		{
			/* XXX Is this still possible? */
			prefetcher->record = NULL;	/* skip this record */
			continue;
		}

		/*
		 * If this is a record that creates a new SMGR relation, we'll avoid
		 * prefetching anything from that rnode until it has been replayed.
		 */
		if (replaying_lsn < record->lsn &&
			record->header.xl_rmid == RM_SMGR_ID &&
			(record->header.xl_info & ~XLR_INFO_MASK) == XLOG_SMGR_CREATE)
		{
			xl_smgr_create *xlrec = (xl_smgr_create *) record->main_data;

			XLogPrefetcherAddFilter(prefetcher, xlrec->rnode, 0, record->lsn);
		}

		/* Scan the record's block references. */
		if (!XLogPrefetcherScanBlocks(prefetcher))
			return false;

		/* Advance to the next record. */
		prefetcher->record = NULL;
	}
}

/*
 * Scan the current record for block references, and consider prefetching.
 *
 * Return true if we processed the current record to completion and still have
 * queue space to process a new record, and false if we saturated the I/O
 * queue and need to wait for recovery to advance before we continue.
 */
static bool
XLogPrefetcherScanBlocks(XLogPrefetcher *prefetcher)
{
	DecodedXLogRecord *record = prefetcher->record;

	Assert(!XLogPrefetcherSaturated(prefetcher));

	/*
	 * We might already have been partway through processing this record when
	 * our queue became saturated, so we need to start where we left off.
	 */
	for (int block_id = prefetcher->next_block_id;
		 block_id <= record->max_block_id;
		 ++block_id)
	{
		DecodedBkpBlock *block = &record->blocks[block_id];
		PrefetchBufferResult prefetch;
		SMgrRelation reln;

		/* Ignore everything but the main fork for now. */
		if (block->forknum != MAIN_FORKNUM)
			continue;

		/*
		 * If there is a full page image attached, we won't be reading the
		 * page, so you might think we should skip it.  However, if the
		 * underlying filesystem uses larger logical blocks than us, it might
		 * still need to perform a read-before-write some time later.
		 * Therefore, only prefetch if configured to do so.
		 */
		if (block->has_image && !recovery_prefetch_fpw)
		{
			XLogPrefetchIncrement(&SharedStats->skip_fpw);
			continue;
		}

		/*
		 * If this block will initialize a new page then it's probably a
		 * relation extension.  Since that might create a new segment, we
		 * can't try to prefetch this block until the record has been
		 * replayed, or we might try to open a file that doesn't exist yet.
		 */
		if (block->flags & BKPBLOCK_WILL_INIT)
		{
			XLogPrefetcherAddFilter(prefetcher, block->rnode, block->blkno,
									record->lsn);
			XLogPrefetchIncrement(&SharedStats->skip_new);
			continue;
		}

		/* Should we skip this block due to a filter? */
		if (XLogPrefetcherIsFiltered(prefetcher, block->rnode, block->blkno))
		{
			XLogPrefetchIncrement(&SharedStats->skip_new);
			continue;
		}

		/* Fast path for repeated references to the same relation. */
		if (RelFileNodeEquals(block->rnode, prefetcher->last_rnode))
		{
			/*
			 * If this is a repeat access to the same block, then skip it.
			 *
			 * XXX We could also check for last_blkno + 1 too, and also update
			 * last_blkno; it's not clear if the kernel would do a better job
			 * of sequential prefetching.
			 */
			if (block->blkno == prefetcher->last_blkno)
			{
				XLogPrefetchIncrement(&SharedStats->skip_seq);
				continue;
			}

			/* We can avoid calling smgropen(). */
			reln = prefetcher->last_reln;
		}
		else
		{
			/* Otherwise we have to open it. */
			reln = smgropen(block->rnode, InvalidBackendId);
			prefetcher->last_rnode = block->rnode;
			prefetcher->last_reln = reln;
		}
		prefetcher->last_blkno = block->blkno;

		/* Try to prefetch this block! */
		prefetch = PrefetchSharedBuffer(reln, block->forknum, block->blkno);
		if (BufferIsValid(prefetch.recent_buffer))
		{
			/*
			 * It was already cached, so do nothing.  We'll remember the
			 * buffer, so that recovery can try to avoid looking it up again.
			 */
			block->recent_buffer = prefetch.recent_buffer;
			XLogPrefetchIncrement(&SharedStats->skip_hit);
		}
		else if (prefetch.initiated_io)
		{
			/*
			 * I/O has possibly been initiated (though we don't know if it was
			 * already cached by the kernel, so we just have to assume that it
			 * has due to lack of better information).  Record this as an I/O
			 * in progress until eventually we replay this LSN.
			 */
			XLogPrefetchIncrement(&SharedStats->prefetch);
			XLogPrefetcherInitiatedIO(prefetcher, record->lsn);

			/*
			 * If the queue is now full, we'll have to wait before processing
			 * any more blocks from this record, or move to a new record if
			 * that was the last block.
			 */
			if (XLogPrefetcherSaturated(prefetcher))
			{
				prefetcher->next_block_id = block_id + 1;
				return false;
			}
		}
		else
		{
			/*
			 * Neither cached nor initiated.  The underlying segment file
			 * doesn't exist.  Presumably it will be unlinked by a later WAL
			 * record.  When recovery reads this block, it will use the
			 * EXTENSION_CREATE_RECOVERY flag.  We certainly don't want to do
			 * that sort of thing while merely prefetching, so let's just
			 * ignore references to this relation until this record is
			 * replayed, and let recovery create the dummy file or complain if
			 * something is wrong.
			 */
			XLogPrefetcherAddFilter(prefetcher, block->rnode, 0,
									record->lsn);
			XLogPrefetchIncrement(&SharedStats->skip_new);
		}
	}

	return true;
}

/*
 * Expose statistics about recovery prefetching.
 */
Datum
pg_stat_get_prefetch_recovery(PG_FUNCTION_ARGS)
{
#define PG_STAT_GET_PREFETCH_RECOVERY_COLS 10
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Datum		values[PG_STAT_GET_PREFETCH_RECOVERY_COLS];
	bool		nulls[PG_STAT_GET_PREFETCH_RECOVERY_COLS];

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mod required, but it is not allowed in this context")));

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	if (pg_atomic_read_u32(&SharedStats->reset_request) != SharedStats->reset_handled)
	{
		/* There's an unhandled reset request, so just show NULLs */
		for (int i = 0; i < PG_STAT_GET_PREFETCH_RECOVERY_COLS; ++i)
			nulls[i] = true;
	}
	else
	{
		for (int i = 0; i < PG_STAT_GET_PREFETCH_RECOVERY_COLS; ++i)
			nulls[i] = false;
	}

	values[0] = TimestampTzGetDatum(pg_atomic_read_u64(&SharedStats->reset_time));
	values[1] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->prefetch));
	values[2] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_hit));
	values[3] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_new));
	values[4] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_fpw));
	values[5] = Int64GetDatum(pg_atomic_read_u64(&SharedStats->skip_seq));
	values[6] = Int32GetDatum(SharedStats->distance);
	values[7] = Int32GetDatum(SharedStats->queue_depth);
	values[8] = Float4GetDatum(SharedStats->avg_distance);
	values[9] = Float4GetDatum(SharedStats->avg_queue_depth);
	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	tuplestore_donestoring(tupstore);

	return (Datum) 0;
}

/*
 * Compute (n + 1) % prefetch_queue_size, assuming n < prefetch_queue_size,
 * without using division.
 */
static inline int
XLogPrefetcherNext(XLogPrefetcher *prefetcher, int n)
{
	int			next = n + 1;

	return next == prefetcher->prefetch_queue_size ? 0 : next;
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
		 * to cover this WAL record, but leave the (presumably lower) block
		 * number there because we don't want to have to track individual
		 * blocks.
		 */
		filter->filter_until_replayed = lsn;
		dlist_delete(&filter->link);
		dlist_push_head(&prefetcher->filter_queue, &filter->link);
	}
}

/*
 * Have we replayed the records that caused us to begin filtering a block
 * range?  That means that relations should have been created, extended or
 * dropped as required, so we can drop relevant filters.
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
		XLogPrefetcherFilter *filter = hash_search(prefetcher->filter_table, &rnode,
												   HASH_FIND, NULL);

		if (filter && filter->filter_from_block <= blockno)
			return true;
	}

	return false;
}

/*
 * Insert an LSN into the queue.  The queue must not be full already.  This
 * tracks the fact that we have (to the best of our knowledge) initiated an
 * I/O, so that we can impose a cap on concurrent prefetching.
 */
static inline void
XLogPrefetcherInitiatedIO(XLogPrefetcher *prefetcher,
						  XLogRecPtr prefetching_lsn)
{
	Assert(!XLogPrefetcherSaturated(prefetcher));
	prefetcher->prefetch_queue[prefetcher->prefetch_head] = prefetching_lsn;
	prefetcher->prefetch_head =
		XLogPrefetcherNext(prefetcher, prefetcher->prefetch_head);
	SharedStats->queue_depth++;

	Assert(SharedStats->queue_depth <= prefetcher->prefetch_queue_size);
}

/*
 * Have we replayed the records that caused us to initiate the oldest
 * prefetches yet?  That means that they're definitely finished, so we can can
 * forget about them and allow ourselves to initiate more prefetches.  For now
 * we don't have any awareness of when I/O really completes.
 */
static inline void
XLogPrefetcherCompletedIO(XLogPrefetcher *prefetcher, XLogRecPtr replaying_lsn)
{
	while (prefetcher->prefetch_head != prefetcher->prefetch_tail &&
		   prefetcher->prefetch_queue[prefetcher->prefetch_tail] < replaying_lsn)
	{
		prefetcher->prefetch_tail =
			XLogPrefetcherNext(prefetcher, prefetcher->prefetch_tail);
		SharedStats->queue_depth--;

		Assert(SharedStats->queue_depth >= 0);
	}
}

/*
 * Check if the maximum allowed number of I/Os is already in flight.
 */
static inline bool
XLogPrefetcherSaturated(XLogPrefetcher *prefetcher)
{
	int			next = XLogPrefetcherNext(prefetcher, prefetcher->prefetch_head);

	return next == prefetcher->prefetch_tail;
}

void
assign_recovery_prefetch(bool new_value, void *extra)
{
	/* Reconfigure prefetching, because a setting it depends on changed. */
	recovery_prefetch = new_value;
	if (AmStartupProcess())
		XLogPrefetchReconfigure();
}

void
assign_recovery_prefetch_fpw(bool new_value, void *extra)
{
	/* Reconfigure prefetching, because a setting it depends on changed. */
	recovery_prefetch_fpw = new_value;
	if (AmStartupProcess())
		XLogPrefetchReconfigure();
}
