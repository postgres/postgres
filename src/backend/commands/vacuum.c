/*-------------------------------------------------------------------------
 *
 * vacuum.c
 *	  The postgres vacuum cleaner.
 *
 * This file now includes only control and dispatch code for VACUUM and
 * ANALYZE commands.  Regular VACUUM is implemented in vacuumlazy.c,
 * ANALYZE in analyze.c, and VACUUM FULL is a variant of CLUSTER, handled
 * in cluster.c.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/vacuum.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/clog.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_database.h"
#include "catalog/pg_namespace.h"
#include "commands/cluster.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/autovacuum.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/tqual.h"


/*
 * GUC parameters
 */
int			vacuum_freeze_min_age;
int			vacuum_freeze_table_age;


/* A few variables that don't seem worth passing around as parameters */
static MemoryContext vac_context = NULL;
static BufferAccessStrategy vac_strategy;


/* non-export function prototypes */
static List *get_rel_oids(Oid relid, const RangeVar *vacrel);
static void vac_truncate_clog(TransactionId frozenXID, MultiXactId minMulti);
static bool vacuum_rel(Oid relid, VacuumStmt *vacstmt, bool do_toast,
		   bool for_wraparound);


/*
 * Primary entry point for VACUUM and ANALYZE commands.
 *
 * relid is normally InvalidOid; if it is not, then it provides the relation
 * OID to be processed, and vacstmt->relation is ignored.  (The non-invalid
 * case is currently only used by autovacuum.)
 *
 * do_toast is passed as FALSE by autovacuum, because it processes TOAST
 * tables separately.
 *
 * for_wraparound is used by autovacuum to let us know when it's forcing
 * a vacuum for wraparound, which should not be auto-canceled.
 *
 * bstrategy is normally given as NULL, but in autovacuum it can be passed
 * in to use the same buffer strategy object across multiple vacuum() calls.
 *
 * isTopLevel should be passed down from ProcessUtility.
 *
 * It is the caller's responsibility that vacstmt and bstrategy
 * (if given) be allocated in a memory context that won't disappear
 * at transaction commit.
 */
