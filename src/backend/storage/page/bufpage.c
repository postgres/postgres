/*-------------------------------------------------------------------------
 *
 * bufpage.c
 *	  POSTGRES standard buffer page code.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/page/bufpage.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/itup.h"
#include "access/xlog.h"
#include "storage/checksum.h"
#include "utils/memdebug.h"
#include "utils/memutils.h"


/* GUC variable */
bool		ignore_checksum_failure = false;


/* ----------------------------------------------------------------
 *						Page support functions
 * ----------------------------------------------------------------
 */

/*
 * PageInit
 *		Initializes the contents of a page.
 *		Note that we don't calculate an initial checksum here; that's not done
 *		until it's time to write.
 */
void
PageInit(Page page, Size pageSize, Size specialSize)
{
	PageHeader	p = (PageHeader) page;

	specialSize = MAXALIGN(specialSize);

	Assert(pageSize == BLCKSZ);
	Assert(pageSize > specialSize + SizeOfPageHeaderData);

	/* Make sure all fields of page are zero, as well as unused space */
	MemSet(p, 0, pageSize);

	p->pd_flags = 0;
	p->pd_lower = SizeOfPageHeaderData;
	p->pd_upper = pageSize - specialSize;
	p->pd_special = pageSize - specialSize;
	PageSetPageSizeAndVersion(page, pageSize, PG_PAGE_LAYOUT_VERSION);
	/* p->pd_prune_xid = InvalidTransactionId;		done by above MemSet */
}


/*
 * PageIsVerified
 *		Check that the page header and checksum (if any) appear valid.
 *
 * This is called when a page has just been read in from disk.  The idea is
 * to cheaply detect trashed pages before we go nuts following bogus item
 * pointers, testing invalid transaction identifiers, etc.
 *
 * It turns out to be necessary to allow zeroed pages here too.  Even though
 * this routine is *not* called when deliberately adding a page to a relation,
 * there are scenarios in which a zeroed page might be found in a table.
 * (Example: a backend extends a relation, then crashes before it can write
 * any WAL entry about the new page.  The kernel will already have the
 * zeroed page in the file, and it will stay that way after restart.)  So we
 * allow zeroed pages here, and are careful that the page access macros
 * treat such a page as empty and without free space.  Eventually, VACUUM
 * will clean up such a page and make it usable.
 */
bool
PageIsVerified(Page page, BlockNumber blkno)
{
	PageHeader	p = (PageHeader) page;
	char	   *pagebytes;
	int			i;
	bool		checksum_failure = false;
	bool		header_sane = false;
	bool		all_zeroes = false;
	uint16		checksum = 0;

	/*
	 * Don't verify page data unless the page passes basic non-zero test
	 */
	if (!PageIsNew(page))
	{
		if (DataChecksumsEnabled())
		{
			checksum = pg_checksum_page((char *) page, blkno);

			if (checksum != p->pd_checksum)
				checksum_failure = true;
		}

		/*
		 * The following checks don't prove the header is correct, only that
		 * it looks sane enough to allow into the buffer pool. Later usage of
		 * the block can still reveal problems, which is why we offer the
		 * checksum option.
		 */
		if ((p->pd_flags & ~PD_VALID_FLAG_BITS) == 0 &&
			p->pd_lower <= p->pd_upper &&
			p->pd_upper <= p->pd_special &&
			p->pd_special <= BLCKSZ &&
			p->pd_special == MAXALIGN(p->pd_special))
			header_sane = true;

		if (header_sane && !checksum_failure)
			return true;
	}

	/* Check all-zeroes case */
	all_zeroes = true;
	pagebytes = (char *) page;
	for (i = 0; i < BLCKSZ; i++)
	{
		if (pagebytes[i] != 0)
		{
			all_zeroes = false;
			break;
		}
	}

	if (all_zeroes)
		return true;

	/*
	 * Throw a WARNING if the checksum fails, but only after we've checked for
	 * the all-zeroes case.
	 */
	if (checksum_failure)
	{
		ereport(WARNING,
				(ERRCODE_DATA_CORRUPTED,
				 errmsg("page verification failed, calculated checksum %u but expected %u",
						checksum, p->pd_checksum)));

		if (header_sane && ignore_checksum_failure)
			return true;
	}

	return false;
}


