/*--------------------------------------------------------------------------
 *
 * xid_wraparound.c
 *		Utilities for testing XID wraparound
 *
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/xid_wraparound/xid_wraparound.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "storage/proc.h"
#include "utils/xid8.h"

PG_MODULE_MAGIC;

static int64 consume_xids_shortcut(void);
static FullTransactionId consume_xids_common(FullTransactionId untilxid, uint64 nxids);

/*
 * Consume the specified number of XIDs.
 */
PG_FUNCTION_INFO_V1(consume_xids);
Datum
consume_xids(PG_FUNCTION_ARGS)
{
	int64		nxids = PG_GETARG_INT64(0);
	FullTransactionId lastxid;

	if (nxids < 0)
		elog(ERROR, "invalid nxids argument: %lld", (long long) nxids);

	if (nxids == 0)
		lastxid = ReadNextFullTransactionId();
	else
		lastxid = consume_xids_common(InvalidFullTransactionId, (uint64) nxids);

	PG_RETURN_FULLTRANSACTIONID(lastxid);
}

/*
 * Consume XIDs, up to the given XID.
 */
PG_FUNCTION_INFO_V1(consume_xids_until);
Datum
consume_xids_until(PG_FUNCTION_ARGS)
{
	FullTransactionId targetxid = PG_GETARG_FULLTRANSACTIONID(0);
	FullTransactionId lastxid;

	if (!FullTransactionIdIsNormal(targetxid))
		elog(ERROR, "targetxid %llu is not normal",
			 (unsigned long long) U64FromFullTransactionId(targetxid));

	lastxid = consume_xids_common(targetxid, 0);

	PG_RETURN_FULLTRANSACTIONID(lastxid);
}

/*
 * Common functionality between the two public functions.
 */
static FullTransactionId
consume_xids_common(FullTransactionId untilxid, uint64 nxids)
{
	FullTransactionId lastxid;
	uint64		last_reported_at = 0;
	uint64		consumed = 0;

	/* Print a NOTICE every REPORT_INTERVAL xids */
#define REPORT_INTERVAL (10 * 1000000)

	/* initialize 'lastxid' with the system's current next XID */
	lastxid = ReadNextFullTransactionId();

	/*
	 * We consume XIDs by calling GetNewTransactionId(true), which marks the
	 * consumed XIDs as subtransactions of the current top-level transaction.
	 * For that to work, this transaction must have a top-level XID.
	 *
	 * GetNewTransactionId registers them in the subxid cache in PGPROC, until
	 * the cache overflows, but beyond that, we don't keep track of the
	 * consumed XIDs.
	 */
	(void) GetTopTransactionId();

	for (;;)
	{
		uint64		xids_left;

		CHECK_FOR_INTERRUPTS();

		/* How many XIDs do we have left to consume? */
		if (nxids > 0)
		{
			if (consumed >= nxids)
				break;
			xids_left = nxids - consumed;
		}
		else
		{
			if (FullTransactionIdFollowsOrEquals(lastxid, untilxid))
				break;
			xids_left = U64FromFullTransactionId(untilxid) - U64FromFullTransactionId(lastxid);
		}

		/*
		 * If we still have plenty of XIDs to consume, try to take a shortcut
		 * and bump up the nextXid counter directly.
		 */
		if (xids_left > 2000 &&
			consumed - last_reported_at < REPORT_INTERVAL &&
			MyProc->subxidStatus.overflowed)
		{
			int64		consumed_by_shortcut = consume_xids_shortcut();

			if (consumed_by_shortcut > 0)
			{
				consumed += consumed_by_shortcut;
				continue;
			}
		}

		/* Slow path: Call GetNewTransactionId to allocate a new XID. */
		lastxid = GetNewTransactionId(true);
		consumed++;

		/* Report progress */
		if (consumed - last_reported_at >= REPORT_INTERVAL)
		{
			if (nxids > 0)
				elog(NOTICE, "consumed %llu / %llu XIDs, latest %u:%u",
					 (unsigned long long) consumed, (unsigned long long) nxids,
					 EpochFromFullTransactionId(lastxid),
					 XidFromFullTransactionId(lastxid));
			else
				elog(NOTICE, "consumed up to %u:%u / %u:%u",
					 EpochFromFullTransactionId(lastxid),
					 XidFromFullTransactionId(lastxid),
					 EpochFromFullTransactionId(untilxid),
					 XidFromFullTransactionId(untilxid));
			last_reported_at = consumed;
		}
	}

	return lastxid;
}

/*
 * These constants copied from .c files, because they're private.
 */
#define COMMIT_TS_XACTS_PER_PAGE (BLCKSZ / 10)
#define SUBTRANS_XACTS_PER_PAGE (BLCKSZ / sizeof(TransactionId))
#define CLOG_XACTS_PER_BYTE 4
#define CLOG_XACTS_PER_PAGE (BLCKSZ * CLOG_XACTS_PER_BYTE)

/*
 * All the interesting action in GetNewTransactionId happens when we extend
 * the SLRUs, or at the uint32 wraparound. If the nextXid counter is not close
 * to any of those interesting values, take a shortcut and bump nextXID
 * directly, close to the next "interesting" value.
 */
static inline uint32
XidSkip(FullTransactionId fullxid)
{
	uint32		low = XidFromFullTransactionId(fullxid);
	uint32		rem;
	uint32		distance;

	if (low < 5 || low >= UINT32_MAX - 5)
		return 0;
	distance = UINT32_MAX - 5 - low;

	rem = low % COMMIT_TS_XACTS_PER_PAGE;
	if (rem == 0)
		return 0;
	distance = Min(distance, COMMIT_TS_XACTS_PER_PAGE - rem);

	rem = low % SUBTRANS_XACTS_PER_PAGE;
	if (rem == 0)
		return 0;
	distance = Min(distance, SUBTRANS_XACTS_PER_PAGE - rem);

	rem = low % CLOG_XACTS_PER_PAGE;
	if (rem == 0)
		return 0;
	distance = Min(distance, CLOG_XACTS_PER_PAGE - rem);

	return distance;
}

static int64
consume_xids_shortcut(void)
{
	FullTransactionId nextXid;
	uint32		consumed;

	LWLockAcquire(XidGenLock, LW_EXCLUSIVE);
	nextXid = TransamVariables->nextXid;

	/*
	 * Go slow near the "interesting values". The interesting zones include 5
	 * transactions before and after SLRU page switches.
	 */
	consumed = XidSkip(nextXid);
	if (consumed > 0)
		TransamVariables->nextXid.value += (uint64) consumed;

	LWLockRelease(XidGenLock);

	return consumed;
}