void
vacuum(VacuumStmt *vacstmt, Oid relid, bool do_toast,
	   BufferAccessStrategy bstrategy, bool for_wraparound, bool isTopLevel)
{
	const char *stmttype;
	volatile bool in_outer_xact,
				use_own_xacts;
	List	   *relations;

	/* sanity checks on options */
	Assert(vacstmt->options & (VACOPT_VACUUM | VACOPT_ANALYZE));
	Assert((vacstmt->options & VACOPT_VACUUM) ||
		   !(vacstmt->options & (VACOPT_FULL | VACOPT_FREEZE)));
	Assert((vacstmt->options & VACOPT_ANALYZE) || vacstmt->va_cols == NIL);

	stmttype = (vacstmt->options & VACOPT_VACUUM) ? "VACUUM" : "ANALYZE";

	/*
	 * We cannot run VACUUM inside a user transaction block; if we were inside
	 * a transaction, then our commit- and start-transaction-command calls
	 * would not have the intended effect!	There are numerous other subtle
	 * dependencies on this, too.
	 *
	 * ANALYZE (without VACUUM) can run either way.
	 */
	if (vacstmt->options & VACOPT_VACUUM)
	{
		PreventTransactionChain(isTopLevel, stmttype);
		in_outer_xact = false;
	}
	else
		in_outer_xact = IsInTransactionChain(isTopLevel);

	/*
	 * Send info about dead objects to the statistics collector, unless we are
	 * in autovacuum --- autovacuum.c does this for itself.
	 */
	if ((vacstmt->options & VACOPT_VACUUM) && !IsAutoVacuumWorkerProcess())
		pgstat_vacuum_stat();

	/*
	 * Create special memory context for cross-transaction storage.
	 *
	 * Since it is a child of PortalContext, it will go away eventually even
	 * if we suffer an error; there's no need for special abort cleanup logic.
	 */
	vac_context = AllocSetContextCreate(PortalContext,
										"Vacuum",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * If caller didn't give us a buffer strategy object, make one in the
	 * cross-transaction memory context.
	 */
	if (bstrategy == NULL)
	{
		MemoryContext old_context = MemoryContextSwitchTo(vac_context);

		bstrategy = GetAccessStrategy(BAS_VACUUM);
		MemoryContextSwitchTo(old_context);
	}
	vac_strategy = bstrategy;

	/*
	 * Build list of relations to process, unless caller gave us one. (If we
	 * build one, we put it in vac_context for safekeeping.)
	 */
	relations = get_rel_oids(relid, vacstmt->relation);

	/*
	 * Decide whether we need to start/commit our own transactions.
	 *
	 * For VACUUM (with or without ANALYZE): always do so, so that we can
	 * release locks as soon as possible.  (We could possibly use the outer
	 * transaction for a one-table VACUUM, but handling TOAST tables would be
	 * problematic.)
	 *
	 * For ANALYZE (no VACUUM): if inside a transaction block, we cannot
	 * start/commit our own transactions.  Also, there's no need to do so if
	 * only processing one relation.  For multiple relations when not within a
	 * transaction block, and also in an autovacuum worker, use own
	 * transactions so we can release locks sooner.
	 */
	if (vacstmt->options & VACOPT_VACUUM)
		use_own_xacts = true;
	else
	{
		Assert(vacstmt->options & VACOPT_ANALYZE);
		if (IsAutoVacuumWorkerProcess())
			use_own_xacts = true;
		else if (in_outer_xact)
			use_own_xacts = false;
		else if (list_length(relations) > 1)
			use_own_xacts = true;
		else
			use_own_xacts = false;
	}

	/*
	 * vacuum_rel expects to be entered with no transaction active; it will
	 * start and commit its own transaction.  But we are called by an SQL
	 * command, and so we are executing inside a transaction already. We
	 * commit the transaction started in PostgresMain() here, and start
	 * another one before exiting to match the commit waiting for us back in
	 * PostgresMain().
	 */
	if (use_own_xacts)
	{
		/* ActiveSnapshot is not set by autovacuum */
		if (ActiveSnapshotSet())
			PopActiveSnapshot();

		/* matches the StartTransaction in PostgresMain() */
		CommitTransactionCommand();
	}

	/* Turn vacuum cost accounting on or off */
	PG_TRY();
	{
		ListCell   *cur;

		VacuumCostActive = (VacuumCostDelay > 0);
		VacuumCostBalance = 0;
		VacuumPageHit = 0;
		VacuumPageMiss = 0;
		VacuumPageDirty = 0;

		/*
		 * Loop to process each selected relation.
		 */
		foreach(cur, relations)
		{
			Oid			relid = lfirst_oid(cur);

			if (vacstmt->options & VACOPT_VACUUM)
			{
				if (!vacuum_rel(relid, vacstmt, do_toast, for_wraparound))
					continue;
			}

			if (vacstmt->options & VACOPT_ANALYZE)
			{
				/*
				 * If using separate xacts, start one for analyze. Otherwise,
				 * we can use the outer transaction.
				 */
				if (use_own_xacts)
				{
					StartTransactionCommand();
					/* functions in indexes may want a snapshot set */
					PushActiveSnapshot(GetTransactionSnapshot());
				}

				analyze_rel(relid, vacstmt, vac_strategy);

				if (use_own_xacts)
				{
					PopActiveSnapshot();
					CommitTransactionCommand();
				}
			}
		}
	}
	PG_CATCH();
	{
		/* Make sure cost accounting is turned off after error */
		VacuumCostActive = false;
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Turn off vacuum cost accounting */
	VacuumCostActive = false;

	/*
	 * Finish up processing.
	 */
	if (use_own_xacts)
	{
		/* here, we are not in a transaction */

		/*
		 * This matches the CommitTransaction waiting for us in
		 * PostgresMain().
		 */
		StartTransactionCommand();
	}

	if ((vacstmt->options & VACOPT_VACUUM) && !IsAutoVacuumWorkerProcess())
	{
		/*
		 * Update pg_database.datfrozenxid, and truncate pg_clog if possible.
		 * (autovacuum.c does this for itself.)
		 */
		vac_update_datfrozenxid();
	}

	/*
	 * Clean up working storage --- note we must do this after
	 * StartTransactionCommand, else we might be trying to delete the active
	 * context!
	 */
	MemoryContextDelete(vac_context);
	vac_context = NULL;
}

/*
 * Build a list of Oids for each relation to be processed
 *
 * The list is built in vac_context so that it will survive across our
 * per-relation transactions.
 */
static List *
get_rel_oids(Oid relid, const RangeVar *vacrel)
{
	List	   *oid_list = NIL;
	MemoryContext oldcontext;

	/* OID supplied by VACUUM's caller? */
	if (OidIsValid(relid))
	{
		oldcontext = MemoryContextSwitchTo(vac_context);
		oid_list = lappend_oid(oid_list, relid);
		MemoryContextSwitchTo(oldcontext);
	}
	else if (vacrel)
	{
		/* Process a specific relation */
		Oid			relid;

		/*
		 * Since we don't take a lock here, the relation might be gone, or the
		 * RangeVar might no longer refer to the OID we look up here.  In the
		 * former case, VACUUM will do nothing; in the latter case, it will
		 * process the OID we looked up here, rather than the new one. Neither
		 * is ideal, but there's little practical alternative, since we're
		 * going to commit this transaction and begin a new one between now
		 * and then.
		 */
		relid = RangeVarGetRelid(vacrel, NoLock, false);

		/* Make a relation list entry for this guy */
		oldcontext = MemoryContextSwitchTo(vac_context);
		oid_list = lappend_oid(oid_list, relid);
		MemoryContextSwitchTo(oldcontext);
	}
	else
	{
		/*
		 * Process all plain relations and materialized views listed in
		 * pg_class
		 */
		Relation	pgclass;
		HeapScanDesc scan;
		HeapTuple	tuple;

		pgclass = heap_open(RelationRelationId, AccessShareLock);

		scan = heap_beginscan(pgclass, SnapshotNow, 0, NULL);

		while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
		{
			Form_pg_class classForm = (Form_pg_class) GETSTRUCT(tuple);

			if (classForm->relkind != RELKIND_RELATION &&
				classForm->relkind != RELKIND_MATVIEW)
				continue;

			/* Make a relation list entry for this guy */
			oldcontext = MemoryContextSwitchTo(vac_context);
			oid_list = lappend_oid(oid_list, HeapTupleGetOid(tuple));
			MemoryContextSwitchTo(oldcontext);
		}

		heap_endscan(scan);
		heap_close(pgclass, AccessShareLock);
	}

	return oid_list;
}

/*
 * vacuum_set_xid_limits() -- compute oldest-Xmin and freeze cutoff points
 */
void
vacuum_set_xid_limits(int freeze_min_age,
					  int freeze_table_age,
					  bool sharedRel,
					  TransactionId *oldestXmin,
					  TransactionId *freezeLimit,
					  TransactionId *freezeTableLimit,
					  MultiXactId *multiXactCutoff)
{
	int			freezemin;
	TransactionId limit;
	TransactionId safeLimit;

	/*
	 * We can always ignore processes running lazy vacuum.	This is because we
	 * use these values only for deciding which tuples we must keep in the
	 * tables.	Since lazy vacuum doesn't write its XID anywhere, it's safe to
	 * ignore it.  In theory it could be problematic to ignore lazy vacuums in
	 * a full vacuum, but keep in mind that only one vacuum process can be
	 * working on a particular table at any time, and that each vacuum is
	 * always an independent transaction.
	 */
	*oldestXmin = GetOldestXmin(sharedRel, true);

	Assert(TransactionIdIsNormal(*oldestXmin));

	/*
	 * Determine the minimum freeze age to use: as specified by the caller, or
	 * vacuum_freeze_min_age, but in any case not more than half
	 * autovacuum_freeze_max_age, so that autovacuums to prevent XID
	 * wraparound won't occur too frequently.
	 */
	freezemin = freeze_min_age;
	if (freezemin < 0)
		freezemin = vacuum_freeze_min_age;
	freezemin = Min(freezemin, autovacuum_freeze_max_age / 2);
	Assert(freezemin >= 0);

	/*
	 * Compute the cutoff XID, being careful not to generate a "permanent" XID
	 */
	limit = *oldestXmin - freezemin;
	if (!TransactionIdIsNormal(limit))
		limit = FirstNormalTransactionId;

	/*
	 * If oldestXmin is very far back (in practice, more than
	 * autovacuum_freeze_max_age / 2 XIDs old), complain and force a minimum
	 * freeze age of zero.
	 */
	safeLimit = ReadNewTransactionId() - autovacuum_freeze_max_age;
	if (!TransactionIdIsNormal(safeLimit))
		safeLimit = FirstNormalTransactionId;

	if (TransactionIdPrecedes(limit, safeLimit))
	{
		ereport(WARNING,
				(errmsg("oldest xmin is far in the past"),
				 errhint("Close open transactions soon to avoid wraparound problems.")));
		limit = *oldestXmin;
	}

	*freezeLimit = limit;

	if (freezeTableLimit != NULL)
	{
		int			freezetable;

		/*
		 * Determine the table freeze age to use: as specified by the caller,
		 * or vacuum_freeze_table_age, but in any case not more than
		 * autovacuum_freeze_max_age * 0.95, so that if you have e.g nightly
		 * VACUUM schedule, the nightly VACUUM gets a chance to freeze tuples
		 * before anti-wraparound autovacuum is launched.
		 */
		freezetable = freeze_table_age;
		if (freezetable < 0)
			freezetable = vacuum_freeze_table_age;
		freezetable = Min(freezetable, autovacuum_freeze_max_age * 0.95);
		Assert(freezetable >= 0);

		/*
		 * Compute the cutoff XID, being careful not to generate a "permanent"
		 * XID.
		 */
		limit = ReadNewTransactionId() - freezetable;
		if (!TransactionIdIsNormal(limit))
			limit = FirstNormalTransactionId;

		*freezeTableLimit = limit;
	}

	if (multiXactCutoff != NULL)
	{
		MultiXactId mxLimit;

		/*
		 * simplistic multixactid freezing: use the same freezing policy as
		 * for Xids
		 */
		mxLimit = GetOldestMultiXactId() - freezemin;
		if (mxLimit < FirstMultiXactId)
			mxLimit = FirstMultiXactId;

		*multiXactCutoff = mxLimit;
	}
}

/*
 * vac_estimate_reltuples() -- estimate the new value for pg_class.reltuples
 *
 *		If we scanned the whole relation then we should just use the count of
 *		live tuples seen; but if we did not, we should not trust the count
 *		unreservedly, especially not in VACUUM, which may have scanned a quite
 *		nonrandom subset of the table.	When we have only partial information,
 *		we take the old value of pg_class.reltuples as a measurement of the
 *		tuple density in the unscanned pages.
 *
 *		This routine is shared by VACUUM and ANALYZE.
 */
double
vac_estimate_reltuples(Relation relation, bool is_analyze,
					   BlockNumber total_pages,
					   BlockNumber scanned_pages,
					   double scanned_tuples)
{
	BlockNumber old_rel_pages = relation->rd_rel->relpages;
	double		old_rel_tuples = relation->rd_rel->reltuples;
	double		old_density;
	double		new_density;
	double		multiplier;
	double		updated_density;

	/* If we did scan the whole table, just use the count as-is */
	if (scanned_pages >= total_pages)
		return scanned_tuples;

	/*
	 * If scanned_pages is zero but total_pages isn't, keep the existing value
	 * of reltuples.  (Note: callers should avoid updating the pg_class
	 * statistics in this situation, since no new information has been
	 * provided.)
	 */
	if (scanned_pages == 0)
		return old_rel_tuples;

	/*
	 * If old value of relpages is zero, old density is indeterminate; we
	 * can't do much except scale up scanned_tuples to match total_pages.
	 */
	if (old_rel_pages == 0)
		return floor((scanned_tuples / scanned_pages) * total_pages + 0.5);

	/*
	 * Okay, we've covered the corner cases.  The normal calculation is to
	 * convert the old measurement to a density (tuples per page), then update
	 * the density using an exponential-moving-average approach, and finally
	 * compute reltuples as updated_density * total_pages.
	 *
	 * For ANALYZE, the moving average multiplier is just the fraction of the
	 * table's pages we scanned.  This is equivalent to assuming that the
	 * tuple density in the unscanned pages didn't change.  Of course, it
	 * probably did, if the new density measurement is different. But over
	 * repeated cycles, the value of reltuples will converge towards the
	 * correct value, if repeated measurements show the same new density.
	 *
	 * For VACUUM, the situation is a bit different: we have looked at a
	 * nonrandom sample of pages, but we know for certain that the pages we
	 * didn't look at are precisely the ones that haven't changed lately.
	 * Thus, there is a reasonable argument for doing exactly the same thing
	 * as for the ANALYZE case, that is use the old density measurement as the
	 * value for the unscanned pages.
	 *
	 * This logic could probably use further refinement.
	 */
	old_density = old_rel_tuples / old_rel_pages;
	new_density = scanned_tuples / scanned_pages;
	multiplier = (double) scanned_pages / (double) total_pages;
	updated_density = old_density + (new_density - old_density) * multiplier;
	return floor(updated_density * total_pages + 0.5);
}


/*
 *	vac_update_relstats() -- update statistics for one relation
 *
 *		Update the whole-relation statistics that are kept in its pg_class
 *		row.  There are additional stats that will be updated if we are
 *		doing ANALYZE, but we always update these stats.  This routine works
 *		for both index and heap relation entries in pg_class.
 *
 *		We violate transaction semantics here by overwriting the rel's
 *		existing pg_class tuple with the new values.  This is reasonably
 *		safe since the new values are correct whether or not this transaction
 *		commits.  The reason for this is that if we updated these tuples in
 *		the usual way, vacuuming pg_class itself wouldn't work very well ---
 *		by the time we got done with a vacuum cycle, most of the tuples in
 *		pg_class would've been obsoleted.  Of course, this only works for
 *		fixed-size never-null columns, but these are.
 *
 *		Note another assumption: that two VACUUMs/ANALYZEs on a table can't
 *		run in parallel, nor can VACUUM/ANALYZE run in parallel with a
 *		schema alteration such as adding an index, rule, or trigger.  Otherwise
 *		our updates of relhasindex etc might overwrite uncommitted updates.
 *
 *		Another reason for doing it this way is that when we are in a lazy
 *		VACUUM and have PROC_IN_VACUUM set, we mustn't do any updates ---
 *		somebody vacuuming pg_class might think they could delete a tuple
 *		marked with xmin = our xid.
 *
 *		This routine is shared by VACUUM and ANALYZE.
 */
void
vac_update_relstats(Relation relation,
					BlockNumber num_pages, double num_tuples,
					BlockNumber num_all_visible_pages,
					bool hasindex, TransactionId frozenxid,
					MultiXactId minmulti)
{
	Oid			relid = RelationGetRelid(relation);
	Relation	rd;
	HeapTuple	ctup;
	Form_pg_class pgcform;
	bool		dirty;

	rd = heap_open(RelationRelationId, RowExclusiveLock);

	/* Fetch a copy of the tuple to scribble on */
	ctup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(relid));
	if (!HeapTupleIsValid(ctup))
		elog(ERROR, "pg_class entry for relid %u vanished during vacuuming",
			 relid);
	pgcform = (Form_pg_class) GETSTRUCT(ctup);

	/* Apply required updates, if any, to copied tuple */

	dirty = false;
	if (pgcform->relpages != (int32) num_pages)
	{
		pgcform->relpages = (int32) num_pages;
		dirty = true;
	}
	if (pgcform->reltuples != (float4) num_tuples)
	{
		pgcform->reltuples = (float4) num_tuples;
		dirty = true;
	}
	if (pgcform->relallvisible != (int32) num_all_visible_pages)
	{
		pgcform->relallvisible = (int32) num_all_visible_pages;
		dirty = true;
	}
	if (pgcform->relhasindex != hasindex)
	{
		pgcform->relhasindex = hasindex;
		dirty = true;
	}

	/*
	 * If we have discovered that there are no indexes, then there's no
	 * primary key either.	This could be done more thoroughly...
	 */
	if (pgcform->relhaspkey && !hasindex)
	{
		pgcform->relhaspkey = false;
		dirty = true;
	}

	/* We also clear relhasrules and relhastriggers if needed */
	if (pgcform->relhasrules && relation->rd_rules == NULL)
	{
		pgcform->relhasrules = false;
		dirty = true;
	}
	if (pgcform->relhastriggers && relation->trigdesc == NULL)
	{
		pgcform->relhastriggers = false;
		dirty = true;
	}

	/*
	 * relfrozenxid should never go backward.  Caller can pass
	 * InvalidTransactionId if it has no new data.
	 */
	if (TransactionIdIsNormal(frozenxid) &&
		TransactionIdPrecedes(pgcform->relfrozenxid, frozenxid))
	{
		pgcform->relfrozenxid = frozenxid;
		dirty = true;
	}

	/* relminmxid must never go backward, either */
	if (MultiXactIdIsValid(minmulti) &&
		MultiXactIdPrecedes(pgcform->relminmxid, minmulti))
	{
		pgcform->relminmxid = minmulti;
		dirty = true;
	}

	/* If anything changed, write out the tuple. */
	if (dirty)
		heap_inplace_update(rd, ctup);

	heap_close(rd, RowExclusiveLock);
}


/*
 *	vac_update_datfrozenxid() -- update pg_database.datfrozenxid for our DB
 *
 *		Update pg_database's datfrozenxid entry for our database to be the
 *		minimum of the pg_class.relfrozenxid values.
 *
 *		Similarly, update our datminmxid to be the minimum of the
 *		pg_class.relminmxid values.
 *
 *		If we are able to advance either pg_database value, also try to
 *		truncate pg_clog and pg_multixact.
 *
 *		We violate transaction semantics here by overwriting the database's
 *		existing pg_database tuple with the new value.	This is reasonably
 *		safe since the new value is correct whether or not this transaction
 *		commits.  As with vac_update_relstats, this avoids leaving dead tuples
 *		behind after a VACUUM.
 */
void
vac_update_datfrozenxid(void)
{
	HeapTuple	tuple;
	Form_pg_database dbform;
	Relation	relation;
	SysScanDesc scan;
	HeapTuple	classTup;
	TransactionId newFrozenXid;
	MultiXactId newMinMulti;
	bool		dirty = false;

	/*
	 * Initialize the "min" calculation with GetOldestXmin, which is a
	 * reasonable approximation to the minimum relfrozenxid for not-yet-
	 * committed pg_class entries for new tables; see AddNewRelationTuple().
	 * So we cannot produce a wrong minimum by starting with this.
	 */
	newFrozenXid = GetOldestXmin(true, true);

	/*
	 * Similarly, initialize the MultiXact "min" with the value that would be
	 * used on pg_class for new tables.  See AddNewRelationTuple().
	 */
	newMinMulti = GetOldestMultiXactId();

	/*
	 * We must seqscan pg_class to find the minimum Xid, because there is no
	 * index that can help us here.
	 */
	relation = heap_open(RelationRelationId, AccessShareLock);

	scan = systable_beginscan(relation, InvalidOid, false,
							  SnapshotNow, 0, NULL);

	while ((classTup = systable_getnext(scan)) != NULL)
	{
		Form_pg_class classForm = (Form_pg_class) GETSTRUCT(classTup);

		/*
		 * Only consider relations able to hold unfrozen XIDs (anything else
		 * should have InvalidTransactionId in relfrozenxid anyway.)
		 */
		if (classForm->relkind != RELKIND_RELATION &&
			classForm->relkind != RELKIND_MATVIEW &&
			classForm->relkind != RELKIND_TOASTVALUE)
			continue;

		Assert(TransactionIdIsNormal(classForm->relfrozenxid));
		Assert(MultiXactIdIsValid(classForm->relminmxid));

		if (TransactionIdPrecedes(classForm->relfrozenxid, newFrozenXid))
			newFrozenXid = classForm->relfrozenxid;

		if (MultiXactIdPrecedes(classForm->relminmxid, newMinMulti))
			newMinMulti = classForm->relminmxid;
	}

	/* we're done with pg_class */
	systable_endscan(scan);
	heap_close(relation, AccessShareLock);

	Assert(TransactionIdIsNormal(newFrozenXid));
	Assert(MultiXactIdIsValid(newMinMulti));

	/* Now fetch the pg_database tuple we need to update. */
	relation = heap_open(DatabaseRelationId, RowExclusiveLock);

	/* Fetch a copy of the tuple to scribble on */
	tuple = SearchSysCacheCopy1(DATABASEOID, ObjectIdGetDatum(MyDatabaseId));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "could not find tuple for database %u", MyDatabaseId);
	dbform = (Form_pg_database) GETSTRUCT(tuple);

	/*
	 * Don't allow datfrozenxid to go backward (probably can't happen anyway);
	 * and detect the common case where it doesn't go forward either.
	 */
	if (TransactionIdPrecedes(dbform->datfrozenxid, newFrozenXid))
	{
		dbform->datfrozenxid = newFrozenXid;
		dirty = true;
	}

	/* ditto */
	if (MultiXactIdPrecedes(dbform->datminmxid, newMinMulti))
	{
		dbform->datminmxid = newMinMulti;
		dirty = true;
	}

	if (dirty)
		heap_inplace_update(relation, tuple);

	heap_freetuple(tuple);
	heap_close(relation, RowExclusiveLock);

	/*
	 * If we were able to advance datfrozenxid, see if we can truncate
	 * pg_clog. Also do it if the shared XID-wrap-limit info is stale, since
	 * this action will update that too.
	 */
	if (dirty || ForceTransactionIdLimitUpdate())
		vac_truncate_clog(newFrozenXid, newMinMulti);
}


