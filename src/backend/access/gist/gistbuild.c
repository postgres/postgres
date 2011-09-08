/*-------------------------------------------------------------------------
 *
 * gistbuild.c
 *	  build algorithm for GiST indexes implementation.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/gist/gistbuild.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "optimizer/cost.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
#include "utils/memutils.h"
#include "utils/rel.h"

/* Step of index tuples for check whether to switch to buffering build mode */
#define BUFFERING_MODE_SWITCH_CHECK_STEP 256

/*
 * Number of tuples to process in the slow way before switching to buffering
 * mode, when buffering is explicitly turned on. Also, the number of tuples
 * to process between readjusting the buffer size parameter, while in
 * buffering mode.
 */
#define BUFFERING_MODE_TUPLE_SIZE_STATS_TARGET 4096

typedef enum
{
	GIST_BUFFERING_DISABLED,	/* in regular build mode and aren't going to
								 * switch */
	GIST_BUFFERING_AUTO,		/* in regular build mode, but will switch to
								 * buffering build mode if the index grows too
								 * big */
	GIST_BUFFERING_STATS,		/* gathering statistics of index tuple size
								 * before switching to the buffering build
								 * mode */
	GIST_BUFFERING_ACTIVE		/* in buffering build mode */
}	GistBufferingMode;

/* Working state for gistbuild and its callback */
typedef struct
{
	Relation	indexrel;
	GISTSTATE	giststate;
	GISTBuildBuffers *gfbb;

	int64		indtuples;		/* number of tuples indexed */
	int64		indtuplesSize;	/* total size of all indexed tuples */

	Size		freespace;		/* amount of free space to leave on pages */

	GistBufferingMode bufferingMode;
	MemoryContext tmpCtx;
} GISTBuildState;

static void gistInitBuffering(GISTBuildState *buildstate);
static int	calculatePagesPerBuffer(GISTBuildState *buildstate, int levelStep);
static void gistBuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state);
static void gistBufferingBuildInsert(GISTBuildState *buildstate,
						 IndexTuple itup);
static bool gistProcessItup(GISTBuildState *buildstate, IndexTuple itup,
				GISTBufferingInsertStack *startparent);
static void gistbufferinginserttuples(GISTBuildState *buildstate,
						  Buffer buffer,
						  IndexTuple *itup, int ntup, OffsetNumber oldoffnum,
						  GISTBufferingInsertStack *path);
static void gistBufferingFindCorrectParent(GISTBuildState *buildstate,
							   GISTBufferingInsertStack *child);
static void gistProcessEmptyingQueue(GISTBuildState *buildstate);
static void gistEmptyAllBuffers(GISTBuildState *buildstate);
static void gistFreeUnreferencedPath(GISTBufferingInsertStack *path);
static int	gistGetMaxLevel(Relation index);


/*
 * Main entry point to GiST index build. Initially calls insert over and over,
 * but switches to more efficient buffering build algorithm after a certain
 * number of tuples (unless buffering mode is disabled).
 */
