/*-------------------------------------------------------------------------
 *
 * vacuum.c--
 *    the postgres vacuum cleaner
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/vacuum.c,v 1.4 1996/10/18 08:13:36 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/file.h>

#include "postgres.h"
#include "utils/portal.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "access/transam.h"
#include "utils/tqual.h"
#include "access/htup.h"

#include "catalog/pg_index.h"
#include "catalog/catname.h"
#include "catalog/pg_class.h"
#include "catalog/pg_proc.h"

#include "storage/fd.h"		/* for O_ */
#include "storage/itemid.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/smgr.h"

#include "utils/elog.h"
#include "utils/mcxt.h"
#include "utils/palloc.h"

#include "commands/vacuum.h"

#ifdef NEED_RUSAGE
#include "rusagestub.h"
#else /* NEED_RUSAGE */
#include <sys/time.h>
#include <sys/resource.h>
#endif /* NEED_RUSAGE */

bool VacuumRunning =	false;

/* non-export function prototypes */
static void _vc_init(void);
static void _vc_shutdown(void);
static void _vc_vacuum(NameData *VacRelP);
static VRelList _vc_getrels(Portal p, NameData *VacRelP);
static void _vc_vacone(Portal p, VRelList curvrl);
static void _vc_vacheap(Portal p, VRelList curvrl, Relation onerel);
static void _vc_vacindices(VRelList curvrl, Relation onerel);
static void _vc_vaconeind(VRelList curvrl, Relation indrel);
static void _vc_updstats(Oid relid, int npages, int ntuples, bool hasindex);
static void _vc_setpagelock(Relation rel, BlockNumber blkno);
static bool _vc_tidreapped(ItemPointer itemptr, VRelList curvrl, Relation indrel);
static void _vc_reappage(Portal p, VRelList curvrl, VPageDescr vpc);
static void _vc_free(Portal p, VRelList vrl);
static Relation _vc_getarchrel(Relation heaprel);
static void _vc_archive(Relation archrel, HeapTuple htup);
static bool _vc_isarchrel(char *rname);
static char * _vc_find_eq (char *bot, int nelem, int size, char *elm, int (*compar)(char *, char *));
static int _vc_cmp_blk (char *left, char *right);
static int _vc_cmp_offno (char *left, char *right);

void
vacuum(char *vacrel)
{
    NameData VacRel;

    /* vacrel gets de-allocated on transaction commit */
    	
    /* initialize vacuum cleaner */
    _vc_init();

    /* vacuum the database */
    if (vacrel)
    {
	strcpy(VacRel.data,vacrel);
	_vc_vacuum(&VacRel);
    }
    else
	_vc_vacuum(NULL);

    /* clean up */
    _vc_shutdown();
}

/*
 *  _vc_init(), _vc_shutdown() -- start up and shut down the vacuum cleaner.
 *
 *	We run exactly one vacuum cleaner at a time.  We use the file system
 *	to guarantee an exclusive lock on vacuuming, since a single vacuum
 *	cleaner instantiation crosses transaction boundaries, and we'd lose
 *	postgres-style locks at the end of every transaction.
 *
 *	The strangeness with committing and starting transactions in the
 *	init and shutdown routines is due to the fact that the vacuum cleaner
 *	is invoked via a sql command, and so is already executing inside
 *	a transaction.  We need to leave ourselves in a predictable state
 *	on entry and exit to the vacuum cleaner.  We commit the transaction
 *	started in PostgresMain() inside _vc_init(), and start one in
 *	_vc_shutdown() to match the commit waiting for us back in
 *	PostgresMain().
 */
static void
_vc_init()
{
    int fd;

    if ((fd = open("pg_vlock", O_CREAT|O_EXCL, 0600)) < 0)
	elog(WARN, "can't create lock file -- another vacuum cleaner running?");

    close(fd);

    /*
     *  By here, exclusive open on the lock file succeeded.  If we abort
     *  for any reason during vacuuming, we need to remove the lock file.
     *  This global variable is checked in the transaction manager on xact
     *  abort, and the routine vc_abort() is called if necessary.
     */

    VacuumRunning = true;

    /* matches the StartTransaction in PostgresMain() */
    CommitTransactionCommand();
}

