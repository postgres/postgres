/*-------------------------------------------------------------------------
 *
 * gistbuildbuffers.c
 *	  node buffer management functions for GiST buffering build algorithm.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistbuildbuffers.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/buffile.h"
#include "storage/bufmgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

static GISTNodeBufferPage *gistAllocateNewPageBuffer(GISTBuildBuffers *gfbb);
static void gistAddLoadedBuffer(GISTBuildBuffers *gfbb,
					GISTNodeBuffer *nodeBuffer);
static void gistLoadNodeBuffer(GISTBuildBuffers *gfbb,
				   GISTNodeBuffer *nodeBuffer);
static void gistUnloadNodeBuffer(GISTBuildBuffers *gfbb,
					 GISTNodeBuffer *nodeBuffer);
static void gistPlaceItupToPage(GISTNodeBufferPage *pageBuffer,
					IndexTuple item);
static void gistGetItupFromPage(GISTNodeBufferPage *pageBuffer,
					IndexTuple *item);
static long gistBuffersGetFreeBlock(GISTBuildBuffers *gfbb);
static void gistBuffersReleaseBlock(GISTBuildBuffers *gfbb, long blocknum);

static void ReadTempFileBlock(BufFile *file, long blknum, void *ptr);
static void WriteTempFileBlock(BufFile *file, long blknum, void *ptr);


/*
 * Initialize GiST build buffers.
 */
GISTBuildBuffers *
gistInitBuildBuffers(int pagesPerBuffer, int levelStep, int maxLevel)
{
	GISTBuildBuffers *gfbb;
	HASHCTL		hashCtl;

	gfbb = palloc(sizeof(GISTBuildBuffers));
	gfbb->pagesPerBuffer = pagesPerBuffer;
	gfbb->levelStep = levelStep;

	/*
	 * Create a temporary file to hold buffer pages that are swapped out of
	 * memory.
	 */
	gfbb->pfile = BufFileCreateTemp(false);
	gfbb->nFileBlocks = 0;

	/* Initialize free page management. */
	gfbb->nFreeBlocks = 0;
	gfbb->freeBlocksLen = 32;
	gfbb->freeBlocks = (long *) palloc(gfbb->freeBlocksLen * sizeof(long));

	/*
	 * Current memory context will be used for all in-memory data structures
	 * of buffers which are persistent during buffering build.
	 */
	gfbb->context = CurrentMemoryContext;

	/*
	 * nodeBuffersTab hash is association between index blocks and it's
	 * buffers.
	 */
	hashCtl.keysize = sizeof(BlockNumber);
	hashCtl.entrysize = sizeof(GISTNodeBuffer);
	hashCtl.hcxt = CurrentMemoryContext;
	hashCtl.hash = tag_hash;
	hashCtl.match = memcmp;
	gfbb->nodeBuffersTab = hash_create("gistbuildbuffers",
									   1024,
									   &hashCtl,
									   HASH_ELEM | HASH_CONTEXT
									   | HASH_FUNCTION | HASH_COMPARE);

	gfbb->bufferEmptyingQueue = NIL;

	/*
	 * Per-level node buffers lists for final buffers emptying process. Node
	 * buffers are inserted here when they are created.
	 */
	gfbb->buffersOnLevelsLen = 1;
	gfbb->buffersOnLevels = (List **) palloc(sizeof(List *) *
											 gfbb->buffersOnLevelsLen);
	gfbb->buffersOnLevels[0] = NIL;

	/*
	 * Block numbers of node buffers which last pages are currently loaded
	 * into main memory.
	 */
	gfbb->loadedBuffersLen = 32;
	gfbb->loadedBuffers = (GISTNodeBuffer **) palloc(gfbb->loadedBuffersLen *
												   sizeof(GISTNodeBuffer *));
	gfbb->loadedBuffersCount = 0;

	gfbb->rootlevel = maxLevel;

	return gfbb;
}

/*
 * Returns a node buffer for given block. The buffer is created if it
 * doesn't exist yet.
 */