Datum
gistbuild(PG_FUNCTION_ARGS)
{
	Relation	heap = (Relation) PG_GETARG_POINTER(0);
	Relation	index = (Relation) PG_GETARG_POINTER(1);
	IndexInfo  *indexInfo = (IndexInfo *) PG_GETARG_POINTER(2);
	IndexBuildResult *result;
	double		reltuples;
	GISTBuildState buildstate;
	Buffer		buffer;
	Page		page;
	MemoryContext oldcxt = CurrentMemoryContext;
	int			fillfactor;

	buildstate.indexrel = index;
	if (index->rd_options)
	{
		/* Get buffering mode from the options string */
		GiSTOptions *options = (GiSTOptions *) index->rd_options;
		char	   *bufferingMode = (char *) options + options->bufferingModeOffset;

		if (strcmp(bufferingMode, "on") == 0)
			buildstate.bufferingMode = GIST_BUFFERING_STATS;
		else if (strcmp(bufferingMode, "off") == 0)
			buildstate.bufferingMode = GIST_BUFFERING_DISABLED;
		else
			buildstate.bufferingMode = GIST_BUFFERING_AUTO;

		fillfactor = options->fillfactor;
	}
	else
	{
		/*
		 * By default, switch to buffering mode when the index grows too large
		 * to fit in cache.
		 */
		buildstate.bufferingMode = GIST_BUFFERING_AUTO;
		fillfactor = GIST_DEFAULT_FILLFACTOR;
	}
	/* Calculate target amount of free space to leave on pages */
	buildstate.freespace = BLCKSZ * (100 - fillfactor) / 100;

	/*
	 * We expect to be called exactly once for any index relation. If that's
	 * not the case, big trouble's what we have.
	 */
	if (RelationGetNumberOfBlocks(index) != 0)
		elog(ERROR, "index \"%s\" already contains data",
			 RelationGetRelationName(index));

	/* no locking is needed */
	initGISTstate(&buildstate.giststate, index);

	/* initialize the root page */
	buffer = gistNewBuffer(index);
	Assert(BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO);
	page = BufferGetPage(buffer);

	START_CRIT_SECTION();

	GISTInitBuffer(buffer, F_LEAF);

	MarkBufferDirty(buffer);

	if (RelationNeedsWAL(index))
	{
		XLogRecPtr	recptr;
		XLogRecData rdata;

		rdata.data = (char *) &(index->rd_node);
		rdata.len = sizeof(RelFileNode);
		rdata.buffer = InvalidBuffer;
		rdata.next = NULL;

		recptr = XLogInsert(RM_GIST_ID, XLOG_GIST_CREATE_INDEX, &rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
	}
	else
		PageSetLSN(page, GetXLogRecPtrForTemp());

	UnlockReleaseBuffer(buffer);

	END_CRIT_SECTION();

	/* build the index */
	buildstate.indtuples = 0;
	buildstate.indtuplesSize = 0;

	/*
	 * create a temporary memory context that is reset once for each tuple
	 * processed.
	 */
	buildstate.tmpCtx = createTempGistContext();

	/*
	 * Do the heap scan.
	 */
	reltuples = IndexBuildHeapScan(heap, index, indexInfo, true,
								   gistBuildCallback, (void *) &buildstate);

	/*
	 * If buffering was used, flush out all the tuples that are still in the
	 * buffers.
	 */
	if (buildstate.bufferingMode == GIST_BUFFERING_ACTIVE)
	{
		elog(DEBUG1, "all tuples processed, emptying buffers");
		gistEmptyAllBuffers(&buildstate);
	}

	/* okay, all heap tuples are indexed */
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(buildstate.tmpCtx);

	freeGISTstate(&buildstate.giststate);

	/*
	 * Return statistics
	 */
	result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = (double) buildstate.indtuples;

	PG_RETURN_POINTER(result);
}

/*
 * Validator for "buffering" reloption on GiST indexes. Allows "on", "off"
 * and "auto" values.
 */
void
gistValidateBufferingOption(char *value)
{
	if (value == NULL ||
		(strcmp(value, "on") != 0 &&
		 strcmp(value, "off") != 0 &&
		 strcmp(value, "auto") != 0))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid value for \"buffering\" option"),
			   errdetail("Valid values are \"on\", \"off\" and \"auto\".")));
	}
}

/*
 * Attempt to switch to buffering mode.
 *
 * If there is not enough memory for buffering build, sets bufferingMode
 * to GIST_BUFFERING_DISABLED, so that we don't bother to try the switch
 * anymore. Otherwise initializes the build buffers, and sets bufferingMode to
 * GIST_BUFFERING_ACTIVE.
 */