static void
_vc_shutdown()
{
    /* on entry, not in a transaction */
    if (unlink("pg_vlock") < 0)
	elog(WARN, "vacuum: can't destroy lock file!");

    /* okay, we're done */
    VacuumRunning = false;

    /* matches the CommitTransaction in PostgresMain() */
    StartTransactionCommand();
}

void
vc_abort()
{
    /* on abort, remove the vacuum cleaner lock file */
    (void) unlink("pg_vlock");

    VacuumRunning = false;
}

/*
 *  _vc_vacuum() -- vacuum the database.
 *
 *	This routine builds a list of relations to vacuum, and then calls
 *	code that vacuums them one at a time.  We are careful to vacuum each
 *	relation in a separate transaction in order to avoid holding too many
 *	locks at one time.
 */
static void
_vc_vacuum(NameData *VacRelP)
{
    VRelList vrl, cur;
    char *pname;
    Portal p;

    /*
     *  Create a portal for safe memory across transctions.  We need to
     *  palloc the name space for it because our hash function expects
     *	the name to be on a longword boundary.  CreatePortal copies the
     *  name to safe storage for us.
     */

    pname = (char *) palloc(strlen(VACPNAME) + 1);
    strcpy(pname, VACPNAME);
    p = CreatePortal(pname);
    pfree(pname);

    /* get list of relations */
    vrl = _vc_getrels(p, VacRelP);

    /* vacuum each heap relation */
    for (cur = vrl; cur != (VRelList) NULL; cur = cur->vrl_next)
	_vc_vacone(p, cur);

    _vc_free(p, vrl);

    PortalDestroy(&p);
}

static VRelList
_vc_getrels(Portal p, NameData *VacRelP)
{
    Relation pgclass;
    TupleDesc pgcdesc;
    HeapScanDesc pgcscan;
    HeapTuple pgctup;
    Buffer buf;
    PortalVariableMemory portalmem;
    MemoryContext old;
    VRelList vrl, cur;
    Datum d;
    char *rname;
    char rkind;
    int16 smgrno;
    bool n;
    ScanKeyData  pgckey;
    bool found = false;

    StartTransactionCommand();

    if (VacRelP->data) {
	ScanKeyEntryInitialize(&pgckey, 0x0, Anum_pg_class_relname,
			       NameEqualRegProcedure, 
			       PointerGetDatum(VacRelP->data));
    } else {
	ScanKeyEntryInitialize(&pgckey, 0x0, Anum_pg_class_relkind,
			       CharacterEqualRegProcedure, CharGetDatum('r'));
    }

    portalmem = PortalGetVariableMemory(p);
    vrl = (VRelList) NULL;

    pgclass = heap_openr(RelationRelationName);
    pgcdesc = RelationGetTupleDescriptor(pgclass);

    pgcscan = heap_beginscan(pgclass, false, NowTimeQual, 1, &pgckey);

    while (HeapTupleIsValid(pgctup = heap_getnext(pgcscan, 0, &buf))) {

	found = true;
	
	/*
	 *  We have to be careful not to vacuum the archive (since it
	 *  already contains vacuumed tuples), and not to vacuum
	 *  relations on write-once storage managers like the Sony
	 *  jukebox at Berkeley.
	 */

	d = (Datum) heap_getattr(pgctup, buf, Anum_pg_class_relname,
				 pgcdesc, &n);
	rname = (char*)d;

	/* skip archive relations */
	if (_vc_isarchrel(rname)) {
	    ReleaseBuffer(buf);
	    continue;
	}

	d = (Datum) heap_getattr(pgctup, buf, Anum_pg_class_relsmgr,
				 pgcdesc, &n);
	smgrno = DatumGetInt16(d);

	/* skip write-once storage managers */
	if (smgriswo(smgrno)) {
	    ReleaseBuffer(buf);
	    continue;
	}

	d = (Datum) heap_getattr(pgctup, buf, Anum_pg_class_relkind,
				 pgcdesc, &n);

	rkind = DatumGetChar(d);

	/* skip system relations */
	if (rkind != 'r') {
	    ReleaseBuffer(buf);
	    elog(NOTICE, "Vacuum: can not process index and certain system tables" );
	    continue;
	}
				 
	/* get a relation list entry for this guy */
	old = MemoryContextSwitchTo((MemoryContext)portalmem);
	if (vrl == (VRelList) NULL) {
	    vrl = cur = (VRelList) palloc(sizeof(VRelListData));
	} else {
	    cur->vrl_next = (VRelList) palloc(sizeof(VRelListData));
	    cur = cur->vrl_next;
	}
	(void) MemoryContextSwitchTo(old);

	cur->vrl_relid = pgctup->t_oid;
	cur->vrl_attlist = (VAttList) NULL;
	cur->vrl_pgdsc = (VPageDescr*) NULL;
	cur->vrl_nrepg = 0;
	cur->vrl_npages = cur->vrl_ntups = 0;
	cur->vrl_hasindex = false;
	cur->vrl_next = (VRelList) NULL;

	/* wei hates it if you forget to do this */
	ReleaseBuffer(buf);
    }
    if (found == false)
    	elog(NOTICE, "Vacuum: table not found" );

    
    heap_close(pgclass);
    heap_endscan(pgcscan);

    CommitTransactionCommand();

    return (vrl);
}