/*
 *	PageAddItem
 *
 *	Add an item to a page.  Return value is offset at which it was
 *	inserted, or InvalidOffsetNumber if there's not room to insert.
 *
 *	If overwrite is true, we just store the item at the specified
 *	offsetNumber (which must be either a currently-unused item pointer,
 *	or one past the last existing item).  Otherwise,
 *	if offsetNumber is valid and <= current max offset in the page,
 *	insert item into the array at that position by shuffling ItemId's
 *	down to make room.
 *	If offsetNumber is not valid, then assign one by finding the first
 *	one that is both unused and deallocated.
 *
 *	If is_heap is true, we enforce that there can't be more than
 *	MaxHeapTuplesPerPage line pointers on the page.
 *
 *	!!! EREPORT(ERROR) IS DISALLOWED HERE !!!
 */
OffsetNumber
PageAddItem(Page page,
			Item item,
			Size size,
			OffsetNumber offsetNumber,
			bool overwrite,
			bool is_heap)
{
	PageHeader	phdr = (PageHeader) page;
	Size		alignedSize;
	int			lower;
	int			upper;
	ItemId		itemId;
	OffsetNumber limit;
	bool		needshuffle = false;

	/*
	 * Be wary about corrupted page pointers
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
		ereport(PANIC,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	/*
	 * Select offsetNumber to place the new item at
	 */
	limit = OffsetNumberNext(PageGetMaxOffsetNumber(page));

	/* was offsetNumber passed in? */
	if (OffsetNumberIsValid(offsetNumber))
	{
		/* yes, check it */
		if (overwrite)
		{
			if (offsetNumber < limit)
			{
				itemId = PageGetItemId(phdr, offsetNumber);
				if (ItemIdIsUsed(itemId) || ItemIdHasStorage(itemId))
				{
					elog(WARNING, "will not overwrite a used ItemId");
					return InvalidOffsetNumber;
				}
			}
		}
		else
		{
			if (offsetNumber < limit)
				needshuffle = true;		/* need to move existing linp's */
		}
	}
	else
	{
		/* offsetNumber was not passed in, so find a free slot */
		/* if no free slot, we'll put it at limit (1st open slot) */
		if (PageHasFreeLinePointers(phdr))
		{
			/*
			 * Look for "recyclable" (unused) ItemId.  We check for no storage
			 * as well, just to be paranoid --- unused items should never have
			 * storage.
			 */
			for (offsetNumber = 1; offsetNumber < limit; offsetNumber++)
			{
				itemId = PageGetItemId(phdr, offsetNumber);
				if (!ItemIdIsUsed(itemId) && !ItemIdHasStorage(itemId))
					break;
			}
			if (offsetNumber >= limit)
			{
				/* the hint is wrong, so reset it */
				PageClearHasFreeLinePointers(phdr);
			}
		}
		else
		{
			/* don't bother searching if hint says there's no free slot */
			offsetNumber = limit;
		}
	}

	if (offsetNumber > limit)
	{
		elog(WARNING, "specified item offset is too large");
		return InvalidOffsetNumber;
	}

	if (is_heap && offsetNumber > MaxHeapTuplesPerPage)
	{
		elog(WARNING, "can't put more than MaxHeapTuplesPerPage items in a heap page");
		return InvalidOffsetNumber;
	}

	/*
	 * Compute new lower and upper pointers for page, see if it'll fit.
	 *
	 * Note: do arithmetic as signed ints, to avoid mistakes if, say,
	 * alignedSize > pd_upper.
	 */
	if (offsetNumber == limit || needshuffle)
		lower = phdr->pd_lower + sizeof(ItemIdData);
	else
		lower = phdr->pd_lower;

	alignedSize = MAXALIGN(size);

	upper = (int) phdr->pd_upper - (int) alignedSize;

	if (lower > upper)
		return InvalidOffsetNumber;

	/*
	 * OK to insert the item.  First, shuffle the existing pointers if needed.
	 */
	itemId = PageGetItemId(phdr, offsetNumber);

	if (needshuffle)
		memmove(itemId + 1, itemId,
				(limit - offsetNumber) * sizeof(ItemIdData));

	/* set the item pointer */
	ItemIdSetNormal(itemId, upper, size);

	/*
	 * Items normally contain no uninitialized bytes.  Core bufpage consumers
	 * conform, but this is not a necessary coding rule; a new index AM could
	 * opt to depart from it.  However, data type input functions and other
	 * C-language functions that synthesize datums should initialize all
	 * bytes; datumIsEqual() relies on this.  Testing here, along with the
	 * similar check in printtup(), helps to catch such mistakes.
	 *
	 * Values of the "name" type retrieved via index-only scans may contain
	 * uninitialized bytes; see comment in btrescan().  Valgrind will report
	 * this as an error, but it is safe to ignore.
	 */
	VALGRIND_CHECK_MEM_IS_DEFINED(item, size);

	/* copy the item's data onto the page */
	memcpy((char *) page + upper, item, size);

	/* adjust page header */
	phdr->pd_lower = (LocationIndex) lower;
	phdr->pd_upper = (LocationIndex) upper;

	return offsetNumber;
}

