/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  the postgres vacuum cleaner
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/vacuum.c,v 1.134 2000/01/10 04:09:50 tgl Exp $
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
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "parser/parse_oper.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/portal.h"
#include "utils/relcache.h"
#include "utils/syscache.h"

#ifndef HAVE_GETRUSAGE
#include "rusagestub.h"
#else
#include <sys/time.h>
#include <sys/resource.h>
#endif


bool		VacuumRunning = false;

static Portal vc_portal;

static int	MESSAGE_LEVEL;		/* message level */

static TransactionId XmaxRecent;

#define swapLong(a,b)	{long tmp; tmp=a; a=b; b=tmp;}
#define swapInt(a,b)	{int tmp; tmp=a; a=b; b=tmp;}
#define swapDatum(a,b)	{Datum tmp; tmp=a; a=b; b=tmp;}
#define VacAttrStatsEqValid(stats) ( stats->f_cmpeq.fn_addr != NULL )
#define VacAttrStatsLtGtValid(stats) ( stats->f_cmplt.fn_addr != NULL && \
								   stats->f_cmpgt.fn_addr != NULL && \
								   RegProcedureIsValid(stats->outfunc) )


/* non-export function prototypes */
static void vc_init(void);
static void vc_shutdown(void);
static void vc_vacuum(NameData *VacRelP, bool analyze, List *va_cols);
static VRelList vc_getrels(NameData *VacRelP);
static void vc_vacone(Oid relid, bool analyze, List *va_cols);
static void vc_scanheap(VRelStats *vacrelstats, Relation onerel, VPageList vacuum_pages, VPageList fraged_pages);
static void vc_rpfheap(VRelStats *vacrelstats, Relation onerel, VPageList vacuum_pages, VPageList fraged_pages, int nindices, Relation *Irel);
static void vc_vacheap(VRelStats *vacrelstats, Relation onerel, VPageList vpl);
static void vc_vacpage(Page page, VPageDescr vpd);
static void vc_vaconeind(VPageList vpl, Relation indrel, int num_tuples, int keep_tuples);
static void vc_scanoneind(Relation indrel, int num_tuples);
static void vc_attrstats(Relation onerel, VRelStats *vacrelstats, HeapTuple tuple);
static void vc_bucketcpy(Form_pg_attribute attr, Datum value, Datum *bucket, int *bucket_len);
static void vc_updstats(Oid relid, int num_pages, int num_tuples, bool hasindex, VRelStats *vacrelstats);
static void vc_delstats(Oid relid, int attcnt, int *attnums);
static VPageDescr vc_tidreapped(ItemPointer itemptr, VPageList vpl);
static void vc_reappage(VPageList vpl, VPageDescr vpc);
static void vc_vpinsert(VPageList vpl, VPageDescr vpnew);
static void vc_getindices(Oid relid, int *nindices, Relation **Irel);
static void vc_clsindices(int nindices, Relation *Irel);
static void vc_mkindesc(Relation onerel, int nindices, Relation *Irel, IndDesc **Idesc);
static void *vc_find_eq(void *bot, int nelem, int size, void *elm,
		   int (*compar) (const void *, const void *));
static int	vc_cmp_blk(const void *left, const void *right);
static int	vc_cmp_offno(const void *left, const void *right);
static int	vc_cmp_vtlinks(const void *left, const void *right);
static bool vc_enough_space(VPageDescr vpd, Size len);
static char *vc_show_rusage(struct rusage *ru0);


void
vacuum(char *vacrel, bool verbose, bool analyze, List *va_spec)
{
	NameData	VacRel;
	PortalVariableMemory pmem;
	MemoryContext old;
	List	   *le;
	List	   *va_cols = NIL;

	if (va_spec != NIL && !analyze)
		elog(ERROR, "Can't vacuum columns, only tables.  You can 'vacuum analyze' columns.");

	/*
	 * We cannot run VACUUM inside a user transaction block; if we were
	 * inside a transaction, then our commit- and start-transaction-command
	 * calls would not have the intended effect!  Furthermore, the forced
	 * commit that occurs before truncating the relation's file would have
	 * the effect of committing the rest of the user's transaction too,
	 * which would certainly not be the desired behavior.
	 */
	if (IsTransactionBlock())
		elog(ERROR, "VACUUM cannot run inside a BEGIN/END block");

	/* initialize vacuum cleaner, particularly vc_portal */
	vc_init();

	if (verbose)
		MESSAGE_LEVEL = NOTICE;
	else
		MESSAGE_LEVEL = DEBUG;

	/* vacrel gets de-allocated on transaction commit, so copy it */
	if (vacrel)
		strcpy(NameStr(VacRel), vacrel);

	/* must also copy the column list, if any, to safe storage */
	pmem = PortalGetVariableMemory(vc_portal);
	old = MemoryContextSwitchTo((MemoryContext) pmem);
	foreach(le, va_spec)
	{
		char	   *col = (char *) lfirst(le);

		va_cols = lappend(va_cols, pstrdup(col));
	}
	MemoryContextSwitchTo(old);

	/* vacuum the database */
	if (vacrel)
		vc_vacuum(&VacRel, analyze, va_cols);
	else
		vc_vacuum(NULL, analyze, NIL);

	/* clean up */
	vc_shutdown();
}

/*
 *	vc_init(), vc_shutdown() -- start up and shut down the vacuum cleaner.
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
 *		started in PostgresMain() inside vc_init(), and start one in
 *		vc_shutdown() to match the commit waiting for us back in
 *		PostgresMain().
 */
static void
vc_init()
{
	char	   *pname;

	/*
	 * Create a portal for safe memory across transactions. We need to
	 * palloc the name space for it because our hash function expects the
	 * name to be on a longword boundary.  CreatePortal copies the name to
	 * safe storage for us.
	 */
	pname = pstrdup(VACPNAME);
	vc_portal = CreatePortal(pname);
	pfree(pname);

	/*
	 * Set flag to indicate that vc_portal must be removed after an error.
	 * This global variable is checked in the transaction manager on xact
	 * abort, and the routine vc_abort() is called if necessary.
	 */
	VacuumRunning = true;

	/* matches the StartTransaction in PostgresMain() */
	CommitTransactionCommand();
}

static void
vc_shutdown()
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

	/*
	 * Release our portal for cross-transaction memory.
	 */
	PortalDrop(&vc_portal);

	/* okay, we're done */
	VacuumRunning = false;

	/* matches the CommitTransaction in PostgresMain() */
	StartTransactionCommand();
}

void
vc_abort()
{
	/* Clear flag first, to avoid recursion if PortalDrop elog's */
	VacuumRunning = false;

	/*
	 * Release our portal for cross-transaction memory.
	 */
	PortalDrop(&vc_portal);
}

/*
 *	vc_vacuum() -- vacuum the database.
 *
 *		This routine builds a list of relations to vacuum, and then calls
 *		code that vacuums them one at a time.  We are careful to vacuum each
 *		relation in a separate transaction in order to avoid holding too many
 *		locks at one time.
 */
static void
vc_vacuum(NameData *VacRelP, bool analyze, List *va_cols)
{
	VRelList	vrl,
				cur;

	/* get list of relations */
	vrl = vc_getrels(VacRelP);

	/* vacuum each heap relation */
	for (cur = vrl; cur != (VRelList) NULL; cur = cur->vrl_next)
		vc_vacone(cur->vrl_relid, analyze, va_cols);
}

static VRelList
vc_getrels(NameData *VacRelP)
{
	Relation	rel;
	TupleDesc	tupdesc;
	HeapScanDesc scan;
	HeapTuple	tuple;
	PortalVariableMemory portalmem;
	MemoryContext old;
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
		ScanKeyEntryInitialize(&key, 0x0, Anum_pg_class_relname,
							   F_NAMEEQ,
							   PointerGetDatum(NameStr(*VacRelP)));
	}
	else
	{
		ScanKeyEntryInitialize(&key, 0x0, Anum_pg_class_relkind,
							   F_CHAREQ, CharGetDatum('r'));
	}

	portalmem = PortalGetVariableMemory(vc_portal);
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
			elog(NOTICE, "Vacuum: can not process index and certain system tables");
			continue;
		}

		/* get a relation list entry for this guy */
		old = MemoryContextSwitchTo((MemoryContext) portalmem);
		if (vrl == (VRelList) NULL)
			vrl = cur = (VRelList) palloc(sizeof(VRelListData));
		else
		{
			cur->vrl_next = (VRelList) palloc(sizeof(VRelListData));
			cur = cur->vrl_next;
		}
		MemoryContextSwitchTo(old);

		cur->vrl_relid = tuple->t_data->t_oid;
		cur->vrl_next = (VRelList) NULL;
	}
	if (found == false)
		elog(NOTICE, "Vacuum: table not found");

	heap_endscan(scan);
	heap_close(rel, AccessShareLock);

	CommitTransactionCommand();

	return vrl;
}

