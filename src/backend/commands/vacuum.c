/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  the postgres vacuum cleaner
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/vacuum.c,v 1.173 2000/11/16 22:30:19 tgl Exp $
 *

 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "utils/temprel.h"

#ifndef HAVE_GETRUSAGE
#include "rusagestub.h"
#else
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifdef XLOG
#include "access/xlog.h"
XLogRecPtr	log_heap_move(Relation reln, 
				ItemPointerData from, HeapTuple newtup);
#endif

static MemoryContext vac_context = NULL;

static int	MESSAGE_LEVEL;		/* message level */

static TransactionId XmaxRecent;

/* non-export function prototypes */
static void vacuum_init(void);
static void vacuum_shutdown(void);
static void vac_vacuum(NameData *VacRelP, bool analyze, List *anal_cols2);
static VRelList getrels(NameData *VacRelP);
static void vacuum_rel(Oid relid, bool analyze, bool is_toastrel);
static void scan_heap(VRelStats *vacrelstats, Relation onerel, VacPageList vacuum_pages, VacPageList fraged_pages);
static void repair_frag(VRelStats *vacrelstats, Relation onerel, VacPageList vacuum_pages, VacPageList fraged_pages, int nindices, Relation *Irel);
static void vacuum_heap(VRelStats *vacrelstats, Relation onerel, VacPageList vacpagelist);
static void vacuum_page(Page page, VacPage vacpage);
static void vacuum_index(VacPageList vacpagelist, Relation indrel, int num_tuples, int keep_tuples);
static void scan_index(Relation indrel, int num_tuples);
static void update_relstats(Oid relid, int num_pages, int num_tuples, bool hasindex, VRelStats *vacrelstats);
static VacPage tid_reaped(ItemPointer itemptr, VacPageList vacpagelist);
static void reap_page(VacPageList vacpagelist, VacPage vacpage);
static void vpage_insert(VacPageList vacpagelist, VacPage vpnew);
static void get_indices(Relation relation, int *nindices, Relation **Irel);
static void close_indices(int nindices, Relation *Irel);
static IndexInfo **get_index_desc(Relation onerel, int nindices,
								  Relation *Irel);
static void *vac_find_eq(void *bot, int nelem, int size, void *elm,
			 int (*compar) (const void *, const void *));
static int	vac_cmp_blk(const void *left, const void *right);
static int	vac_cmp_offno(const void *left, const void *right);
static int	vac_cmp_vtlinks(const void *left, const void *right);
static bool enough_space(VacPage vacpage, Size len);
static char *show_rusage(struct rusage * ru0);


void
vacuum(char *vacrel, bool verbose, bool analyze, List *anal_cols)
{
	NameData	VacRel;
	Name		VacRelName;
	MemoryContext old;
	List	   *le;
	List	   *anal_cols2 = NIL;

	if (anal_cols != NIL && !analyze)
		elog(ERROR, "Can't vacuum columns, only tables.  You can 'vacuum analyze' columns.");

	/*
	 * We cannot run VACUUM inside a user transaction block; if we were
	 * inside a transaction, then our commit- and
	 * start-transaction-command calls would not have the intended effect!
	 * Furthermore, the forced commit that occurs before truncating the
	 * relation's file would have the effect of committing the rest of the
	 * user's transaction too, which would certainly not be the desired
	 * behavior.
	 */
	if (IsTransactionBlock())
		elog(ERROR, "VACUUM cannot run inside a BEGIN/END block");

	if (verbose)
		MESSAGE_LEVEL = NOTICE;
	else
		MESSAGE_LEVEL = DEBUG;

	/*
	 * Create special memory context for cross-transaction storage.
	 *
	 * Since it is a child of QueryContext, it will go away eventually
	 * even if we suffer an error; there's no need for special abort
	 * cleanup logic.
	 */
	vac_context = AllocSetContextCreate(QueryContext,
										"Vacuum",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);

	/* vacrel gets de-allocated on xact commit, so copy it to safe storage */
	if (vacrel)
	{
		namestrcpy(&VacRel, vacrel);
		VacRelName = &VacRel;
	}
	else
		VacRelName = NULL;

	/* must also copy the column list, if any, to safe storage */
	old = MemoryContextSwitchTo(vac_context);
	foreach(le, anal_cols)
	{
		char	   *col = (char *) lfirst(le);

		anal_cols2 = lappend(anal_cols2, pstrdup(col));
	}
	MemoryContextSwitchTo(old);

	/*
	 * Start up the vacuum cleaner.
	 *
	 * NOTE: since this commits the current transaction, the memory holding
	 * any passed-in parameters gets freed here.  We must have already
	 * copied pass-by-reference parameters to safe storage.  Don't make me
	 * fix this again!
	 */
	vacuum_init();

	/* vacuum the database */
	vac_vacuum(VacRelName, analyze, anal_cols2);

	/* clean up */
	vacuum_shutdown();
}

/*
 *	vacuum_init(), vacuum_shutdown() -- start up and shut down the vacuum cleaner.
 *
 *		Formerly, there was code here to prevent more than one VACUUM from
 *		executing concurrently in the same database.  However, there's no
 *		good reason to prevent that, and manually removing lockfiles after
 *		a vacuum crash was a pain for dbadmins.  So, forget about lockfiles,
 *		and just rely on the exclusive lock we grab on each target table
 *		to ensure that there aren't two VACUUMs running on the same table
 *		at the same time.
 *
 *		The strangeness with committing and starting transactions in the
 *		init and shutdown routines is due to the fact that the vacuum cleaner
 *		is invoked via an SQL command, and so is already executing inside
 *		a transaction.	We need to leave ourselves in a predictable state
 *		on entry and exit to the vacuum cleaner.  We commit the transaction
 *		started in PostgresMain() inside vacuum_init(), and start one in
 *		vacuum_shutdown() to match the commit waiting for us back in
 *		PostgresMain().
 */
static void
vacuum_init()
{
	/* matches the StartTransaction in PostgresMain() */
	CommitTransactionCommand();
}

static void
vacuum_shutdown()
{
	/* on entry, we are not in a transaction */

	/*
	 * Flush the init file that relcache.c uses to save startup time. The
	 * next backend startup will rebuild the init file with up-to-date
	 * information from pg_class.  This lets the optimizer see the stats
	 * that we've collected for certain critical system indexes.  See
	 * relcache.c for more details.
	 *
	 * Ignore any failure to unlink the file, since it might not be there if
	 * no backend has been started since the last vacuum...
	 */
	unlink(RELCACHE_INIT_FILENAME);

	/* matches the CommitTransaction in PostgresMain() */
	StartTransactionCommand();

	/*
	 * Clean up working storage --- note we must do this after
	 * StartTransactionCommand, else we might be trying to delete
	 * the active context!
	 */
	MemoryContextDelete(vac_context);
	vac_context = NULL;
}

/*
 *	vac_vacuum() -- vacuum the database.
 *
 *		This routine builds a list of relations to vacuum, and then calls
 *		code that vacuums them one at a time.  We are careful to vacuum each
 *		relation in a separate transaction in order to avoid holding too many
 *		locks at one time.
 */
static void
vac_vacuum(NameData *VacRelP, bool analyze, List *anal_cols2)
{
	VRelList	vrl,
				cur;

	/* get list of relations */
	vrl = getrels(VacRelP);

	/* vacuum each heap relation */
	for (cur = vrl; cur != (VRelList) NULL; cur = cur->vrl_next)
	{
		vacuum_rel(cur->vrl_relid, analyze, false);
		/* analyze separately so locking is minimized */
		if (analyze)
			analyze_rel(cur->vrl_relid, anal_cols2, MESSAGE_LEVEL);
	}
}

