/*-------------------------------------------------------------------------
 *
 * vacuum.c--
 *	  the postgres vacuum cleaner
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/vacuum.c,v 1.90 1998/10/23 16:49:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "postgres.h"

#include "miscadmin.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "parser/parse_oper.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/shmem.h"
#include "storage/smgr.h"
#include "storage/itemptr.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/mcxt.h"
#include "utils/portal.h"
#include "utils/syscache.h"

#ifndef HAVE_GETRUSAGE
#include <rusagestub.h>
#else
#include <sys/time.h>
#include <sys/resource.h>
#endif

 /* #include <port-protos.h> *//* Why? */

extern int	BlowawayRelationBuffers(Relation rel, BlockNumber block);

bool		VacuumRunning = false;

static Portal vc_portal;

static int	MESSAGE_LEVEL;		/* message level */

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
static void vc_vaconeind(VPageList vpl, Relation indrel, int num_tuples);
static void vc_scanoneind(Relation indrel, int num_tuples);
static void vc_attrstats(Relation onerel, VRelStats *vacrelstats, HeapTuple tuple);
static void vc_bucketcpy(Form_pg_attribute attr, Datum value, Datum *bucket, int16 *bucket_len);
static void vc_updstats(Oid relid, int num_pages, int num_tuples, bool hasindex, VRelStats *vacrelstats);
static void vc_delhilowstats(Oid relid, int attcnt, int *attnums);
static void vc_setpagelock(Relation rel, BlockNumber blkno);
static VPageDescr vc_tidreapped(ItemPointer itemptr, VPageList vpl);
static void vc_reappage(VPageList vpl, VPageDescr vpc);
static void vc_vpinsert(VPageList vpl, VPageDescr vpnew);
static void vc_free(VRelList vrl);
static void vc_getindices(Oid relid, int *nindices, Relation **Irel);
static void vc_clsindices(int nindices, Relation *Irel);
static void vc_mkindesc(Relation onerel, int nindices, Relation *Irel, IndDesc **Idesc);
static char *vc_find_eq(char *bot, int nelem, int size, char *elm, int (*compar) (char *, char *));
static int	vc_cmp_blk(char *left, char *right);
static int	vc_cmp_offno(char *left, char *right);
static bool vc_enough_space(VPageDescr vpd, Size len);

void
vacuum(char *vacrel, bool verbose, bool analyze, List *va_spec)
{
	char	   *pname;
	MemoryContext old;
	PortalVariableMemory pmem;
	NameData	VacRel;
	List	   *le;
	List	   *va_cols = NIL;

	/*
	 * Create a portal for safe memory across transctions.	We need to
	 * palloc the name space for it because our hash function expects the
	 * name to be on a longword boundary.  CreatePortal copies the name to
	 * safe storage for us.
	 */
	pname = (char *) palloc(strlen(VACPNAME) + 1);
	strcpy(pname, VACPNAME);
	vc_portal = CreatePortal(pname);
	pfree(pname);

	if (verbose)
		MESSAGE_LEVEL = NOTICE;
	else
		MESSAGE_LEVEL = DEBUG;

	/* vacrel gets de-allocated on transaction commit */
	if (vacrel)
		strcpy(VacRel.data, vacrel);

	pmem = PortalGetVariableMemory(vc_portal);
	old = MemoryContextSwitchTo((MemoryContext) pmem);

	if (va_spec != NIL && !analyze)
		elog(ERROR, "Can't vacuum columns, only tables.  You can 'vacuum analyze' columns.");

	foreach(le, va_spec)
	{
		char	   *col = (char *) lfirst(le);
		char	   *dest;

		dest = (char *) palloc(strlen(col) + 1);
		strcpy(dest, col);
		va_cols = lappend(va_cols, dest);
	}
	MemoryContextSwitchTo(old);

	/* initialize vacuum cleaner */
	vc_init();

	/* vacuum the database */
	if (vacrel)
		vc_vacuum(&VacRel, analyze, va_cols);
	else
		vc_vacuum(NULL, analyze, NIL);

	PortalDestroy(&vc_portal);

	/* clean up */
	vc_shutdown();
}

/*
 *	vc_init(), vc_shutdown() -- start up and shut down the vacuum cleaner.
 *
 *		We run exactly one vacuum cleaner at a time.  We use the file system
 *		to guarantee an exclusive lock on vacuuming, since a single vacuum
 *		cleaner instantiation crosses transaction boundaries, and we'd lose
 *		postgres-style locks at the end of every transaction.
 *
 *		The strangeness with committing and starting transactions in the
 *		init and shutdown routines is due to the fact that the vacuum cleaner
 *		is invoked via a sql command, and so is already executing inside
 *		a transaction.	We need to leave ourselves in a predictable state
 *		on entry and exit to the vacuum cleaner.  We commit the transaction
 *		started in PostgresMain() inside vc_init(), and start one in
 *		vc_shutdown() to match the commit waiting for us back in
 *		PostgresMain().
 */