/*
 *	vc_vacone() -- vacuum one heap relation
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
vc_vacone(Oid relid, bool analyze, List *va_cols)
{
	HeapTuple	tuple,
				typetuple;
	Relation	onerel;
	VPageListData vacuum_pages; /* List of pages to vacuum and/or clean
								 * indices */
	VPageListData fraged_pages; /* List of pages with space enough for
								 * re-using */
	VPageDescr *vpp;
	Relation   *Irel;
	int32		nindices,
				i;
	VRelStats  *vacrelstats;

	StartTransactionCommand();

	/*
	 * Check for user-requested abort.  Note we want this to be inside
	 * a transaction, so xact.c doesn't issue useless NOTICE.
	 */
	if (QueryCancel)
		CancelQuery();

	/*
	 * Race condition -- if the pg_class tuple has gone away since the
	 * last time we saw it, we don't need to vacuum it.
	 */
	tuple = SearchSysCacheTuple(RELOID,
								ObjectIdGetDatum(relid),
								0, 0, 0);
	if (!HeapTupleIsValid(tuple))
	{
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

#ifndef NO_SECURITY
	if (!pg_ownercheck(GetPgUserName(), RelationGetRelationName(onerel),
					   RELNAME))
	{
		elog(NOTICE, "Skipping \"%s\" --- only table owner can VACUUM it",
			 RelationGetRelationName(onerel));
		heap_close(onerel, AccessExclusiveLock);
		CommitTransactionCommand();
		return;
	}
#endif

	/*
	 * Set up statistics-gathering machinery.
	 */
	vacrelstats = (VRelStats *) palloc(sizeof(VRelStats));
	vacrelstats->relid = relid;
	vacrelstats->num_pages = vacrelstats->num_tuples = 0;
	vacrelstats->hasindex = false;
	/* we can VACUUM ANALYZE any table except pg_statistic; see vc_updstats */
	if (analyze &&
		strcmp(RelationGetRelationName(onerel), StatisticRelationName) != 0)
	{
		int			attr_cnt,
				   *attnums = NULL;
		Form_pg_attribute *attr;

		attr_cnt = onerel->rd_att->natts;
		attr = onerel->rd_att->attrs;

		if (va_cols != NIL)
		{
			int			tcnt = 0;
			List	   *le;

			if (length(va_cols) > attr_cnt)
				elog(ERROR, "vacuum: too many attributes specified for relation %s",
					 RelationGetRelationName(onerel));
			attnums = (int *) palloc(attr_cnt * sizeof(int));
			foreach(le, va_cols)
			{
				char	   *col = (char *) lfirst(le);

				for (i = 0; i < attr_cnt; i++)
				{
					if (namestrcmp(&(attr[i]->attname), col) == 0)
						break;
				}
				if (i < attr_cnt)		/* found */
					attnums[tcnt++] = i;
				else
				{
					elog(ERROR, "vacuum: there is no attribute %s in %s",
						 col, RelationGetRelationName(onerel));
				}
			}
			attr_cnt = tcnt;
		}

		vacrelstats->vacattrstats = (VacAttrStats *) palloc(attr_cnt * sizeof(VacAttrStats));

		for (i = 0; i < attr_cnt; i++)
		{
			Operator	func_operator;
			Form_pg_operator pgopform;
			VacAttrStats *stats;

			stats = &vacrelstats->vacattrstats[i];
			stats->attr = palloc(ATTRIBUTE_TUPLE_SIZE);
			memmove(stats->attr, attr[((attnums) ? attnums[i] : i)], ATTRIBUTE_TUPLE_SIZE);
			stats->best = stats->guess1 = stats->guess2 = 0;
			stats->max = stats->min = 0;
			stats->best_len = stats->guess1_len = stats->guess2_len = 0;
			stats->max_len = stats->min_len = 0;
			stats->initialized = false;
			stats->best_cnt = stats->guess1_cnt = stats->guess1_hits = stats->guess2_hits = 0;
			stats->max_cnt = stats->min_cnt = stats->null_cnt = stats->nonnull_cnt = 0;

			func_operator = oper("=", stats->attr->atttypid, stats->attr->atttypid, true);
			if (func_operator != NULL)
			{
				pgopform = (Form_pg_operator) GETSTRUCT(func_operator);
				fmgr_info(pgopform->oprcode, &(stats->f_cmpeq));
			}
			else
				stats->f_cmpeq.fn_addr = NULL;

			func_operator = oper("<", stats->attr->atttypid, stats->attr->atttypid, true);
			if (func_operator != NULL)
			{
				pgopform = (Form_pg_operator) GETSTRUCT(func_operator);
				fmgr_info(pgopform->oprcode, &(stats->f_cmplt));
				stats->op_cmplt = oprid(func_operator);
			}
			else
			{
				stats->f_cmplt.fn_addr = NULL;
				stats->op_cmplt = InvalidOid;
			}

			func_operator = oper(">", stats->attr->atttypid, stats->attr->atttypid, true);
			if (func_operator != NULL)
			{
				pgopform = (Form_pg_operator) GETSTRUCT(func_operator);
				fmgr_info(pgopform->oprcode, &(stats->f_cmpgt));
			}
			else
				stats->f_cmpgt.fn_addr = NULL;

			typetuple = SearchSysCacheTuple(TYPEOID,
								 ObjectIdGetDatum(stats->attr->atttypid),
											0, 0, 0);
			if (HeapTupleIsValid(typetuple))
				stats->outfunc = ((Form_pg_type) GETSTRUCT(typetuple))->typoutput;
			else
				stats->outfunc = InvalidOid;
		}
		vacrelstats->va_natts = attr_cnt;
		/* delete existing pg_statistic rows for relation */
		vc_delstats(relid, ((attnums) ? attr_cnt : 0), attnums);
		if (attnums)
			pfree(attnums);
	}
	else
	{
		vacrelstats->va_natts = 0;
		vacrelstats->vacattrstats = (VacAttrStats *) NULL;
	}

	GetXmaxRecent(&XmaxRecent);

	/* scan it */
	vacuum_pages.vpl_num_pages = fraged_pages.vpl_num_pages = 0;
	vc_scanheap(vacrelstats, onerel, &vacuum_pages, &fraged_pages);

	/* Now open indices */
	Irel = (Relation *) NULL;
	vc_getindices(vacrelstats->relid, &nindices, &Irel);

	if (nindices > 0)
		vacrelstats->hasindex = true;
	else
		vacrelstats->hasindex = false;

	/* Clean/scan index relation(s) */
	if (Irel != (Relation *) NULL)
	{
		if (vacuum_pages.vpl_num_pages > 0)
		{
			for (i = 0; i < nindices; i++)
				vc_vaconeind(&vacuum_pages, Irel[i], vacrelstats->num_tuples, 0);
		}
		else
/* just scan indices to update statistic */
		{
			for (i = 0; i < nindices; i++)
				vc_scanoneind(Irel[i], vacrelstats->num_tuples);
		}
	}

	if (fraged_pages.vpl_num_pages > 0) /* Try to shrink heap */
		vc_rpfheap(vacrelstats, onerel, &vacuum_pages, &fraged_pages, nindices, Irel);
	else
	{
		if (Irel != (Relation *) NULL)
			vc_clsindices(nindices, Irel);
		if (vacuum_pages.vpl_num_pages > 0)		/* Clean pages from
												 * vacuum_pages list */
			vc_vacheap(vacrelstats, onerel, &vacuum_pages);
	}

	/* ok - free vacuum_pages list of reapped pages */
	if (vacuum_pages.vpl_num_pages > 0)
	{
		vpp = vacuum_pages.vpl_pagedesc;
		for (i = 0; i < vacuum_pages.vpl_num_pages; i++, vpp++)
			pfree(*vpp);
		pfree(vacuum_pages.vpl_pagedesc);
		if (fraged_pages.vpl_num_pages > 0)
			pfree(fraged_pages.vpl_pagedesc);
	}

	/* update statistics in pg_class */
	vc_updstats(vacrelstats->relid, vacrelstats->num_pages,
			vacrelstats->num_tuples, vacrelstats->hasindex, vacrelstats);

	/* all done with this class, but hold lock until commit */
	heap_close(onerel, NoLock);

	/* next command frees attribute stats */
	CommitTransactionCommand();
}

/*
 *	vc_scanheap() -- scan an open heap relation
 *
 *		This routine sets commit times, constructs vacuum_pages list of
 *		empty/uninitialized pages and pages with dead tuples and
 *		~LP_USED line pointers, constructs fraged_pages list of pages
 *		appropriate for purposes of shrinking and maintains statistics
 *		on the number of live tuples in a heap.
 */