GISTNodeBuffer *
gistGetNodeBuffer(GISTBuildBuffers *gfbb, GISTSTATE *giststate,
				  BlockNumber nodeBlocknum, int level)
{
	GISTNodeBuffer *nodeBuffer;
	bool		found;

	/* Find node buffer in hash table */
	nodeBuffer = (GISTNodeBuffer *) hash_search(gfbb->nodeBuffersTab,
												(const void *) &nodeBlocknum,
												HASH_ENTER,
												&found);
	if (!found)
	{
		/*
		 * Node buffer wasn't found. Initialize the new buffer as empty.
		 */
		MemoryContext oldcxt = MemoryContextSwitchTo(gfbb->context);

		/* nodeBuffer->nodeBlocknum is the hash key and was filled in already */
		nodeBuffer->blocksCount = 0;
		nodeBuffer->pageBlocknum = InvalidBlockNumber;
		nodeBuffer->pageBuffer = NULL;
		nodeBuffer->queuedForEmptying = false;
		nodeBuffer->level = level;

		/*
		 * Add this buffer to the list of buffers on this level. Enlarge
		 * buffersOnLevels array if needed.
		 */
		if (level >= gfbb->buffersOnLevelsLen)
		{
			int			i;

			gfbb->buffersOnLevels =
				(List **) repalloc(gfbb->buffersOnLevels,
								   (level + 1) * sizeof(List *));

			/* initialize the enlarged portion */
			for (i = gfbb->buffersOnLevelsLen; i <= level; i++)
				gfbb->buffersOnLevels[i] = NIL;
			gfbb->buffersOnLevelsLen = level + 1;
		}

		/*
		 * Prepend the new buffer to the list of buffers on this level. It's
		 * not arbitrary that the new buffer is put to the beginning of the
		 * list: in the final emptying phase we loop through all buffers at
		 * each level, and flush them. If a page is split during the emptying,
		 * it's more efficient to flush the new splitted pages first, before
		 * moving on to pre-existing pages on the level. The buffers just
		 * created during the page split are likely still in cache, so
		 * flushing them immediately is more efficient than putting them to
		 * the end of the queue.
		 */
		gfbb->buffersOnLevels[level] = lcons(nodeBuffer,
											 gfbb->buffersOnLevels[level]);

		MemoryContextSwitchTo(oldcxt);
	}

	return nodeBuffer;
}

/*
 * Allocate memory for a buffer page.
 */
static GISTNodeBufferPage *
gistAllocateNewPageBuffer(GISTBuildBuffers *gfbb)
{
	GISTNodeBufferPage *pageBuffer;

	pageBuffer = (GISTNodeBufferPage *) MemoryContextAlloc(gfbb->context,
														   BLCKSZ);
	pageBuffer->prev = InvalidBlockNumber;

	/* Set page free space */
	PAGE_FREE_SPACE(pageBuffer) = BLCKSZ - BUFFER_PAGE_DATA_OFFSET;
	return pageBuffer;
}

/*
 * Add specified buffer into loadedBuffers array.
 */
static void
gistAddLoadedBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer)
{
	/* Never add a temporary buffer to the array */
	if (nodeBuffer->isTemp)
		return;

	/* Enlarge the array if needed */
	if (gfbb->loadedBuffersCount >= gfbb->loadedBuffersLen)
	{
		gfbb->loadedBuffersLen *= 2;
		gfbb->loadedBuffers = (GISTNodeBuffer **)
			repalloc(gfbb->loadedBuffers,
					 gfbb->loadedBuffersLen * sizeof(GISTNodeBuffer *));
	}

	gfbb->loadedBuffers[gfbb->loadedBuffersCount] = nodeBuffer;
	gfbb->loadedBuffersCount++;
}

/*
 * Load last page of node buffer into main memory.
 */
