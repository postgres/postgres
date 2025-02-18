/*-------------------------------------------------------------------------
 *
 * read_stream.c
 *	  Mechanism for accessing buffered relation data with look-ahead
 *
 * Code that needs to access relation data typically pins blocks one at a
 * time, often in a predictable order that might be sequential or data-driven.
 * Calling the simple ReadBuffer() function for each block is inefficient,
 * because blocks that are not yet in the buffer pool require I/O operations
 * that are small and might stall waiting for storage.  This mechanism looks
 * into the future and calls StartReadBuffers() and WaitReadBuffers() to read
 * neighboring blocks together and ahead of time, with an adaptive look-ahead
 * distance.
 *
 * A user-provided callback generates a stream of block numbers that is used
 * to form reads of up to io_combine_limit, by attempting to merge them with a
 * pending read.  When that isn't possible, the existing pending read is sent
 * to StartReadBuffers() so that a new one can begin to form.
 *
 * The algorithm for controlling the look-ahead distance tries to classify the
 * stream into three ideal behaviors:
 *
 * A) No I/O is necessary, because the requested blocks are fully cached
 * already.  There is no benefit to looking ahead more than one block, so
 * distance is 1.  This is the default initial assumption.
 *
 * B) I/O is necessary, but read-ahead advice is undesirable because the
 * access is sequential and we can rely on the kernel's read-ahead heuristics,
 * or impossible because direct I/O is enabled, or the system doesn't support
 * read-ahead advice.  There is no benefit in looking ahead more than
 * io_combine_limit, because in this case the only goal is larger read system
 * calls.  Looking further ahead would pin many buffers and perform
 * speculative work for no benefit.
 *
 * C) I/O is necessary, it appears to be random, and this system supports
 * read-ahead advice.  We'll look further ahead in order to reach the
 * configured level of I/O concurrency.
 *
 * The distance increases rapidly and decays slowly, so that it moves towards
 * those levels as different I/O patterns are discovered.  For example, a
 * sequential scan of fully cached data doesn't bother looking ahead, but a
 * sequential scan that hits a region of uncached blocks will start issuing
 * increasingly wide read calls until it plateaus at io_combine_limit.
 *
 * The main data structure is a circular queue of buffers of size
 * max_pinned_buffers plus some extra space for technical reasons, ready to be
 * returned by read_stream_next_buffer().  Each buffer also has an optional
 * variable sized object that is passed from the callback to the consumer of
 * buffers.
 *
 * Parallel to the queue of buffers, there is a circular queue of in-progress
 * I/Os that have been started with StartReadBuffers(), and for which
 * WaitReadBuffers() must be called before returning the buffer.
 *
 * For example, if the callback returns block numbers 10, 42, 43, 44, 60 in
 * successive calls, then these data structures might appear as follows:
 *
 *                          buffers buf/data       ios
 *
 *                          +----+  +-----+       +--------+
 *                          |    |  |     |  +----+ 42..44 | <- oldest_io_index
 *                          +----+  +-----+  |    +--------+
 *   oldest_buffer_index -> | 10 |  |  ?  |  | +--+ 60..60 |
 *                          +----+  +-----+  | |  +--------+
 *                          | 42 |  |  ?  |<-+ |  |        | <- next_io_index
 *                          +----+  +-----+    |  +--------+
 *                          | 43 |  |  ?  |    |  |        |
 *                          +----+  +-----+    |  +--------+
 *                          | 44 |  |  ?  |    |  |        |
 *                          +----+  +-----+    |  +--------+
 *                          | 60 |  |  ?  |<---+
 *                          +----+  +-----+
 *     next_buffer_index -> |    |  |     |
 *                          +----+  +-----+
 *
 * In the example, 5 buffers are pinned, and the next buffer to be streamed to
 * the client is block 10.  Block 10 was a hit and has no associated I/O, but
 * the range 42..44 requires an I/O wait before its buffers are returned, as
 * does block 60.
 *
 *
 * Portions Copyright (c) 2024-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/storage/aio/read_stream.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "storage/fd.h"
#include "storage/smgr.h"
#include "storage/read_stream.h"
#include "utils/memdebug.h"
#include "utils/rel.h"
#include "utils/spccache.h"

typedef struct InProgressIO
{
	int16		buffer_index;
	ReadBuffersOperation op;
} InProgressIO;

/*
 * State for managing a stream of reads.
 */
