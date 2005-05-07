/*-------------------------------------------------------------------------
 *
 * vacuumlazy.c
 *	  Concurrent ("lazy") vacuuming.
 *
 *
 * The major space usage for LAZY VACUUM is storage for the array of dead
 * tuple TIDs, with the next biggest need being storage for per-disk-page
 * free space info.  We want to ensure we can vacuum even the very largest
 * relations with finite memory space usage.  To do that, we set upper bounds
 * on the number of tuples and pages we will keep track of at once.
 *
 * We are willing to use at most VacuumMem memory space to keep track of
 * dead tuples.  We initially allocate an array of TIDs of that size.
 * If the array threatens to overflow, we suspend the heap scan phase
 * and perform a pass of index cleanup and page compaction, then resume
 * the heap scan with an empty TID array.
 *
 * We can limit the storage for page free space to MaxFSMPages entries,
 * since that's the most the free space map will be willing to remember
 * anyway.	If the relation has fewer than that many pages with free space,
 * life is easy: just build an array of per-page info.	If it has more,
 * we store the free space info as a heap ordered by amount of free space,
 * so that we can discard the pages with least free space to ensure we never
 * have more than MaxFSMPages entries in all.  The surviving page entries
 * are passed to the free space map at conclusion of the scan.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/vacuumlazy.c,v 1.32.2.1 2005/05/07 21:33:21 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xlog.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "utils/lsyscache.h"


/*
 * Space/time tradeoff parameters: do these need to be user-tunable?
 *
 * To consider truncating the relation, we want there to be at least
 * REL_TRUNCATE_MINIMUM or (relsize / REL_TRUNCATE_FRACTION) (whichever
 * is less) potentially-freeable pages.
 */
#define REL_TRUNCATE_MINIMUM	1000
#define REL_TRUNCATE_FRACTION	16

/* MAX_TUPLES_PER_PAGE can be a conservative upper limit */
#define MAX_TUPLES_PER_PAGE		((int) (BLCKSZ / sizeof(HeapTupleHeaderData)))


typedef struct LVRelStats
{
	/* Overall statistics about rel */
	BlockNumber rel_pages;
	double		rel_tuples;
	BlockNumber nonempty_pages; /* actually, last nonempty page + 1 */
	Size		threshold;		/* minimum interesting free space */
	/* List of TIDs of tuples we intend to delete */
	/* NB: this list is ordered by TID address */
	int			num_dead_tuples;	/* current # of entries */
	int			max_dead_tuples;	/* # slots allocated in array */
	ItemPointer dead_tuples;	/* array of ItemPointerData */
	/* Array or heap of per-page info about free space */
	/* We use a simple array until it fills up, then convert to heap */
	bool		fs_is_heap;		/* are we using heap organization? */
	int			num_free_pages; /* current # of entries */
	int			max_free_pages; /* # slots allocated in array */
	PageFreeSpaceInfo *free_pages;		/* array or heap of blkno/avail */
} LVRelStats;


static int	elevel = -1;

static TransactionId OldestXmin;
static TransactionId FreezeLimit;


/* non-export function prototypes */
static void lazy_scan_heap(Relation onerel, LVRelStats *vacrelstats,
			   Relation *Irel, int nindexes);
static void lazy_vacuum_heap(Relation onerel, LVRelStats *vacrelstats);
static void lazy_scan_index(Relation indrel, LVRelStats *vacrelstats);
static void lazy_vacuum_index(Relation indrel, LVRelStats *vacrelstats);
static int lazy_vacuum_page(Relation onerel, BlockNumber blkno, Buffer buffer,
				 int tupindex, LVRelStats *vacrelstats);
static void lazy_truncate_heap(Relation onerel, LVRelStats *vacrelstats);
static BlockNumber count_nondeletable_pages(Relation onerel,
						 LVRelStats *vacrelstats);
static void lazy_space_alloc(LVRelStats *vacrelstats, BlockNumber relblocks);
static void lazy_record_dead_tuple(LVRelStats *vacrelstats,
					   ItemPointer itemptr);
static void lazy_record_free_space(LVRelStats *vacrelstats,
					   BlockNumber page, Size avail);
static bool lazy_tid_reaped(ItemPointer itemptr, void *state);
static bool dummy_tid_reaped(ItemPointer itemptr, void *state);
static void lazy_update_fsm(Relation onerel, LVRelStats *vacrelstats);
static int	vac_cmp_itemptr(const void *left, const void *right);
static int	vac_cmp_page_spaces(const void *left, const void *right);


/*
 *	lazy_vacuum_rel() -- perform LAZY VACUUM for one heap relation
 *
 *		This routine vacuums a single heap, cleans out its indexes, and
 *		updates its num_pages and num_tuples statistics.
 *
 *		At entry, we have already established a transaction and opened
 *		and locked the relation.
 */
