/*-------------------------------------------------------------------------
 *
 * logtape.c
 *	  Management of "logical tapes" within temporary files.
 *
 * This module exists to support sorting via multiple merge passes (see
 * tuplesort.c).  Merging is an ideal algorithm for tape devices, but if
 * we implement it on disk by creating a separate file for each "tape",
 * there is an annoying problem: the peak space usage is at least twice
 * the volume of actual data to be sorted.  (This must be so because each
 * datum will appear in both the input and output tapes of the final
 * merge pass.  For seven-tape polyphase merge, which is otherwise a
 * pretty good algorithm, peak usage is more like 4x actual data volume.)
 *
 * We can work around this problem by recognizing that any one tape
 * dataset (with the possible exception of the final output) is written
 * and read exactly once in a perfectly sequential manner.  Therefore,
 * a datum once read will not be required again, and we can recycle its
 * space for use by the new tape dataset(s) being generated.  In this way,
 * the total space usage is essentially just the actual data volume, plus
 * insignificant bookkeeping and start/stop overhead.
 *
 * Few OSes allow arbitrary parts of a file to be released back to the OS,
 * so we have to implement this space-recycling ourselves within a single
 * logical file.  logtape.c exists to perform this bookkeeping and provide
 * the illusion of N independent tape devices to tuplesort.c.  Note that
 * logtape.c itself depends on buffile.c to provide a "logical file" of
 * larger size than the underlying OS may support.
 *
 * For simplicity, we allocate and release space in the underlying file
 * in BLCKSZ-size blocks.  Space allocation boils down to keeping track
 * of which blocks in the underlying file belong to which logical tape,
 * plus any blocks that are free (recycled and not yet reused).
 * The blocks in each logical tape form a chain, with a prev- and next-
 * pointer in each block.
 *
 * The initial write pass is guaranteed to fill the underlying file
 * perfectly sequentially, no matter how data is divided into logical tapes.
 * Once we begin merge passes, the access pattern becomes considerably
 * less predictable --- but the seeking involved should be comparable to
 * what would happen if we kept each logical tape in a separate file,
 * so there's no serious performance penalty paid to obtain the space
 * savings of recycling.  We try to localize the write accesses by always
 * writing to the lowest-numbered free block when we have a choice; it's
 * not clear this helps much, but it can't hurt.  (XXX perhaps a LIFO
 * policy for free blocks would be better?)
 *
 * To further make the I/Os more sequential, we can use a larger buffer
 * when reading, and read multiple blocks from the same tape in one go,
 * whenever the buffer becomes empty.  LogicalTapeAssignReadBufferSize()
 * can be used to set the size of the read buffer.
 *
 * To support the above policy of writing to the lowest free block,
 * ltsGetFreeBlock sorts the list of free block numbers into decreasing
 * order each time it is asked for a block and the list isn't currently
 * sorted.  This is an efficient way to handle it because we expect cycles
 * of releasing many blocks followed by re-using many blocks, due to
 * the larger read buffer.
 *
 * Since all the bookkeeping and buffer memory is allocated with palloc(),
 * and the underlying file(s) are made with OpenTemporaryFile, all resources
 * for a logical tape set are certain to be cleaned up even if processing
 * is aborted by ereport(ERROR).  To avoid confusion, the caller should take
 * care that all calls for a single LogicalTapeSet are made in the same
 * palloc context.
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/logtape.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "storage/buffile.h"
#include "utils/logtape.h"
#include "utils/memutils.h"

/*
 * A TapeBlockTrailer is stored at the end of each BLCKSZ block.
 *
 * The first block of a tape has prev == -1.  The last block of a tape
 * stores the number of valid bytes on the block, inverted, in 'next'
 * Therefore next < 0 indicates the last block.
 */
typedef struct TapeBlockTrailer
{
	long		prev;			/* previous block on this tape, or -1 on first
								 * block */
	long		next;			/* next block on this tape, or # of valid
								 * bytes on last block (if < 0) */
} TapeBlockTrailer;

#define TapeBlockPayloadSize  (BLCKSZ - sizeof(TapeBlockTrailer))
#define TapeBlockGetTrailer(buf) \
	((TapeBlockTrailer *) ((char *) buf + TapeBlockPayloadSize))