/*
 *	vac_truncate_clog() -- attempt to truncate the commit log
 *
 *		Scan pg_database to determine the system-wide oldest datfrozenxid,
 *		and use it to truncate the transaction commit log (pg_clog).
 *		Also update the XID wrap limit info maintained by varsup.c.
 *
 *		The passed XID is simply the one I just wrote into my pg_database
 *		entry.	It's used to initialize the "min" calculation.
 *
 *		This routine is only invoked when we've managed to change our
 *		DB's datfrozenxid entry, or we found that the shared XID-wrap-limit
 *		info is stale.
 */
static void
vac_truncate_clog(TransactionId frozenXID, MultiXactId minMulti)
{
	TransactionId myXID = GetCurrentTransactionId();
	Relation	relation;
	HeapScanDesc scan;
	HeapTuple	tuple;
	Oid			oldestxid_datoid;
	Oid			minmulti_datoid;
	bool		frozenAlreadyWrapped = false;

	/* init oldest datoids to sync with my frozen values */
	oldestxid_datoid = MyDatabaseId;
	minmulti_datoid = MyDatabaseId;

	/*
	 * Scan pg_database to compute the minimum datfrozenxid
	 *
	 * Note: we need not worry about a race condition with new entries being
	 * inserted by CREATE DATABASE.  Any such entry will have a copy of some
	 * existing DB's datfrozenxid, and that source DB cannot be ours because
	 * of the interlock against copying a DB containing an active backend.
	 * Hence the new entry will not reduce the minimum.  Also, if two VACUUMs
	 * concurrently modify the datfrozenxid's of different databases, the
	 * worst possible outcome is that pg_clog is not truncated as aggressively
	 * as it could be.
	 */
	relation = heap_open(DatabaseRelationId, AccessShareLock);

	scan = heap_beginscan(relation, SnapshotNow, 0, NULL);

	while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_database dbform = (Form_pg_database) GETSTRUCT(tuple);

		Assert(TransactionIdIsNormal(dbform->datfrozenxid));
		Assert(MultiXactIdIsValid(dbform->datminmxid));

		if (TransactionIdPrecedes(myXID, dbform->datfrozenxid))
			frozenAlreadyWrapped = true;
		else if (TransactionIdPrecedes(dbform->datfrozenxid, frozenXID))
		{
			frozenXID = dbform->datfrozenxid;
			oldestxid_datoid = HeapTupleGetOid(tuple);
		}

		if (MultiXactIdPrecedes(dbform->datminmxid, minMulti))
		{
			minMulti = dbform->datminmxid;
			minmulti_datoid = HeapTupleGetOid(tuple);
		}
	}

	heap_endscan(scan);

	heap_close(relation, AccessShareLock);

	/*
	 * Do not truncate CLOG if we seem to have suffered wraparound already;
	 * the computed minimum XID might be bogus.  This case should now be
	 * impossible due to the defenses in GetNewTransactionId, but we keep the
	 * test anyway.
	 */
	if (frozenAlreadyWrapped)
	{
		ereport(WARNING,
				(errmsg("some databases have not been vacuumed in over 2 billion transactions"),
				 errdetail("You might have already suffered transaction-wraparound data loss.")));
		return;
	}

	/* Truncate CLOG and Multi to the oldest computed value */
	TruncateCLOG(frozenXID);
	TruncateMultiXact(minMulti);

	/*
	 * Update the wrap limit for GetNewTransactionId and creation of new
	 * MultiXactIds.  Note: these functions will also signal the postmaster
	 * for an(other) autovac cycle if needed.	XXX should we avoid possibly
	 * signalling twice?
	 */
	SetTransactionIdLimit(frozenXID, oldestxid_datoid);
	MultiXactAdvanceOldest(minMulti, minmulti_datoid);
}