struct ReadStream
{
	int16		max_ios;
	int16		ios_in_progress;
	int16		queue_size;
	int16		max_pinned_buffers;
	int16		pinned_buffers;
	int16		distance;
	bool		advice_enabled;

	/*
	 * One-block buffer to support 'ungetting' a block number, to resolve flow
	 * control problems when I/Os are split.
	 */
	BlockNumber buffered_blocknum;

	/*
	 * The callback that will tell us which block numbers to read, and an
	 * opaque pointer that will be pass to it for its own purposes.
	 */
	ReadStreamBlockNumberCB callback;
	void	   *callback_private_data;

	/* Next expected block, for detecting sequential access. */
	BlockNumber seq_blocknum;

	/* The read operation we are currently preparing. */
	BlockNumber pending_read_blocknum;
	int16		pending_read_nblocks;

	/* Space for buffers and optional per-buffer private data. */
	size_t		per_buffer_data_size;
	void	   *per_buffer_data;

	/* Read operations that have been started but not waited for yet. */
	InProgressIO *ios;
	int16		oldest_io_index;
	int16		next_io_index;

	bool		fast_path;

	/* Circular queue of buffers. */
	int16		oldest_buffer_index;	/* Next pinned buffer to return */
	int16		next_buffer_index;	/* Index of next buffer to pin */
	Buffer		buffers[FLEXIBLE_ARRAY_MEMBER];
};

/*
 * Return a pointer to the per-buffer data by index.
 */
static inline void *
get_per_buffer_data(ReadStream *stream, int16 buffer_index)
{
	return (char *) stream->per_buffer_data +
		stream->per_buffer_data_size * buffer_index;
}

/*
 * General-use ReadStreamBlockNumberCB for block range scans.  Loops over the
 * blocks [current_blocknum, last_exclusive).
 */
BlockNumber
block_range_read_stream_cb(ReadStream *stream,
						   void *callback_private_data,
						   void *per_buffer_data)
{
	BlockRangeReadStreamPrivate *p = callback_private_data;

	if (p->current_blocknum < p->last_exclusive)
		return p->current_blocknum++;

	return InvalidBlockNumber;
}

/*
 * Ask the callback which block it would like us to read next, with a one block
 * buffer in front to allow read_stream_unget_block() to work.
 */
static inline BlockNumber
read_stream_get_block(ReadStream *stream, void *per_buffer_data)
{
	BlockNumber blocknum;

	blocknum = stream->buffered_blocknum;
	if (blocknum != InvalidBlockNumber)
		stream->buffered_blocknum = InvalidBlockNumber;
	else
	{
		/*
		 * Tell Valgrind that the per-buffer data is undefined.  That replaces
		 * the "noaccess" state that was set when the consumer moved past this
		 * entry last time around the queue, and should also catch callbacks
		 * that fail to initialize data that the buffer consumer later
		 * accesses.  On the first go around, it is undefined already.
		 */
		VALGRIND_MAKE_MEM_UNDEFINED(per_buffer_data,
									stream->per_buffer_data_size);
		blocknum = stream->callback(stream,
									stream->callback_private_data,
									per_buffer_data);
	}

	return blocknum;
}

/*
 * In order to deal with short reads in StartReadBuffers(), we sometimes need
 * to defer handling of a block until later.
 */
