/*-------------------------------------------------------------------------
 *
 * vacuum.c--
 *    the postgres vacuum cleaner
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/commands/vacuum.c,v 1.9 1996/11/27 07:27:20 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/file.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <postgres.h>

#include <utils/portal.h>
#include <access/genam.h>
#include <access/heapam.h>
#include <access/xact.h>
#include <storage/bufmgr.h>
#include <access/transam.h>
#include <catalog/pg_index.h>
#include <catalog/index.h>
#include <catalog/catname.h>
#include <catalog/pg_class.h>
#include <catalog/pg_proc.h>
#include <storage/smgr.h>
#include <storage/lmgr.h>
#include <utils/mcxt.h>
#include <utils/syscache.h>
#include <commands/vacuum.h>
#include <storage/bufpage.h>
#include "storage/shmem.h"
#ifdef NEED_RUSAGE
# include <rusagestub.h>
#else /* NEED_RUSAGE */
# include <sys/time.h>
# include <sys/resource.h>
#endif /* NEED_RUSAGE */

bool VacuumRunning =	false;

#ifdef VACUUM_QUIET
static int MESSLEV = DEBUG;
#else
static int MESSLEV = NOTICE;
#endif

typedef struct {
    FuncIndexInfo	finfo;
    FuncIndexInfo	*finfoP;
    IndexTupleForm	tform;
    int			natts;
} IndDesc;

