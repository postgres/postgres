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
 * The blocks in each logical tape are remembered using a method borrowed
 * from the Unix HFS filesystem: we store data block numbers in an
 * "indirect block".  If an indirect block fills up, we write it out to
 * the underlying file and remember its location in a second-level indirect
 * block.  In the same way second-level blocks are remembered in third-
 * level blocks, and so on if necessary (of course we're talking huge
 * amounts of data here).  The topmost indirect block of a given logical
 * tape is never actually written out to the physical file, but all lower-
 * level indirect blocks will be.
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
 * To support the above policy of writing to the lowest free block,
 * ltsGetFreeBlock sorts the list of free block numbers into decreasing
 * order each time it is asked for a block and the list isn't currently
 * sorted.  This is an efficient way to handle it because we expect cycles
 * of releasing many blocks followed by re-using many blocks, due to
 * tuplesort.c's "preread" behavior.
 *
 * Since all the bookkeeping and buffer memory is allocated with palloc(),
 * and the underlying file(s) are made with OpenTemporaryFile, all resources
 * for a logical tape set are certain to be cleaned up even if processing
 * is aborted by ereport(ERROR).  To avoid confusion, the caller should take
 * care that all calls for a single LogicalTapeSet are made in the same
 * palloc context.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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

/*
 * Block indexes are "long"s, so we can fit this many per indirect block.
 * NB: we assume this is an exact fit!
 */
#define BLOCKS_PER_INDIR_BLOCK	((int) (BLCKSZ / sizeof(long)))

/*
 * We use a struct like this for each active indirection level of each
 * logical tape.  If the indirect block is not the highest level of its
 * tape, the "nextup" link points to the next higher level.  Only the
 * "ptrs" array is written out if we have to dump the indirect block to
 * disk.  If "ptrs" is not completely full, we store -1L in the first
 * unused slot at completion of the write phase for the logical tape.
 */
typedef struct IndirectBlock
{
	int			nextSlot;		/* next pointer slot to write or read */
	struct IndirectBlock *nextup;		/* parent indirect level, or NULL if
										 * top */
	long		ptrs[BLOCKS_PER_INDIR_BLOCK];	/* indexes of contained blocks */
} IndirectBlock;

/*
 * This data structure represents a single "logical tape" within the set
 * of logical tapes stored in the same file.  We must keep track of the
 * current partially-read-or-written data block as well as the active
 * indirect block level(s).
 */
typedef struct LogicalTape
{
	IndirectBlock *indirect;	/* bottom of my indirect-block hierarchy */
	bool		writing;		/* T while in write phase */
	bool		frozen;			/* T if blocks should not be freed when read */
	bool		dirty;			/* does buffer need to be written? */

	/*
	 * The total data volume in the logical tape is numFullBlocks * BLCKSZ +
	 * lastBlockBytes.  BUT: we do not update lastBlockBytes during writing,
	 * only at completion of a write phase.
	 */
	long		numFullBlocks;	/* number of complete blocks in log tape */
	int			lastBlockBytes; /* valid bytes in last (incomplete) block */

	/*
	 * Buffer for current data block.  Note we don't bother to store the
	 * actual file block number of the data block (during the write phase it
	 * hasn't been assigned yet, and during read we don't care anymore). But
	 * we do need the relative block number so we can detect end-of-tape while
	 * reading.
	 */
	char	   *buffer;			/* physical buffer (separately palloc'd) */
	long		curBlockNumber; /* this block's logical blk# within tape */
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
	long		nFileBlocks;	/* # of blocks used in underlying file */

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
static void ltsRecordBlockNum(LogicalTapeSet *lts, IndirectBlock *indirect,
				  long blocknum);
static long ltsRewindIndirectBlock(LogicalTapeSet *lts,
					   IndirectBlock *indirect,
					   bool freezing);
static long ltsRewindFrozenIndirectBlock(LogicalTapeSet *lts,
							 IndirectBlock *indirect);
static long ltsRecallNextBlockNum(LogicalTapeSet *lts,
					  IndirectBlock *indirect,
					  bool frozen);
static long ltsRecallPrevBlockNum(LogicalTapeSet *lts,
					  IndirectBlock *indirect);
static void ltsDumpBuffer(LogicalTapeSet *lts, LogicalTape *lt);


/*
 * Write a block-sized buffer to the specified block of the underlying file.
 *
 * NB: should not attempt to write beyond current end of file (ie, create
 * "holes" in file), since BufFile doesn't allow that.  The first write pass
 * must write blocks sequentially.
 *
 * No need for an error return convention; we ereport() on any error.
 */
static void
ltsWriteBlock(LogicalTapeSet *lts, long blocknum, void *buffer)
{
	if (BufFileSeekBlock(lts->pfile, blocknum) != 0 ||
		BufFileWrite(lts->pfile, buffer, BLCKSZ) != BLCKSZ)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write block %ld of temporary file: %m",
						blocknum)));
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
 *
 * NB: should only be called when writer is ready to write immediately,
 * to ensure that first write pass is sequential.
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
		return lts->nFileBlocks++;
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
 * These routines manipulate indirect-block hierarchies.  All are recursive
 * so that they don't have any specific limit on the depth of hierarchy.
 */

