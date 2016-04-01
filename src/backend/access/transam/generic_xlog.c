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
 * Unchanged regions of a page are not represented in its delta.  As a
 * result, a delta can be more compact than the full page image.  But having
 * an unchanged region in the middle of two fragments that is smaller than
 * the fragment header (offset and length) does not pay off in terms of the
 * overall size of the delta. For this reason, we break fragments only if
 * the unchanged region is bigger than MATCH_THRESHOLD.
 *
 * The worst case for delta sizes occurs when we did not find any unchanged
 * region in the page.  The size of the delta will be the size of the page plus
 * the size of the fragment header in that case.
 *-------------------------------------------------------------------------
 */
#define FRAGMENT_HEADER_SIZE	(2 * sizeof(OffsetNumber))
#define MATCH_THRESHOLD			FRAGMENT_HEADER_SIZE
#define MAX_DELTA_SIZE			BLCKSZ + FRAGMENT_HEADER_SIZE

/* Struct of generic xlog data for single page */
typedef struct
{
	Buffer	buffer;			/* registered buffer */
	char	image[BLCKSZ];	/* copy of page image for modification */
	char	data[MAX_DELTA_SIZE]; /* delta between page images */
	int		dataLen;		/* space consumed in data field */
	bool	fullImage;		/* are we taking a full image of this page? */
} PageData;

/* State of generic xlog record construction */
struct GenericXLogState
{
	bool		isLogged;
	PageData	pages[MAX_GENERIC_XLOG_PAGES];
};

static void writeFragment(PageData *pageData, OffsetNumber offset,
						  OffsetNumber len, Pointer data);
static void writeDelta(PageData *pageData);
static void applyPageRedo(Page page, Pointer data, Size dataSize);

/*
 * Write next fragment into delta.
 */
static void
writeFragment(PageData *pageData, OffsetNumber offset, OffsetNumber length,
			  Pointer data)
{
	Pointer			ptr = pageData->data + pageData->dataLen;

	/* Check if we have enough space */
	Assert(pageData->dataLen + sizeof(offset) +
		   sizeof(length) + length <= sizeof(pageData->data));

	/* Write fragment data */
	memcpy(ptr, &offset, sizeof(offset));
	ptr += sizeof(offset);
	memcpy(ptr, &length, sizeof(length));
	ptr += sizeof(length);
	memcpy(ptr, data, length);
	ptr += length;

	pageData->dataLen = ptr - pageData->data;
}

/*
 * Make delta for given page.
 */