/*
 *  _vc_vacone() -- vacuum one heap relation
 *
 *	This routine vacuums a single heap, cleans out its indices, and
 *	updates its statistics npages and ntuples statistics.
 *
 *	Doing one heap at a time incurs extra overhead, since we need to
 *	check that the heap exists again just before we vacuum it.  The
 *	reason that we do this is so that vacuuming can be spread across
 *	many small transactions.  Otherwise, two-phase locking would require
 *	us to lock the entire database during one pass of the vacuum cleaner.
 */
static void
_vc_vacone(Portal p, VRelList curvrl)
{
    Relation pgclass;
    TupleDesc pgcdesc;
    HeapTuple pgctup;
    Buffer pgcbuf;
    HeapScanDesc pgcscan;
    Relation onerel;
    ScanKeyData pgckey;

    StartTransactionCommand();

    ScanKeyEntryInitialize(&pgckey, 0x0, ObjectIdAttributeNumber,
			   ObjectIdEqualRegProcedure,
			   ObjectIdGetDatum(curvrl->vrl_relid));

    pgclass = heap_openr(RelationRelationName);
    pgcdesc = RelationGetTupleDescriptor(pgclass);
    pgcscan = heap_beginscan(pgclass, false, NowTimeQual, 1, &pgckey);

    /*
     *  Race condition -- if the pg_class tuple has gone away since the
     *  last time we saw it, we don't need to vacuum it.
     */

    if (!HeapTupleIsValid(pgctup = heap_getnext(pgcscan, 0, &pgcbuf))) {
	heap_endscan(pgcscan);
	heap_close(pgclass);
	CommitTransactionCommand();
	return;
    }

    /* now open the class and vacuum it */
    onerel = heap_open(curvrl->vrl_relid);

    /* we require the relation to be locked until the indices are cleaned */
    RelationSetLockForWrite(onerel);

    /* vacuum it */
    _vc_vacheap(p, curvrl, onerel);

    /* if we vacuumed any heap tuples, vacuum the indices too */
    if (curvrl->vrl_nrepg > 0)
	_vc_vacindices(curvrl, onerel);
    else
	curvrl->vrl_hasindex = onerel->rd_rel->relhasindex;

    /* all done with this class */
    heap_close(onerel);
    heap_endscan(pgcscan);
    heap_close(pgclass);

    /* update statistics in pg_class */
    _vc_updstats(curvrl->vrl_relid, curvrl->vrl_npages, curvrl->vrl_ntups,
		 curvrl->vrl_hasindex);

    CommitTransactionCommand();
}

# define	ADJUST_FREE_SPACE(S)\
	((DOUBLEALIGN(S) == (S)) ? (S) : (DOUBLEALIGN(S) - sizeof(double)))

/*
 *  _vc_vacheap() -- vacuum an open heap relation
 *
 *	This routine sets commit times, vacuums dead tuples, cleans up
 *	wasted space on the page, and maintains statistics on the number
 *	of live tuples in a heap.  In addition, it records the tids of
 *	all tuples removed from the heap for any reason.  These tids are
 *	used in a scan of indices on the relation to get rid of dead
 *	index tuples.
 */
