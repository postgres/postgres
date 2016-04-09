/*-------------------------------------------------------------------------
 *
 * generic_xlog.c
 *	 Implementation of generic xlog records.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/generic_xlog.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/generic_xlog.h"
#include "access/xlogutils.h"
#include "miscadmin.h"
#include "utils/memutils.h"

/*-------------------------------------------------------------------------
 * Internally, a delta between pages consists of a set of fragments.  Each
 * fragment represents changes made in a given region of a page.  A fragment
 * is made up as follows:
 *
 * - offset of page region (OffsetNumber)
 * - length of page region (OffsetNumber)
 * - data - the data to place into the region ('length' number of bytes)
 *
 * Unchanged regions of a page are not represented in its delta.  As a result,
 * a delta can be more compact than the full page image.  But having an
 * unchanged region between two fragments that is smaller than the fragment
 * header (offset+length) does not pay off in terms of the overall size of
 * the delta.  For this reason, we merge adjacent fragments if the unchanged
 * region between them is <= MATCH_THRESHOLD bytes.
 *
 * The worst case for delta sizes occurs when we did not find any unchanged
 * region in the page.  The size of the delta will be the size of the page plus
 * the size of the fragment header in that case.
 *-------------------------------------------------------------------------
 */
#define FRAGMENT_HEADER_SIZE	(2 * sizeof(OffsetNumber))
#define MATCH_THRESHOLD			FRAGMENT_HEADER_SIZE
#define MAX_DELTA_SIZE			(BLCKSZ + FRAGMENT_HEADER_SIZE)

/* Struct of generic xlog data for single page */
typedef struct
{
	Buffer		buffer;			/* registered buffer */
	bool		fullImage;		/* are we taking a full image of this page? */
	int			deltaLen;		/* space consumed in delta field */
	char		image[BLCKSZ];	/* copy of page image for modification */
	char		delta[MAX_DELTA_SIZE];	/* delta between page images */
} PageData;

/* State of generic xlog record construction */
struct GenericXLogState
{
	bool		isLogged;
	PageData	pages[MAX_GENERIC_XLOG_PAGES];
};

static void writeFragment(PageData *pageData, OffsetNumber offset,
			  OffsetNumber len, const char *data);
static void computeDelta(PageData *pageData);
static void applyPageRedo(Page page, const char *delta, Size deltaSize);


/*
 * Write next fragment into pageData's delta.
 *
 * The fragment has the given offset and length, and data points to the
 * actual data (of length length).
 */
static void
writeFragment(PageData *pageData, OffsetNumber offset, OffsetNumber length,
			  const char *data)
{
	char	   *ptr = pageData->delta + pageData->deltaLen;

	/* Verify we have enough space */
	Assert(pageData->deltaLen + sizeof(offset) +
		   sizeof(length) + length <= sizeof(pageData->delta));

	/* Write fragment data */
	memcpy(ptr, &offset, sizeof(offset));
	ptr += sizeof(offset);
	memcpy(ptr, &length, sizeof(length));
	ptr += sizeof(length);
	memcpy(ptr, data, length);
	ptr += length;

	pageData->deltaLen = ptr - pageData->delta;
}

/*
 * Compute the delta record for given page.
 */