/*
 * PageGetTempPage
 *		Get a temporary page in local memory for special processing.
 *		The returned page is not initialized at all; caller must do that.
 */
Page
PageGetTempPage(Page page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	return temp;
}

/*
 * PageGetTempPageCopy
 *		Get a temporary page in local memory for special processing.
 *		The page is initialized by copying the contents of the given page.
 */
Page
PageGetTempPageCopy(Page page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	memcpy(temp, page, pageSize);

	return temp;
}

/*
 * PageGetTempPageCopySpecial
 *		Get a temporary page in local memory for special processing.
 *		The page is PageInit'd with the same special-space size as the
 *		given page, and the special space is copied from the given page.
 */
Page
PageGetTempPageCopySpecial(Page page)
{
	Size		pageSize;
	Page		temp;

	pageSize = PageGetPageSize(page);
	temp = (Page) palloc(pageSize);

	PageInit(temp, pageSize, PageGetSpecialSize(page));
	memcpy(PageGetSpecialPointer(temp),
		   PageGetSpecialPointer(page),
		   PageGetSpecialSize(page));

	return temp;
}

/*
 * PageRestoreTempPage
 *		Copy temporary page back to permanent page after special processing
 *		and release the temporary page.
 */
void
PageRestoreTempPage(Page tempPage, Page oldPage)
{
	Size		pageSize;

	pageSize = PageGetPageSize(tempPage);
	memcpy((char *) oldPage, (char *) tempPage, pageSize);

	pfree(tempPage);
}

/*
 * sorting support for PageRepairFragmentation, PageIndexMultiDelete,
 * PageIndexDeleteNoCompact
 */
typedef struct itemIdSortData
{
	uint16		offsetindex;	/* linp array index */
	int16		itemoff;		/* page offset of item data */
	uint16		alignedlen;		/* MAXALIGN(item data len) */
} itemIdSortData;
typedef itemIdSortData *itemIdSort;

static int
itemoffcompare(const void *itemidp1, const void *itemidp2)
{
	/* Sort in decreasing itemoff order */
	return ((itemIdSort) itemidp2)->itemoff -
		((itemIdSort) itemidp1)->itemoff;
}

/*
 * After removing or marking some line pointers unused, move the tuples to
 * remove the gaps caused by the removed items.
 */
static void
compactify_tuples(itemIdSort itemidbase, int nitems, Page page)
{
	PageHeader	phdr = (PageHeader) page;
	Offset		upper;
	int			i;

	/* sort itemIdSortData array into decreasing itemoff order */
	qsort((char *) itemidbase, nitems, sizeof(itemIdSortData),
		  itemoffcompare);

	upper = phdr->pd_special;
	for (i = 0; i < nitems; i++)
	{
		itemIdSort	itemidptr = &itemidbase[i];
		ItemId		lp;

		lp = PageGetItemId(page, itemidptr->offsetindex + 1);
		upper -= itemidptr->alignedlen;
		memmove((char *) page + upper,
				(char *) page + itemidptr->itemoff,
				itemidptr->alignedlen);
		lp->lp_off = upper;
	}

	phdr->pd_upper = upper;
}