static inline void
read_stream_unget_block(ReadStream *stream, BlockNumber blocknum)
{
	/* We shouldn't ever unget more than one block. */
	Assert(stream->buffered_blocknum == InvalidBlockNumber);
	Assert(blocknum != InvalidBlockNumber);
	stream->buffered_blocknum = blocknum;
}

static void
read_stream_start_pending_read(ReadStream *stream, bool suppress_advice)
{
	bool		need_wait;
	int			nblocks;
	int			flags;
	int16		io_index;
	int16		overflow;
	int16		buffer_index;

	/* This should only be called with a pending read. */
	Assert(stream->pending_read_nblocks > 0);
	Assert(stream->pending_read_nblocks <= io_combine_limit);

	/* We had better not exceed the pin limit by starting this read. */
	Assert(stream->pinned_buffers + stream->pending_read_nblocks <=
		   stream->max_pinned_buffers);

	/* We had better not be overwriting an existing pinned buffer. */
	if (stream->pinned_buffers > 0)
		Assert(stream->next_buffer_index != stream->oldest_buffer_index);
	else
		Assert(stream->next_buffer_index == stream->oldest_buffer_index);

	/*
	 * If advice hasn't been suppressed, this system supports it, and this
	 * isn't a strictly sequential pattern, then we'll issue advice.
	 */
	if (!suppress_advice &&
		stream->advice_enabled &&
		stream->pending_read_blocknum != stream->seq_blocknum)
		flags = READ_BUFFERS_ISSUE_ADVICE;
	else
		flags = 0;

	/* We say how many blocks we want to read, but may be smaller on return. */
	buffer_index = stream->next_buffer_index;
	io_index = stream->next_io_index;
	nblocks = stream->pending_read_nblocks;
	need_wait = StartReadBuffers(&stream->ios[io_index].op,
								 &stream->buffers[buffer_index],
								 stream->pending_read_blocknum,
								 &nblocks,
								 flags);
	stream->pinned_buffers += nblocks;

	/* Remember whether we need to wait before returning this buffer. */
	if (!need_wait)
	{
		/* Look-ahead distance decays, no I/O necessary (behavior A). */
		if (stream->distance > 1)
			stream->distance--;
	}
	else
	{
		/*
		 * Remember to call WaitReadBuffers() before returning head buffer.
		 * Look-ahead distance will be adjusted after waiting.
		 */
		stream->ios[io_index].buffer_index = buffer_index;
		if (++stream->next_io_index == stream->max_ios)
			stream->next_io_index = 0;
		Assert(stream->ios_in_progress < stream->max_ios);
		stream->ios_in_progress++;
		stream->seq_blocknum = stream->pending_read_blocknum + nblocks;
	}

	/*
	 * We gave a contiguous range of buffer space to StartReadBuffers(), but
	 * we want it to wrap around at queue_size.  Slide overflowing buffers to
	 * the front of the array.
	 */
	overflow = (buffer_index + nblocks) - stream->queue_size;
	if (overflow > 0)
		memmove(&stream->buffers[0],
				&stream->buffers[stream->queue_size],
				sizeof(stream->buffers[0]) * overflow);

	/* Compute location of start of next read, without using % operator. */
	buffer_index += nblocks;
	if (buffer_index >= stream->queue_size)
		buffer_index -= stream->queue_size;
	Assert(buffer_index >= 0 && buffer_index < stream->queue_size);
	stream->next_buffer_index = buffer_index;

	/* Adjust the pending read to cover the remaining portion, if any. */
	stream->pending_read_blocknum += nblocks;
	stream->pending_read_nblocks -= nblocks;
}