/*
 *	vacuum_rel() -- vacuum one heap relation
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
vacuum_rel(Oid relid, VacuumStmt *vacstmt, bool do_toast, bool for_wraparound)
{
	LOCKMODE	lmode;
	Relation	onerel;
	LockRelId	onerelid;
	Oid			toast_relid;
	Oid			save_userid;
	int			save_sec_context;
	int			save_nestlevel;

	/* Begin a transaction for vacuuming this relation */
	StartTransactionCommand();

	/*
	 * Functions in indexes may want a snapshot set.  Also, setting a snapshot
	 * ensures that RecentGlobalXmin is kept truly recent.
	 */
	PushActiveSnapshot(GetTransactionSnapshot());

	if (!(vacstmt->options & VACOPT_FULL))
	{
		/*
		 * In lazy vacuum, we can set the PROC_IN_VACUUM flag, which lets
		 * other concurrent VACUUMs know that they can ignore this one while
		 * determining their OldestXmin.  (The reason we don't set it during a
		 * full VACUUM is exactly that we may have to run user-defined
		 * functions for functional indexes, and we want to make sure that if
		 * they use the snapshot set above, any tuples it requires can't get
		 * removed from other tables.  An index function that depends on the
		 * contents of other tables is arguably broken, but we won't break it
		 * here by violating transaction semantics.)
		 *
		 * We also set the VACUUM_FOR_WRAPAROUND flag, which is passed down by
		 * autovacuum; it's used to avoid canceling a vacuum that was invoked
		 * in an emergency.
		 *
		 * Note: these flags remain set until CommitTransaction or
		 * AbortTransaction.  We don't want to clear them until we reset
		 * MyPgXact->xid/xmin, else OldestXmin might appear to go backwards,
		 * which is probably Not Good.
		 */
		LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
		MyPgXact->vacuumFlags |= PROC_IN_VACUUM;
		if (for_wraparound)
			MyPgXact->vacuumFlags |= PROC_VACUUM_FOR_WRAPAROUND;
		LWLockRelease(ProcArrayLock);
	}

	/*
	 * Check for user-requested abort.	Note we want this to be inside a
	 * transaction, so xact.c doesn't issue useless WARNING.
	 */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Determine the type of lock we want --- hard exclusive lock for a FULL
	 * vacuum, but just ShareUpdateExclusiveLock for concurrent vacuum. Either
	 * way, we can be sure that no other backend is vacuuming the same table.
	 */
	lmode = (vacstmt->options & VACOPT_FULL) ? AccessExclusiveLock : ShareUpdateExclusiveLock;

	/*
	 * Open the relation and get the appropriate lock on it.
	 *
	 * There's a race condition here: the rel may have gone away since the
	 * last time we saw it.  If so, we don't need to vacuum it.
	 *
	 * If we've been asked not to wait for the relation lock, acquire it first
	 * in non-blocking mode, before calling try_relation_open().
	 */
	if (!(vacstmt->options & VACOPT_NOWAIT))
		onerel = try_relation_open(relid, lmode);
	else if (ConditionalLockRelationOid(relid, lmode))
		onerel = try_relation_open(relid, NoLock);
	else
	{
		onerel = NULL;
		if (IsAutoVacuumWorkerProcess() && Log_autovacuum_min_duration >= 0)
			ereport(LOG,
					(errcode(ERRCODE_LOCK_NOT_AVAILABLE),
				   errmsg("skipping vacuum of \"%s\" --- lock not available",
						  vacstmt->relation->relname)));
	}

	if (!onerel)
	{
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Check permissions.
	 *
	 * We allow the user to vacuum a table if he is superuser, the table
	 * owner, or the database owner (but in the latter case, only if it's not
	 * a shared relation).	pg_class_ownercheck includes the superuser case.
	 *
	 * Note we choose to treat permissions failure as a WARNING and keep
	 * trying to vacuum the rest of the DB --- is this appropriate?
	 */
	if (!(pg_class_ownercheck(RelationGetRelid(onerel), GetUserId()) ||
		  (pg_database_ownercheck(MyDatabaseId, GetUserId()) && !onerel->rd_rel->relisshared)))
	{
		if (onerel->rd_rel->relisshared)
			ereport(WARNING,
				  (errmsg("skipping \"%s\" --- only superuser can vacuum it",
						  RelationGetRelationName(onerel))));
		else if (onerel->rd_rel->relnamespace == PG_CATALOG_NAMESPACE)
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- only superuser or database owner can vacuum it",
							RelationGetRelationName(onerel))));
		else
			ereport(WARNING,
					(errmsg("skipping \"%s\" --- only table or database owner can vacuum it",
							RelationGetRelationName(onerel))));
		relation_close(onerel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Check that it's a vacuumable relation; we used to do this in
	 * get_rel_oids() but seems safer to check after we've locked the
	 * relation.
	 */
	if (onerel->rd_rel->relkind != RELKIND_RELATION &&
		onerel->rd_rel->relkind != RELKIND_MATVIEW &&
		onerel->rd_rel->relkind != RELKIND_TOASTVALUE)
	{
		ereport(WARNING,
				(errmsg("skipping \"%s\" --- cannot vacuum non-tables or special system tables",
						RelationGetRelationName(onerel))));
		relation_close(onerel, lmode);
		PopActiveSnapshot();
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
	if (RELATION_IS_OTHER_TEMP(onerel))
	{
		relation_close(onerel, lmode);
		PopActiveSnapshot();
		CommitTransactionCommand();
		return false;
	}

	/*
	 * Get a session-level lock too. This will protect our access to the
	 * relation across multiple transactions, so that we can vacuum the
	 * relation's TOAST table (if any) secure in the knowledge that no one is
	 * deleting the parent relation.
	 *
	 * NOTE: this cannot block, even if someone else is waiting for access,
	 * because the lock manager knows that both lock requests are from the
	 * same process.
	 */
	onerelid = onerel->rd_lockInfo.lockRelId;
	LockRelationIdForSession(&onerelid, lmode);

	/*
	 * Remember the relation's TOAST relation for later, if the caller asked
	 * us to process it.  In VACUUM FULL, though, the toast table is
	 * automatically rebuilt by cluster_rel so we shouldn't recurse to it.
	 */
	if (do_toast && !(vacstmt->options & VACOPT_FULL))
		toast_relid = onerel->rd_rel->reltoastrelid;
	else
		toast_relid = InvalidOid;

	/*
	 * Switch to the table owner's userid, so that any index functions are run
	 * as that user.  Also lock down security-restricted operations and
	 * arrange to make GUC variable changes local to this command. (This is
	 * unnecessary, but harmless, for lazy VACUUM.)
	 */
	GetUserIdAndSecContext(&save_userid, &save_sec_context);
	SetUserIdAndSecContext(onerel->rd_rel->relowner,
						   save_sec_context | SECURITY_RESTRICTED_OPERATION);
	save_nestlevel = NewGUCNestLevel();

	/*
	 * Do the actual work --- either FULL or "lazy" vacuum
	 */
	if (vacstmt->options & VACOPT_FULL)
	{
		/* close relation before vacuuming, but hold lock until commit */
		relation_close(onerel, NoLock);
		onerel = NULL;

		/* VACUUM FULL is now a variant of CLUSTER; see cluster.c */
		cluster_rel(relid, InvalidOid, false,
					(vacstmt->options & VACOPT_VERBOSE) != 0,
					vacstmt->freeze_min_age, vacstmt->freeze_table_age);
	}
	else
		lazy_vacuum_rel(onerel, vacstmt, vac_strategy);

	/* Roll back any GUC changes executed by index functions */
	AtEOXact_GUC(false, save_nestlevel);

	/* Restore userid and security context */
	SetUserIdAndSecContext(save_userid, save_sec_context);

	/* all done with this class, but hold lock until commit */
	if (onerel)
		relation_close(onerel, NoLock);

	/*
	 * Complete the transaction and free all temporary memory used.
	 */
	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * If the relation has a secondary toast rel, vacuum that too while we
	 * still hold the session lock on the master table.  Note however that
	 * "analyze" will not get done on the toast table.	This is good, because
	 * the toaster always uses hardcoded index access and statistics are
	 * totally unimportant for toast relations.
	 */
	if (toast_relid != InvalidOid)
		vacuum_rel(toast_relid, vacstmt, false, for_wraparound);

	/*
	 * Now release the session-level lock on the master table.
	 */
	UnlockRelationIdForSession(&onerelid, lmode);

	/* Report that we really did it. */
	return true;
}


/*
 * Open all the vacuumable indexes of the given relation, obtaining the
 * specified kind of lock on each.	Return an array of Relation pointers for
 * the indexes into *Irel, and the number of indexes into *nindexes.
 *
 * We consider an index vacuumable if it is marked insertable (IndexIsReady).
 * If it isn't, probably a CREATE INDEX CONCURRENTLY command failed early in
 * execution, and what we have is too corrupt to be processable.  We will
 * vacuum even if the index isn't indisvalid; this is important because in a
 * unique index, uniqueness checks will be performed anyway and had better not
 * hit dangling index pointers.
 */
void
vac_open_indexes(Relation relation, LOCKMODE lockmode,
				 int *nindexes, Relation **Irel)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	int			i;

	Assert(lockmode != NoLock);

	indexoidlist = RelationGetIndexList(relation);

	/* allocate enough memory for all indexes */
	i = list_length(indexoidlist);

	if (i > 0)
		*Irel = (Relation *) palloc(i * sizeof(Relation));
	else
		*Irel = NULL;

	/* collect just the ready indexes */
	i = 0;
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indrel;

		indrel = index_open(indexoid, lockmode);
		if (IndexIsReady(indrel->rd_index))
			(*Irel)[i++] = indrel;
		else
			index_close(indrel, lockmode);
	}

	*nindexes = i;

	list_free(indexoidlist);
}