static void
gistInitBuffering(GISTBuildState *buildstate)
{
	Relation	index = buildstate->indexrel;
	int			pagesPerBuffer;
	Size		pageFreeSpace;
	Size		itupAvgSize,
				itupMinSize;
	double		avgIndexTuplesPerPage,
				maxIndexTuplesPerPage;
	int			i;
	int			levelStep;

	/* Calc space of index page which is available for index tuples */
	pageFreeSpace = BLCKSZ - SizeOfPageHeaderData - sizeof(GISTPageOpaqueData)
		- sizeof(ItemIdData)
		- buildstate->freespace;

	/*
	 * Calculate average size of already inserted index tuples using gathered
	 * statistics.
	 */
	itupAvgSize = (double) buildstate->indtuplesSize /
		(double) buildstate->indtuples;

	/*
	 * Calculate minimal possible size of index tuple by index metadata.
	 * Minimal possible size of varlena is VARHDRSZ.
	 *
	 * XXX: that's not actually true, as a short varlen can be just 2 bytes.
	 * And we should take padding into account here.
	 */
	itupMinSize = (Size) MAXALIGN(sizeof(IndexTupleData));
	for (i = 0; i < index->rd_att->natts; i++)
	{
		if (index->rd_att->attrs[i]->attlen < 0)
			itupMinSize += VARHDRSZ;
		else
			itupMinSize += index->rd_att->attrs[i]->attlen;
	}

	/* Calculate average and maximal number of index tuples which fit to page */
	avgIndexTuplesPerPage = pageFreeSpace / itupAvgSize;
	maxIndexTuplesPerPage = pageFreeSpace / itupMinSize;

	/*
	 * We need to calculate two parameters for the buffering algorithm:
	 * levelStep and pagesPerBuffer.
	 *
	 * levelStep determines the size of subtree that we operate on, while
	 * emptying a buffer. A higher value is better, as you need fewer buffer
	 * emptying steps to build the index. However, if you set it too high, the
	 * subtree doesn't fit in cache anymore, and you quickly lose the benefit
	 * of the buffers.
	 *
	 * In Arge et al's paper, levelStep is chosen as logB(M/4B), where B is
	 * the number of tuples on page (ie. fanout), and M is the amount of
	 * internal memory available. Curiously, they doesn't explain *why* that
	 * setting is optimal. We calculate it by taking the highest levelStep so
	 * that a subtree still fits in cache. For a small B, our way of
	 * calculating levelStep is very close to Arge et al's formula. For a
	 * large B, our formula gives a value that is 2x higher.
	 *
	 * The average size of a subtree of depth n can be calculated as a
	 * geometric series:
	 *
	 * B^0 + B^1 + B^2 + ... + B^n = (1 - B^(n + 1)) / (1 - B)
	 *
	 * where B is the average number of index tuples on page. The subtree is
	 * cached in the shared buffer cache and the OS cache, so we choose
	 * levelStep so that the subtree size is comfortably smaller than
	 * effective_cache_size, with a safety factor of 4.
	 *
	 * The estimate on the average number of index tuples on page is based on
	 * average tuple sizes observed before switching to buffered build, so the
	 * real subtree size can be somewhat larger. Also, it would selfish to
	 * gobble the whole cache for our index build. The safety factor of 4
	 * should account for those effects.
	 *
	 * The other limiting factor for setting levelStep is that while
	 * processing a subtree, we need to hold one page for each buffer at the
	 * next lower buffered level. The max. number of buffers needed for that
	 * is maxIndexTuplesPerPage^levelStep. This is very conservative, but
	 * hopefully maintenance_work_mem is set high enough that you're
	 * constrained by effective_cache_size rather than maintenance_work_mem.
	 *
	 * XXX: the buffer hash table consumes a fair amount of memory too per
	 * buffer, but that is not currently taken into account. That scales on
	 * the total number of buffers used, ie. the index size and on levelStep.
	 * Note that a higher levelStep *reduces* the amount of memory needed for
	 * the hash table.
	 */
	levelStep = 1;
	while (
	/* subtree must fit in cache (with safety factor of 4) */
		   (1 - pow(avgIndexTuplesPerPage, (double) (levelStep + 1))) / (1 - avgIndexTuplesPerPage) < effective_cache_size / 4
		   &&
	/* each node in the lowest level of a subtree has one page in memory */
		   (pow(maxIndexTuplesPerPage, (double) levelStep) < (maintenance_work_mem * 1024) / BLCKSZ)
		)
	{
		levelStep++;
	}

	/*
	 * We just reached an unacceptable value of levelStep in previous loop.
	 * So, decrease levelStep to get last acceptable value.
	 */
	levelStep--;

	/*
	 * If there's not enough cache or maintenance_work_mem, fall back to plain
	 * inserts.
	 */
	if (levelStep <= 0)
	{
		elog(DEBUG1, "failed to switch to buffered GiST build");
		buildstate->bufferingMode = GIST_BUFFERING_DISABLED;
		return;
	}

	/*
	 * The second parameter to set is pagesPerBuffer, which determines the
	 * size of each buffer. We adjust pagesPerBuffer also during the build,
	 * which is why this calculation is in a separate function.
	 */
	pagesPerBuffer = calculatePagesPerBuffer(buildstate, levelStep);

	/* Initialize GISTBuildBuffers with these parameters */
	buildstate->gfbb = gistInitBuildBuffers(pagesPerBuffer, levelStep,
											gistGetMaxLevel(index));

	buildstate->bufferingMode = GIST_BUFFERING_ACTIVE;

	elog(DEBUG1, "switched to buffered GiST build; level step = %d, pagesPerBuffer = %d",
		 levelStep, pagesPerBuffer);
}

/*
 * Calculate pagesPerBuffer parameter for the buffering algorithm.
 *
 * Buffer size is chosen so that assuming that tuples are distributed
 * randomly, emptying half a buffer fills on average one page in every buffer
 * at the next lower level.
 */
