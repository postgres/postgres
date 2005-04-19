/*-------------------------------------------------------------------------
 *
 * tidbitmap.c
 *	  PostgreSQL tuple-id (TID) bitmap package
 *
 * This module provides bitmap data structures that are spiritually
 * similar to Bitmapsets, but are specially adapted to store sets of
 * tuple identifiers (TIDs), or ItemPointers.  In particular, the division
 * of an ItemPointer into BlockNumber and OffsetNumber is catered for.
 * Also, since we wish to be able to store very large tuple sets in
 * memory with this data structure, we support "lossy" storage, in which
 * we no longer remember individual tuple offsets on a page but only the
 * fact that a particular page needs to be visited.
 *
 * The "lossy" storage uses one bit per disk page, so at the standard 8K
 * BLCKSZ, we can represent all pages in 64Gb of disk space in about 1Mb
 * of memory.  People pushing around tables of that size should have a
 * couple of Mb to spare, so we don't worry about providing a second level
 * of lossiness.  In theory we could fall back to page ranges at some
 * point, but for now that seems useless complexity.
 *
 *
 * Copyright (c) 2003-2005, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/nodes/tidbitmap.c,v 1.2 2005/04/19 22:35:15 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/htup.h"
#include "nodes/tidbitmap.h"
#include "utils/hsearch.h"


/*
 * The maximum number of tuples per page is not large (typically 256 with
 * 8K pages, or 1024 with 32K pages).  So there's not much point in making
 * the per-page bitmaps variable size.  We just legislate that the size
 * is this:
 */
#define MAX_TUPLES_PER_PAGE  ((BLCKSZ - 1) / MAXALIGN(offsetof(HeapTupleHeaderData, t_bits) + sizeof(ItemIdData)) + 1)

/*
 * When we have to switch over to lossy storage, we use a data structure
 * with one bit per page, where all pages having the same number DIV
 * PAGES_PER_CHUNK are aggregated into one chunk.  When a chunk is present
 * and has the bit set for a given page, there must not be a per-page entry
 * for that page in the page table.
 *
 * We actually store both exact pages and lossy chunks in the same hash
 * table, using identical data structures.  (This is because dynahash.c's
 * memory management doesn't allow space to be transferred easily from one
 * hashtable to another.)  Therefore it's best if PAGES_PER_CHUNK is the
 * same as MAX_TUPLES_PER_PAGE, or at least not too different.  But we
 * also want PAGES_PER_CHUNK to be a power of 2 to avoid expensive integer
 * remainder operations.  So, define it like this:
 */
#define PAGES_PER_CHUNK  (BLCKSZ / 32)

/* The bitmap unit size can be adjusted by changing these declarations: */
#define BITS_PER_BITMAPWORD 32
typedef uint32 bitmapword;		/* must be an unsigned type */

#define WORDNUM(x)	((x) / BITS_PER_BITMAPWORD)
#define BITNUM(x)	((x) % BITS_PER_BITMAPWORD)

/* number of active words for an exact page: */
#define WORDS_PER_PAGE  ((MAX_TUPLES_PER_PAGE - 1) / BITS_PER_BITMAPWORD + 1)
/* number of active words for a lossy chunk: */
#define WORDS_PER_CHUNK  ((PAGES_PER_CHUNK - 1) / BITS_PER_BITMAPWORD + 1)

/*
 * The hashtable entries are represented by this data structure.  For
 * an exact page, blockno is the page number and bit k of the bitmap
 * represents tuple offset k+1.  For a lossy chunk, blockno is the first
 * page in the chunk (this must be a multiple of PAGES_PER_CHUNK) and
 * bit k represents page blockno+k.  Note that it is not possible to
 * have exact storage for the first page of a chunk if we are using
 * lossy storage for any page in the chunk's range, since the same
 * hashtable entry has to serve both purposes.
 */
typedef struct PagetableEntry
{
	BlockNumber	blockno;		/* page number (hashtable key) */
	bool		ischunk;		/* T = lossy storage, F = exact */
	bitmapword	words[Max(WORDS_PER_PAGE, WORDS_PER_CHUNK)];
} PagetableEntry;

/*
 * Here is the representation for a whole TIDBitMap:
 */