void
lazy_vacuum_rel(Relation onerel, VacuumStmt *vacstmt)
{
	LVRelStats *vacrelstats;
	Relation   *Irel;
	int			nindexes;
	bool		hasindex;
	BlockNumber possibly_freeable;

	if (vacstmt->verbose)
		elevel = INFO;
	else
		elevel = DEBUG2;

	vacuum_set_xid_limits(vacstmt, onerel->rd_rel->relisshared,
						  &OldestXmin, &FreezeLimit);

	vacrelstats = (LVRelStats *) palloc0(sizeof(LVRelStats));

	/* Set threshold for interesting free space = average request size */
	/* XXX should we scale it up or down?  Adjust vacuum.c too, if so */
	vacrelstats->threshold = GetAvgFSMRequestSize(&onerel->rd_node);

	/* Open all indexes of the relation */
	vac_open_indexes(onerel, &nindexes, &Irel);
	hasindex = (nindexes > 0);

	/* Do the vacuuming */
	lazy_scan_heap(onerel, vacrelstats, Irel, nindexes);

	/* Done with indexes */
	vac_close_indexes(nindexes, Irel);

	/*
	 * Optionally truncate the relation.
	 *
	 * Don't even think about it unless we have a shot at releasing a goodly
	 * number of pages.  Otherwise, the time taken isn't worth it.
	 */
	possibly_freeable = vacrelstats->rel_pages - vacrelstats->nonempty_pages;
	if (possibly_freeable >= REL_TRUNCATE_MINIMUM ||
	 possibly_freeable >= vacrelstats->rel_pages / REL_TRUNCATE_FRACTION)
		lazy_truncate_heap(onerel, vacrelstats);

	/* Update shared free space map with final free space info */
	lazy_update_fsm(onerel, vacrelstats);

	/* Update statistics in pg_class */
	vac_update_relstats(RelationGetRelid(onerel), vacrelstats->rel_pages,
						vacrelstats->rel_tuples, hasindex);
}


/*
 *	lazy_scan_heap() -- scan an open heap relation
 *
 *		This routine sets commit status bits, builds lists of dead tuples
 *		and pages with free space, and calculates statistics on the number
 *		of live tuples in the heap.  When done, or when we run low on space
 *		for dead-tuple TIDs, invoke vacuuming of indexes and heap.
 */