#define TapeBlockIsLast(buf) (TapeBlockGetTrailer(buf)->next < 0)
#define TapeBlockGetNBytes(buf) \
	(TapeBlockIsLast(buf) ? \
	 (- TapeBlockGetTrailer(buf)->next) : TapeBlockPayloadSize)
#define TapeBlockSetNBytes(buf, nbytes) \
	(TapeBlockGetTrailer(buf)->next = -(nbytes))


/*
 * This data structure represents a single "logical tape" within the set
 * of logical tapes stored in the same file.
 *
 * While writing, we hold the current partially-written data block in the
 * buffer.  While reading, we can hold multiple blocks in the buffer.  Note
 * that we don't retain the trailers of a block when it's read into the
 * buffer.  The buffer therefore contains one large contiguous chunk of data
 * from the tape.
 */
typedef struct LogicalTape
{
	bool		writing;		/* T while in write phase */
	bool		frozen;			/* T if blocks should not be freed when read */
	bool		dirty;			/* does buffer need to be written? */

	/*
	 * Block numbers of the first, current, and next block of the tape.
	 *
	 * The "current" block number is only valid when writing, or reading from
	 * a frozen tape.  (When reading from an unfrozen tape, we use a larger
	 * read buffer that holds multiple blocks, so the "current" block is
	 * ambiguous.)
	 */
	long		firstBlockNumber;
	long		curBlockNumber;
	long		nextBlockNumber;

	/*
	 * Buffer for current data block(s).
	 */
	char	   *buffer;			/* physical buffer (separately palloc'd) */
	int			buffer_size;	/* allocated size of the buffer */
	int			pos;			/* next read/write position in buffer */
	int			nbytes;			/* total # of valid bytes in buffer */
} LogicalTape;

/*
 * This data structure represents a set of related "logical tapes" sharing
 * space in a single underlying file.  (But that "file" may be multiple files
 * if needed to escape OS limits on file size; buffile.c handles that for us.)
 * The number of tapes is fixed at creation.
 */
struct LogicalTapeSet
{
	BufFile    *pfile;			/* underlying file for whole tape set */

	/*
	 * File size tracking.  nBlocksWritten is the size of the underlying file,
	 * in BLCKSZ blocks.  nBlocksAllocated is the number of blocks allocated
	 * by ltsGetFreeBlock(), and it is always greater than or equal to
	 * nBlocksWritten.  Blocks between nBlocksAllocated and nBlocksWritten are
	 * blocks that have been allocated for a tape, but have not been written
	 * to the underlying file yet.
	 */
	long		nBlocksAllocated;	/* # of blocks allocated */
	long		nBlocksWritten; /* # of blocks used in underlying file */

	/*
	 * We store the numbers of recycled-and-available blocks in freeBlocks[].
	 * When there are no such blocks, we extend the underlying file.
	 *
	 * If forgetFreeSpace is true then any freed blocks are simply forgotten
	 * rather than being remembered in freeBlocks[].  See notes for
	 * LogicalTapeSetForgetFreeSpace().
	 *
	 * If blocksSorted is true then the block numbers in freeBlocks are in
	 * *decreasing* order, so that removing the last entry gives us the lowest
	 * free block.  We re-sort the blocks whenever a block is demanded; this
	 * should be reasonably efficient given the expected usage pattern.
	 */
	bool		forgetFreeSpace;	/* are we remembering free blocks? */
	bool		blocksSorted;	/* is freeBlocks[] currently in order? */
	long	   *freeBlocks;		/* resizable array */
	int			nFreeBlocks;	/* # of currently free blocks */
	int			freeBlocksLen;	/* current allocated length of freeBlocks[] */

	/* The array of logical tapes. */
	int			nTapes;			/* # of logical tapes in set */
	LogicalTape tapes[FLEXIBLE_ARRAY_MEMBER];	/* has nTapes nentries */
};

static void ltsWriteBlock(LogicalTapeSet *lts, long blocknum, void *buffer);
static void ltsReadBlock(LogicalTapeSet *lts, long blocknum, void *buffer);
static long ltsGetFreeBlock(LogicalTapeSet *lts);
static void ltsReleaseBlock(LogicalTapeSet *lts, long blocknum);