static void
read_stream_look_ahead(ReadStream *stream, bool suppress_advice)
{
	while (stream->ios_in_progress < stream->max_ios &&
		   stream->pinned_buffers + stream->pending_read_nblocks < stream->distance)
	{
		BlockNumber blocknum;
		int16		buffer_index;
		void	   *per_buffer_data;

		if (stream->pending_read_nblocks == io_combine_limit)
		{
			read_stream_start_pending_read(stream, suppress_advice);
			suppress_advice = false;
			continue;
		}

		/*
		 * See which block the callback wants next in the stream.  We need to
		 * compute the index of the Nth block of the pending read including
		 * wrap-around, but we don't want to use the expensive % operator.
		 */
		buffer_index = stream->next_buffer_index + stream->pending_read_nblocks;
		if (buffer_index >= stream->queue_size)
			buffer_index -= stream->queue_size;
		Assert(buffer_index >= 0 && buffer_index < stream->queue_size);
		per_buffer_data = get_per_buffer_data(stream, buffer_index);
		blocknum = read_stream_get_block(stream, per_buffer_data);
		if (blocknum == InvalidBlockNumber)
		{
			/* End of stream. */
			stream->distance = 0;
			break;
		}

		/* Can we merge it with the pending read? */
		if (stream->pending_read_nblocks > 0 &&
			stream->pending_read_blocknum + stream->pending_read_nblocks == blocknum)
		{
			stream->pending_read_nblocks++;
			continue;
		}

		/* We have to start the pending read before we can build another. */
		while (stream->pending_read_nblocks > 0)
		{
			read_stream_start_pending_read(stream, suppress_advice);
			suppress_advice = false;
			if (stream->ios_in_progress == stream->max_ios)
			{
				/* And we've hit the limit.  Rewind, and stop here. */
				read_stream_unget_block(stream, blocknum);
				return;
			}
		}

		/* This is the start of a new pending read. */
		stream->pending_read_blocknum = blocknum;
		stream->pending_read_nblocks = 1;
	}

	/*
	 * We don't start the pending read just because we've hit the distance
	 * limit, preferring to give it another chance to grow to full
	 * io_combine_limit size once more buffers have been consumed.  However,
	 * if we've already reached io_combine_limit, or we've reached the
	 * distance limit and there isn't anything pinned yet, or the callback has
	 * signaled end-of-stream, we start the read immediately.
	 */
	if (stream->pending_read_nblocks > 0 &&
		(stream->pending_read_nblocks == io_combine_limit ||
		 (stream->pending_read_nblocks == stream->distance &&
		  stream->pinned_buffers == 0) ||
		 stream->distance == 0) &&
		stream->ios_in_progress < stream->max_ios)
		read_stream_start_pending_read(stream, suppress_advice);
}

/*
 * Create a new read stream object that can be used to perform the equivalent
 * of a series of ReadBuffer() calls for one fork of one relation.
 * Internally, it generates larger vectored reads where possible by looking
 * ahead.  The callback should return block numbers or InvalidBlockNumber to
 * signal end-of-stream, and if per_buffer_data_size is non-zero, it may also
 * write extra data for each block into the space provided to it.  It will
 * also receive callback_private_data for its own purposes.
 */