static void
lazy_scan_heap(Relation onerel, LVRelStats *vacrelstats,
			   Relation *Irel, int nindexes)
{
	BlockNumber nblocks,
				blkno;
	HeapTupleData tuple;
	char	   *relname;
	BlockNumber empty_pages;
	double		num_tuples,
				tups_vacuumed,
				nkeep,
				nunused;
	int			i;
	VacRUsage	ru0;

	vac_init_rusage(&ru0);

	relname = RelationGetRelationName(onerel);
	ereport(elevel,
			(errmsg("vacuuming \"%s.%s\"",
					get_namespace_name(RelationGetNamespace(onerel)),
					relname)));

	empty_pages = 0;
	num_tuples = tups_vacuumed = nkeep = nunused = 0;

	nblocks = RelationGetNumberOfBlocks(onerel);
	vacrelstats->rel_pages = nblocks;
	vacrelstats->nonempty_pages = 0;

	lazy_space_alloc(vacrelstats, nblocks);

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber offnum,
					maxoff;
		bool		pgchanged,
					tupgone,
					hastup;
		int			prev_dead_count;

		CHECK_FOR_INTERRUPTS();

		/*
		 * If we are close to overrunning the available space for
		 * dead-tuple TIDs, pause and do a cycle of vacuuming before we
		 * tackle this page.
		 */
		if ((vacrelstats->max_dead_tuples - vacrelstats->num_dead_tuples) < MAX_TUPLES_PER_PAGE &&
			vacrelstats->num_dead_tuples > 0)
		{
			/* Remove index entries */
			for (i = 0; i < nindexes; i++)
				lazy_vacuum_index(Irel[i], vacrelstats);
			/* Remove tuples from heap */
			lazy_vacuum_heap(onerel, vacrelstats);
			/* Forget the now-vacuumed tuples, and press on */
			vacrelstats->num_dead_tuples = 0;
		}

		buf = ReadBuffer(onerel, blkno);

		/* In this phase we only need shared access to the buffer */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buf);

		if (PageIsNew(page))
		{
			/*
			 * An all-zeroes page could be left over if a backend extends
			 * the relation but crashes before initializing the page.
			 * Reclaim such pages for use.
			 *
			 * We have to be careful here because we could be looking at
			 * a page that someone has just added to the relation and not
			 * yet been able to initialize (see RelationGetBufferForTuple).
			 * To interlock against that, release the buffer read lock
			 * (which we must do anyway) and grab the relation extension
			 * lock before re-locking in exclusive mode.  If the page is
			 * still uninitialized by then, it must be left over from a
			 * crashed backend, and we can initialize it.
			 *
			 * We don't really need the relation lock when this is a new
			 * or temp relation, but it's probably not worth the code space
			 * to check that, since this surely isn't a critical path.
			 *
			 * Note: the comparable code in vacuum.c need not do all this
			 * because it's got exclusive lock on the whole relation.
			 */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			LockPage(onerel, 0, ExclusiveLock);
			UnlockPage(onerel, 0, ExclusiveLock);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			if (PageIsNew(page))
			{
				ereport(WARNING,
						(errmsg("relation \"%s\" page %u is uninitialized --- fixing",
								relname, blkno)));
				PageInit(page, BufferGetPageSize(buf), 0);
				empty_pages++;
				lazy_record_free_space(vacrelstats, blkno,
									   PageGetFreeSpace(page));
			}
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			WriteBuffer(buf);
			continue;
		}

		if (PageIsEmpty(page))
		{
			empty_pages++;
			lazy_record_free_space(vacrelstats, blkno,
								   PageGetFreeSpace(page));
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);
			continue;
		}

		pgchanged = false;
		hastup = false;
		prev_dead_count = vacrelstats->num_dead_tuples;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;
			uint16		sv_infomask;

			itemid = PageGetItemId(page, offnum);

			if (!ItemIdIsUsed(itemid))
			{
				nunused += 1;
				continue;
			}

			tuple.t_datamcxt = NULL;
			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			ItemPointerSet(&(tuple.t_self), blkno, offnum);

			tupgone = false;
			sv_infomask = tuple.t_data->t_infomask;

			switch (HeapTupleSatisfiesVacuum(tuple.t_data, OldestXmin))
			{
				case HEAPTUPLE_DEAD:
					tupgone = true;		/* we can delete the tuple */
					break;
				case HEAPTUPLE_LIVE:

					/*
					 * Tuple is good.  Consider whether to replace its
					 * xmin value with FrozenTransactionId.
					 *
					 * NB: Since we hold only a shared buffer lock here, we
					 * are assuming that TransactionId read/write is
					 * atomic.	This is not the only place that makes such
					 * an assumption.  It'd be possible to avoid the
					 * assumption by momentarily acquiring exclusive lock,
					 * but for the moment I see no need to.
					 */
					if (TransactionIdIsNormal(HeapTupleHeaderGetXmin(tuple.t_data)) &&
						TransactionIdPrecedes(HeapTupleHeaderGetXmin(tuple.t_data),
											  FreezeLimit))
					{
						HeapTupleHeaderSetXmin(tuple.t_data, FrozenTransactionId);
						/* infomask should be okay already */
						Assert(tuple.t_data->t_infomask & HEAP_XMIN_COMMITTED);
						pgchanged = true;
					}
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must not
					 * remove it from relation.
					 */
					nkeep += 1;
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:
					/* This is an expected case during concurrent vacuum */
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:
					/* This is an expected case during concurrent vacuum */
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					break;
			}

			/* check for hint-bit update by HeapTupleSatisfiesVacuum */
			if (sv_infomask != tuple.t_data->t_infomask)
				pgchanged = true;

			/*
			 * Other checks...
			 */
			if (onerel->rd_rel->relhasoids &&
				!OidIsValid(HeapTupleGetOid(&tuple)))
				elog(WARNING, "relation \"%s\" TID %u/%u: OID is invalid",
					 relname, blkno, offnum);

			if (tupgone)
			{
				lazy_record_dead_tuple(vacrelstats, &(tuple.t_self));
				tups_vacuumed += 1;
			}
			else
			{
				num_tuples += 1;
				hastup = true;
			}
		}						/* scan along page */

		/*
		 * If we remembered any tuples for deletion, then the page will be
		 * visited again by lazy_vacuum_heap, which will compute and
		 * record its post-compaction free space.  If not, then we're done
		 * with this page, so remember its free space as-is.
		 */
		if (vacrelstats->num_dead_tuples == prev_dead_count)
		{
			lazy_record_free_space(vacrelstats, blkno,
								   PageGetFreeSpace(page));
		}

		/* Remember the location of the last page with nonremovable tuples */
		if (hastup)
			vacrelstats->nonempty_pages = blkno + 1;

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		if (pgchanged)
			SetBufferCommitInfoNeedsSave(buf);

		ReleaseBuffer(buf);
	}

	/* save stats for use later */
	vacrelstats->rel_tuples = num_tuples;

	/* If any tuples need to be deleted, perform final vacuum cycle */
	/* XXX put a threshold on min number of tuples here? */
	if (vacrelstats->num_dead_tuples > 0)
	{
		/* Remove index entries */
		for (i = 0; i < nindexes; i++)
			lazy_vacuum_index(Irel[i], vacrelstats);
		/* Remove tuples from heap */
		lazy_vacuum_heap(onerel, vacrelstats);
	}
	else
	{
		/* Must do post-vacuum cleanup and statistics update anyway */
		for (i = 0; i < nindexes; i++)
			lazy_scan_index(Irel[i], vacrelstats);
	}

	ereport(elevel,
			(errmsg("\"%s\": found %.0f removable, %.0f nonremovable row versions in %u pages",
					RelationGetRelationName(onerel),
					tups_vacuumed, num_tuples, nblocks),
			 errdetail("%.0f dead row versions cannot be removed yet.\n"
					   "There were %.0f unused item pointers.\n"
					   "%u pages are entirely empty.\n"
					   "%s",
					   nkeep,
					   nunused,
					   empty_pages,
					   vac_show_rusage(&ru0))));
}