/*
 * Write a block-sized buffer to the specified block of the underlying file.
 *
 * No need for an error return convention; we ereport() on any error.
 */
static void
ltsWriteBlock(LogicalTapeSet *lts, long blocknum, void *buffer)
{
	/*
	 * BufFile does not support "holes", so if we're about to write a block
	 * that's past the current end of file, fill the space between the current
	 * end of file and the target block with zeros.
	 *
	 * This should happen rarely, otherwise you are not writing very
	 * sequentially.  In current use, this only happens when the sort ends
	 * writing a run, and switches to another tape.  The last block of the
	 * previous tape isn't flushed to disk until the end of the sort, so you
	 * get one-block hole, where the last block of the previous tape will
	 * later go.
	 */
	while (blocknum > lts->nBlocksWritten)
	{
		char		zerobuf[BLCKSZ];

		MemSet(zerobuf, 0, sizeof(zerobuf));

		ltsWriteBlock(lts, lts->nBlocksWritten, zerobuf);
	}

	/* Write the requested block */
	if (BufFileSeekBlock(lts->pfile, blocknum) != 0 ||
		BufFileWrite(lts->pfile, buffer, BLCKSZ) != BLCKSZ)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write block %ld of temporary file: %m",
						blocknum)));

	/* Update nBlocksWritten, if we extended the file */
	if (blocknum == lts->nBlocksWritten)
		lts->nBlocksWritten++;
}

/*
 * Read a block-sized buffer from the specified block of the underlying file.
 *
 * No need for an error return convention; we ereport() on any error.   This
 * module should never attempt to read a block it doesn't know is there.
 */
static void
ltsReadBlock(LogicalTapeSet *lts, long blocknum, void *buffer)
{
	if (BufFileSeekBlock(lts->pfile, blocknum) != 0 ||
		BufFileRead(lts->pfile, buffer, BLCKSZ) != BLCKSZ)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read block %ld of temporary file: %m",
						blocknum)));
}

/*
 * Read as many blocks as we can into the per-tape buffer.
 *
 * Returns true if anything was read, 'false' on EOF.
 */
static bool
ltsReadFillBuffer(LogicalTapeSet *lts, LogicalTape *lt)
{
	lt->pos = 0;
	lt->nbytes = 0;

	do
	{
		char	   *thisbuf = lt->buffer + lt->nbytes;

		/* Fetch next block number */
		if (lt->nextBlockNumber == -1L)
			break;				/* EOF */

		/* Read the block */
		ltsReadBlock(lts, lt->nextBlockNumber, (void *) thisbuf);
		if (!lt->frozen)
			ltsReleaseBlock(lts, lt->nextBlockNumber);
		lt->curBlockNumber = lt->nextBlockNumber;

		lt->nbytes += TapeBlockGetNBytes(thisbuf);
		if (TapeBlockIsLast(thisbuf))
		{
			lt->nextBlockNumber = -1L;
			/* EOF */
			break;
		}
		else
			lt->nextBlockNumber = TapeBlockGetTrailer(thisbuf)->next;

		/* Advance to next block, if we have buffer space left */
	} while (lt->buffer_size - lt->nbytes > BLCKSZ);

	return (lt->nbytes > 0);
}

/*
 * qsort comparator for sorting freeBlocks[] into decreasing order.
 */
static int
freeBlocks_cmp(const void *a, const void *b)
{
	long		ablk = *((const long *) a);
	long		bblk = *((const long *) b);

	/* can't just subtract because long might be wider than int */
	if (ablk < bblk)
		return 1;
	if (ablk > bblk)
		return -1;
	return 0;
}

/*
 * Select a currently unused block for writing to.
 */
static long
ltsGetFreeBlock(LogicalTapeSet *lts)
{
	/*
	 * If there are multiple free blocks, we select the one appearing last in
	 * freeBlocks[] (after sorting the array if needed).  If there are none,
	 * assign the next block at the end of the file.
	 */
	if (lts->nFreeBlocks > 0)
	{
		if (!lts->blocksSorted)
		{
			qsort((void *) lts->freeBlocks, lts->nFreeBlocks,
				  sizeof(long), freeBlocks_cmp);
			lts->blocksSorted = true;
		}
		return lts->freeBlocks[--lts->nFreeBlocks];
	}
	else
		return lts->nBlocksAllocated++;
}