static int
calculatePagesPerBuffer(GISTBuildState *buildstate, int levelStep)
{
	double		pagesPerBuffer;
	double		avgIndexTuplesPerPage;
	double		itupAvgSize;
	Size		pageFreeSpace;

	/* Calc space of index page which is available for index tuples */
	pageFreeSpace = BLCKSZ - SizeOfPageHeaderData - sizeof(GISTPageOpaqueData)
		- sizeof(ItemIdData)
		- buildstate->freespace;

	/*
	 * Calculate average size of already inserted index tuples using gathered
	 * statistics.
	 */
	itupAvgSize = (double) buildstate->indtuplesSize /
		(double) buildstate->indtuples;

	avgIndexTuplesPerPage = pageFreeSpace / itupAvgSize;

	/*
	 * Recalculate required size of buffers.
	 */
	pagesPerBuffer = 2 * pow(avgIndexTuplesPerPage, levelStep);

	return round(pagesPerBuffer);
}

/*
 * Per-tuple callback from IndexBuildHeapScan.
 */
static void
gistBuildCallback(Relation index,
				  HeapTuple htup,
				  Datum *values,
				  bool *isnull,
				  bool tupleIsAlive,
				  void *state)
{
	GISTBuildState *buildstate = (GISTBuildState *) state;
	IndexTuple	itup;
	MemoryContext oldCtx;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/* form an index tuple and point it at the heap tuple */
	itup = gistFormTuple(&buildstate->giststate, index, values, isnull, true);
	itup->t_tid = htup->t_self;

	if (buildstate->bufferingMode == GIST_BUFFERING_ACTIVE)
	{
		/* We have buffers, so use them. */
		gistBufferingBuildInsert(buildstate, itup);
	}
	else
	{
		/*
		 * There's no buffers (yet). Since we already have the index relation
		 * locked, we call gistdoinsert directly.
		 */
		gistdoinsert(index, itup, buildstate->freespace,
					 &buildstate->giststate);
	}

	/* Update tuple count and total size. */
	buildstate->indtuples += 1;
	buildstate->indtuplesSize += IndexTupleSize(itup);

	MemoryContextSwitchTo(oldCtx);
	MemoryContextReset(buildstate->tmpCtx);

	if (buildstate->bufferingMode == GIST_BUFFERING_ACTIVE &&
		buildstate->indtuples % BUFFERING_MODE_TUPLE_SIZE_STATS_TARGET == 0)
	{
		/* Adjust the target buffer size now */
		buildstate->gfbb->pagesPerBuffer =
			calculatePagesPerBuffer(buildstate, buildstate->gfbb->levelStep);
	}

	/*
	 * In 'auto' mode, check if the index has grown too large to fit in cache,
	 * and switch to buffering mode if it has.
	 *
	 * To avoid excessive calls to smgrnblocks(), only check this every
	 * BUFFERING_MODE_SWITCH_CHECK_STEP index tuples
	 */
	if ((buildstate->bufferingMode == GIST_BUFFERING_AUTO &&
		 buildstate->indtuples % BUFFERING_MODE_SWITCH_CHECK_STEP == 0 &&
		 effective_cache_size < smgrnblocks(index->rd_smgr, MAIN_FORKNUM)) ||
		(buildstate->bufferingMode == GIST_BUFFERING_STATS &&
		 buildstate->indtuples >= BUFFERING_MODE_TUPLE_SIZE_STATS_TARGET))
	{
		/*
		 * Index doesn't fit in effective cache anymore. Try to switch to
		 * buffering build mode.
		 */
		gistInitBuffering(buildstate);
	}
}

/*
 * Insert function for buffering index build.
 */
static void
gistBufferingBuildInsert(GISTBuildState *buildstate, IndexTuple itup)
{
	/* Insert the tuple to buffers. */
	gistProcessItup(buildstate, itup, NULL);

	/* If we filled up (half of a) buffer, process buffer emptying. */
	gistProcessEmptyingQueue(buildstate);
}

/*
 * Process an index tuple. Runs the tuple down the tree until we reach a leaf
 * page or node buffer, and inserts the tuple there. Returns true if we have
 * to stop buffer emptying process (because one of child buffers can't take
 * index tuples anymore).
 */