/*
 *	lazy_vacuum_heap() -- second pass over the heap
 *
 *		This routine marks dead tuples as unused and compacts out free
 *		space on their pages.  Pages not having dead tuples recorded from
 *		lazy_scan_heap are not visited at all.
 *
 * Note: the reason for doing this as a second pass is we cannot remove
 * the tuples until we've removed their index entries, and we want to
 * process index entry removal in batches as large as possible.
 */
static void
lazy_vacuum_heap(Relation onerel, LVRelStats *vacrelstats)
{
	int			tupindex;
	int			npages;
	VacRUsage	ru0;

	vac_init_rusage(&ru0);
	npages = 0;

	tupindex = 0;
	while (tupindex < vacrelstats->num_dead_tuples)
	{
		BlockNumber tblk;
		Buffer		buf;
		Page		page;

		CHECK_FOR_INTERRUPTS();

		tblk = ItemPointerGetBlockNumber(&vacrelstats->dead_tuples[tupindex]);
		buf = ReadBuffer(onerel, tblk);
		LockBufferForCleanup(buf);
		tupindex = lazy_vacuum_page(onerel, tblk, buf, tupindex, vacrelstats);
		/* Now that we've compacted the page, record its available space */
		page = BufferGetPage(buf);
		lazy_record_free_space(vacrelstats, tblk,
							   PageGetFreeSpace(page));
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		WriteBuffer(buf);
		npages++;
	}

	ereport(elevel,
			(errmsg("\"%s\": removed %d row versions in %d pages",
					RelationGetRelationName(onerel),
					tupindex, npages),
			 errdetail("%s",
					   vac_show_rusage(&ru0))));
}

/*
 *	lazy_vacuum_page() -- free dead tuples on a page
 *					 and repair its fragmentation.
 *
 * Caller is expected to handle reading, locking, and writing the buffer.
 *
 * tupindex is the index in vacrelstats->dead_tuples of the first dead
 * tuple for this page.  We assume the rest follow sequentially.
 * The return value is the first tupindex after the tuples of this page.
 */
static int
lazy_vacuum_page(Relation onerel, BlockNumber blkno, Buffer buffer,
				 int tupindex, LVRelStats *vacrelstats)
{
	OffsetNumber unused[BLCKSZ / sizeof(OffsetNumber)];
	int			uncnt;
	Page		page = BufferGetPage(buffer);
	ItemId		itemid;

	START_CRIT_SECTION();
	for (; tupindex < vacrelstats->num_dead_tuples; tupindex++)
	{
		BlockNumber tblk;
		OffsetNumber toff;

		tblk = ItemPointerGetBlockNumber(&vacrelstats->dead_tuples[tupindex]);
		if (tblk != blkno)
			break;				/* past end of tuples for this block */
		toff = ItemPointerGetOffsetNumber(&vacrelstats->dead_tuples[tupindex]);
		itemid = PageGetItemId(page, toff);
		itemid->lp_flags &= ~LP_USED;
	}

	uncnt = PageRepairFragmentation(page, unused);

	/* XLOG stuff */
	if (!onerel->rd_istemp)
	{
		XLogRecPtr	recptr;

		recptr = log_heap_clean(onerel, buffer, unused, uncnt);
		PageSetLSN(page, recptr);
		PageSetSUI(page, ThisStartUpID);
	}
	else
	{
		/* No XLOG record, but still need to flag that XID exists on disk */
		MyXactMadeTempRelUpdate = true;
	}

	END_CRIT_SECTION();

	return tupindex;
}

