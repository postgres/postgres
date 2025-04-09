/*--------------------------------------------------------------------------
 *
 * regress_injection.c
 *		Functions supporting test-specific subject matter.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/injection_points/regress_injection.c
 *
 * -------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/table.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postmaster/autovacuum.h"
#include "storage/procarray.h"
#include "utils/rel.h"
#include "utils/xid8.h"

/*
 * removable_cutoff - for syscache-update-pruned.spec
 *
 * Wrapper around GetOldestNonRemovableTransactionId().  In general, this can
 * move backward.  runningcheck=false isolation tests can reasonably prevent
 * that.  For the causes of backward movement, see
 * postgr.es/m/CAEze2Wj%2BV0kTx86xB_YbyaqTr5hnE_igdWAwuhSyjXBYscf5-Q%40mail.gmail.com
 * and the header comment for ComputeXidHorizons().  One can assume this
 * doesn't move backward if one (a) passes a shared catalog as the argument
 * and (b) arranges for concurrent activity not to reach AbortTransaction().
 * Non-runningcheck tests can control most concurrent activity, except
 * autovacuum and the isolationtester control connection.  AbortTransaction()
 * in those would justify test failure.  Seeing autoanalyze can allocate an
 * XID in any database, (a) ensures we'll consistently not ignore those XIDs.
 */
PG_FUNCTION_INFO_V1(removable_cutoff);
Datum
removable_cutoff(PG_FUNCTION_ARGS)
{
	Relation	rel = NULL;
	TransactionId xid;
	FullTransactionId next_fxid_before,
				next_fxid;

	/* could take other relkinds callee takes, but we've not yet needed it */
	if (!PG_ARGISNULL(0))
		rel = table_open(PG_GETARG_OID(0), AccessShareLock);

	if (!rel->rd_rel->relisshared && autovacuum_start_daemon)
		elog(WARNING,
			 "removable_cutoff(non-shared-rel) can move backward under autovacuum=on");

	/*
	 * No lock or snapshot necessarily prevents oldestXid from advancing past
	 * "xid" while this function runs.  That concerns us only in that we must
	 * not ascribe "xid" to the wrong epoch.  (That may never arise in
	 * isolation testing, but let's set a good example.)  As a crude solution,
	 * retry until nextXid doesn't change.
	 */
	next_fxid = ReadNextFullTransactionId();
	do
	{
		CHECK_FOR_INTERRUPTS();
		next_fxid_before = next_fxid;
		xid = GetOldestNonRemovableTransactionId(rel);
		next_fxid = ReadNextFullTransactionId();
	} while (!FullTransactionIdEquals(next_fxid, next_fxid_before));

	if (rel)
		table_close(rel, AccessShareLock);

	PG_RETURN_FULLTRANSACTIONID(FullTransactionIdFromAllowableAt(next_fxid,
																 xid));
}