/*
 * Record a data block number in a logical tape's lowest indirect block,
 * or record an indirect block's number in the next higher indirect level.
 */
static void
ltsRecordBlockNum(LogicalTapeSet *lts, IndirectBlock *indirect,
				  long blocknum)
{
	if (indirect->nextSlot >= BLOCKS_PER_INDIR_BLOCK)
	{
		/*
		 * This indirect block is full, so dump it out and recursively save
		 * its address in the next indirection level.  Create a new
		 * indirection level if there wasn't one before.
		 */
		long		indirblock = ltsGetFreeBlock(lts);

		ltsWriteBlock(lts, indirblock, (void *) indirect->ptrs);
		if (indirect->nextup == NULL)
		{
			indirect->nextup = (IndirectBlock *) palloc(sizeof(IndirectBlock));
			indirect->nextup->nextSlot = 0;
			indirect->nextup->nextup = NULL;
		}
		ltsRecordBlockNum(lts, indirect->nextup, indirblock);

		/*
		 * Reset to fill another indirect block at this level.
		 */
		indirect->nextSlot = 0;
	}
	indirect->ptrs[indirect->nextSlot++] = blocknum;
}

/*
 * Reset a logical tape's indirect-block hierarchy after a write pass
 * to prepare for reading.  We dump out partly-filled blocks except
 * at the top of the hierarchy, and we rewind each level to the start.
 * This call returns the first data block number, or -1L if the tape
 * is empty.
 *
 * Unless 'freezing' is true, release indirect blocks to the free pool after
 * reading them.
 */
static long
ltsRewindIndirectBlock(LogicalTapeSet *lts,
					   IndirectBlock *indirect,
					   bool freezing)
{
	/* Handle case of never-written-to tape */
	if (indirect == NULL)
		return -1L;

	/* Insert sentinel if block is not full */
	if (indirect->nextSlot < BLOCKS_PER_INDIR_BLOCK)
		indirect->ptrs[indirect->nextSlot] = -1L;

	/*
	 * If block is not topmost, write it out, and recurse to obtain address of
	 * first block in this hierarchy level.  Read that one in.
	 */
	if (indirect->nextup != NULL)
	{
		long		indirblock = ltsGetFreeBlock(lts);

		ltsWriteBlock(lts, indirblock, (void *) indirect->ptrs);
		ltsRecordBlockNum(lts, indirect->nextup, indirblock);
		indirblock = ltsRewindIndirectBlock(lts, indirect->nextup, freezing);
		Assert(indirblock != -1L);
		ltsReadBlock(lts, indirblock, (void *) indirect->ptrs);
		if (!freezing)
			ltsReleaseBlock(lts, indirblock);
	}

	/*
	 * Reset my next-block pointer, and then fetch a block number if any.
	 */
	indirect->nextSlot = 0;
	if (indirect->ptrs[0] == -1L)
		return -1L;
	return indirect->ptrs[indirect->nextSlot++];
}

/*
 * Rewind a previously-frozen indirect-block hierarchy for another read pass.
 * This call returns the first data block number, or -1L if the tape
 * is empty.
 */
static long
ltsRewindFrozenIndirectBlock(LogicalTapeSet *lts,
							 IndirectBlock *indirect)
{
	/* Handle case of never-written-to tape */
	if (indirect == NULL)
		return -1L;

	/*
	 * If block is not topmost, recurse to obtain address of first block in
	 * this hierarchy level.  Read that one in.
	 */
	if (indirect->nextup != NULL)
	{
		long		indirblock;

		indirblock = ltsRewindFrozenIndirectBlock(lts, indirect->nextup);
		Assert(indirblock != -1L);
		ltsReadBlock(lts, indirblock, (void *) indirect->ptrs);
	}

	/*
	 * Reset my next-block pointer, and then fetch a block number if any.
	 */
	indirect->nextSlot = 0;
	if (indirect->ptrs[0] == -1L)
		return -1L;
	return indirect->ptrs[indirect->nextSlot++];
}