/*
 * Return a block# to the freelist.
 */
static void
ltsReleaseBlock(LogicalTapeSet *lts, long blocknum)
{
	int			ndx;

	/*
	 * Do nothing if we're no longer interested in remembering free space.
	 */
	if (lts->forgetFreeSpace)
		return;

	/*
	 * Enlarge freeBlocks array if full.
	 */
	if (lts->nFreeBlocks >= lts->freeBlocksLen)
	{
		lts->freeBlocksLen *= 2;
		lts->freeBlocks = (long *) repalloc(lts->freeBlocks,
											lts->freeBlocksLen * sizeof(long));
	}

	/*
	 * Add blocknum to array, and mark the array unsorted if it's no longer in
	 * decreasing order.
	 */
	ndx = lts->nFreeBlocks++;
	lts->freeBlocks[ndx] = blocknum;
	if (ndx > 0 && lts->freeBlocks[ndx - 1] < blocknum)
		lts->blocksSorted = false;
}

/*
 * Create a set of logical tapes in a temporary underlying file.
 *
 * Each tape is initialized in write state.
 */
LogicalTapeSet *
LogicalTapeSetCreate(int ntapes)
{
	LogicalTapeSet *lts;
	LogicalTape *lt;
	int			i;

	/*
	 * Create top-level struct including per-tape LogicalTape structs.
	 */
	Assert(ntapes > 0);
	lts = (LogicalTapeSet *) palloc(offsetof(LogicalTapeSet, tapes) +
									ntapes * sizeof(LogicalTape));
	lts->pfile = BufFileCreateTemp(false);
	lts->nBlocksAllocated = 0L;
	lts->nBlocksWritten = 0L;
	lts->forgetFreeSpace = false;
	lts->blocksSorted = true;	/* a zero-length array is sorted ... */
	lts->freeBlocksLen = 32;	/* reasonable initial guess */
	lts->freeBlocks = (long *) palloc(lts->freeBlocksLen * sizeof(long));
	lts->nFreeBlocks = 0;
	lts->nTapes = ntapes;

	/*
	 * Initialize per-tape structs.  Note we allocate the I/O buffer and the
	 * first block for a tape only when it is first actually written to.  This
	 * avoids wasting memory space when tuplesort.c overestimates the number
	 * of tapes needed.
	 */
	for (i = 0; i < ntapes; i++)
	{
		lt = &lts->tapes[i];
		lt->writing = true;
		lt->frozen = false;
		lt->dirty = false;
		lt->firstBlockNumber = -1L;
		lt->curBlockNumber = -1L;
		lt->buffer = NULL;
		lt->buffer_size = 0;
		lt->pos = 0;
		lt->nbytes = 0;
	}
	return lts;
}

/*
 * Close a logical tape set and release all resources.
 */
void
LogicalTapeSetClose(LogicalTapeSet *lts)
{
	LogicalTape *lt;
	int			i;

	BufFileClose(lts->pfile);
	for (i = 0; i < lts->nTapes; i++)
	{
		lt = &lts->tapes[i];
		if (lt->buffer)
			pfree(lt->buffer);
	}
	pfree(lts->freeBlocks);
	pfree(lts);
}

/*
 * Mark a logical tape set as not needing management of free space anymore.
 *
 * This should be called if the caller does not intend to write any more data
 * into the tape set, but is reading from un-frozen tapes.  Since no more
 * writes are planned, remembering free blocks is no longer useful.  Setting
 * this flag lets us avoid wasting time and space in ltsReleaseBlock(), which
 * is not designed to handle large numbers of free blocks.
 */
void
LogicalTapeSetForgetFreeSpace(LogicalTapeSet *lts)
{
	lts->forgetFreeSpace = true;
}

/*
 * Write to a logical tape.
 *
 * There are no error returns; we ereport() on failure.
 */