static void
vc_init()
{
	int			fd;

	if ((fd = open("pg_vlock", O_CREAT | O_EXCL, 0600)) < 0)
	{
		elog(ERROR, "Can't create lock file.  Is another vacuum cleaner running?\n\
\tIf not, you may remove the pg_vlock file in the %s\n\
\tdirectory", DatabasePath);
	}
	close(fd);

	/*
	 * By here, exclusive open on the lock file succeeded.	If we abort
	 * for any reason during vacuuming, we need to remove the lock file.
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
	/* on entry, not in a transaction */
	if (unlink("pg_vlock") < 0)
		elog(ERROR, "vacuum: can't destroy lock file!");

	/* okay, we're done */
	VacuumRunning = false;

	/* matches the CommitTransaction in PostgresMain() */
	StartTransactionCommand();

}

void
vc_abort()
{
	/* on abort, remove the vacuum cleaner lock file */
	unlink("pg_vlock");

	VacuumRunning = false;
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

	if (analyze && VacRelP == NULL && vrl != NULL)
		vc_delhilowstats(InvalidOid, 0, NULL);

	/* vacuum each heap relation */
	for (cur = vrl; cur != (VRelList) NULL; cur = cur->vrl_next)
		vc_vacone(cur->vrl_relid, analyze, va_cols);

	vc_free(vrl);
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

	if (VacRelP->data)
	{
		ScanKeyEntryInitialize(&key, 0x0, Anum_pg_class_relname,
							   F_NAMEEQ,
							   PointerGetDatum(VacRelP->data));
	}
	else
	{
		ScanKeyEntryInitialize(&key, 0x0, Anum_pg_class_relkind,
							   F_CHAREQ, CharGetDatum('r'));
	}

	portalmem = PortalGetVariableMemory(vc_portal);
	vrl = cur = (VRelList) NULL;

	rel = heap_openr(RelationRelationName);
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

		cur->vrl_relid = tuple->t_oid;
		cur->vrl_next = (VRelList) NULL;
	}
	if (found == false)
		elog(NOTICE, "Vacuum: table not found");


	heap_endscan(scan);
	heap_close(rel);

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

	/* now open the class and vacuum it */
	onerel = heap_open(relid);

	vacrelstats = (VRelStats *) palloc(sizeof(VRelStats));
	vacrelstats->relid = relid;
	vacrelstats->num_pages = vacrelstats->num_tuples = 0;
	vacrelstats->hasindex = false;
	if (analyze && !IsSystemRelationName((RelationGetRelationName(onerel))->data))
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
					 (RelationGetRelationName(onerel))->data);
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
						 col, (RelationGetRelationName(onerel))->data);
				}
			}
			attr_cnt = tcnt;
		}

		vacrelstats->vacattrstats =
			(VacAttrStats *) palloc(attr_cnt * sizeof(VacAttrStats));

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
			}
			else
				stats->f_cmplt.fn_addr = NULL;

			func_operator = oper(">", stats->attr->atttypid, stats->attr->atttypid, true);
			if (func_operator != NULL)
			{
				pgopform = (Form_pg_operator) GETSTRUCT(func_operator);
				fmgr_info(pgopform->oprcode, &(stats->f_cmpgt));
			}
			else
				stats->f_cmpgt.fn_addr = NULL;

			typetuple = SearchSysCacheTuple(TYPOID,
								 ObjectIdGetDatum(stats->attr->atttypid),
											0, 0, 0);
			if (HeapTupleIsValid(typetuple))
				stats->outfunc = ((Form_pg_type) GETSTRUCT(typetuple))->typoutput;
			else
				stats->outfunc = InvalidOid;
		}
		vacrelstats->va_natts = attr_cnt;
		vc_delhilowstats(relid, ((attnums) ? attr_cnt : 0), attnums);
		if (attnums)
			pfree(attnums);
	}
	else
	{
		vacrelstats->va_natts = 0;
		vacrelstats->vacattrstats = (VacAttrStats *) NULL;
	}

	/* we require the relation to be locked until the indices are cleaned */
	RelationSetLockForWrite(onerel);

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
				vc_vaconeind(&vacuum_pages, Irel[i], vacrelstats->num_tuples);
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

	/* all done with this class */
	heap_close(onerel);

	/* update statistics in pg_class */
	vc_updstats(vacrelstats->relid, vacrelstats->num_pages,
				vacrelstats->num_tuples, vacrelstats->hasindex, vacrelstats);

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
	ItemPointer itemptr;
	Buffer		buf;
	HeapTuple	tuple;
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
				nunused,
				ncrash,
				empty_pages,
				new_pages,
				changed_pages,
				empty_end_pages;
	Size		free_size,
				usable_free_size;
	Size		min_tlen = MAXTUPLEN;
	Size		max_tlen = 0;
	int32		i;
	struct rusage ru0,
				ru1;
	bool		do_shrinking = true;

	getrusage(RUSAGE_SELF, &ru0);

	tups_vacuumed = num_tuples = nunused = ncrash = empty_pages =
		new_pages = changed_pages = empty_end_pages = 0;
	free_size = usable_free_size = 0;

	relname = (RelationGetRelationName(onerel))->data;

	nblocks = RelationGetNumberOfBlocks(onerel);

	vpc = (VPageDescr) palloc(sizeof(VPageDescrData) + MaxOffsetNumber * sizeof(OffsetNumber));
	vpc->vpd_offsets_used = 0;

	elog(MESSAGE_LEVEL, "--Relation %s--", relname);

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

			tuple = (HeapTuple) PageGetItem(page, itemid);
			tupgone = false;

			if (!(tuple->t_infomask & HEAP_XMIN_COMMITTED))
			{
				if (tuple->t_infomask & HEAP_XMIN_INVALID)
					tupgone = true;
				else
				{
					if (TransactionIdDidAbort(tuple->t_xmin))
						tupgone = true;
					else if (TransactionIdDidCommit(tuple->t_xmin))
					{
						tuple->t_infomask |= HEAP_XMIN_COMMITTED;
						pgchanged = true;
					}
					else if (!TransactionIdIsInProgress(tuple->t_xmin))
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
							 relname, blkno, offnum, tuple->t_xmin);
						do_shrinking = false;
					}
				}
			}

			/*
			 * here we are concerned about tuples with xmin committed and
			 * xmax unknown or committed
			 */
			if (tuple->t_infomask & HEAP_XMIN_COMMITTED &&
				!(tuple->t_infomask & HEAP_XMAX_INVALID))
			{
				if (tuple->t_infomask & HEAP_XMAX_COMMITTED)
					tupgone = true;
				else if (TransactionIdDidAbort(tuple->t_xmax))
				{
					tuple->t_infomask |= HEAP_XMAX_INVALID;
					pgchanged = true;
				}
				else if (TransactionIdDidCommit(tuple->t_xmax))
					tupgone = true;
				else if (!TransactionIdIsInProgress(tuple->t_xmax))
				{

					/*
					 * Not Aborted, Not Committed, Not in Progress - so it
					 * from crashed process. - vadim 06/02/97
					 */
					tuple->t_infomask |= HEAP_XMAX_INVALID;;
					pgchanged = true;
				}
				else
				{
					elog(NOTICE, "Rel %s: TID %u/%u: DeleteTransactionInProgress %u - can't shrink relation",
						 relname, blkno, offnum, tuple->t_xmax);
					do_shrinking = false;
				}
			}

			/*
			 * It's possibly! But from where it comes ? And should we fix
			 * it ?  - vadim 11/28/96
			 */
			itemptr = &(tuple->t_ctid);
			if (!ItemPointerIsValid(itemptr) ||
				BlockIdGetBlockNumber(&(itemptr->ip_blkid)) != blkno)
			{
				elog(NOTICE, "Rel %s: TID %u/%u: TID IN TUPLEHEADER %u/%u IS NOT THE SAME. TUPGONE %d.",
					 relname, blkno, offnum,
					 BlockIdGetBlockNumber(&(itemptr->ip_blkid)),
					 itemptr->ip_posid, tupgone);
			}

			/*
			 * Other checks...
			 */
			if (tuple->t_len != itemid->lp_len)
			{
				elog(NOTICE, "Rel %s: TID %u/%u: TUPLE_LEN IN PAGEHEADER %u IS NOT THE SAME AS IN TUPLEHEADER %u. TUPGONE %d.",
					 relname, blkno, offnum,
					 itemid->lp_len, tuple->t_len, tupgone);
			}
			if (!OidIsValid(tuple->t_oid))
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
				if (tuple->t_len < min_tlen)
					min_tlen = tuple->t_len;
				if (tuple->t_len > max_tlen)
					max_tlen = tuple->t_len;
				vc_attrstats(onerel, vacrelstats, tuple);
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

	getrusage(RUSAGE_SELF, &ru1);

	elog(MESSAGE_LEVEL, "Pages %u: Changed %u, Reapped %u, Empty %u, New %u; \
Tup %u: Vac %u, Crash %u, UnUsed %u, MinLen %u, MaxLen %u; Re-using: Free/Avail. Space %u/%u; EndEmpty/Avail. Pages %u/%u. Elapsed %u/%u sec.",
		 nblocks, changed_pages, vacuum_pages->vpl_num_pages, empty_pages, new_pages,
		 num_tuples, tups_vacuumed, ncrash, nunused, min_tlen, max_tlen,
		 free_size, usable_free_size, empty_end_pages, fraged_pages->vpl_num_pages,
		 ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec,
		 ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

}	/* vc_scanheap */