static VRelList
getrels(NameData *VacRelP)
{
	Relation	rel;
	TupleDesc	tupdesc;
	HeapScanDesc scan;
	HeapTuple	tuple;
	VRelList	vrl,
				cur;
	Datum		d;
	char	   *rname;
	char		rkind;
	bool		n;
	bool		found = false;
	ScanKeyData key;

	StartTransactionCommand();

	if (NameStr(*VacRelP))
	{

		/*
		 * we could use the cache here, but it is clearer to use scankeys
		 * for both vacuum cases, bjm 2000/01/19
		 */
		char	   *nontemp_relname;

		/* We must re-map temp table names bjm 2000-04-06 */
		if ((nontemp_relname =
			 get_temp_rel_by_username(NameStr(*VacRelP))) == NULL)
			nontemp_relname = NameStr(*VacRelP);

		ScanKeyEntryInitialize(&key, 0x0, Anum_pg_class_relname,
							   F_NAMEEQ,
							   PointerGetDatum(nontemp_relname));
	}
	else
	{
		ScanKeyEntryInitialize(&key, 0x0, Anum_pg_class_relkind,
							   F_CHAREQ, CharGetDatum('r'));
	}

	vrl = cur = (VRelList) NULL;

	rel = heap_openr(RelationRelationName, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	scan = heap_beginscan(rel, false, SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		found = true;

		d = heap_getattr(tuple, Anum_pg_class_relname, tupdesc, &n);
		rname = (char *) d;

		d = heap_getattr(tuple, Anum_pg_class_relkind, tupdesc, &n);

		rkind = DatumGetChar(d);

		if (rkind != RELKIND_RELATION)
		{
			elog(NOTICE, "Vacuum: can not process indecies, views and certain system tables");
			continue;
		}

		/* get a relation list entry for this guy */
		if (vrl == (VRelList) NULL)
			vrl = cur = (VRelList)
				MemoryContextAlloc(vac_context, sizeof(VRelListData));
		else
		{
			cur->vrl_next = (VRelList)
				MemoryContextAlloc(vac_context, sizeof(VRelListData));
			cur = cur->vrl_next;
		}

		cur->vrl_relid = tuple->t_data->t_oid;
		cur->vrl_next = (VRelList) NULL;
	}

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	if (!found)
		elog(NOTICE, "Vacuum: table not found");

	CommitTransactionCommand();

	return vrl;
}

/*
 *	vacuum_rel() -- vacuum one heap relation
 *
 *		This routine vacuums a single heap, cleans out its indices, and
 *		updates its statistics num_pages and num_tuples statistics.
 *
 *		Doing one heap at a time incurs extra overhead, since we need to
 *		check that the heap exists again just before we vacuum it.	The
 *		reason that we do this is so that vacuuming can be spread across
 *		many small transactions.  Otherwise, two-phase locking would require
 *		us to lock the entire database during one pass of the vacuum cleaner.
 */
static void
vacuum_rel(Oid relid, bool analyze, bool is_toastrel)
{
	Relation	onerel;
	VacPageListData vacuum_pages; /* List of pages to vacuum and/or clean
								 * indices */
	VacPageListData fraged_pages; /* List of pages with space enough for
								 * re-using */
	VacPage    *vacpage;
	Relation   *Irel;
	int32		nindices,
				i;
	VRelStats  *vacrelstats;
	bool		reindex = false;
	Oid			toast_relid;

	if (!is_toastrel)
		StartTransactionCommand();

	/*
	 * Check for user-requested abort.	Note we want this to be inside a
	 * transaction, so xact.c doesn't issue useless NOTICE.
	 */
	if (QueryCancel)
		CancelQuery();

	/*
	 * Race condition -- if the pg_class tuple has gone away since the
	 * last time we saw it, we don't need to vacuum it.
	 */
	if (!SearchSysCacheExists(RELOID,
							  ObjectIdGetDatum(relid),
							  0, 0, 0))
	{
		if (!is_toastrel)
			CommitTransactionCommand();
		return;
	}

	/*
	 * Open the class, get an exclusive lock on it, and check permissions.
	 *
	 * Note we choose to treat permissions failure as a NOTICE and keep
	 * trying to vacuum the rest of the DB --- is this appropriate?
	 */
	onerel = heap_open(relid, AccessExclusiveLock);

	if (!pg_ownercheck(GetUserId(), RelationGetRelationName(onerel),
					   RELNAME))
	{
		elog(NOTICE, "Skipping \"%s\" --- only table owner can VACUUM it",
			 RelationGetRelationName(onerel));
		heap_close(onerel, AccessExclusiveLock);
		if (!is_toastrel)
			CommitTransactionCommand();
		return;
	}

	/*
	 * Remember the relation'ss TOAST relation for later
	 */
	toast_relid = onerel->rd_rel->reltoastrelid;

	/*
	 * Set up statistics-gathering machinery.
	 */
	vacrelstats = (VRelStats *) palloc(sizeof(VRelStats));
	vacrelstats->relid = relid;
	vacrelstats->num_pages = vacrelstats->num_tuples = 0;
	vacrelstats->hasindex = false;

	GetXmaxRecent(&XmaxRecent);

	/* scan it */
	reindex = false;
	vacuum_pages.num_pages = fraged_pages.num_pages = 0;
	scan_heap(vacrelstats, onerel, &vacuum_pages, &fraged_pages);
	if (IsIgnoringSystemIndexes() &&
		IsSystemRelationName(RelationGetRelationName(onerel)))
		reindex = true;

	/* Now open indices */
	nindices = 0;
	Irel = (Relation *) NULL;
	get_indices(onerel, &nindices, &Irel);
	if (!Irel)
		reindex = false;
	else if (!RelationGetForm(onerel)->relhasindex)
		reindex = true;
	if (nindices > 0)
		vacrelstats->hasindex = true;
	else
		vacrelstats->hasindex = false;
	if (reindex)
	{
		for (i = 0; i < nindices; i++)
			index_close(Irel[i]);
		Irel = (Relation *) NULL;
		activate_indexes_of_a_table(relid, false);
	}

	/* Clean/scan index relation(s) */
	if (Irel != (Relation *) NULL)
	{
		if (vacuum_pages.num_pages > 0)
		{
			for (i = 0; i < nindices; i++)
				vacuum_index(&vacuum_pages, Irel[i],
							 vacrelstats->num_tuples, 0);
		}
		else
		{
			/* just scan indices to update statistic */
			for (i = 0; i < nindices; i++)
				scan_index(Irel[i], vacrelstats->num_tuples);
		}
	}

	if (fraged_pages.num_pages > 0)
	{
		/* Try to shrink heap */
		repair_frag(vacrelstats, onerel, &vacuum_pages, &fraged_pages,
					nindices, Irel);
	}
	else
	{
		if (Irel != (Relation *) NULL)
			close_indices(nindices, Irel);
		if (vacuum_pages.num_pages > 0)
		{
			/* Clean pages from vacuum_pages list */
			vacuum_heap(vacrelstats, onerel, &vacuum_pages);
		}
		else
		{
			/*
			 * Flush dirty pages out to disk.  We must do this even if we
			 * didn't do anything else, because we want to ensure that all
			 * tuples have correct on-row commit status on disk (see
			 * bufmgr.c's comments for FlushRelationBuffers()).
			 */
			i = FlushRelationBuffers(onerel, vacrelstats->num_pages);
			if (i < 0)
				elog(ERROR, "VACUUM (vacuum_rel): FlushRelationBuffers returned %d",
					 i);
		}
	}
	if (reindex)
		activate_indexes_of_a_table(relid, true);

	/*
	 * ok - free vacuum_pages list of reaped pages
	 *
	 * Isn't this a waste of code?  Upcoming commit should free memory, no?
	 */
	if (vacuum_pages.num_pages > 0)
	{
		vacpage = vacuum_pages.pagedesc;
		for (i = 0; i < vacuum_pages.num_pages; i++, vacpage++)
			pfree(*vacpage);
		pfree(vacuum_pages.pagedesc);
		if (fraged_pages.num_pages > 0)
			pfree(fraged_pages.pagedesc);
	}

	/* all done with this class, but hold lock until commit */
	heap_close(onerel, NoLock);

	/* update statistics in pg_class */
	update_relstats(vacrelstats->relid, vacrelstats->num_pages,
					vacrelstats->num_tuples, vacrelstats->hasindex,
					vacrelstats);

	/*
	 * If the relation has a secondary toast one, vacuum that too
	 * while we still hold the lock on the master table. We don't
	 * need to propagate "analyze" to it, because the toaster
	 * always uses hardcoded index access and statistics are
	 * totally unimportant for toast relations
	 */
	if (toast_relid != InvalidOid)
		vacuum_rel(toast_relid, false, true);

	/* next command frees attribute stats */
	if (!is_toastrel)
		CommitTransactionCommand();
}

/*
 *	scan_heap() -- scan an open heap relation
 *
 *		This routine sets commit times, constructs vacuum_pages list of
 *		empty/uninitialized pages and pages with dead tuples and
 *		~LP_USED line pointers, constructs fraged_pages list of pages
 *		appropriate for purposes of shrinking and maintains statistics
 *		on the number of live tuples in a heap.
 */
static void
scan_heap(VRelStats *vacrelstats, Relation onerel,
			VacPageList vacuum_pages, VacPageList fraged_pages)
{
	BlockNumber nblocks,
				blkno;
	ItemId		itemid;
	Buffer		buf;
	HeapTupleData tuple;
	Page		page,
				tempPage = NULL;
	OffsetNumber offnum,
				maxoff;
	bool		pgchanged,
				tupgone,
				dobufrel,
				notup;
	char	   *relname;
	VacPage		vacpage,
				vp;
	uint32		tups_vacuumed,
				num_tuples,
				nkeep,
				nunused,
				ncrash,
				empty_pages,
				new_pages,
				changed_pages,
				empty_end_pages;
	Size		free_size,
				usable_free_size;
	Size		min_tlen = MaxTupleSize;
	Size		max_tlen = 0;
	int32		i;
	bool		do_shrinking = true;
	VTupleLink	vtlinks = (VTupleLink) palloc(100 * sizeof(VTupleLinkData));
	int			num_vtlinks = 0;
	int			free_vtlinks = 100;
	struct rusage ru0;

	getrusage(RUSAGE_SELF, &ru0);

	relname = RelationGetRelationName(onerel);
	elog(MESSAGE_LEVEL, "--Relation %s--", relname);

	tups_vacuumed = num_tuples = nkeep = nunused = ncrash = empty_pages =
		new_pages = changed_pages = empty_end_pages = 0;
	free_size = usable_free_size = 0;

	nblocks = RelationGetNumberOfBlocks(onerel);

	vacpage = (VacPage) palloc(sizeof(VacPageData) + MaxOffsetNumber * sizeof(OffsetNumber));
	vacpage->offsets_used = 0;

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		buf = ReadBuffer(onerel, blkno);
		page = BufferGetPage(buf);
		vacpage->blkno = blkno;
		vacpage->offsets_free = 0;

		if (PageIsNew(page))
		{
			elog(NOTICE, "Rel %s: Uninitialized page %u - fixing",
				 relname, blkno);
			PageInit(page, BufferGetPageSize(buf), 0);
			vacpage->free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			free_size += (vacpage->free - sizeof(ItemIdData));
			new_pages++;
			empty_end_pages++;
			reap_page(vacuum_pages, vacpage);
			WriteBuffer(buf);
			continue;
		}

		if (PageIsEmpty(page))
		{
			vacpage->free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			free_size += (vacpage->free - sizeof(ItemIdData));
			empty_pages++;
			empty_end_pages++;
			reap_page(vacuum_pages, vacpage);
			ReleaseBuffer(buf);
			continue;
		}

		pgchanged = false;
		notup = true;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			itemid = PageGetItemId(page, offnum);

			/*
			 * Collect un-used items too - it's possible to have indices
			 * pointing here after crash.
			 */
			if (!ItemIdIsUsed(itemid))
			{
				vacpage->offsets[vacpage->offsets_free++] = offnum;
				nunused++;
				continue;
			}

			tuple.t_datamcxt = NULL;
			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple.t_len = ItemIdGetLength(itemid);
			ItemPointerSet(&(tuple.t_self), blkno, offnum);
			tupgone = false;

			if (!(tuple.t_data->t_infomask & HEAP_XMIN_COMMITTED))
			{
				if (tuple.t_data->t_infomask & HEAP_XMIN_INVALID)
					tupgone = true;
				else if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
				{
					if (TransactionIdDidCommit((TransactionId)
											   tuple.t_data->t_cmin))
					{
						tuple.t_data->t_infomask |= HEAP_XMIN_INVALID;
						pgchanged = true;
						tupgone = true;
					}
					else
					{
						tuple.t_data->t_infomask |= HEAP_XMIN_COMMITTED;
						pgchanged = true;
					}
				}
				else if (tuple.t_data->t_infomask & HEAP_MOVED_IN)
				{
					if (!TransactionIdDidCommit((TransactionId)
												tuple.t_data->t_cmin))
					{
						tuple.t_data->t_infomask |= HEAP_XMIN_INVALID;
						pgchanged = true;
						tupgone = true;
					}
					else
					{
						tuple.t_data->t_infomask |= HEAP_XMIN_COMMITTED;
						pgchanged = true;
					}
				}
				else
				{
					if (TransactionIdDidAbort(tuple.t_data->t_xmin))
						tupgone = true;
					else if (TransactionIdDidCommit(tuple.t_data->t_xmin))
					{
						tuple.t_data->t_infomask |= HEAP_XMIN_COMMITTED;
						pgchanged = true;
					}
					else if (!TransactionIdIsInProgress(tuple.t_data->t_xmin))
					{

						/*
						 * Not Aborted, Not Committed, Not in Progress -
						 * so it's from crashed process. - vadim 11/26/96
						 */
						ncrash++;
						tupgone = true;
					}
					else
					{
						elog(NOTICE, "Rel %s: TID %u/%u: InsertTransactionInProgress %u - can't shrink relation",
						   relname, blkno, offnum, tuple.t_data->t_xmin);
						do_shrinking = false;
					}
				}
			}

			/*
			 * here we are concerned about tuples with xmin committed and
			 * xmax unknown or committed
			 */
			if (tuple.t_data->t_infomask & HEAP_XMIN_COMMITTED &&
				!(tuple.t_data->t_infomask & HEAP_XMAX_INVALID))
			{
				if (tuple.t_data->t_infomask & HEAP_XMAX_COMMITTED)
				{
					if (tuple.t_data->t_infomask & HEAP_MARKED_FOR_UPDATE)
					{
						tuple.t_data->t_infomask |= HEAP_XMAX_INVALID;
						tuple.t_data->t_infomask &=
							~(HEAP_XMAX_COMMITTED | HEAP_MARKED_FOR_UPDATE);
						pgchanged = true;
					}
					else
						tupgone = true;
				}
				else if (TransactionIdDidAbort(tuple.t_data->t_xmax))
				{
					tuple.t_data->t_infomask |= HEAP_XMAX_INVALID;
					pgchanged = true;
				}
				else if (TransactionIdDidCommit(tuple.t_data->t_xmax))
				{
					if (tuple.t_data->t_infomask & HEAP_MARKED_FOR_UPDATE)
					{
						tuple.t_data->t_infomask |= HEAP_XMAX_INVALID;
						tuple.t_data->t_infomask &=
							~(HEAP_XMAX_COMMITTED | HEAP_MARKED_FOR_UPDATE);
						pgchanged = true;
					}
					else
						tupgone = true;
				}
				else if (!TransactionIdIsInProgress(tuple.t_data->t_xmax))
				{

					/*
					 * Not Aborted, Not Committed, Not in Progress - so it
					 * from crashed process. - vadim 06/02/97
					 */
					tuple.t_data->t_infomask |= HEAP_XMAX_INVALID;
					tuple.t_data->t_infomask &=
						~(HEAP_XMAX_COMMITTED | HEAP_MARKED_FOR_UPDATE);
					pgchanged = true;
				}
				else
				{
					elog(NOTICE, "Rel %s: TID %u/%u: DeleteTransactionInProgress %u - can't shrink relation",
						 relname, blkno, offnum, tuple.t_data->t_xmax);
					do_shrinking = false;
				}

				/*
				 * If tuple is recently deleted then we must not remove it
				 * from relation.
				 */
				if (tupgone && (tuple.t_data->t_infomask & HEAP_XMIN_INVALID) == 0 && tuple.t_data->t_xmax >= XmaxRecent)
				{
					tupgone = false;
					nkeep++;
					if (!(tuple.t_data->t_infomask & HEAP_XMAX_COMMITTED))
					{
						tuple.t_data->t_infomask |= HEAP_XMAX_COMMITTED;
						pgchanged = true;
					}

					/*
					 * If we do shrinking and this tuple is updated one
					 * then remember it to construct updated tuple
					 * dependencies.
					 */
					if (do_shrinking && !(ItemPointerEquals(&(tuple.t_self),
											   &(tuple.t_data->t_ctid))))
					{
						if (free_vtlinks == 0)
						{
							free_vtlinks = 1000;
							vtlinks = (VTupleLink) repalloc(vtlinks,
										   (free_vtlinks + num_vtlinks) *
												 sizeof(VTupleLinkData));
						}
						vtlinks[num_vtlinks].new_tid = tuple.t_data->t_ctid;
						vtlinks[num_vtlinks].this_tid = tuple.t_self;
						free_vtlinks--;
						num_vtlinks++;
					}
				}
			}

			/*
			 * Other checks...
			 */
			if (!OidIsValid(tuple.t_data->t_oid))
			{
				elog(NOTICE, "Rel %s: TID %u/%u: OID IS INVALID. TUPGONE %d.",
					 relname, blkno, offnum, tupgone);
			}

			if (tupgone)
			{
				ItemId		lpp;

				/*
				 * Here we are building a temporary copy of the page with
				 * dead tuples removed.  Below we will apply
				 * PageRepairFragmentation to the copy, so that we can
				 * determine how much space will be available after
				 * removal of dead tuples.  But note we are NOT changing
				 * the real page yet...
				 */
				if (tempPage == (Page) NULL)
				{
					Size		pageSize;

					pageSize = PageGetPageSize(page);
					tempPage = (Page) palloc(pageSize);
					memmove(tempPage, page, pageSize);
				}

				/* mark it unused on the temp page */
				lpp = &(((PageHeader) tempPage)->pd_linp[offnum - 1]);
				lpp->lp_flags &= ~LP_USED;

				vacpage->offsets[vacpage->offsets_free++] = offnum;
				tups_vacuumed++;
			}
			else
			{
				num_tuples++;
				notup = false;
				if (tuple.t_len < min_tlen)
					min_tlen = tuple.t_len;
				if (tuple.t_len > max_tlen)
					max_tlen = tuple.t_len;
			}
		}

		if (pgchanged)
		{
			WriteBuffer(buf);
			dobufrel = false;
			changed_pages++;
		}
		else
			dobufrel = true;

		if (tempPage != (Page) NULL)
		{						/* Some tuples are gone */
			PageRepairFragmentation(tempPage);
			vacpage->free = ((PageHeader) tempPage)->pd_upper - ((PageHeader) tempPage)->pd_lower;
			free_size += vacpage->free;
			reap_page(vacuum_pages, vacpage);
			pfree(tempPage);
			tempPage = (Page) NULL;
		}
		else if (vacpage->offsets_free > 0)
		{						/* there are only ~LP_USED line pointers */
			vacpage->free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			free_size += vacpage->free;
			reap_page(vacuum_pages, vacpage);
		}
		if (dobufrel)
			ReleaseBuffer(buf);
		if (notup)
			empty_end_pages++;
		else
			empty_end_pages = 0;
	}

	pfree(vacpage);

	/* save stats in the rel list for use later */
	vacrelstats->num_tuples = num_tuples;
	vacrelstats->num_pages = nblocks;
