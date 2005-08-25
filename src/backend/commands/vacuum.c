/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  The postgres vacuum cleaner.
 *
 * This file includes the "full" version of VACUUM, as well as control code
 * used by all three of full VACUUM, lazy VACUUM, and ANALYZE.	See
 * vacuumlazy.c and analyze.c for the rest of the code for the latter two.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/commands/vacuum.c,v 1.263.2.2 2005/08/25 22:07:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/clog.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/xlog.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_index.h"
#include "commands/vacuum.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "storage/sinval.h"
#include "storage/smgr.h"
#include "tcop/pquery.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"
#include "utils/syscache.h"
#include "pgstat.h"


typedef struct VacPageData
{
	BlockNumber blkno;			/* BlockNumber of this Page */
	Size		free;			/* FreeSpace on this Page */
	uint16		offsets_used;	/* Number of OffNums used by vacuum */
	uint16		offsets_free;	/* Number of OffNums free or to be free */
	OffsetNumber offsets[1];	/* Array of free OffNums */
} VacPageData;

typedef VacPageData *VacPage;

typedef struct VacPageListData
{
	BlockNumber empty_end_pages;	/* Number of "empty" end-pages */
	int			num_pages;		/* Number of pages in pagedesc */
	int			num_allocated_pages;	/* Number of allocated pages in
										 * pagedesc */
	VacPage    *pagedesc;		/* Descriptions of pages */
} VacPageListData;

typedef VacPageListData *VacPageList;

typedef struct VTupleLinkData
{
	ItemPointerData new_tid;
	ItemPointerData this_tid;
} VTupleLinkData;

typedef VTupleLinkData *VTupleLink;

typedef struct VTupleMoveData
{
	ItemPointerData tid;		/* tuple ID */
	VacPage		vacpage;		/* where to move */
	bool		cleanVpd;		/* clean vacpage before using */
} VTupleMoveData;

typedef VTupleMoveData *VTupleMove;

typedef struct VRelStats
{
	BlockNumber rel_pages;
	double		rel_tuples;
	Size		min_tlen;
	Size		max_tlen;
	bool		hasindex;
	int			num_vtlinks;
	VTupleLink	vtlinks;
} VRelStats;


static MemoryContext vac_context = NULL;

static int	elevel = -1;

static TransactionId OldestXmin;
static TransactionId FreezeLimit;


/* non-export function prototypes */
static List *getrels(const RangeVar *vacrel, const char *stmttype);
static void vac_update_dbstats(Oid dbid,
				   TransactionId vacuumXID,
				   TransactionId frozenXID);
static void vac_truncate_clog(TransactionId vacuumXID,
				  TransactionId frozenXID);
static bool vacuum_rel(Oid relid, VacuumStmt *vacstmt, char expected_relkind);
static void full_vacuum_rel(Relation onerel, VacuumStmt *vacstmt);
static void scan_heap(VRelStats *vacrelstats, Relation onerel,
		  VacPageList vacuum_pages, VacPageList fraged_pages);
static void repair_frag(VRelStats *vacrelstats, Relation onerel,
			VacPageList vacuum_pages, VacPageList fraged_pages,
			int nindexes, Relation *Irel);
static void vacuum_heap(VRelStats *vacrelstats, Relation onerel,
			VacPageList vacpagelist);
static void vacuum_page(Relation onerel, Buffer buffer, VacPage vacpage);
static void vacuum_index(VacPageList vacpagelist, Relation indrel,
			 double num_tuples, int keep_tuples);
static void scan_index(Relation indrel, double num_tuples);
static bool tid_reaped(ItemPointer itemptr, void *state);
static bool dummy_tid_reaped(ItemPointer itemptr, void *state);
static void vac_update_fsm(Relation onerel, VacPageList fraged_pages,
			   BlockNumber rel_pages);
static VacPage copy_vac_page(VacPage vacpage);
static void vpage_insert(VacPageList vacpagelist, VacPage vpnew);
static void *vac_bsearch(const void *key, const void *base,
			size_t nelem, size_t size,
			int (*compar) (const void *, const void *));
static int	vac_cmp_blk(const void *left, const void *right);
static int	vac_cmp_offno(const void *left, const void *right);
static int	vac_cmp_vtlinks(const void *left, const void *right);
static bool enough_space(VacPage vacpage, Size len);


/****************************************************************************
 *																			*
 *			Code common to all flavors of VACUUM and ANALYZE				*
 *																			*
 ****************************************************************************
 */


/*
 * Primary entry point for VACUUM and ANALYZE commands.
 */
void
vacuum(VacuumStmt *vacstmt)
{
	const char *stmttype = vacstmt->vacuum ? "VACUUM" : "ANALYZE";
	MemoryContext anl_context = NULL;
	TransactionId initialOldestXmin = InvalidTransactionId;
	TransactionId initialFreezeLimit = InvalidTransactionId;
	bool		all_rels;
	List	   *vrl,
			   *cur;

	if (vacstmt->verbose)
		elevel = INFO;
	else
		elevel = DEBUG2;

	/*
	 * We cannot run VACUUM inside a user transaction block; if we were
	 * inside a transaction, then our commit- and
	 * start-transaction-command calls would not have the intended effect!
	 * Furthermore, the forced commit that occurs before truncating the
	 * relation's file would have the effect of committing the rest of the
	 * user's transaction too, which would certainly not be the desired
	 * behavior.
	 */
	if (vacstmt->vacuum)
		PreventTransactionChain((void *) vacstmt, stmttype);

	/*
	 * Send info about dead objects to the statistics collector
	 */
	if (vacstmt->vacuum)
		pgstat_vacuum_tabstat();

	/*
	 * Create special memory context for cross-transaction storage.
	 *
	 * Since it is a child of PortalContext, it will go away eventually even
	 * if we suffer an error; there's no need for special abort cleanup
	 * logic.
	 */
	vac_context = AllocSetContextCreate(PortalContext,
										"Vacuum",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * If we are running only ANALYZE, we don't need per-table
	 * transactions, but we still need a memory context with table
	 * lifetime.
	 */
	if (vacstmt->analyze && !vacstmt->vacuum)
		anl_context = AllocSetContextCreate(PortalContext,
											"Analyze",
											ALLOCSET_DEFAULT_MINSIZE,
											ALLOCSET_DEFAULT_INITSIZE,
											ALLOCSET_DEFAULT_MAXSIZE);

	/* Assume we are processing everything unless one table is mentioned */
	all_rels = (vacstmt->relation == NULL);

	/* Build list of relations to process (note this lives in vac_context) */
	vrl = getrels(vacstmt->relation, stmttype);

	/*
	 * Formerly, there was code here to prevent more than one VACUUM from
	 * executing concurrently in the same database.  However, there's no
	 * good reason to prevent that, and manually removing lockfiles after
	 * a vacuum crash was a pain for dbadmins.	So, forget about
	 * lockfiles, and just rely on the locks we grab on each target table
	 * to ensure that there aren't two VACUUMs running on the same table
	 * at the same time.
	 */

	/*
	 * The strangeness with committing and starting transactions here is
	 * due to wanting to run each table's VACUUM as a separate
	 * transaction, so that we don't hold locks unnecessarily long.  Also,
	 * if we are doing VACUUM ANALYZE, the ANALYZE part runs as a separate
	 * transaction from the VACUUM to further reduce locking.
	 *
	 * vacuum_rel expects to be entered with no transaction active; it will
	 * start and commit its own transaction.  But we are called by an SQL
	 * command, and so we are executing inside a transaction already.  We
	 * commit the transaction started in PostgresMain() here, and start
	 * another one before exiting to match the commit waiting for us back
	 * in PostgresMain().
	 *
	 * In the case of an ANALYZE statement (no vacuum, just analyze) it's
	 * okay to run the whole thing in the outer transaction, and so we
	 * skip transaction start/stop operations.
	 */
	if (vacstmt->vacuum)
	{
		if (all_rels)
		{
			/*
			 * It's a database-wide VACUUM.
			 *
			 * Compute the initially applicable OldestXmin and FreezeLimit
			 * XIDs, so that we can record these values at the end of the
			 * VACUUM. Note that individual tables may well be processed
			 * with newer values, but we can guarantee that no
			 * (non-shared) relations are processed with older ones.
			 *
			 * It is okay to record non-shared values in pg_database, even
			 * though we may vacuum shared relations with older cutoffs,
			 * because only the minimum of the values present in
			 * pg_database matters.  We can be sure that shared relations
			 * have at some time been vacuumed with cutoffs no worse than
			 * the global minimum; for, if there is a backend in some
			 * other DB with xmin = OLDXMIN that's determining the cutoff
			 * with which we vacuum shared relations, it is not possible
			 * for that database to have a cutoff newer than OLDXMIN
			 * recorded in pg_database.
			 */
			vacuum_set_xid_limits(vacstmt, false,
								  &initialOldestXmin,
								  &initialFreezeLimit);
		}

		/* matches the StartTransaction in PostgresMain() */
		CommitTransactionCommand();
	}

	/*
	 * Loop to process each selected relation.
	 */
	foreach(cur, vrl)
	{
		Oid			relid = lfirsto(cur);

		if (vacstmt->vacuum)
		{
			if (!vacuum_rel(relid, vacstmt, RELKIND_RELATION))
				all_rels = false;		/* forget about updating dbstats */
		}
		if (vacstmt->analyze)
		{
			MemoryContext old_context = NULL;

			/*
			 * If we vacuumed, use new transaction for analyze. Otherwise,
			 * we can use the outer transaction, but we still need to call
			 * analyze_rel in a memory context that will be cleaned up on
			 * return (else we leak memory while processing multiple
			 * tables).
			 */
			if (vacstmt->vacuum)
			{
				StartTransactionCommand();
				SetQuerySnapshot();		/* might be needed for functions
										 * in indexes */
			}
			else
				old_context = MemoryContextSwitchTo(anl_context);

			analyze_rel(relid, vacstmt);

			if (vacstmt->vacuum)
				CommitTransactionCommand();
			else
			{
				MemoryContextSwitchTo(old_context);
				MemoryContextResetAndDeleteChildren(anl_context);
			}
		}
	}

	/*
	 * Finish up processing.
	 */
	if (vacstmt->vacuum)
	{
		/* here, we are not in a transaction */

		/*
		 * This matches the CommitTransaction waiting for us in
		 * PostgresMain().
		 */
		StartTransactionCommand();

		/*
		 * If it was a database-wide VACUUM, print FSM usage statistics
		 * (we don't make you be superuser to see these).
		 */
		if (vacstmt->relation == NULL)
			PrintFreeSpaceMapStatistics(elevel);

		/*
		 * If we completed a database-wide VACUUM without skipping any
		 * relations, update the database's pg_database row with info
		 * about the transaction IDs used, and try to truncate pg_clog.
		 */
		if (all_rels)
		{
			vac_update_dbstats(MyDatabaseId,
							   initialOldestXmin, initialFreezeLimit);
			vac_truncate_clog(initialOldestXmin, initialFreezeLimit);
		}
	}

	/*
	 * Clean up working storage --- note we must do this after
	 * StartTransactionCommand, else we might be trying to delete the
	 * active context!
	 */
	MemoryContextDelete(vac_context);
	vac_context = NULL;

	if (anl_context)
		MemoryContextDelete(anl_context);
}

/*
 * Build a list of Oids for each relation to be processed
 *
 * The list is built in vac_context so that it will survive across our
 * per-relation transactions.
 */
static List *
getrels(const RangeVar *vacrel, const char *stmttype)
{
	List	   *vrl = NIL;
	MemoryContext oldcontext;

	if (vacrel)
	{
		/* Process specific relation */
		Oid			relid;

		relid = RangeVarGetRelid(vacrel, false);

		/* Make a relation list entry for this guy */
		oldcontext = MemoryContextSwitchTo(vac_context);
		vrl = lappendo(vrl, relid);
		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		/* Process all plain relations listed in pg_class */
		Relation	pgclass;
		HeapScanDesc scan;
		HeapTuple	tuple;
		ScanKeyData key;

		ScanKeyEntryInitialize(&key, 0x0,
							   Anum_pg_class_relkind,
							   F_CHAREQ,
							   CharGetDatum(RELKIND_RELATION));

		pgclass = heap_openr(RelationRelationName, AccessShareLock);

		scan = heap_beginscan(pgclass, SnapshotNow, 1, &key);

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			/* Make a relation list entry for this guy */
			oldcontext = MemoryContextSwitchTo(vac_context);
			vrl = lappendo(vrl, HeapTupleGetOid(tuple));
			MemoryContextSwitchTo(oldcontext);
		}

		heap_endscan(scan);
		heap_close(pgclass, AccessShareLock);
	}

	return vrl;
}