/*
 * Obtain next data block number in the forward direction, or -1L if no more.
 *
 * Unless 'frozen' is true, release indirect blocks to the free pool after
 * reading them.
 */
static long
ltsRecallNextBlockNum(LogicalTapeSet *lts,
					  IndirectBlock *indirect,
					  bool frozen)
{
	/* Handle case of never-written-to tape */
	if (indirect == NULL)
		return -1L;

	if (indirect->nextSlot >= BLOCKS_PER_INDIR_BLOCK ||
		indirect->ptrs[indirect->nextSlot] == -1L)
	{
		long		indirblock;

		if (indirect->nextup == NULL)
			return -1L;			/* nothing left at this level */
		indirblock = ltsRecallNextBlockNum(lts, indirect->nextup, frozen);
		if (indirblock == -1L)
			return -1L;			/* nothing left at this level */
		ltsReadBlock(lts, indirblock, (void *) indirect->ptrs);
		if (!frozen)
			ltsReleaseBlock(lts, indirblock);
		indirect->nextSlot = 0;
	}
	if (indirect->ptrs[indirect->nextSlot] == -1L)
		return -1L;
	return indirect->ptrs[indirect->nextSlot++];
}

/*
 * Obtain next data block number in the reverse direction, or -1L if no more.
 *
 * Note this fetches the block# before the one last returned, no matter which
 * direction of call returned that one.  If we fail, no change in state.
 *
 * This routine can only be used in 'frozen' state, so there's no need to
 * pass a parameter telling whether to release blocks ... we never do.
 */