static void
gistLoadNodeBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer)
{
	/* Check if we really should load something */
	if (!nodeBuffer->pageBuffer && nodeBuffer->blocksCount > 0)
	{
		/* Allocate memory for page */
		nodeBuffer->pageBuffer = gistAllocateNewPageBuffer(gfbb);

		/* Read block from temporary file */
		ReadTempFileBlock(gfbb->pfile, nodeBuffer->pageBlocknum,
						  nodeBuffer->pageBuffer);

		/* Mark file block as free */
		gistBuffersReleaseBlock(gfbb, nodeBuffer->pageBlocknum);

		/* Mark node buffer as loaded */
		gistAddLoadedBuffer(gfbb, nodeBuffer);
		nodeBuffer->pageBlocknum = InvalidBlockNumber;
	}
}

/*
 * Write last page of node buffer to the disk.
 */
static void
gistUnloadNodeBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer)
{
	/* Check if we have something to write */
	if (nodeBuffer->pageBuffer)
	{
		BlockNumber blkno;

		/* Get free file block */
		blkno = gistBuffersGetFreeBlock(gfbb);

		/* Write block to the temporary file */
		WriteTempFileBlock(gfbb->pfile, blkno, nodeBuffer->pageBuffer);

		/* Free memory of that page */
		pfree(nodeBuffer->pageBuffer);
		nodeBuffer->pageBuffer = NULL;

		/* Save block number */
		nodeBuffer->pageBlocknum = blkno;
	}
}

/*
 * Write last pages of all node buffers to the disk.
 */
void
gistUnloadNodeBuffers(GISTBuildBuffers *gfbb)
{
	int			i;

	/* Unload all the buffers that have a page loaded in memory. */
	for (i = 0; i < gfbb->loadedBuffersCount; i++)
		gistUnloadNodeBuffer(gfbb, gfbb->loadedBuffers[i]);

	/* Now there are no node buffers with loaded last page */
	gfbb->loadedBuffersCount = 0;
}

/*
 * Add index tuple to buffer page.
 */
static void
gistPlaceItupToPage(GISTNodeBufferPage *pageBuffer, IndexTuple itup)
{
	Size		itupsz = IndexTupleSize(itup);
	char	   *ptr;

	/* There should be enough of space. */
	Assert(PAGE_FREE_SPACE(pageBuffer) >= MAXALIGN(itupsz));

	/* Reduce free space value of page to reserve a spot for the tuple. */
	PAGE_FREE_SPACE(pageBuffer) -= MAXALIGN(itupsz);

	/* Get pointer to the spot we reserved (ie. end of free space). */
	ptr = (char *) pageBuffer + BUFFER_PAGE_DATA_OFFSET
		+ PAGE_FREE_SPACE(pageBuffer);

	/* Copy the index tuple there. */
	memcpy(ptr, itup, itupsz);
}

/*
 * Get last item from buffer page and remove it from page.
 */
static void
gistGetItupFromPage(GISTNodeBufferPage *pageBuffer, IndexTuple *itup)
{
	IndexTuple	ptr;
	Size		itupsz;

	Assert(!PAGE_IS_EMPTY(pageBuffer)); /* Page shouldn't be empty */

	/* Get pointer to last index tuple */
	ptr = (IndexTuple) ((char *) pageBuffer
						+ BUFFER_PAGE_DATA_OFFSET
						+ PAGE_FREE_SPACE(pageBuffer));
	itupsz = IndexTupleSize(ptr);

	/* Make a copy of the tuple */
	*itup = (IndexTuple) palloc(itupsz);
	memcpy(*itup, ptr, itupsz);

	/* Mark the space used by the tuple as free */
	PAGE_FREE_SPACE(pageBuffer) += MAXALIGN(itupsz);
}

/*
 * Push an index tuple to node buffer.
 */