/*
 * PageRepairFragmentation
 *
 * Frees fragmented space on a page.
 * It doesn't remove unused line pointers! Please don't change this.
 *
 * This routine is usable for heap pages only, but see PageIndexMultiDelete.
 *
 * As a side effect, the page's PD_HAS_FREE_LINES hint bit is updated.
 */
void
PageRepairFragmentation(Page page)
{
	Offset		pd_lower = ((PageHeader) page)->pd_lower;
	Offset		pd_upper = ((PageHeader) page)->pd_upper;
	Offset		pd_special = ((PageHeader) page)->pd_special;
	ItemId		lp;
	int			nline,
				nstorage,
				nunused;
	int			i;
	Size		totallen;

	/*
	 * It's worth the trouble to be more paranoid here than in most places,
	 * because we are about to reshuffle data in (what is usually) a shared
	 * disk buffer.  If we aren't careful then corrupted pointers, lengths,
	 * etc could cause us to clobber adjacent disk buffers, spreading the data
	 * loss further.  So, check everything.
	 */
	if (pd_lower < SizeOfPageHeaderData ||
		pd_lower > pd_upper ||
		pd_upper > pd_special ||
		pd_special > BLCKSZ ||
		pd_special != MAXALIGN(pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						pd_lower, pd_upper, pd_special)));

	nline = PageGetMaxOffsetNumber(page);
	nunused = nstorage = 0;
	for (i = FirstOffsetNumber; i <= nline; i++)
	{
		lp = PageGetItemId(page, i);
		if (ItemIdIsUsed(lp))
		{
			if (ItemIdHasStorage(lp))
				nstorage++;
		}
		else
		{
			/* Unused entries should have lp_len = 0, but make sure */
			ItemIdSetUnused(lp);
			nunused++;
		}
	}

	if (nstorage == 0)
	{
		/* Page is completely empty, so just reset it quickly */
		((PageHeader) page)->pd_upper = pd_special;
	}
	else
	{
		/* Need to compact the page the hard way */
		itemIdSortData itemidbase[MaxHeapTuplesPerPage];
		itemIdSort	itemidptr = itemidbase;

		totallen = 0;
		for (i = 0; i < nline; i++)
		{
			lp = PageGetItemId(page, i + 1);
			if (ItemIdHasStorage(lp))
			{
				itemidptr->offsetindex = i;
				itemidptr->itemoff = ItemIdGetOffset(lp);
				if (itemidptr->itemoff < (int) pd_upper ||
					itemidptr->itemoff >= (int) pd_special)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted item pointer: %u",
									itemidptr->itemoff)));
				itemidptr->alignedlen = MAXALIGN(ItemIdGetLength(lp));
				totallen += itemidptr->alignedlen;
				itemidptr++;
			}
		}

		if (totallen > (Size) (pd_special - pd_lower))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
			   errmsg("corrupted item lengths: total %u, available space %u",
					  (unsigned int) totallen, pd_special - pd_lower)));

		compactify_tuples(itemidbase, nstorage, page);
	}

	/* Set hint bit for PageAddItem */
	if (nunused > 0)
		PageSetHasFreeLinePointers(page);
	else
		PageClearHasFreeLinePointers(page);
}

/*
 * PageGetFreeSpace
 *		Returns the size of the free (allocatable) space on a page,
 *		reduced by the space needed for a new line pointer.
 *
 * Note: this should usually only be used on index pages.  Use
 * PageGetHeapFreeSpace on heap pages.
 */
Size
PageGetFreeSpace(Page page)
{
	int			space;

	/*
	 * Use signed arithmetic here so that we behave sensibly if pd_lower >
	 * pd_upper.
	 */
	space = (int) ((PageHeader) page)->pd_upper -
		(int) ((PageHeader) page)->pd_lower;

	if (space < (int) sizeof(ItemIdData))
		return 0;
	space -= sizeof(ItemIdData);

	return (Size) space;
}