/*
 * vacuum_set_xid_limits() -- compute oldest-Xmin and freeze cutoff points
 */
void
vacuum_set_xid_limits(VacuumStmt *vacstmt, bool sharedRel,
					  TransactionId *oldestXmin,
					  TransactionId *freezeLimit)
{
	TransactionId limit;

	*oldestXmin = GetOldestXmin(sharedRel);

	Assert(TransactionIdIsNormal(*oldestXmin));

	if (vacstmt->freeze)
	{
		/* FREEZE option: use oldest Xmin as freeze cutoff too */
		limit = *oldestXmin;
	}
	else
	{
		/*
		 * Normal case: freeze cutoff is well in the past, to wit, about
		 * halfway to the wrap horizon
		 */
		limit = GetCurrentTransactionId() - (MaxTransactionId >> 2);
	}

	/*
	 * Be careful not to generate a "permanent" XID
	 */
	if (!TransactionIdIsNormal(limit))
		limit = FirstNormalTransactionId;

	/*
	 * Ensure sane relationship of limits
	 */
	if (TransactionIdFollows(limit, *oldestXmin))
	{
		ereport(WARNING,
				(errmsg("oldest xmin is far in the past"),
				 errhint("Close open transactions soon to avoid wraparound problems.")));
		limit = *oldestXmin;
	}

	*freezeLimit = limit;
}


/*
 *	vac_update_relstats() -- update statistics for one relation
 *
 *		Update the whole-relation statistics that are kept in its pg_class
 *		row.  There are additional stats that will be updated if we are
 *		doing ANALYZE, but we always update these stats.  This routine works
 *		for both index and heap relation entries in pg_class.
 *
 *		We violate no-overwrite semantics here by storing new values for the
 *		statistics columns directly into the pg_class tuple that's already on
 *		the page.  The reason for this is that if we updated these tuples in
 *		the usual way, vacuuming pg_class itself wouldn't work very well ---
 *		by the time we got done with a vacuum cycle, most of the tuples in
 *		pg_class would've been obsoleted.  Of course, this only works for
 *		fixed-size never-null columns, but these are.
 *
 *		This routine is shared by full VACUUM, lazy VACUUM, and stand-alone
 *		ANALYZE.
 */
void
vac_update_relstats(Oid relid, BlockNumber num_pages, double num_tuples,
					bool hasindex)
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
	if (!heap_fetch(rd, SnapshotNow, &rtup, &buffer, false, NULL))
		elog(ERROR, "pg_class entry for relid %u vanished during vacuuming",
			 relid);

	/* ensure no one else does this at the same time */
	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);

	/* overwrite the existing statistics in the tuple */
	pgcform = (Form_pg_class) GETSTRUCT(&rtup);
	pgcform->relpages = (int32) num_pages;
	pgcform->reltuples = num_tuples;
	pgcform->relhasindex = hasindex;

	/*
	 * If we have discovered that there are no indexes, then there's no
	 * primary key either.	This could be done more thoroughly...
	 */
	if (!hasindex)
		pgcform->relhaspkey = false;

	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);

	/*
	 * Invalidate the tuple in the catcaches; this also arranges to flush
	 * the relation's relcache entry.  (If we fail to commit for some
	 * reason, no flush will occur, but no great harm is done since there
	 * are no noncritical state updates here.)
	 */
	CacheInvalidateHeapTuple(rd, &rtup);

	/* Write the buffer */
	WriteBuffer(buffer);

	heap_close(rd, RowExclusiveLock);
}


/*
 *	vac_update_dbstats() -- update statistics for one database
 *
 *		Update the whole-database statistics that are kept in its pg_database
 *		row.
 *
 *		We violate no-overwrite semantics here by storing new values for the
 *		statistics columns directly into the tuple that's already on the page.
 *		As with vac_update_relstats, this avoids leaving dead tuples behind
 *		after a VACUUM; which is good since GetRawDatabaseInfo
 *		can get confused by finding dead tuples in pg_database.
 *
 *		This routine is shared by full and lazy VACUUM.  Note that it is only
 *		applied after a database-wide VACUUM operation.
 */
static void
vac_update_dbstats(Oid dbid,
				   TransactionId vacuumXID,
				   TransactionId frozenXID)
{
	Relation	relation;
	ScanKeyData entry[1];
	HeapScanDesc scan;
	HeapTuple	tuple;
	Form_pg_database dbform;

	relation = heap_openr(DatabaseRelationName, RowExclusiveLock);

	/* Must use a heap scan, since there's no syscache for pg_database */
	ScanKeyEntryInitialize(&entry[0], 0x0,
						   ObjectIdAttributeNumber, F_OIDEQ,
						   ObjectIdGetDatum(dbid));

	scan = heap_beginscan(relation, SnapshotNow, 1, entry);

	tuple = heap_getnext(scan, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for database %u", dbid);

	/* ensure no one else does this at the same time */
	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_EXCLUSIVE);

	dbform = (Form_pg_database) GETSTRUCT(tuple);

	/* overwrite the existing statistics in the tuple */
	dbform->datvacuumxid = vacuumXID;
	dbform->datfrozenxid = frozenXID;

	LockBuffer(scan->rs_cbuf, BUFFER_LOCK_UNLOCK);

	/* invalidate the tuple in the cache and write the buffer */
	CacheInvalidateHeapTuple(relation, tuple);
	WriteNoReleaseBuffer(scan->rs_cbuf);

	heap_endscan(scan);

	heap_close(relation, RowExclusiveLock);
}


/*
 *	vac_truncate_clog() -- attempt to truncate the commit log
 *
 *		Scan pg_database to determine the system-wide oldest datvacuumxid,
 *		and use it to truncate the transaction commit log (pg_clog).
 *		Also generate a warning if the system-wide oldest datfrozenxid
 *		seems to be in danger of wrapping around.
 *
 *		The passed XIDs are simply the ones I just wrote into my pg_database
 *		entry.	They're used to initialize the "min" calculations.
 *
 *		This routine is shared by full and lazy VACUUM.  Note that it is only
 *		applied after a database-wide VACUUM operation.
 */
static void
vac_truncate_clog(TransactionId vacuumXID, TransactionId frozenXID)
{
	TransactionId myXID;
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	tuple;
	int32		age;
	bool		vacuumAlreadyWrapped = false;
	bool		frozenAlreadyWrapped = false;

	myXID = GetCurrentTransactionId();

	relation = heap_openr(DatabaseRelationName, AccessShareLock);

	scan = heap_beginscan(relation, SnapshotNow, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);

		/* Ignore non-connectable databases (eg, template0) */
		/* It's assumed that these have been frozen correctly */
		if (!dbform->datallowconn)
			continue;

		if (TransactionIdIsNormal(dbform->datvacuumxid))
		{
			if (TransactionIdPrecedes(myXID, dbform->datvacuumxid))
				vacuumAlreadyWrapped = true;
			else if (TransactionIdPrecedes(dbform->datvacuumxid, vacuumXID))
				vacuumXID = dbform->datvacuumxid;
		}
		if (TransactionIdIsNormal(dbform->datfrozenxid))
		{
			if (TransactionIdPrecedes(myXID, dbform->datfrozenxid))
				frozenAlreadyWrapped = true;
			else if (TransactionIdPrecedes(dbform->datfrozenxid, frozenXID))
				frozenXID = dbform->datfrozenxid;
		}
	}

	heap_endscan(scan);

	heap_close(relation, AccessShareLock);

	/*
	 * Do not truncate CLOG if we seem to have suffered wraparound
	 * already; the computed minimum XID might be bogus.
	 */
	if (vacuumAlreadyWrapped)
	{
		ereport(WARNING,
				(errmsg("some databases have not been vacuumed in over 2 billion transactions"),
				 errdetail("You may have already suffered transaction-wraparound data loss.")));
		return;
	}

	/* Truncate CLOG to the oldest vacuumxid */
	TruncateCLOG(vacuumXID);

	/* Give warning about impending wraparound problems */
	if (frozenAlreadyWrapped)
	{
		ereport(WARNING,
				(errmsg("some databases have not been vacuumed in over 1 billion transactions"),
				 errhint("Better vacuum them soon, or you may have a wraparound failure.")));
	}
	else
	{
		age = (int32) (myXID - frozenXID);
		if (age > (int32) ((MaxTransactionId >> 3) * 3))
			ereport(WARNING,
					(errmsg("some databases have not been vacuumed in %d transactions",
							age),
					 errhint("Better vacuum them within %d transactions, "
							 "or you may have a wraparound failure.",
							 (int32) (MaxTransactionId >> 1) - age)));
	}
}


/****************************************************************************
 *																			*
 *			Code common to both flavors of VACUUM							*
 *																			*
 ****************************************************************************
 */