/*
 * Release the resources acquired by vac_open_indexes.	Optionally release
 * the locks (say NoLock to keep 'em).
 */
void
vac_close_indexes(int nindexes, Relation *Irel, LOCKMODE lockmode)
{
	if (Irel == NULL)
		return;

	while (nindexes--)
	{
		Relation	ind = Irel[nindexes];

		index_close(ind, lockmode);
	}
	pfree(Irel);
}

/*
 * vacuum_delay_point --- check for interrupts and cost-based delay.
 *
 * This should be called in each major loop of VACUUM processing,
 * typically once per page processed.
 */
void
vacuum_delay_point(void)
{
	/* Always check for interrupts */
	CHECK_FOR_INTERRUPTS();

	/* Nap if appropriate */
	if (VacuumCostActive && !InterruptPending &&
		VacuumCostBalance >= VacuumCostLimit)
	{
		int			msec;

		msec = VacuumCostDelay * VacuumCostBalance / VacuumCostLimit;
		if (msec > VacuumCostDelay * 4)
			msec = VacuumCostDelay * 4;

		pg_usleep(msec * 1000L);

		VacuumCostBalance = 0;

		/* update balance values for workers */
		AutoVacuumUpdateDelay();

		/* Might have gotten an interrupt while sleeping */
		CHECK_FOR_INTERRUPTS();
	}
}