/*	  vacrelstats->natts = attr_cnt;*/
	if (num_tuples == 0)
		min_tlen = max_tlen = 0;
	vacrelstats->min_tlen = min_tlen;
	vacrelstats->max_tlen = max_tlen;

	vacuum_pages->empty_end_pages = empty_end_pages;
	fraged_pages->empty_end_pages = empty_end_pages;

	/*
	 * Try to make fraged_pages keeping in mind that we can't use free
	 * space of "empty" end-pages and last page if it reaped.
	 */
	if (do_shrinking && vacuum_pages->num_pages - empty_end_pages > 0)
	{
		int			nusf;		/* blocks usefull for re-using */

		nusf = vacuum_pages->num_pages - empty_end_pages;
		if ((vacuum_pages->pagedesc[nusf - 1])->blkno == nblocks - empty_end_pages - 1)
			nusf--;

		for (i = 0; i < nusf; i++)
		{
			vp = vacuum_pages->pagedesc[i];
			if (enough_space(vp, min_tlen))
			{
				vpage_insert(fraged_pages, vp);
				usable_free_size += vp->free;
			}
		}
	}

	if (usable_free_size > 0 && num_vtlinks > 0)
	{
		qsort((char *) vtlinks, num_vtlinks, sizeof(VTupleLinkData),
			  vac_cmp_vtlinks);
		vacrelstats->vtlinks = vtlinks;
		vacrelstats->num_vtlinks = num_vtlinks;
	}
	else
	{
		vacrelstats->vtlinks = NULL;
		vacrelstats->num_vtlinks = 0;
		pfree(vtlinks);
	}

	elog(MESSAGE_LEVEL, "Pages %u: Changed %u, reaped %u, Empty %u, New %u; \
Tup %u: Vac %u, Keep/VTL %u/%u, Crash %u, UnUsed %u, MinLen %lu, MaxLen %lu; \
Re-using: Free/Avail. Space %lu/%lu; EndEmpty/Avail. Pages %u/%u. %s",
		 nblocks, changed_pages, vacuum_pages->num_pages, empty_pages,
		 new_pages, num_tuples, tups_vacuumed,
		 nkeep, vacrelstats->num_vtlinks, ncrash,
		 nunused, (unsigned long)min_tlen, (unsigned long)max_tlen,
		 (unsigned long)free_size, (unsigned long)usable_free_size,
		 empty_end_pages, fraged_pages->num_pages,
		 show_rusage(&ru0));

}


/*
 *	repair_frag() -- try to repair relation's fragmentation
 *
 *		This routine marks dead tuples as unused and tries re-use dead space
 *		by moving tuples (and inserting indices if needed). It constructs
 *		Nvacpagelist list of free-ed pages (moved tuples) and clean indices
 *		for them after committing (in hack-manner - without losing locks
 *		and freeing memory!) current transaction. It truncates relation
 *		if some end-blocks are gone away.
 */