static void
vc_scanheap(VRelStats *vacrelstats, Relation onerel,
			VPageList vacuum_pages, VPageList fraged_pages)
{
	int			nblocks,
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
	VPageDescr	vpc,
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

	vpc = (VPageDescr) palloc(sizeof(VPageDescrData) + MaxOffsetNumber * sizeof(OffsetNumber));
	vpc->vpd_offsets_used = 0;

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		buf = ReadBuffer(onerel, blkno);
		page = BufferGetPage(buf);
		vpc->vpd_blkno = blkno;
		vpc->vpd_offsets_free = 0;

		if (PageIsNew(page))
		{
			elog(NOTICE, "Rel %s: Uninitialized page %u - fixing",
				 relname, blkno);
			PageInit(page, BufferGetPageSize(buf), 0);
			vpc->vpd_free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			free_size += (vpc->vpd_free - sizeof(ItemIdData));
			new_pages++;
			empty_end_pages++;
			vc_reappage(vacuum_pages, vpc);
			WriteBuffer(buf);
			continue;
		}

		if (PageIsEmpty(page))
		{
			vpc->vpd_free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			free_size += (vpc->vpd_free - sizeof(ItemIdData));
			empty_pages++;
			empty_end_pages++;
			vc_reappage(vacuum_pages, vpc);
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
				vpc->vpd_offsets[vpc->vpd_offsets_free++] = offnum;
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
						pgchanged = true;
						tuple.t_data->t_infomask |= HEAP_XMAX_INVALID;
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
				if (tupgone && tuple.t_data->t_xmax >= XmaxRecent)
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

				if (tempPage == (Page) NULL)
				{
					Size		pageSize;

					pageSize = PageGetPageSize(page);
					tempPage = (Page) palloc(pageSize);
					memmove(tempPage, page, pageSize);
				}

				lpp = &(((PageHeader) tempPage)->pd_linp[offnum - 1]);

				/* mark it unused */
				lpp->lp_flags &= ~LP_USED;

				vpc->vpd_offsets[vpc->vpd_offsets_free++] = offnum;
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
				vc_attrstats(onerel, vacrelstats, &tuple);
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
			vpc->vpd_free = ((PageHeader) tempPage)->pd_upper - ((PageHeader) tempPage)->pd_lower;
			free_size += vpc->vpd_free;
			vc_reappage(vacuum_pages, vpc);
			pfree(tempPage);
			tempPage = (Page) NULL;
		}
		else if (vpc->vpd_offsets_free > 0)
		{						/* there are only ~LP_USED line pointers */
			vpc->vpd_free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			free_size += vpc->vpd_free;
			vc_reappage(vacuum_pages, vpc);
		}
		if (dobufrel)
			ReleaseBuffer(buf);
		if (notup)
			empty_end_pages++;
		else
			empty_end_pages = 0;
	}

	pfree(vpc);

	/* save stats in the rel list for use later */
	vacrelstats->num_tuples = num_tuples;
	vacrelstats->num_pages = nblocks;
/*	  vacrelstats->natts = attr_cnt;*/
	if (num_tuples == 0)
		min_tlen = max_tlen = 0;
	vacrelstats->min_tlen = min_tlen;
	vacrelstats->max_tlen = max_tlen;

	vacuum_pages->vpl_empty_end_pages = empty_end_pages;
	fraged_pages->vpl_empty_end_pages = empty_end_pages;

	/*
	 * Try to make fraged_pages keeping in mind that we can't use free
	 * space of "empty" end-pages and last page if it reapped.
	 */
	if (do_shrinking && vacuum_pages->vpl_num_pages - empty_end_pages > 0)
	{
		int			nusf;		/* blocks usefull for re-using */

		nusf = vacuum_pages->vpl_num_pages - empty_end_pages;
		if ((vacuum_pages->vpl_pagedesc[nusf - 1])->vpd_blkno == nblocks - empty_end_pages - 1)
			nusf--;

		for (i = 0; i < nusf; i++)
		{
			vp = vacuum_pages->vpl_pagedesc[i];
			if (vc_enough_space(vp, min_tlen))
			{
				vc_vpinsert(fraged_pages, vp);
				usable_free_size += vp->vpd_free;
			}
		}
	}

	if (usable_free_size > 0 && num_vtlinks > 0)
	{
		qsort((char *) vtlinks, num_vtlinks, sizeof(VTupleLinkData),
			  vc_cmp_vtlinks);
		vacrelstats->vtlinks = vtlinks;
		vacrelstats->num_vtlinks = num_vtlinks;
	}
	else
	{
		vacrelstats->vtlinks = NULL;
		vacrelstats->num_vtlinks = 0;
		pfree(vtlinks);
	}

	elog(MESSAGE_LEVEL, "Pages %u: Changed %u, Reapped %u, Empty %u, New %u; \
Tup %u: Vac %u, Keep/VTL %u/%u, Crash %u, UnUsed %u, MinLen %u, MaxLen %u; \
Re-using: Free/Avail. Space %u/%u; EndEmpty/Avail. Pages %u/%u. %s",
		 nblocks, changed_pages, vacuum_pages->vpl_num_pages, empty_pages,
		 new_pages, num_tuples, tups_vacuumed,
		 nkeep, vacrelstats->num_vtlinks, ncrash,
		 nunused, min_tlen, max_tlen, free_size, usable_free_size,
		 empty_end_pages, fraged_pages->vpl_num_pages,
		 vc_show_rusage(&ru0));

}	/* vc_scanheap */


/*
 *	vc_rpfheap() -- try to repair relation's fragmentation
 *
 *		This routine marks dead tuples as unused and tries re-use dead space
 *		by moving tuples (and inserting indices if needed). It constructs
 *		Nvpl list of free-ed pages (moved tuples) and clean indices
 *		for them after committing (in hack-manner - without losing locks
 *		and freeing memory!) current transaction. It truncates relation
 *		if some end-blocks are gone away.
 */