static void
computeDelta(PageData *pageData)
{
	Page		page = BufferGetPage(pageData->buffer, NULL, NULL,
									 BGP_NO_SNAPSHOT_TEST),
				image = (Page) pageData->image;
	int			i,
				fragmentBegin = -1,
				fragmentEnd = -1;
	uint16		pageLower = ((PageHeader) page)->pd_lower,
				pageUpper = ((PageHeader) page)->pd_upper,
				imageLower = ((PageHeader) image)->pd_lower,
				imageUpper = ((PageHeader) image)->pd_upper;

	pageData->deltaLen = 0;

	for (i = 0; i < BLCKSZ; i++)
	{
		bool		match;

		/*
		 * Check if bytes in old and new page images match.  We do not care
		 * about data in the unallocated area between pd_lower and pd_upper.
		 * We assume the unallocated area to expand with unmatched bytes.
		 * Bytes inside the unallocated area are assumed to always match.
		 */
		if (i < pageLower)
		{
			if (i < imageLower)
				match = (page[i] == image[i]);
			else
				match = false;
		}
		else if (i >= pageUpper)
		{
			if (i >= imageUpper)
				match = (page[i] == image[i]);
			else
				match = false;
		}
		else
		{
			match = true;
		}

		if (match)
		{
			if (fragmentBegin >= 0)
			{
				/* Matched byte is potentially part of a fragment. */
				if (fragmentEnd < 0)
					fragmentEnd = i;

				/*
				 * Write next fragment if sequence of matched bytes is longer
				 * than MATCH_THRESHOLD.
				 */
				if (i - fragmentEnd >= MATCH_THRESHOLD)
				{
					writeFragment(pageData, fragmentBegin,
								  fragmentEnd - fragmentBegin,
								  page + fragmentBegin);
					fragmentBegin = -1;
					fragmentEnd = -1;
				}
			}
		}
		else
		{
			/* On unmatched byte, start new fragment if it is not done yet */
			if (fragmentBegin < 0)
				fragmentBegin = i;
			fragmentEnd = -1;
		}
	}

	if (fragmentBegin >= 0)
		writeFragment(pageData, fragmentBegin,
					  BLCKSZ - fragmentBegin,
					  page + fragmentBegin);

	/*
	 * If xlog debug is enabled, then check produced delta.  Result of delta
	 * application to saved image should be the same as current page state.
	 */
#ifdef WAL_DEBUG
	if (XLOG_DEBUG)
	{
		char		tmp[BLCKSZ];

		memcpy(tmp, image, BLCKSZ);
		applyPageRedo(tmp, pageData->delta, pageData->deltaLen);
		if (memcmp(tmp, page, pageLower) != 0 ||
		  memcmp(tmp + pageUpper, page + pageUpper, BLCKSZ - pageUpper) != 0)
			elog(ERROR, "result of generic xlog apply does not match");
	}
#endif
}

/*
 * Start new generic xlog record for modifications to specified relation.
 */
GenericXLogState *
GenericXLogStart(Relation relation)
{
	GenericXLogState *state;
	int			i;

	state = (GenericXLogState *) palloc(sizeof(GenericXLogState));

	state->isLogged = RelationNeedsWAL(relation);
	for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		state->pages[i].buffer = InvalidBuffer;

	return state;
}

/*
 * Register new buffer for generic xlog record.
 *
 * Returns pointer to the page's image in the GenericXLogState, which
 * is what the caller should modify.
 *
 * If the buffer is already registered, just return its existing entry.
 */
Page
GenericXLogRegister(GenericXLogState *state, Buffer buffer, bool isNew)
{
	int			block_id;

	/* Search array for existing entry or first unused slot */
	for (block_id = 0; block_id < MAX_GENERIC_XLOG_PAGES; block_id++)
	{
		PageData   *page = &state->pages[block_id];

		if (BufferIsInvalid(page->buffer))
		{
			/* Empty slot, so use it (there cannot be a match later) */
			page->buffer = buffer;
			page->fullImage = isNew;
			memcpy(page->image,
				   BufferGetPage(buffer, NULL, NULL, BGP_NO_SNAPSHOT_TEST),
				   BLCKSZ);
			return (Page) page->image;
		}
		else if (page->buffer == buffer)
		{
			/*
			 * Buffer is already registered.  Just return the image, which is
			 * already prepared.
			 */
			return (Page) page->image;
		}
	}

	elog(ERROR, "maximum number %d of generic xlog buffers is exceeded",
		 MAX_GENERIC_XLOG_PAGES);
	/* keep compiler quiet */
	return NULL;
}

/*
 * Apply changes represented by GenericXLogState to the actual buffers,
 * and emit a generic xlog record.
 */