static void
repair_frag(VRelStats *vacrelstats, Relation onerel,
			   VacPageList vacuum_pages, VacPageList fraged_pages,
			   int nindices, Relation *Irel)
{
	TransactionId myXID;
	CommandId	myCID;
	Buffer		buf,
				cur_buffer;
	int			nblocks,
				blkno;
	Page		page,
				ToPage = NULL;
	OffsetNumber offnum = 0,
				maxoff = 0,
				newoff,
				max_offset;
	ItemId		itemid,
				newitemid;
	HeapTupleData tuple,
				newtup;
	TupleDesc	tupdesc;
	IndexInfo **indexInfo = NULL;
	Datum		idatum[INDEX_MAX_KEYS];
	char		inulls[INDEX_MAX_KEYS];
	InsertIndexResult iresult;
	VacPageListData Nvacpagelist;
	VacPage		cur_page = NULL,
				last_vacuum_page,
				vacpage,
			   *curpage;
	int			cur_item = 0;
	int			last_move_dest_block = -1,
				last_vacuum_block,
				i = 0;
	Size		tuple_len;
	int			num_moved,
				num_fraged_pages,
				vacuumed_pages;
	int			checked_moved,
				num_tuples,
				keep_tuples = 0;
	bool		isempty,
				dowrite,
				chain_tuple_moved;
	struct rusage ru0;

	getrusage(RUSAGE_SELF, &ru0);

	myXID = GetCurrentTransactionId();
	myCID = GetCurrentCommandId();

	tupdesc = RelationGetDescr(onerel);

	if (Irel != (Relation *) NULL)		/* preparation for index' inserts */
		indexInfo = get_index_desc(onerel, nindices, Irel);

	Nvacpagelist.num_pages = 0;
	num_fraged_pages = fraged_pages->num_pages;
	Assert(vacuum_pages->num_pages > vacuum_pages->empty_end_pages);
	vacuumed_pages = vacuum_pages->num_pages - vacuum_pages->empty_end_pages;
	last_vacuum_page = vacuum_pages->pagedesc[vacuumed_pages - 1];
	last_vacuum_block = last_vacuum_page->blkno;
	cur_buffer = InvalidBuffer;
	num_moved = 0;

	vacpage = (VacPage) palloc(sizeof(VacPageData) + MaxOffsetNumber * sizeof(OffsetNumber));
	vacpage->offsets_used = vacpage->offsets_free = 0;

	/*
	 * Scan pages backwards from the last nonempty page, trying to move
	 * tuples down to lower pages.	Quit when we reach a page that we have
	 * moved any tuples onto.  Note that if a page is still in the
	 * fraged_pages list (list of candidate move-target pages) when we
	 * reach it, we will remove it from the list.  This ensures we never
	 * move a tuple up to a higher page number.
	 *
	 * NB: this code depends on the vacuum_pages and fraged_pages lists being
	 * in order, and on fraged_pages being a subset of vacuum_pages.
	 */
	nblocks = vacrelstats->num_pages;
	for (blkno = nblocks - vacuum_pages->empty_end_pages - 1;
		 blkno > last_move_dest_block;
		 blkno--)
	{
		buf = ReadBuffer(onerel, blkno);
		page = BufferGetPage(buf);

		vacpage->offsets_free = 0;

		isempty = PageIsEmpty(page);

		dowrite = false;
		if (blkno == last_vacuum_block) /* it's reaped page */
		{
			if (last_vacuum_page->offsets_free > 0) /* there are dead tuples */
			{					/* on this page - clean */
				Assert(!isempty);
				vacuum_page(page, last_vacuum_page);
				dowrite = true;
			}
			else
				Assert(isempty);
			--vacuumed_pages;
			if (vacuumed_pages > 0)
			{
				/* get prev reaped page from vacuum_pages */
				last_vacuum_page = vacuum_pages->pagedesc[vacuumed_pages - 1];
				last_vacuum_block = last_vacuum_page->blkno;
			}
			else
			{
				last_vacuum_page = NULL;
				last_vacuum_block = -1;
			}
			if (num_fraged_pages > 0 &&
			fraged_pages->pagedesc[num_fraged_pages - 1]->blkno ==
				(BlockNumber) blkno)
			{
				/* page is in fraged_pages too; remove it */
				--num_fraged_pages;
			}
			if (isempty)
			{
				ReleaseBuffer(buf);
				continue;
			}
		}
		else
			Assert(!isempty);

		chain_tuple_moved = false;		/* no one chain-tuple was moved
										 * off this page, yet */
		vacpage->blkno = blkno;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			itemid = PageGetItemId(page, offnum);

			if (!ItemIdIsUsed(itemid))
				continue;

			tuple.t_datamcxt = NULL;
			tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
			tuple_len = tuple.t_len = ItemIdGetLength(itemid);
			ItemPointerSet(&(tuple.t_self), blkno, offnum);

			if (!(tuple.t_data->t_infomask & HEAP_XMIN_COMMITTED))
			{
				if ((TransactionId) tuple.t_data->t_cmin != myXID)
					elog(ERROR, "Invalid XID in t_cmin");
				if (tuple.t_data->t_infomask & HEAP_MOVED_IN)
					elog(ERROR, "HEAP_MOVED_IN was not expected");

				/*
				 * If this (chain) tuple is moved by me already then I
				 * have to check is it in vacpage or not - i.e. is it moved
				 * while cleaning this page or some previous one.
				 */
				if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
				{
					if (keep_tuples == 0)
						continue;
					if (chain_tuple_moved)		/* some chains was moved
												 * while */
					{			/* cleaning this page */
						Assert(vacpage->offsets_free > 0);
						for (i = 0; i < vacpage->offsets_free; i++)
						{
							if (vacpage->offsets[i] == offnum)
								break;
						}
						if (i >= vacpage->offsets_free) /* not found */
						{
							vacpage->offsets[vacpage->offsets_free++] = offnum;
							keep_tuples--;
						}
					}
					else
					{
						vacpage->offsets[vacpage->offsets_free++] = offnum;
						keep_tuples--;
					}
					continue;
				}
				elog(ERROR, "HEAP_MOVED_OFF was expected");
			}

			/*
			 * If this tuple is in the chain of tuples created in updates
			 * by "recent" transactions then we have to move all chain of
			 * tuples to another places.
			 */
			if ((tuple.t_data->t_infomask & HEAP_UPDATED &&
				 tuple.t_data->t_xmin >= XmaxRecent) ||
				(!(tuple.t_data->t_infomask & HEAP_XMAX_INVALID) &&
				 !(ItemPointerEquals(&(tuple.t_self), &(tuple.t_data->t_ctid)))))
			{
				Buffer		Cbuf = buf;
				Page		Cpage;
				ItemId		Citemid;
				ItemPointerData Ctid;
				HeapTupleData tp = tuple;
				Size		tlen = tuple_len;
				VTupleMove	vtmove = (VTupleMove)
				palloc(100 * sizeof(VTupleMoveData));
				int			num_vtmove = 0;
				int			free_vtmove = 100;
				VacPage		to_vacpage = NULL;
				int			to_item = 0;
				bool		freeCbuf = false;
				int			ti;

				if (vacrelstats->vtlinks == NULL)
					elog(ERROR, "No one parent tuple was found");
				if (cur_buffer != InvalidBuffer)
				{
					WriteBuffer(cur_buffer);
					cur_buffer = InvalidBuffer;
				}

				/*
				 * If this tuple is in the begin/middle of the chain then
				 * we have to move to the end of chain.
				 */
				while (!(tp.t_data->t_infomask & HEAP_XMAX_INVALID) &&
				!(ItemPointerEquals(&(tp.t_self), &(tp.t_data->t_ctid))))
				{
					Ctid = tp.t_data->t_ctid;
					if (freeCbuf)
						ReleaseBuffer(Cbuf);
					freeCbuf = true;
					Cbuf = ReadBuffer(onerel,
									  ItemPointerGetBlockNumber(&Ctid));
					Cpage = BufferGetPage(Cbuf);
					Citemid = PageGetItemId(Cpage,
									  ItemPointerGetOffsetNumber(&Ctid));
					if (!ItemIdIsUsed(Citemid))
					{

						/*
						 * This means that in the middle of chain there
						 * was tuple updated by older (than XmaxRecent)
						 * xaction and this tuple is already deleted by
						 * me. Actually, upper part of chain should be
						 * removed and seems that this should be handled
						 * in scan_heap(), but it's not implemented at
						 * the moment and so we just stop shrinking here.
						 */
						ReleaseBuffer(Cbuf);
						pfree(vtmove);
						vtmove = NULL;
						elog(NOTICE, "Child itemid in update-chain marked as unused - can't continue repair_frag");
						break;
					}
					tp.t_datamcxt = NULL;
					tp.t_data = (HeapTupleHeader) PageGetItem(Cpage, Citemid);
					tp.t_self = Ctid;
					tlen = tp.t_len = ItemIdGetLength(Citemid);
				}
				if (vtmove == NULL)
					break;
				/* first, can chain be moved ? */
				for (;;)
				{
					if (to_vacpage == NULL ||
						!enough_space(to_vacpage, tlen))
					{

						/*
						 * if to_vacpage no longer has enough free space to be
						 * useful, remove it from fraged_pages list
						 */
						if (to_vacpage != NULL &&
						 !enough_space(to_vacpage, vacrelstats->min_tlen))
						{
							Assert(num_fraged_pages > to_item);
							memmove(fraged_pages->pagedesc + to_item,
								fraged_pages->pagedesc + to_item + 1,
									sizeof(VacPage) * (num_fraged_pages - to_item - 1));
							num_fraged_pages--;
						}
						for (i = 0; i < num_fraged_pages; i++)
						{
							if (enough_space(fraged_pages->pagedesc[i], tlen))
								break;
						}

						/* can't move item anywhere */
						if (i == num_fraged_pages)
						{
							for (i = 0; i < num_vtmove; i++)
							{
								Assert(vtmove[i].vacpage->offsets_used > 0);
								(vtmove[i].vacpage->offsets_used)--;
							}
							num_vtmove = 0;
							break;
						}
						to_item = i;
						to_vacpage = fraged_pages->pagedesc[to_item];
					}
					to_vacpage->free -= MAXALIGN(tlen);
					if (to_vacpage->offsets_used >= to_vacpage->offsets_free)
						to_vacpage->free -= MAXALIGN(sizeof(ItemIdData));
					(to_vacpage->offsets_used)++;
					if (free_vtmove == 0)
					{
						free_vtmove = 1000;
						vtmove = (VTupleMove) repalloc(vtmove,
											 (free_vtmove + num_vtmove) *
												 sizeof(VTupleMoveData));
					}
					vtmove[num_vtmove].tid = tp.t_self;
					vtmove[num_vtmove].vacpage = to_vacpage;
					if (to_vacpage->offsets_used == 1)
						vtmove[num_vtmove].cleanVpd = true;
					else
						vtmove[num_vtmove].cleanVpd = false;
					free_vtmove--;
					num_vtmove++;

					/* All done ? */
					if (!(tp.t_data->t_infomask & HEAP_UPDATED) ||
						tp.t_data->t_xmin < XmaxRecent)
						break;

					/* Well, try to find tuple with old row version */
					for (;;)
					{
						Buffer		Pbuf;
						Page		Ppage;
						ItemId		Pitemid;
						HeapTupleData Ptp;
						VTupleLinkData vtld,
								   *vtlp;

						vtld.new_tid = tp.t_self;
						vtlp = (VTupleLink)
							vac_find_eq((void *) (vacrelstats->vtlinks),
									   vacrelstats->num_vtlinks,
									   sizeof(VTupleLinkData),
									   (void *) &vtld,
									   vac_cmp_vtlinks);
						if (vtlp == NULL)
							elog(ERROR, "Parent tuple was not found");
						tp.t_self = vtlp->this_tid;
						Pbuf = ReadBuffer(onerel,
								ItemPointerGetBlockNumber(&(tp.t_self)));
						Ppage = BufferGetPage(Pbuf);
						Pitemid = PageGetItemId(Ppage,
							   ItemPointerGetOffsetNumber(&(tp.t_self)));
						if (!ItemIdIsUsed(Pitemid))
							elog(ERROR, "Parent itemid marked as unused");
						Ptp.t_datamcxt = NULL;
						Ptp.t_data = (HeapTupleHeader) PageGetItem(Ppage, Pitemid);
						Assert(ItemPointerEquals(&(vtld.new_tid),
												 &(Ptp.t_data->t_ctid)));

						/*
						 * Read above about cases when
						 * !ItemIdIsUsed(Citemid) (child item is
						 * removed)... Due to the fact that at the moment
						 * we don't remove unuseful part of update-chain,
						 * it's possible to get too old parent row here.
						 * Like as in the case which caused this problem,
						 * we stop shrinking here. I could try to find
						 * real parent row but want not to do it because
						 * of real solution will be implemented anyway,
						 * latter, and we are too close to 6.5 release. -
						 * vadim 06/11/99
						 */
						if (Ptp.t_data->t_xmax != tp.t_data->t_xmin)
						{
							if (freeCbuf)
								ReleaseBuffer(Cbuf);
							freeCbuf = false;
							ReleaseBuffer(Pbuf);
							for (i = 0; i < num_vtmove; i++)
							{
								Assert(vtmove[i].vacpage->offsets_used > 0);
								(vtmove[i].vacpage->offsets_used)--;
							}
							num_vtmove = 0;
							elog(NOTICE, "Too old parent tuple found - can't continue repair_frag");
							break;
						}
#ifdef NOT_USED					/* I'm not sure that this will wotk
								 * properly... */

						/*
						 * If this tuple is updated version of row and it
						 * was created by the same transaction then no one
						 * is interested in this tuple - mark it as
						 * removed.
						 */
						if (Ptp.t_data->t_infomask & HEAP_UPDATED &&
							Ptp.t_data->t_xmin == Ptp.t_data->t_xmax)
						{
							TransactionIdStore(myXID,
								(TransactionId *) &(Ptp.t_data->t_cmin));
							Ptp.t_data->t_infomask &=
								~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | HEAP_MOVED_IN);
							Ptp.t_data->t_infomask |= HEAP_MOVED_OFF;
							WriteBuffer(Pbuf);
							continue;
						}
#endif
						tp.t_datamcxt = Ptp.t_datamcxt;
						tp.t_data = Ptp.t_data;
						tlen = tp.t_len = ItemIdGetLength(Pitemid);
						if (freeCbuf)
							ReleaseBuffer(Cbuf);
						Cbuf = Pbuf;
						freeCbuf = true;
						break;
					}
					if (num_vtmove == 0)
						break;
				}
				if (freeCbuf)
					ReleaseBuffer(Cbuf);
				if (num_vtmove == 0)	/* chain can't be moved */
				{
					pfree(vtmove);
					break;
				}
				ItemPointerSetInvalid(&Ctid);
				for (ti = 0; ti < num_vtmove; ti++)
				{
					VacPage	destvacpage = vtmove[ti].vacpage;

					/* Get page to move from */
					tuple.t_self = vtmove[ti].tid;
					Cbuf = ReadBuffer(onerel,
							 ItemPointerGetBlockNumber(&(tuple.t_self)));

					/* Get page to move to */
					cur_buffer = ReadBuffer(onerel, destvacpage->blkno);

					LockBuffer(cur_buffer, BUFFER_LOCK_EXCLUSIVE);
					if (cur_buffer != Cbuf)
						LockBuffer(Cbuf, BUFFER_LOCK_EXCLUSIVE);

					ToPage = BufferGetPage(cur_buffer);
					Cpage = BufferGetPage(Cbuf);

					/* NO ELOG(ERROR) TILL CHANGES ARE LOGGED */

					Citemid = PageGetItemId(Cpage,
							ItemPointerGetOffsetNumber(&(tuple.t_self)));
					tuple.t_datamcxt = NULL;
					tuple.t_data = (HeapTupleHeader) PageGetItem(Cpage, Citemid);
					tuple_len = tuple.t_len = ItemIdGetLength(Citemid);

					/*
					 * make a copy of the source tuple, and then mark the
					 * source tuple MOVED_OFF.
					 */
					heap_copytuple_with_tuple(&tuple, &newtup);

					RelationInvalidateHeapTuple(onerel, &tuple);

					TransactionIdStore(myXID, (TransactionId *) &(tuple.t_data->t_cmin));
					tuple.t_data->t_infomask &=
						~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | HEAP_MOVED_IN);
					tuple.t_data->t_infomask |= HEAP_MOVED_OFF;

					/*
					 * If this page was not used before - clean it.
					 *
					 * NOTE: a nasty bug used to lurk here.  It is possible
					 * for the source and destination pages to be the same
					 * (since this tuple-chain member can be on a page lower
					 * than the one we're currently processing in the outer
					 * loop).  If that's true, then after vacuum_page() the
					 * source tuple will have been moved, and tuple.t_data
					 * will be pointing at garbage.  Therefore we must do
					 * everything that uses tuple.t_data BEFORE this step!!
					 *
					 * This path is different from the other callers of
					 * vacuum_page, because we have already incremented the
					 * vacpage's offsets_used field to account for the
					 * tuple(s) we expect to move onto the page. Therefore
					 * vacuum_page's check for offsets_used == 0 is
					 * wrong. But since that's a good debugging check for
					 * all other callers, we work around it here rather
					 * than remove it.
					 */
					if (!PageIsEmpty(ToPage) && vtmove[ti].cleanVpd)
					{
						int			sv_offsets_used = destvacpage->offsets_used;

						destvacpage->offsets_used = 0;
						vacuum_page(ToPage, destvacpage);
						destvacpage->offsets_used = sv_offsets_used;
					}

					/*
					 * Update the state of the copied tuple, and store it
					 * on the destination page.
					 */
					TransactionIdStore(myXID, (TransactionId *) &(newtup.t_data->t_cmin));
					newtup.t_data->t_infomask &=
						~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | HEAP_MOVED_OFF);
					newtup.t_data->t_infomask |= HEAP_MOVED_IN;
					newoff = PageAddItem(ToPage, (Item) newtup.t_data, tuple_len,
										 InvalidOffsetNumber, LP_USED);
					if (newoff == InvalidOffsetNumber)
					{
						elog(STOP, "moving chain: failed to add item with len = %lu to page %u",
							 (unsigned long)tuple_len, destvacpage->blkno);
					}
					newitemid = PageGetItemId(ToPage, newoff);
					pfree(newtup.t_data);
					newtup.t_datamcxt = NULL;
					newtup.t_data = (HeapTupleHeader) PageGetItem(ToPage, newitemid);
					ItemPointerSet(&(newtup.t_self), destvacpage->blkno, newoff);

#ifdef XLOG
					{
						XLogRecPtr	recptr = 
							log_heap_move(onerel, tuple.t_self, &newtup);

						if (Cbuf != cur_buffer)
						{
							PageSetLSN(Cpage, recptr);
							PageSetSUI(Cpage, ThisStartUpID);
						}
						PageSetLSN(ToPage, recptr);
						PageSetSUI(ToPage, ThisStartUpID);
					}
#endif

					if (((int) destvacpage->blkno) > last_move_dest_block)
						last_move_dest_block = destvacpage->blkno;

					/*
					 * Set new tuple's t_ctid pointing to itself for last
					 * tuple in chain, and to next tuple in chain otherwise.
					 */
					if (!ItemPointerIsValid(&Ctid))
						newtup.t_data->t_ctid = newtup.t_self;
					else
						newtup.t_data->t_ctid = Ctid;
					Ctid = newtup.t_self;

					num_moved++;

					/*
					 * Remember that we moved tuple from the current page
					 * (corresponding index tuple will be cleaned).
					 */
					if (Cbuf == buf)
						vacpage->offsets[vacpage->offsets_free++] =
							ItemPointerGetOffsetNumber(&(tuple.t_self));
					else
						keep_tuples++;

					LockBuffer(cur_buffer, BUFFER_LOCK_UNLOCK);
					if (cur_buffer != Cbuf)
						LockBuffer(Cbuf, BUFFER_LOCK_UNLOCK);

					if (Irel != (Relation *) NULL)
					{
						/*
						 * XXX using CurrentMemoryContext here means
						 * intra-vacuum memory leak for functional indexes.
						 * Should fix someday.
						 *
						 * XXX This code fails to handle partial indexes!
						 * Probably should change it to use ExecOpenIndices.
						 */
						for (i = 0; i < nindices; i++)
						{
							FormIndexDatum(indexInfo[i],
										   &newtup,
										   tupdesc,
										   CurrentMemoryContext,
										   idatum,
										   inulls);
							iresult = index_insert(Irel[i],
												   idatum,
												   inulls,
												   &newtup.t_self,
												   onerel);
							if (iresult)
								pfree(iresult);
						}
					}
					WriteBuffer(cur_buffer);
					WriteBuffer(Cbuf);
				}
				cur_buffer = InvalidBuffer;
				pfree(vtmove);
				chain_tuple_moved = true;
				continue;
			}

			/* try to find new page for this tuple */
			if (cur_buffer == InvalidBuffer ||
				!enough_space(cur_page, tuple_len))
			{
				if (cur_buffer != InvalidBuffer)
				{
					WriteBuffer(cur_buffer);
					cur_buffer = InvalidBuffer;

					/*
					 * If previous target page is now too full to add *any*
					 * tuple to it, remove it from fraged_pages.
					 */
					if (!enough_space(cur_page, vacrelstats->min_tlen))
					{
						Assert(num_fraged_pages > cur_item);
						memmove(fraged_pages->pagedesc + cur_item,
								fraged_pages->pagedesc + cur_item + 1,
								sizeof(VacPage) * (num_fraged_pages - cur_item - 1));
						num_fraged_pages--;
					}
				}
				for (i = 0; i < num_fraged_pages; i++)
				{
					if (enough_space(fraged_pages->pagedesc[i], tuple_len))
						break;
				}
				if (i == num_fraged_pages)
					break;		/* can't move item anywhere */
				cur_item = i;
				cur_page = fraged_pages->pagedesc[cur_item];
				cur_buffer = ReadBuffer(onerel, cur_page->blkno);
				LockBuffer(cur_buffer, BUFFER_LOCK_EXCLUSIVE);
				ToPage = BufferGetPage(cur_buffer);
				/* if this page was not used before - clean it */
				if (!PageIsEmpty(ToPage) && cur_page->offsets_used == 0)
					vacuum_page(ToPage, cur_page);
			}
			else
				LockBuffer(cur_buffer, BUFFER_LOCK_EXCLUSIVE);

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			/* copy tuple */
			heap_copytuple_with_tuple(&tuple, &newtup);

			RelationInvalidateHeapTuple(onerel, &tuple);

			/*
			 * Mark new tuple as moved_in by vacuum and store vacuum XID
			 * in t_cmin !!!
			 */
			TransactionIdStore(myXID, (TransactionId *) &(newtup.t_data->t_cmin));
			newtup.t_data->t_infomask &=
				~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | HEAP_MOVED_OFF);
			newtup.t_data->t_infomask |= HEAP_MOVED_IN;

			/* add tuple to the page */
			newoff = PageAddItem(ToPage, (Item) newtup.t_data, tuple_len,
								 InvalidOffsetNumber, LP_USED);
			if (newoff == InvalidOffsetNumber)
			{
				elog(ERROR, "\
failed to add item with len = %lu to page %u (free space %lu, nusd %u, noff %u)",
					 (unsigned long)tuple_len, cur_page->blkno, (unsigned long)cur_page->free,
				 cur_page->offsets_used, cur_page->offsets_free);
			}
			newitemid = PageGetItemId(ToPage, newoff);
			pfree(newtup.t_data);
			newtup.t_datamcxt = NULL;
			newtup.t_data = (HeapTupleHeader) PageGetItem(ToPage, newitemid);
			ItemPointerSet(&(newtup.t_data->t_ctid), cur_page->blkno, newoff);
			newtup.t_self = newtup.t_data->t_ctid;

			/*
			 * Mark old tuple as moved_off by vacuum and store vacuum XID
			 * in t_cmin !!!
			 */
			TransactionIdStore(myXID, (TransactionId *) &(tuple.t_data->t_cmin));
			tuple.t_data->t_infomask &=
				~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | HEAP_MOVED_IN);
			tuple.t_data->t_infomask |= HEAP_MOVED_OFF;