static void
vc_rpfheap(VRelStats *vacrelstats, Relation onerel,
		   VPageList vacuum_pages, VPageList fraged_pages,
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
	TupleDesc	tupdesc = NULL;
	Datum	   *idatum = NULL;
	char	   *inulls = NULL;
	InsertIndexResult iresult;
	VPageListData Nvpl;
	VPageDescr	cur_page = NULL,
				last_vacuum_page,
				vpc,
			   *vpp;
	int			cur_item = 0;
	IndDesc    *Idesc,
			   *idcur;
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

	if (Irel != (Relation *) NULL)		/* preparation for index' inserts */
	{
		vc_mkindesc(onerel, nindices, Irel, &Idesc);
		tupdesc = RelationGetDescr(onerel);
		idatum = (Datum *) palloc(INDEX_MAX_KEYS * sizeof(*idatum));
		inulls = (char *) palloc(INDEX_MAX_KEYS * sizeof(*inulls));
	}

	Nvpl.vpl_num_pages = 0;
	num_fraged_pages = fraged_pages->vpl_num_pages;
	Assert(vacuum_pages->vpl_num_pages > vacuum_pages->vpl_empty_end_pages);
	vacuumed_pages = vacuum_pages->vpl_num_pages - vacuum_pages->vpl_empty_end_pages;
	last_vacuum_page = vacuum_pages->vpl_pagedesc[vacuumed_pages - 1];
	last_vacuum_block = last_vacuum_page->vpd_blkno;
	cur_buffer = InvalidBuffer;
	num_moved = 0;

	vpc = (VPageDescr) palloc(sizeof(VPageDescrData) + MaxOffsetNumber * sizeof(OffsetNumber));
	vpc->vpd_offsets_used = vpc->vpd_offsets_free = 0;

	/*
	 * Scan pages backwards from the last nonempty page, trying to move
	 * tuples down to lower pages.  Quit when we reach a page that we
	 * have moved any tuples onto.  Note that if a page is still in the
	 * fraged_pages list (list of candidate move-target pages) when we
	 * reach it, we will remove it from the list.  This ensures we never
	 * move a tuple up to a higher page number.
	 *
	 * NB: this code depends on the vacuum_pages and fraged_pages lists
	 * being in order, and on fraged_pages being a subset of vacuum_pages.
	 */
	nblocks = vacrelstats->num_pages;
	for (blkno = nblocks - vacuum_pages->vpl_empty_end_pages - 1;
		 blkno > last_move_dest_block;
		 blkno--)
	{
		buf = ReadBuffer(onerel, blkno);
		page = BufferGetPage(buf);

		vpc->vpd_offsets_free = 0;

		isempty = PageIsEmpty(page);

		dowrite = false;
		if (blkno == last_vacuum_block) /* it's reapped page */
		{
			if (last_vacuum_page->vpd_offsets_free > 0) /* there are dead tuples */
			{					/* on this page - clean */
				Assert(!isempty);
				vc_vacpage(page, last_vacuum_page);
				dowrite = true;
			}
			else
				Assert(isempty);
			--vacuumed_pages;
			if (vacuumed_pages > 0)
			{
				/* get prev reapped page from vacuum_pages */
				last_vacuum_page = vacuum_pages->vpl_pagedesc[vacuumed_pages - 1];
				last_vacuum_block = last_vacuum_page->vpd_blkno;
			}
			else
			{
				last_vacuum_page = NULL;
				last_vacuum_block = -1;
			}
			if (num_fraged_pages > 0 &&
				blkno ==
				fraged_pages->vpl_pagedesc[num_fraged_pages-1]->vpd_blkno)
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
		vpc->vpd_blkno = blkno;
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
				 * have to check is it in vpc or not - i.e. is it moved
				 * while cleaning this page or some previous one.
				 */
				if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
				{
					if (keep_tuples == 0)
						continue;
					if (chain_tuple_moved)		/* some chains was moved
												 * while */
					{			/* cleaning this page */
						Assert(vpc->vpd_offsets_free > 0);
						for (i = 0; i < vpc->vpd_offsets_free; i++)
						{
							if (vpc->vpd_offsets[i] == offnum)
								break;
						}
						if (i >= vpc->vpd_offsets_free) /* not found */
						{
							vpc->vpd_offsets[vpc->vpd_offsets_free++] = offnum;
							keep_tuples--;
						}
					}
					else
					{
						vpc->vpd_offsets[vpc->vpd_offsets_free++] = offnum;
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
				VPageDescr	to_vpd = NULL;
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
						 * This means that in the middle of chain there was
						 * tuple updated by older (than XmaxRecent) xaction
						 * and this tuple is already deleted by me. Actually,
						 * upper part of chain should be removed and seems 
						 * that this should be handled in vc_scanheap(), but
						 * it's not implemented at the moment and so we
						 * just stop shrinking here.
						 */
						ReleaseBuffer(Cbuf);
						pfree(vtmove);
						vtmove = NULL;
						elog(NOTICE, "Child itemid in update-chain marked as unused - can't continue vc_rpfheap");
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
					if (to_vpd == NULL ||
						!vc_enough_space(to_vpd, tlen))
					{
						/* if to_vpd no longer has enough free space to be
						 * useful, remove it from fraged_pages list
						 */
						if (to_vpd != NULL &&
							!vc_enough_space(to_vpd, vacrelstats->min_tlen))
						{
							Assert(num_fraged_pages > to_item);
							memmove(fraged_pages->vpl_pagedesc + to_item,
									fraged_pages->vpl_pagedesc + to_item + 1,
									sizeof(VPageDescr) * (num_fraged_pages - to_item - 1));
							num_fraged_pages--;
						}
						for (i = 0; i < num_fraged_pages; i++)
						{
							if (vc_enough_space(fraged_pages->vpl_pagedesc[i], tlen))
								break;
						}
						if (i == num_fraged_pages)		/* can't move item
														 * anywhere */
						{
							for (i = 0; i < num_vtmove; i++)
							{
								Assert(vtmove[i].vpd->vpd_offsets_used > 0);
								(vtmove[i].vpd->vpd_offsets_used)--;
							}
							num_vtmove = 0;
							break;
						}
						to_item = i;
						to_vpd = fraged_pages->vpl_pagedesc[to_item];
					}
					to_vpd->vpd_free -= MAXALIGN(tlen);
					if (to_vpd->vpd_offsets_used >= to_vpd->vpd_offsets_free)
						to_vpd->vpd_free -= MAXALIGN(sizeof(ItemIdData));
					(to_vpd->vpd_offsets_used)++;
					if (free_vtmove == 0)
					{
						free_vtmove = 1000;
						vtmove = (VTupleMove) repalloc(vtmove,
											 (free_vtmove + num_vtmove) *
												 sizeof(VTupleMoveData));
					}
					vtmove[num_vtmove].tid = tp.t_self;
					vtmove[num_vtmove].vpd = to_vpd;
					if (to_vpd->vpd_offsets_used == 1)
						vtmove[num_vtmove].cleanVpd = true;
					else
						vtmove[num_vtmove].cleanVpd = false;
					free_vtmove--;
					num_vtmove++;

					/*
					 * All done ?
					 */
					if (!(tp.t_data->t_infomask & HEAP_UPDATED) ||
						tp.t_data->t_xmin < XmaxRecent)
						break;

					/*
					 * Well, try to find tuple with old row version
					 */
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
							vc_find_eq((void *) (vacrelstats->vtlinks),
									   vacrelstats->num_vtlinks,
									   sizeof(VTupleLinkData),
									   (void *) &vtld,
									   vc_cmp_vtlinks);
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
						 * Read above about cases when !ItemIdIsUsed(Citemid)
						 * (child item is removed)... Due to the fact that
						 * at the moment we don't remove unuseful part of 
						 * update-chain, it's possible to get too old
						 * parent row here. Like as in the case which 
						 * caused this problem, we stop shrinking here.
						 * I could try to find real parent row but want
						 * not to do it because of real solution will
						 * be implemented anyway, latter, and we are too
						 * close to 6.5 release.		- vadim 06/11/99
						 */
						if (Ptp.t_data->t_xmax != tp.t_data->t_xmin)
						{
							if (freeCbuf)
								ReleaseBuffer(Cbuf);
							freeCbuf = false;
							ReleaseBuffer(Pbuf);
							for (i = 0; i < num_vtmove; i++)
							{
								Assert(vtmove[i].vpd->vpd_offsets_used > 0);
								(vtmove[i].vpd->vpd_offsets_used)--;
							}
							num_vtmove = 0;
							elog(NOTICE, "Too old parent tuple found - can't continue vc_rpfheap");
							break;
						}
#ifdef NOT_USED			/* I'm not sure that this will wotk properly... */
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
					/* Get tuple from chain */
					tuple.t_self = vtmove[ti].tid;
					Cbuf = ReadBuffer(onerel,
							 ItemPointerGetBlockNumber(&(tuple.t_self)));
					Cpage = BufferGetPage(Cbuf);
					Citemid = PageGetItemId(Cpage,
							ItemPointerGetOffsetNumber(&(tuple.t_self)));
					tuple.t_datamcxt = NULL;
					tuple.t_data = (HeapTupleHeader) PageGetItem(Cpage, Citemid);
					tuple_len = tuple.t_len = ItemIdGetLength(Citemid);
					/* Get page to move in */
					cur_buffer = ReadBuffer(onerel, vtmove[ti].vpd->vpd_blkno);

					/*
					 * We should LockBuffer(cur_buffer) but don't, at the
					 * moment. If you'll do LockBuffer then UNLOCK it
					 * before index_insert: unique btree-s call heap_fetch
					 * to get t_infomask of inserted heap tuple !!!
					 */
					ToPage = BufferGetPage(cur_buffer);
					/* if this page was not used before - clean it */
					if (!PageIsEmpty(ToPage) && vtmove[ti].cleanVpd)
						vc_vacpage(ToPage, vtmove[ti].vpd);
					heap_copytuple_with_tuple(&tuple, &newtup);
					RelationInvalidateHeapTuple(onerel, &tuple);
					TransactionIdStore(myXID, (TransactionId *) &(newtup.t_data->t_cmin));
					newtup.t_data->t_infomask &=
						~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | HEAP_MOVED_OFF);
					newtup.t_data->t_infomask |= HEAP_MOVED_IN;
					newoff = PageAddItem(ToPage, (Item) newtup.t_data, tuple_len,
										 InvalidOffsetNumber, LP_USED);
					if (newoff == InvalidOffsetNumber)
					{
						elog(ERROR, "\
moving chain: failed to add item with len = %u to page %u",
							 tuple_len, vtmove[ti].vpd->vpd_blkno);
					}
					newitemid = PageGetItemId(ToPage, newoff);
					pfree(newtup.t_data);
					newtup.t_datamcxt = NULL;
					newtup.t_data = (HeapTupleHeader) PageGetItem(ToPage, newitemid);
					ItemPointerSet(&(newtup.t_self), vtmove[ti].vpd->vpd_blkno, newoff);
					if (((int) vtmove[ti].vpd->vpd_blkno) > last_move_dest_block)
						last_move_dest_block = vtmove[ti].vpd->vpd_blkno;

					/*
					 * Set t_ctid pointing to itself for last tuple in
					 * chain and to next tuple in chain otherwise.
					 */
					if (!ItemPointerIsValid(&Ctid))
						newtup.t_data->t_ctid = newtup.t_self;
					else
						newtup.t_data->t_ctid = Ctid;
					Ctid = newtup.t_self;

					TransactionIdStore(myXID, (TransactionId *) &(tuple.t_data->t_cmin));
					tuple.t_data->t_infomask &=
						~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | HEAP_MOVED_IN);
					tuple.t_data->t_infomask |= HEAP_MOVED_OFF;

					num_moved++;

					/*
					 * Remember that we moved tuple from the current page
					 * (corresponding index tuple will be cleaned).
					 */
					if (Cbuf == buf)
						vpc->vpd_offsets[vpc->vpd_offsets_free++] =
							ItemPointerGetOffsetNumber(&(tuple.t_self));
					else
						keep_tuples++;

					if (Irel != (Relation *) NULL)
					{
						for (i = 0, idcur = Idesc; i < nindices; i++, idcur++)
						{
							FormIndexDatum(idcur->natts,
							   (AttrNumber *) &(idcur->tform->indkey[0]),
										   &newtup,
										   tupdesc,
										   idatum,
										   inulls,
										   idcur->finfoP);
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
					if (Cbuf == buf)
						ReleaseBuffer(Cbuf);
					else
						WriteBuffer(Cbuf);
				}
				cur_buffer = InvalidBuffer;
				pfree(vtmove);
				chain_tuple_moved = true;
				continue;
			}

			/* try to find new page for this tuple */
			if (cur_buffer == InvalidBuffer ||
				!vc_enough_space(cur_page, tuple_len))
			{
				if (cur_buffer != InvalidBuffer)
				{
					WriteBuffer(cur_buffer);
					cur_buffer = InvalidBuffer;
					/*
					 * If previous target page is now too full to add
					 * *any* tuple to it, remove it from fraged_pages.
					 */
					if (!vc_enough_space(cur_page, vacrelstats->min_tlen))
					{
						Assert(num_fraged_pages > cur_item);
						memmove(fraged_pages->vpl_pagedesc + cur_item,
								fraged_pages->vpl_pagedesc + cur_item + 1,
								sizeof(VPageDescr) * (num_fraged_pages - cur_item - 1));
						num_fraged_pages--;
					}
				}
				for (i = 0; i < num_fraged_pages; i++)
				{
					if (vc_enough_space(fraged_pages->vpl_pagedesc[i], tuple_len))
						break;
				}
				if (i == num_fraged_pages)
					break;		/* can't move item anywhere */
				cur_item = i;
				cur_page = fraged_pages->vpl_pagedesc[cur_item];
				cur_buffer = ReadBuffer(onerel, cur_page->vpd_blkno);
				ToPage = BufferGetPage(cur_buffer);
				/* if this page was not used before - clean it */
				if (!PageIsEmpty(ToPage) && cur_page->vpd_offsets_used == 0)
					vc_vacpage(ToPage, cur_page);
			}

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
failed to add item with len = %u to page %u (free space %u, nusd %u, noff %u)",
					 tuple_len, cur_page->vpd_blkno, cur_page->vpd_free,
				 cur_page->vpd_offsets_used, cur_page->vpd_offsets_free);
			}
			newitemid = PageGetItemId(ToPage, newoff);
			pfree(newtup.t_data);
			newtup.t_datamcxt = NULL;
			newtup.t_data = (HeapTupleHeader) PageGetItem(ToPage, newitemid);
			ItemPointerSet(&(newtup.t_data->t_ctid), cur_page->vpd_blkno, newoff);
			newtup.t_self = newtup.t_data->t_ctid;

			/*
			 * Mark old tuple as moved_off by vacuum and store vacuum XID
			 * in t_cmin !!!
			 */
			TransactionIdStore(myXID, (TransactionId *) &(tuple.t_data->t_cmin));
			tuple.t_data->t_infomask &=
				~(HEAP_XMIN_COMMITTED | HEAP_XMIN_INVALID | HEAP_MOVED_IN);
			tuple.t_data->t_infomask |= HEAP_MOVED_OFF;

			cur_page->vpd_offsets_used++;
			num_moved++;
			cur_page->vpd_free = ((PageHeader) ToPage)->pd_upper - ((PageHeader) ToPage)->pd_lower;
			if (((int) cur_page->vpd_blkno) > last_move_dest_block)
				last_move_dest_block = cur_page->vpd_blkno;

			vpc->vpd_offsets[vpc->vpd_offsets_free++] = offnum;

			/* insert index' tuples if needed */
			if (Irel != (Relation *) NULL)
			{
				for (i = 0, idcur = Idesc; i < nindices; i++, idcur++)
				{
					FormIndexDatum(idcur->natts,
							   (AttrNumber *) &(idcur->tform->indkey[0]),
								   &newtup,
								   tupdesc,
								   idatum,
								   inulls,
								   idcur->finfoP);
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
					if (chain_tuple_moved)		/* some chains was moved
												 * while */
					{			/* cleaning this page */
						Assert(vpc->vpd_offsets_free > 0);
						for (i = 0; i < vpc->vpd_offsets_free; i++)
						{
							if (vpc->vpd_offsets[i] == off)
								break;
						}
						if (i >= vpc->vpd_offsets_free) /* not found */
						{
							vpc->vpd_offsets[vpc->vpd_offsets_free++] = off;
							Assert(keep_tuples > 0);
							keep_tuples--;
						}
					}
					else
					{
						vpc->vpd_offsets[vpc->vpd_offsets_free++] = off;
						Assert(keep_tuples > 0);
						keep_tuples--;
					}
				}
			}
		}

		if (vpc->vpd_offsets_free > 0)	/* some tuples were moved */
		{
			if (chain_tuple_moved)		/* else - they are ordered */
			{
				qsort((char *) (vpc->vpd_offsets), vpc->vpd_offsets_free,
					  sizeof(OffsetNumber), vc_cmp_offno);
			}
			vc_reappage(&Nvpl, vpc);
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

		/*
		 * We have to commit our tuple' movings before we'll truncate
		 * relation, but we shouldn't lose our locks. And so - quick hack:
		 * flush buffers and record status of current transaction as
		 * committed, and continue. - vadim 11/13/96
		 */
		FlushBufferPool();
		TransactionIdCommit(myXID);
		FlushBufferPool();
	}

	/*
	 * Clean uncleaned reapped pages from vacuum_pages list list and set
	 * xmin committed for inserted tuples
	 */
	checked_moved = 0;
	for (i = 0, vpp = vacuum_pages->vpl_pagedesc; i < vacuumed_pages; i++, vpp++)
	{
		Assert((*vpp)->vpd_blkno < blkno);
		buf = ReadBuffer(onerel, (*vpp)->vpd_blkno);
		page = BufferGetPage(buf);
		if ((*vpp)->vpd_offsets_used == 0)		/* this page was not used */
		{
			if (!PageIsEmpty(page))
				vc_vacpage(page, *vpp);
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
			Assert((*vpp)->vpd_offsets_used == num_tuples);
			checked_moved += num_tuples;
		}
		WriteBuffer(buf);
	}
	Assert(num_moved == checked_moved);

	elog(MESSAGE_LEVEL, "Rel %s: Pages: %u --> %u; Tuple(s) moved: %u. %s",
		 RelationGetRelationName(onerel),
		 nblocks, blkno, num_moved,
		 vc_show_rusage(&ru0));

	if (Nvpl.vpl_num_pages > 0)
	{
		/* vacuum indices again if needed */
		if (Irel != (Relation *) NULL)
		{
			VPageDescr *vpleft,
					   *vpright,
						vpsave;

			/* re-sort Nvpl.vpl_pagedesc */
			for (vpleft = Nvpl.vpl_pagedesc,
				 vpright = Nvpl.vpl_pagedesc + Nvpl.vpl_num_pages - 1;
				 vpleft < vpright; vpleft++, vpright--)
			{
				vpsave = *vpleft;
				*vpleft = *vpright;
				*vpright = vpsave;
			}
			Assert(keep_tuples >= 0);
			for (i = 0; i < nindices; i++)
				vc_vaconeind(&Nvpl, Irel[i],
							 vacrelstats->num_tuples, keep_tuples);
		}

		/*
		 * clean moved tuples from last page in Nvpl list
		 */
		if (vpc->vpd_blkno == blkno - 1 && vpc->vpd_offsets_free > 0)
		{
			buf = ReadBuffer(onerel, vpc->vpd_blkno);
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
			Assert(vpc->vpd_offsets_free == num_tuples);
			PageRepairFragmentation(page);
			WriteBuffer(buf);
		}

		/* now - free new list of reapped pages */
		vpp = Nvpl.vpl_pagedesc;
		for (i = 0; i < Nvpl.vpl_num_pages; i++, vpp++)
			pfree(*vpp);
		pfree(Nvpl.vpl_pagedesc);
	}

	/* truncate relation */
	if (blkno < nblocks)
	{
		i = FlushRelationBuffers(onerel, blkno, false);
		if (i < 0)
			elog(FATAL, "VACUUM (vc_rpfheap): FlushRelationBuffers returned %d", i);
		blkno = smgrtruncate(DEFAULT_SMGR, onerel, blkno);
		Assert(blkno >= 0);
		vacrelstats->num_pages = blkno; /* set new number of blocks */
	}

	if (Irel != (Relation *) NULL)		/* pfree index' allocations */
	{
		pfree(Idesc);
		pfree(idatum);
		pfree(inulls);
		vc_clsindices(nindices, Irel);
	}

	pfree(vpc);
	if (vacrelstats->vtlinks != NULL)
		pfree(vacrelstats->vtlinks);

}	/* vc_rpfheap */