struct TIDBitmap
{
	NodeTag		type;			/* to make it a valid Node */
	MemoryContext mcxt;			/* memory context containing me */
	HTAB	   *pagetable;		/* hash table of PagetableEntry's */
	int			nentries;		/* number of entries in pagetable */
	int			maxentries;		/* limit on same to meet maxbytes */
	int			npages;			/* number of exact entries in pagetable */
	int			nchunks;		/* number of lossy entries in pagetable */
	bool		iterating;		/* tbm_begin_iterate called? */
	/* the remaining fields are used while producing sorted output: */
	TBMIterateResult *output;	/* NULL if not yet created */
	PagetableEntry **spages;	/* sorted exact-page list, or NULL */
	PagetableEntry **schunks;	/* sorted lossy-chunk list, or NULL */
	int			spageptr;		/* next spages index */
	int			schunkptr;		/* next schunks index */
	int			schunkbit;		/* next bit to check in current schunk */
};


/* Local function prototypes */
static PagetableEntry *tbm_find_pageentry(const TIDBitmap *tbm,
										  BlockNumber pageno);
static PagetableEntry *tbm_get_pageentry(TIDBitmap *tbm, BlockNumber pageno);
static bool tbm_page_is_lossy(const TIDBitmap *tbm, BlockNumber pageno);
static void tbm_mark_page_lossy(TIDBitmap *tbm, BlockNumber pageno);
static void tbm_lossify(TIDBitmap *tbm);
static int	tbm_comparator(const void *left, const void *right);


/*
 * tbm_create - create an initially-empty bitmap
 *
 * The bitmap will live in the memory context that is CurrentMemoryContext
 * at the time of this call.  It will be limited to (approximately) maxbytes
 * total memory consumption.
 */