#ifdef XLOG
			{
				XLogRecPtr	recptr = 
					log_heap_move(onerel, tuple.t_self, &newtup);

				PageSetLSN(page, recptr);
				PageSetSUI(page, ThisStartUpID);
				PageSetLSN(ToPage, recptr);
				PageSetSUI(ToPage, ThisStartUpID);
			}
#endif

			cur_page->offsets_used++;
			num_moved++;
			cur_page->free = ((PageHeader) ToPage)->pd_upper - ((PageHeader) ToPage)->pd_lower;
			if (((int) cur_page->blkno) > last_move_dest_block)
				last_move_dest_block = cur_page->blkno;

			vacpage->offsets[vacpage->offsets_free++] = offnum;

			LockBuffer(cur_buffer, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			/* insert index' tuples if needed */
			if (Irel != (Relation *) NULL)
			{
				/*
				 * XXX using CurrentMemoryContext here means
				 * intra-vacuum memory leak for functional indexes.
				 * Should fix someday.
				 *
				 * XXX This code fails to handle partial indexes!
				 * Probably should change it to use ExecOpenIndices.
				 */
				for (i = 0; i < nindices; i++)
				{
					FormIndexDatum(indexInfo[i],
								   &newtup,
								   tupdesc,
								   CurrentMemoryContext,
								   idatum,
								   inulls);
					iresult = index_insert(Irel[i],
										   idatum,
										   inulls,
										   &newtup.t_self,
										   onerel);
					if (iresult)
						pfree(iresult);
				}
			}

		}						/* walk along page */

		if (offnum < maxoff && keep_tuples > 0)
		{
			OffsetNumber off;

			for (off = OffsetNumberNext(offnum);
				 off <= maxoff;
				 off = OffsetNumberNext(off))
			{
				itemid = PageGetItemId(page, off);
				if (!ItemIdIsUsed(itemid))
					continue;
				tuple.t_datamcxt = NULL;
				tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
				if (tuple.t_data->t_infomask & HEAP_XMIN_COMMITTED)
					continue;
				if ((TransactionId) tuple.t_data->t_cmin != myXID)
					elog(ERROR, "Invalid XID in t_cmin (4)");
				if (tuple.t_data->t_infomask & HEAP_MOVED_IN)
					elog(ERROR, "HEAP_MOVED_IN was not expected (2)");
				if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
				{
					/* some chains was moved while */
					if (chain_tuple_moved)
					{			/* cleaning this page */
						Assert(vacpage->offsets_free > 0);
						for (i = 0; i < vacpage->offsets_free; i++)
						{
							if (vacpage->offsets[i] == off)
								break;
						}
						if (i >= vacpage->offsets_free) /* not found */
						{
							vacpage->offsets[vacpage->offsets_free++] = off;
							Assert(keep_tuples > 0);
							keep_tuples--;
						}
					}
					else
					{
						vacpage->offsets[vacpage->offsets_free++] = off;
						Assert(keep_tuples > 0);
						keep_tuples--;
					}
				}
			}
		}

		if (vacpage->offsets_free > 0)	/* some tuples were moved */
		{
			if (chain_tuple_moved)		/* else - they are ordered */
			{
				qsort((char *) (vacpage->offsets), vacpage->offsets_free,
					  sizeof(OffsetNumber), vac_cmp_offno);
			}
			reap_page(&Nvacpagelist, vacpage);
			WriteBuffer(buf);
		}
		else if (dowrite)
			WriteBuffer(buf);
		else
			ReleaseBuffer(buf);

		if (offnum <= maxoff)
			break;				/* some item(s) left */

	}							/* walk along relation */

	blkno++;					/* new number of blocks */

	if (cur_buffer != InvalidBuffer)
	{
		Assert(num_moved > 0);
		WriteBuffer(cur_buffer);
	}

	if (num_moved > 0)
	{
#ifdef XLOG
		RecordTransactionCommit();
#else
		/*
		 * We have to commit our tuple' movings before we'll truncate
		 * relation, but we shouldn't lose our locks. And so - quick hack:
		 * flush buffers and record status of current transaction as
		 * committed, and continue. - vadim 11/13/96
		 */
		FlushBufferPool();
		TransactionIdCommit(myXID);
		FlushBufferPool();
#endif
	}

	/*
	 * Clean uncleaned reaped pages from vacuum_pages list list and set
	 * xmin committed for inserted tuples
	 */
	checked_moved = 0;
	for (i = 0, curpage = vacuum_pages->pagedesc; i < vacuumed_pages; i++, curpage++)
	{
		Assert((*curpage)->blkno < (BlockNumber) blkno);
		buf = ReadBuffer(onerel, (*curpage)->blkno);
		page = BufferGetPage(buf);
		if ((*curpage)->offsets_used == 0)		/* this page was not used */
		{
			if (!PageIsEmpty(page))
				vacuum_page(page, *curpage);
		}
		else
/* this page was used */
		{
			num_tuples = 0;
			max_offset = PageGetMaxOffsetNumber(page);
			for (newoff = FirstOffsetNumber;
				 newoff <= max_offset;
				 newoff = OffsetNumberNext(newoff))
			{
				itemid = PageGetItemId(page, newoff);
				if (!ItemIdIsUsed(itemid))
					continue;
				tuple.t_datamcxt = NULL;
				tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);
				if (!(tuple.t_data->t_infomask & HEAP_XMIN_COMMITTED))
				{
					if ((TransactionId) tuple.t_data->t_cmin != myXID)
						elog(ERROR, "Invalid XID in t_cmin (2)");
					if (tuple.t_data->t_infomask & HEAP_MOVED_IN)
					{
						tuple.t_data->t_infomask |= HEAP_XMIN_COMMITTED;
						num_tuples++;
					}
					else if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
						tuple.t_data->t_infomask |= HEAP_XMIN_INVALID;
					else
						elog(ERROR, "HEAP_MOVED_OFF/HEAP_MOVED_IN was expected");
				}
			}
			Assert((*curpage)->offsets_used == num_tuples);
			checked_moved += num_tuples;
		}
		WriteBuffer(buf);
	}
	Assert(num_moved == checked_moved);

	elog(MESSAGE_LEVEL, "Rel %s: Pages: %u --> %u; Tuple(s) moved: %u. %s",
		 RelationGetRelationName(onerel),
		 nblocks, blkno, num_moved,
		 show_rusage(&ru0));

	if (Nvacpagelist.num_pages > 0)
	{
		/* vacuum indices again if needed */
		if (Irel != (Relation *) NULL)
		{
			VacPage	   *vpleft,
					   *vpright,
						vpsave;

			/* re-sort Nvacpagelist.pagedesc */
			for (vpleft = Nvacpagelist.pagedesc,
				 vpright = Nvacpagelist.pagedesc + Nvacpagelist.num_pages - 1;
				 vpleft < vpright; vpleft++, vpright--)
			{
				vpsave = *vpleft;
				*vpleft = *vpright;
				*vpright = vpsave;
			}
			Assert(keep_tuples >= 0);
			for (i = 0; i < nindices; i++)
				vacuum_index(&Nvacpagelist, Irel[i],
							 vacrelstats->num_tuples, keep_tuples);
		}

		/* clean moved tuples from last page in Nvacpagelist list */
		if (vacpage->blkno == (BlockNumber) (blkno - 1) &&
			vacpage->offsets_free > 0)
		{
			buf = ReadBuffer(onerel, vacpage->blkno);
			page = BufferGetPage(buf);
			num_tuples = 0;
			for (offnum = FirstOffsetNumber;
				 offnum <= maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				if (!ItemIdIsUsed(itemid))
					continue;
				tuple.t_datamcxt = NULL;
				tuple.t_data = (HeapTupleHeader) PageGetItem(page, itemid);

				if (!(tuple.t_data->t_infomask & HEAP_XMIN_COMMITTED))
				{
					if ((TransactionId) tuple.t_data->t_cmin != myXID)
						elog(ERROR, "Invalid XID in t_cmin (3)");
					if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
					{
						itemid->lp_flags &= ~LP_USED;
						num_tuples++;
					}
					else
						elog(ERROR, "HEAP_MOVED_OFF was expected (2)");
				}

			}
			Assert(vacpage->offsets_free == num_tuples);
			PageRepairFragmentation(page);
			WriteBuffer(buf);
		}

		/* now - free new list of reaped pages */
		curpage = Nvacpagelist.pagedesc;
		for (i = 0; i < Nvacpagelist.num_pages; i++, curpage++)
			pfree(*curpage);
		pfree(Nvacpagelist.pagedesc);
	}

	/*
	 * Flush dirty pages out to disk.  We do this unconditionally, even if
	 * we don't need to truncate, because we want to ensure that all tuples
	 * have correct on-row commit status on disk (see bufmgr.c's comments
	 * for FlushRelationBuffers()).
	 */
	i = FlushRelationBuffers(onerel, blkno);
	if (i < 0)
		elog(ERROR, "VACUUM (repair_frag): FlushRelationBuffers returned %d",
			 i);

	/* truncate relation, if needed */
	if (blkno < nblocks)
	{
		blkno = smgrtruncate(DEFAULT_SMGR, onerel, blkno);
		Assert(blkno >= 0);
		vacrelstats->num_pages = blkno; /* set new number of blocks */
	}

	if (Irel != (Relation *) NULL)		/* pfree index' allocations */
	{
		close_indices(nindices, Irel);
		pfree(indexInfo);
	}

	pfree(vacpage);
	if (vacrelstats->vtlinks != NULL)
		pfree(vacrelstats->vtlinks);
}