void
LogicalTapeWrite(LogicalTapeSet *lts, int tapenum,
				 void *ptr, size_t size)
{
	LogicalTape *lt;
	size_t		nthistime;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];
	Assert(lt->writing);

	/* Allocate data buffer and first block on first write */
	if (lt->buffer == NULL)
	{
		lt->buffer = (char *) palloc(BLCKSZ);
		lt->buffer_size = BLCKSZ;
	}
	if (lt->curBlockNumber == -1)
	{
		Assert(lt->firstBlockNumber == -1);
		Assert(lt->pos == 0);

		lt->curBlockNumber = ltsGetFreeBlock(lts);
		lt->firstBlockNumber = lt->curBlockNumber;

		TapeBlockGetTrailer(lt->buffer)->prev = -1L;
	}

	Assert(lt->buffer_size == BLCKSZ);
	while (size > 0)
	{
		if (lt->pos >= TapeBlockPayloadSize)
		{
			/* Buffer full, dump it out */
			long		nextBlockNumber;

			if (!lt->dirty)
			{
				/* Hmm, went directly from reading to writing? */
				elog(ERROR, "invalid logtape state: should be dirty");
			}

			/*
			 * First allocate the next block, so that we can store it in the
			 * 'next' pointer of this block.
			 */
			nextBlockNumber = ltsGetFreeBlock(lts);

			/* set the next-pointer and dump the current block. */
			TapeBlockGetTrailer(lt->buffer)->next = nextBlockNumber;
			ltsWriteBlock(lts, lt->curBlockNumber, (void *) lt->buffer);

			/* initialize the prev-pointer of the next block */
			TapeBlockGetTrailer(lt->buffer)->prev = lt->curBlockNumber;
			lt->curBlockNumber = nextBlockNumber;
			lt->pos = 0;
			lt->nbytes = 0;
		}

		nthistime = TapeBlockPayloadSize - lt->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(lt->buffer + lt->pos, ptr, nthistime);

		lt->dirty = true;
		lt->pos += nthistime;
		if (lt->nbytes < lt->pos)
			lt->nbytes = lt->pos;
		ptr = (void *) ((char *) ptr + nthistime);
		size -= nthistime;
	}
}

/*
 * Rewind logical tape and switch from writing to reading.
 *
 * The tape must currently be in writing state, or "frozen" in read state.
 *
 * 'buffer_size' specifies how much memory to use for the read buffer.
 * Regardless of the argument, the actual amount of memory used is between
 * BLCKSZ and MaxAllocSize, and is a multiple of BLCKSZ.  The given value is
 * rounded down and truncated to fit those constraints, if necessary.  If the
 * tape is frozen, the 'buffer_size' argument is ignored, and a small BLCKSZ
 * byte buffer is used.
 */
void
LogicalTapeRewindForRead(LogicalTapeSet *lts, int tapenum, size_t buffer_size)
{
	LogicalTape *lt;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];

	/*
	 * Round and cap buffer_size if needed.
	 */
	if (lt->frozen)
		buffer_size = BLCKSZ;
	else
	{
		/* need at least one block */
		if (buffer_size < BLCKSZ)
			buffer_size = BLCKSZ;

		/*
		 * palloc() larger than MaxAllocSize would fail (a multi-gigabyte
		 * buffer is unlikely to be helpful, anyway)
		 */
		if (buffer_size > MaxAllocSize)
			buffer_size = MaxAllocSize;

		/* round down to BLCKSZ boundary */
		buffer_size -= buffer_size % BLCKSZ;
	}

	if (lt->writing)
	{
		/*
		 * Completion of a write phase.  Flush last partial data block, and
		 * rewind for normal (destructive) read.
		 */
		if (lt->dirty)
		{
			TapeBlockSetNBytes(lt->buffer, lt->nbytes);
			ltsWriteBlock(lts, lt->curBlockNumber, (void *) lt->buffer);
		}
		lt->writing = false;
	}
	else
	{
		/*
		 * This is only OK if tape is frozen; we rewind for (another) read
		 * pass.
		 */
		Assert(lt->frozen);
	}

	/* Allocate a read buffer (unless the tape is empty) */
	if (lt->buffer)
		pfree(lt->buffer);
	lt->buffer = NULL;
	lt->buffer_size = 0;
	if (lt->firstBlockNumber != -1L)
	{
		lt->buffer = palloc(buffer_size);
		lt->buffer_size = buffer_size;
	}

	/* Read the first block, or reset if tape is empty */
	lt->nextBlockNumber = lt->firstBlockNumber;
	lt->pos = 0;
	lt->nbytes = 0;
	ltsReadFillBuffer(lts, lt);
}