/*
 * PageGetExactFreeSpace
 *		Returns the size of the free (allocatable) space on a page,
 *		without any consideration for adding/removing line pointers.
 */
Size
PageGetExactFreeSpace(Page page)
{
	int			space;

	/*
	 * Use signed arithmetic here so that we behave sensibly if pd_lower >
	 * pd_upper.
	 */
	space = (int) ((PageHeader) page)->pd_upper -
		(int) ((PageHeader) page)->pd_lower;

	if (space < 0)
		return 0;

	return (Size) space;
}


/*
 * PageGetHeapFreeSpace
 *		Returns the size of the free (allocatable) space on a page,
 *		reduced by the space needed for a new line pointer.
 *
 * The difference between this and PageGetFreeSpace is that this will return
 * zero if there are already MaxHeapTuplesPerPage line pointers in the page
 * and none are free.  We use this to enforce that no more than
 * MaxHeapTuplesPerPage line pointers are created on a heap page.  (Although
 * no more tuples than that could fit anyway, in the presence of redirected
 * or dead line pointers it'd be possible to have too many line pointers.
 * To avoid breaking code that assumes MaxHeapTuplesPerPage is a hard limit
 * on the number of line pointers, we make this extra check.)
 */
Size
PageGetHeapFreeSpace(Page page)
{
	Size		space;

	space = PageGetFreeSpace(page);
	if (space > 0)
	{
		OffsetNumber offnum,
					nline;

		/*
		 * Are there already MaxHeapTuplesPerPage line pointers in the page?
		 */
		nline = PageGetMaxOffsetNumber(page);
		if (nline >= MaxHeapTuplesPerPage)
		{
			if (PageHasFreeLinePointers((PageHeader) page))
			{
				/*
				 * Since this is just a hint, we must confirm that there is
				 * indeed a free line pointer
				 */
				for (offnum = FirstOffsetNumber; offnum <= nline; offnum = OffsetNumberNext(offnum))
				{
					ItemId		lp = PageGetItemId(page, offnum);

					if (!ItemIdIsUsed(lp))
						break;
				}

				if (offnum > nline)
				{
					/*
					 * The hint is wrong, but we can't clear it here since we
					 * don't have the ability to mark the page dirty.
					 */
					space = 0;
				}
			}
			else
			{
				/*
				 * Although the hint might be wrong, PageAddItem will believe
				 * it anyway, so we must believe it too.
				 */
				space = 0;
			}
		}
	}
	return space;
}


/*
 * PageIndexTupleDelete
 *
 * This routine does the work of removing a tuple from an index page.
 *
 * Unlike heap pages, we compact out the line pointer for the removed tuple.
 */