/*
 *	vacuum_heap() -- free dead tuples
 *
 *		This routine marks dead tuples as unused and truncates relation
 *		if there are "empty" end-blocks.
 */
static void
vacuum_heap(VRelStats *vacrelstats, Relation onerel, VacPageList vacuum_pages)
{
	Buffer		buf;
	Page		page;
	VacPage    *vacpage;
	int			nblocks;
	int			i;

	nblocks = vacuum_pages->num_pages;
	nblocks -= vacuum_pages->empty_end_pages;		/* nothing to do with
													 * them */

	for (i = 0, vacpage = vacuum_pages->pagedesc; i < nblocks; i++, vacpage++)
	{
		if ((*vacpage)->offsets_free > 0)
		{
			buf = ReadBuffer(onerel, (*vacpage)->blkno);
			page = BufferGetPage(buf);
			vacuum_page(page, *vacpage);
			WriteBuffer(buf);
		}
	}

	/*
	 * Flush dirty pages out to disk.  We do this unconditionally, even if
	 * we don't need to truncate, because we want to ensure that all tuples
	 * have correct on-row commit status on disk (see bufmgr.c's comments
	 * for FlushRelationBuffers()).
	 */
	Assert(vacrelstats->num_pages >= vacuum_pages->empty_end_pages);
	nblocks = vacrelstats->num_pages - vacuum_pages->empty_end_pages;

	i = FlushRelationBuffers(onerel, nblocks);
	if (i < 0)
		elog(ERROR, "VACUUM (vacuum_heap): FlushRelationBuffers returned %d",
			 i);

	/* truncate relation if there are some empty end-pages */
	if (vacuum_pages->empty_end_pages > 0)
	{
		elog(MESSAGE_LEVEL, "Rel %s: Pages: %u --> %u.",
			 RelationGetRelationName(onerel),
			 vacrelstats->num_pages, nblocks);
		nblocks = smgrtruncate(DEFAULT_SMGR, onerel, nblocks);
		Assert(nblocks >= 0);
		vacrelstats->num_pages = nblocks; /* set new number of blocks */
	}
}