void
gistPushItupToNodeBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer,
						 IndexTuple itup)
{
	/*
	 * Most part of memory operations will be in buffering build persistent
	 * context. So, let's switch to it.
	 */
	MemoryContext oldcxt = MemoryContextSwitchTo(gfbb->context);

	/*
	 * If the buffer is currently empty, create the first page.
	 */
	if (nodeBuffer->blocksCount == 0)
	{
		nodeBuffer->pageBuffer = gistAllocateNewPageBuffer(gfbb);
		nodeBuffer->blocksCount = 1;
		gistAddLoadedBuffer(gfbb, nodeBuffer);
	}

	/* Load last page of node buffer if it wasn't in memory already */
	if (!nodeBuffer->pageBuffer)
		gistLoadNodeBuffer(gfbb, nodeBuffer);

	/*
	 * Check if there is enough space on the last page for the tuple.
	 */
	if (PAGE_NO_SPACE(nodeBuffer->pageBuffer, itup))
	{
		/*
		 * Nope. Swap previous block to disk and allocate a new one.
		 */
		BlockNumber blkno;

		/* Write filled page to the disk */
		blkno = gistBuffersGetFreeBlock(gfbb);
		WriteTempFileBlock(gfbb->pfile, blkno, nodeBuffer->pageBuffer);

		/*
		 * Reset the in-memory page as empty, and link the previous block to
		 * the new page by storing its block number in the prev-link.
		 */
		PAGE_FREE_SPACE(nodeBuffer->pageBuffer) =
			BLCKSZ - MAXALIGN(offsetof(GISTNodeBufferPage, tupledata));
		nodeBuffer->pageBuffer->prev = blkno;

		/* We've just added one more page */
		nodeBuffer->blocksCount++;
	}

	gistPlaceItupToPage(nodeBuffer->pageBuffer, itup);

	/*
	 * If the buffer just overflowed, add it to the emptying queue.
	 */
	if (BUFFER_HALF_FILLED(nodeBuffer, gfbb) && !nodeBuffer->queuedForEmptying)
	{
		gfbb->bufferEmptyingQueue = lcons(nodeBuffer,
										  gfbb->bufferEmptyingQueue);
		nodeBuffer->queuedForEmptying = true;
	}

	/* Restore memory context */
	MemoryContextSwitchTo(oldcxt);
}

/*
 * Removes one index tuple from node buffer. Returns true if success and false
 * if node buffer is empty.
 */
bool
gistPopItupFromNodeBuffer(GISTBuildBuffers *gfbb, GISTNodeBuffer *nodeBuffer,
						  IndexTuple *itup)
{
	/*
	 * If node buffer is empty then return false.
	 */
	if (nodeBuffer->blocksCount <= 0)
		return false;

	/* Load last page of node buffer if needed */
	if (!nodeBuffer->pageBuffer)
		gistLoadNodeBuffer(gfbb, nodeBuffer);

	/*
	 * Get index tuple from last non-empty page.
	 */
	gistGetItupFromPage(nodeBuffer->pageBuffer, itup);

	/*
	 * If we just removed the last tuple from the page, fetch previous page on
	 * this node buffer (if any).
	 */
	if (PAGE_IS_EMPTY(nodeBuffer->pageBuffer))
	{
		BlockNumber prevblkno;

		/*
		 * blocksCount includes the page in pageBuffer, so decrease it now.
		 */
		nodeBuffer->blocksCount--;

		/*
		 * If there's more pages, fetch previous one.
		 */
		prevblkno = nodeBuffer->pageBuffer->prev;
		if (prevblkno != InvalidBlockNumber)
		{
			/* There is a previous page. Fetch it. */
			Assert(nodeBuffer->blocksCount > 0);
			ReadTempFileBlock(gfbb->pfile, prevblkno, nodeBuffer->pageBuffer);

			/*
			 * Now that we've read the block in memory, we can release its
			 * on-disk block for reuse.
			 */
			gistBuffersReleaseBlock(gfbb, prevblkno);
		}
		else
		{
			/* No more pages. Free memory. */
			Assert(nodeBuffer->blocksCount == 0);
			pfree(nodeBuffer->pageBuffer);
			nodeBuffer->pageBuffer = NULL;
		}
	}
	return true;
}

/*
 * Select a currently unused block for writing to.
 */
static long
gistBuffersGetFreeBlock(GISTBuildBuffers *gfbb)
{
	/*
	 * If there are multiple free blocks, we select the one appearing last in
	 * freeBlocks[].  If there are none, assign the next block at the end of
	 * the file (causing the file to be extended).
	 */
	if (gfbb->nFreeBlocks > 0)
		return gfbb->freeBlocks[--gfbb->nFreeBlocks];
	else
		return gfbb->nFileBlocks++;
}