void
PageIndexTupleDelete(Page page, OffsetNumber offnum)
{
	PageHeader	phdr = (PageHeader) page;
	char	   *addr;
	ItemId		tup;
	Size		size;
	unsigned	offset;
	int			nbytes;
	int			offidx;
	int			nline;

	/*
	 * As with PageRepairFragmentation, paranoia seems justified.
	 */
	if (phdr->pd_lower < SizeOfPageHeaderData ||
		phdr->pd_lower > phdr->pd_upper ||
		phdr->pd_upper > phdr->pd_special ||
		phdr->pd_special > BLCKSZ)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						phdr->pd_lower, phdr->pd_upper, phdr->pd_special)));

	nline = PageGetMaxOffsetNumber(page);
	if ((int) offnum <= 0 || (int) offnum > nline)
		elog(ERROR, "invalid index offnum: %u", offnum);

	/* change offset number to offset index */
	offidx = offnum - 1;

	tup = PageGetItemId(page, offnum);
	Assert(ItemIdHasStorage(tup));
	size = ItemIdGetLength(tup);
	offset = ItemIdGetOffset(tup);

	if (offset < phdr->pd_upper || (offset + size) > phdr->pd_special ||
		offset != MAXALIGN(offset) || size != MAXALIGN(size))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted item pointer: offset = %u, size = %u",
						offset, (unsigned int) size)));

	/*
	 * First, we want to get rid of the pd_linp entry for the index tuple. We
	 * copy all subsequent linp's back one slot in the array. We don't use
	 * PageGetItemId, because we are manipulating the _array_, not individual
	 * linp's.
	 */
	nbytes = phdr->pd_lower -
		((char *) &phdr->pd_linp[offidx + 1] - (char *) phdr);

	if (nbytes > 0)
		memmove((char *) &(phdr->pd_linp[offidx]),
				(char *) &(phdr->pd_linp[offidx + 1]),
				nbytes);

	/*
	 * Now move everything between the old upper bound (beginning of tuple
	 * space) and the beginning of the deleted tuple forward, so that space in
	 * the middle of the page is left free.  If we've just deleted the tuple
	 * at the beginning of tuple space, then there's no need to do the copy
	 * (and bcopy on some architectures SEGV's if asked to move zero bytes).
	 */

	/* beginning of tuple space */
	addr = (char *) page + phdr->pd_upper;

	if (offset > phdr->pd_upper)
		memmove(addr + size, addr, (int) (offset - phdr->pd_upper));

	/* adjust free space boundary pointers */
	phdr->pd_upper += size;
	phdr->pd_lower -= sizeof(ItemIdData);

	/*
	 * Finally, we need to adjust the linp entries that remain.
	 *
	 * Anything that used to be before the deleted tuple's data was moved
	 * forward by the size of the deleted tuple.
	 */
	if (!PageIsEmpty(page))
	{
		int			i;

		nline--;				/* there's one less than when we started */
		for (i = 1; i <= nline; i++)
		{
			ItemId		ii = PageGetItemId(phdr, i);

			Assert(ItemIdHasStorage(ii));
			if (ItemIdGetOffset(ii) <= offset)
				ii->lp_off += size;
		}
	}
}


/*
 * PageIndexMultiDelete
 *
 * This routine handles the case of deleting multiple tuples from an
 * index page at once.  It is considerably faster than a loop around
 * PageIndexTupleDelete ... however, the caller *must* supply the array
 * of item numbers to be deleted in item number order!
 */
void
PageIndexMultiDelete(Page page, OffsetNumber *itemnos, int nitems)
{
	PageHeader	phdr = (PageHeader) page;
	Offset		pd_lower = phdr->pd_lower;
	Offset		pd_upper = phdr->pd_upper;
	Offset		pd_special = phdr->pd_special;
	itemIdSortData itemidbase[MaxIndexTuplesPerPage];
	ItemIdData	newitemids[MaxIndexTuplesPerPage];
	itemIdSort	itemidptr;
	ItemId		lp;
	int			nline,
				nused;
	Size		totallen;
	Size		size;
	unsigned	offset;
	int			nextitm;
	OffsetNumber offnum;

	Assert(nitems <= MaxIndexTuplesPerPage);

	/*
	 * If there aren't very many items to delete, then retail
	 * PageIndexTupleDelete is the best way.  Delete the items in reverse
	 * order so we don't have to think about adjusting item numbers for
	 * previous deletions.
	 *
	 * TODO: tune the magic number here
	 */
	if (nitems <= 2)
	{
		while (--nitems >= 0)
			PageIndexTupleDelete(page, itemnos[nitems]);
		return;
	}

	/*
	 * As with PageRepairFragmentation, paranoia seems justified.
	 */
	if (pd_lower < SizeOfPageHeaderData ||
		pd_lower > pd_upper ||
		pd_upper > pd_special ||
		pd_special > BLCKSZ ||
		pd_special != MAXALIGN(pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						pd_lower, pd_upper, pd_special)));

	/*
	 * Scan the item pointer array and build a list of just the ones we are
	 * going to keep.  Notice we do not modify the page yet, since we are
	 * still validity-checking.
	 */
	nline = PageGetMaxOffsetNumber(page);
	itemidptr = itemidbase;
	totallen = 0;
	nused = 0;
	nextitm = 0;
	for (offnum = FirstOffsetNumber; offnum <= nline; offnum = OffsetNumberNext(offnum))
	{
		lp = PageGetItemId(page, offnum);
		Assert(ItemIdHasStorage(lp));
		size = ItemIdGetLength(lp);
		offset = ItemIdGetOffset(lp);
		if (offset < pd_upper ||
			(offset + size) > pd_special ||
			offset != MAXALIGN(offset))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted item pointer: offset = %u, size = %u",
							offset, (unsigned int) size)));

		if (nextitm < nitems && offnum == itemnos[nextitm])
		{
			/* skip item to be deleted */
			nextitm++;
		}
		else
		{
			itemidptr->offsetindex = nused;		/* where it will go */
			itemidptr->itemoff = offset;
			itemidptr->alignedlen = MAXALIGN(size);
			totallen += itemidptr->alignedlen;
			newitemids[nused] = *lp;
			itemidptr++;
			nused++;
		}
	}

	/* this will catch invalid or out-of-order itemnos[] */
	if (nextitm != nitems)
		elog(ERROR, "incorrect index offsets supplied");

	if (totallen > (Size) (pd_special - pd_lower))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
			   errmsg("corrupted item lengths: total %u, available space %u",
					  (unsigned int) totallen, pd_special - pd_lower)));

	/*
	 * Looks good. Overwrite the line pointers with the copy, from which we've
	 * removed all the unused items.
	 */
	memcpy(phdr->pd_linp, newitemids, nused * sizeof(ItemIdData));
	phdr->pd_lower = SizeOfPageHeaderData + nused * sizeof(ItemIdData);

	/* and compactify the tuple data */
	compactify_tuples(itemidbase, nused, page);
}