/*
 *	vacuum_page() -- free dead tuples on a page
 *					 and repair its fragmentation.
 */
static void
vacuum_page(Page page, VacPage vacpage)
{
	ItemId		itemid;
	int			i;

	/* There shouldn't be any tuples moved onto the page yet! */
	Assert(vacpage->offsets_used == 0);

	for (i = 0; i < vacpage->offsets_free; i++)
	{
		itemid = &(((PageHeader) page)->pd_linp[vacpage->offsets[i] - 1]);
		itemid->lp_flags &= ~LP_USED;
	}
	PageRepairFragmentation(page);

}

/*
 *	_scan_index() -- scan one index relation to update statistic.
 *
 */
static void
scan_index(Relation indrel, int num_tuples)
{
	RetrieveIndexResult res;
	IndexScanDesc iscan;
	int			nitups;
	int			nipages;
	struct rusage ru0;

	getrusage(RUSAGE_SELF, &ru0);

	/* walk through the entire index */
	iscan = index_beginscan(indrel, false, 0, (ScanKey) NULL);
	nitups = 0;

	while ((res = index_getnext(iscan, ForwardScanDirection))
		   != (RetrieveIndexResult) NULL)
	{
		nitups++;
		pfree(res);
	}

	index_endscan(iscan);

	/* now update statistics in pg_class */
	nipages = RelationGetNumberOfBlocks(indrel);
	update_relstats(RelationGetRelid(indrel), nipages, nitups, false, NULL);

	elog(MESSAGE_LEVEL, "Index %s: Pages %u; Tuples %u. %s",
		 RelationGetRelationName(indrel), nipages, nitups,
		 show_rusage(&ru0));

	if (nitups != num_tuples)
		elog(NOTICE, "Index %s: NUMBER OF INDEX' TUPLES (%u) IS NOT THE SAME AS HEAP' (%u).\
\n\tRecreate the index.",
			 RelationGetRelationName(indrel), nitups, num_tuples);

}

/*
 *	vacuum_index() -- vacuum one index relation.
 *
 *		Vpl is the VacPageList of the heap we're currently vacuuming.
 *		It's locked. Indrel is an index relation on the vacuumed heap.
 *		We don't set locks on the index	relation here, since the indexed
 *		access methods support locking at different granularities.
 *		We let them handle it.
 *
 *		Finally, we arrange to update the index relation's statistics in
 *		pg_class.
 */
static void
vacuum_index(VacPageList vacpagelist, Relation indrel, int num_tuples, int keep_tuples)
{
	RetrieveIndexResult res;
	IndexScanDesc iscan;
	ItemPointer heapptr;
	int			tups_vacuumed;
	int			num_index_tuples;
	int			num_pages;
	VacPage		vp;
	struct rusage ru0;

	getrusage(RUSAGE_SELF, &ru0);

	/* walk through the entire index */
	iscan = index_beginscan(indrel, false, 0, (ScanKey) NULL);
	tups_vacuumed = 0;
	num_index_tuples = 0;

	while ((res = index_getnext(iscan, ForwardScanDirection))
		   != (RetrieveIndexResult) NULL)
	{
		heapptr = &res->heap_iptr;

		if ((vp = tid_reaped(heapptr, vacpagelist)) != (VacPage) NULL)
		{
#ifdef NOT_USED
			elog(DEBUG, "<%x,%x> -> <%x,%x>",
				 ItemPointerGetBlockNumber(&(res->index_iptr)),
				 ItemPointerGetOffsetNumber(&(res->index_iptr)),
				 ItemPointerGetBlockNumber(&(res->heap_iptr)),
				 ItemPointerGetOffsetNumber(&(res->heap_iptr)));
#endif
			if (vp->offsets_free == 0)
			{
				elog(NOTICE, "Index %s: pointer to EmptyPage (blk %u off %u) - fixing",
					 RelationGetRelationName(indrel),
					 vp->blkno, ItemPointerGetOffsetNumber(heapptr));
			}
			++tups_vacuumed;
			index_delete(indrel, &res->index_iptr);
		}
		else
			num_index_tuples++;

		pfree(res);
	}

	index_endscan(iscan);

	/* now update statistics in pg_class */
	num_pages = RelationGetNumberOfBlocks(indrel);
	update_relstats(RelationGetRelid(indrel), num_pages, num_index_tuples, false, NULL);

	elog(MESSAGE_LEVEL, "Index %s: Pages %u; Tuples %u: Deleted %u. %s",
		 RelationGetRelationName(indrel), num_pages,
		 num_index_tuples - keep_tuples, tups_vacuumed,
		 show_rusage(&ru0));

	if (num_index_tuples != num_tuples + keep_tuples)
		elog(NOTICE, "Index %s: NUMBER OF INDEX' TUPLES (%u) IS NOT THE SAME AS HEAP' (%u).\
\n\tRecreate the index.",
		  RelationGetRelationName(indrel), num_index_tuples, num_tuples);

}

/*
 *	tid_reaped() -- is a particular tid reaped?
 *
 *		vacpagelist->VacPage_array is sorted in right order.
 */
static VacPage
tid_reaped(ItemPointer itemptr, VacPageList vacpagelist)
{
	OffsetNumber ioffno;
	OffsetNumber *voff;
	VacPage		vp,
			   *vpp;
	VacPageData vacpage;

	vacpage.blkno = ItemPointerGetBlockNumber(itemptr);
	ioffno = ItemPointerGetOffsetNumber(itemptr);

	vp = &vacpage;
	vpp = (VacPage *) vac_find_eq((void *) (vacpagelist->pagedesc),
					vacpagelist->num_pages, sizeof(VacPage), (void *) &vp,
									vac_cmp_blk);

	if (vpp == (VacPage *) NULL)
		return (VacPage) NULL;
	vp = *vpp;

	/* ok - we are on true page */

	if (vp->offsets_free == 0)
	{							/* this is EmptyPage !!! */
		return vp;
	}

	voff = (OffsetNumber *) vac_find_eq((void *) (vp->offsets),
			vp->offsets_free, sizeof(OffsetNumber), (void *) &ioffno,
									   vac_cmp_offno);

	if (voff == (OffsetNumber *) NULL)
		return (VacPage) NULL;

	return vp;

}