/*
 * Return a block# to the freelist.
 */
static void
gistBuffersReleaseBlock(GISTBuildBuffers *gfbb, long blocknum)
{
	int			ndx;

	/* Enlarge freeBlocks array if full. */
	if (gfbb->nFreeBlocks >= gfbb->freeBlocksLen)
	{
		gfbb->freeBlocksLen *= 2;
		gfbb->freeBlocks = (long *) repalloc(gfbb->freeBlocks,
											 gfbb->freeBlocksLen *
											 sizeof(long));
	}

	/* Add blocknum to array */
	ndx = gfbb->nFreeBlocks++;
	gfbb->freeBlocks[ndx] = blocknum;
}

/*
 * Free buffering build data structure.
 */
void
gistFreeBuildBuffers(GISTBuildBuffers *gfbb)
{
	/* Close buffers file. */
	BufFileClose(gfbb->pfile);

	/* All other things will be freed on memory context release */
}

/*
 * Data structure representing information about node buffer for index tuples
 * relocation from splitted node buffer.
 */
typedef struct
{
	GISTENTRY	entry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	GISTPageSplitInfo *splitinfo;
	GISTNodeBuffer *nodeBuffer;
} RelocationBufferInfo;

/*
 * At page split, distribute tuples from the buffer of the split page to
 * new buffers for the created page halves. This also adjusts the downlinks
 * in 'splitinfo' to include the tuples in the buffers.
 */