/*
 * PageIndexDeleteNoCompact
 *		Delete the given items for an index page, and defragment the resulting
 *		free space, but do not compact the item pointers array.
 *
 * itemnos is the array of tuples to delete; nitems is its size.  maxIdxTuples
 * is the maximum number of tuples that can exist in a page.
 *
 * Unused items at the end of the array are removed.
 *
 * This is used for index AMs that require that existing TIDs of live tuples
 * remain unchanged.
 */
void
PageIndexDeleteNoCompact(Page page, OffsetNumber *itemnos, int nitems)
{
	PageHeader	phdr = (PageHeader) page;
	LocationIndex pd_lower = phdr->pd_lower;
	LocationIndex pd_upper = phdr->pd_upper;
	LocationIndex pd_special = phdr->pd_special;
	int			nline;
	bool		empty;
	OffsetNumber offnum;
	int			nextitm;

	/*
	 * As with PageRepairFragmentation, paranoia seems justified.
	 */
	if (pd_lower < SizeOfPageHeaderData ||
		pd_lower > pd_upper ||
		pd_upper > pd_special ||
		pd_special > BLCKSZ ||
		pd_special != MAXALIGN(pd_special))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted page pointers: lower = %u, upper = %u, special = %u",
						pd_lower, pd_upper, pd_special)));

	/*
	 * Scan the existing item pointer array and mark as unused those that are
	 * in our kill-list; make sure any non-interesting ones are marked unused
	 * as well.
	 */
	nline = PageGetMaxOffsetNumber(page);
	empty = true;
	nextitm = 0;
	for (offnum = FirstOffsetNumber; offnum <= nline; offnum = OffsetNumberNext(offnum))
	{
		ItemId		lp;
		ItemLength	itemlen;
		ItemOffset	offset;

		lp = PageGetItemId(page, offnum);

		itemlen = ItemIdGetLength(lp);
		offset = ItemIdGetOffset(lp);

		if (ItemIdIsUsed(lp))
		{
			if (offset < pd_upper ||
				(offset + itemlen) > pd_special ||
				offset != MAXALIGN(offset))
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
				   errmsg("corrupted item pointer: offset = %u, length = %u",
						  offset, (unsigned int) itemlen)));

			if (nextitm < nitems && offnum == itemnos[nextitm])
			{
				/* this one is on our list to delete, so mark it unused */
				ItemIdSetUnused(lp);
				nextitm++;
			}
			else if (ItemIdHasStorage(lp))
			{
				/* This one's live -- must do the compaction dance */
				empty = false;
			}
			else
			{
				/* get rid of this one too */
				ItemIdSetUnused(lp);
			}
		}
	}

	/* this will catch invalid or out-of-order itemnos[] */
	if (nextitm != nitems)
		elog(ERROR, "incorrect index offsets supplied");

	if (empty)
	{
		/* Page is completely empty, so just reset it quickly */
		phdr->pd_lower = SizeOfPageHeaderData;
		phdr->pd_upper = pd_special;
	}
	else
	{
		/* There are live items: need to compact the page the hard way */
		itemIdSortData itemidbase[MaxOffsetNumber];
		itemIdSort	itemidptr;
		int			i;
		Size		totallen;

		/*
		 * Scan the page taking note of each item that we need to preserve.
		 * This includes both live items (those that contain data) and
		 * interspersed unused ones.  It's critical to preserve these unused
		 * items, because otherwise the offset numbers for later live items
		 * would change, which is not acceptable.  Unused items might get used
		 * again later; that is fine.
		 */
		itemidptr = itemidbase;
		totallen = 0;
		PageClearHasFreeLinePointers(page);
		for (i = 0; i < nline; i++)
		{
			ItemId		lp;

			itemidptr->offsetindex = i;

			lp = PageGetItemId(page, i + 1);
			if (ItemIdHasStorage(lp))
			{
				itemidptr->itemoff = ItemIdGetOffset(lp);
				itemidptr->alignedlen = MAXALIGN(ItemIdGetLength(lp));
				totallen += itemidptr->alignedlen;
				itemidptr++;
			}
			else
			{
				PageSetHasFreeLinePointers(page);
				ItemIdSetUnused(lp);
			}
		}
		nline = itemidptr - itemidbase;
		/* By here, there are exactly nline elements in itemidbase array */

		if (totallen > (Size) (pd_special - pd_lower))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
			   errmsg("corrupted item lengths: total %u, available space %u",
					  (unsigned int) totallen, pd_special - pd_lower)));

		/*
		 * Defragment the data areas of each tuple, being careful to preserve
		 * each item's position in the linp array.
		 */
		compactify_tuples(itemidbase, nline, page);
	}
}