/*
 *	update_relstats() -- update statistics for one relation
 *
 *		Statistics are stored in several places: the pg_class row for the
 *		relation has stats about the whole relation, the pg_attribute rows
 *		for each attribute store "dispersion", and there is a pg_statistic
 *		row for each (non-system) attribute.  (Dispersion probably ought to
 *		be moved to pg_statistic, but it's not worth doing unless there's
 *		another reason to have to change pg_attribute.)  Dispersion and
 *		pg_statistic values are only updated by VACUUM ANALYZE, but we
 *		always update the stats in pg_class.
 *
 *		This routine works for both index and heap relation entries in
 *		pg_class.  We violate no-overwrite semantics here by storing new
 *		values for the statistics columns directly into the pg_class
 *		tuple that's already on the page.  The reason for this is that if
 *		we updated these tuples in the usual way, vacuuming pg_class itself
 *		wouldn't work very well --- by the time we got done with a vacuum
 *		cycle, most of the tuples in pg_class would've been obsoleted.
 *		Updating pg_class's own statistics would be especially tricky.
 *		Of course, this only works for fixed-size never-null columns, but
 *		these are.
 */
static void
update_relstats(Oid relid, int num_pages, int num_tuples, bool hasindex,
			VRelStats *vacrelstats)
{
	Relation	rd;
	HeapTupleData rtup;
	HeapTuple	ctup;
	Form_pg_class pgcform;
	Buffer		buffer;

	/*
	 * update number of tuples and number of pages in pg_class
	 */
	rd = heap_openr(RelationRelationName, RowExclusiveLock);

	ctup = SearchSysCache(RELOID,
						  ObjectIdGetDatum(relid),
						  0, 0, 0);
	if (!HeapTupleIsValid(ctup))
		elog(ERROR, "pg_class entry for relid %u vanished during vacuuming",
			 relid);

	/* get the buffer cache tuple */
	rtup.t_self = ctup->t_self;
	ReleaseSysCache(ctup);
	heap_fetch(rd, SnapshotNow, &rtup, &buffer);

	/* overwrite the existing statistics in the tuple */
	pgcform = (Form_pg_class) GETSTRUCT(&rtup);
	pgcform->reltuples = num_tuples;
	pgcform->relpages = num_pages;
	pgcform->relhasindex = hasindex;

	/* invalidate the tuple in the cache and write the buffer */
	RelationInvalidateHeapTuple(rd, &rtup);
	WriteBuffer(buffer);

	heap_close(rd, RowExclusiveLock);
}

/*
 *	reap_page() -- save a page on the array of reaped pages.
 *
 *		As a side effect of the way that the vacuuming loop for a given
 *		relation works, higher pages come after lower pages in the array
 *		(and highest tid on a page is last).
 */
static void
reap_page(VacPageList vacpagelist, VacPage vacpage)
{
	VacPage	newvacpage;

	/* allocate a VacPageData entry */
	newvacpage = (VacPage) palloc(sizeof(VacPageData) + vacpage->offsets_free * sizeof(OffsetNumber));

	/* fill it in */
	if (vacpage->offsets_free > 0)
		memmove(newvacpage->offsets, vacpage->offsets, vacpage->offsets_free * sizeof(OffsetNumber));
	newvacpage->blkno = vacpage->blkno;
	newvacpage->free = vacpage->free;
	newvacpage->offsets_used = vacpage->offsets_used;
	newvacpage->offsets_free = vacpage->offsets_free;

	/* insert this page into vacpagelist list */
	vpage_insert(vacpagelist, newvacpage);

}

static void
vpage_insert(VacPageList vacpagelist, VacPage vpnew)
{
#define PG_NPAGEDESC 1024

	/* allocate a VacPage entry if needed */
	if (vacpagelist->num_pages == 0)
	{
		vacpagelist->pagedesc = (VacPage *) palloc(PG_NPAGEDESC * sizeof(VacPage));
		vacpagelist->num_allocated_pages = PG_NPAGEDESC;
	}
	else if (vacpagelist->num_pages >= vacpagelist->num_allocated_pages)
	{
		vacpagelist->num_allocated_pages *= 2;
		vacpagelist->pagedesc = (VacPage *) repalloc(vacpagelist->pagedesc, vacpagelist->num_allocated_pages * sizeof(VacPage));
	}
	vacpagelist->pagedesc[vacpagelist->num_pages] = vpnew;
	(vacpagelist->num_pages)++;

}

static void *
vac_find_eq(void *bot, int nelem, int size, void *elm,
		   int (*compar) (const void *, const void *))
{
	int			res;
	int			last = nelem - 1;
	int			celm = nelem / 2;
	bool		last_move,
				first_move;

	last_move = first_move = true;
	for (;;)
	{
		if (first_move == true)
		{
			res = compar(bot, elm);
			if (res > 0)
				return NULL;
			if (res == 0)
				return bot;
			first_move = false;
		}
		if (last_move == true)
		{
			res = compar(elm, (void *) ((char *) bot + last * size));
			if (res > 0)
				return NULL;
			if (res == 0)
				return (void *) ((char *) bot + last * size);
			last_move = false;
		}
		res = compar(elm, (void *) ((char *) bot + celm * size));
		if (res == 0)
			return (void *) ((char *) bot + celm * size);
		if (res < 0)
		{
			if (celm == 0)
				return NULL;
			last = celm - 1;
			celm = celm / 2;
			last_move = true;
			continue;
		}

		if (celm == last)
			return NULL;

		last = last - celm - 1;
		bot = (void *) ((char *) bot + (celm + 1) * size);
		celm = (last + 1) / 2;
		first_move = true;
	}

}

static int
vac_cmp_blk(const void *left, const void *right)
{
	BlockNumber lblk,
				rblk;

	lblk = (*((VacPage *) left))->blkno;
	rblk = (*((VacPage *) right))->blkno;

	if (lblk < rblk)
		return -1;
	if (lblk == rblk)
		return 0;
	return 1;

}

static int
vac_cmp_offno(const void *left, const void *right)
{

	if (*(OffsetNumber *) left < *(OffsetNumber *) right)
		return -1;
	if (*(OffsetNumber *) left == *(OffsetNumber *) right)
		return 0;
	return 1;

}

static int
vac_cmp_vtlinks(const void *left, const void *right)
{

	if (((VTupleLink) left)->new_tid.ip_blkid.bi_hi <
		((VTupleLink) right)->new_tid.ip_blkid.bi_hi)
		return -1;
	if (((VTupleLink) left)->new_tid.ip_blkid.bi_hi >
		((VTupleLink) right)->new_tid.ip_blkid.bi_hi)
		return 1;
	/* bi_hi-es are equal */
	if (((VTupleLink) left)->new_tid.ip_blkid.bi_lo <
		((VTupleLink) right)->new_tid.ip_blkid.bi_lo)
		return -1;
	if (((VTupleLink) left)->new_tid.ip_blkid.bi_lo >
		((VTupleLink) right)->new_tid.ip_blkid.bi_lo)
		return 1;
	/* bi_lo-es are equal */
	if (((VTupleLink) left)->new_tid.ip_posid <
		((VTupleLink) right)->new_tid.ip_posid)
		return -1;
	if (((VTupleLink) left)->new_tid.ip_posid >
		((VTupleLink) right)->new_tid.ip_posid)
		return 1;
	return 0;

}


static void
get_indices(Relation relation, int *nindices, Relation **Irel)
{
	List	   *indexoidlist,
			   *indexoidscan;
	int			i;

	indexoidlist = RelationGetIndexList(relation);

	*nindices = length(indexoidlist);

	if (*nindices > 0)
		*Irel = (Relation *) palloc(*nindices * sizeof(Relation));
	else
		*Irel = NULL;

	i = 0;
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirsti(indexoidscan);

		(*Irel)[i] = index_open(indexoid);
		i++;
	}

	freeList(indexoidlist);
}


static void
close_indices(int nindices, Relation *Irel)
{

	if (Irel == (Relation *) NULL)
		return;

	while (nindices--)
		index_close(Irel[nindices]);
	pfree(Irel);

}


/*
 * Obtain IndexInfo data for each index on the rel
 */
static IndexInfo **
get_index_desc(Relation onerel, int nindices, Relation *Irel)
{
	IndexInfo **indexInfo;
	int			i;
	HeapTuple	cachetuple;

	indexInfo = (IndexInfo **) palloc(nindices * sizeof(IndexInfo *));

	for (i = 0; i < nindices; i++)
	{
		cachetuple = SearchSysCache(INDEXRELID,
							 ObjectIdGetDatum(RelationGetRelid(Irel[i])),
									0, 0, 0);
		if (!HeapTupleIsValid(cachetuple))
			elog(ERROR, "get_index_desc: index %u not found",
				 RelationGetRelid(Irel[i]));
		indexInfo[i] = BuildIndexInfo(cachetuple);
		ReleaseSysCache(cachetuple);
	}

	return indexInfo;
}


static bool
enough_space(VacPage vacpage, Size len)
{

	len = MAXALIGN(len);

	if (len > vacpage->free)
		return false;

	if (vacpage->offsets_used < vacpage->offsets_free)	/* there are free
														 * itemid(s) */
		return true;			/* and len <= free_space */

	/* ok. noff_usd >= noff_free and so we'll have to allocate new itemid */
	if (len + MAXALIGN(sizeof(ItemIdData)) <= vacpage->free)
		return true;

	return false;

}


/*
 * Compute elapsed time since ru0 usage snapshot, and format into
 * a displayable string.  Result is in a static string, which is
 * tacky, but no one ever claimed that the Postgres backend is
 * threadable...
 */
static char *
show_rusage(struct rusage * ru0)
{
	static char result[64];
	struct rusage ru1;

	getrusage(RUSAGE_SELF, &ru1);

	if (ru1.ru_stime.tv_usec < ru0->ru_stime.tv_usec)
	{
		ru1.ru_stime.tv_sec--;
		ru1.ru_stime.tv_usec += 1000000;
	}
	if (ru1.ru_utime.tv_usec < ru0->ru_utime.tv_usec)
	{
		ru1.ru_utime.tv_sec--;
		ru1.ru_utime.tv_usec += 1000000;
	}

	snprintf(result, sizeof(result),
			 "CPU %d.%02ds/%d.%02du sec.",
			 (int) (ru1.ru_stime.tv_sec - ru0->ru_stime.tv_sec),
			 (int) (ru1.ru_stime.tv_usec - ru0->ru_stime.tv_usec) / 10000,
			 (int) (ru1.ru_utime.tv_sec - ru0->ru_utime.tv_sec),
		   (int) (ru1.ru_utime.tv_usec - ru0->ru_utime.tv_usec) / 10000);

	return result;
}