/*
 *	vc_vacheap() -- free dead tuples
 *
 *		This routine marks dead tuples as unused and truncates relation
 *		if there are "empty" end-blocks.
 */
static void
vc_vacheap(VRelStats *vacrelstats, Relation onerel, VPageList vacuum_pages)
{
	Buffer		buf;
	Page		page;
	VPageDescr *vpp;
	int			nblocks;
	int			i;

	nblocks = vacuum_pages->vpl_num_pages;
	nblocks -= vacuum_pages->vpl_empty_end_pages;		/* nothing to do with
														 * them */

	for (i = 0, vpp = vacuum_pages->vpl_pagedesc; i < nblocks; i++, vpp++)
	{
		if ((*vpp)->vpd_offsets_free > 0)
		{
			buf = ReadBuffer(onerel, (*vpp)->vpd_blkno);
			page = BufferGetPage(buf);
			vc_vacpage(page, *vpp);
			WriteBuffer(buf);
		}
	}

	/* truncate relation if there are some empty end-pages */
	if (vacuum_pages->vpl_empty_end_pages > 0)
	{
		Assert(vacrelstats->num_pages >= vacuum_pages->vpl_empty_end_pages);
		nblocks = vacrelstats->num_pages - vacuum_pages->vpl_empty_end_pages;
		elog(MESSAGE_LEVEL, "Rel %s: Pages: %u --> %u.",
			 RelationGetRelationName(onerel),
			 vacrelstats->num_pages, nblocks);

		/*
		 * We have to flush "empty" end-pages (if changed, but who knows it)
		 * before truncation
		 */
		FlushBufferPool();

		i = FlushRelationBuffers(onerel, nblocks, false);
		if (i < 0)
			elog(FATAL, "VACUUM (vc_vacheap): FlushRelationBuffers returned %d", i);

		nblocks = smgrtruncate(DEFAULT_SMGR, onerel, nblocks);
		Assert(nblocks >= 0);
		vacrelstats->num_pages = nblocks;		/* set new number of
												 * blocks */
	}

}	/* vc_vacheap */

/*
 *	vc_vacpage() -- free dead tuples on a page
 *					 and repair its fragmentation.
 */
static void
vc_vacpage(Page page, VPageDescr vpd)
{
	ItemId		itemid;
	int			i;

	/* There shouldn't be any tuples moved onto the page yet! */
	Assert(vpd->vpd_offsets_used == 0);

	for (i = 0; i < vpd->vpd_offsets_free; i++)
	{
		itemid = &(((PageHeader) page)->pd_linp[vpd->vpd_offsets[i] - 1]);
		itemid->lp_flags &= ~LP_USED;
	}
	PageRepairFragmentation(page);

}	/* vc_vacpage */

/*
 *	_vc_scanoneind() -- scan one index relation to update statistic.
 *
 */