static ReadStream *
read_stream_begin_impl(int flags,
					   BufferAccessStrategy strategy,
					   Relation rel,
					   SMgrRelation smgr,
					   char persistence,
					   ForkNumber forknum,
					   ReadStreamBlockNumberCB callback,
					   void *callback_private_data,
					   size_t per_buffer_data_size)
{
	ReadStream *stream;
	size_t		size;
	int16		queue_size;
	int			max_ios;
	int			strategy_pin_limit;
	uint32		max_pinned_buffers;
	Oid			tablespace_id;

	/*
	 * Decide how many I/Os we will allow to run at the same time.  That
	 * currently means advice to the kernel to tell it that we will soon read.
	 * This number also affects how far we look ahead for opportunities to
	 * start more I/Os.
	 */
	tablespace_id = smgr->smgr_rlocator.locator.spcOid;
	if (!OidIsValid(MyDatabaseId) ||
		(rel && IsCatalogRelation(rel)) ||
		IsCatalogRelationOid(smgr->smgr_rlocator.locator.relNumber))
	{
		/*
		 * Avoid circularity while trying to look up tablespace settings or
		 * before spccache.c is ready.
		 */
		max_ios = effective_io_concurrency;
	}
	else if (flags & READ_STREAM_MAINTENANCE)
		max_ios = get_tablespace_maintenance_io_concurrency(tablespace_id);
	else
		max_ios = get_tablespace_io_concurrency(tablespace_id);

	/* Cap to INT16_MAX to avoid overflowing below */
	max_ios = Min(max_ios, PG_INT16_MAX);

	/*
	 * Choose the maximum number of buffers we're prepared to pin.  We try to
	 * pin fewer if we can, though.  We clamp it to at least io_combine_limit
	 * so that we can have a chance to build up a full io_combine_limit sized
	 * read, even when max_ios is zero.  Be careful not to allow int16 to
	 * overflow (even though that's not possible with the current GUC range
	 * limits), allowing also for the spare entry and the overflow space.
	 */
	max_pinned_buffers = Max(max_ios * 4, io_combine_limit);
	max_pinned_buffers = Min(max_pinned_buffers,
							 PG_INT16_MAX - io_combine_limit - 1);

	/* Give the strategy a chance to limit the number of buffers we pin. */
	strategy_pin_limit = GetAccessStrategyPinLimit(strategy);
	max_pinned_buffers = Min(strategy_pin_limit, max_pinned_buffers);

	/* Don't allow this backend to pin more than its share of buffers. */
	if (SmgrIsTemp(smgr))
		LimitAdditionalLocalPins(&max_pinned_buffers);
	else
		LimitAdditionalPins(&max_pinned_buffers);
	Assert(max_pinned_buffers > 0);

	/*
	 * We need one extra entry for buffers and per-buffer data, because users
	 * of per-buffer data have access to the object until the next call to
	 * read_stream_next_buffer(), so we need a gap between the head and tail
	 * of the queue so that we don't clobber it.
	 */
	queue_size = max_pinned_buffers + 1;

	/*
	 * Allocate the object, the buffers, the ios and per_buffer_data space in
	 * one big chunk.  Though we have queue_size buffers, we want to be able
	 * to assume that all the buffers for a single read are contiguous (i.e.
	 * don't wrap around halfway through), so we allow temporary overflows of
	 * up to the maximum possible read size by allocating an extra
	 * io_combine_limit - 1 elements.
	 */
	size = offsetof(ReadStream, buffers);
	size += sizeof(Buffer) * (queue_size + io_combine_limit - 1);
	size += sizeof(InProgressIO) * Max(1, max_ios);
	size += per_buffer_data_size * queue_size;
	size += MAXIMUM_ALIGNOF * 2;
	stream = (ReadStream *) palloc(size);
	memset(stream, 0, offsetof(ReadStream, buffers));
	stream->ios = (InProgressIO *)
		MAXALIGN(&stream->buffers[queue_size + io_combine_limit - 1]);
	if (per_buffer_data_size > 0)
		stream->per_buffer_data = (void *)
			MAXALIGN(&stream->ios[Max(1, max_ios)]);

#ifdef USE_PREFETCH

	/*
	 * This system supports prefetching advice.  We can use it as long as
	 * direct I/O isn't enabled, the caller hasn't promised sequential access
	 * (overriding our detection heuristics), and max_ios hasn't been set to
	 * zero.
	 */
	if ((io_direct_flags & IO_DIRECT_DATA) == 0 &&
		(flags & READ_STREAM_SEQUENTIAL) == 0 &&
		max_ios > 0)
		stream->advice_enabled = true;
#endif

	/*
	 * For now, max_ios = 0 is interpreted as max_ios = 1 with advice disabled
	 * above.  If we had real asynchronous I/O we might need a slightly
	 * different definition.
	 */
	if (max_ios == 0)
		max_ios = 1;

	stream->max_ios = max_ios;
	stream->per_buffer_data_size = per_buffer_data_size;
	stream->max_pinned_buffers = max_pinned_buffers;
	stream->queue_size = queue_size;
	stream->callback = callback;
	stream->callback_private_data = callback_private_data;
	stream->buffered_blocknum = InvalidBlockNumber;

	/*
	 * Skip the initial ramp-up phase if the caller says we're going to be
	 * reading the whole relation.  This way we start out assuming we'll be
	 * doing full io_combine_limit sized reads (behavior B).
	 */
	if (flags & READ_STREAM_FULL)
		stream->distance = Min(max_pinned_buffers, io_combine_limit);
	else
		stream->distance = 1;

	/*
	 * Since we always access the same relation, we can initialize parts of
	 * the ReadBuffersOperation objects and leave them that way, to avoid
	 * wasting CPU cycles writing to them for each read.
	 */
	for (int i = 0; i < max_ios; ++i)
	{
		stream->ios[i].op.rel = rel;
		stream->ios[i].op.smgr = smgr;
		stream->ios[i].op.persistence = persistence;
		stream->ios[i].op.forknum = forknum;
		stream->ios[i].op.strategy = strategy;
	}

	return stream;
}