/*
 *	lazy_scan_index() -- scan one index relation to update pg_class statistic.
 *
 * We use this when we have no deletions to do.
 */
static void
lazy_scan_index(Relation indrel, LVRelStats *vacrelstats)
{
	IndexBulkDeleteResult *stats;
	IndexVacuumCleanupInfo vcinfo;
	VacRUsage	ru0;

	vac_init_rusage(&ru0);

	/*
	 * If index is unsafe for concurrent access, must lock it.
	 */
	if (!indrel->rd_am->amconcurrent)
		LockRelation(indrel, AccessExclusiveLock);

	/*
	 * Even though we're not planning to delete anything, we use the
	 * ambulkdelete call, because (a) the scan happens within the index AM
	 * for more speed, and (b) it may want to pass private statistics to
	 * the amvacuumcleanup call.
	 */
	stats = index_bulk_delete(indrel, dummy_tid_reaped, NULL);

	/* Do post-VACUUM cleanup, even though we deleted nothing */
	vcinfo.vacuum_full = false;
	vcinfo.message_level = elevel;

	stats = index_vacuum_cleanup(indrel, &vcinfo, stats);

	/*
	 * Release lock acquired above.
	 */
	if (!indrel->rd_am->amconcurrent)
		UnlockRelation(indrel, AccessExclusiveLock);

	if (!stats)
		return;

	/* now update statistics in pg_class */
	vac_update_relstats(RelationGetRelid(indrel),
						stats->num_pages, stats->num_index_tuples,
						false);

	ereport(elevel,
			(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
					RelationGetRelationName(indrel),
					stats->num_index_tuples,
					stats->num_pages),
			 errdetail("%u index pages have been deleted, %u are currently reusable.\n"
					   "%s",
					   stats->pages_deleted, stats->pages_free,
					   vac_show_rusage(&ru0))));

	pfree(stats);
}

/*
 *	lazy_vacuum_index() -- vacuum one index relation.
 *
 *		Delete all the index entries pointing to tuples listed in
 *		vacrelstats->dead_tuples.
 *
 *		Finally, we arrange to update the index relation's statistics in
 *		pg_class.
 */
static void
lazy_vacuum_index(Relation indrel, LVRelStats *vacrelstats)
{
	IndexBulkDeleteResult *stats;
	IndexVacuumCleanupInfo vcinfo;
	VacRUsage	ru0;

	vac_init_rusage(&ru0);

	/*
	 * If index is unsafe for concurrent access, must lock it.
	 */
	if (!indrel->rd_am->amconcurrent)
		LockRelation(indrel, AccessExclusiveLock);

	/* Do bulk deletion */
	stats = index_bulk_delete(indrel, lazy_tid_reaped, (void *) vacrelstats);

	/* Do post-VACUUM cleanup */
	vcinfo.vacuum_full = false;
	vcinfo.message_level = elevel;

	stats = index_vacuum_cleanup(indrel, &vcinfo, stats);

	/*
	 * Release lock acquired above.
	 */
	if (!indrel->rd_am->amconcurrent)
		UnlockRelation(indrel, AccessExclusiveLock);

	if (!stats)
		return;

	/* now update statistics in pg_class */
	vac_update_relstats(RelationGetRelid(indrel),
						stats->num_pages, stats->num_index_tuples,
						false);

	ereport(elevel,
			(errmsg("index \"%s\" now contains %.0f row versions in %u pages",
					RelationGetRelationName(indrel),
					stats->num_index_tuples,
					stats->num_pages),
			 errdetail("%.0f index row versions were removed.\n"
		 "%u index pages have been deleted, %u are currently reusable.\n"
					   "%s",
					   stats->tuples_removed,
					   stats->pages_deleted, stats->pages_free,
					   vac_show_rusage(&ru0))));

	pfree(stats);
}

/*
 * lazy_truncate_heap - try to truncate off any empty pages at the end
 */