static long
ltsRecallPrevBlockNum(LogicalTapeSet *lts,
					  IndirectBlock *indirect)
{
	/* Handle case of never-written-to tape */
	if (indirect == NULL)
		return -1L;

	if (indirect->nextSlot <= 1)
	{
		long		indirblock;

		if (indirect->nextup == NULL)
			return -1L;			/* nothing left at this level */
		indirblock = ltsRecallPrevBlockNum(lts, indirect->nextup);
		if (indirblock == -1L)
			return -1L;			/* nothing left at this level */
		ltsReadBlock(lts, indirblock, (void *) indirect->ptrs);

		/*
		 * The previous block would only have been written out if full, so we
		 * need not search it for a -1 sentinel.
		 */
		indirect->nextSlot = BLOCKS_PER_INDIR_BLOCK + 1;
	}
	indirect->nextSlot--;
	return indirect->ptrs[indirect->nextSlot - 1];
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
	lts->nFileBlocks = 0L;
	lts->forgetFreeSpace = false;
	lts->blocksSorted = true;	/* a zero-length array is sorted ... */
	lts->freeBlocksLen = 32;	/* reasonable initial guess */
	lts->freeBlocks = (long *) palloc(lts->freeBlocksLen * sizeof(long));
	lts->nFreeBlocks = 0;
	lts->nTapes = ntapes;

	/*
	 * Initialize per-tape structs.  Note we allocate the I/O buffer and
	 * first-level indirect block for a tape only when it is first actually
	 * written to.  This avoids wasting memory space when tuplesort.c
	 * overestimates the number of tapes needed.
	 */
	for (i = 0; i < ntapes; i++)
	{
		lt = &lts->tapes[i];
		lt->indirect = NULL;
		lt->writing = true;
		lt->frozen = false;
		lt->dirty = false;
		lt->numFullBlocks = 0L;
		lt->lastBlockBytes = 0;
		lt->buffer = NULL;
		lt->curBlockNumber = 0L;
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
	IndirectBlock *ib,
			   *nextib;
	int			i;

	BufFileClose(lts->pfile);
	for (i = 0; i < lts->nTapes; i++)
	{
		lt = &lts->tapes[i];
		for (ib = lt->indirect; ib != NULL; ib = nextib)
		{
			nextib = ib->nextup;
			pfree(ib);
		}
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
 * Dump the dirty buffer of a logical tape.
 */
static void
ltsDumpBuffer(LogicalTapeSet *lts, LogicalTape *lt)
{
	long		datablock = ltsGetFreeBlock(lts);

	Assert(lt->dirty);
	ltsWriteBlock(lts, datablock, (void *) lt->buffer);
	ltsRecordBlockNum(lts, lt->indirect, datablock);
	lt->dirty = false;
	/* Caller must do other state update as needed */
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

	/* Allocate data buffer and first indirect block on first write */
	if (lt->buffer == NULL)
		lt->buffer = (char *) palloc(BLCKSZ);
	if (lt->indirect == NULL)
	{
		lt->indirect = (IndirectBlock *) palloc(sizeof(IndirectBlock));
		lt->indirect->nextSlot = 0;
		lt->indirect->nextup = NULL;
	}

	while (size > 0)
	{
		if (lt->pos >= BLCKSZ)
		{
			/* Buffer full, dump it out */
			if (lt->dirty)
				ltsDumpBuffer(lts, lt);
			else
			{
				/* Hmm, went directly from reading to writing? */
				elog(ERROR, "invalid logtape state: should be dirty");
			}
			lt->numFullBlocks++;
			lt->curBlockNumber++;
			lt->pos = 0;
			lt->nbytes = 0;
		}

		nthistime = BLCKSZ - lt->pos;
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
 * Rewind logical tape and switch from writing to reading or vice versa.
 *
 * Unless the tape has been "frozen" in read state, forWrite must be the
 * opposite of the previous tape state.
 */
void
LogicalTapeRewind(LogicalTapeSet *lts, int tapenum, bool forWrite)
{
	LogicalTape *lt;
	long		datablocknum;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];

	if (!forWrite)
	{
		if (lt->writing)
		{
			/*
			 * Completion of a write phase.  Flush last partial data block,
			 * flush any partial indirect blocks, rewind for normal
			 * (destructive) read.
			 */
			if (lt->dirty)
				ltsDumpBuffer(lts, lt);
			lt->lastBlockBytes = lt->nbytes;
			lt->writing = false;
			datablocknum = ltsRewindIndirectBlock(lts, lt->indirect, false);
		}
		else
		{
			/*
			 * This is only OK if tape is frozen; we rewind for (another) read
			 * pass.
			 */
			Assert(lt->frozen);
			datablocknum = ltsRewindFrozenIndirectBlock(lts, lt->indirect);
		}
		/* Read the first block, or reset if tape is empty */
		lt->curBlockNumber = 0L;
		lt->pos = 0;
		lt->nbytes = 0;
		if (datablocknum != -1L)
		{
			ltsReadBlock(lts, datablocknum, (void *) lt->buffer);
			if (!lt->frozen)
				ltsReleaseBlock(lts, datablocknum);
			lt->nbytes = (lt->curBlockNumber < lt->numFullBlocks) ?
				BLCKSZ : lt->lastBlockBytes;
		}
	}
	else
	{
		/*
		 * Completion of a read phase.  Rewind and prepare for write.
		 *
		 * NOTE: we assume the caller has read the tape to the end; otherwise
		 * untouched data and indirect blocks will not have been freed. We
		 * could add more code to free any unread blocks, but in current usage
		 * of this module it'd be useless code.
		 */
		IndirectBlock *ib,
				   *nextib;

		Assert(!lt->writing && !lt->frozen);
		/* Must truncate the indirect-block hierarchy down to one level. */
		if (lt->indirect)
		{
			for (ib = lt->indirect->nextup; ib != NULL; ib = nextib)
			{
				nextib = ib->nextup;
				pfree(ib);
			}
			lt->indirect->nextSlot = 0;
			lt->indirect->nextup = NULL;
		}
		lt->writing = true;
		lt->dirty = false;
		lt->numFullBlocks = 0L;
		lt->lastBlockBytes = 0;
		lt->curBlockNumber = 0L;
		lt->pos = 0;
		lt->nbytes = 0;
	}
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
			long		datablocknum = ltsRecallNextBlockNum(lts, lt->indirect,
															 lt->frozen);

			if (datablocknum == -1L)
				break;			/* EOF */
			lt->curBlockNumber++;
			lt->pos = 0;
			ltsReadBlock(lts, datablocknum, (void *) lt->buffer);
			if (!lt->frozen)
				ltsReleaseBlock(lts, datablocknum);
			lt->nbytes = (lt->curBlockNumber < lt->numFullBlocks) ?
				BLCKSZ : lt->lastBlockBytes;
			if (lt->nbytes <= 0)
				break;			/* EOF (possible here?) */
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
	long		datablocknum;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];
	Assert(lt->writing);

	/*
	 * Completion of a write phase.  Flush last partial data block, flush any
	 * partial indirect blocks, rewind for nondestructive read.
	 */
	if (lt->dirty)
		ltsDumpBuffer(lts, lt);
	lt->lastBlockBytes = lt->nbytes;
	lt->writing = false;
	lt->frozen = true;
	datablocknum = ltsRewindIndirectBlock(lts, lt->indirect, true);
	/* Read the first block, or reset if tape is empty */
	lt->curBlockNumber = 0L;
	lt->pos = 0;
	lt->nbytes = 0;
	if (datablocknum != -1L)
	{
		ltsReadBlock(lts, datablocknum, (void *) lt->buffer);
		lt->nbytes = (lt->curBlockNumber < lt->numFullBlocks) ?
			BLCKSZ : lt->lastBlockBytes;
	}
}

/*
 * Backspace the tape a given number of bytes.  (We also support a more
 * general seek interface, see below.)
 *
 * *Only* a frozen-for-read tape can be backed up; we don't support
 * random access during write, and an unfrozen read tape may have
 * already discarded the desired data!
 *
 * Return value is TRUE if seek successful, FALSE if there isn't that much
 * data before the current point (in which case there's no state change).
 */
bool
LogicalTapeBackspace(LogicalTapeSet *lts, int tapenum, size_t size)
{
	LogicalTape *lt;
	long		nblocks;
	int			newpos;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];
	Assert(lt->frozen);

	/*
	 * Easy case for seek within current block.
	 */
	if (size <= (size_t) lt->pos)
	{
		lt->pos -= (int) size;
		return true;
	}

	/*
	 * Not-so-easy case.  Figure out whether it's possible at all.
	 */
	size -= (size_t) lt->pos;	/* part within this block */
	nblocks = size / BLCKSZ;
	size = size % BLCKSZ;
	if (size)
	{
		nblocks++;
		newpos = (int) (BLCKSZ - size);
	}
	else
		newpos = 0;
	if (nblocks > lt->curBlockNumber)
		return false;			/* a seek too far... */

	/*
	 * OK, we need to back up nblocks blocks.  This implementation would be
	 * pretty inefficient for long seeks, but we really aren't expecting that
	 * (a seek over one tuple is typical).
	 */
	while (nblocks-- > 0)
	{
		long		datablocknum = ltsRecallPrevBlockNum(lts, lt->indirect);

		if (datablocknum == -1L)
			elog(ERROR, "unexpected end of tape");
		lt->curBlockNumber--;
		if (nblocks == 0)
		{
			ltsReadBlock(lts, datablocknum, (void *) lt->buffer);
			lt->nbytes = BLCKSZ;
		}
	}
	lt->pos = newpos;
	return true;
}

/*
 * Seek to an arbitrary position in a logical tape.
 *
 * *Only* a frozen-for-read tape can be seeked.
 *
 * Return value is TRUE if seek successful, FALSE if there isn't that much
 * data in the tape (in which case there's no state change).
 */
bool
LogicalTapeSeek(LogicalTapeSet *lts, int tapenum,
				long blocknum, int offset)
{
	LogicalTape *lt;

	Assert(tapenum >= 0 && tapenum < lts->nTapes);
	lt = &lts->tapes[tapenum];
	Assert(lt->frozen);
	Assert(offset >= 0 && offset <= BLCKSZ);

	/*
	 * Easy case for seek within current block.
	 */
	if (blocknum == lt->curBlockNumber && offset <= lt->nbytes)
	{
		lt->pos = offset;
		return true;
	}

	/*
	 * Not-so-easy case.  Figure out whether it's possible at all.
	 */
	if (blocknum < 0 || blocknum > lt->numFullBlocks ||
		(blocknum == lt->numFullBlocks && offset > lt->lastBlockBytes))
		return false;

	/*
	 * OK, advance or back up to the target block.  This implementation would
	 * be pretty inefficient for long seeks, but we really aren't expecting
	 * that (a seek over one tuple is typical).
	 */
	while (lt->curBlockNumber > blocknum)
	{
		long		datablocknum = ltsRecallPrevBlockNum(lts, lt->indirect);

		if (datablocknum == -1L)
			elog(ERROR, "unexpected end of tape");
		if (--lt->curBlockNumber == blocknum)
			ltsReadBlock(lts, datablocknum, (void *) lt->buffer);
	}
	while (lt->curBlockNumber < blocknum)
	{
		long		datablocknum = ltsRecallNextBlockNum(lts, lt->indirect,
														 lt->frozen);

		if (datablocknum == -1L)
			elog(ERROR, "unexpected end of tape");
		if (++lt->curBlockNumber == blocknum)
			ltsReadBlock(lts, datablocknum, (void *) lt->buffer);
	}
	lt->nbytes = (lt->curBlockNumber < lt->numFullBlocks) ?
		BLCKSZ : lt->lastBlockBytes;
	lt->pos = offset;
	return true;
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
	*blocknum = lt->curBlockNumber;
	*offset = lt->pos;
}

/*
 * Obtain total disk space currently used by a LogicalTapeSet, in blocks.
 */
long
LogicalTapeSetBlocks(LogicalTapeSet *lts)
{
	return lts->nFileBlocks;
}