/* non-export function prototypes */
static void _vc_init(void);
static void _vc_shutdown(void);
static void _vc_vacuum(NameData *VacRelP);
static VRelList _vc_getrels(Portal p, NameData *VacRelP);
static void _vc_vacone (VRelList curvrl);
static void _vc_scanheap (VRelList curvrl, Relation onerel, VPageList Vvpl, VPageList Fvpl);
static void _vc_rpfheap (VRelList curvrl, Relation onerel, VPageList Vvpl, VPageList Fvpl, int nindices, Relation *Irel);
static void _vc_vacheap (VRelList curvrl, Relation onerel, VPageList vpl);
static void _vc_vacpage (Page page, VPageDescr vpd, Relation archrel);
static void _vc_vaconeind (VPageList vpl, Relation indrel, int nhtups);
static void _vc_updstats(Oid relid, int npages, int ntuples, bool hasindex);
static void _vc_setpagelock(Relation rel, BlockNumber blkno);
static VPageDescr _vc_tidreapped (ItemPointer itemptr, VPageList curvrl);
static void _vc_reappage (VPageList vpl, VPageDescr vpc);
static void _vc_vpinsert (VPageList vpl, VPageDescr vpnew);
static void _vc_free(Portal p, VRelList vrl);
static void _vc_getindices (Oid relid, int *nindices, Relation **Irel);
static void _vc_clsindices (int nindices, Relation *Irel);
static Relation _vc_getarchrel(Relation heaprel);
static void _vc_archive(Relation archrel, HeapTuple htup);
static bool _vc_isarchrel(char *rname);
static void _vc_mkindesc (Relation onerel, int nindices, Relation *Irel, IndDesc **Idesc);
static char * _vc_find_eq (char *bot, int nelem, int size, char *elm, int (*compar)(char *, char *));
static int _vc_cmp_blk (char *left, char *right);
static int _vc_cmp_offno (char *left, char *right);
static bool _vc_enough_space (VPageDescr vpd, Size len);


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
	_vc_vacone (cur);

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

	/* don't vacuum large objects for now - something breaks when we do */
	if ( (strlen(rname) > 4) && rname[0] == 'X' &&
		rname[1] == 'i' && rname[2] == 'n' &&
		(rname[3] == 'v' || rname[3] == 'x'))
	{
	    elog (NOTICE, "Rel %.*s: can't vacuum LargeObjects now", 
			NAMEDATALEN, rname);
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
_vc_vacone (VRelList curvrl)
{
    Relation pgclass;
    TupleDesc pgcdesc;
    HeapTuple pgctup;
    Buffer pgcbuf;
    HeapScanDesc pgcscan;
    Relation onerel;
    ScanKeyData pgckey;
    VPageListData Vvpl;	/* List of pages to vacuum and/or clean indices */
    VPageListData Fvpl;	/* List of pages with space enough for re-using */
    VPageDescr *vpp;
    Relation *Irel;
    int nindices;
    int i;

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

    /* scan it */
    Vvpl.vpl_npages = Fvpl.vpl_npages = 0;
    _vc_scanheap(curvrl, onerel, &Vvpl, &Fvpl);

    /* Now open/count indices */
    Irel = (Relation *) NULL;
    if ( Vvpl.vpl_npages > 0 )
		    /* Open all indices of this relation */
	_vc_getindices(curvrl->vrl_relid, &nindices, &Irel);
    else
		    /* Count indices only */
	_vc_getindices(curvrl->vrl_relid, &nindices, NULL);
    
    if ( nindices > 0 )
	curvrl->vrl_hasindex = true;
    else
	curvrl->vrl_hasindex = false;

    /* Clean index' relation(s) */
    if ( Irel != (Relation*) NULL )
    {
	for (i = 0; i < nindices; i++)
	    _vc_vaconeind (&Vvpl, Irel[i], curvrl->vrl_ntups);
    }

    if ( Fvpl.vpl_npages > 0 )		/* Try to shrink heap */
    	_vc_rpfheap (curvrl, onerel, &Vvpl, &Fvpl, nindices, Irel);
    else if ( Vvpl.vpl_npages > 0 )	/* Clean pages from Vvpl list */
	_vc_vacheap (curvrl, onerel, &Vvpl);

    /* ok - free Vvpl list of reapped pages */
    if ( Vvpl.vpl_npages > 0 )
    {
    	vpp = Vvpl.vpl_pgdesc;
    	for (i = 0; i < Vvpl.vpl_npages; i++, vpp++)
	    pfree(*vpp);
    	pfree (Vvpl.vpl_pgdesc);
    	if ( Fvpl.vpl_npages > 0 )
	    pfree (Fvpl.vpl_pgdesc);
    }

    /* all done with this class */
    heap_close(onerel);
    heap_endscan(pgcscan);
    heap_close(pgclass);

    /* update statistics in pg_class */
    _vc_updstats(curvrl->vrl_relid, curvrl->vrl_npages, curvrl->vrl_ntups,
		 curvrl->vrl_hasindex);

    CommitTransactionCommand();
}

/*
 *  _vc_scanheap() -- scan an open heap relation
 *
 *	This routine sets commit times, constructs Vvpl list of 
 *	empty/uninitialized pages and pages with dead tuples and
 *	~LP_USED line pointers, constructs Fvpl list of pages
 *	appropriate for purposes of shrinking and maintains statistics 
 *	on the number of live tuples in a heap.
 */
static void
_vc_scanheap (VRelList curvrl, Relation onerel, 
			VPageList Vvpl, VPageList Fvpl)
{
    int nblocks, blkno;
    ItemId itemid;
    HeapTuple htup;
    Buffer buf;
    Page page, tempPage = NULL;
    OffsetNumber offnum, maxoff;
    bool pgchanged, tupgone, dobufrel, notup;
    AbsoluteTime purgetime, expiretime;
    RelativeTime preservetime;
    char *relname;
    VPageDescr vpc, vp;
    uint32 nvac, ntups, nunused, ncrash, nempg, nnepg, nchpg, nemend;
    Size frsize, frsusf;
    Size min_tlen = MAXTUPLEN;
    Size max_tlen = 0;
    int i;
    struct rusage ru0, ru1;

    getrusage(RUSAGE_SELF, &ru0);

    nvac = ntups = nunused = ncrash = nempg = nnepg = nchpg = nemend = 0;
    frsize = frsusf = 0;
    
    relname = (RelationGetRelationName(onerel))->data;

    nblocks = RelationGetNumberOfBlocks(onerel);

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
    vpc->vpd_nusd = 0;
	    
    for (blkno = 0; blkno < nblocks; blkno++) {
	buf = ReadBuffer(onerel, blkno);
	page = BufferGetPage(buf);
	vpc->vpd_blkno = blkno;
	vpc->vpd_noff = 0;
	vpc->vpd_noff = 0;

	if (PageIsNew(page)) {
	    elog (NOTICE, "Rel %.*s: Uninitialized page %u - fixing",
		NAMEDATALEN, relname, blkno);
	    PageInit (page, BufferGetPageSize (buf), 0);
	    vpc->vpd_free = ((PageHeader)page)->pd_upper - ((PageHeader)page)->pd_lower;
	    frsize += (vpc->vpd_free - sizeof (ItemIdData));
	    nnepg++;
	    nemend++;
	    _vc_reappage (Vvpl, vpc);
	    WriteBuffer(buf);
	    continue;
	}

	if (PageIsEmpty(page)) {
	    vpc->vpd_free = ((PageHeader)page)->pd_upper - ((PageHeader)page)->pd_lower;
	    frsize += (vpc->vpd_free - sizeof (ItemIdData));
	    nempg++;
	    nemend++;
	    _vc_reappage (Vvpl, vpc);
	    ReleaseBuffer(buf);
	    continue;
	}

	pgchanged = false;
	notup = true;
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
	     offnum <= maxoff;
	     offnum = OffsetNumberNext(offnum)) {
	    itemid = PageGetItemId(page, offnum);

	    /*
	     * Collect un-used items too - it's possible to have
	     * indices pointing here after crash.
	     */
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
		    tupgone = true;
		} else if (TransactionIdDidCommit(htup->t_xmin)) {
		    htup->t_tmin = TransactionIdGetCommitTime(htup->t_xmin);
		    pgchanged = true;
		} else if ( !TransactionIdIsInProgress (htup->t_xmin) ) {
		    /* 
		     * Not Aborted, Not Committed, Not in Progress -
		     * so it from crashed process. - vadim 11/26/96
		     */
		    ncrash++;
		    tupgone = true;
		}
		else {
		    elog (MESSLEV, "Rel %.*s: InsertTransactionInProgress %u for TID %u/%u",
			NAMEDATALEN, relname, htup->t_xmin, blkno, offnum);
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
		    }
		}
	    }

	    /*
	     * Is it possible at all ? - vadim 11/26/96
	     */
	    if ( !TransactionIdIsValid((TransactionId)htup->t_xmin) )
	    {
		elog (NOTICE, "TID %u/%u: INSERT_TRANSACTION_ID IS INVALID. \
DELETE_TRANSACTION_ID_VALID %d, TUPGONE %d.", 
			TransactionIdIsValid((TransactionId)htup->t_xmax),
			tupgone);
	    }
	    
	    if (tupgone) {
		ItemId lpp;
                                                    
		if ( tempPage == (Page) NULL )
		{
		    Size pageSize;
		    
		    pageSize = PageGetPageSize(page);
		    tempPage = (Page) palloc(pageSize);
		    memmove (tempPage, page, pageSize);
		}
		
		lpp = &(((PageHeader) tempPage)->pd_linp[offnum - 1]);

		/* mark it unused */
		lpp->lp_flags &= ~LP_USED;

	    	vpc->vpd_voff[vpc->vpd_noff++] = offnum;
	    	nvac++;

	    } else {
		ntups++;
		notup = false;
		if ( htup->t_len < min_tlen )
		    min_tlen = htup->t_len;
		if ( htup->t_len > max_tlen )
		    max_tlen = htup->t_len;
	    }
	}

	if (pgchanged) {
	    WriteBuffer(buf);
	    dobufrel = false;
	    nchpg++;
	}
	else
	    dobufrel = true;
	if ( tempPage != (Page) NULL )
	{ /* Some tuples are gone */
	    PageRepairFragmentation(tempPage);
	    vpc->vpd_free = ((PageHeader)tempPage)->pd_upper - ((PageHeader)tempPage)->pd_lower;
	    frsize += vpc->vpd_free;
	    _vc_reappage (Vvpl, vpc);
	    pfree (tempPage);
	    tempPage = (Page) NULL;
	}
	else if ( vpc->vpd_noff > 0 )
	{ /* there are only ~LP_USED line pointers */
	    vpc->vpd_free = ((PageHeader)page)->pd_upper - ((PageHeader)page)->pd_lower;
	    frsize += vpc->vpd_free;
	    _vc_reappage (Vvpl, vpc);
	}
	if ( dobufrel )
	    ReleaseBuffer(buf);
	if ( notup )
	    nemend++;
	else
	    nemend = 0;
    }

    pfree (vpc);

    /* save stats in the rel list for use later */
    curvrl->vrl_ntups = ntups;
    curvrl->vrl_npages = nblocks;
    
    Vvpl->vpl_nemend = nemend;
    Fvpl->vpl_nemend = nemend;

    /* 
     * Try to make Fvpl keeping in mind that we can't use free space 
     * of "empty" end-pages and last page if it reapped.
     */
    if ( Vvpl->vpl_npages - nemend > 0 )
    {
	int nusf;		/* blocks usefull for re-using */
	
	nusf = Vvpl->vpl_npages - nemend;
	if ( (Vvpl->vpl_pgdesc[nusf-1])->vpd_blkno == nblocks - nemend - 1 )
	    nusf--;
    
	for (i = 0; i < nusf; i++)
    	{
	    vp = Vvpl->vpl_pgdesc[i];
	    if ( _vc_enough_space (vp, min_tlen) )
	    {
		_vc_vpinsert (Fvpl, vp);
		frsusf += vp->vpd_free;
	    }
	}
    }

    getrusage(RUSAGE_SELF, &ru1);
    
    elog (MESSLEV, "Rel %.*s: Pages %u: Changed %u, Reapped %u, Empty %u, New %u; \
Tup %u: Vac %u, Crash %u, UnUsed %u, MinLen %u, MaxLen %u; Re-using: Free/Avail. Space %u/%u; EndEmpty/Avail. Pages %u/%u. Elapsed %u/%u sec.",
	NAMEDATALEN, relname, 
	nblocks, nchpg, Vvpl->vpl_npages, nempg, nnepg,
	ntups, nvac, ncrash, nunused, min_tlen, max_tlen, 
	frsize, frsusf, nemend, Fvpl->vpl_npages,
	ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec, 
	ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

} /* _vc_scanheap */