static bool
gistProcessItup(GISTBuildState *buildstate, IndexTuple itup,
				GISTBufferingInsertStack *startparent)
{
	GISTSTATE  *giststate = &buildstate->giststate;
	GISTBuildBuffers *gfbb = buildstate->gfbb;
	Relation	indexrel = buildstate->indexrel;
	GISTBufferingInsertStack *path;
	BlockNumber childblkno;
	Buffer		buffer;
	bool		result = false;

	/*
	 * NULL passed in startparent means that we start index tuple processing
	 * from the root.
	 */
	if (!startparent)
		path = gfbb->rootitem;
	else
		path = startparent;

	/*
	 * Loop until we reach a leaf page (level == 0) or a level with buffers
	 * (not including the level we start at, because we would otherwise make
	 * no progress).
	 */
	for (;;)
	{
		ItemId		iid;
		IndexTuple	idxtuple,
					newtup;
		Page		page;
		OffsetNumber childoffnum;
		GISTBufferingInsertStack *parent;

		/* Have we reached a level with buffers? */
		if (LEVEL_HAS_BUFFERS(path->level, gfbb) && path != startparent)
			break;

		/* Have we reached a leaf page? */
		if (path->level == 0)
			break;

		/*
		 * Nope. Descend down to the next level then. Choose a child to
		 * descend down to.
		 */
		buffer = ReadBuffer(indexrel, path->blkno);
		LockBuffer(buffer, GIST_EXCLUSIVE);

		page = (Page) BufferGetPage(buffer);
		childoffnum = gistchoose(indexrel, page, itup, giststate);
		iid = PageGetItemId(page, childoffnum);
		idxtuple = (IndexTuple) PageGetItem(page, iid);
		childblkno = ItemPointerGetBlockNumber(&(idxtuple->t_tid));

		/*
		 * Check that the key representing the target child node is consistent
		 * with the key we're inserting. Update it if it's not.
		 */
		newtup = gistgetadjusted(indexrel, idxtuple, itup, giststate);
		if (newtup)
			gistbufferinginserttuples(buildstate, buffer, &newtup, 1,
									  childoffnum, path);
		UnlockReleaseBuffer(buffer);

		/* Create new path item representing current page */
		parent = path;
		path = (GISTBufferingInsertStack *) MemoryContextAlloc(gfbb->context,
										   sizeof(GISTBufferingInsertStack));
		path->parent = parent;
		path->level = parent->level - 1;
		path->blkno = childblkno;
		path->downlinkoffnum = childoffnum;
		path->refCount = 0;		/* it's unreferenced for now */

		/* Adjust reference count of parent */
		if (parent)
			parent->refCount++;
	}

	if (LEVEL_HAS_BUFFERS(path->level, gfbb))
	{
		/*
		 * We've reached level with buffers. Place the index tuple to the
		 * buffer, and add the buffer to the emptying queue if it overflows.
		 */
		GISTNodeBuffer *childNodeBuffer;

		/* Find the buffer or create a new one */
		childNodeBuffer = gistGetNodeBuffer(gfbb, giststate, path->blkno,
										 path->downlinkoffnum, path->parent);

		/* Add index tuple to it */
		gistPushItupToNodeBuffer(gfbb, childNodeBuffer, itup);

		if (BUFFER_OVERFLOWED(childNodeBuffer, gfbb))
			result = true;
	}
	else
	{
		/*
		 * We've reached a leaf page. Place the tuple here.
		 */
		buffer = ReadBuffer(indexrel, path->blkno);
		LockBuffer(buffer, GIST_EXCLUSIVE);
		gistbufferinginserttuples(buildstate, buffer, &itup, 1,
								  InvalidOffsetNumber, path);
		UnlockReleaseBuffer(buffer);
	}

	/*
	 * Free unreferenced path items, if any. Path item may be referenced by
	 * node buffer.
	 */
	gistFreeUnreferencedPath(path);

	return result;
}

/*
 * Insert tuples to a given page.
 *
 * This is analogous with gistinserttuples() in the regular insertion code.
 */