/*
 * Rewind logical tape and switch from reading to writing.
 *
 * NOTE: we assume the caller has read the tape to the end; otherwise
 * untouched data will not have been freed. We could add more code to free
 * any unread blocks, but in current usage of this module it'd be useless
 * code.
 */
void
LogicalTapeRewindForWrite(LogicalTapeSet *lts, int tapenum)
{
	LogicalTape *lt;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];

	Assert(!lt->writing && !lt->frozen);
	lt->writing = true;
	lt->dirty = false;
	lt->firstBlockNumber = -1L;
	lt->curBlockNumber = -1L;
	lt->pos = 0;
	lt->nbytes = 0;
	if (lt->buffer)
		pfree(lt->buffer);
	lt->buffer = NULL;
	lt->buffer_size = 0;
}

/*
 * Read from a logical tape.
 *
 * Early EOF is indicated by return value less than #bytes requested.
 */
size_t
LogicalTapeRead(LogicalTapeSet *lts, int tapenum,
				void *ptr, size_t size)
{
	LogicalTape *lt;
	size_t		nread = 0;
	size_t		nthistime;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];
	Assert(!lt->writing);

	while (size > 0)
	{
		if (lt->pos >= lt->nbytes)
		{
			/* Try to load more data into buffer. */
			if (!ltsReadFillBuffer(lts, lt))
				break;			/* EOF */
		}

		nthistime = lt->nbytes - lt->pos;
		if (nthistime > size)
			nthistime = size;
		Assert(nthistime > 0);

		memcpy(ptr, lt->buffer + lt->pos, nthistime);

		lt->pos += nthistime;
		ptr = (void *) ((char *) ptr + nthistime);
		size -= nthistime;
		nread += nthistime;
	}

	return nread;
}

/*
 * "Freeze" the contents of a tape so that it can be read multiple times
 * and/or read backwards.  Once a tape is frozen, its contents will not
 * be released until the LogicalTapeSet is destroyed.  This is expected
 * to be used only for the final output pass of a merge.
 *
 * This *must* be called just at the end of a write pass, before the
 * tape is rewound (after rewind is too late!).  It performs a rewind
 * and switch to read mode "for free".  An immediately following rewind-
 * for-read call is OK but not necessary.
 */
void
LogicalTapeFreeze(LogicalTapeSet *lts, int tapenum)
{
	LogicalTape *lt;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];
	Assert(lt->writing);

	/*
	 * Completion of a write phase.  Flush last partial data block, and rewind
	 * for nondestructive read.
	 */
	if (lt->dirty)
	{
		TapeBlockSetNBytes(lt->buffer, lt->nbytes);
		ltsWriteBlock(lts, lt->curBlockNumber, (void *) lt->buffer);
		lt->writing = false;
	}
	lt->writing = false;
	lt->frozen = true;

	/*
	 * The seek and backspace functions assume a single block read buffer.
	 * That's OK with current usage.  A larger buffer is helpful to make the
	 * read pattern of the backing file look more sequential to the OS, when
	 * we're reading from multiple tapes.  But at the end of a sort, when a
	 * tape is frozen, we only read from a single tape anyway.
	 */
	if (!lt->buffer || lt->buffer_size != BLCKSZ)
	{
		if (lt->buffer)
			pfree(lt->buffer);
		lt->buffer = palloc(BLCKSZ);
		lt->buffer_size = BLCKSZ;
	}

	/* Read the first block, or reset if tape is empty */
	lt->curBlockNumber = lt->firstBlockNumber;
	lt->pos = 0;
	lt->nbytes = 0;

	if (lt->firstBlockNumber == -1L)
		lt->nextBlockNumber = -1L;
	ltsReadBlock(lts, lt->curBlockNumber, (void *) lt->buffer);
	if (TapeBlockIsLast(lt->buffer))
		lt->nextBlockNumber = -1L;
	else
		lt->nextBlockNumber = TapeBlockGetTrailer(lt->buffer)->next;
	lt->nbytes = TapeBlockGetNBytes(lt->buffer);
}