/*
 *	vacuum_rel() -- vacuum one heap relation
 *
 *		Returns TRUE if we actually processed the relation (or can ignore it
 *		for some reason), FALSE if we failed to process it due to permissions
 *		or other reasons.  (A FALSE result really means that some data
 *		may have been left unvacuumed, so we can't update XID stats.)
 *
 *		Doing one heap at a time incurs extra overhead, since we need to
 *		check that the heap exists again just before we vacuum it.	The
 *		reason that we do this is so that vacuuming can be spread across
 *		many small transactions.  Otherwise, two-phase locking would require
 *		us to lock the entire database during one pass of the vacuum cleaner.
 *
 *		At entry and exit, we are not inside a transaction.
 */
static bool
vacuum_rel(Oid relid, VacuumStmt *vacstmt, char expected_relkind)
{
	LOCKMODE	lmode;
	Relation	onerel;
	LockRelId	onerelid;
	Oid			toast_relid;
	bool		result;

	/* Begin a transaction for vacuuming this relation */
	StartTransactionCommand();
	SetQuerySnapshot();			/* might be needed for functions in
								 * indexes */

	/*
	 * Check for user-requested abort.	Note we want this to be inside a
	 * transaction, so xact.c doesn't issue useless WARNING.
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Race condition -- if the pg_class tuple has gone away since the
	 * last time we saw it, we don't need to vacuum it.
	 */
	if (!SearchSysCacheExists(RELOID,
							  ObjectIdGetDatum(relid),
							  0, 0, 0))
	{
		CommitTransactionCommand();
		return true;			/* okay 'cause no data there */
	}

	/*
	 * Determine the type of lock we want --- hard exclusive lock for a
	 * FULL vacuum, but just ShareUpdateExclusiveLock for concurrent
	 * vacuum.	Either way, we can be sure that no other backend is
	 * vacuuming the same table.
	 */
	lmode = vacstmt->full ? AccessExclusiveLock : ShareUpdateExclusiveLock;

	/*
	 * Open the class, get an appropriate lock on it, and check
	 * permissions.
	 *
	 * We allow the user to vacuum a table if he is superuser, the table
	 * owner, or the database owner (but in the latter case, only if it's
	 * not a shared relation).	pg_class_ownercheck includes the superuser
	 * case.
	 *
	 * Note we choose to treat permissions failure as a WARNING and keep
	 * trying to vacuum the rest of the DB --- is this appropriate?
	 */
	onerel = relation_open(relid, lmode);

	if (!(pg_class_ownercheck(RelationGetRelid(onerel), GetUserId()) ||
		  (pg_database_ownercheck(MyDatabaseId, GetUserId()) && !onerel->rd_rel->relisshared)))
	{
		ereport(WARNING,
				(errmsg("skipping \"%s\" --- only table or database owner can vacuum it",
						RelationGetRelationName(onerel))));
		relation_close(onerel, lmode);
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Check that it's a plain table; we used to do this in getrels() but
	 * seems safer to check after we've locked the relation.
	 */
	if (onerel->rd_rel->relkind != expected_relkind)
	{
		ereport(WARNING,
				(errmsg("skipping \"%s\" --- cannot vacuum indexes, views, or special system tables",
						RelationGetRelationName(onerel))));
		relation_close(onerel, lmode);
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Silently ignore tables that are temp tables of other backends ---
	 * trying to vacuum these will lead to great unhappiness, since their
	 * contents are probably not up-to-date on disk.  (We don't throw a
	 * warning here; it would just lead to chatter during a database-wide
	 * VACUUM.)
	 */
	if (isOtherTempNamespace(RelationGetNamespace(onerel)))
	{
		relation_close(onerel, lmode);
		CommitTransactionCommand();
		return true;			/* assume no long-lived data in temp
								 * tables */
	}

	/*
	 * Get a session-level lock too. This will protect our access to the
	 * relation across multiple transactions, so that we can vacuum the
	 * relation's TOAST table (if any) secure in the knowledge that no one
	 * is deleting the parent relation.
	 *
	 * NOTE: this cannot block, even if someone else is waiting for access,
	 * because the lock manager knows that both lock requests are from the
	 * same process.
	 */
	onerelid = onerel->rd_lockInfo.lockRelId;
	LockRelationForSession(&onerelid, lmode);

	/*
	 * Remember the relation's TOAST relation for later
	 */
	toast_relid = onerel->rd_rel->reltoastrelid;

	/*
	 * Do the actual work --- either FULL or "lazy" vacuum
	 */
	if (vacstmt->full)
		full_vacuum_rel(onerel, vacstmt);
	else
		lazy_vacuum_rel(onerel, vacstmt);

	result = true;				/* did the vacuum */

	/* all done with this class, but hold lock until commit */
	relation_close(onerel, NoLock);

	/*
	 * Complete the transaction and free all temporary memory used.
	 */
	CommitTransactionCommand();

	/*
	 * If the relation has a secondary toast rel, vacuum that too while we
	 * still hold the session lock on the master table.  Note however that
	 * "analyze" will not get done on the toast table.	This is good,
	 * because the toaster always uses hardcoded index access and
	 * statistics are totally unimportant for toast relations.
	 */
	if (toast_relid != InvalidOid)
	{
		if (!vacuum_rel(toast_relid, vacstmt, RELKIND_TOASTVALUE))
			result = false;		/* failed to vacuum the TOAST table? */
	}

	/*
	 * Now release the session-level lock on the master table.
	 */
	UnlockRelationForSession(&onerelid, lmode);

	return result;
}


/****************************************************************************
 *																			*
 *			Code for VACUUM FULL (only)										*
 *																			*
 ****************************************************************************
 */


/*
 *	full_vacuum_rel() -- perform FULL VACUUM for one heap relation
 *
 *		This routine vacuums a single heap, cleans out its indexes, and
 *		updates its num_pages and num_tuples statistics.
 *
 *		At entry, we have already established a transaction and opened
 *		and locked the relation.
 */
static void
full_vacuum_rel(Relation onerel, VacuumStmt *vacstmt)
{
	VacPageListData vacuum_pages;		/* List of pages to vacuum and/or
										 * clean indexes */
	VacPageListData fraged_pages;		/* List of pages with space enough
										 * for re-using */
	Relation   *Irel;
	int			nindexes,
				i;
	VRelStats  *vacrelstats;

	vacuum_set_xid_limits(vacstmt, onerel->rd_rel->relisshared,
						  &OldestXmin, &FreezeLimit);

	/*
	 * Set up statistics-gathering machinery.
	 */
	vacrelstats = (VRelStats *) palloc(sizeof(VRelStats));
	vacrelstats->rel_pages = 0;
	vacrelstats->rel_tuples = 0;
	vacrelstats->hasindex = false;

	/* scan the heap */
	vacuum_pages.num_pages = fraged_pages.num_pages = 0;
	scan_heap(vacrelstats, onerel, &vacuum_pages, &fraged_pages);

	/* Now open all indexes of the relation */
	vac_open_indexes(onerel, &nindexes, &Irel);
	if (nindexes > 0)
		vacrelstats->hasindex = true;

	/* Clean/scan index relation(s) */
	if (Irel != (Relation *) NULL)
	{
		if (vacuum_pages.num_pages > 0)
		{
			for (i = 0; i < nindexes; i++)
				vacuum_index(&vacuum_pages, Irel[i],
							 vacrelstats->rel_tuples, 0);
		}
		else
		{
			/* just scan indexes to update statistic */
			for (i = 0; i < nindexes; i++)
				scan_index(Irel[i], vacrelstats->rel_tuples);
		}
	}

	if (fraged_pages.num_pages > 0)
	{
		/* Try to shrink heap */
		repair_frag(vacrelstats, onerel, &vacuum_pages, &fraged_pages,
					nindexes, Irel);
		vac_close_indexes(nindexes, Irel);
	}
	else
	{
		vac_close_indexes(nindexes, Irel);
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
			i = FlushRelationBuffers(onerel, vacrelstats->rel_pages);
			if (i < 0)
				elog(ERROR, "FlushRelationBuffers returned %d", i);
		}
	}

	/* update shared free space map with final free space info */
	vac_update_fsm(onerel, &fraged_pages, vacrelstats->rel_pages);

	/* update statistics in pg_class */
	vac_update_relstats(RelationGetRelid(onerel), vacrelstats->rel_pages,
						vacrelstats->rel_tuples, vacrelstats->hasindex);
}


/*
 *	scan_heap() -- scan an open heap relation
 *
 *		This routine sets commit status bits, constructs vacuum_pages (list
 *		of pages we need to compact free space on and/or clean indexes of
 *		deleted tuples), constructs fraged_pages (list of pages with free
 *		space that tuples could be moved into), and calculates statistics
 *		on the number of live tuples in the heap.
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
	OffsetNumber offnum,
				maxoff;
	bool		pgchanged,
				tupgone,
				notup;
	char	   *relname;
	VacPage		vacpage,
				vacpagecopy;
	BlockNumber empty_pages,
				empty_end_pages;
	double		num_tuples,
				tups_vacuumed,
				nkeep,
				nunused;
	double		free_space,
				usable_free_space;
	Size		min_tlen = MaxTupleSize;
	Size		max_tlen = 0;
	int			i;
	bool		do_shrinking = true;
	VTupleLink	vtlinks = (VTupleLink) palloc(100 * sizeof(VTupleLinkData));
	int			num_vtlinks = 0;
	int			free_vtlinks = 100;
	VacRUsage	ru0;

	vac_init_rusage(&ru0);

	relname = RelationGetRelationName(onerel);
	ereport(elevel,
			(errmsg("vacuuming \"%s.%s\"",
					get_namespace_name(RelationGetNamespace(onerel)),
					relname)));

	empty_pages = empty_end_pages = 0;
	num_tuples = tups_vacuumed = nkeep = nunused = 0;
	free_space = 0;

	nblocks = RelationGetNumberOfBlocks(onerel);

	/*
	 * We initially create each VacPage item in a maximal-sized workspace,
	 * then copy the workspace into a just-large-enough copy.
	 */
	vacpage = (VacPage) palloc(sizeof(VacPageData) + MaxOffsetNumber * sizeof(OffsetNumber));

	for (blkno = 0; blkno < nblocks; blkno++)
	{
		Page		page,
					tempPage = NULL;
		bool		do_reap,
					do_frag;

		CHECK_FOR_INTERRUPTS();

		buf = ReadBuffer(onerel, blkno);
		page = BufferGetPage(buf);

		vacpage->blkno = blkno;
		vacpage->offsets_used = 0;
		vacpage->offsets_free = 0;

		if (PageIsNew(page))
		{
			ereport(WARNING,
			(errmsg("relation \"%s\" page %u is uninitialized --- fixing",
					relname, blkno)));
			PageInit(page, BufferGetPageSize(buf), 0);
			vacpage->free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			free_space += vacpage->free;
			empty_pages++;
			empty_end_pages++;
			vacpagecopy = copy_vac_page(vacpage);
			vpage_insert(vacuum_pages, vacpagecopy);
			vpage_insert(fraged_pages, vacpagecopy);
			WriteBuffer(buf);
			continue;
		}

		if (PageIsEmpty(page))
		{
			vacpage->free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			free_space += vacpage->free;
			empty_pages++;
			empty_end_pages++;
			vacpagecopy = copy_vac_page(vacpage);
			vpage_insert(vacuum_pages, vacpagecopy);
			vpage_insert(fraged_pages, vacpagecopy);
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
			uint16		sv_infomask;

			itemid = PageGetItemId(page, offnum);

			/*
			 * Collect un-used items too - it's possible to have indexes
			 * pointing here after crash.
			 */
			if (!ItemIdIsUsed(itemid))
			{
				vacpage->offsets[vacpage->offsets_free++] = offnum;
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

					/*
					 * If we do shrinking and this tuple is updated one
					 * then remember it to construct updated tuple
					 * dependencies.
					 */
					if (do_shrinking &&
						!(ItemPointerEquals(&(tuple.t_self),
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
					break;
				case HEAPTUPLE_INSERT_IN_PROGRESS:

					/*
					 * This should not happen, since we hold exclusive
					 * lock on the relation; shouldn't we raise an error?
					 * (Actually, it can happen in system catalogs, since
					 * we tend to release write lock before commit there.)
					 */
					ereport(NOTICE,
							(errmsg("relation \"%s\" TID %u/%u: InsertTransactionInProgress %u --- can't shrink relation",
									relname, blkno, offnum, HeapTupleHeaderGetXmin(tuple.t_data))));
					do_shrinking = false;
					break;
				case HEAPTUPLE_DELETE_IN_PROGRESS:

					/*
					 * This should not happen, since we hold exclusive
					 * lock on the relation; shouldn't we raise an error?
					 * (Actually, it can happen in system catalogs, since
					 * we tend to release write lock before commit there.)
					 */
					ereport(NOTICE,
							(errmsg("relation \"%s\" TID %u/%u: DeleteTransactionInProgress %u --- can't shrink relation",
									relname, blkno, offnum, HeapTupleHeaderGetXmax(tuple.t_data))));
					do_shrinking = false;
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
				ItemId		lpp;

				/*
				 * Here we are building a temporary copy of the page with
				 * dead tuples removed.  Below we will apply
				 * PageRepairFragmentation to the copy, so that we can
				 * determine how much space will be available after
				 * removal of dead tuples.	But note we are NOT changing
				 * the real page yet...
				 */
				if (tempPage == (Page) NULL)
				{
					Size		pageSize;

					pageSize = PageGetPageSize(page);
					tempPage = (Page) palloc(pageSize);
					memcpy(tempPage, page, pageSize);
				}

				/* mark it unused on the temp page */
				lpp = PageGetItemId(tempPage, offnum);
				lpp->lp_flags &= ~LP_USED;

				vacpage->offsets[vacpage->offsets_free++] = offnum;
				tups_vacuumed += 1;
			}
			else
			{
				num_tuples += 1;
				notup = false;
				if (tuple.t_len < min_tlen)
					min_tlen = tuple.t_len;
				if (tuple.t_len > max_tlen)
					max_tlen = tuple.t_len;
			}
		}						/* scan along page */

		if (tempPage != (Page) NULL)
		{
			/* Some tuples are removable; figure free space after removal */
			PageRepairFragmentation(tempPage, NULL);
			vacpage->free = ((PageHeader) tempPage)->pd_upper - ((PageHeader) tempPage)->pd_lower;
			pfree(tempPage);
			do_reap = true;
		}
		else
		{
			/* Just use current available space */
			vacpage->free = ((PageHeader) page)->pd_upper - ((PageHeader) page)->pd_lower;
			/* Need to reap the page if it has ~LP_USED line pointers */
			do_reap = (vacpage->offsets_free > 0);
		}

		free_space += vacpage->free;

		/*
		 * Add the page to fraged_pages if it has a useful amount of free
		 * space.  "Useful" means enough for a minimal-sized tuple. But we
		 * don't know that accurately near the start of the relation, so
		 * add pages unconditionally if they have >= BLCKSZ/10 free space.
		 */
		do_frag = (vacpage->free >= min_tlen || vacpage->free >= BLCKSZ / 10);

		if (do_reap || do_frag)
		{
			vacpagecopy = copy_vac_page(vacpage);
			if (do_reap)
				vpage_insert(vacuum_pages, vacpagecopy);
			if (do_frag)
				vpage_insert(fraged_pages, vacpagecopy);
		}

		/*
		 * Include the page in empty_end_pages if it will be empty after
		 * vacuuming; this is to keep us from using it as a move
		 * destination.
		 */
		if (notup)
		{
			empty_pages++;
			empty_end_pages++;
		}
		else
			empty_end_pages = 0;

		if (pgchanged)
			WriteBuffer(buf);
		else
			ReleaseBuffer(buf);
	}

	pfree(vacpage);

	/* save stats in the rel list for use later */
	vacrelstats->rel_tuples = num_tuples;
	vacrelstats->rel_pages = nblocks;
	if (num_tuples == 0)
		min_tlen = max_tlen = 0;
	vacrelstats->min_tlen = min_tlen;
	vacrelstats->max_tlen = max_tlen;

	vacuum_pages->empty_end_pages = empty_end_pages;
	fraged_pages->empty_end_pages = empty_end_pages;

	/*
	 * Clear the fraged_pages list if we found we couldn't shrink. Else,
	 * remove any "empty" end-pages from the list, and compute usable free
	 * space = free space in remaining pages.
	 */
	if (do_shrinking)
	{
		Assert((BlockNumber) fraged_pages->num_pages >= empty_end_pages);
		fraged_pages->num_pages -= empty_end_pages;
		usable_free_space = 0;
		for (i = 0; i < fraged_pages->num_pages; i++)
			usable_free_space += fraged_pages->pagedesc[i]->free;
	}
	else
	{
		fraged_pages->num_pages = 0;
		usable_free_space = 0;
	}

	/* don't bother to save vtlinks if we will not call repair_frag */
	if (fraged_pages->num_pages > 0 && num_vtlinks > 0)
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

	ereport(elevel,
			(errmsg("\"%s\": found %.0f removable, %.0f nonremovable row versions in %u pages",
					RelationGetRelationName(onerel),
					tups_vacuumed, num_tuples, nblocks),
			 errdetail("%.0f dead row versions cannot be removed yet.\n"
				"Nonremovable row versions range from %lu to %lu bytes long.\n"
					   "There were %.0f unused item pointers.\n"
		 "Total free space (including removable row versions) is %.0f bytes.\n"
					   "%u pages are or will become empty, including %u at the end of the table.\n"
					   "%u pages containing %.0f free bytes are potential move destinations.\n"
					   "%s",
					   nkeep,
					   (unsigned long) min_tlen, (unsigned long) max_tlen,
					   nunused,
					   free_space,
					   empty_pages, empty_end_pages,
					   fraged_pages->num_pages, usable_free_space,
					   vac_show_rusage(&ru0))));
}


/*
 *	repair_frag() -- try to repair relation's fragmentation
 *
 *		This routine marks dead tuples as unused and tries re-use dead space
 *		by moving tuples (and inserting indexes if needed). It constructs
 *		Nvacpagelist list of free-ed pages (moved tuples) and clean indexes
 *		for them after committing (in hack-manner - without losing locks
 *		and freeing memory!) current transaction. It truncates relation
 *		if some end-blocks are gone away.
 */
static void
repair_frag(VRelStats *vacrelstats, Relation onerel,
			VacPageList vacuum_pages, VacPageList fraged_pages,
			int nindexes, Relation *Irel)
{
	TransactionId myXID;
	CommandId	myCID;
	Buffer		buf,
				cur_buffer;
	BlockNumber nblocks,
				blkno;
	BlockNumber last_move_dest_block = 0,
				last_vacuum_block;
	Page		page,
				ToPage = NULL;
	OffsetNumber offnum,
				maxoff,
				newoff,
				max_offset;
	ItemId		itemid,
				newitemid;
	HeapTupleData tuple,
				newtup;
	TupleDesc	tupdesc;
	ResultRelInfo *resultRelInfo;
	EState	   *estate;
	TupleTable	tupleTable;
	TupleTableSlot *slot;
	VacPageListData Nvacpagelist;
	VacPage		cur_page = NULL,
				last_vacuum_page,
				vacpage,
			   *curpage;
	int			cur_item = 0;
	int			i;
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
	VacRUsage	ru0;

	vac_init_rusage(&ru0);

	myXID = GetCurrentTransactionId();
	myCID = GetCurrentCommandId();

	tupdesc = RelationGetDescr(onerel);

	/*
	 * We need a ResultRelInfo and an EState so we can use the regular
	 * executor's index-entry-making machinery.
	 */
	estate = CreateExecutorState();

	resultRelInfo = makeNode(ResultRelInfo);
	resultRelInfo->ri_RangeTableIndex = 1;		/* dummy */
	resultRelInfo->ri_RelationDesc = onerel;
	resultRelInfo->ri_TrigDesc = NULL;	/* we don't fire triggers */

	ExecOpenIndices(resultRelInfo);

	estate->es_result_relations = resultRelInfo;
	estate->es_num_result_relations = 1;
	estate->es_result_relation_info = resultRelInfo;

	/* Set up a dummy tuple table too */
	tupleTable = ExecCreateTupleTable(1);
	slot = ExecAllocTableSlot(tupleTable);
	ExecSetSlotDescriptor(slot, tupdesc, false);

	Nvacpagelist.num_pages = 0;
	num_fraged_pages = fraged_pages->num_pages;
	Assert((BlockNumber) vacuum_pages->num_pages >= vacuum_pages->empty_end_pages);
	vacuumed_pages = vacuum_pages->num_pages - vacuum_pages->empty_end_pages;
	if (vacuumed_pages > 0)
	{
		/* get last reaped page from vacuum_pages */
		last_vacuum_page = vacuum_pages->pagedesc[vacuumed_pages - 1];
		last_vacuum_block = last_vacuum_page->blkno;
	}
	else
	{
		last_vacuum_page = NULL;
		last_vacuum_block = InvalidBlockNumber;
	}
	cur_buffer = InvalidBuffer;
	num_moved = 0;

	vacpage = (VacPage) palloc(sizeof(VacPageData) + MaxOffsetNumber * sizeof(OffsetNumber));
	vacpage->offsets_used = vacpage->offsets_free = 0;

	/*
	 * Scan pages backwards from the last nonempty page, trying to move
	 * tuples down to lower pages.	Quit when we reach a page that we have
	 * moved any tuples onto, or the first page if we haven't moved
	 * anything, or when we find a page we cannot completely empty (this
	 * last condition is handled by "break" statements within the loop).
	 *
	 * NB: this code depends on the vacuum_pages and fraged_pages lists being
	 * in order by blkno.
	 */
	nblocks = vacrelstats->rel_pages;
	for (blkno = nblocks - vacuum_pages->empty_end_pages - 1;
		 blkno > last_move_dest_block;
		 blkno--)
	{
		CHECK_FOR_INTERRUPTS();

		/*
		 * Forget fraged_pages pages at or after this one; they're no
		 * longer useful as move targets, since we only want to move down.
		 * Note that since we stop the outer loop at last_move_dest_block,
		 * pages removed here cannot have had anything moved onto them
		 * already.
		 *
		 * Also note that we don't change the stored fraged_pages list, only
		 * our local variable num_fraged_pages; so the forgotten pages are
		 * still available to be loaded into the free space map later.
		 */
		while (num_fraged_pages > 0 &&
			fraged_pages->pagedesc[num_fraged_pages - 1]->blkno >= blkno)
		{
			Assert(fraged_pages->pagedesc[num_fraged_pages - 1]->offsets_used == 0);
			--num_fraged_pages;
		}

		/*
		 * Process this page of relation.
		 */
		buf = ReadBuffer(onerel, blkno);
		page = BufferGetPage(buf);

		vacpage->offsets_free = 0;

		isempty = PageIsEmpty(page);

		dowrite = false;

		/* Is the page in the vacuum_pages list? */
		if (blkno == last_vacuum_block)
		{
			if (last_vacuum_page->offsets_free > 0)
			{
				/* there are dead tuples on this page - clean them */
				Assert(!isempty);
				LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
				vacuum_page(onerel, buf, last_vacuum_page);
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
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
				last_vacuum_block = InvalidBlockNumber;
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
				if (tuple.t_data->t_infomask & HEAP_MOVED_IN)
					elog(ERROR, "HEAP_MOVED_IN was not expected");

				/*
				 * If this (chain) tuple is moved by me already then I
				 * have to check is it in vacpage or not - i.e. is it
				 * moved while cleaning this page or some previous one.
				 */
				if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
				{
					if (HeapTupleHeaderGetXvac(tuple.t_data) != myXID)
						elog(ERROR, "invalid XVAC in tuple header");
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
			 *
			 * NOTE: this test is not 100% accurate: it is possible for a
			 * tuple to be an updated one with recent xmin, and yet not
			 * have a corresponding tuple in the vtlinks list.	Presumably
			 * there was once a parent tuple with xmax matching the xmin,
			 * but it's possible that that tuple has been removed --- for
			 * example, if it had xmin = xmax then
			 * HeapTupleSatisfiesVacuum would deem it removable as soon as
			 * the xmin xact completes.
			 *
			 * To be on the safe side, we abandon the repair_frag process if
			 * we cannot find the parent tuple in vtlinks.	This may be
			 * overly conservative; AFAICS it would be safe to move the
			 * chain.
			 */
			if (((tuple.t_data->t_infomask & HEAP_UPDATED) &&
			 !TransactionIdPrecedes(HeapTupleHeaderGetXmin(tuple.t_data),
									OldestXmin)) ||
				(!(tuple.t_data->t_infomask & (HEAP_XMAX_INVALID |
											   HEAP_MARKED_FOR_UPDATE)) &&
				 !(ItemPointerEquals(&(tuple.t_self),
									 &(tuple.t_data->t_ctid)))))
			{
				Buffer		Cbuf = buf;
				bool		freeCbuf = false;
				bool		chain_move_failed = false;
				ItemPointerData Ctid;
				HeapTupleData tp = tuple;
				Size		tlen = tuple_len;
				VTupleMove	vtmove;
				int			num_vtmove;
				int			free_vtmove;
				VacPage		to_vacpage = NULL;
				int			to_item = 0;
				int			ti;

				if (cur_buffer != InvalidBuffer)
				{
					WriteBuffer(cur_buffer);
					cur_buffer = InvalidBuffer;
				}

				/* Quick exit if we have no vtlinks to search in */
				if (vacrelstats->vtlinks == NULL)
				{
					elog(DEBUG2, "parent item in update-chain not found --- can't continue repair_frag");
					break;		/* out of walk-along-page loop */
				}

				/*
				 * If this tuple is in the begin/middle of the chain then
				 * we have to move to the end of chain.  As with any
				 * t_ctid chase, we have to verify that each new tuple
				 * is really the descendant of the tuple we came from.
				 */
				while (!(tp.t_data->t_infomask & (HEAP_XMAX_INVALID |
											  HEAP_MARKED_FOR_UPDATE)) &&
					   !(ItemPointerEquals(&(tp.t_self),
										   &(tp.t_data->t_ctid))))
				{
					ItemPointerData nextTid;
					TransactionId priorXmax;
					Buffer		nextBuf;
					Page		nextPage;
					OffsetNumber nextOffnum;
					ItemId		nextItemid;
					HeapTupleHeader nextTdata;

					nextTid = tp.t_data->t_ctid;
					priorXmax = HeapTupleHeaderGetXmax(tp.t_data);
					/* assume block# is OK (see heap_fetch comments) */
					nextBuf = ReadBuffer(onerel,
										 ItemPointerGetBlockNumber(&nextTid));
					nextPage = BufferGetPage(nextBuf);
					/* If bogus or unused slot, assume tp is end of chain */
					nextOffnum = ItemPointerGetOffsetNumber(&nextTid);
					if (nextOffnum < FirstOffsetNumber ||
						nextOffnum > PageGetMaxOffsetNumber(nextPage))
					{
						ReleaseBuffer(nextBuf);
						break;
					}
					nextItemid = PageGetItemId(nextPage, nextOffnum);
					if (!ItemIdIsUsed(nextItemid))
					{
						ReleaseBuffer(nextBuf);
						break;
					}
					/* if not matching XMIN, assume tp is end of chain */
					nextTdata = (HeapTupleHeader) PageGetItem(nextPage,
															  nextItemid);
					if (!TransactionIdEquals(HeapTupleHeaderGetXmin(nextTdata),
											 priorXmax))
					{
						ReleaseBuffer(nextBuf);
						break;
					}
					/* OK, switch our attention to the next tuple in chain */
					tp.t_datamcxt = NULL;
					tp.t_data = nextTdata;
					tp.t_self = nextTid;
					tlen = tp.t_len = ItemIdGetLength(nextItemid);
					if (freeCbuf)
						ReleaseBuffer(Cbuf);
					Cbuf = nextBuf;
					freeCbuf = true;
				}

				/* Set up workspace for planning the chain move */
				vtmove = (VTupleMove) palloc(100 * sizeof(VTupleMoveData));
				num_vtmove = 0;
				free_vtmove = 100;

				/*
				 * Now, walk backwards up the chain (towards older tuples)
				 * and check if all items in chain can be moved.  We record
				 * all the moves that need to be made in the vtmove array.
				 */
				for (;;)
				{
					Buffer		Pbuf;
					Page		Ppage;
					ItemId		Pitemid;
					HeapTupleHeader PTdata;
					VTupleLinkData vtld,
							   *vtlp;

					/* Identify a target page to move this tuple to */
					if (to_vacpage == NULL ||
						!enough_space(to_vacpage, tlen))
					{
						for (i = 0; i < num_fraged_pages; i++)
						{
							if (enough_space(fraged_pages->pagedesc[i], tlen))
								break;
						}

						if (i == num_fraged_pages)
						{
							/* can't move item anywhere */
							chain_move_failed = true;
							break;		/* out of check-all-items loop */
						}
						to_item = i;
						to_vacpage = fraged_pages->pagedesc[to_item];
					}
					to_vacpage->free -= MAXALIGN(tlen);
					if (to_vacpage->offsets_used >= to_vacpage->offsets_free)
						to_vacpage->free -= sizeof(ItemIdData);
					(to_vacpage->offsets_used)++;
					if (free_vtmove == 0)
					{
						free_vtmove = 1000;
						vtmove = (VTupleMove)
							repalloc(vtmove,
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

					/* At beginning of chain? */
					if (!(tp.t_data->t_infomask & HEAP_UPDATED) ||
						TransactionIdPrecedes(HeapTupleHeaderGetXmin(tp.t_data),
											  OldestXmin))
						break;

					/* No, move to tuple with prior row version */
					vtld.new_tid = tp.t_self;
					vtlp = (VTupleLink)
						vac_bsearch((void *) &vtld,
									(void *) (vacrelstats->vtlinks),
									vacrelstats->num_vtlinks,
									sizeof(VTupleLinkData),
									vac_cmp_vtlinks);
					if (vtlp == NULL)
					{
						/* see discussion above */
						elog(DEBUG2, "parent item in update-chain not found --- can't continue repair_frag");
						chain_move_failed = true;
						break;	/* out of check-all-items loop */
					}
					tp.t_self = vtlp->this_tid;
					Pbuf = ReadBuffer(onerel,
								ItemPointerGetBlockNumber(&(tp.t_self)));
					Ppage = BufferGetPage(Pbuf);
					Pitemid = PageGetItemId(Ppage,
							   ItemPointerGetOffsetNumber(&(tp.t_self)));
					/* this can't happen since we saw tuple earlier: */
					if (!ItemIdIsUsed(Pitemid))
						elog(ERROR, "parent itemid marked as unused");
					PTdata = (HeapTupleHeader) PageGetItem(Ppage, Pitemid);

					/* ctid should not have changed since we saved it */
					Assert(ItemPointerEquals(&(vtld.new_tid),
											 &(PTdata->t_ctid)));

					/*
					 * Read above about cases when !ItemIdIsUsed(nextItemid)
					 * (child item is removed)... Due to the fact that at
					 * the moment we don't remove unuseful part of
					 * update-chain, it's possible to get non-matching parent
					 * row here. Like as in the case which caused this
					 * problem, we stop shrinking here. I could try to
					 * find real parent row but want not to do it because
					 * of real solution will be implemented anyway, later,
					 * and we are too close to 6.5 release. - vadim
					 * 06/11/99
					 */
					if (!(TransactionIdEquals(HeapTupleHeaderGetXmax(PTdata),
									 HeapTupleHeaderGetXmin(tp.t_data))))
					{
						ReleaseBuffer(Pbuf);
						elog(DEBUG2, "too old parent tuple found --- can't continue repair_frag");
						chain_move_failed = true;
						break;	/* out of check-all-items loop */
					}
					tp.t_datamcxt = NULL;
					tp.t_data = PTdata;
					tlen = tp.t_len = ItemIdGetLength(Pitemid);
					if (freeCbuf)
						ReleaseBuffer(Cbuf);
					Cbuf = Pbuf;
					freeCbuf = true;
				}				/* end of check-all-items loop */

				if (freeCbuf)
					ReleaseBuffer(Cbuf);
				freeCbuf = false;

				if (chain_move_failed)
				{
					/*
					 * Undo changes to offsets_used state.	We don't
					 * bother cleaning up the amount-free state, since
					 * we're not going to do any further tuple motion.
					 */
					for (i = 0; i < num_vtmove; i++)
					{
						Assert(vtmove[i].vacpage->offsets_used > 0);
						(vtmove[i].vacpage->offsets_used)--;
					}
					pfree(vtmove);
					break;		/* out of walk-along-page loop */
				}

				/*
				 * Okay, move the whle tuple chain
				 */
				ItemPointerSetInvalid(&Ctid);
				for (ti = 0; ti < num_vtmove; ti++)
				{
					VacPage		destvacpage = vtmove[ti].vacpage;
					Page		Cpage;
					ItemId		Citemid;

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

					/*
					 * register invalidation of source tuple in catcaches.
					 */
					CacheInvalidateHeapTuple(onerel, &tuple);

					/* NO EREPORT(ERROR) TILL CHANGES ARE LOGGED */
					START_CRIT_SECTION();

					tuple.t_data->t_infomask &= ~(HEAP_XMIN_COMMITTED |
												  HEAP_XMIN_INVALID |
												  HEAP_MOVED_IN);
					tuple.t_data->t_infomask |= HEAP_MOVED_OFF;
					HeapTupleHeaderSetXvac(tuple.t_data, myXID);

					/*
					 * If this page was not used before - clean it.
					 *
					 * NOTE: a nasty bug used to lurk here.  It is possible
					 * for the source and destination pages to be the same
					 * (since this tuple-chain member can be on a page
					 * lower than the one we're currently processing in
					 * the outer loop).  If that's true, then after
					 * vacuum_page() the source tuple will have been
					 * moved, and tuple.t_data will be pointing at
					 * garbage.  Therefore we must do everything that uses
					 * tuple.t_data BEFORE this step!!
					 *
					 * This path is different from the other callers of
					 * vacuum_page, because we have already incremented
					 * the vacpage's offsets_used field to account for the
					 * tuple(s) we expect to move onto the page. Therefore
					 * vacuum_page's check for offsets_used == 0 is wrong.
					 * But since that's a good debugging check for all
					 * other callers, we work around it here rather than
					 * remove it.
					 */
					if (!PageIsEmpty(ToPage) && vtmove[ti].cleanVpd)
					{
						int			sv_offsets_used = destvacpage->offsets_used;

						destvacpage->offsets_used = 0;
						vacuum_page(onerel, cur_buffer, destvacpage);
						destvacpage->offsets_used = sv_offsets_used;
					}

					/*
					 * Update the state of the copied tuple, and store it
					 * on the destination page.
					 */
					newtup.t_data->t_infomask &= ~(HEAP_XMIN_COMMITTED |
												   HEAP_XMIN_INVALID |
												   HEAP_MOVED_OFF);
					newtup.t_data->t_infomask |= HEAP_MOVED_IN;
					HeapTupleHeaderSetXvac(newtup.t_data, myXID);
					newoff = PageAddItem(ToPage,
										 (Item) newtup.t_data,
										 tuple_len,
										 InvalidOffsetNumber,
										 LP_USED);
					if (newoff == InvalidOffsetNumber)
						elog(PANIC, "failed to add item with len = %lu to page %u while moving tuple chain",
						  (unsigned long) tuple_len, destvacpage->blkno);
					newitemid = PageGetItemId(ToPage, newoff);
					/* drop temporary copy, and point to the version on the dest page */
					pfree(newtup.t_data);
					newtup.t_datamcxt = NULL;
					newtup.t_data = (HeapTupleHeader) PageGetItem(ToPage, newitemid);

					ItemPointerSet(&(newtup.t_self), destvacpage->blkno, newoff);

					/*
					 * Set new tuple's t_ctid pointing to itself for last
					 * tuple in chain, and to next tuple in chain
					 * otherwise.
					 */
					if (!ItemPointerIsValid(&Ctid))
						newtup.t_data->t_ctid = newtup.t_self;
					else
						newtup.t_data->t_ctid = Ctid;
					Ctid = newtup.t_self;

					/* XLOG stuff */
					if (!onerel->rd_istemp)
					{
						XLogRecPtr	recptr =
						log_heap_move(onerel, Cbuf, tuple.t_self,
									  cur_buffer, &newtup);

						if (Cbuf != cur_buffer)
						{
							PageSetLSN(Cpage, recptr);
							PageSetSUI(Cpage, ThisStartUpID);
						}
						PageSetLSN(ToPage, recptr);
						PageSetSUI(ToPage, ThisStartUpID);
					}
					else
					{
						/*
						 * No XLOG record, but still need to flag that XID
						 * exists on disk
						 */
						MyXactMadeTempRelUpdate = true;
					}

					END_CRIT_SECTION();

					if (destvacpage->blkno > last_move_dest_block)
						last_move_dest_block = destvacpage->blkno;

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

					/* Create index entries for the moved tuple */
					if (resultRelInfo->ri_NumIndices > 0)
					{
						ExecStoreTuple(&newtup, slot, InvalidBuffer, false);
						ExecInsertIndexTuples(slot, &(newtup.t_self),
											  estate, true);
						ResetPerTupleExprContext(estate);
					}

					WriteBuffer(cur_buffer);
					WriteBuffer(Cbuf);
				}				/* end of move-the-tuple-chain loop */

				cur_buffer = InvalidBuffer;
				pfree(vtmove);
				chain_tuple_moved = true;

				/* advance to next tuple in walk-along-page loop */
				continue;
			}					/* end of is-tuple-in-chain test */

			/* try to find new page for this tuple */
			if (cur_buffer == InvalidBuffer ||
				!enough_space(cur_page, tuple_len))
			{
				if (cur_buffer != InvalidBuffer)
				{
					WriteBuffer(cur_buffer);
					cur_buffer = InvalidBuffer;
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
					vacuum_page(onerel, cur_buffer, cur_page);
			}
			else
				LockBuffer(cur_buffer, BUFFER_LOCK_EXCLUSIVE);

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			/* copy tuple */
			heap_copytuple_with_tuple(&tuple, &newtup);

			/*
			 * register invalidation of source tuple in catcaches.
			 *
			 * (Note: we do not need to register the copied tuple, because we
			 * are not changing the tuple contents and so there cannot be
			 * any need to flush negative catcache entries.)
			 */
			CacheInvalidateHeapTuple(onerel, &tuple);

			/* NO EREPORT(ERROR) TILL CHANGES ARE LOGGED */
			START_CRIT_SECTION();

			/*
			 * Mark new tuple as MOVED_IN by me.
			 */
			newtup.t_data->t_infomask &= ~(HEAP_XMIN_COMMITTED |
										   HEAP_XMIN_INVALID |
										   HEAP_MOVED_OFF);
			newtup.t_data->t_infomask |= HEAP_MOVED_IN;
			HeapTupleHeaderSetXvac(newtup.t_data, myXID);

			/* add tuple to the page */
			newoff = PageAddItem(ToPage, (Item) newtup.t_data, tuple_len,
								 InvalidOffsetNumber, LP_USED);
			if (newoff == InvalidOffsetNumber)
			{
				elog(PANIC, "failed to add item with len = %lu to page %u (free space %lu, nusd %u, noff %u)",
					 (unsigned long) tuple_len,
					 cur_page->blkno, (unsigned long) cur_page->free,
					 cur_page->offsets_used, cur_page->offsets_free);
			}
			newitemid = PageGetItemId(ToPage, newoff);
			pfree(newtup.t_data);
			newtup.t_datamcxt = NULL;
			newtup.t_data = (HeapTupleHeader) PageGetItem(ToPage, newitemid);
			ItemPointerSet(&(newtup.t_data->t_ctid), cur_page->blkno, newoff);
			newtup.t_self = newtup.t_data->t_ctid;

			/*
			 * Mark old tuple as MOVED_OFF by me.
			 */
			tuple.t_data->t_infomask &= ~(HEAP_XMIN_COMMITTED |
										  HEAP_XMIN_INVALID |
										  HEAP_MOVED_IN);
			tuple.t_data->t_infomask |= HEAP_MOVED_OFF;
			HeapTupleHeaderSetXvac(tuple.t_data, myXID);

			/* XLOG stuff */
			if (!onerel->rd_istemp)
			{
				XLogRecPtr	recptr =
				log_heap_move(onerel, buf, tuple.t_self,
							  cur_buffer, &newtup);

				PageSetLSN(page, recptr);
				PageSetSUI(page, ThisStartUpID);
				PageSetLSN(ToPage, recptr);
				PageSetSUI(ToPage, ThisStartUpID);
			}
			else
			{
				/*
				 * No XLOG record, but still need to flag that XID exists
				 * on disk
				 */
				MyXactMadeTempRelUpdate = true;
			}

			END_CRIT_SECTION();

			cur_page->offsets_used++;
			num_moved++;
			cur_page->free = ((PageHeader) ToPage)->pd_upper - ((PageHeader) ToPage)->pd_lower;
			if (cur_page->blkno > last_move_dest_block)
				last_move_dest_block = cur_page->blkno;

			vacpage->offsets[vacpage->offsets_free++] = offnum;

			LockBuffer(cur_buffer, BUFFER_LOCK_UNLOCK);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			/* insert index' tuples if needed */
			if (resultRelInfo->ri_NumIndices > 0)
			{
				ExecStoreTuple(&newtup, slot, InvalidBuffer, false);
				ExecInsertIndexTuples(slot, &(newtup.t_self), estate, true);
				ResetPerTupleExprContext(estate);
			}
		}						/* walk along page */

		/*
		 * If we broke out of the walk-along-page loop early (ie, still
		 * have offnum <= maxoff), then we failed to move some tuple off
		 * this page.  No point in shrinking any more, so clean up and
		 * exit the per-page loop.
		 */
		if (offnum < maxoff && keep_tuples > 0)
		{
			OffsetNumber off;

			/*
			 * Fix vacpage state for any unvisited tuples remaining on
			 * page
			 */
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
				if (tuple.t_data->t_infomask & HEAP_MOVED_IN)
					elog(ERROR, "HEAP_MOVED_IN was not expected");
				if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
				{
					if (HeapTupleHeaderGetXvac(tuple.t_data) != myXID)
						elog(ERROR, "invalid XVAC in tuple header");
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
				else
					elog(ERROR, "HEAP_MOVED_OFF was expected");
			}
		}

		if (vacpage->offsets_free > 0)	/* some tuples were moved */
		{
			if (chain_tuple_moved)		/* else - they are ordered */
			{
				qsort((char *) (vacpage->offsets), vacpage->offsets_free,
					  sizeof(OffsetNumber), vac_cmp_offno);
			}
			vpage_insert(&Nvacpagelist, copy_vac_page(vacpage));
			WriteBuffer(buf);
		}
		else if (dowrite)
			WriteBuffer(buf);
		else
			ReleaseBuffer(buf);

		if (offnum <= maxoff)
			break;				/* had to quit early, see above note */

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
		 * We have to commit our tuple movings before we truncate the
		 * relation.  Ideally we should do Commit/StartTransactionCommand
		 * here, relying on the session-level table lock to protect our
		 * exclusive access to the relation.  However, that would require
		 * a lot of extra code to close and re-open the relation, indexes,
		 * etc.  For now, a quick hack: record status of current
		 * transaction as committed, and continue.
		 */
		RecordTransactionCommit();
	}

	/*
	 * We are not going to move any more tuples across pages, but we still
	 * need to apply vacuum_page to compact free space in the remaining
	 * pages in vacuum_pages list.	Note that some of these pages may also
	 * be in the fraged_pages list, and may have had tuples moved onto
	 * them; if so, we already did vacuum_page and needn't do it again.
	 */
	for (i = 0, curpage = vacuum_pages->pagedesc;
		 i < vacuumed_pages;
		 i++, curpage++)
	{
		CHECK_FOR_INTERRUPTS();
		Assert((*curpage)->blkno < blkno);
		if ((*curpage)->offsets_used == 0)
		{
			/* this page was not used as a move target, so must clean it */
			buf = ReadBuffer(onerel, (*curpage)->blkno);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);
			if (!PageIsEmpty(page))
				vacuum_page(onerel, buf, *curpage);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			WriteBuffer(buf);
		}
	}

	/*
	 * Now scan all the pages that we moved tuples onto and update tuple
	 * status bits.  This is not really necessary, but will save time for
	 * future transactions examining these tuples.
	 *
	 * XXX NOTICE that this code fails to clear HEAP_MOVED_OFF tuples from
	 * pages that were move source pages but not move dest pages.  One
	 * also wonders whether it wouldn't be better to skip this step and
	 * let the tuple status updates happen someplace that's not holding an
	 * exclusive lock on the relation.
	 */
	checked_moved = 0;
	for (i = 0, curpage = fraged_pages->pagedesc;
		 i < num_fraged_pages;
		 i++, curpage++)
	{
		CHECK_FOR_INTERRUPTS();
		Assert((*curpage)->blkno < blkno);
		if ((*curpage)->blkno > last_move_dest_block)
			break;				/* no need to scan any further */
		if ((*curpage)->offsets_used == 0)
			continue;			/* this page was never used as a move dest */
		buf = ReadBuffer(onerel, (*curpage)->blkno);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
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
				if (!(tuple.t_data->t_infomask & HEAP_MOVED))
					elog(ERROR, "HEAP_MOVED_OFF/HEAP_MOVED_IN was expected");
				if (HeapTupleHeaderGetXvac(tuple.t_data) != myXID)
					elog(ERROR, "invalid XVAC in tuple header");
				if (tuple.t_data->t_infomask & HEAP_MOVED_IN)
				{
					tuple.t_data->t_infomask |= HEAP_XMIN_COMMITTED;
					tuple.t_data->t_infomask &= ~HEAP_MOVED;
					num_tuples++;
				}
				else
					tuple.t_data->t_infomask |= HEAP_XMIN_INVALID;
			}
		}
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		WriteBuffer(buf);
		Assert((*curpage)->offsets_used == num_tuples);
		checked_moved += num_tuples;
	}
	Assert(num_moved == checked_moved);

	/*
	 * It'd be cleaner to make this report at the bottom of this routine,
	 * but then the rusage would double-count the second pass of index
	 * vacuuming.  So do it here and ignore the relatively small amount of
	 * processing that occurs below.
	 */
	ereport(elevel,
			(errmsg("\"%s\": moved %u row versions, truncated %u to %u pages",
					RelationGetRelationName(onerel),
					num_moved, nblocks, blkno),
			 errdetail("%s",
					   vac_show_rusage(&ru0))));

	/*
	 * Reflect the motion of system tuples to catalog cache here.
	 */
	CommandCounterIncrement();

	if (Nvacpagelist.num_pages > 0)
	{
		/* vacuum indexes again if needed */
		if (Irel != (Relation *) NULL)
		{
			VacPage    *vpleft,
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
			for (i = 0; i < nindexes; i++)
				vacuum_index(&Nvacpagelist, Irel[i],
							 vacrelstats->rel_tuples, keep_tuples);
		}

		/* clean moved tuples from last page in Nvacpagelist list */
		if (vacpage->blkno == (blkno - 1) &&
			vacpage->offsets_free > 0)
		{
			OffsetNumber unused[BLCKSZ / sizeof(OffsetNumber)];
			int			uncnt;

			buf = ReadBuffer(onerel, vacpage->blkno);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buf);
			num_tuples = 0;
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

				if (!(tuple.t_data->t_infomask & HEAP_XMIN_COMMITTED))
				{
					if (tuple.t_data->t_infomask & HEAP_MOVED_OFF)
					{
						if (HeapTupleHeaderGetXvac(tuple.t_data) != myXID)
							elog(ERROR, "invalid XVAC in tuple header");
						itemid->lp_flags &= ~LP_USED;
						num_tuples++;
					}
					else
						elog(ERROR, "HEAP_MOVED_OFF was expected");
				}

			}
			Assert(vacpage->offsets_free == num_tuples);

			START_CRIT_SECTION();

			uncnt = PageRepairFragmentation(page, unused);

			/* XLOG stuff */
			if (!onerel->rd_istemp)
			{
				XLogRecPtr	recptr;

				recptr = log_heap_clean(onerel, buf, unused, uncnt);
				PageSetLSN(page, recptr);
				PageSetSUI(page, ThisStartUpID);
			}
			else
			{
				/*
				 * No XLOG record, but still need to flag that XID exists
				 * on disk
				 */
				MyXactMadeTempRelUpdate = true;
			}

			END_CRIT_SECTION();

			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
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
	 * we don't need to truncate, because we want to ensure that all
	 * tuples have correct on-row commit status on disk (see bufmgr.c's
	 * comments for FlushRelationBuffers()).
	 */
	i = FlushRelationBuffers(onerel, blkno);
	if (i < 0)
		elog(ERROR, "FlushRelationBuffers returned %d", i);

	/* truncate relation, if needed */
	if (blkno < nblocks)
	{
		blkno = smgrtruncate(DEFAULT_SMGR, onerel, blkno);
		onerel->rd_nblocks = blkno;		/* update relcache immediately */
		onerel->rd_targblock = InvalidBlockNumber;
		vacrelstats->rel_pages = blkno; /* set new number of blocks */
	}

	/* clean up */
	pfree(vacpage);
	if (vacrelstats->vtlinks != NULL)
		pfree(vacrelstats->vtlinks);

	ExecDropTupleTable(tupleTable, true);

	ExecCloseIndices(resultRelInfo);

	FreeExecutorState(estate);
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
	VacPage    *vacpage;
	BlockNumber relblocks;
	int			nblocks;
	int			i;

	nblocks = vacuum_pages->num_pages;
	nblocks -= vacuum_pages->empty_end_pages;	/* nothing to do with them */

	for (i = 0, vacpage = vacuum_pages->pagedesc; i < nblocks; i++, vacpage++)
	{
		CHECK_FOR_INTERRUPTS();
		if ((*vacpage)->offsets_free > 0)
		{
			buf = ReadBuffer(onerel, (*vacpage)->blkno);
			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
			vacuum_page(onerel, buf, *vacpage);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);
			WriteBuffer(buf);
		}
	}

	/*
	 * Flush dirty pages out to disk.  We do this unconditionally, even if
	 * we don't need to truncate, because we want to ensure that all
	 * tuples have correct on-row commit status on disk (see bufmgr.c's
	 * comments for FlushRelationBuffers()).
	 */
	Assert(vacrelstats->rel_pages >= vacuum_pages->empty_end_pages);
	relblocks = vacrelstats->rel_pages - vacuum_pages->empty_end_pages;

	i = FlushRelationBuffers(onerel, relblocks);
	if (i < 0)
		elog(ERROR, "FlushRelationBuffers returned %d", i);

	/* truncate relation if there are some empty end-pages */
	if (vacuum_pages->empty_end_pages > 0)
	{
		ereport(elevel,
				(errmsg("\"%s\": truncated %u to %u pages",
						RelationGetRelationName(onerel),
						vacrelstats->rel_pages, relblocks)));
		relblocks = smgrtruncate(DEFAULT_SMGR, onerel, relblocks);
		onerel->rd_nblocks = relblocks; /* update relcache immediately */
		onerel->rd_targblock = InvalidBlockNumber;
		vacrelstats->rel_pages = relblocks;		/* set new number of
												 * blocks */
	}
}

/*
 *	vacuum_page() -- free dead tuples on a page
 *					 and repair its fragmentation.
 */
static void
vacuum_page(Relation onerel, Buffer buffer, VacPage vacpage)
{
	OffsetNumber unused[BLCKSZ / sizeof(OffsetNumber)];
	int			uncnt;
	Page		page = BufferGetPage(buffer);
	ItemId		itemid;
	int			i;

	/* There shouldn't be any tuples moved onto the page yet! */
	Assert(vacpage->offsets_used == 0);

	START_CRIT_SECTION();

	for (i = 0; i < vacpage->offsets_free; i++)
	{
		itemid = PageGetItemId(page, vacpage->offsets[i]);
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
}

/*
 *	scan_index() -- scan one index relation to update statistic.
 *
 * We use this when we have no deletions to do.
 */
static void
scan_index(Relation indrel, double num_tuples)
{
	IndexBulkDeleteResult *stats;
	IndexVacuumCleanupInfo vcinfo;
	VacRUsage	ru0;

	vac_init_rusage(&ru0);

	/*
	 * Even though we're not planning to delete anything, we use the
	 * ambulkdelete call, because (a) the scan happens within the index AM
	 * for more speed, and (b) it may want to pass private statistics to
	 * the amvacuumcleanup call.
	 */
	stats = index_bulk_delete(indrel, dummy_tid_reaped, NULL);

	/* Do post-VACUUM cleanup, even though we deleted nothing */
	vcinfo.vacuum_full = true;
	vcinfo.message_level = elevel;

	stats = index_vacuum_cleanup(indrel, &vcinfo, stats);

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

	/*
	 * Check for tuple count mismatch.	If the index is partial, then it's
	 * OK for it to have fewer tuples than the heap; else we got trouble.
	 */
	if (stats->num_index_tuples != num_tuples)
	{
		if (stats->num_index_tuples > num_tuples ||
			!vac_is_partial_index(indrel))
			ereport(WARNING,
					(errmsg("index \"%s\" contains %.0f row versions, but table contains %.0f row versions",
							RelationGetRelationName(indrel),
							stats->num_index_tuples, num_tuples),
					 errhint("Rebuild the index with REINDEX.")));
	}

	pfree(stats);
}

/*
 *	vacuum_index() -- vacuum one index relation.
 *
 *		Vpl is the VacPageList of the heap we're currently vacuuming.
 *		It's locked. Indrel is an index relation on the vacuumed heap.
 *
 *		We don't bother to set locks on the index relation here, since
 *		the parent table is exclusive-locked already.
 *
 *		Finally, we arrange to update the index relation's statistics in
 *		pg_class.
 */
static void
vacuum_index(VacPageList vacpagelist, Relation indrel,
			 double num_tuples, int keep_tuples)
{
	IndexBulkDeleteResult *stats;
	IndexVacuumCleanupInfo vcinfo;
	VacRUsage	ru0;

	vac_init_rusage(&ru0);

	/* Do bulk deletion */
	stats = index_bulk_delete(indrel, tid_reaped, (void *) vacpagelist);

	/* Do post-VACUUM cleanup */
	vcinfo.vacuum_full = true;
	vcinfo.message_level = elevel;

	stats = index_vacuum_cleanup(indrel, &vcinfo, stats);

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

	/*
	 * Check for tuple count mismatch.	If the index is partial, then it's
	 * OK for it to have fewer tuples than the heap; else we got trouble.
	 */
	if (stats->num_index_tuples != num_tuples + keep_tuples)
	{
		if (stats->num_index_tuples > num_tuples + keep_tuples ||
			!vac_is_partial_index(indrel))
			ereport(WARNING,
					(errmsg("index \"%s\" contains %.0f row versions, but table contains %.0f row versions",
							RelationGetRelationName(indrel),
					  stats->num_index_tuples, num_tuples + keep_tuples),
					 errhint("Rebuild the index with REINDEX.")));
	}

	pfree(stats);
}

/*
 *	tid_reaped() -- is a particular tid reaped?
 *
 *		This has the right signature to be an IndexBulkDeleteCallback.
 *
 *		vacpagelist->VacPage_array is sorted in right order.
 */
static bool
tid_reaped(ItemPointer itemptr, void *state)
{
	VacPageList vacpagelist = (VacPageList) state;
	OffsetNumber ioffno;
	OffsetNumber *voff;
	VacPage		vp,
			   *vpp;
	VacPageData vacpage;

	vacpage.blkno = ItemPointerGetBlockNumber(itemptr);
	ioffno = ItemPointerGetOffsetNumber(itemptr);

	vp = &vacpage;
	vpp = (VacPage *) vac_bsearch((void *) &vp,
								  (void *) (vacpagelist->pagedesc),
								  vacpagelist->num_pages,
								  sizeof(VacPage),
								  vac_cmp_blk);

	if (vpp == NULL)
		return false;

	/* ok - we are on a partially or fully reaped page */
	vp = *vpp;

	if (vp->offsets_free == 0)
	{
		/* this is EmptyPage, so claim all tuples on it are reaped!!! */
		return true;
	}

	voff = (OffsetNumber *) vac_bsearch((void *) &ioffno,
										(void *) (vp->offsets),
										vp->offsets_free,
										sizeof(OffsetNumber),
										vac_cmp_offno);

	if (voff == NULL)
		return false;

	/* tid is reaped */
	return true;
}

/*
 * Dummy version for scan_index.
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
vac_update_fsm(Relation onerel, VacPageList fraged_pages,
			   BlockNumber rel_pages)
{
	int			nPages = fraged_pages->num_pages;
	VacPage    *pagedesc = fraged_pages->pagedesc;
	Size		threshold;
	PageFreeSpaceInfo *pageSpaces;
	int			outPages;
	int			i;

	/*
	 * We only report pages with free space at least equal to the average
	 * request size --- this avoids cluttering FSM with uselessly-small
	 * bits of space.  Although FSM would discard pages with little free
	 * space anyway, it's important to do this prefiltering because (a) it
	 * reduces the time spent holding the FSM lock in
	 * RecordRelationFreeSpace, and (b) FSM uses the number of pages
	 * reported as a statistic for guiding space management.  If we didn't
	 * threshold our reports the same way vacuumlazy.c does, we'd be
	 * skewing that statistic.
	 */
	threshold = GetAvgFSMRequestSize(&onerel->rd_node);

	/* +1 to avoid palloc(0) */
	pageSpaces = (PageFreeSpaceInfo *)
		palloc((nPages + 1) * sizeof(PageFreeSpaceInfo));
	outPages = 0;

	for (i = 0; i < nPages; i++)
	{
		/*
		 * fraged_pages may contain entries for pages that we later
		 * decided to truncate from the relation; don't enter them into
		 * the free space map!
		 */
		if (pagedesc[i]->blkno >= rel_pages)
			break;

		if (pagedesc[i]->free >= threshold)
		{
			pageSpaces[outPages].blkno = pagedesc[i]->blkno;
			pageSpaces[outPages].avail = pagedesc[i]->free;
			outPages++;
		}
	}

	RecordRelationFreeSpace(&onerel->rd_node, outPages, pageSpaces);

	pfree(pageSpaces);
}

/* Copy a VacPage structure */
static VacPage
copy_vac_page(VacPage vacpage)
{
	VacPage		newvacpage;

	/* allocate a VacPageData entry */
	newvacpage = (VacPage) palloc(sizeof(VacPageData) +
						   vacpage->offsets_free * sizeof(OffsetNumber));

	/* fill it in */
	if (vacpage->offsets_free > 0)
		memcpy(newvacpage->offsets, vacpage->offsets,
			   vacpage->offsets_free * sizeof(OffsetNumber));
	newvacpage->blkno = vacpage->blkno;
	newvacpage->free = vacpage->free;
	newvacpage->offsets_used = vacpage->offsets_used;
	newvacpage->offsets_free = vacpage->offsets_free;

	return newvacpage;
}

/*
 * Add a VacPage pointer to a VacPageList.
 *
 *		As a side effect of the way that scan_heap works,
 *		higher pages come after lower pages in the array
 *		(and highest tid on a page is last).
 */
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

/*
 * vac_bsearch: just like standard C library routine bsearch(),
 * except that we first test to see whether the target key is outside
 * the range of the table entries.	This case is handled relatively slowly
 * by the normal binary search algorithm (ie, no faster than any other key)
 * but it occurs often enough in VACUUM to be worth optimizing.
 */
static void *
vac_bsearch(const void *key, const void *base,
			size_t nelem, size_t size,
			int (*compar) (const void *, const void *))
{
	int			res;
	const void *last;

	if (nelem == 0)
		return NULL;
	res = compar(key, base);
	if (res < 0)
		return NULL;
	if (res == 0)
		return (void *) base;
	if (nelem > 1)
	{
		last = (const void *) ((const char *) base + (nelem - 1) * size);
		res = compar(key, last);
		if (res > 0)
			return NULL;
		if (res == 0)
			return (void *) last;
	}
	if (nelem <= 2)
		return NULL;			/* already checked 'em all */
	return bsearch(key, base, nelem, size, compar);
}

/*
 * Comparator routines for use with qsort() and bsearch().
 */
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


void
vac_open_indexes(Relation relation, int *nindexes, Relation **Irel)
{
	List	   *indexoidlist,
			   *indexoidscan;
	int			i;

	indexoidlist = RelationGetIndexList(relation);

	*nindexes = length(indexoidlist);

	if (*nindexes > 0)
		*Irel = (Relation *) palloc(*nindexes * sizeof(Relation));
	else
		*Irel = NULL;

	i = 0;
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirsto(indexoidscan);

		(*Irel)[i] = index_open(indexoid);
		i++;
	}

	freeList(indexoidlist);
}


void
vac_close_indexes(int nindexes, Relation *Irel)
{
	if (Irel == (Relation *) NULL)
		return;

	while (nindexes--)
		index_close(Irel[nindexes]);
	pfree(Irel);
}


/*
 * Is an index partial (ie, could it contain fewer tuples than the heap?)
 */
bool
vac_is_partial_index(Relation indrel)
{
	/*
	 * If the index's AM doesn't support nulls, it's partial for our
	 * purposes
	 */
	if (!indrel->rd_am->amindexnulls)
		return true;

	/* Otherwise, look to see if there's a partial-index predicate */
	if (!heap_attisnull(indrel->rd_indextuple, Anum_pg_index_indpred))
		return true;

	return false;
}


static bool
enough_space(VacPage vacpage, Size len)
{
	len = MAXALIGN(len);

	if (len > vacpage->free)
		return false;

	/* if there are free itemid(s) and len <= free_space... */
	if (vacpage->offsets_used < vacpage->offsets_free)
		return true;

	/* noff_used >= noff_free and so we'll have to allocate new itemid */
	if (len + sizeof(ItemIdData) <= vacpage->free)
		return true;

	return false;
}


/*
 * Initialize usage snapshot.
 */
void
vac_init_rusage(VacRUsage *ru0)
{
	struct timezone tz;

	getrusage(RUSAGE_SELF, &ru0->ru);
	gettimeofday(&ru0->tv, &tz);
}

/*
 * Compute elapsed time since ru0 usage snapshot, and format into
 * a displayable string.  Result is in a static string, which is
 * tacky, but no one ever claimed that the Postgres backend is
 * threadable...
 */
const char *
vac_show_rusage(VacRUsage *ru0)
{
	static char result[100];
	VacRUsage	ru1;

	vac_init_rusage(&ru1);

	if (ru1.tv.tv_usec < ru0->tv.tv_usec)
	{
		ru1.tv.tv_sec--;
		ru1.tv.tv_usec += 1000000;
	}
	if (ru1.ru.ru_stime.tv_usec < ru0->ru.ru_stime.tv_usec)
	{
		ru1.ru.ru_stime.tv_sec--;
		ru1.ru.ru_stime.tv_usec += 1000000;
	}
	if (ru1.ru.ru_utime.tv_usec < ru0->ru.ru_utime.tv_usec)
	{
		ru1.ru.ru_utime.tv_sec--;
		ru1.ru.ru_utime.tv_usec += 1000000;
	}

	snprintf(result, sizeof(result),
			 "CPU %d.%02ds/%d.%02du sec elapsed %d.%02d sec.",
			 (int) (ru1.ru.ru_stime.tv_sec - ru0->ru.ru_stime.tv_sec),
	  (int) (ru1.ru.ru_stime.tv_usec - ru0->ru.ru_stime.tv_usec) / 10000,
			 (int) (ru1.ru.ru_utime.tv_sec - ru0->ru.ru_utime.tv_sec),
	  (int) (ru1.ru.ru_utime.tv_usec - ru0->ru.ru_utime.tv_usec) / 10000,
			 (int) (ru1.tv.tv_sec - ru0->tv.tv_sec),
			 (int) (ru1.tv.tv_usec - ru0->tv.tv_usec) / 10000);

	return result;
}