/*
 *  _vc_rpfheap() -- try to repaire relation' fragmentation
 *
 *	This routine marks dead tuples as unused and tries re-use dead space
 *	by moving tuples (and inserting indices if needed). It constructs 
 *	Nvpl list of free-ed pages (moved tuples) and clean indices
 *	for them after committing (in hack-manner - without losing locks
 *	and freeing memory!) current transaction. It truncates relation
 *	if some end-blocks are gone away.
 */
static void
_vc_rpfheap (VRelList curvrl, Relation onerel, 
		VPageList Vvpl, VPageList Fvpl, int nindices, Relation *Irel)
{
    TransactionId myXID;
    CommandId myCID;
    AbsoluteTime myCTM;
    Buffer buf, ToBuf;
    int nblocks, blkno;
    Page page, ToPage;
    OffsetNumber offnum, maxoff, newoff, moff;
    ItemId itemid, newitemid;
    HeapTuple htup, newtup;
    TupleDesc tupdesc;
    Datum *idatum;
    char *inulls;
    InsertIndexResult iresult;
    VPageListData Nvpl;
    VPageDescr ToVpd, Fvplast, Vvplast, vpc, *vpp;
    IndDesc *Idesc, *idcur;
    int Fblklast, Vblklast, i;
    Size tlen;
    int nmoved, Fnpages, Vnpages;
    int nchkmvd, ntups;
    bool isempty, dowrite;
    Relation archrel;
    struct rusage ru0, ru1;

    getrusage(RUSAGE_SELF, &ru0);

    myXID = GetCurrentTransactionId();
    myCID = GetCurrentCommandId();
	
    if ( Irel != (Relation*) NULL )	/* preparation for index' inserts */
    {
	_vc_mkindesc (onerel, nindices, Irel, &Idesc);
	tupdesc = RelationGetTupleDescriptor(onerel);
	idatum = (Datum *) palloc(INDEX_MAX_KEYS * sizeof (*idatum));
	inulls = (char *) palloc(INDEX_MAX_KEYS * sizeof (*inulls));
    }

    /* if the relation has an archive, open it */
    if (onerel->rd_rel->relarch != 'n')
    {
	archrel = _vc_getarchrel(onerel);
	/* Archive tuples from "empty" end-pages */
	for ( vpp = Vvpl->vpl_pgdesc + Vvpl->vpl_npages - 1, 
				i = Vvpl->vpl_nemend; i > 0; i--, vpp-- )
	{
	    if ( (*vpp)->vpd_noff > 0 )
	    {
	    	buf = ReadBuffer(onerel, (*vpp)->vpd_blkno);
	    	page = BufferGetPage(buf);
	    	Assert ( !PageIsEmpty(page) );
		_vc_vacpage (page, *vpp, archrel);
		WriteBuffer (buf);
	    }
	}
    }
    else
	archrel = (Relation) NULL;

    Nvpl.vpl_npages = 0;
    Fnpages = Fvpl->vpl_npages;
    Fvplast = Fvpl->vpl_pgdesc[Fnpages - 1];
    Fblklast = Fvplast->vpd_blkno;
    Assert ( Vvpl->vpl_npages > Vvpl->vpl_nemend );
    Vnpages = Vvpl->vpl_npages - Vvpl->vpl_nemend;
    Vvplast = Vvpl->vpl_pgdesc[Vnpages - 1];
    Vblklast = Vvplast->vpd_blkno;
    Assert ( Vblklast >= Fblklast );
    ToBuf = InvalidBuffer;
    nmoved = 0;

    vpc = (VPageDescr) palloc (sizeof(VPageDescrData) + MaxOffsetNumber*sizeof(OffsetNumber));
    vpc->vpd_nusd = 0;
	
    nblocks = curvrl->vrl_npages;
    for (blkno = nblocks - Vvpl->vpl_nemend - 1; ; blkno--)
    {
	/* if it's reapped page and it was used by me - quit */
	if ( blkno == Fblklast && Fvplast->vpd_nusd > 0 )
	    break;

	buf = ReadBuffer(onerel, blkno);
	page = BufferGetPage(buf);

	vpc->vpd_noff = 0;

	isempty = PageIsEmpty(page);

	if ( blkno == Vblklast )		/* it's reapped page */
	{
	    if ( Vvplast->vpd_noff > 0 )	/* there are dead tuples */
	    {					/* on this page - clean */
		Assert ( ! isempty );
		_vc_vacpage (page, Vvplast, archrel);
		dowrite = true;
	    }
	    else
		Assert ( isempty );
	    Assert ( --Vnpages > 0 );
	    /* get prev reapped page from Vvpl */
	    Vvplast = Vvpl->vpl_pgdesc[Vnpages - 1];
	    Vblklast = Vvplast->vpd_blkno;
	    if ( blkno == Fblklast )	/* this page in Fvpl too */
	    {
		Assert ( --Fnpages > 0 );
		Assert ( Fvplast->vpd_nusd == 0 );
		/* get prev reapped page from Fvpl */
		Fvplast = Fvpl->vpl_pgdesc[Fnpages - 1];
		Fblklast = Fvplast->vpd_blkno;
	    }
	    Assert ( Fblklast <= Vblklast );
	    if ( isempty )
	    {
		ReleaseBuffer(buf);
		continue;
	    }
	}
	else
	{
	    Assert ( ! isempty );
	    dowrite = false;
	}

	vpc->vpd_blkno = blkno;
	maxoff = PageGetMaxOffsetNumber(page);
	for (offnum = FirstOffsetNumber;
		offnum <= maxoff;
		offnum = OffsetNumberNext(offnum))
	{
	    itemid = PageGetItemId(page, offnum);

	    if (!ItemIdIsUsed(itemid))
		continue;

	    htup = (HeapTuple) PageGetItem(page, itemid);
	    tlen = htup->t_len;
		
	    /* try to find new page for this tuple */
	    if ( ToBuf == InvalidBuffer ||
		! _vc_enough_space (ToVpd, tlen) )
	    {
		if ( ToBuf != InvalidBuffer )
		    WriteBuffer(ToBuf);
		ToBuf = InvalidBuffer;
		for (i=0; i < Fnpages; i++)
		{
		    if ( _vc_enough_space (Fvpl->vpl_pgdesc[i], tlen) )
			break;
		}
		if ( i == Fnpages )
		    break;			/* can't move item anywhere */
		ToVpd = Fvpl->vpl_pgdesc[i];
		ToBuf = ReadBuffer(onerel, ToVpd->vpd_blkno);
		ToPage = BufferGetPage(ToBuf);
		/* if this page was not used before - clean it */
		if ( ! PageIsEmpty(ToPage) && ToVpd->vpd_nusd == 0 )
		    _vc_vacpage (ToPage, ToVpd, archrel);
	    }
		
	    /* copy tuple */
	    newtup = (HeapTuple) palloc (tlen);
	    memmove((char *) newtup, (char *) htup, tlen);

	    /* store transaction information */
	    TransactionIdStore(myXID, &(newtup->t_xmin));
	    newtup->t_cmin = myCID;
	    StoreInvalidTransactionId(&(newtup->t_xmax));
	    newtup->t_tmin = INVALID_ABSTIME;
	    newtup->t_tmax = CURRENT_ABSTIME;
	    ItemPointerSetInvalid(&newtup->t_chain);

	    /* add tuple to the page */
	    newoff = PageAddItem (ToPage, (Item)newtup, tlen, 
				InvalidOffsetNumber, LP_USED);
	    if ( newoff == InvalidOffsetNumber )
	    {
		elog (WARN, "\
failed to add item with len = %u to page %u (free space %u, nusd %u, noff %u)",
		tlen, ToVpd->vpd_blkno, ToVpd->vpd_free, 
		ToVpd->vpd_nusd, ToVpd->vpd_noff);
	    }
	    newitemid = PageGetItemId(ToPage, newoff);
	    pfree (newtup);
	    newtup = (HeapTuple) PageGetItem(ToPage, newitemid);
	    ItemPointerSet(&(newtup->t_ctid), ToVpd->vpd_blkno, newoff);

	    /* now logically delete end-tuple */
	    TransactionIdStore(myXID, &(htup->t_xmax));
	    htup->t_cmax = myCID;
	    memmove ((char*)&(htup->t_chain), (char*)&(newtup->t_ctid), sizeof (newtup->t_ctid));

	    ToVpd->vpd_nusd++;
	    nmoved++;
	    ToVpd->vpd_free = ((PageHeader)ToPage)->pd_upper - ((PageHeader)ToPage)->pd_lower;
	    vpc->vpd_voff[vpc->vpd_noff++] = offnum;
		
	    /* insert index' tuples if needed */
	    if ( Irel != (Relation*) NULL )
	    {
		for (i = 0, idcur = Idesc; i < nindices; i++, idcur++)
		{
		    FormIndexDatum (
		    		idcur->natts,
		    		(AttrNumber *)&(idcur->tform->indkey[0]),
				newtup, 
				tupdesc,
				InvalidBuffer,
				idatum,
				inulls,
				idcur->finfoP);
		    iresult = index_insert (
				Irel[i],
				idatum,
				inulls,
				&(newtup->t_ctid),
				true);
		    if (iresult) pfree(iresult);
		}
	    }
		
	} /* walk along page */

	if ( vpc->vpd_noff > 0 )		/* some tuples were moved */
	{
	    _vc_reappage (&Nvpl, vpc);
	    WriteBuffer(buf);
	}
	else if ( dowrite )
	    WriteBuffer(buf);
	else
	    ReleaseBuffer(buf);
	    
	if ( offnum <= maxoff )
	    break;				/* some item(s) left */
	    
    } /* walk along relation */
	
    blkno++;				/* new number of blocks */

    if ( ToBuf != InvalidBuffer )
    {
	Assert (nmoved > 0);
	WriteBuffer(ToBuf);
    }

    if ( nmoved > 0 )
    {
	/* 
	 * We have to commit our tuple' movings before we'll truncate 
	 * relation, but we shouldn't lose our locks. And so - quick hack: 
	 * flush buffers and record status of current transaction
	 * as committed, and continue. - vadim 11/13/96
	 */
	FlushBufferPool(!TransactionFlushEnabled());
	TransactionIdCommit(myXID);
	FlushBufferPool(!TransactionFlushEnabled());
	myCTM = TransactionIdGetCommitTime(myXID);
    }
	
    /* 
     * Clean uncleaned reapped pages from Vvpl list 
     * and set commit' times  for inserted tuples
     */
    nchkmvd = 0;
    for (i = 0, vpp = Vvpl->vpl_pgdesc; i < Vnpages; i++, vpp++)
    {
	Assert ( (*vpp)->vpd_blkno < blkno );
	buf = ReadBuffer(onerel, (*vpp)->vpd_blkno);
	page = BufferGetPage(buf);
	if ( (*vpp)->vpd_nusd == 0 )	/* this page was not used */
	{
	    /* noff == 0 in empty pages only - such pages should be re-used */
	    Assert ( (*vpp)->vpd_noff > 0 );
	    _vc_vacpage (page, *vpp, archrel);
	}
	else				/* this page was used */
	{
	    ntups = 0;
	    moff = PageGetMaxOffsetNumber(page);
	    for (newoff = FirstOffsetNumber;
			newoff <= moff;
			newoff = OffsetNumberNext(newoff))
	    {
	    	itemid = PageGetItemId(page, newoff);
	    	if (!ItemIdIsUsed(itemid))
		    continue;
	    	htup = (HeapTuple) PageGetItem(page, itemid);
	    	if ( TransactionIdEquals((TransactionId)htup->t_xmin, myXID) )
	    	{
	    	    htup->t_tmin = myCTM;
	    	    ntups++;
	    	}
	    }
	    Assert ( (*vpp)->vpd_nusd == ntups );
	    nchkmvd += ntups;
	}
    	WriteBuffer (buf);
    }
    Assert ( nmoved == nchkmvd );

    getrusage(RUSAGE_SELF, &ru1);
    
    elog (MESSLEV, "Rel %.*s: Pages: %u --> %u; Tuple(s) moved: %u. \
Elapsed %u/%u sec.",
		NAMEDATALEN, (RelationGetRelationName(onerel))->data, 
		nblocks, blkno, nmoved,
		ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec, 
		ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

    if ( Nvpl.vpl_npages > 0 )
    {
	/* vacuum indices again if needed */
	if ( Irel != (Relation*) NULL )
	{
	    VPageDescr *vpleft, *vpright, vpsave;
		
	    /* re-sort Nvpl.vpl_pgdesc */
	    for (vpleft = Nvpl.vpl_pgdesc, 
		vpright = Nvpl.vpl_pgdesc + Nvpl.vpl_npages - 1;
		vpleft < vpright; vpleft++, vpright--)
	    {
		vpsave = *vpleft; *vpleft = *vpright; *vpright = vpsave;
	    }
	    for (i = 0; i < nindices; i++)
		_vc_vaconeind (&Nvpl, Irel[i], curvrl->vrl_ntups);
	}

	/* 
	 * clean moved tuples from last page in Nvpl list
	 * if some tuples left there
	 */
	if ( vpc->vpd_noff > 0 && offnum <= maxoff )
	{
	    Assert (vpc->vpd_blkno == blkno - 1);
	    buf = ReadBuffer(onerel, vpc->vpd_blkno);
	    page = BufferGetPage (buf);
	    ntups = 0;
	    maxoff = offnum;
	    for (offnum = FirstOffsetNumber;
			offnum < maxoff;
			offnum = OffsetNumberNext(offnum))
	    {
	    	itemid = PageGetItemId(page, offnum);
	    	if (!ItemIdIsUsed(itemid))
		    continue;
	    	htup = (HeapTuple) PageGetItem(page, itemid);
	    	Assert ( TransactionIdEquals((TransactionId)htup->t_xmax, myXID) );
		itemid->lp_flags &= ~LP_USED;
		ntups++;
	    }
	    Assert ( vpc->vpd_noff == ntups );
	    PageRepairFragmentation(page);
	    WriteBuffer (buf);
	}

	/* now - free new list of reapped pages */
	vpp = Nvpl.vpl_pgdesc;
	for (i = 0; i < Nvpl.vpl_npages; i++, vpp++)
	    pfree(*vpp);
	pfree (Nvpl.vpl_pgdesc);
    }
	
    /* truncate relation */
    if ( blkno < nblocks )
    {
    	blkno = smgrtruncate (onerel->rd_rel->relsmgr, onerel, blkno);
	Assert ( blkno >= 0 );
	curvrl->vrl_npages = blkno;	/* set new number of blocks */
    }

    if ( archrel != (Relation) NULL )
	heap_close(archrel);

    if ( Irel != (Relation*) NULL )	/* pfree index' allocations */
    {
	pfree (Idesc);
	pfree (idatum);
	pfree (inulls);
	_vc_clsindices (nindices, Irel);
    }

    pfree (vpc);

} /* _vc_rpfheap */

/*
 *  _vc_vacheap() -- free dead tuples
 *
 *	This routine marks dead tuples as unused and truncates relation
 *	if there are "empty" end-blocks.
 */
static void
_vc_vacheap (VRelList curvrl, Relation onerel, VPageList Vvpl)
{
    Buffer buf;
    Page page;
    VPageDescr *vpp;
    Relation archrel;
    int nblocks;
    int i;

    nblocks = Vvpl->vpl_npages;
    /* if the relation has an archive, open it */
    if (onerel->rd_rel->relarch != 'n')
	archrel = _vc_getarchrel(onerel);
    else
    {
	archrel = (Relation) NULL;
	nblocks -= Vvpl->vpl_nemend;	/* nothing to do with them */
    }
	
    for (i = 0, vpp = Vvpl->vpl_pgdesc; i < nblocks; i++, vpp++)
    {
	if ( (*vpp)->vpd_noff > 0 )
	{
	    buf = ReadBuffer(onerel, (*vpp)->vpd_blkno);
	    page = BufferGetPage (buf);
	    _vc_vacpage (page, *vpp, archrel);
	    WriteBuffer (buf);
	}
    }

    /* truncate relation if there are some empty end-pages */
    if ( Vvpl->vpl_nemend > 0 )
    {
	Assert ( curvrl->vrl_npages >= Vvpl->vpl_nemend );
	nblocks = curvrl->vrl_npages - Vvpl->vpl_nemend;
	elog (MESSLEV, "Rel %.*s: Pages: %u --> %u.",
		NAMEDATALEN, (RelationGetRelationName(onerel))->data, 
		curvrl->vrl_npages, nblocks);

	/* 
	 * we have to flush "empty" end-pages (if changed, but who knows it)
	 * before truncation 
	 */
	FlushBufferPool(!TransactionFlushEnabled());

    	nblocks = smgrtruncate (onerel->rd_rel->relsmgr, onerel, nblocks);
	Assert ( nblocks >= 0 );
	curvrl->vrl_npages = nblocks;	/* set new number of blocks */
    }

    if ( archrel != (Relation) NULL )
	heap_close(archrel);

} /* _vc_vacheap */

/*
 *  _vc_vacpage() -- free (and archive if needed) dead tuples on a page
 *		     and repaire its fragmentation.
 */
static void
_vc_vacpage (Page page, VPageDescr vpd, Relation archrel)
{
    ItemId itemid;
    HeapTuple htup;
    int i;
    
    Assert ( vpd->vpd_nusd == 0 );
    for (i=0; i < vpd->vpd_noff; i++)
    {
	itemid = &(((PageHeader) page)->pd_linp[vpd->vpd_voff[i] - 1]);
	if ( archrel != (Relation) NULL && ItemIdIsUsed(itemid) )
	{
	    htup = (HeapTuple) PageGetItem (page, itemid);
	    _vc_archive (archrel, htup);
	}
	itemid->lp_flags &= ~LP_USED;
    }
    PageRepairFragmentation(page);

} /* _vc_vacpage */

/*
 *  _vc_vaconeind() -- vacuum one index relation.
 *
 *	Vpl is the VPageList of the heap we're currently vacuuming.
 *	It's locked. Indrel is an index relation on the vacuumed heap. 
 *	We don't set locks on the index	relation here, since the indexed 
 *	access methods support locking at different granularities. 
 *	We let them handle it.
 *
 *	Finally, we arrange to update the index relation's statistics in
 *	pg_class.
 */
static void
_vc_vaconeind(VPageList vpl, Relation indrel, int nhtups)
{
    RetrieveIndexResult res;
    IndexScanDesc iscan;
    ItemPointer heapptr;
    int nvac;
    int nitups;
    int nipages;
    VPageDescr vp;
    struct rusage ru0, ru1;

    getrusage(RUSAGE_SELF, &ru0);

    /* walk through the entire index */
    iscan = index_beginscan(indrel, false, 0, (ScanKey) NULL);
    nvac = 0;
    nitups = 0;

    while ((res = index_getnext(iscan, ForwardScanDirection))
	   != (RetrieveIndexResult) NULL) {
	heapptr = &res->heap_iptr;

	if ( (vp = _vc_tidreapped (heapptr, vpl)) != (VPageDescr) NULL)
	{
#if 0
	    elog(DEBUG, "<%x,%x> -> <%x,%x>",
		 ItemPointerGetBlockNumber(&(res->index_iptr)),
		 ItemPointerGetOffsetNumber(&(res->index_iptr)),
		 ItemPointerGetBlockNumber(&(res->heap_iptr)),
		 ItemPointerGetOffsetNumber(&(res->heap_iptr)));
#endif
	    if ( vp->vpd_noff == 0 ) 
	    {				/* this is EmptyPage !!! */
	    	elog (NOTICE, "Ind %.*s: pointer to EmptyPage (blk %u off %u) - fixing",
			NAMEDATALEN, indrel->rd_rel->relname.data,
	    		vp->vpd_blkno, ItemPointerGetOffsetNumber(heapptr));
	    }
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

    elog (MESSLEV, "Ind %.*s: Pages %u; Tuples %u: Deleted %u. Elapsed %u/%u sec.",
	NAMEDATALEN, indrel->rd_rel->relname.data, nipages, nitups, nvac,
	ru1.ru_stime.tv_sec - ru0.ru_stime.tv_sec, 
	ru1.ru_utime.tv_sec - ru0.ru_utime.tv_sec);

    if ( nitups != nhtups )
    	elog (NOTICE, "NUMBER OF INDEX' TUPLES (%u) IS NOT THE SAME AS HEAP' (%u)",
    		nitups, nhtups);

} /* _vc_vaconeind */

/*
 *  _vc_tidreapped() -- is a particular tid reapped?
 *
 *	vpl->VPageDescr_array is sorted in right order.
 */
static VPageDescr
_vc_tidreapped(ItemPointer itemptr, VPageList vpl)
{
    OffsetNumber ioffno;
    OffsetNumber *voff;
    VPageDescr vp, *vpp;
    VPageDescrData vpd;

    vpd.vpd_blkno = ItemPointerGetBlockNumber(itemptr);
    ioffno = ItemPointerGetOffsetNumber(itemptr);
	
    vp = &vpd;
    vpp = (VPageDescr*) _vc_find_eq ((char*)(vpl->vpl_pgdesc), 
		vpl->vpl_npages, sizeof (VPageDescr), (char*)&vp, 
		_vc_cmp_blk);

    if ( vpp == (VPageDescr*) NULL )
	return ((VPageDescr)NULL);
    vp = *vpp;

    /* ok - we are on true page */

    if ( vp->vpd_noff == 0 ) {		/* this is EmptyPage !!! */
	return (vp);
    }
    
    voff = (OffsetNumber*) _vc_find_eq ((char*)(vp->vpd_voff), 
		vp->vpd_noff, sizeof (OffsetNumber), (char*)&ioffno, 
		_vc_cmp_offno);

    if ( voff == (OffsetNumber*) NULL )
	return ((VPageDescr)NULL);

    return (vp);

} /* _vc_tidreapped */

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
    WriteNoReleaseBuffer(buf);	/* heap_endscan release scan' buffers ? */

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
 *  _vc_reappage() -- save a page on the array of reapped pages.
 *
 *	As a side effect of the way that the vacuuming loop for a given
 *	relation works, higher pages come after lower pages in the array
 *	(and highest tid on a page is last).
 */
static void 
_vc_reappage(VPageList vpl, VPageDescr vpc)
{
    VPageDescr newvpd;

    /* allocate a VPageDescrData entry */
    newvpd = (VPageDescr) palloc(sizeof(VPageDescrData) + vpc->vpd_noff*sizeof(OffsetNumber));

    /* fill it in */
    if ( vpc->vpd_noff > 0 )
    	memmove (newvpd->vpd_voff, vpc->vpd_voff, vpc->vpd_noff*sizeof(OffsetNumber));
    newvpd->vpd_blkno = vpc->vpd_blkno;
    newvpd->vpd_free = vpc->vpd_free;
    newvpd->vpd_nusd = vpc->vpd_nusd;
    newvpd->vpd_noff = vpc->vpd_noff;

    /* insert this page into vpl list */
    _vc_vpinsert (vpl, newvpd);
    
} /* _vc_reappage */

static void
_vc_vpinsert (VPageList vpl, VPageDescr vpnew)
{

    /* allocate a VPageDescr entry if needed */
    if ( vpl->vpl_npages == 0 )
    	vpl->vpl_pgdesc = (VPageDescr*) palloc(100*sizeof(VPageDescr));
    else if ( vpl->vpl_npages % 100 == 0 )
    	vpl->vpl_pgdesc = (VPageDescr*) repalloc(vpl->vpl_pgdesc, (vpl->vpl_npages+100)*sizeof(VPageDescr));
    vpl->vpl_pgdesc[vpl->vpl_npages] = vpnew;
    (vpl->vpl_npages)++;
    
}

static void
_vc_free(Portal p, VRelList vrl)
{
    VRelList p_vrl;
    VAttList p_val, val;
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


static void
_vc_getindices (Oid relid, int *nindices, Relation **Irel)
{
    Relation pgindex;
    Relation irel;
    TupleDesc pgidesc;
    HeapTuple pgitup;
    HeapScanDesc pgiscan;
    Datum d;
    int i, k;
    bool n;
    ScanKeyData pgikey;
    Oid *ioid;

    *nindices = i = 0;
    
    ioid = (Oid *) palloc(10*sizeof(Oid));

    /* prepare a heap scan on the pg_index relation */
    pgindex = heap_openr(IndexRelationName);
    pgidesc = RelationGetTupleDescriptor(pgindex);

    ScanKeyEntryInitialize(&pgikey, 0x0, Anum_pg_index_indrelid,
			   ObjectIdEqualRegProcedure,
			   ObjectIdGetDatum(relid));

    pgiscan = heap_beginscan(pgindex, false, NowTimeQual, 1, &pgikey);

    while (HeapTupleIsValid(pgitup = heap_getnext(pgiscan, 0, NULL))) {
	d = (Datum) heap_getattr(pgitup, InvalidBuffer, Anum_pg_index_indexrelid,
				 pgidesc, &n);
	i++;
	if ( i % 10 == 0 )
	    ioid = (Oid *) repalloc(ioid, (i+10)*sizeof(Oid));
	ioid[i-1] = DatumGetObjectId(d);
    }

    heap_endscan(pgiscan);
    heap_close(pgindex);

    if ( i == 0 ) {	/* No one index found */
	pfree(ioid);
	return;
    }

    if ( Irel != (Relation **) NULL )
	*Irel = (Relation *) palloc(i * sizeof(Relation));
    
    for (k = 0; i > 0; )
    {
	irel = index_open(ioid[--i]);
	if ( irel != (Relation) NULL )
	{
	    if ( Irel != (Relation **) NULL )
		(*Irel)[k] = irel;
	    else
		index_close (irel);
	    k++;
	}
	else
	    elog (NOTICE, "CAN't OPEN INDEX %u - SKIP IT", ioid[i]);
    }
    *nindices = k;
    pfree(ioid);

    if ( Irel != (Relation **) NULL && *nindices == 0 )
    {
	pfree (*Irel);
	*Irel = (Relation *) NULL;
    }

} /* _vc_getindices */


static void
_vc_clsindices (int nindices, Relation *Irel)
{

    if ( Irel == (Relation*) NULL )
    	return;

    while (nindices--) {
	index_close (Irel[nindices]);
    }
    pfree (Irel);

} /* _vc_clsindices */


static void
_vc_mkindesc (Relation onerel, int nindices, Relation *Irel, IndDesc **Idesc)
{
    IndDesc *idcur;
    HeapTuple pgIndexTup;
    AttrNumber *attnumP;
    int natts;
    int i;

    *Idesc = (IndDesc *) palloc (nindices * sizeof (IndDesc));
    
    for (i = 0, idcur = *Idesc; i < nindices; i++, idcur++) {
	pgIndexTup =
		SearchSysCacheTuple(INDEXRELID,
				ObjectIdGetDatum(Irel[i]->rd_id),
				0,0,0);
	Assert(pgIndexTup);
	idcur->tform = (IndexTupleForm)GETSTRUCT(pgIndexTup);
	for (attnumP = &(idcur->tform->indkey[0]), natts = 0;
		*attnumP != InvalidAttrNumber && natts != INDEX_MAX_KEYS;
		attnumP++, natts++);
	if (idcur->tform->indproc != InvalidOid) {
	    idcur->finfoP = &(idcur->finfo);
	    FIgetnArgs(idcur->finfoP) = natts;
	    natts = 1;
	    FIgetProcOid(idcur->finfoP) = idcur->tform->indproc;
	    *(FIgetname(idcur->finfoP)) = '\0';
	} else
	    idcur->finfoP = (FuncIndexInfo *) NULL;
	
	idcur->natts = natts;
    }
    
} /* _vc_mkindesc */


static bool
_vc_enough_space (VPageDescr vpd, Size len)
{

    len = DOUBLEALIGN(len);

    if ( len > vpd->vpd_free )
    	return (false);
    
    if ( vpd->vpd_nusd < vpd->vpd_noff )	/* there are free itemid(s) */
    	return (true);				/* and len <= free_space */
    
    /* ok. noff_usd >= noff_free and so we'll have to allocate new itemid */
    if ( len <= vpd->vpd_free - sizeof (ItemIdData) )
    	return (true);
    
    return (false);
    
} /* _vc_enough_space */