void
gistRelocateBuildBuffersOnSplit(GISTBuildBuffers *gfbb, GISTSTATE *giststate,
								Relation r, int level,
								Buffer buffer, List *splitinfo)
{
	RelocationBufferInfo *relocationBuffersInfos;
	bool		found;
	GISTNodeBuffer *nodeBuffer;
	BlockNumber blocknum;
	IndexTuple	itup;
	int			splitPagesCount = 0,
				i;
	GISTENTRY	entry[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	GISTNodeBuffer oldBuf;
	ListCell   *lc;

	/* If the splitted page doesn't have buffers, we have nothing to do. */
	if (!LEVEL_HAS_BUFFERS(level, gfbb))
		return;

	/*
	 * Get the node buffer of the splitted page.
	 */
	blocknum = BufferGetBlockNumber(buffer);
	nodeBuffer = hash_search(gfbb->nodeBuffersTab, &blocknum,
							 HASH_FIND, &found);
	if (!found)
	{
		/* The page has no buffer, so we have nothing to do. */
		return;
	}

	/*
	 * Make a copy of the old buffer, as we're going reuse it as the buffer
	 * for the new left page, which is on the same block as the old page.
	 * That's not true for the root page, but that's fine because we never
	 * have a buffer on the root page anyway. The original algorithm as
	 * described by Arge et al did, but it's of no use, as you might as well
	 * read the tuples straight from the heap instead of the root buffer.
	 */
	Assert(blocknum != GIST_ROOT_BLKNO);
	memcpy(&oldBuf, nodeBuffer, sizeof(GISTNodeBuffer));
	oldBuf.isTemp = true;

	/* Reset the old buffer, used for the new left page from now on */
	nodeBuffer->blocksCount = 0;
	nodeBuffer->pageBuffer = NULL;
	nodeBuffer->pageBlocknum = InvalidBlockNumber;

	/*
	 * Allocate memory for information about relocation buffers.
	 */
	splitPagesCount = list_length(splitinfo);
	relocationBuffersInfos =
		(RelocationBufferInfo *) palloc(sizeof(RelocationBufferInfo) *
										splitPagesCount);

	/*
	 * Fill relocation buffers information for node buffers of pages produced
	 * by split.
	 */
	i = 0;
	foreach(lc, splitinfo)
	{
		GISTPageSplitInfo *si = (GISTPageSplitInfo *) lfirst(lc);
		GISTNodeBuffer *newNodeBuffer;

		/* Decompress parent index tuple of node buffer page. */
		gistDeCompressAtt(giststate, r,
						  si->downlink, NULL, (OffsetNumber) 0,
						  relocationBuffersInfos[i].entry,
						  relocationBuffersInfos[i].isnull);

		/*
		 * Create a node buffer for the page. The leftmost half is on the same
		 * block as the old page before split, so for the leftmost half this
		 * will return the original buffer. The tuples on the original buffer
		 * were relinked to the temporary buffer, so the original one is now
		 * empty.
		 */
		newNodeBuffer = gistGetNodeBuffer(gfbb, giststate, BufferGetBlockNumber(si->buf), level);

		relocationBuffersInfos[i].nodeBuffer = newNodeBuffer;
		relocationBuffersInfos[i].splitinfo = si;

		i++;
	}

	/*
	 * Loop through all index tuples on the buffer on the splitted page,
	 * moving them to buffers on the new pages.
	 */
	while (gistPopItupFromNodeBuffer(gfbb, &oldBuf, &itup))
	{
		float		sum_grow,
					which_grow[INDEX_MAX_KEYS];
		int			i,
					which;
		IndexTuple	newtup;
		RelocationBufferInfo *targetBufferInfo;

		/*
		 * Choose which page this tuple should go to.
		 */
		gistDeCompressAtt(giststate, r,
						  itup, NULL, (OffsetNumber) 0, entry, isnull);

		which = -1;
		*which_grow = -1.0f;
		sum_grow = 1.0f;

		for (i = 0; i < splitPagesCount && sum_grow; i++)
		{
			int			j;
			RelocationBufferInfo *splitPageInfo = &relocationBuffersInfos[i];

			sum_grow = 0.0f;
			for (j = 0; j < r->rd_att->natts; j++)
			{
				float		usize;

				usize = gistpenalty(giststate, j,
									&splitPageInfo->entry[j],
									splitPageInfo->isnull[j],
									&entry[j], isnull[j]);

				if (which_grow[j] < 0 || usize < which_grow[j])
				{
					which = i;
					which_grow[j] = usize;
					if (j < r->rd_att->natts - 1 && i == 0)
						which_grow[j + 1] = -1;
					sum_grow += which_grow[j];
				}
				else if (which_grow[j] == usize)
					sum_grow += usize;
				else
				{
					sum_grow = 1;
					break;
				}
			}
		}
		targetBufferInfo = &relocationBuffersInfos[which];

		/* Push item to selected node buffer */
		gistPushItupToNodeBuffer(gfbb, targetBufferInfo->nodeBuffer, itup);

		/* Adjust the downlink for this page, if needed. */
		newtup = gistgetadjusted(r, targetBufferInfo->splitinfo->downlink,
								 itup, giststate);
		if (newtup)
		{
			gistDeCompressAtt(giststate, r,
							  newtup, NULL, (OffsetNumber) 0,
							  targetBufferInfo->entry,
							  targetBufferInfo->isnull);

			targetBufferInfo->splitinfo->downlink = newtup;
		}
	}

	pfree(relocationBuffersInfos);
}


/*
 * Wrappers around BufFile operations. The main difference is that these
 * wrappers report errors with ereport(), so that the callers don't need
 * to check the return code.
 */

static void
ReadTempFileBlock(BufFile *file, long blknum, void *ptr)
{
	if (BufFileSeekBlock(file, blknum) != 0)
		elog(ERROR, "could not seek temporary file: %m");
	if (BufFileRead(file, ptr, BLCKSZ) != BLCKSZ)
		elog(ERROR, "could not read temporary file: %m");
}

static void
WriteTempFileBlock(BufFile *file, long blknum, void *ptr)
{
	if (BufFileSeekBlock(file, blknum) != 0)
		elog(ERROR, "could not seek temporary file: %m");
	if (BufFileWrite(file, ptr, BLCKSZ) != BLCKSZ)
	{
		/*
		 * the other errors in Read/WriteTempFileBlock shouldn't happen, but
		 * an error at write can easily happen if you run out of disk space.
		 */
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write block %ld of temporary file: %m",
						blknum)));
	}
}
