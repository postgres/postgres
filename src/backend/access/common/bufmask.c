/*-------------------------------------------------------------------------
 *
 * bufmask.c
 *	  Routines for buffer masking. Used to mask certain bits
 *	  in a page which can be different when the WAL is generated
 *	  and when the WAL is applied.
 *
 * Portions Copyright (c) 2016-2019, PostgreSQL Global Development Group
 *
 * Contains common routines required for masking a page.
 *
 * IDENTIFICATION
 *	  src/backend/access/common/bufmask.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/bufmask.h"

/*
 * mask_page_lsn
 *
 * In consistency checks, the LSN of the two pages compared will likely be
 * different because of concurrent operations when the WAL is generated and
 * the state of the page when WAL is applied. Also, mask out checksum as
 * masking anything else on page means checksum is not going to match as well.
 */
void
mask_page_lsn_and_checksum(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	PageXLogRecPtrSet(phdr->pd_lsn, (uint64) MASK_MARKER);
	phdr->pd_checksum = MASK_MARKER;
}

/*
 * mask_page_hint_bits
 *
 * Mask hint bits in PageHeader. We want to ignore differences in hint bits,
 * since they can be set without emitting any WAL.
 */
void
mask_page_hint_bits(Page page)
{
	PageHeader	phdr = (PageHeader) page;

	/* Ignore prune_xid (it's like a hint-bit) */
	phdr->pd_prune_xid = MASK_MARKER;

	/* Ignore PD_PAGE_FULL and PD_HAS_FREE_LINES flags, they are just hints. */
	PageClearFull(page);
	PageClearHasFreeLinePointers(page);

	/*
	 * During replay, if the page LSN has advanced past our XLOG record's LSN,
	 * we don't mark the page all-visible. See heap_xlog_visible() for
	 * details.
	 */
	PageClearAllVisible(page);
}

/*
 * mask_unused_space
 *
 * Mask the unused space of a page between pd_lower and pd_upper.
 */
void
mask_unused_space(Page page)
{
	int			pd_lower = ((PageHeader) page)->pd_lower;
	int			pd_upper = ((PageHeader) page)->pd_upper;
	int			pd_special = ((PageHeader) page)->pd_special;

	/* Sanity check */
	if (pd_lower > pd_upper || pd_special < pd_upper ||
		pd_lower < SizeOfPageHeaderData || pd_special > BLCKSZ)
	{
		elog(ERROR, "invalid page pd_lower %u pd_upper %u pd_special %u",
			 pd_lower, pd_upper, pd_special);
	}

	memset(page + pd_lower, MASK_MARKER, pd_upper - pd_lower);
}

/*
 * mask_lp_flags
 *
 * In some index AMs, line pointer flags can be modified in master without
 * emitting any WAL record.
 */
void
mask_lp_flags(Page page)
{
	OffsetNumber offnum,
				maxoff;

	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		 offnum <= maxoff;
		 offnum = OffsetNumberNext(offnum))
	{
		ItemId		itemId = PageGetItemId(page, offnum);

		if (ItemIdIsUsed(itemId))
			itemId->lp_flags = LP_UNUSED;
	}
}

/*
 * mask_page_content
 *
 * In some index AMs, the contents of deleted pages need to be almost
 * completely ignored.
 */
void
mask_page_content(Page page)
{
	/* Mask Page Content */
	memset(page + SizeOfPageHeaderData, MASK_MARKER,
		   BLCKSZ - SizeOfPageHeaderData);

	/* Mask pd_lower and pd_upper */
	memset(&((PageHeader) page)->pd_lower, MASK_MARKER,
		   sizeof(uint16));
	memset(&((PageHeader) page)->pd_upper, MASK_MARKER,
		   sizeof(uint16));
}