static void
writeDelta(PageData *pageData)
{
	Page			page = BufferGetPage(pageData->buffer),
					image = (Page) pageData->image;
	int				i,
					fragmentBegin = -1,
					fragmentEnd = -1;
	uint16			pageLower = ((PageHeader) page)->pd_lower,
					pageUpper = ((PageHeader) page)->pd_upper,
					imageLower = ((PageHeader) image)->pd_lower,
					imageUpper = ((PageHeader) image)->pd_upper;

	for (i = 0; i < BLCKSZ; i++)
	{
		bool	match;

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

#ifdef WAL_DEBUG
	/*
	 * If xlog debug is enabled, then check produced delta.  Result of delta
	 * application to saved image should be the same as current page state.
	 */
	if (XLOG_DEBUG)
	{
		char	tmp[BLCKSZ];
		memcpy(tmp, image, BLCKSZ);
		applyPageRedo(tmp, pageData->data, pageData->dataLen);
		if (memcmp(tmp, page, pageLower)
			|| memcmp(tmp + pageUpper, page + pageUpper, BLCKSZ - pageUpper))
			elog(ERROR, "result of generic xlog apply does not match");
	}
#endif
}

/*
 * Start new generic xlog record.
 */
GenericXLogState *
GenericXLogStart(Relation relation)
{
	int					i;
	GenericXLogState   *state;

	state = (GenericXLogState *) palloc(sizeof(GenericXLogState));

	state->isLogged = RelationNeedsWAL(relation);
	for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		state->pages[i].buffer = InvalidBuffer;

	return state;
}

/*
 * Register new buffer for generic xlog record.
 */
Page
GenericXLogRegister(GenericXLogState *state, Buffer buffer, bool isNew)
{
	int block_id;

	/* Place new buffer to unused slot in array */
	for (block_id = 0; block_id < MAX_GENERIC_XLOG_PAGES; block_id++)
	{
		PageData *page = &state->pages[block_id];
		if (BufferIsInvalid(page->buffer))
		{
			page->buffer = buffer;
			memcpy(page->image, BufferGetPage(buffer), BLCKSZ);
			page->dataLen = 0;
			page->fullImage = isNew;
			return (Page)page->image;
		}
		else if (page->buffer == buffer)
		{
			/*
			 * Buffer is already registered.  Just return the image, which is
			 * already prepared.
			 */
			return (Page)page->image;
		}
	}

	elog(ERROR, "maximum number of %d generic xlog buffers is exceeded",
		 MAX_GENERIC_XLOG_PAGES);

	/* keep compiler quiet */
	return NULL;
}

/*
 * Unregister particular buffer for generic xlog record.
 */
void
GenericXLogUnregister(GenericXLogState *state, Buffer buffer)
{
	int block_id;

	/* Find block in array to unregister */
	for (block_id = 0; block_id < MAX_GENERIC_XLOG_PAGES; block_id++)
	{
		if (state->pages[block_id].buffer == buffer)
		{
			/*
			 * Preserve order of pages in array because it could matter for
			 * concurrency.
			 */
			memmove(&state->pages[block_id], &state->pages[block_id + 1],
					(MAX_GENERIC_XLOG_PAGES - block_id - 1) * sizeof(PageData));
			state->pages[MAX_GENERIC_XLOG_PAGES - 1].buffer = InvalidBuffer;
			return;
		}
	}

	elog(ERROR, "registered generic xlog buffer not found");
}

/*
 * Put all changes in registered buffers to generic xlog record.
 */
XLogRecPtr
GenericXLogFinish(GenericXLogState *state)
{
	XLogRecPtr lsn = InvalidXLogRecPtr;
	int i;

	if (state->isLogged)
	{
		/* Logged relation: make xlog record in critical section. */
		XLogBeginInsert();

		START_CRIT_SECTION();

		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			char		tmp[BLCKSZ];
			PageData   *page = &state->pages[i];

			if (BufferIsInvalid(page->buffer))
				continue;

			/* Swap current and saved page image. */
			memcpy(tmp, page->image, BLCKSZ);
			memcpy(page->image, BufferGetPage(page->buffer), BLCKSZ);
			memcpy(BufferGetPage(page->buffer), tmp, BLCKSZ);

			if (page->fullImage)
			{
				/* A full page image does not require anything special */
				XLogRegisterBuffer(i, page->buffer, REGBUF_FORCE_IMAGE);
			}
			else
			{
				/*
				 * In normal mode, calculate delta and write it as data
				 * associated with this page.
				 */
				XLogRegisterBuffer(i, page->buffer, REGBUF_STANDARD);
				writeDelta(page);
				XLogRegisterBufData(i, page->data, page->dataLen);
			}
		}

		/* Insert xlog record */
		lsn = XLogInsert(RM_GENERIC_ID, 0);

		/* Set LSN and mark buffers dirty */
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *page = &state->pages[i];

			if (BufferIsInvalid(page->buffer))
				continue;
			PageSetLSN(BufferGetPage(page->buffer), lsn);
			MarkBufferDirty(page->buffer);
		}
		END_CRIT_SECTION();
	}
	else
	{
		/* Unlogged relation: skip xlog-related stuff */
		START_CRIT_SECTION();
		for (i = 0; i < MAX_GENERIC_XLOG_PAGES; i++)
		{
			PageData   *page = &state->pages[i];

			if (BufferIsInvalid(page->buffer))
				continue;
			memcpy(BufferGetPage(page->buffer), page->image, BLCKSZ);
			MarkBufferDirty(page->buffer);
		}
		END_CRIT_SECTION();
	}

	pfree(state);

	return lsn;
}

/*
 * Abort generic xlog record.
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
applyPageRedo(Page page, Pointer data, Size dataSize)
{
	Pointer ptr = data, end = data + dataSize;

	while (ptr < end)
	{
		OffsetNumber	offset,
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
	uint8		block_id;
	Buffer		buffers[MAX_GENERIC_XLOG_PAGES] = {InvalidBuffer};
	XLogRecPtr	lsn = record->EndRecPtr;

	Assert(record->max_block_id < MAX_GENERIC_XLOG_PAGES);

	/* Iterate over blocks */
	for (block_id = 0; block_id <= record->max_block_id; block_id++)
	{
		XLogRedoAction action;

		if (!XLogRecHasBlockRef(record, block_id))
			continue;

		action = XLogReadBufferForRedo(record, block_id, &buffers[block_id]);

		/* Apply redo to given block if needed */
		if (action == BLK_NEEDS_REDO)
		{
			Pointer	blockData;
			Size	blockDataSize;
			Page	page;

			page = BufferGetPage(buffers[block_id]);
			blockData = XLogRecGetBlockData(record, block_id, &blockDataSize);
			applyPageRedo(page, blockData, blockDataSize);

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