static void
_vc_vacheap(Portal p, VRelList curvrl, Relation onerel)
{
    int nblocks, blkno;
    ItemId itemid;
    HeapTuple htup;
    Buffer buf;
    Page page;
    OffsetNumber offnum, maxoff;
    Relation archrel;
    bool isarchived;
    int nvac;
    int ntups;
    bool pgchanged, tupgone;
    AbsoluteTime purgetime, expiretime;
    RelativeTime preservetime;
    char *relname;
    VPageDescr	vpc;
    uint32 nunused, nempg, nnepg, nchpg;
    Size frsize;
    struct rusage ru0, ru1;

    getrusage(RUSAGE_SELF, &ru0);

    relname = (RelationGetRelationName(onerel))->data;

    nvac = 0;
    ntups = 0;
    nunused = nempg = nnepg = nchpg = 0;
    frsize = 0;
    curvrl->vrl_nrepg = 0;
    
    nblocks = RelationGetNumberOfBlocks(onerel);

    /* if the relation has an archive, open it */
    if (onerel->rd_rel->relarch != 'n') {
	isarchived = true;
	archrel = _vc_getarchrel(onerel);
    } else
	isarchived = false;

    /* don't vacuum large objects for now.
       something breaks when we do*/
    if ( (strlen(relname) > 4) && 
	relname[0] == 'X' &&
	relname[1] == 'i' &&
	relname[2] == 'n' &&
	(relname[3] == 'v' || relname[3] == 'x'))
	    return;

    /* calculate the purge time: tuples that expired before this time
       will be archived or deleted */
    purgetime = GetCurrentTransactionStartTime();
    expiretime = (AbsoluteTime)onerel->rd_rel->relexpires;
    preservetime = (RelativeTime)onerel->rd_rel->relpreserved;

    if (RelativeTimeIsValid(preservetime) && (preservetime)) {
	purgetime -= preservetime;
	if (AbsoluteTimeIsBackwardCompatiblyValid(expiretime) &&
	    expiretime > purgetime)
	    purgetime = expiretime;
    }

    else if (AbsoluteTimeIsBackwardCompatiblyValid(expiretime))
	purgetime = expiretime;

    vpc = (VPageDescr) palloc (sizeof(VPageDescrData) + MaxOffsetNumber*sizeof(OffsetNumber));
	    
    for (blkno = 0; blkno < nblocks; blkno++) {
	buf = ReadBuffer(onerel, blkno);
	page = BufferGetPage(buf);
	vpc->vpd_blkno = blkno;
	vpc->vpd_noff = 0;

	if (PageIsNew(page)) {
	    elog (NOTICE, "Rel %.*s: Uninitialized page %u - fixing",
		NAMEDATALEN, relname, blkno);
	    PageInit (page, BufferGetPageSize (buf), 0);
	    vpc->vpd_free = PageGetFreeSpace (page);
	    vpc->vpd_free = ADJUST_FREE_SPACE (vpc->vpd_free);
	    frsize += vpc->vpd_free;
	    nnepg++;
	    _vc_reappage(p, curvrl, vpc);	/* No one index should point here */
	    WriteBuffer(buf);
	    continue;
	}

	if (PageIsEmpty(page)) {
	    vpc->vpd_free = PageGetFreeSpace (page);
	    vpc->vpd_free = ADJUST_FREE_SPACE (vpc->vpd_free);
	    frsize += vpc->vpd_free;
	    nempg++;
	    _vc_reappage(p, curvrl, vpc);	/* No one index should point here */
	    ReleaseBuffer(buf);
	    continue;
	}

	pgchanged = false;
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
	     offnum <= maxoff;
	     offnum = OffsetNumberNext(offnum)) {
	    itemid = PageGetItemId(page, offnum);

	    if (!ItemIdIsUsed(itemid)) {
	    	vpc->vpd_voff[vpc->vpd_noff++] = offnum;
		nunused++;
		continue;
	    }

	    htup = (HeapTuple) PageGetItem(page, itemid);
	    tupgone = false;

	    if (!AbsoluteTimeIsBackwardCompatiblyValid(htup->t_tmin) && 
		TransactionIdIsValid((TransactionId)htup->t_xmin)) {

		if (TransactionIdDidAbort(htup->t_xmin)) {
		    pgchanged = true;
		    tupgone = true;
		} else if (TransactionIdDidCommit(htup->t_xmin)) {
		    htup->t_tmin = TransactionIdGetCommitTime(htup->t_xmin);
		    pgchanged = true;
		}
	    }

	    if (TransactionIdIsValid((TransactionId)htup->t_xmax)) {
		if (TransactionIdDidAbort(htup->t_xmax)) {
		    StoreInvalidTransactionId(&(htup->t_xmax));
		    pgchanged = true;
		} else if (TransactionIdDidCommit(htup->t_xmax)) {
		    if (!AbsoluteTimeIsBackwardCompatiblyReal(htup->t_tmax)) {

			htup->t_tmax = TransactionIdGetCommitTime(htup->t_xmax);  
			pgchanged = true;
		    }

		    /*
		     *  Reap the dead tuple if its expiration time is
		     *  before purgetime.
		     */

		    if (htup->t_tmax < purgetime) {
			tupgone = true;
			pgchanged = true;
		    }
		}
	    }

	    if (tupgone) {
		ItemId lpp = &(((PageHeader) page)->pd_linp[offnum - 1]);

		/* write the tuple to the archive, if necessary */
		if (isarchived)
		    _vc_archive(archrel, htup);

		/* mark it unused */
		lpp->lp_flags &= ~LP_USED;

	    	vpc->vpd_voff[vpc->vpd_noff++] = offnum;

		++nvac;
	    } else {
		ntups++;
	    }
	}

	if (pgchanged) {
	    PageRepairFragmentation(page);
	    if ( vpc->vpd_noff > 0 ) {	/* there are some new unused tids here */
		vpc->vpd_free = PageGetFreeSpace (page);
		vpc->vpd_free = ADJUST_FREE_SPACE (vpc->vpd_free);
		frsize += vpc->vpd_free;
		_vc_reappage(p, curvrl, vpc);
	    }
	    WriteBuffer(buf);
	    nchpg++;
	} else {
	    if ( vpc->vpd_noff > 0 ) {	/* there are only old unused tids here */
		vpc->vpd_free = PageGetFreeSpace (page);
		vpc->vpd_free = ADJUST_FREE_SPACE (vpc->vpd_free);
		frsize += vpc->vpd_free;
		_vc_reappage(p, curvrl, vpc);
	    }
	    ReleaseBuffer(buf);
	}
    }

    if (isarchived)
	heap_close(archrel);

    /* save stats in the rel list for use later */
    curvrl->vrl_ntups = ntups;
    curvrl->vrl_npages = nblocks;

    getrusage(RUSAGE_SELF, &ru1);
    
    elog (NOTICE, "Rel %.*s: Pages %u: Changed %u, Reapped %u, Empty %u, New %u; \
Tuples %u: Vac %u, UnUsed %u; FreeSpace %u. Elapsed %u/%u sec.",
	NAMEDATALEN, relname, 
	nblocks, nchpg, curvrl->vrl_nrepg, nempg, nnepg, 
	ntups, nvac, nunused, frsize, 
	ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec, 
	ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

    pfree (vpc);
}