static void
gistbufferinginserttuples(GISTBuildState *buildstate, Buffer buffer,
						  IndexTuple *itup, int ntup, OffsetNumber oldoffnum,
						  GISTBufferingInsertStack *path)
{
	GISTBuildBuffers *gfbb = buildstate->gfbb;
	List	   *splitinfo;
	bool		is_split;

	is_split = gistplacetopage(buildstate->indexrel,
							   buildstate->freespace,
							   &buildstate->giststate,
							   buffer,
							   itup, ntup, oldoffnum,
							   InvalidBuffer,
							   &splitinfo,
							   false);

	/*
	 * If this is a root split, update the root path item kept in memory. This
	 * ensures that all path stacks are always complete, including all parent
	 * nodes up to the root. That simplifies the algorithm to re-find correct
	 * parent.
	 */
	if (is_split && BufferGetBlockNumber(buffer) == GIST_ROOT_BLKNO)
	{
		GISTBufferingInsertStack *oldroot = gfbb->rootitem;
		Page		page = BufferGetPage(buffer);
		ItemId		iid;
		IndexTuple	idxtuple;
		BlockNumber leftmostchild;

		gfbb->rootitem = (GISTBufferingInsertStack *) MemoryContextAlloc(
							gfbb->context, sizeof(GISTBufferingInsertStack));
		gfbb->rootitem->parent = NULL;
		gfbb->rootitem->blkno = GIST_ROOT_BLKNO;
		gfbb->rootitem->downlinkoffnum = InvalidOffsetNumber;
		gfbb->rootitem->level = oldroot->level + 1;
		gfbb->rootitem->refCount = 1;

		/*
		 * All the downlinks on the old root page are now on one of the child
		 * pages. Change the block number of the old root entry in the stack
		 * to point to the leftmost child. The other child pages will be
		 * accessible from there by walking right.
		 */
		iid = PageGetItemId(page, FirstOffsetNumber);
		idxtuple = (IndexTuple) PageGetItem(page, iid);
		leftmostchild = ItemPointerGetBlockNumber(&(idxtuple->t_tid));

		oldroot->parent = gfbb->rootitem;
		oldroot->blkno = leftmostchild;
		oldroot->downlinkoffnum = InvalidOffsetNumber;
	}

	if (splitinfo)
	{
		/*
		 * Insert the downlinks to the parent. This is analogous with
		 * gistfinishsplit() in the regular insertion code, but the locking is
		 * simpler, and we have to maintain the buffers.
		 */
		IndexTuple *downlinks;
		int			ndownlinks,
					i;
		Buffer		parentBuffer;
		ListCell   *lc;

		/* Parent may have changed since we memorized this path. */
		gistBufferingFindCorrectParent(buildstate, path);

		/*
		 * If there's a buffer associated with this page, that needs to be
		 * split too. gistRelocateBuildBuffersOnSplit() will also adjust the
		 * downlinks in 'splitinfo', to make sure they're consistent not only
		 * with the tuples already on the pages, but also the tuples in the
		 * buffers that will eventually be inserted to them.
		 */
		gistRelocateBuildBuffersOnSplit(gfbb,
										&buildstate->giststate,
										buildstate->indexrel,
										path, buffer, splitinfo);

		/* Create an array of all the downlink tuples */
		ndownlinks = list_length(splitinfo);
		downlinks = (IndexTuple *) palloc(sizeof(IndexTuple) * ndownlinks);
		i = 0;
		foreach(lc, splitinfo)
		{
			GISTPageSplitInfo *splitinfo = lfirst(lc);

			/*
			 * Since there's no concurrent access, we can release the lower
			 * level buffers immediately. Don't release the buffer for the
			 * original page, though, because the caller will release that.
			 */
			if (splitinfo->buf != buffer)
				UnlockReleaseBuffer(splitinfo->buf);
			downlinks[i++] = splitinfo->downlink;
		}

		/* Insert them into parent. */
		parentBuffer = ReadBuffer(buildstate->indexrel, path->parent->blkno);
		LockBuffer(parentBuffer, GIST_EXCLUSIVE);
		gistbufferinginserttuples(buildstate, parentBuffer,
								  downlinks, ndownlinks,
								  path->downlinkoffnum, path->parent);
		UnlockReleaseBuffer(parentBuffer);

		list_free_deep(splitinfo);		/* we don't need this anymore */
	}
}

/*
 * Find correct parent by following rightlinks in buffering index build. This
 * method of parent searching is possible because no concurrent activity is
 * possible while index builds.
 */