/*
 * Create a new read stream for reading a relation.
 * See read_stream_begin_impl() for the detailed explanation.
 */
ReadStream *
read_stream_begin_relation(int flags,
						   BufferAccessStrategy strategy,
						   Relation rel,
						   ForkNumber forknum,
						   ReadStreamBlockNumberCB callback,
						   void *callback_private_data,
						   size_t per_buffer_data_size)
{
	return read_stream_begin_impl(flags,
								  strategy,
								  rel,
								  RelationGetSmgr(rel),
								  rel->rd_rel->relpersistence,
								  forknum,
								  callback,
								  callback_private_data,
								  per_buffer_data_size);
}

/*
 * Create a new read stream for reading a SMgr relation.
 * See read_stream_begin_impl() for the detailed explanation.
 */
ReadStream *
read_stream_begin_smgr_relation(int flags,
								BufferAccessStrategy strategy,
								SMgrRelation smgr,
								char smgr_persistence,
								ForkNumber forknum,
								ReadStreamBlockNumberCB callback,
								void *callback_private_data,
								size_t per_buffer_data_size)
{
	return read_stream_begin_impl(flags,
								  strategy,
								  NULL,
								  smgr,
								  smgr_persistence,
								  forknum,
								  callback,
								  callback_private_data,
								  per_buffer_data_size);
}

/*
 * Pull one pinned buffer out of a stream.  Each call returns successive
 * blocks in the order specified by the callback.  If per_buffer_data_size was
 * set to a non-zero size, *per_buffer_data receives a pointer to the extra
 * per-buffer data that the callback had a chance to populate, which remains
 * valid until the next call to read_stream_next_buffer().  When the stream
 * runs out of data, InvalidBuffer is returned.  The caller may decide to end
 * the stream early at any time by calling read_stream_end().
 */