TIDBitmap *
tbm_create(long maxbytes)
{
	TIDBitmap  *tbm;
	HASHCTL		hash_ctl;
	long		nbuckets;

	tbm = makeNode(TIDBitmap);
	/* we rely on makeNode to have zeroed all the fields */
	tbm->mcxt = CurrentMemoryContext;

	/*
	 * Estimate number of hashtable entries we can have within maxbytes.
	 * This estimates the hash overhead at MAXALIGN(sizeof(HASHELEMENT))
	 * plus a pointer per hash entry, which is crude but good enough for
	 * our purpose.  (NOTE: this does not count the space for data
	 * structures created during iteration readout.)
	 */
	nbuckets = maxbytes /
		(MAXALIGN(sizeof(HASHELEMENT)) + MAXALIGN(sizeof(PagetableEntry))
		 + sizeof(Pointer));
	nbuckets = Min(nbuckets, INT_MAX-1);	/* safety limit */
	tbm->maxentries = (int) nbuckets;

	MemSet(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(BlockNumber);
	hash_ctl.entrysize = sizeof(PagetableEntry);
	hash_ctl.hash = tag_hash;
	hash_ctl.hcxt = CurrentMemoryContext;
	tbm->pagetable = hash_create("TIDBitmap",
								 128,	/* start small and extend */
								 &hash_ctl,
								 HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

	return tbm;
}

/*
 * tbm_free - free a TIDBitmap
 */
void
tbm_free(TIDBitmap *tbm)
{
	hash_destroy(tbm->pagetable);
	if (tbm->output)
		pfree(tbm->output);
	if (tbm->spages)
		pfree(tbm->spages);
	if (tbm->schunks)
		pfree(tbm->schunks);
	pfree(tbm);
}

/*
 * tbm_add_tuples - add some tuple IDs to a TIDBitmap
 */
void
tbm_add_tuples(TIDBitmap *tbm, const ItemPointer tids, int ntids)
{
	int			i;

	Assert(!tbm->iterating);
	for (i = 0; i < ntids; i++)
	{
		BlockNumber blk = ItemPointerGetBlockNumber(tids + i);
		OffsetNumber off = ItemPointerGetOffsetNumber(tids + i);
		PagetableEntry *page;
		int			wordnum,
					bitnum;

		/* safety check to ensure we don't overrun bit array bounds */
		if (off < 1 || off > MAX_TUPLES_PER_PAGE)
			elog(ERROR, "tuple offset out of range: %u", off);

		if (tbm_page_is_lossy(tbm, blk))
			continue;			/* whole page is already marked */

		page = tbm_get_pageentry(tbm, blk);

		if (page->ischunk)
		{
			/* The page is a lossy chunk header, set bit for itself */
			wordnum = bitnum = 0;
		}
		else
		{
			/* Page is exact, so set bit for individual tuple */
			wordnum = WORDNUM(off - 1);
			bitnum = BITNUM(off - 1);
		}
		page->words[wordnum] |= ((bitmapword) 1 << bitnum);

		if (tbm->nentries > tbm->maxentries)
			tbm_lossify(tbm);
	}
}

/*
 * tbm_union - set union
 *
 * a is modified in-place, b is not changed
 */
void
tbm_union(TIDBitmap *a, const TIDBitmap *b)
{
	HASH_SEQ_STATUS status;
	PagetableEntry *apage;
	PagetableEntry *bpage;
	int		wordnum;

	Assert(!a->iterating);
	/* Scan through chunks and pages in b, merge into a */
	hash_seq_init(&status, b->pagetable);
	while ((bpage = (PagetableEntry *) hash_seq_search(&status)) != NULL)
	{
		if (bpage->ischunk)
		{
			/* Scan b's chunk, mark each indicated page lossy in a */
			for (wordnum = 0; wordnum < WORDS_PER_PAGE; wordnum++)
			{
				bitmapword	w = bpage->words[wordnum];

				if (w != 0)
				{
					BlockNumber	pg;

					pg = bpage->blockno + (wordnum * BITS_PER_BITMAPWORD);
					while (w != 0)
					{
						if (w & 1)
							tbm_mark_page_lossy(a, pg);
						pg++;
						w >>= 1;
					}
				}
			}
		}
		else if (tbm_page_is_lossy(a, bpage->blockno))
		{
			/* page is already lossy in a, nothing to do */
			continue;
		}
		else
		{
			apage = tbm_get_pageentry(a, bpage->blockno);
			if (apage->ischunk)
			{
				/* The page is a lossy chunk header, set bit for itself */
				apage->words[0] |= ((bitmapword) 1 << 0);
			}
			else
			{
				/* Both pages are exact, merge at the bit level */
				for (wordnum = 0; wordnum < WORDS_PER_PAGE; wordnum++)
					apage->words[wordnum] |= bpage->words[wordnum];
			}
		}

		if (a->nentries > a->maxentries)
			tbm_lossify(a);
	}
}

/*
 * tbm_intersect - set intersection
 *
 * a is modified in-place, b is not changed
 */
void
tbm_intersect(TIDBitmap *a, const TIDBitmap *b)
{
	HASH_SEQ_STATUS status;
	PagetableEntry *apage;
	PagetableEntry *bpage;
	int		wordnum;

	Assert(!a->iterating);
	/* Scan through chunks and pages in a, try to match to b */
	hash_seq_init(&status, a->pagetable);
	while ((apage = (PagetableEntry *) hash_seq_search(&status)) != NULL)
	{
		if (apage->ischunk)
		{
			/* Scan each bit in chunk, try to clear */
			bool	candelete = true;

			for (wordnum = 0; wordnum < WORDS_PER_PAGE; wordnum++)
			{
				bitmapword	w = apage->words[wordnum];

				if (w != 0)
				{
					bitmapword	neww = w;
					BlockNumber	pg;
					int		bitnum;

					pg = apage->blockno + (wordnum * BITS_PER_BITMAPWORD);
					bitnum = 0;
					while (w != 0)
					{
						if (w & 1)
						{
							if (!tbm_page_is_lossy(b, pg) &&
								tbm_find_pageentry(b, pg) == NULL)
							{
								/* Page is not in b at all, lose lossy bit */
								neww &= ~((bitmapword) 1 << bitnum);
							}
						}
						pg++;
						bitnum++;
						w >>= 1;
					}
					apage->words[wordnum] = neww;
					if (neww != 0)
						candelete = false;
				}
			}
			if (candelete)
			{
				/* Chunk is now empty, remove it from a */
				if (hash_search(a->pagetable,
								(void *) &apage->blockno,
								HASH_REMOVE, NULL) == NULL)
					elog(ERROR, "hash table corrupted");
				a->nentries--;
				a->nchunks--;
			}
		}
		else if (tbm_page_is_lossy(b, apage->blockno))
		{
			/* page is lossy in b, cannot clear any bits */
			continue;
		}
		else
		{
			bool	candelete = true;

			bpage = tbm_find_pageentry(b, apage->blockno);
			if (bpage != NULL)
			{
				/* Both pages are exact, merge at the bit level */
				Assert(!bpage->ischunk);
				for (wordnum = 0; wordnum < WORDS_PER_PAGE; wordnum++)
				{
					apage->words[wordnum] &= bpage->words[wordnum];
					if (apage->words[wordnum] != 0)
						candelete = false;
				}
			}
			if (candelete)
			{
				/* Page is now empty, remove it from a */
				if (hash_search(a->pagetable,
								(void *) &apage->blockno,
								HASH_REMOVE, NULL) == NULL)
					elog(ERROR, "hash table corrupted");
				a->nentries--;
				a->npages--;
			}
		}
	}
}

/*
 * tbm_begin_iterate - prepare to iterate through a TIDBitmap
 *
 * NB: after this is called, it is no longer allowed to modify the contents
 * of the bitmap.  However, you can call this multiple times to scan the
 * contents repeatedly.
 */
void
tbm_begin_iterate(TIDBitmap *tbm)
{
	HASH_SEQ_STATUS status;
	PagetableEntry *page;
	int			npages;
	int			nchunks;

	tbm->iterating = true;
	/*
	 * Allocate the output data structure if we didn't already.
	 * (We don't do this during tbm_create since it's entirely possible
	 * that a TIDBitmap will live and die without ever being iterated.)
	 */
	if (!tbm->output)
		tbm->output = (TBMIterateResult *)
			MemoryContextAllocZero(tbm->mcxt,
								   sizeof(TBMIterateResult) +
								   MAX_TUPLES_PER_PAGE * sizeof(OffsetNumber));
	/*
	 * Create and fill the sorted page lists if we didn't already.
	 */
	if (!tbm->spages && tbm->npages > 0)
		tbm->spages = (PagetableEntry **)
			MemoryContextAlloc(tbm->mcxt,
							   tbm->npages * sizeof(PagetableEntry *));
	if (!tbm->schunks && tbm->nchunks > 0)
		tbm->schunks = (PagetableEntry **)
			MemoryContextAlloc(tbm->mcxt,
							   tbm->nchunks * sizeof(PagetableEntry *));

	hash_seq_init(&status, tbm->pagetable);
	npages = nchunks = 0;
	while ((page = (PagetableEntry *) hash_seq_search(&status)) != NULL)
	{
		if (page->ischunk)
			tbm->schunks[nchunks++] = page;
		else
			tbm->spages[npages++] = page;
	}
	Assert(npages == tbm->npages);
	Assert(nchunks == tbm->nchunks);
	if (npages > 1)
		qsort(tbm->spages, npages, sizeof(PagetableEntry *), tbm_comparator);
	if (nchunks > 1)
		qsort(tbm->schunks, nchunks, sizeof(PagetableEntry *), tbm_comparator);
	/*
	 * Reset iteration pointers.
	 */
	tbm->spageptr = 0;
	tbm->schunkptr = 0;
	tbm->schunkbit = 0;
}

/*
 * tbm_iterate - scan through next page of a TIDBitmap
 *
 * Returns a TBMIterateResult representing one page, or NULL if there are
 * no more pages to scan.  Pages are guaranteed to be delivered in numerical
 * order.  If result->ntuples < 0, then the bitmap is "lossy" and failed to
 * remember the exact tuples to look at on this page --- the caller must
 * examine all tuples on the page and check if they meet the intended
 * condition.
 */
TBMIterateResult *
tbm_iterate(TIDBitmap *tbm)
{
	TBMIterateResult *output = tbm->output;

	Assert(tbm->iterating);
	/*
	 * If lossy chunk pages remain, make sure we've advanced schunkptr/
	 * schunkbit to the next set bit.
	 */
	while (tbm->schunkptr < tbm->nchunks)
	{
		PagetableEntry *chunk = tbm->schunks[tbm->schunkptr];
		int		schunkbit = tbm->schunkbit;

		while (schunkbit < PAGES_PER_CHUNK)
		{
			int		wordnum = WORDNUM(schunkbit);
			int		bitnum = BITNUM(schunkbit);

			if ((chunk->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
				break;
			schunkbit++;
		}
		if (schunkbit < PAGES_PER_CHUNK)
		{
			tbm->schunkbit = schunkbit;
			break;
		}
		/* advance to next chunk */
		tbm->schunkptr++;
		tbm->schunkbit = 0;
	}
	/*
	 * If both chunk and per-page data remain, must output the numerically
	 * earlier page.
	 */
	if (tbm->schunkptr < tbm->nchunks)
	{
		PagetableEntry *chunk = tbm->schunks[tbm->schunkptr];
		BlockNumber chunk_blockno;

		chunk_blockno = chunk->blockno + tbm->schunkbit;
		if (tbm->spageptr >= tbm->npages ||
			chunk_blockno < tbm->spages[tbm->spageptr]->blockno)
		{
			/* Return a lossy page indicator from the chunk */
			output->blockno = chunk_blockno;
			output->ntuples = -1;
			tbm->schunkbit++;
			return output;
		}
	}

	if (tbm->spageptr < tbm->npages)
	{
		PagetableEntry *page = tbm->spages[tbm->spageptr];
		int			ntuples;
		int			wordnum;

		/* scan bitmap to extract individual offset numbers */
		ntuples = 0;
		for (wordnum = 0; wordnum < WORDS_PER_PAGE; wordnum++)
		{
			bitmapword	w = page->words[wordnum];

			if (w != 0)
			{
				int			off = wordnum * BITS_PER_BITMAPWORD + 1;

				while (w != 0)
				{
					if (w & 1)
						output->offsets[ntuples++] = (OffsetNumber) off;
					off++;
					w >>= 1;
				}
			}
		}
		output->blockno = page->blockno;
		output->ntuples = ntuples;
		tbm->spageptr++;
		return output;
	}

	/* Nothing more in the bitmap */
	return NULL;
}

/*
 * tbm_find_pageentry - find a PagetableEntry for the pageno
 *
 * Returns NULL if there is no non-lossy entry for the pageno.
 */
static PagetableEntry *
tbm_find_pageentry(const TIDBitmap *tbm, BlockNumber pageno)
{
	PagetableEntry *page;

	page = (PagetableEntry *) hash_search(tbm->pagetable,
										  (void *) &pageno,
										  HASH_FIND, NULL);
	if (page == NULL)
		return NULL;
	if (page->ischunk)
		return NULL;			/* don't want a lossy chunk header */
	return page;
}

/*
 * tbm_get_pageentry - find or create a PagetableEntry for the pageno
 *
 * If new, the entry is marked as an exact (non-chunk) entry.
 *
 * This may cause the table to exceed the desired memory size.  It is
 * up to the caller to call tbm_lossify() at the next safe point if so.
 */
static PagetableEntry *
tbm_get_pageentry(TIDBitmap *tbm, BlockNumber pageno)
{
	PagetableEntry *page;
	bool		found;

	/* Look up or create an entry */
	page = (PagetableEntry *) hash_search(tbm->pagetable,
										  (void *) &pageno,
										  HASH_ENTER, &found);
	if (page == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Initialize it if not present before */
	if (!found)
	{
		MemSet(page, 0, sizeof(PagetableEntry));
		page->blockno = pageno;
		/* must count it too */
		tbm->nentries++;
		tbm->npages++;
	}

	return page;
}

/*
 * tbm_page_is_lossy - is the page marked as lossily stored?
 */
static bool
tbm_page_is_lossy(const TIDBitmap *tbm, BlockNumber pageno)
{
	PagetableEntry *page;
	BlockNumber chunk_pageno;
	int			bitno;

	/* we can skip the lookup if there are no lossy chunks */
	if (tbm->nchunks == 0)
		return false;

	bitno = pageno % PAGES_PER_CHUNK;
	chunk_pageno = pageno - bitno;
	page = (PagetableEntry *) hash_search(tbm->pagetable,
										  (void *) &chunk_pageno,
										  HASH_FIND, NULL);
	if (page != NULL && page->ischunk)
	{
		int		wordnum = WORDNUM(bitno);
		int		bitnum = BITNUM(bitno);

		if ((page->words[wordnum] & ((bitmapword) 1 << bitnum)) != 0)
			return true;
	}
	return false;
}

/*
 * tbm_mark_page_lossy - mark the page number as lossily stored
 *
 * This may cause the table to exceed the desired memory size.  It is
 * up to the caller to call tbm_lossify() at the next safe point if so.
 */
static void
tbm_mark_page_lossy(TIDBitmap *tbm, BlockNumber pageno)
{
	PagetableEntry *page;
	bool		found;
	BlockNumber chunk_pageno;
	int			bitno;
	int			wordnum;
	int			bitnum;

	bitno = pageno % PAGES_PER_CHUNK;
	chunk_pageno = pageno - bitno;

	/*
	 * Remove any extant non-lossy entry for the page.  If the page is
	 * its own chunk header, however, we skip this and handle the case
	 * below.
	 */
	if (bitno != 0)
	{
		if (hash_search(tbm->pagetable,
						(void *) &pageno,
						HASH_REMOVE, NULL) != NULL)
		{
			/* It was present, so adjust counts */
			tbm->nentries--;
			tbm->npages--;		/* assume it must have been non-lossy */
		}
	}

	/* Look up or create entry for chunk-header page */
	page = (PagetableEntry *) hash_search(tbm->pagetable,
										  (void *) &chunk_pageno,
										  HASH_ENTER, &found);
	if (page == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("out of memory")));

	/* Initialize it if not present before */
	if (!found)
	{
		MemSet(page, 0, sizeof(PagetableEntry));
		page->blockno = chunk_pageno;
		page->ischunk = true;
		/* must count it too */
		tbm->nentries++;
		tbm->nchunks++;
	}
	else if (!page->ischunk)
	{
		/* chunk header page was formerly non-lossy, make it lossy */
		MemSet(page, 0, sizeof(PagetableEntry));
		page->blockno = chunk_pageno;
		page->ischunk = true;
		/* we assume it had some tuple bit(s) set, so mark it lossy */
		page->words[0] = ((bitmapword) 1 << 0);
		/* adjust counts */
		tbm->nchunks++;
		tbm->npages--;
	}

	/* Now set the original target page's bit */
	wordnum = WORDNUM(bitno);
	bitnum = BITNUM(bitno);
	page->words[wordnum] |= ((bitmapword) 1 << bitnum);
}

/*
 * tbm_lossify - lose some information to get back under the memory limit
 */
static void
tbm_lossify(TIDBitmap *tbm)
{
	HASH_SEQ_STATUS status;
	PagetableEntry *page;

	/*
	 * XXX Really stupid implementation: this just lossifies pages in
	 * essentially random order.  We should be paying some attention
	 * to the number of bits set in each page, instead.  Also it might
	 * be a good idea to lossify more than the minimum number of pages
	 * during each call.
	 */
	Assert(!tbm->iterating);
	hash_seq_init(&status, tbm->pagetable);
	while ((page = (PagetableEntry *) hash_seq_search(&status)) != NULL)
	{
		if (page->ischunk)
			continue;			/* already a chunk header */
		/*
		 * If the page would become a chunk header, we won't save anything
		 * by converting it to lossy, so skip it.
		 */
		if ((page->blockno % PAGES_PER_CHUNK) == 0)
			continue;

		/* This does the dirty work ... */
		tbm_mark_page_lossy(tbm, page->blockno);

		if (tbm->nentries <= tbm->maxentries)
			return;				/* we have done enough */

		/*
		 * Note: tbm_mark_page_lossy may have inserted a lossy chunk into
		 * the hashtable.  We can continue the same seq_search scan since
		 * we do not care whether we visit lossy chunks or not.
		 */
	}
}

/*
 * qsort comparator to handle PagetableEntry pointers.
 */
static int
tbm_comparator(const void *left, const void *right)
{
	BlockNumber l = (*((const PagetableEntry **) left))->blockno;
	BlockNumber r = (*((const PagetableEntry **) right))->blockno;

	if (l < r)
		return -1;
	else if (l > r)
		return 1;
	return 0;
}