static void
gistBufferingFindCorrectParent(GISTBuildState *buildstate,
							   GISTBufferingInsertStack *child)
{
	GISTBuildBuffers *gfbb = buildstate->gfbb;
	Relation	indexrel = buildstate->indexrel;
	GISTBufferingInsertStack *parent = child->parent;
	OffsetNumber i,
				maxoff;
	ItemId		iid;
	IndexTuple	idxtuple;
	Buffer		buffer;
	Page		page;
	bool		copied = false;

	buffer = ReadBuffer(indexrel, parent->blkno);
	page = BufferGetPage(buffer);
	LockBuffer(buffer, GIST_EXCLUSIVE);
	gistcheckpage(indexrel, buffer);

	/* Check if it was not moved */
	if (child->downlinkoffnum != InvalidOffsetNumber &&
		child->downlinkoffnum <= PageGetMaxOffsetNumber(page))
	{
		iid = PageGetItemId(page, child->downlinkoffnum);
		idxtuple = (IndexTuple) PageGetItem(page, iid);
		if (ItemPointerGetBlockNumber(&(idxtuple->t_tid)) == child->blkno)
		{
			/* Still there */
			UnlockReleaseBuffer(buffer);
			return;
		}
	}

	/* parent has changed, look child in right links until found */
	while (true)
	{
		/* Search for relevant downlink in the current page */
		maxoff = PageGetMaxOffsetNumber(page);
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			iid = PageGetItemId(page, i);
			idxtuple = (IndexTuple) PageGetItem(page, iid);
			if (ItemPointerGetBlockNumber(&(idxtuple->t_tid)) == child->blkno)
			{
				/* yes!!, found */
				child->downlinkoffnum = i;
				UnlockReleaseBuffer(buffer);
				return;
			}
		}

		/*
		 * We should copy parent path item because some other path items can
		 * refer to it.
		 */
		if (!copied)
		{
			parent = (GISTBufferingInsertStack *) MemoryContextAlloc(gfbb->context,
										   sizeof(GISTBufferingInsertStack));
			memcpy(parent, child->parent, sizeof(GISTBufferingInsertStack));
			if (parent->parent)
				parent->parent->refCount++;
			gistDecreasePathRefcount(child->parent);
			child->parent = parent;
			parent->refCount = 1;
			copied = true;
		}

		/*
		 * Not found in current page. Move towards rightlink.
		 */
		parent->blkno = GistPageGetOpaque(page)->rightlink;
		UnlockReleaseBuffer(buffer);

		if (parent->blkno == InvalidBlockNumber)
		{
			/*
			 * End of chain and still didn't find parent. Should not happen
			 * during index build.
			 */
			break;
		}

		/* Get the next page */
		buffer = ReadBuffer(indexrel, parent->blkno);
		page = BufferGetPage(buffer);
		LockBuffer(buffer, GIST_EXCLUSIVE);
		gistcheckpage(indexrel, buffer);
	}

	elog(ERROR, "failed to re-find parent for block %u", child->blkno);
}

/*
 * Process buffers emptying stack. Emptying of one buffer can cause emptying
 * of other buffers. This function iterates until this cascading emptying
 * process finished, e.g. until buffers emptying stack is empty.
 */
static void
gistProcessEmptyingQueue(GISTBuildState *buildstate)
{
	GISTBuildBuffers *gfbb = buildstate->gfbb;

	/* Iterate while we have elements in buffers emptying stack. */
	while (gfbb->bufferEmptyingQueue != NIL)
	{
		GISTNodeBuffer *emptyingNodeBuffer;

		/* Get node buffer from emptying stack. */
		emptyingNodeBuffer = (GISTNodeBuffer *) linitial(gfbb->bufferEmptyingQueue);
		gfbb->bufferEmptyingQueue = list_delete_first(gfbb->bufferEmptyingQueue);
		emptyingNodeBuffer->queuedForEmptying = false;

		/*
		 * We are going to load last pages of buffers where emptying will be
		 * to. So let's unload any previously loaded buffers.
		 */
		gistUnloadNodeBuffers(gfbb);

		/*
		 * Pop tuples from the buffer and run them down to the buffers at
		 * lower level, or leaf pages. We continue until one of the lower
		 * level buffers fills up, or this buffer runs empty.
		 *
		 * In Arge et al's paper, the buffer emptying is stopped after
		 * processing 1/2 node buffer worth of tuples, to avoid overfilling
		 * any of the lower level buffers. However, it's more efficient to
		 * keep going until one of the lower level buffers actually fills up,
		 * so that's what we do. This doesn't need to be exact, if a buffer
		 * overfills by a few tuples, there's no harm done.
		 */
		while (true)
		{
			IndexTuple	itup;

			/* Get next index tuple from the buffer */
			if (!gistPopItupFromNodeBuffer(gfbb, emptyingNodeBuffer, &itup))
				break;

			/*
			 * Run it down to the underlying node buffer or leaf page.
			 *
			 * Note: it's possible that the buffer we're emptying splits as a
			 * result of this call. If that happens, our emptyingNodeBuffer
			 * points to the left half of the split. After split, it's very
			 * likely that the new left buffer is no longer over the half-full
			 * threshold, but we might as well keep flushing tuples from it
			 * until we fill a lower-level buffer.
			 */
			if (gistProcessItup(buildstate, itup, emptyingNodeBuffer->path))
			{
				/*
				 * A lower level buffer filled up. Stop emptying this buffer,
				 * to avoid overflowing the lower level buffer.
				 */
				break;
			}

			/* Free all the memory allocated during index tuple processing */
			MemoryContextReset(CurrentMemoryContext);
		}
	}
}