/*
 * Set checksum for a page in shared buffers.
 *
 * If checksums are disabled, or if the page is not initialized, just return
 * the input.  Otherwise, we must make a copy of the page before calculating
 * the checksum, to prevent concurrent modifications (e.g. setting hint bits)
 * from making the final checksum invalid.  It doesn't matter if we include or
 * exclude hints during the copy, as long as we write a valid page and
 * associated checksum.
 *
 * Returns a pointer to the block-sized data that needs to be written. Uses
 * statically-allocated memory, so the caller must immediately write the
 * returned page and not refer to it again.
 */
char *
PageSetChecksumCopy(Page page, BlockNumber blkno)
{
	static char *pageCopy = NULL;

	/* If we don't need a checksum, just return the passed-in data */
	if (PageIsNew(page) || !DataChecksumsEnabled())
		return (char *) page;

	/*
	 * We allocate the copy space once and use it over on each subsequent
	 * call.  The point of palloc'ing here, rather than having a static char
	 * array, is first to ensure adequate alignment for the checksumming code
	 * and second to avoid wasting space in processes that never call this.
	 */
	if (pageCopy == NULL)
		pageCopy = MemoryContextAlloc(TopMemoryContext, BLCKSZ);

	memcpy(pageCopy, (char *) page, BLCKSZ);
	((PageHeader) pageCopy)->pd_checksum = pg_checksum_page(pageCopy, blkno);
	return pageCopy;
}

/*
 * Set checksum for a page in private memory.
 *
 * This must only be used when we know that no other process can be modifying
 * the page buffer.
 */
void
PageSetChecksumInplace(Page page, BlockNumber blkno)
{
	/* If we don't need a checksum, just return */
	if (PageIsNew(page) || !DataChecksumsEnabled())
		return;

	((PageHeader) page)->pd_checksum = pg_checksum_page((char *) page, blkno);
}