Buffer
read_stream_next_buffer(ReadStream *stream, void **per_buffer_data)
{
	Buffer		buffer;
	int16		oldest_buffer_index;

#ifndef READ_STREAM_DISABLE_FAST_PATH

	/*
	 * A fast path for all-cached scans (behavior A).  This is the same as the
	 * usual algorithm, but it is specialized for no I/O and no per-buffer
	 * data, so we can skip the queue management code, stay in the same buffer
	 * slot and use singular StartReadBuffer().
	 */
	if (likely(stream->fast_path))
	{
		BlockNumber next_blocknum;

		/* Fast path assumptions. */
		Assert(stream->ios_in_progress == 0);
		Assert(stream->pinned_buffers == 1);
		Assert(stream->distance == 1);
		Assert(stream->pending_read_nblocks == 0);
		Assert(stream->per_buffer_data_size == 0);

		/* We're going to return the buffer we pinned last time. */
		oldest_buffer_index = stream->oldest_buffer_index;
		Assert((oldest_buffer_index + 1) % stream->queue_size ==
			   stream->next_buffer_index);
		buffer = stream->buffers[oldest_buffer_index];
		Assert(buffer != InvalidBuffer);

		/* Choose the next block to pin. */
		next_blocknum = read_stream_get_block(stream, NULL);

		if (likely(next_blocknum != InvalidBlockNumber))
		{
			/*
			 * Pin a buffer for the next call.  Same buffer entry, and
			 * arbitrary I/O entry (they're all free).  We don't have to
			 * adjust pinned_buffers because we're transferring one to caller
			 * but pinning one more.
			 */
			if (likely(!StartReadBuffer(&stream->ios[0].op,
										&stream->buffers[oldest_buffer_index],
										next_blocknum,
										stream->advice_enabled ?
										READ_BUFFERS_ISSUE_ADVICE : 0)))
			{
				/* Fast return. */
				return buffer;
			}

			/* Next call must wait for I/O for the newly pinned buffer. */
			stream->oldest_io_index = 0;
			stream->next_io_index = stream->max_ios > 1 ? 1 : 0;
			stream->ios_in_progress = 1;
			stream->ios[0].buffer_index = oldest_buffer_index;
			stream->seq_blocknum = next_blocknum + 1;
		}
		else
		{
			/* No more blocks, end of stream. */
			stream->distance = 0;
			stream->oldest_buffer_index = stream->next_buffer_index;
			stream->pinned_buffers = 0;
		}

		stream->fast_path = false;
		return buffer;
	}
#endif

	if (unlikely(stream->pinned_buffers == 0))
	{
		Assert(stream->oldest_buffer_index == stream->next_buffer_index);

		/* End of stream reached?  */
		if (stream->distance == 0)
			return InvalidBuffer;

		/*
		 * The usual order of operations is that we look ahead at the bottom
		 * of this function after potentially finishing an I/O and making
		 * space for more, but if we're just starting up we'll need to crank
		 * the handle to get started.
		 */
		read_stream_look_ahead(stream, true);

		/* End of stream reached? */
		if (stream->pinned_buffers == 0)
		{
			Assert(stream->distance == 0);
			return InvalidBuffer;
		}
	}

	/* Grab the oldest pinned buffer and associated per-buffer data. */
	Assert(stream->pinned_buffers > 0);
	oldest_buffer_index = stream->oldest_buffer_index;
	Assert(oldest_buffer_index >= 0 &&
		   oldest_buffer_index < stream->queue_size);
	buffer = stream->buffers[oldest_buffer_index];
	if (per_buffer_data)
		*per_buffer_data = get_per_buffer_data(stream, oldest_buffer_index);

	Assert(BufferIsValid(buffer));

	/* Do we have to wait for an associated I/O first? */
	if (stream->ios_in_progress > 0 &&
		stream->ios[stream->oldest_io_index].buffer_index == oldest_buffer_index)
	{
		int16		io_index = stream->oldest_io_index;
		int16		distance;

		/* Sanity check that we still agree on the buffers. */
		Assert(stream->ios[io_index].op.buffers ==
			   &stream->buffers[oldest_buffer_index]);

		WaitReadBuffers(&stream->ios[io_index].op);

		Assert(stream->ios_in_progress > 0);
		stream->ios_in_progress--;
		if (++stream->oldest_io_index == stream->max_ios)
			stream->oldest_io_index = 0;

		if (stream->ios[io_index].op.flags & READ_BUFFERS_ISSUE_ADVICE)
		{
			/* Distance ramps up fast (behavior C). */
			distance = stream->distance * 2;
			distance = Min(distance, stream->max_pinned_buffers);
			stream->distance = distance;
		}
		else
		{
			/* No advice; move towards io_combine_limit (behavior B). */
			if (stream->distance > io_combine_limit)
			{
				stream->distance--;
			}
			else
			{
				distance = stream->distance * 2;
				distance = Min(distance, io_combine_limit);
				distance = Min(distance, stream->max_pinned_buffers);
				stream->distance = distance;
			}
		}
	}

#ifdef CLOBBER_FREED_MEMORY
	/* Clobber old buffer for debugging purposes. */
	stream->buffers[oldest_buffer_index] = InvalidBuffer;
#endif

#if defined(CLOBBER_FREED_MEMORY) || defined(USE_VALGRIND)

	/*
	 * The caller will get access to the per-buffer data, until the next call.
	 * We wipe the one before, which is never occupied because queue_size
	 * allowed one extra element.  This will hopefully trip up client code
	 * that is holding a dangling pointer to it.
	 */
	if (stream->per_buffer_data)
	{
		void	   *per_buffer_data;

		per_buffer_data = get_per_buffer_data(stream,
											  oldest_buffer_index == 0 ?
											  stream->queue_size - 1 :
											  oldest_buffer_index - 1);

#if defined(CLOBBER_FREED_MEMORY)
		/* This also tells Valgrind the memory is "noaccess". */
		wipe_mem(per_buffer_data, stream->per_buffer_data_size);
#elif defined(USE_VALGRIND)
		/* Tell it ourselves. */
		VALGRIND_MAKE_MEM_NOACCESS(per_buffer_data,
								   stream->per_buffer_data_size);
#endif
	}
#endif

	/* Pin transferred to caller. */
	Assert(stream->pinned_buffers > 0);
	stream->pinned_buffers--;

	/* Advance oldest buffer, with wrap-around. */
	stream->oldest_buffer_index++;
	if (stream->oldest_buffer_index == stream->queue_size)
		stream->oldest_buffer_index = 0;

	/* Prepare for the next call. */
	read_stream_look_ahead(stream, false);

#ifndef READ_STREAM_DISABLE_FAST_PATH
	/* See if we can take the fast path for all-cached scans next time. */
	if (stream->ios_in_progress == 0 &&
		stream->pinned_buffers == 1 &&
		stream->distance == 1 &&
		stream->pending_read_nblocks == 0 &&
		stream->per_buffer_data_size == 0)
	{
		stream->fast_path = true;
	}
#endif

	return buffer;
}