/*
 *	vc_rpfheap() -- try to repaire relation' fragmentation
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
		   VPageList vacuum_pages, VPageList fraged_pages, int nindices, Relation *Irel)
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
	HeapTuple	tuple,
				newtup;
	TupleDesc	tupdesc = NULL;
	Datum	   *idatum = NULL;
	char	   *inulls = NULL;
	InsertIndexResult iresult;
	VPageListData Nvpl;
	VPageDescr	cur_page = NULL,
				last_fraged_page,
				last_vacuum_page,
				vpc,
			   *vpp;
	int			cur_item = 0;
	IndDesc    *Idesc,
			   *idcur;
	int			last_fraged_block,
				last_vacuum_block,
				i;
	Size		tuple_len;
	int			num_moved,
				num_fraged_pages,
				vacuumed_pages;
	int			checked_moved,
				num_tuples;
	bool		isempty,
				dowrite;
	struct rusage ru0,
				ru1;

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
	last_fraged_page = fraged_pages->vpl_pagedesc[num_fraged_pages - 1];
	last_fraged_block = last_fraged_page->vpd_blkno;
	Assert(vacuum_pages->vpl_num_pages > vacuum_pages->vpl_empty_end_pages);
	vacuumed_pages = vacuum_pages->vpl_num_pages - vacuum_pages->vpl_empty_end_pages;
	last_vacuum_page = vacuum_pages->vpl_pagedesc[vacuumed_pages - 1];
	last_vacuum_block = last_vacuum_page->vpd_blkno;
	Assert(last_vacuum_block >= last_fraged_block);
	cur_buffer = InvalidBuffer;
	num_moved = 0;

	vpc = (VPageDescr) palloc(sizeof(VPageDescrData) + MaxOffsetNumber * sizeof(OffsetNumber));
	vpc->vpd_offsets_used = vpc->vpd_offsets_free = 0;

	nblocks = vacrelstats->num_pages;
	for (blkno = nblocks - vacuum_pages->vpl_empty_end_pages - 1;; blkno--)
	{
		/* if it's reapped page and it was used by me - quit */
		if (blkno == last_fraged_block && last_fraged_page->vpd_offsets_used > 0)
			break;

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
			Assert(vacuumed_pages > 0);
			/* get prev reapped page from vacuum_pages */
			last_vacuum_page = vacuum_pages->vpl_pagedesc[vacuumed_pages - 1];
			last_vacuum_block = last_vacuum_page->vpd_blkno;
			if (blkno == last_fraged_block)		/* this page in
												 * fraged_pages too */
			{
				--num_fraged_pages;
				Assert(num_fraged_pages > 0);
				Assert(last_fraged_page->vpd_offsets_used == 0);
				/* get prev reapped page from fraged_pages */
				last_fraged_page = fraged_pages->vpl_pagedesc[num_fraged_pages - 1];
				last_fraged_block = last_fraged_page->vpd_blkno;
			}
			Assert(last_fraged_block <= last_vacuum_block);
			if (isempty)
			{
				ReleaseBuffer(buf);
				continue;
			}
		}
		else
			Assert(!isempty);

		vpc->vpd_blkno = blkno;
		maxoff = PageGetMaxOffsetNumber(page);
		for (offnum = FirstOffsetNumber;
			 offnum <= maxoff;
			 offnum = OffsetNumberNext(offnum))
		{
			itemid = PageGetItemId(page, offnum);

			if (!ItemIdIsUsed(itemid))
				continue;

			tuple = (HeapTuple) PageGetItem(page, itemid);
			tuple_len = tuple->t_len;

			/* try to find new page for this tuple */
			if (cur_buffer == InvalidBuffer ||
				!vc_enough_space(cur_page, tuple_len))
			{
				if (cur_buffer != InvalidBuffer)
				{
					WriteBuffer(cur_buffer);
					cur_buffer = InvalidBuffer;

					/*
					 * If no one tuple can't be added to this page -
					 * remove page from fraged_pages. - vadim 11/27/96
					 *
					 * But we can't remove last page - this is our
					 * "show-stopper" !!!	- vadim 02/25/98
					 */
					if (cur_page != last_fraged_page &&
						!vc_enough_space(cur_page, vacrelstats->min_tlen))
					{
						Assert(num_fraged_pages > cur_item + 1);
						memmove(fraged_pages->vpl_pagedesc + cur_item,
								fraged_pages->vpl_pagedesc + cur_item + 1,
								sizeof(VPageDescr *) * (num_fraged_pages - cur_item - 1));
						num_fraged_pages--;
						Assert(last_fraged_page == fraged_pages->vpl_pagedesc[num_fraged_pages - 1]);
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
			newtup = (HeapTuple) palloc(tuple_len);
			memmove((char *) newtup, (char *) tuple, tuple_len);

			RelationInvalidateHeapTuple(onerel, tuple);

			/* store transaction information */
			TransactionIdStore(myXID, &(newtup->t_xmin));
			newtup->t_cmin = myCID;
			StoreInvalidTransactionId(&(newtup->t_xmax));
			/* set xmin to unknown and xmax to invalid */
			newtup->t_infomask &= ~(HEAP_XACT_MASK);
			newtup->t_infomask |= HEAP_XMAX_INVALID;

			/* add tuple to the page */
			newoff = PageAddItem(ToPage, (Item) newtup, tuple_len,
								 InvalidOffsetNumber, LP_USED);
			if (newoff == InvalidOffsetNumber)
			{
				elog(ERROR, "\
failed to add item with len = %u to page %u (free space %u, nusd %u, noff %u)",
					 tuple_len, cur_page->vpd_blkno, cur_page->vpd_free,
				 cur_page->vpd_offsets_used, cur_page->vpd_offsets_free);
			}
			newitemid = PageGetItemId(ToPage, newoff);
			pfree(newtup);
			newtup = (HeapTuple) PageGetItem(ToPage, newitemid);
			ItemPointerSet(&(newtup->t_ctid), cur_page->vpd_blkno, newoff);

			/* now logically delete end-tuple */
			TransactionIdStore(myXID, &(tuple->t_xmax));
			tuple->t_cmax = myCID;
			/* set xmax to unknown */
			tuple->t_infomask &= ~(HEAP_XMAX_INVALID | HEAP_XMAX_COMMITTED);

			cur_page->vpd_offsets_used++;
			num_moved++;
			cur_page->vpd_free = ((PageHeader) ToPage)->pd_upper - ((PageHeader) ToPage)->pd_lower;
			vpc->vpd_offsets[vpc->vpd_offsets_free++] = offnum;

			/* insert index' tuples if needed */
			if (Irel != (Relation *) NULL)
			{
				for (i = 0, idcur = Idesc; i < nindices; i++, idcur++)
				{
					FormIndexDatum(idcur->natts,
							   (AttrNumber *) &(idcur->tform->indkey[0]),
								   newtup,
								   tupdesc,
								   idatum,
								   inulls,
								   idcur->finfoP);
					iresult = index_insert(Irel[i],
										   idatum,
										   inulls,
										   &newtup->t_ctid,
										   onerel);
					if (iresult)
						pfree(iresult);
				}
			}

		}						/* walk along page */

		if (vpc->vpd_offsets_free > 0)	/* some tuples were moved */
		{
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
		FlushBufferPool(!TransactionFlushEnabled());
		TransactionIdCommit(myXID);
		FlushBufferPool(!TransactionFlushEnabled());
	}

	/*
	 * Clean uncleaned reapped pages from vacuum_pages list and set xmin
	 * committed for inserted tuples
	 */
	checked_moved = 0;
	for (i = 0, vpp = vacuum_pages->vpl_pagedesc; i < vacuumed_pages; i++, vpp++)
	{
		Assert((*vpp)->vpd_blkno < blkno);
		buf = ReadBuffer(onerel, (*vpp)->vpd_blkno);
		page = BufferGetPage(buf);
		if ((*vpp)->vpd_offsets_used == 0)		/* this page was not used */
		{

			/*
			 * noff == 0 in empty pages only - such pages should be
			 * re-used
			 */
			Assert((*vpp)->vpd_offsets_free > 0);
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
				tuple = (HeapTuple) PageGetItem(page, itemid);
				if (TransactionIdEquals((TransactionId) tuple->t_xmin, myXID))
				{
					tuple->t_infomask |= HEAP_XMIN_COMMITTED;
					num_tuples++;
				}
			}
			Assert((*vpp)->vpd_offsets_used == num_tuples);
			checked_moved += num_tuples;
		}
		WriteBuffer(buf);
	}
	Assert(num_moved == checked_moved);

	getrusage(RUSAGE_SELF, &ru1);

	elog(MESSAGE_LEVEL, "Rel %s: Pages: %u --> %u; Tuple(s) moved: %u. \
Elapsed %u/%u sec.",
		 (RelationGetRelationName(onerel))->data,
		 nblocks, blkno, num_moved,
		 ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec,
		 ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

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
			for (i = 0; i < nindices; i++)
				vc_vaconeind(&Nvpl, Irel[i], vacrelstats->num_tuples);
		}

		/*
		 * clean moved tuples from last page in Nvpl list if some tuples
		 * left there
		 */
		if (vpc->vpd_offsets_free > 0 && offnum <= maxoff)
		{
			Assert(vpc->vpd_blkno == blkno - 1);
			buf = ReadBuffer(onerel, vpc->vpd_blkno);
			page = BufferGetPage(buf);
			num_tuples = 0;
			maxoff = offnum;
			for (offnum = FirstOffsetNumber;
				 offnum < maxoff;
				 offnum = OffsetNumberNext(offnum))
			{
				itemid = PageGetItemId(page, offnum);
				if (!ItemIdIsUsed(itemid))
					continue;
				tuple = (HeapTuple) PageGetItem(page, itemid);
				Assert(TransactionIdEquals((TransactionId) tuple->t_xmax, myXID));
				itemid->lp_flags &= ~LP_USED;
				num_tuples++;
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
		i = BlowawayRelationBuffers(onerel, blkno);
		if (i < 0)
			elog(FATAL, "VACUUM (vc_rpfheap): BlowawayRelationBuffers returned %d", i);
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
			 (RelationGetRelationName(onerel))->data,
			 vacrelstats->num_pages, nblocks);

		/*
		 * we have to flush "empty" end-pages (if changed, but who knows
		 * it) before truncation
		 */
		FlushBufferPool(!TransactionFlushEnabled());

		i = BlowawayRelationBuffers(onerel, nblocks);
		if (i < 0)
			elog(FATAL, "VACUUM (vc_vacheap): BlowawayRelationBuffers returned %d", i);

		nblocks = smgrtruncate(DEFAULT_SMGR, onerel, nblocks);
		Assert(nblocks >= 0);
		vacrelstats->num_pages = nblocks;		/* set new number of
												 * blocks */
	}

}	/* vc_vacheap */

/*
 *	vc_vacpage() -- free dead tuples on a page
 *					 and repaire its fragmentation.
 */
static void
vc_vacpage(Page page, VPageDescr vpd)
{
	ItemId		itemid;
	int			i;

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
	struct rusage ru0,
				ru1;

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

	getrusage(RUSAGE_SELF, &ru1);

	elog(MESSAGE_LEVEL, "Index %s: Pages %u; Tuples %u. Elapsed %u/%u sec.",
		 indrel->rd_rel->relname.data, nipages, nitups,
		 ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec,
		 ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

	if (nitups != num_tuples)
		elog(NOTICE, "Index %s: NUMBER OF INDEX' TUPLES (%u) IS NOT THE SAME AS HEAP' (%u)",
			 indrel->rd_rel->relname.data, nitups, num_tuples);

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
vc_vaconeind(VPageList vpl, Relation indrel, int num_tuples)
{
	RetrieveIndexResult res;
	IndexScanDesc iscan;
	ItemPointer heapptr;
	int			tups_vacuumed;
	int			num_index_tuples;
	int			num_pages;
	VPageDescr	vp;
	struct rusage ru0,
				ru1;

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
#if 0
			elog(DEBUG, "<%x,%x> -> <%x,%x>",
				 ItemPointerGetBlockNumber(&(res->index_iptr)),
				 ItemPointerGetOffsetNumber(&(res->index_iptr)),
				 ItemPointerGetBlockNumber(&(res->heap_iptr)),
				 ItemPointerGetOffsetNumber(&(res->heap_iptr)));
#endif
			if (vp->vpd_offsets_free == 0)
			{					/* this is EmptyPage !!! */
				elog(NOTICE, "Index %s: pointer to EmptyPage (blk %u off %u) - fixing",
					 indrel->rd_rel->relname.data,
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

	getrusage(RUSAGE_SELF, &ru1);

	elog(MESSAGE_LEVEL, "Index %s: Pages %u; Tuples %u: Deleted %u. Elapsed %u/%u sec.",
		 indrel->rd_rel->relname.data, num_pages, num_index_tuples, tups_vacuumed,
		 ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec,
		 ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

	if (num_index_tuples != num_tuples)
		elog(NOTICE, "Index %s: NUMBER OF INDEX' TUPLES (%u) IS NOT THE SAME AS HEAP' (%u)",
			 indrel->rd_rel->relname.data, num_index_tuples, num_tuples);

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
	vpp = (VPageDescr *) vc_find_eq((char *) (vpl->vpl_pagedesc),
					vpl->vpl_num_pages, sizeof(VPageDescr), (char *) &vp,
									vc_cmp_blk);

	if (vpp == (VPageDescr *) NULL)
		return (VPageDescr) NULL;
	vp = *vpp;

	/* ok - we are on true page */

	if (vp->vpd_offsets_free == 0)
	{							/* this is EmptyPage !!! */
		return vp;
	}

	voff = (OffsetNumber *) vc_find_eq((char *) (vp->vpd_offsets),
			vp->vpd_offsets_free, sizeof(OffsetNumber), (char *) &ioffno,
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
 *	frequently in each column
 *	These figures are used to compute the selectivity of the column
 *
 *	We use a three-bucked cache to get the most frequent item
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
				stats->guess1_cnt = stats->guess2_hits;
				swapLong(stats->guess1_hits, stats->guess2_hits);
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
vc_bucketcpy(Form_pg_attribute attr, Datum value, Datum *bucket, int16 *bucket_len)
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
 *	vc_updstats() -- update pg_class statistics for one relation
 *
 *		This routine works for both index and heap relation entries in
 *		pg_class.  We violate no-overwrite semantics here by storing new
 *		values for num_tuples, num_pages, and hasindex directly in the pg_class
 *		tuple that's already on the page.  The reason for this is that if
 *		we updated these tuples in the usual way, then every tuple in pg_class
 *		would be replaced every day.  This would make planning and executing
 *		historical queries very expensive.
 */
static void
vc_updstats(Oid relid, int num_pages, int num_tuples, bool hasindex, VRelStats *vacrelstats)
{
	Relation	rd,
				ad,
				sd;
	HeapScanDesc scan;
	HeapTuple	rtup,
				ctup,
				atup,
				stup;
	Form_pg_class pgcform;
	ScanKeyData askey;
	Form_pg_attribute attp;
	Buffer		buffer;

	/*
	 * update number of tuples and number of pages in pg_class
	 */
	ctup = SearchSysCacheTupleCopy(RELOID,
							   ObjectIdGetDatum(relid),
							   0, 0, 0);
	if (!HeapTupleIsValid(ctup))
		elog(ERROR, "pg_class entry for relid %d vanished during vacuuming",
			 relid);

	rd = heap_openr(RelationRelationName);

	/* get the buffer cache tuple */
	rtup = heap_fetch(rd, SnapshotNow, &ctup->t_ctid, &buffer);
	pfree(ctup);
	
	/* overwrite the existing statistics in the tuple */
	vc_setpagelock(rd, ItemPointerGetBlockNumber(&rtup->t_ctid));
	pgcform = (Form_pg_class) GETSTRUCT(rtup);
	pgcform->reltuples = num_tuples;
	pgcform->relpages = num_pages;
	pgcform->relhasindex = hasindex;

	if (vacrelstats != NULL && vacrelstats->va_natts > 0)
	{
		VacAttrStats *vacattrstats = vacrelstats->vacattrstats;
		int			natts = vacrelstats->va_natts;

		ad = heap_openr(AttributeRelationName);
		sd = heap_openr(StatisticRelationName);
		ScanKeyEntryInitialize(&askey, 0, Anum_pg_attribute_attrelid,
							   F_INT4EQ, relid);

		scan = heap_beginscan(ad, false, SnapshotNow, 1, &askey);

		while (HeapTupleIsValid(atup = heap_getnext(scan, 0)))
		{
			int			i;
			float32data selratio;		/* average ratio of rows selected
										 * for a random constant */
			VacAttrStats *stats;
			Datum		values[Natts_pg_statistic];
			char		nulls[Natts_pg_statistic];

			attp = (Form_pg_attribute) GETSTRUCT(atup);
			if (attp->attnum <= 0)		/* skip system attributes for now, */
				/* they are unique anyway */
				continue;

			for (i = 0; i < natts; i++)
			{
				if (attp->attnum == vacattrstats[i].attr->attnum)
					break;
			}
			if (i >= natts)
				continue;
			stats = &(vacattrstats[i]);

			/* overwrite the existing statistics in the tuple */
			if (VacAttrStatsEqValid(stats))
			{
				Buffer		abuffer;

				/*
				 * We manipulate the heap tuple in the
				 * buffer, so we fetch it to get the
				 * buffer number
				 */
				atup = heap_fetch(ad, SnapshotNow, &atup->t_ctid, &abuffer);
				vc_setpagelock(ad, ItemPointerGetBlockNumber(&atup->t_ctid));
				attp = (Form_pg_attribute) GETSTRUCT(atup);

				if (stats->nonnull_cnt + stats->null_cnt == 0 ||
					(stats->null_cnt <= 1 && stats->best_cnt == 1))
					selratio = 0;
				else if (VacAttrStatsLtGtValid(stats) && stats->min_cnt + stats->max_cnt == stats->nonnull_cnt)
				{
					double		min_cnt_d = stats->min_cnt,
								max_cnt_d = stats->max_cnt,
								null_cnt_d = stats->null_cnt,
								nonnullcnt_d = stats->nonnull_cnt;		/* prevent overflow */

					selratio = (min_cnt_d * min_cnt_d + max_cnt_d * max_cnt_d + null_cnt_d * null_cnt_d) /
						(nonnullcnt_d + null_cnt_d) / (nonnullcnt_d + null_cnt_d);
				}
				else
				{
					double		most = (double) (stats->best_cnt > stats->null_cnt ? stats->best_cnt : stats->null_cnt);
					double		total = ((double) stats->nonnull_cnt) + ((double) stats->null_cnt);

					/*
					 * we assume count of other values are 20% of best
					 * count in table
					 */
					selratio = (most * most + 0.20 * most * (total - most)) / total / total;
				}
				if (selratio > 1.0)
					selratio = 1.0;
				attp->attdisbursion = selratio;

				/*
				 * Invalidate the cache for the tuple
				 * and write the buffer
				 */
				RelationInvalidateHeapTuple(ad, atup);
				WriteNoReleaseBuffer(abuffer);
				ReleaseBuffer(abuffer);

				/* DO PG_STATISTIC INSERTS */

				/*
				 * doing system relations, especially pg_statistic is a
				 * problem
				 */
				if (VacAttrStatsLtGtValid(stats) && stats->initialized	/* &&
																		 * !IsSystemRelationName(
																		 *
					 pgcform->relname.data) */ )
				{
					FmgrInfo	out_function;
					char	   *out_string;

					for (i = 0; i < Natts_pg_statistic; ++i)
						nulls[i] = ' ';

					/* ----------------
					 *	initialize values[]
					 * ----------------
					 */
					i = 0;
					values[i++] = (Datum) relid;		/* 1 */
					values[i++] = (Datum) attp->attnum; /* 2 */
					values[i++] = (Datum) InvalidOid;	/* 3 */
					fmgr_info(stats->outfunc, &out_function);
					out_string = (*fmgr_faddr(&out_function)) (stats->min, stats->attr->atttypid);
					values[i++] = (Datum) fmgr(F_TEXTIN, out_string);
					pfree(out_string);
					out_string = (char *) (*fmgr_faddr(&out_function)) (stats->max, stats->attr->atttypid);
					values[i++] = (Datum) fmgr(F_TEXTIN, out_string);
					pfree(out_string);

					stup = heap_formtuple(sd->rd_att, values, nulls);

					/* ----------------
					 *	insert the tuple in the relation and get the tuple's oid.
					 * ----------------
					 */
					heap_insert(sd, stup);
					pfree(DatumGetPointer(values[3]));
					pfree(DatumGetPointer(values[4]));
					pfree(stup);
				}
			}
		}
		heap_endscan(scan);
		heap_close(ad);
		heap_close(sd);
	}

	/*
	 * Invalidate the cached pg_class tuple and
	 * write the buffer
	 */
	RelationInvalidateHeapTuple(rd, rtup);

	WriteNoReleaseBuffer(buffer);

	ReleaseBuffer(buffer);

	heap_close(rd);
}

/*
 *	vc_delhilowstats() -- delete pg_statistics rows
 *
 */
static void
vc_delhilowstats(Oid relid, int attcnt, int *attnums)
{
	Relation	pgstatistic;
	HeapScanDesc scan;
	HeapTuple	tuple;
	ScanKeyData key;

	pgstatistic = heap_openr(StatisticRelationName);

	if (relid != InvalidOid)
	{
		ScanKeyEntryInitialize(&key, 0x0, Anum_pg_statistic_starelid,
							   F_OIDEQ,
							   ObjectIdGetDatum(relid));
		scan = heap_beginscan(pgstatistic, false, SnapshotNow, 1, &key);
	}
	else
		scan = heap_beginscan(pgstatistic, false, SnapshotNow, 0, NULL);

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
		heap_delete(pgstatistic, &tuple->t_ctid);
	}

	heap_endscan(scan);
	heap_close(pgstatistic);
}

static void
vc_setpagelock(Relation rel, BlockNumber blkno)
{
	ItemPointerData itm;

	ItemPointerSet(&itm, blkno, 1);

	RelationSetLockForWritePage(rel, &itm);
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

	/* allocate a VPageDescr entry if needed */
	if (vpl->vpl_num_pages == 0)
		vpl->vpl_pagedesc = (VPageDescr *) palloc(100 * sizeof(VPageDescr));
	else if (vpl->vpl_num_pages % 100 == 0)
		vpl->vpl_pagedesc = (VPageDescr *) repalloc(vpl->vpl_pagedesc, (vpl->vpl_num_pages + 100) * sizeof(VPageDescr));
	vpl->vpl_pagedesc[vpl->vpl_num_pages] = vpnew;
	(vpl->vpl_num_pages)++;

}

static void
vc_free(VRelList vrl)
{
	VRelList	p_vrl;
	MemoryContext old;
	PortalVariableMemory pmem;

	pmem = PortalGetVariableMemory(vc_portal);
	old = MemoryContextSwitchTo((MemoryContext) pmem);

	while (vrl != (VRelList) NULL)
	{

		/* free rel list entry */
		p_vrl = vrl;
		vrl = vrl->vrl_next;
		pfree(p_vrl);
	}

	MemoryContextSwitchTo(old);
}

static char *
vc_find_eq(char *bot, int nelem, int size, char *elm, int (*compar) (char *, char *))
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
			res = compar(elm, bot + last * size);
			if (res > 0)
				return NULL;
			if (res == 0)
				return bot + last * size;
			last_move = false;
		}
		res = compar(elm, bot + celm * size);
		if (res == 0)
			return bot + celm * size;
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
		bot = bot + (celm + 1) * size;
		celm = (last + 1) / 2;
		first_move = true;
	}

}	/* vc_find_eq */

static int
vc_cmp_blk(char *left, char *right)
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
vc_cmp_offno(char *left, char *right)
{

	if (*(OffsetNumber *) left < *(OffsetNumber *) right)
		return -1;
	if (*(OffsetNumber *) left == *(OffsetNumber *) right)
		return 0;
	return 1;

}	/* vc_cmp_offno */


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
	pgindex = heap_openr(IndexRelationName);
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
	heap_close(pgindex);

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

	len = DOUBLEALIGN(len);

	if (len > vpd->vpd_free)
		return false;

	if (vpd->vpd_offsets_used < vpd->vpd_offsets_free)	/* there are free
														 * itemid(s) */
		return true;			/* and len <= free_space */

	/* ok. noff_usd >= noff_free and so we'll have to allocate new itemid */
	if (len <= vpd->vpd_free - sizeof(ItemIdData))
		return true;

	return false;

}	/* vc_enough_space */