XLogRecPtr
GenericXLogFinish(GenericXLogState *state)
{
	XLogRecPtr	lsn = InvalidXLogRecPtr;
	int			i;

	if (state->isLogged)
	{
		/* Logged relation: make xlog record in critical section. */
		XLogBeginInsert();

		START_CRIT_SECTION();

		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];
			Page		page;
			char		tmp[BLCKSZ];

			if (BufferIsInvalid(pageData->buffer))
				continue;

			page = BufferGetPage(pageData->buffer, NULL, NULL,
								 BGP_NO_SNAPSHOT_TEST);

			/* Swap current and saved page image. */
			memcpy(tmp, pageData->image, BLCKSZ);
			memcpy(pageData->image, page, BLCKSZ);
			memcpy(page, tmp, BLCKSZ);

			if (pageData->fullImage)
			{
				/* A full page image does not require anything special */
				XLogRegisterBuffer(i, pageData->buffer, REGBUF_FORCE_IMAGE);
			}
			else
			{
				/*
				 * In normal mode, calculate delta and write it as xlog data
				 * associated with this page.
				 */
				XLogRegisterBuffer(i, pageData->buffer, REGBUF_STANDARD);
				computeDelta(pageData);
				XLogRegisterBufData(i, pageData->delta, pageData->deltaLen);
			}
		}

		/* Insert xlog record */
		lsn = XLogInsert(RM_GENERIC_ID, 0);

		/* Set LSN and mark buffers dirty */
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];

			if (BufferIsInvalid(pageData->buffer))
				continue;
			PageSetLSN(BufferGetPage(pageData->buffer, NULL, NULL,
									 BGP_NO_SNAPSHOT_TEST), lsn);
			MarkBufferDirty(pageData->buffer);
		}
		END_CRIT_SECTION();
	}
	else
	{
		/* Unlogged relation: skip xlog-related stuff */
		START_CRIT_SECTION();
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *pageData = &state->pages[i];

			if (BufferIsInvalid(pageData->buffer))
				continue;
			memcpy(BufferGetPage(pageData->buffer, NULL, NULL,
								 BGP_NO_SNAPSHOT_TEST),
				   pageData->image,
				   BLCKSZ);
			MarkBufferDirty(pageData->buffer);
		}
		END_CRIT_SECTION();
	}

	pfree(state);

	return lsn;
}

/*
 * Abort generic xlog record construction.  No changes are applied to buffers.
 *
 * Note: caller is responsible for releasing locks/pins on buffers, if needed.
 */
void
GenericXLogAbort(GenericXLogState *state)
{
	pfree(state);
}

/*
 * Apply delta to given page image.
 */
static void
applyPageRedo(Page page, const char *delta, Size deltaSize)
{
	const char *ptr = delta;
	const char *end = delta + deltaSize;

	while (ptr < end)
	{
		OffsetNumber offset,
					length;

		memcpy(&offset, ptr, sizeof(offset));
		ptr += sizeof(offset);
		memcpy(&length, ptr, sizeof(length));
		ptr += sizeof(length);

		memcpy(page + offset, ptr, length);

		ptr += length;
	}
}

/*
 * Redo function for generic xlog record.
 */
void
generic_redo(XLogReaderState *record)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	Buffer		buffers[MAX_GENERIC_XLOG_PAGES];
	uint8		block_id;

	/* Protect limited size of buffers[] array */
	Assert(record->max_block_id < MAX_GENERIC_XLOG_PAGES);

	/* Iterate over blocks */
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		XLogRedoAction action;

		if (!XLogRecHasBlockRef(record, block_id))
		{
			buffers[block_id] = InvalidBuffer;
			continue;
		}

		action = XLogReadBufferForRedo(record, block_id, &buffers[block_id]);

		/* Apply redo to given block if needed */
		if (action == BLK_NEEDS_REDO)
		{
			Page		page;
			char	   *blockDelta;
			Size		blockDeltaSize;

			page = BufferGetPage(buffers[block_id], NULL, NULL,
								 BGP_NO_SNAPSHOT_TEST);
			blockDelta = XLogRecGetBlockData(record, block_id, &blockDeltaSize);
			applyPageRedo(page, blockDelta, blockDeltaSize);

			PageSetLSN(page, lsn);
			MarkBufferDirty(buffers[block_id]);
		}
	}

	/* Changes are done: unlock and release all buffers */
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		if (BufferIsValid(buffers[block_id]))
			UnlockReleaseBuffer(buffers[block_id]);
	}
}