/*
 * Transitional support for code that would like to perform or skip reads
 * itself, without using the stream.  Returns, and consumes, the next block
 * number that would be read by the stream's look-ahead algorithm, or
 * InvalidBlockNumber if the end of the stream is reached.  Also reports the
 * strategy that would be used to read it.
 */
BlockNumber
read_stream_next_block(ReadStream *stream, BufferAccessStrategy *strategy)
{
	*strategy = stream->ios[0].op.strategy;
	return read_stream_get_block(stream, NULL);
}

/*
 * Reset a read stream by releasing any queued up buffers, allowing the stream
 * to be used again for different blocks.  This can be used to clear an
 * end-of-stream condition and start again, or to throw away blocks that were
 * speculatively read and read some different blocks instead.
 */
void
read_stream_reset(ReadStream *stream)
{
	Buffer		buffer;

	/* Stop looking ahead. */
	stream->distance = 0;

	/* Forget buffered block number and fast path state. */
	stream->buffered_blocknum = InvalidBlockNumber;
	stream->fast_path = false;

	/* Unpin anything that wasn't consumed. */
	while ((buffer = read_stream_next_buffer(stream, NULL)) != InvalidBuffer)
		ReleaseBuffer(buffer);

	Assert(stream->pinned_buffers == 0);
	Assert(stream->ios_in_progress == 0);

	/* Start off assuming data is cached. */
	stream->distance = 1;
}

/*
 * Release and free a read stream.
 */
void
read_stream_end(ReadStream *stream)
{
	read_stream_reset(stream);
	pfree(stream);
}