static void
vc_scanoneind(Relation indrel, int num_tuples)
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
	vc_updstats(RelationGetRelid(indrel), nipages, nitups, false, NULL);

	elog(MESSAGE_LEVEL, "Index %s: Pages %u; Tuples %u. %s",
		 RelationGetRelationName(indrel), nipages, nitups,
		 vc_show_rusage(&ru0));

	if (nitups != num_tuples)
		elog(NOTICE, "Index %s: NUMBER OF INDEX' TUPLES (%u) IS NOT THE SAME AS HEAP' (%u).\
\n\tRecreate the index.",
			 RelationGetRelationName(indrel), nitups, num_tuples);

}	/* vc_scanoneind */

/*
 *	vc_vaconeind() -- vacuum one index relation.
 *
 *		Vpl is the VPageList of the heap we're currently vacuuming.
 *		It's locked. Indrel is an index relation on the vacuumed heap.
 *		We don't set locks on the index	relation here, since the indexed
 *		access methods support locking at different granularities.
 *		We let them handle it.
 *
 *		Finally, we arrange to update the index relation's statistics in
 *		pg_class.
 */
static void
vc_vaconeind(VPageList vpl, Relation indrel, int num_tuples, int keep_tuples)
{
	RetrieveIndexResult res;
	IndexScanDesc iscan;
	ItemPointer heapptr;
	int			tups_vacuumed;
	int			num_index_tuples;
	int			num_pages;
	VPageDescr	vp;
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

		if ((vp = vc_tidreapped(heapptr, vpl)) != (VPageDescr) NULL)
		{
#ifdef NOT_USED
			elog(DEBUG, "<%x,%x> -> <%x,%x>",
				 ItemPointerGetBlockNumber(&(res->index_iptr)),
				 ItemPointerGetOffsetNumber(&(res->index_iptr)),
				 ItemPointerGetBlockNumber(&(res->heap_iptr)),
				 ItemPointerGetOffsetNumber(&(res->heap_iptr)));
#endif
			if (vp->vpd_offsets_free == 0)
			{					/* this is EmptyPage !!! */
				elog(NOTICE, "Index %s: pointer to EmptyPage (blk %u off %u) - fixing",
					 RelationGetRelationName(indrel),
					 vp->vpd_blkno, ItemPointerGetOffsetNumber(heapptr));
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
	vc_updstats(RelationGetRelid(indrel), num_pages, num_index_tuples, false, NULL);

	elog(MESSAGE_LEVEL, "Index %s: Pages %u; Tuples %u: Deleted %u. %s",
		 RelationGetRelationName(indrel), num_pages,
		 num_index_tuples - keep_tuples, tups_vacuumed,
		 vc_show_rusage(&ru0));

	if (num_index_tuples != num_tuples + keep_tuples)
		elog(NOTICE, "Index %s: NUMBER OF INDEX' TUPLES (%u) IS NOT THE SAME AS HEAP' (%u).\
\n\tRecreate the index.",
			 RelationGetRelationName(indrel), num_index_tuples, num_tuples);

}	/* vc_vaconeind */

/*
 *	vc_tidreapped() -- is a particular tid reapped?
 *
 *		vpl->VPageDescr_array is sorted in right order.
 */
static VPageDescr
vc_tidreapped(ItemPointer itemptr, VPageList vpl)
{
	OffsetNumber ioffno;
	OffsetNumber *voff;
	VPageDescr	vp,
			   *vpp;
	VPageDescrData vpd;

	vpd.vpd_blkno = ItemPointerGetBlockNumber(itemptr);
	ioffno = ItemPointerGetOffsetNumber(itemptr);

	vp = &vpd;
	vpp = (VPageDescr *) vc_find_eq((void *) (vpl->vpl_pagedesc),
					vpl->vpl_num_pages, sizeof(VPageDescr), (void *) &vp,
									vc_cmp_blk);

	if (vpp == (VPageDescr *) NULL)
		return (VPageDescr) NULL;
	vp = *vpp;

	/* ok - we are on true page */

	if (vp->vpd_offsets_free == 0)
	{							/* this is EmptyPage !!! */
		return vp;
	}

	voff = (OffsetNumber *) vc_find_eq((void *) (vp->vpd_offsets),
			vp->vpd_offsets_free, sizeof(OffsetNumber), (void *) &ioffno,
									   vc_cmp_offno);

	if (voff == (OffsetNumber *) NULL)
		return (VPageDescr) NULL;

	return vp;

}	/* vc_tidreapped */

/*
 *	vc_attrstats() -- compute column statistics used by the optimzer
 *
 *	We compute the column min, max, null and non-null counts.
 *	Plus we attempt to find the count of the value that occurs most
 *	frequently in each column.  These figures are used to compute
 *	the selectivity of the column.
 *
 *	We use a three-bucked cache to get the most frequent item.
 *	The 'guess' buckets count hits.  A cache miss causes guess1
 *	to get the most hit 'guess' item in the most recent cycle, and
 *	the new item goes into guess2.	Whenever the total count of hits
 *	of a 'guess' entry is larger than 'best', 'guess' becomes 'best'.
 *
 *	This method works perfectly for columns with unique values, and columns
 *	with only two unique values, plus nulls.
 *
 *	It becomes less perfect as the number of unique values increases and
 *	their distribution in the table becomes more random.
 *
 */
static void
vc_attrstats(Relation onerel, VRelStats *vacrelstats, HeapTuple tuple)
{
	int			i,
				attr_cnt = vacrelstats->va_natts;
	VacAttrStats *vacattrstats = vacrelstats->vacattrstats;
	TupleDesc	tupDesc = onerel->rd_att;
	Datum		value;
	bool		isnull;

	for (i = 0; i < attr_cnt; i++)
	{
		VacAttrStats *stats = &vacattrstats[i];
		bool		value_hit = true;

		value = heap_getattr(tuple,
							 stats->attr->attnum, tupDesc, &isnull);

		if (!VacAttrStatsEqValid(stats))
			continue;

		if (isnull)
			stats->null_cnt++;
		else
		{
			stats->nonnull_cnt++;
			if (stats->initialized == false)
			{
				vc_bucketcpy(stats->attr, value, &stats->best, &stats->best_len);
				/* best_cnt gets incremented later */
				vc_bucketcpy(stats->attr, value, &stats->guess1, &stats->guess1_len);
				stats->guess1_cnt = stats->guess1_hits = 1;
				vc_bucketcpy(stats->attr, value, &stats->guess2, &stats->guess2_len);
				stats->guess2_hits = 1;
				if (VacAttrStatsLtGtValid(stats))
				{
					vc_bucketcpy(stats->attr, value, &stats->max, &stats->max_len);
					vc_bucketcpy(stats->attr, value, &stats->min, &stats->min_len);
				}
				stats->initialized = true;
			}
			if (VacAttrStatsLtGtValid(stats))
			{
				if ((*fmgr_faddr(&stats->f_cmplt)) (value, stats->min))
				{
					vc_bucketcpy(stats->attr, value, &stats->min, &stats->min_len);
					stats->min_cnt = 0;
				}
				if ((*fmgr_faddr(&stats->f_cmpgt)) (value, stats->max))
				{
					vc_bucketcpy(stats->attr, value, &stats->max, &stats->max_len);
					stats->max_cnt = 0;
				}
				if ((*fmgr_faddr(&stats->f_cmpeq)) (value, stats->min))
					stats->min_cnt++;
				else if ((*fmgr_faddr(&stats->f_cmpeq)) (value, stats->max))
					stats->max_cnt++;
			}
			if ((*fmgr_faddr(&stats->f_cmpeq)) (value, stats->best))
				stats->best_cnt++;
			else if ((*fmgr_faddr(&stats->f_cmpeq)) (value, stats->guess1))
			{
				stats->guess1_cnt++;
				stats->guess1_hits++;
			}
			else if ((*fmgr_faddr(&stats->f_cmpeq)) (value, stats->guess2))
				stats->guess2_hits++;
			else
				value_hit = false;

			if (stats->guess2_hits > stats->guess1_hits)
			{
				swapDatum(stats->guess1, stats->guess2);
				swapInt(stats->guess1_len, stats->guess2_len);
				swapLong(stats->guess1_hits, stats->guess2_hits);
				stats->guess1_cnt = stats->guess1_hits;
			}
			if (stats->guess1_cnt > stats->best_cnt)
			{
				swapDatum(stats->best, stats->guess1);
				swapInt(stats->best_len, stats->guess1_len);
				swapLong(stats->best_cnt, stats->guess1_cnt);
				stats->guess1_hits = 1;
				stats->guess2_hits = 1;
			}
			if (!value_hit)
			{
				vc_bucketcpy(stats->attr, value, &stats->guess2, &stats->guess2_len);
				stats->guess1_hits = 1;
				stats->guess2_hits = 1;
			}
		}
	}
	return;
}

/*
 *	vc_bucketcpy() -- update pg_class statistics for one relation
 *
 */
static void
vc_bucketcpy(Form_pg_attribute attr, Datum value, Datum *bucket, int *bucket_len)
{
	if (attr->attbyval && attr->attlen != -1)
		*bucket = value;
	else
	{
		int			len = (attr->attlen != -1 ? attr->attlen : VARSIZE(value));

		if (len > *bucket_len)
		{
			if (*bucket_len != 0)
				pfree(DatumGetPointer(*bucket));
			*bucket = PointerGetDatum(palloc(len));
			*bucket_len = len;
		}
		memmove(DatumGetPointer(*bucket), DatumGetPointer(value), len);
	}
}

/*
 *	vc_updstats() -- update statistics for one relation
 *
 *		Statistics are stored in several places: the pg_class row for the
 *		relation has stats about the whole relation, the pg_attribute rows
 *		for each attribute store "disbursion", and there is a pg_statistic
 *		row for each (non-system) attribute.  (Disbursion probably ought to
 *		be moved to pg_statistic, but it's not worth doing unless there's
 *		another reason to have to change pg_attribute.)  Disbursion and
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
 *
 *		Updates of pg_attribute statistics are handled in the same way
 *		for the same reasons.
 *
 *		To keep things simple, we punt for pg_statistic, and don't try
 *		to compute or store rows for pg_statistic itself in pg_statistic.
 *		This could possibly be made to work, but it's not worth the trouble.
 */
static void
vc_updstats(Oid relid, int num_pages, int num_tuples, bool hasindex,
			VRelStats *vacrelstats)
{
	Relation	rd,
				ad,
				sd;
	HeapScanDesc scan;
	HeapTupleData rtup;
	HeapTuple	ctup,
				atup,
				stup;
	Form_pg_class pgcform;
	ScanKeyData askey;
	Form_pg_attribute attp;
	Buffer		buffer;

	/*
	 * update number of tuples and number of pages in pg_class
	 */
	rd = heap_openr(RelationRelationName, RowExclusiveLock);

	ctup = SearchSysCacheTupleCopy(RELOID,
								   ObjectIdGetDatum(relid),
								   0, 0, 0);
	if (!HeapTupleIsValid(ctup))
		elog(ERROR, "pg_class entry for relid %u vanished during vacuuming",
			 relid);

	/* get the buffer cache tuple */
	rtup.t_self = ctup->t_self;
	heap_fetch(rd, SnapshotNow, &rtup, &buffer);
	heap_freetuple(ctup);

	/* overwrite the existing statistics in the tuple */
	pgcform = (Form_pg_class) GETSTRUCT(&rtup);
	pgcform->reltuples = num_tuples;
	pgcform->relpages = num_pages;
	pgcform->relhasindex = hasindex;

	/* invalidate the tuple in the cache and write the buffer */
	RelationInvalidateHeapTuple(rd, &rtup);
	WriteBuffer(buffer);

	heap_close(rd, RowExclusiveLock);

	if (vacrelstats != NULL && vacrelstats->va_natts > 0)
	{
		VacAttrStats *vacattrstats = vacrelstats->vacattrstats;
		int			natts = vacrelstats->va_natts;

		ad = heap_openr(AttributeRelationName, RowExclusiveLock);
		sd = heap_openr(StatisticRelationName, RowExclusiveLock);

		/* Find pg_attribute rows for this relation */
		ScanKeyEntryInitialize(&askey, 0, Anum_pg_attribute_attrelid,
							   F_INT4EQ, relid);

		scan = heap_beginscan(ad, false, SnapshotNow, 1, &askey);

		while (HeapTupleIsValid(atup = heap_getnext(scan, 0)))
		{
			int			i;
			VacAttrStats *stats;

			attp = (Form_pg_attribute) GETSTRUCT(atup);
			if (attp->attnum <= 0)		/* skip system attributes for now */
				continue;

			for (i = 0; i < natts; i++)
			{
				if (attp->attnum == vacattrstats[i].attr->attnum)
					break;
			}
			if (i >= natts)
				continue;				/* skip attr if no stats collected */
			stats = &(vacattrstats[i]);

			if (VacAttrStatsEqValid(stats))
			{
				float32data selratio;		/* average ratio of rows selected
											 * for a random constant */

				/* Compute disbursion */
				if (stats->nonnull_cnt == 0 && stats->null_cnt == 0)
				{
					/* empty relation, so put a dummy value in attdisbursion */
					selratio = 0;
				}
				else if (stats->null_cnt <= 1 && stats->best_cnt == 1)
				{
					/* looks like we have a unique-key attribute ---
					 * flag this with special -1.0 flag value.
					 *
					 * The correct disbursion is 1.0/numberOfRows, but since
					 * the relation row count can get updated without
					 * recomputing disbursion, we want to store a "symbolic"
					 * value and figure 1.0/numberOfRows on the fly.
					 */
					selratio = -1;
				}
				else
				{
					if (VacAttrStatsLtGtValid(stats) &&
						stats->min_cnt + stats->max_cnt == stats->nonnull_cnt)
					{
						/* exact result when there are just 1 or 2 values... */
						double		min_cnt_d = stats->min_cnt,
									max_cnt_d = stats->max_cnt,
									null_cnt_d = stats->null_cnt;
						double		total = ((double) stats->nonnull_cnt) + null_cnt_d;

						selratio = (min_cnt_d * min_cnt_d + max_cnt_d * max_cnt_d + null_cnt_d * null_cnt_d) / (total * total);
					}
					else
					{
						double		most = (double) (stats->best_cnt > stats->null_cnt ? stats->best_cnt : stats->null_cnt);
						double		total = ((double) stats->nonnull_cnt) + ((double) stats->null_cnt);

						/*
						 * we assume count of other values are 20% of best
						 * count in table
						 */
						selratio = (most * most + 0.20 * most * (total - most)) / (total * total);
					}
					/* Make sure calculated values are in-range */
					if (selratio < 0.0)
						selratio = 0.0;
					else if (selratio > 1.0)
						selratio = 1.0;
				}

				/* overwrite the existing statistics in the tuple */
				attp->attdisbursion = selratio;

				/* invalidate the tuple in the cache and write the buffer */
				RelationInvalidateHeapTuple(ad, atup);
				WriteNoReleaseBuffer(scan->rs_cbuf);

				/*
				 * Create pg_statistic tuples for the relation, if we have
				 * gathered the right data.  vc_delstats() previously
				 * deleted all the pg_statistic tuples for the rel, so we
				 * just have to insert new ones here.
				 *
				 * Note vc_vacone() has seen to it that we won't come here
				 * when vacuuming pg_statistic itself.
				 */
				if (VacAttrStatsLtGtValid(stats) && stats->initialized)
				{
					float32data nullratio;
					float32data bestratio;
					FmgrInfo	out_function;
					char	   *out_string;
					double		best_cnt_d = stats->best_cnt,
								null_cnt_d = stats->null_cnt,
								nonnull_cnt_d = stats->nonnull_cnt;		/* prevent overflow */
					Datum		values[Natts_pg_statistic];
					char		nulls[Natts_pg_statistic];

					nullratio = null_cnt_d / (nonnull_cnt_d + null_cnt_d);
					bestratio = best_cnt_d / (nonnull_cnt_d + null_cnt_d);

					fmgr_info(stats->outfunc, &out_function);

					for (i = 0; i < Natts_pg_statistic; ++i)
						nulls[i] = ' ';

					/* ----------------
					 *	initialize values[]
					 * ----------------
					 */
					i = 0;
					values[i++] = (Datum) relid;		/* starelid */
					values[i++] = (Datum) attp->attnum; /* staattnum */
					values[i++] = (Datum) stats->op_cmplt;	/* staop */
					/* hack: this code knows float4 is pass-by-ref */
					values[i++] = PointerGetDatum(&nullratio);	/* stanullfrac */
					values[i++] = PointerGetDatum(&bestratio);	/* stacommonfrac */
					out_string = (*fmgr_faddr(&out_function)) (stats->best, stats->attr->atttypid, stats->attr->atttypmod);
					values[i++] = PointerGetDatum(textin(out_string)); /* stacommonval */
					pfree(out_string);
					out_string = (*fmgr_faddr(&out_function)) (stats->min, stats->attr->atttypid, stats->attr->atttypmod);
					values[i++] = PointerGetDatum(textin(out_string)); /* staloval */
					pfree(out_string);
					out_string = (char *) (*fmgr_faddr(&out_function)) (stats->max, stats->attr->atttypid, stats->attr->atttypmod);
					values[i++] = PointerGetDatum(textin(out_string)); /* stahival */
					pfree(out_string);

					stup = heap_formtuple(sd->rd_att, values, nulls);

					/* ----------------
					 *	Watch out for oversize tuple, which can happen if
					 *	all three of the saved data values are long.
					 *	Our fallback strategy is just to not store the
					 *	pg_statistic tuple at all in that case.  (We could
					 *	replace the values by NULLs and still store the
					 *	numeric stats, but presently selfuncs.c couldn't
					 *	do anything useful with that case anyway.)
					 *
					 *	We could reduce the probability of overflow, but not
					 *	prevent it, by storing the data values as compressed
					 *	text; is that worth doing?  The problem should go
					 *	away whenever long tuples get implemented...
					 * ----------------
					 */
					if (MAXALIGN(stup->t_len) <= MaxTupleSize)
					{
						/* OK, store tuple and update indexes too */
						Relation	irelations[Num_pg_statistic_indices];

						heap_insert(sd, stup);
						CatalogOpenIndices(Num_pg_statistic_indices, Name_pg_statistic_indices, irelations);
						CatalogIndexInsert(irelations, Num_pg_statistic_indices, sd, stup);
						CatalogCloseIndices(Num_pg_statistic_indices, irelations);
					}

					/* release allocated space */
					pfree(DatumGetPointer(values[Anum_pg_statistic_stacommonval-1]));
					pfree(DatumGetPointer(values[Anum_pg_statistic_staloval-1]));
					pfree(DatumGetPointer(values[Anum_pg_statistic_stahival-1]));
					heap_freetuple(stup);
				}
			}
		}
		heap_endscan(scan);
		/* close rels, but hold locks till upcoming commit */
		heap_close(ad, NoLock);
		heap_close(sd, NoLock);
	}
}

/*
 *	vc_delstats() -- delete pg_statistic rows for a relation
 *
 *	If a list of attribute numbers is given, only zap stats for those attrs.
 */
static void
vc_delstats(Oid relid, int attcnt, int *attnums)
{
	Relation	pgstatistic;
	HeapScanDesc scan;
	HeapTuple	tuple;
	ScanKeyData key;

	pgstatistic = heap_openr(StatisticRelationName, RowExclusiveLock);

	ScanKeyEntryInitialize(&key, 0x0, Anum_pg_statistic_starelid,
						   F_OIDEQ, ObjectIdGetDatum(relid));
	scan = heap_beginscan(pgstatistic, false, SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		if (attcnt > 0)
		{
			Form_pg_statistic pgs = (Form_pg_statistic) GETSTRUCT(tuple);
			int			i;

			for (i = 0; i < attcnt; i++)
			{
				if (pgs->staattnum == attnums[i] + 1)
					break;
			}
			if (i >= attcnt)
				continue;		/* don't delete it */
		}
		heap_delete(pgstatistic, &tuple->t_self, NULL);
	}

	heap_endscan(scan);
	/*
	 * Close rel, but *keep* lock; we will need to reacquire it later,
	 * so there's a possibility of deadlock against another VACUUM process
	 * if we let go now.  Keeping the lock shouldn't delay any common
	 * operation other than an attempted VACUUM of pg_statistic itself.
	 */
	heap_close(pgstatistic, NoLock);
}

/*
 *	vc_reappage() -- save a page on the array of reapped pages.
 *
 *		As a side effect of the way that the vacuuming loop for a given
 *		relation works, higher pages come after lower pages in the array
 *		(and highest tid on a page is last).
 */
static void
vc_reappage(VPageList vpl, VPageDescr vpc)
{
	VPageDescr	newvpd;

	/* allocate a VPageDescrData entry */
	newvpd = (VPageDescr) palloc(sizeof(VPageDescrData) + vpc->vpd_offsets_free * sizeof(OffsetNumber));

	/* fill it in */
	if (vpc->vpd_offsets_free > 0)
		memmove(newvpd->vpd_offsets, vpc->vpd_offsets, vpc->vpd_offsets_free * sizeof(OffsetNumber));
	newvpd->vpd_blkno = vpc->vpd_blkno;
	newvpd->vpd_free = vpc->vpd_free;
	newvpd->vpd_offsets_used = vpc->vpd_offsets_used;
	newvpd->vpd_offsets_free = vpc->vpd_offsets_free;

	/* insert this page into vpl list */
	vc_vpinsert(vpl, newvpd);

}	/* vc_reappage */

static void
vc_vpinsert(VPageList vpl, VPageDescr vpnew)
{
#define PG_NPAGEDESC 1024

	/* allocate a VPageDescr entry if needed */
	if (vpl->vpl_num_pages == 0)
	{
		vpl->vpl_pagedesc = (VPageDescr *) palloc(PG_NPAGEDESC * sizeof(VPageDescr));
		vpl->vpl_num_allocated_pages = PG_NPAGEDESC;
	}
	else if (vpl->vpl_num_pages >= vpl->vpl_num_allocated_pages)
	{
		vpl->vpl_num_allocated_pages *= 2;
		vpl->vpl_pagedesc = (VPageDescr *) repalloc(vpl->vpl_pagedesc, vpl->vpl_num_allocated_pages * sizeof(VPageDescr));
	}
	vpl->vpl_pagedesc[vpl->vpl_num_pages] = vpnew;
	(vpl->vpl_num_pages)++;

}

static void *
vc_find_eq(void *bot, int nelem, int size, void *elm,
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

}	/* vc_find_eq */

static int
vc_cmp_blk(const void *left, const void *right)
{
	BlockNumber lblk,
				rblk;

	lblk = (*((VPageDescr *) left))->vpd_blkno;
	rblk = (*((VPageDescr *) right))->vpd_blkno;

	if (lblk < rblk)
		return -1;
	if (lblk == rblk)
		return 0;
	return 1;

}	/* vc_cmp_blk */

static int
vc_cmp_offno(const void *left, const void *right)
{

	if (*(OffsetNumber *) left < *(OffsetNumber *) right)
		return -1;
	if (*(OffsetNumber *) left == *(OffsetNumber *) right)
		return 0;
	return 1;

}	/* vc_cmp_offno */

static int
vc_cmp_vtlinks(const void *left, const void *right)
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
vc_getindices(Oid relid, int *nindices, Relation **Irel)
{
	Relation	pgindex;
	Relation	irel;
	TupleDesc	tupdesc;
	HeapTuple	tuple;
	HeapScanDesc scan;
	Datum		d;
	int			i,
				k;
	bool		n;
	ScanKeyData key;
	Oid		   *ioid;

	*nindices = i = 0;

	ioid = (Oid *) palloc(10 * sizeof(Oid));

	/* prepare a heap scan on the pg_index relation */
	pgindex = heap_openr(IndexRelationName, AccessShareLock);
	tupdesc = RelationGetDescr(pgindex);

	ScanKeyEntryInitialize(&key, 0x0, Anum_pg_index_indrelid,
						   F_OIDEQ,
						   ObjectIdGetDatum(relid));

	scan = heap_beginscan(pgindex, false, SnapshotNow, 1, &key);

	while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
	{
		d = heap_getattr(tuple, Anum_pg_index_indexrelid,
						 tupdesc, &n);
		i++;
		if (i % 10 == 0)
			ioid = (Oid *) repalloc(ioid, (i + 10) * sizeof(Oid));
		ioid[i - 1] = DatumGetObjectId(d);
	}

	heap_endscan(scan);
	heap_close(pgindex, AccessShareLock);

	if (i == 0)
	{							/* No one index found */
		pfree(ioid);
		return;
	}

	if (Irel != (Relation **) NULL)
		*Irel = (Relation *) palloc(i * sizeof(Relation));

	for (k = 0; i > 0;)
	{
		irel = index_open(ioid[--i]);
		if (irel != (Relation) NULL)
		{
			if (Irel != (Relation **) NULL)
				(*Irel)[k] = irel;
			else
				index_close(irel);
			k++;
		}
		else
			elog(NOTICE, "CAN'T OPEN INDEX %u - SKIP IT", ioid[i]);
	}
	*nindices = k;
	pfree(ioid);

	if (Irel != (Relation **) NULL && *nindices == 0)
	{
		pfree(*Irel);
		*Irel = (Relation *) NULL;
	}

}	/* vc_getindices */


static void
vc_clsindices(int nindices, Relation *Irel)
{

	if (Irel == (Relation *) NULL)
		return;

	while (nindices--)
		index_close(Irel[nindices]);
	pfree(Irel);

}	/* vc_clsindices */


static void
vc_mkindesc(Relation onerel, int nindices, Relation *Irel, IndDesc **Idesc)
{
	IndDesc    *idcur;
	HeapTuple	cachetuple;
	AttrNumber *attnumP;
	int			natts;
	int			i;

	*Idesc = (IndDesc *) palloc(nindices * sizeof(IndDesc));

	for (i = 0, idcur = *Idesc; i < nindices; i++, idcur++)
	{
		cachetuple = SearchSysCacheTupleCopy(INDEXRELID,
							 ObjectIdGetDatum(RelationGetRelid(Irel[i])),
											 0, 0, 0);
		Assert(cachetuple);

		/*
		 * we never free the copy we make, because Idesc needs it for
		 * later
		 */
		idcur->tform = (Form_pg_index) GETSTRUCT(cachetuple);
		for (attnumP = &(idcur->tform->indkey[0]), natts = 0;
			 natts < INDEX_MAX_KEYS && *attnumP != InvalidAttrNumber;
			 attnumP++, natts++);
		if (idcur->tform->indproc != InvalidOid)
		{
			idcur->finfoP = &(idcur->finfo);
			FIgetnArgs(idcur->finfoP) = natts;
			natts = 1;
			FIgetProcOid(idcur->finfoP) = idcur->tform->indproc;
			*(FIgetname(idcur->finfoP)) = '\0';
		}
		else
			idcur->finfoP = (FuncIndexInfo *) NULL;

		idcur->natts = natts;
	}

}	/* vc_mkindesc */


static bool
vc_enough_space(VPageDescr vpd, Size len)
{

	len = MAXALIGN(len);

	if (len > vpd->vpd_free)
		return false;

	if (vpd->vpd_offsets_used < vpd->vpd_offsets_free)	/* there are free
														 * itemid(s) */
		return true;			/* and len <= free_space */

	/* ok. noff_usd >= noff_free and so we'll have to allocate new itemid */
	if (len + MAXALIGN(sizeof(ItemIdData)) <= vpd->vpd_free)
		return true;

	return false;

}	/* vc_enough_space */


/*
 * Compute elapsed time since ru0 usage snapshot, and format into
 * a displayable string.  Result is in a static string, which is
 * tacky, but no one ever claimed that the Postgres backend is
 * threadable...
 */
static char *
vc_show_rusage(struct rusage *ru0)
{
	static char		result[64];
	struct rusage	ru1;

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