static void
lazy_truncate_heap(Relation onerel, LVRelStats *vacrelstats)
{
	BlockNumber old_rel_pages = vacrelstats->rel_pages;
	BlockNumber new_rel_pages;
	PageFreeSpaceInfo *pageSpaces;
	int			n;
	int			i,
				j;
	VacRUsage	ru0;

	vac_init_rusage(&ru0);

	/*
	 * We need full exclusive lock on the relation in order to do
	 * truncation. If we can't get it, give up rather than waiting --- we
	 * don't want to block other backends, and we don't want to deadlock
	 * (which is quite possible considering we already hold a lower-grade
	 * lock).
	 */
	if (!ConditionalLockRelation(onerel, AccessExclusiveLock))
		return;

	/*
	 * Now that we have exclusive lock, look to see if the rel has grown
	 * whilst we were vacuuming with non-exclusive lock.  If so, give up;
	 * the newly added pages presumably contain non-deletable tuples.
	 */
	new_rel_pages = RelationGetNumberOfBlocks(onerel);
	if (new_rel_pages != old_rel_pages)
	{
		/* might as well use the latest news when we update pg_class stats */
		vacrelstats->rel_pages = new_rel_pages;
		UnlockRelation(onerel, AccessExclusiveLock);
		return;
	}

	/*
	 * Scan backwards from the end to verify that the end pages actually
	 * contain nothing we need to keep.  This is *necessary*, not
	 * optional, because other backends could have added tuples to these
	 * pages whilst we were vacuuming.
	 */
	new_rel_pages = count_nondeletable_pages(onerel, vacrelstats);

	if (new_rel_pages >= old_rel_pages)
	{
		/* can't do anything after all */
		UnlockRelation(onerel, AccessExclusiveLock);
		return;
	}

	/*
	 * Okay to truncate.
	 *
	 * First, flush any shared buffers for the blocks we intend to delete.
	 * FlushRelationBuffers is a bit more than we need for this, since it
	 * will also write out dirty buffers for blocks we aren't deleting,
	 * but it's the closest thing in bufmgr's API.
	 */
	i = FlushRelationBuffers(onerel, new_rel_pages);
	if (i < 0)
		elog(ERROR, "FlushRelationBuffers returned %d", i);

	/*
	 * Do the physical truncation.
	 */
	new_rel_pages = smgrtruncate(DEFAULT_SMGR, onerel, new_rel_pages);
	onerel->rd_nblocks = new_rel_pages; /* update relcache immediately */
	onerel->rd_targblock = InvalidBlockNumber;
	vacrelstats->rel_pages = new_rel_pages;		/* save new number of
												 * blocks */

	/*
	 * Drop free-space info for removed blocks; these must not get entered
	 * into the FSM!
	 */
	pageSpaces = vacrelstats->free_pages;
	n = vacrelstats->num_free_pages;
	j = 0;
	for (i = 0; i < n; i++)
	{
		if (pageSpaces[i].blkno < new_rel_pages)
		{
			pageSpaces[j] = pageSpaces[i];
			j++;
		}
	}
	vacrelstats->num_free_pages = j;
	/* We destroyed the heap ordering, so mark array unordered */
	vacrelstats->fs_is_heap = false;

	/*
	 * We keep the exclusive lock until commit (perhaps not necessary)?
	 */

	ereport(elevel,
			(errmsg("\"%s\": truncated %u to %u pages",
					RelationGetRelationName(onerel),
					old_rel_pages, new_rel_pages),
			 errdetail("%s",
					   vac_show_rusage(&ru0))));
}

/*
 * Rescan end pages to verify that they are (still) empty of needed tuples.
 *
 * Returns number of nondeletable pages (last nonempty page + 1).
 */
static BlockNumber
count_nondeletable_pages(Relation onerel, LVRelStats *vacrelstats)
{
	BlockNumber blkno;
	HeapTupleData tuple;

	/* Strange coding of loop control is needed because blkno is unsigned */
	blkno = vacrelstats->rel_pages;
	while (blkno > vacrelstats->nonempty_pages)
	{
		Buffer		buf;
		Page		page;
		OffsetNumber offnum,
					maxoff;
		bool		pgchanged,
					tupgone,
					hastup;

		CHECK_FOR_INTERRUPTS();

		blkno--;

		buf = ReadBuffer(onerel, blkno);

		/* In this phase we only need shared access to the buffer */
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		page = BufferGetPage(buf);

		if (PageIsNew(page) || PageIsEmpty(page))
		{
			/* PageIsNew robably shouldn't happen... */
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buf);
			continue;
		}

		pgchanged = false;
		hastup = false;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			ItemId		itemid;
			uint16		sv_infomask;

			itemid = PageGetItemId(page, offnum);

			if (!ItemIdIsUsed(itemid))
				continue;

			tuple.t_datamcxt = NULL;
			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			ItemPointerSet(&(tuple.t_self), blkno, offnum);

			tupgone = false;
			sv_infomask = tuple.t_data->t_infomask;

			switch (HeapTupleSatisfiesVacuum(tuple.t_data, OldestXmin))
			{
				case HEAPTUPLE_DEAD:
					tupgone = true;		/* we can delete the tuple */
					break;
				case HEAPTUPLE_LIVE:
					/* Shouldn't be necessary to re-freeze anything */
					break;
				case HEAPTUPLE_RECENTLY_DEAD:

					/*
					 * If tuple is recently deleted then we must not
					 * remove it from relation.
					 */
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:
					/* This is an expected case during concurrent vacuum */
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:
					/* This is an expected case during concurrent vacuum */
					break;
				default:
					elog(ERROR, "unexpected HeapTupleSatisfiesVacuum result");
					break;
			}

			/* check for hint-bit update by HeapTupleSatisfiesVacuum */
			if (sv_infomask != tuple.t_data->t_infomask)
				pgchanged = true;

			if (!tupgone)
			{
				hastup = true;
				break;			/* can stop scanning */
			}
		}						/* scan along page */

		LockBuffer(buf, BUFFER_LOCK_UNLOCK);

		if (pgchanged)
			WriteBuffer(buf);
		else
			ReleaseBuffer(buf);

		/* Done scanning if we found a tuple here */
		if (hastup)
			return blkno + 1;
	}

	/*
	 * If we fall out of the loop, all the previously-thought-to-be-empty
	 * pages really are; we need not bother to look at the last
	 * known-nonempty page.
	 */
	return vacrelstats->nonempty_pages;
}