/*
 * Empty all node buffers, from top to bottom. This is done at the end of
 * index build to flush all remaining tuples to the index.
 *
 * Note: This destroys the buffersOnLevels lists, so the buffers should not
 * be inserted to after this call.
 */
static void
gistEmptyAllBuffers(GISTBuildState *buildstate)
{
	GISTBuildBuffers *gfbb = buildstate->gfbb;
	MemoryContext oldCtx;
	int			i;

	oldCtx = MemoryContextSwitchTo(buildstate->tmpCtx);

	/*
	 * Iterate through the levels from top to bottom.
	 */
	for (i = gfbb->buffersOnLevelsLen - 1; i >= 0; i--)
	{
		/*
		 * Empty all buffers on this level. Note that new buffers can pop up
		 * in the list during the processing, as a result of page splits, so a
		 * simple walk through the list won't work. We remove buffers from the
		 * list when we see them empty; a buffer can't become non-empty once
		 * it's been fully emptied.
		 */
		while (gfbb->buffersOnLevels[i] != NIL)
		{
			GISTNodeBuffer *nodeBuffer;

			nodeBuffer = (GISTNodeBuffer *) linitial(gfbb->buffersOnLevels[i]);

			if (nodeBuffer->blocksCount != 0)
			{
				/*
				 * Add this buffer to the emptying queue, and proceed to empty
				 * the queue.
				 */
				MemoryContextSwitchTo(gfbb->context);
				gfbb->bufferEmptyingQueue =
					lcons(nodeBuffer, gfbb->bufferEmptyingQueue);
				MemoryContextSwitchTo(buildstate->tmpCtx);
				gistProcessEmptyingQueue(buildstate);
			}
			else
				gfbb->buffersOnLevels[i] =
					list_delete_first(gfbb->buffersOnLevels[i]);
		}
	}
	MemoryContextSwitchTo(oldCtx);
}

/*
 * Free unreferenced parts of a path stack.
 */
static void
gistFreeUnreferencedPath(GISTBufferingInsertStack *path)
{
	while (path->refCount == 0)
	{
		/*
		 * Path part is unreferenced. We can free it and decrease reference
		 * count of parent. If parent becomes unreferenced too procedure
		 * should be repeated for it.
		 */
		GISTBufferingInsertStack *tmp = path->parent;

		pfree(path);
		path = tmp;
		if (path)
			path->refCount--;
		else
			break;
	}
}

/*
 * Decrease reference count of a path part, and free any unreferenced parts of
 * the path stack.
 */
void
gistDecreasePathRefcount(GISTBufferingInsertStack *path)
{
	path->refCount--;
	gistFreeUnreferencedPath(path);
}

/*
 * Get the depth of the GiST index.
 */
static int
gistGetMaxLevel(Relation index)
{
	int			maxLevel;
	BlockNumber blkno;

	/*
	 * Traverse down the tree, starting from the root, until we hit the leaf
	 * level.
	 */
	maxLevel = 0;
	blkno = GIST_ROOT_BLKNO;
	while (true)
	{
		Buffer		buffer;
		Page		page;
		IndexTuple	itup;

		buffer = ReadBuffer(index, blkno);

		/*
		 * There's no concurrent access during index build, so locking is just
		 * pro forma.
		 */
		LockBuffer(buffer, GIST_SHARE);
		page = (Page) BufferGetPage(buffer);

		if (GistPageIsLeaf(page))
		{
			/* We hit the bottom, so we're done. */
			UnlockReleaseBuffer(buffer);
			break;
		}

		/*
		 * Pick the first downlink on the page, and follow it. It doesn't
		 * matter which downlink we choose, the tree has the same depth
		 * everywhere, so we just pick the first one.
		 */
		itup = (IndexTuple) PageGetItem(page,
									 PageGetItemId(page, FirstOffsetNumber));
		blkno = ItemPointerGetBlockNumber(&(itup->t_tid));
		UnlockReleaseBuffer(buffer);

		/*
		 * We're going down on the tree. It means that there is yet one more
		 * level is the tree.
		 */
		maxLevel++;
	}
	return maxLevel;
}