/*
 *  _vc_vacindices() -- vacuum all the indices for a particular heap relation.
 *
 *	On entry, curvrl points at the relation currently being vacuumed.
 *	We already have a write lock on the relation, so we don't need to
 *	worry about anyone building an index on it while we're doing the
 *	vacuuming.  The tid list for curvrl is sorted in reverse tid order:
 *	that is, tids on higher page numbers are before those on lower page
 *	numbers, and tids high on the page are before those low on the page.
 *	We use this ordering to cut down the search cost when we look at an
 *	index entry.
 *
 *	We're executing inside the transaction that vacuumed the heap.
 */
static void
_vc_vacindices(VRelList curvrl, Relation onerel)
{
    Relation pgindex;
    TupleDesc pgidesc;
    HeapTuple pgitup;
    HeapScanDesc pgiscan;
    Buffer buf;
    Relation indrel;
    Oid indoid;
    Datum d;
    bool n;
    int nindices;
    ScanKeyData pgikey;

    /* see if we can dodge doing any work at all */
    if (!(onerel->rd_rel->relhasindex))
	return;

    nindices = 0;

    /* prepare a heap scan on the pg_index relation */
    pgindex = heap_openr(IndexRelationName);
    pgidesc = RelationGetTupleDescriptor(pgindex);

    ScanKeyEntryInitialize(&pgikey, 0x0, Anum_pg_index_indrelid,
			   ObjectIdEqualRegProcedure,
			   ObjectIdGetDatum(curvrl->vrl_relid));

    pgiscan = heap_beginscan(pgindex, false, NowTimeQual, 1, &pgikey);

    /* vacuum all the indices */
    while (HeapTupleIsValid(pgitup = heap_getnext(pgiscan, 0, &buf))) {
	d = (Datum) heap_getattr(pgitup, buf, Anum_pg_index_indexrelid,
				 pgidesc, &n);
	indoid = DatumGetObjectId(d);
	indrel = index_open(indoid);
	_vc_vaconeind(curvrl, indrel);
	heap_close(indrel);
	nindices++;
    }

    heap_endscan(pgiscan);
    heap_close(pgindex);

    if (nindices > 0)
	curvrl->vrl_hasindex = true;
    else
	curvrl->vrl_hasindex = false;
}