/*
 * lazy_space_alloc - space allocation decisions for lazy vacuum
 *
 * See the comments at the head of this file for rationale.
 */
static void
lazy_space_alloc(LVRelStats *vacrelstats, BlockNumber relblocks)
{
	int			maxtuples;
	int			maxpages;

	maxtuples = (int) ((VacuumMem * 1024L) / sizeof(ItemPointerData));
	/* stay sane if small VacuumMem */
	if (maxtuples < MAX_TUPLES_PER_PAGE)
		maxtuples = MAX_TUPLES_PER_PAGE;

	vacrelstats->num_dead_tuples = 0;
	vacrelstats->max_dead_tuples = maxtuples;
	vacrelstats->dead_tuples = (ItemPointer)
		palloc(maxtuples * sizeof(ItemPointerData));

	maxpages = MaxFSMPages;
	/* No need to allocate more pages than the relation has blocks */
	if (relblocks < (BlockNumber) maxpages)
		maxpages = (int) relblocks;
	/* avoid palloc(0) */
	if (maxpages < 1)
		maxpages = 1;

	vacrelstats->fs_is_heap = false;
	vacrelstats->num_free_pages = 0;
	vacrelstats->max_free_pages = maxpages;
	vacrelstats->free_pages = (PageFreeSpaceInfo *)
		palloc(maxpages * sizeof(PageFreeSpaceInfo));
}

/*
 * lazy_record_dead_tuple - remember one deletable tuple
 */
static void
lazy_record_dead_tuple(LVRelStats *vacrelstats,
					   ItemPointer itemptr)
{
	/*
	 * The array shouldn't overflow under normal behavior, but perhaps it
	 * could if we are given a really small VacuumMem. In that case, just
	 * forget the last few tuples.
	 */
	if (vacrelstats->num_dead_tuples < vacrelstats->max_dead_tuples)
	{
		vacrelstats->dead_tuples[vacrelstats->num_dead_tuples] = *itemptr;
		vacrelstats->num_dead_tuples++;
	}
}

/*
 * lazy_record_free_space - remember free space on one page
 */