/*
 * Backspace the tape a given number of bytes.  (We also support a more
 * general seek interface, see below.)
 *
 * *Only* a frozen-for-read tape can be backed up; we don't support
 * random access during write, and an unfrozen read tape may have
 * already discarded the desired data!
 *
 * Returns the number of bytes backed up.  It can be less than the
 * requested amount, if there isn't that much data before the current
 * position.  The tape is positioned to the beginning of the tape in
 * that case.
 */
size_t
LogicalTapeBackspace(LogicalTapeSet *lts, int tapenum, size_t size)
{
	LogicalTape *lt;
	size_t		seekpos = 0;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];
	Assert(lt->frozen);
	Assert(lt->buffer_size == BLCKSZ);

	/*
	 * Easy case for seek within current block.
	 */
	if (size <= (size_t) lt->pos)
	{
		lt->pos -= (int) size;
		return size;
	}

	/*
	 * Not-so-easy case, have to walk back the chain of blocks.  This
	 * implementation would be pretty inefficient for long seeks, but we
	 * really aren't doing that (a seek over one tuple is typical).
	 */
	seekpos = (size_t) lt->pos; /* part within this block */
	while (size > seekpos)
	{
		long		prev = TapeBlockGetTrailer(lt->buffer)->prev;

		if (prev == -1L)
		{
			/* Tried to back up beyond the beginning of tape. */
			if (lt->curBlockNumber != lt->firstBlockNumber)
				elog(ERROR, "unexpected end of tape");
			lt->pos = 0;
			return seekpos;
		}

		ltsReadBlock(lts, prev, (void *) lt->buffer);

		if (TapeBlockGetTrailer(lt->buffer)->next != lt->curBlockNumber)
			elog(ERROR, "broken tape, next of block %ld is %ld, expected %ld",
				 prev,
				 TapeBlockGetTrailer(lt->buffer)->next,
				 lt->curBlockNumber);

		lt->nbytes = TapeBlockPayloadSize;
		lt->curBlockNumber = prev;
		lt->nextBlockNumber = TapeBlockGetTrailer(lt->buffer)->next;

		seekpos += TapeBlockPayloadSize;
	}

	/*
	 * 'seekpos' can now be greater than 'size', because it points to the
	 * beginning the target block.  The difference is the position within the
	 * page.
	 */
	lt->pos = seekpos - size;
	return size;
}

/*
 * Seek to an arbitrary position in a logical tape.
 *
 * *Only* a frozen-for-read tape can be seeked.
 *
 * Must be called with a block/offset previously returned by
 * LogicalTapeTell().
 */
void
LogicalTapeSeek(LogicalTapeSet *lts, int tapenum,
				long blocknum, int offset)
{
	LogicalTape *lt;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];
	Assert(lt->frozen);
	Assert(offset >= 0 && offset <= TapeBlockPayloadSize);
	Assert(lt->buffer_size == BLCKSZ);

	if (blocknum != lt->curBlockNumber)
	{
		ltsReadBlock(lts, blocknum, (void *) lt->buffer);
		lt->curBlockNumber = blocknum;
		lt->nbytes = TapeBlockPayloadSize;
		lt->nextBlockNumber = TapeBlockGetTrailer(lt->buffer)->next;
	}

	if (offset > lt->nbytes)
		elog(ERROR, "invalid tape seek position");
	lt->pos = offset;
}

/*
 * Obtain current position in a form suitable for a later LogicalTapeSeek.
 *
 * NOTE: it'd be OK to do this during write phase with intention of using
 * the position for a seek after freezing.  Not clear if anyone needs that.
 */
void
LogicalTapeTell(LogicalTapeSet *lts, int tapenum,
				long *blocknum, int *offset)
{
	LogicalTape *lt;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];

	/* With a larger buffer, 'pos' wouldn't be the same as offset within page */
	Assert(lt->buffer_size == BLCKSZ);

	*blocknum = lt->curBlockNumber;
	*offset = lt->pos;
}

/*
 * Obtain total disk space currently used by a LogicalTapeSet, in blocks.
 */
long
LogicalTapeSetBlocks(LogicalTapeSet *lts)
{
	return lts->nBlocksAllocated;
}