/*
 *  _vc_vaconeind() -- vacuum one index relation.
 *
 *	Curvrl is the VRelList entry for the heap we're currently vacuuming.
 *	It's locked. Onerel is an index relation on the vacuumed heap. 
 *	We don't set locks on the index	relation here, since the indexed 
 *	access methods support locking at different granularities. 
 *	We let them handle it.
 *
 *	Finally, we arrange to update the index relation's statistics in
 *	pg_class.
 */
static void
_vc_vaconeind(VRelList curvrl, Relation indrel)
{
    RetrieveIndexResult res;
    IndexScanDesc iscan;
    ItemPointer heapptr;
    int nvac;
    int nitups;
    int nipages;
    struct rusage ru0, ru1;

    getrusage(RUSAGE_SELF, &ru0);

    /* walk through the entire index */
    iscan = index_beginscan(indrel, false, 0, (ScanKey) NULL);
    nvac = 0;
    nitups = 0;

    while ((res = index_getnext(iscan, ForwardScanDirection))
	   != (RetrieveIndexResult) NULL) {
	heapptr = &res->heap_iptr;

	if (_vc_tidreapped(heapptr, curvrl, indrel)) {
#if 0
	    elog(DEBUG, "<%x,%x> -> <%x,%x>",
		 ItemPointerGetBlockNumber(&(res->index_iptr)),
		 ItemPointerGetOffsetNumber(&(res->index_iptr)),
		 ItemPointerGetBlockNumber(&(res->heap_iptr)),
		 ItemPointerGetOffsetNumber(&(res->heap_iptr)));
#endif
	    ++nvac;
	    index_delete(indrel, &res->index_iptr);
	} else {
	    nitups++;
	}

	/* be tidy */
	pfree(res);
    }

    index_endscan(iscan);

    /* now update statistics in pg_class */
    nipages = RelationGetNumberOfBlocks(indrel);
    _vc_updstats(indrel->rd_id, nipages, nitups, false);

    getrusage(RUSAGE_SELF, &ru1);

    elog (NOTICE, "Ind %.*s: Pages %u; Tuples %u: Deleted %u. Elapsed %u/%u sec.",
	NAMEDATALEN, indrel->rd_rel->relname.data, nipages, nitups, nvac,
	ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec, 
	ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

}

/*
 *  _vc_updstats() -- update pg_class statistics for one relation
 *
 *	This routine works for both index and heap relation entries in
 *	pg_class.  We violate no-overwrite semantics here by storing new
 *	values for ntuples, npages, and hasindex directly in the pg_class
 *	tuple that's already on the page.  The reason for this is that if
 *	we updated these tuples in the usual way, then every tuple in pg_class
 *	would be replaced every day.  This would make planning and executing
 *	historical queries very expensive.
 */
static void
_vc_updstats(Oid relid, int npages, int ntuples, bool hasindex)
{
    Relation rd;
    HeapScanDesc sdesc;
    HeapTuple tup;
    Buffer buf;
    Form_pg_class pgcform;
    ScanKeyData skey;

    /*
     * update number of tuples and number of pages in pg_class
     */
    ScanKeyEntryInitialize(&skey, 0x0, ObjectIdAttributeNumber,
			   ObjectIdEqualRegProcedure,
			   ObjectIdGetDatum(relid));

    rd = heap_openr(RelationRelationName);
    sdesc = heap_beginscan(rd, false, NowTimeQual, 1, &skey);

    if (!HeapTupleIsValid(tup = heap_getnext(sdesc, 0, &buf)))
	elog(WARN, "pg_class entry for relid %d vanished during vacuuming",
		   relid);

    /* overwrite the existing statistics in the tuple */
    _vc_setpagelock(rd, BufferGetBlockNumber(buf));
    pgcform = (Form_pg_class) GETSTRUCT(tup);
    pgcform->reltuples = ntuples;
    pgcform->relpages = npages;
    pgcform->relhasindex = hasindex;
 
    /* XXX -- after write, should invalidate relcache in other backends */
    WriteNoReleaseBuffer(buf);

    /* that's all, folks */
    heap_endscan(sdesc);
    heap_close(rd);

}

static void _vc_setpagelock(Relation rel, BlockNumber blkno)
{
    ItemPointerData itm;

    ItemPointerSet(&itm, blkno, 1);

    RelationSetLockForWritePage(rel, &itm);
}

/*
 *  _vc_tidreapped() -- is a particular tid reapped?
 *
 *	VPageDescr array is sorted in right order.
 */
static bool
_vc_tidreapped(ItemPointer itemptr, VRelList curvrl, Relation indrel)
{
    OffsetNumber ioffno;
    OffsetNumber *voff;
    VPageDescr vp, *vpp;
    VPageDescrData vpd;

    vpd.vpd_blkno = ItemPointerGetBlockNumber(itemptr);
    ioffno = ItemPointerGetOffsetNumber(itemptr);
	
    vp = &vpd;
    vpp = (VPageDescr*) _vc_find_eq ((char*)(curvrl->vrl_pgdsc), 
		curvrl->vrl_nrepg, sizeof (VPageDescr), (char*)&vp, 
		_vc_cmp_blk);

    if ( vpp == (VPageDescr*) NULL )
	return (false);
    vp = *vpp;

    /* ok - we are on true page */

    if ( vp->vpd_noff == 0 ) {		/* this is EmptyPage !!! */
	elog (NOTICE, "Ind %.*s: pointer to EmptyPage (blk %u off %u) - fixing",
		NAMEDATALEN, indrel->rd_rel->relname.data,
	    	vpd.vpd_blkno, ioffno);
	return (true);
    }
    
    voff = (OffsetNumber*) _vc_find_eq ((char*)(vp->vpd_voff), 
		vp->vpd_noff, sizeof (OffsetNumber), (char*)&ioffno, 
		_vc_cmp_offno);

    if ( voff == (OffsetNumber*) NULL )
	return (false);

    return (true);

} /* _vc_tidreapped */


/*
 *  _vc_reappage() -- save a page on the array of reaped pages for the current
 *		     entry on the vacuum relation list.
 *
 *	As a side effect of the way that the vacuuming loop for a given
 *	relation works, higher pages come after lower pages in the array
 *	(and highest tid on a page is last).
 */
static void
_vc_reappage(Portal p,
	    VRelList curvrl,
	    VPageDescr vpc)
{
    PortalVariableMemory pmem;
    MemoryContext old;
    VPageDescr newvpd;

    /* allocate a VPageDescrData entry in the portal memory context */
    pmem = PortalGetVariableMemory(p);
    old = MemoryContextSwitchTo((MemoryContext) pmem);
    if ( curvrl->vrl_nrepg == 0 )
    	curvrl->vrl_pgdsc = (VPageDescr*) palloc(100*sizeof(VPageDescr));
    else if ( curvrl->vrl_nrepg % 100 == 0 )
    	curvrl->vrl_pgdsc = (VPageDescr*) repalloc(curvrl->vrl_pgdsc, (curvrl->vrl_nrepg+100)*sizeof(VPageDescr));
    newvpd = (VPageDescr) palloc(sizeof(VPageDescrData) + vpc->vpd_noff*sizeof(OffsetNumber));
    MemoryContextSwitchTo(old);

    /* fill it in */
    if ( vpc->vpd_noff > 0 )
    	memmove (newvpd->vpd_voff, vpc->vpd_voff, vpc->vpd_noff*sizeof(OffsetNumber));
    newvpd->vpd_blkno = vpc->vpd_blkno;
    newvpd->vpd_free = vpc->vpd_free;
    newvpd->vpd_noff = vpc->vpd_noff;

    curvrl->vrl_pgdsc[curvrl->vrl_nrepg] = newvpd;
    (curvrl->vrl_nrepg)++;
}

static void
_vc_free(Portal p, VRelList vrl)
{
    VRelList p_vrl;
    VAttList p_val, val;
    VPageDescr p_vpd, *vpd;
    int i;
    MemoryContext old;
    PortalVariableMemory pmem;

    pmem = PortalGetVariableMemory(p);
    old = MemoryContextSwitchTo((MemoryContext)pmem);

    while (vrl != (VRelList) NULL) {

	/* free attribute list */
	val = vrl->vrl_attlist;
	while (val != (VAttList) NULL) {
	    p_val = val;
	    val = val->val_next;
	    pfree(p_val);
	}

	/* free page' descriptions */
	if ( vrl->vrl_nrepg > 0 ) {
	    vpd = vrl->vrl_pgdsc;
	    for (i = 0; i < vrl->vrl_nrepg; i++) {
		pfree(vpd[i]);
	    }
	    pfree (vpd);
	}

	/* free rel list entry */
	p_vrl = vrl;
	vrl = vrl->vrl_next;
	pfree(p_vrl);
    }

    (void) MemoryContextSwitchTo(old);
}

/*
 *  _vc_getarchrel() -- open the archive relation for a heap relation
 *
 *	The archive relation is named 'a,XXXXX' for the heap relation
 *	whose relid is XXXXX.
 */

#define ARCHIVE_PREFIX	"a,"

static Relation
_vc_getarchrel(Relation heaprel)
{
    Relation archrel;
    char *archrelname;

    archrelname = palloc(sizeof(ARCHIVE_PREFIX) + NAMEDATALEN); /* bogus */
    sprintf(archrelname, "%s%d", ARCHIVE_PREFIX, heaprel->rd_id);

    archrel = heap_openr(archrelname);

    pfree(archrelname);
    return (archrel);
}

/*
 *  _vc_archive() -- write a tuple to an archive relation
 *
 *	In the future, this will invoke the archived accessd method.  For
 *	now, archive relations are on mag disk.
 */
static void
_vc_archive(Relation archrel, HeapTuple htup)
{
    doinsert(archrel, htup);
}

static bool
_vc_isarchrel(char *rname)
{
    if (strncmp(ARCHIVE_PREFIX, rname,strlen(ARCHIVE_PREFIX)) == 0)
	return (true);

    return (false);
}

static char *
_vc_find_eq (char *bot, int nelem, int size, char *elm, int (*compar)(char *, char *))
{
    int res;
    int last = nelem - 1;
    int celm = nelem / 2;
    bool last_move, first_move;
    
    last_move = first_move = true;
    for ( ; ; )
    {
	if ( first_move == true )
	{
	    res = compar (bot, elm);
	    if ( res > 0 )
		return (NULL);
	    if ( res == 0 )
		return (bot);
	    first_move = false;
	}
	if ( last_move == true )
	{
	    res = compar (elm, bot + last*size);
	    if ( res > 0 )
		return (NULL);
	    if ( res == 0 )
		return (bot + last*size);
	    last_move = false;
	}
    	res = compar (elm, bot + celm*size);
    	if ( res == 0 )
	    return (bot + celm*size);
	if ( res < 0 )
	{
	    if ( celm == 0 )
		return (NULL);
	    last = celm - 1;
	    celm = celm / 2;
	    last_move = true;
	    continue;
	}
	
	if ( celm == last )
	    return (NULL);
	    
	last = last - celm - 1;
	bot = bot + (celm+1)*size;
	celm = (last + 1) / 2;
	first_move = true;
    }

} /* _vc_find_eq */

static int 
_vc_cmp_blk (char *left, char *right)
{
    BlockNumber lblk, rblk;

    lblk = (*((VPageDescr*)left))->vpd_blkno;
    rblk = (*((VPageDescr*)right))->vpd_blkno;

    if ( lblk < rblk )
    	return (-1);
    if ( lblk == rblk )
    	return (0);
    return (1);

} /* _vc_cmp_blk */

static int 
_vc_cmp_offno (char *left, char *right)
{

    if ( *(OffsetNumber*)left < *(OffsetNumber*)right )
    	return (-1);
    if ( *(OffsetNumber*)left == *(OffsetNumber*)right )
    	return (0);
    return (1);

} /* _vc_cmp_offno */