static void
lazy_record_free_space(LVRelStats *vacrelstats,
					   BlockNumber page,
					   Size avail)
{
	PageFreeSpaceInfo *pageSpaces;
	int			n;

	/*
	 * A page with less than stats->threshold free space will be forgotten
	 * immediately, and never passed to the free space map.  Removing the
	 * uselessly small entries early saves cycles, and in particular
	 * reduces the amount of time we spend holding the FSM lock when we
	 * finally call RecordRelationFreeSpace.  Since the FSM will probably
	 * drop pages with little free space anyway, there's no point in
	 * making this really small.
	 *
	 * XXX Is it worth trying to measure average tuple size, and using that
	 * to adjust the threshold?  Would be worthwhile if FSM has no stats
	 * yet for this relation.  But changing the threshold as we scan the
	 * rel might lead to bizarre behavior, too.  Also, it's probably
	 * better if vacuum.c has the same thresholding behavior as we do
	 * here.
	 */
	if (avail < vacrelstats->threshold)
		return;

	/* Copy pointers to local variables for notational simplicity */
	pageSpaces = vacrelstats->free_pages;
	n = vacrelstats->max_free_pages;

	/* If we haven't filled the array yet, just keep adding entries */
	if (vacrelstats->num_free_pages < n)
	{
		pageSpaces[vacrelstats->num_free_pages].blkno = page;
		pageSpaces[vacrelstats->num_free_pages].avail = avail;
		vacrelstats->num_free_pages++;
		return;
	}

	/*----------
	 * The rest of this routine works with "heap" organization of the
	 * free space arrays, wherein we maintain the heap property
	 *			avail[(j-1) div 2] <= avail[j]	for 0 < j < n.
	 * In particular, the zero'th element always has the smallest available
	 * space and can be discarded to make room for a new page with more space.
	 * See Knuth's discussion of heap-based priority queues, sec 5.2.3;
	 * but note he uses 1-origin array subscripts, not 0-origin.
	 *----------
	 */

	/* If we haven't yet converted the array to heap organization, do it */
	if (!vacrelstats->fs_is_heap)
	{
		/*
		 * Scan backwards through the array, "sift-up" each value into its
		 * correct position.  We can start the scan at n/2-1 since each
		 * entry above that position has no children to worry about.
		 */
		int			l = n / 2;

		while (--l >= 0)
		{
			BlockNumber R = pageSpaces[l].blkno;
			Size		K = pageSpaces[l].avail;
			int			i;		/* i is where the "hole" is */

			i = l;
			for (;;)
			{
				int			j = 2 * i + 1;

				if (j >= n)
					break;
				if (j + 1 < n && pageSpaces[j].avail > pageSpaces[j + 1].avail)
					j++;
				if (K <= pageSpaces[j].avail)
					break;
				pageSpaces[i] = pageSpaces[j];
				i = j;
			}
			pageSpaces[i].blkno = R;
			pageSpaces[i].avail = K;
		}

		vacrelstats->fs_is_heap = true;
	}

	/* If new page has more than zero'th entry, insert it into heap */
	if (avail > pageSpaces[0].avail)
	{
		/*
		 * Notionally, we replace the zero'th entry with the new data, and
		 * then sift-up to maintain the heap property.	Physically, the
		 * new data doesn't get stored into the arrays until we find the
		 * right location for it.
		 */
		int			i = 0;		/* i is where the "hole" is */

		for (;;)
		{
			int			j = 2 * i + 1;

			if (j >= n)
				break;
			if (j + 1 < n && pageSpaces[j].avail > pageSpaces[j + 1].avail)
				j++;
			if (avail <= pageSpaces[j].avail)
				break;
			pageSpaces[i] = pageSpaces[j];
			i = j;
		}
		pageSpaces[i].blkno = page;
		pageSpaces[i].avail = avail;
	}
}

/*
 *	lazy_tid_reaped() -- is a particular tid deletable?
 *
 *		This has the right signature to be an IndexBulkDeleteCallback.
 *
 *		Assumes dead_tuples array is in sorted order.
 */
static bool
lazy_tid_reaped(ItemPointer itemptr, void *state)
{
	LVRelStats *vacrelstats = (LVRelStats *) state;
	ItemPointer res;

	res = (ItemPointer) bsearch((void *) itemptr,
								(void *) vacrelstats->dead_tuples,
								vacrelstats->num_dead_tuples,
								sizeof(ItemPointerData),
								vac_cmp_itemptr);

	return (res != NULL);
}

/*
 * Dummy version for lazy_scan_index.
 */
static bool
dummy_tid_reaped(ItemPointer itemptr, void *state)
{
	return false;
}

/*
 * Update the shared Free Space Map with the info we now have about
 * free space in the relation, discarding any old info the map may have.
 */
static void
lazy_update_fsm(Relation onerel, LVRelStats *vacrelstats)
{
	PageFreeSpaceInfo *pageSpaces = vacrelstats->free_pages;
	int			nPages = vacrelstats->num_free_pages;

	/*
	 * Sort data into order, as required by RecordRelationFreeSpace.
	 */
	if (nPages > 1)
		qsort(pageSpaces, nPages, sizeof(PageFreeSpaceInfo),
			  vac_cmp_page_spaces);

	RecordRelationFreeSpace(&onerel->rd_node, nPages, pageSpaces);
}

/*
 * Comparator routines for use with qsort() and bsearch().
 */
static int
vac_cmp_itemptr(const void *left, const void *right)
{
	BlockNumber lblk,
				rblk;
	OffsetNumber loff,
				roff;

	lblk = ItemPointerGetBlockNumber((ItemPointer) left);
	rblk = ItemPointerGetBlockNumber((ItemPointer) right);

	if (lblk < rblk)
		return -1;
	if (lblk > rblk)
		return 1;

	loff = ItemPointerGetOffsetNumber((ItemPointer) left);
	roff = ItemPointerGetOffsetNumber((ItemPointer) right);

	if (loff < roff)
		return -1;
	if (loff > roff)
		return 1;

	return 0;
}

static int
vac_cmp_page_spaces(const void *left, const void *right)
{
	PageFreeSpaceInfo *linfo = (PageFreeSpaceInfo *) left;
	PageFreeSpaceInfo *rinfo = (PageFreeSpaceInfo *) right;

	if (linfo->blkno < rinfo->blkno)
		return -1;
	else if (linfo->blkno > rinfo->blkno)
		return 1;
	return 0;
}
