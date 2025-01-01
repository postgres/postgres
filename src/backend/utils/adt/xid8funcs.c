/*-------------------------------------------------------------------------
 * xid8funcs.c
 *
 *	Export internal transaction IDs to user level.
 *
 * Note that only top-level transaction IDs are exposed to user sessions.
 * This is important because xid8s frequently persist beyond the global
 * xmin horizon, or may even be shipped to other machines, so we cannot
 * rely on being able to correlate subtransaction IDs with their parents
 * via functions such as SubTransGetTopmostTransaction().
 *
 * These functions are used to support the txid_XXX functions and the newer
 * pg_current_xact_id, pg_current_snapshot and related fmgr functions, since
 * the only difference between them is whether they expose xid8 or int8 values
 * to users.  The txid_XXX variants should eventually be dropped.
 *
 *
 *	Copyright (c) 2003-2025, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *	64-bit txids: Marko Kreen, Skype Technologies
 *
 *	src/backend/utils/adt/xid8funcs.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "funcapi.h"
#include "lib/qunique.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/lwlock.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"
#include "utils/xid8.h"


/*
 * If defined, use bsearch() function for searching for xid8s in snapshots
 * that have more than the specified number of values.
 */
#define USE_BSEARCH_IF_NXIP_GREATER 30


/*
 * Snapshot containing FullTransactionIds.
 */
typedef struct
{
	/*
	 * 4-byte length hdr, should not be touched directly.
	 *
	 * Explicit embedding is ok as we want always correct alignment anyway.
	 */
	int32		__varsz;

	uint32		nxip;			/* number of fxids in xip array */
	FullTransactionId xmin;
	FullTransactionId xmax;
	/* in-progress fxids, xmin <= xip[i] < xmax: */
	FullTransactionId xip[FLEXIBLE_ARRAY_MEMBER];
} pg_snapshot;

#define PG_SNAPSHOT_SIZE(nxip) \
	(offsetof(pg_snapshot, xip) + sizeof(FullTransactionId) * (nxip))
#define PG_SNAPSHOT_MAX_NXIP \
	((MaxAllocSize - offsetof(pg_snapshot, xip)) / sizeof(FullTransactionId))

/*
 * Compile-time limits on the procarray (MAX_BACKENDS processes plus
 * MAX_BACKENDS prepared transactions) guarantee nxip won't be too large.
 */
StaticAssertDecl(MAX_BACKENDS * 2 <= PG_SNAPSHOT_MAX_NXIP,
				 "possible overflow in pg_current_snapshot()");


/*
 * Helper to get a TransactionId from a 64-bit xid with wraparound detection.
 *
 * It is an ERROR if the xid is in the future.  Otherwise, returns true if
 * the transaction is still new enough that we can determine whether it
 * committed and false otherwise.  If *extracted_xid is not NULL, it is set
 * to the low 32 bits of the transaction ID (i.e. the actual XID, without the
 * epoch).
 *
 * The caller must hold XactTruncationLock since it's dealing with arbitrary
 * XIDs, and must continue to hold it until it's done with any clog lookups
 * relating to those XIDs.
 */
static bool
TransactionIdInRecentPast(FullTransactionId fxid, TransactionId *extracted_xid)
{
	TransactionId xid = XidFromFullTransactionId(fxid);
	uint32		now_epoch;
	TransactionId now_epoch_next_xid;
	FullTransactionId now_fullxid;
	TransactionId oldest_xid;
	FullTransactionId oldest_fxid;

	now_fullxid = ReadNextFullTransactionId();
	now_epoch_next_xid = XidFromFullTransactionId(now_fullxid);
	now_epoch = EpochFromFullTransactionId(now_fullxid);

	if (extracted_xid != NULL)
		*extracted_xid = xid;

	if (!TransactionIdIsValid(xid))
		return false;

	/* For non-normal transaction IDs, we can ignore the epoch. */
	if (!TransactionIdIsNormal(xid))
		return true;

	/* If the transaction ID is in the future, throw an error. */
	if (!FullTransactionIdPrecedes(fxid, now_fullxid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("transaction ID %llu is in the future",
						(unsigned long long) U64FromFullTransactionId(fxid))));

	/*
	 * TransamVariables->oldestClogXid is protected by XactTruncationLock, but
	 * we don't acquire that lock here.  Instead, we require the caller to
	 * acquire it, because the caller is presumably going to look up the
	 * returned XID.  If we took and released the lock within this function, a
	 * CLOG truncation could occur before the caller finished with the XID.
	 */
	Assert(LWLockHeldByMe(XactTruncationLock));

	/*
	 * If fxid is not older than TransamVariables->oldestClogXid, the relevant
	 * CLOG entry is guaranteed to still exist.  Convert
	 * TransamVariables->oldestClogXid into a FullTransactionId to compare it
	 * with fxid.  Determine the right epoch knowing that oldest_fxid
	 * shouldn't be more than 2^31 older than now_fullxid.
	 */
	oldest_xid = TransamVariables->oldestClogXid;
	Assert(TransactionIdPrecedesOrEquals(oldest_xid, now_epoch_next_xid));
	if (oldest_xid <= now_epoch_next_xid)
	{
		oldest_fxid = FullTransactionIdFromEpochAndXid(now_epoch, oldest_xid);
	}
	else
	{
		Assert(now_epoch > 0);
		oldest_fxid = FullTransactionIdFromEpochAndXid(now_epoch - 1, oldest_xid);
	}
	return !FullTransactionIdPrecedes(fxid, oldest_fxid);
}

/*
 * Convert a TransactionId obtained from a snapshot held by the caller to a
 * FullTransactionId.  Use next_fxid as a reference FullTransactionId, so that
 * we can compute the high order bits.  It must have been obtained by the
 * caller with ReadNextFullTransactionId() after the snapshot was created.
 */
static FullTransactionId
widen_snapshot_xid(TransactionId xid, FullTransactionId next_fxid)
{
	TransactionId next_xid = XidFromFullTransactionId(next_fxid);
	uint32		epoch = EpochFromFullTransactionId(next_fxid);

	/* Special transaction ID. */
	if (!TransactionIdIsNormal(xid))
		return FullTransactionIdFromEpochAndXid(0, xid);

	/*
	 * The 64 bit result must be <= next_fxid, since next_fxid hadn't been
	 * issued yet when the snapshot was created.  Every TransactionId in the
	 * snapshot must therefore be from the same epoch as next_fxid, or the
	 * epoch before.  We know this because next_fxid is never allow to get
	 * more than one epoch ahead of the TransactionIds in any snapshot.
	 */
	if (xid > next_xid)
		epoch--;

	return FullTransactionIdFromEpochAndXid(epoch, xid);
}

/*
 * txid comparator for qsort/bsearch
 */
static int
cmp_fxid(const void *aa, const void *bb)
{
	FullTransactionId a = *(const FullTransactionId *) aa;
	FullTransactionId b = *(const FullTransactionId *) bb;

	if (FullTransactionIdPrecedes(a, b))
		return -1;
	if (FullTransactionIdPrecedes(b, a))
		return 1;
	return 0;
}

/*
 * Sort a snapshot's txids, so we can use bsearch() later.  Also remove
 * any duplicates.
 *
 * For consistency of on-disk representation, we always sort even if bsearch
 * will not be used.
 */
static void
sort_snapshot(pg_snapshot *snap)
{
	if (snap->nxip > 1)
	{
		qsort(snap->xip, snap->nxip, sizeof(FullTransactionId), cmp_fxid);
		snap->nxip = qunique(snap->xip, snap->nxip, sizeof(FullTransactionId),
							 cmp_fxid);
	}
}

/*
 * check fxid visibility.
 */
static bool
is_visible_fxid(FullTransactionId value, const pg_snapshot *snap)
{
	if (FullTransactionIdPrecedes(value, snap->xmin))
		return true;
	else if (!FullTransactionIdPrecedes(value, snap->xmax))
		return false;
#ifdef USE_BSEARCH_IF_NXIP_GREATER
	else if (snap->nxip > USE_BSEARCH_IF_NXIP_GREATER)
	{
		void	   *res;

		res = bsearch(&value, snap->xip, snap->nxip, sizeof(FullTransactionId),
					  cmp_fxid);
		/* if found, transaction is still in progress */
		return (res) ? false : true;
	}
#endif
	else
	{
		uint32		i;

		for (i = 0; i < snap->nxip; i++)
		{
			if (FullTransactionIdEquals(value, snap->xip[i]))
				return false;
		}
		return true;
	}
}

/*
 * helper functions to use StringInfo for pg_snapshot creation.
 */

static StringInfo
buf_init(FullTransactionId xmin, FullTransactionId xmax)
{
	pg_snapshot snap;
	StringInfo	buf;

	snap.xmin = xmin;
	snap.xmax = xmax;
	snap.nxip = 0;

	buf = makeStringInfo();
	appendBinaryStringInfo(buf, &snap, PG_SNAPSHOT_SIZE(0));
	return buf;
}

static void
buf_add_txid(StringInfo buf, FullTransactionId fxid)
{
	pg_snapshot *snap = (pg_snapshot *) buf->data;

	/* do this before possible realloc */
	snap->nxip++;

	appendBinaryStringInfo(buf, &fxid, sizeof(fxid));
}

static pg_snapshot *
buf_finalize(StringInfo buf)
{
	pg_snapshot *snap = (pg_snapshot *) buf->data;

	SET_VARSIZE(snap, buf->len);

	/* buf is not needed anymore */
	buf->data = NULL;
	pfree(buf);

	return snap;
}

/*
 * parse snapshot from cstring
 */
static pg_snapshot *
parse_snapshot(const char *str, Node *escontext)
{
	FullTransactionId xmin;
	FullTransactionId xmax;
	FullTransactionId last_val = InvalidFullTransactionId;
	FullTransactionId val;
	const char *str_start = str;
	char	   *endp;
	StringInfo	buf;

	xmin = FullTransactionIdFromU64(strtou64(str, &endp, 10));
	if (*endp != ':')
		goto bad_format;
	str = endp + 1;

	xmax = FullTransactionIdFromU64(strtou64(str, &endp, 10));
	if (*endp != ':')
		goto bad_format;
	str = endp + 1;

	/* it should look sane */
	if (!FullTransactionIdIsValid(xmin) ||
		!FullTransactionIdIsValid(xmax) ||
		FullTransactionIdPrecedes(xmax, xmin))
		goto bad_format;

	/* allocate buffer */
	buf = buf_init(xmin, xmax);

	/* loop over values */
	while (*str != '\0')
	{
		/* read next value */
		val = FullTransactionIdFromU64(strtou64(str, &endp, 10));
		str = endp;

		/* require the input to be in order */
		if (FullTransactionIdPrecedes(val, xmin) ||
			FullTransactionIdFollowsOrEquals(val, xmax) ||
			FullTransactionIdPrecedes(val, last_val))
			goto bad_format;

		/* skip duplicates */
		if (!FullTransactionIdEquals(val, last_val))
			buf_add_txid(buf, val);
		last_val = val;

		if (*str == ',')
			str++;
		else if (*str != '\0')
			goto bad_format;
	}

	return buf_finalize(buf);

bad_format:
	ereturn(escontext, NULL,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type %s: \"%s\"",
					"pg_snapshot", str_start)));
}

/*
 * pg_current_xact_id() returns xid8
 *
 *	Return the current toplevel full transaction ID.
 *	If the current transaction does not have one, one is assigned.
 */
Datum
pg_current_xact_id(PG_FUNCTION_ARGS)
{
	/*
	 * Must prevent during recovery because if an xid is not assigned we try
	 * to assign one, which would fail. Programs already rely on this function
	 * to always return a valid current xid, so we should not change this to
	 * return NULL or similar invalid xid.
	 */
	PreventCommandDuringRecovery("pg_current_xact_id()");

	PG_RETURN_FULLTRANSACTIONID(GetTopFullTransactionId());
}

/*
 * Same as pg_current_xact_id() but doesn't assign a new xid if there
 * isn't one yet.
 */
Datum
pg_current_xact_id_if_assigned(PG_FUNCTION_ARGS)
{
	FullTransactionId topfxid = GetTopFullTransactionIdIfAny();

	if (!FullTransactionIdIsValid(topfxid))
		PG_RETURN_NULL();

	PG_RETURN_FULLTRANSACTIONID(topfxid);
}

/*
 * pg_current_snapshot() returns pg_snapshot
 *
 *		Return current snapshot
 *
 * Note that only top-transaction XIDs are included in the snapshot.
 */
Datum
pg_current_snapshot(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap;
	uint32		nxip,
				i;
	Snapshot	cur;
	FullTransactionId next_fxid = ReadNextFullTransactionId();

	cur = GetActiveSnapshot();
	if (cur == NULL)
		elog(ERROR, "no active snapshot set");

	/* allocate */
	nxip = cur->xcnt;
	snap = palloc(PG_SNAPSHOT_SIZE(nxip));

	/* fill */
	snap->xmin = widen_snapshot_xid(cur->xmin, next_fxid);
	snap->xmax = widen_snapshot_xid(cur->xmax, next_fxid);
	snap->nxip = nxip;
	for (i = 0; i < nxip; i++)
		snap->xip[i] = widen_snapshot_xid(cur->xip[i], next_fxid);

	/*
	 * We want them guaranteed to be in ascending order.  This also removes
	 * any duplicate xids.  Normally, an XID can only be assigned to one
	 * backend, but when preparing a transaction for two-phase commit, there
	 * is a transient state when both the original backend and the dummy
	 * PGPROC entry reserved for the prepared transaction hold the same XID.
	 */
	sort_snapshot(snap);

	/* set size after sorting, because it may have removed duplicate xips */
	SET_VARSIZE(snap, PG_SNAPSHOT_SIZE(snap->nxip));

	PG_RETURN_POINTER(snap);
}

/*
 * pg_snapshot_in(cstring) returns pg_snapshot
 *
 *		input function for type pg_snapshot
 */
Datum
pg_snapshot_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	pg_snapshot *snap;

	snap = parse_snapshot(str, fcinfo->context);

	PG_RETURN_POINTER(snap);
}

/*
 * pg_snapshot_out(pg_snapshot) returns cstring
 *
 *		output function for type pg_snapshot
 */
Datum
pg_snapshot_out(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(0);
	StringInfoData str;
	uint32		i;

	initStringInfo(&str);

	appendStringInfo(&str, UINT64_FORMAT ":",
					 U64FromFullTransactionId(snap->xmin));
	appendStringInfo(&str, UINT64_FORMAT ":",
					 U64FromFullTransactionId(snap->xmax));

	for (i = 0; i < snap->nxip; i++)
	{
		if (i > 0)
			appendStringInfoChar(&str, ',');
		appendStringInfo(&str, UINT64_FORMAT,
						 U64FromFullTransactionId(snap->xip[i]));
	}

	PG_RETURN_CSTRING(str.data);
}

/*
 * pg_snapshot_recv(internal) returns pg_snapshot
 *
 *		binary input function for type pg_snapshot
 *
 *		format: int4 nxip, int8 xmin, int8 xmax, int8 xip
 */
Datum
pg_snapshot_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	pg_snapshot *snap;
	FullTransactionId last = InvalidFullTransactionId;
	int			nxip;
	int			i;
	FullTransactionId xmin;
	FullTransactionId xmax;

	/* load and validate nxip */
	nxip = pq_getmsgint(buf, 4);
	if (nxip < 0 || nxip > PG_SNAPSHOT_MAX_NXIP)
		goto bad_format;

	xmin = FullTransactionIdFromU64((uint64) pq_getmsgint64(buf));
	xmax = FullTransactionIdFromU64((uint64) pq_getmsgint64(buf));
	if (!FullTransactionIdIsValid(xmin) ||
		!FullTransactionIdIsValid(xmax) ||
		FullTransactionIdPrecedes(xmax, xmin))
		goto bad_format;

	snap = palloc(PG_SNAPSHOT_SIZE(nxip));
	snap->xmin = xmin;
	snap->xmax = xmax;

	for (i = 0; i < nxip; i++)
	{
		FullTransactionId cur =
			FullTransactionIdFromU64((uint64) pq_getmsgint64(buf));

		if (FullTransactionIdPrecedes(cur, last) ||
			FullTransactionIdPrecedes(cur, xmin) ||
			FullTransactionIdPrecedes(xmax, cur))
			goto bad_format;

		/* skip duplicate xips */
		if (FullTransactionIdEquals(cur, last))
		{
			i--;
			nxip--;
			continue;
		}

		snap->xip[i] = cur;
		last = cur;
	}
	snap->nxip = nxip;
	SET_VARSIZE(snap, PG_SNAPSHOT_SIZE(nxip));
	PG_RETURN_POINTER(snap);

bad_format:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
			 errmsg("invalid external pg_snapshot data")));
	PG_RETURN_POINTER(NULL);	/* keep compiler quiet */
}

/*
 * pg_snapshot_send(pg_snapshot) returns bytea
 *
 *		binary output function for type pg_snapshot
 *
 *		format: int4 nxip, u64 xmin, u64 xmax, u64 xip...
 */
Datum
pg_snapshot_send(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(0);
	StringInfoData buf;
	uint32		i;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, snap->nxip);
	pq_sendint64(&buf, (int64) U64FromFullTransactionId(snap->xmin));
	pq_sendint64(&buf, (int64) U64FromFullTransactionId(snap->xmax));
	for (i = 0; i < snap->nxip; i++)
		pq_sendint64(&buf, (int64) U64FromFullTransactionId(snap->xip[i]));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * pg_visible_in_snapshot(xid8, pg_snapshot) returns bool
 *
 *		is txid visible in snapshot ?
 */
Datum
pg_visible_in_snapshot(PG_FUNCTION_ARGS)
{
	FullTransactionId value = PG_GETARG_FULLTRANSACTIONID(0);
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(1);

	PG_RETURN_BOOL(is_visible_fxid(value, snap));
}

/*
 * pg_snapshot_xmin(pg_snapshot) returns xid8
 *
 *		return snapshot's xmin
 */
Datum
pg_snapshot_xmin(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(0);

	PG_RETURN_FULLTRANSACTIONID(snap->xmin);
}

/*
 * pg_snapshot_xmax(pg_snapshot) returns xid8
 *
 *		return snapshot's xmax
 */
Datum
pg_snapshot_xmax(PG_FUNCTION_ARGS)
{
	pg_snapshot *snap = (pg_snapshot *) PG_GETARG_VARLENA_P(0);

	PG_RETURN_FULLTRANSACTIONID(snap->xmax);
}

/*
 * pg_snapshot_xip(pg_snapshot) returns setof xid8
 *
 *		return in-progress xid8s in snapshot.
 */
Datum
pg_snapshot_xip(PG_FUNCTION_ARGS)
{
	FuncCallContext *fctx;
	pg_snapshot *snap;
	FullTransactionId value;

	/* on first call initialize fctx and get copy of snapshot */
	if (SRF_IS_FIRSTCALL())
	{
		pg_snapshot *arg = (pg_snapshot *) PG_GETARG_VARLENA_P(0);

		fctx = SRF_FIRSTCALL_INIT();

		/* make a copy of user snapshot */
		snap = MemoryContextAlloc(fctx->multi_call_memory_ctx, VARSIZE(arg));
		memcpy(snap, arg, VARSIZE(arg));

		fctx->user_fctx = snap;
	}

	/* return values one-by-one */
	fctx = SRF_PERCALL_SETUP();
	snap = fctx->user_fctx;
	if (fctx->call_cntr < snap->nxip)
	{
		value = snap->xip[fctx->call_cntr];
		SRF_RETURN_NEXT(fctx, FullTransactionIdGetDatum(value));
	}
	else
	{
		SRF_RETURN_DONE(fctx);
	}
}

/*
 * Report the status of a recent transaction ID, or null for wrapped,
 * truncated away or otherwise too old XIDs.
 *
 * The passed epoch-qualified xid is treated as a normal xid, not a
 * multixact id.
 *
 * If it points to a committed subxact the result is the subxact status even
 * though the parent xact may still be in progress or may have aborted.
 */
Datum
pg_xact_status(PG_FUNCTION_ARGS)
{
	const char *status;
	FullTransactionId fxid = PG_GETARG_FULLTRANSACTIONID(0);
	TransactionId xid;

	/*
	 * We must protect against concurrent truncation of clog entries to avoid
	 * an I/O error on SLRU lookup.
	 */
	LWLockAcquire(XactTruncationLock, LW_SHARED);
	if (TransactionIdInRecentPast(fxid, &xid))
	{
		Assert(TransactionIdIsValid(xid));

		/*
		 * Like when doing visibility checks on a row, check whether the
		 * transaction is still in progress before looking into the CLOG.
		 * Otherwise we would incorrectly return "committed" for a transaction
		 * that is committing and has already updated the CLOG, but hasn't
		 * removed its XID from the proc array yet. (See comment on that race
		 * condition at the top of heapam_visibility.c)
		 */
		if (TransactionIdIsInProgress(xid))
			status = "in progress";
		else if (TransactionIdDidCommit(xid))
			status = "committed";
		else
		{
			/* it must have aborted or crashed */
			status = "aborted";
		}
	}
	else
	{
		status = NULL;
	}
	LWLockRelease(XactTruncationLock);

	if (status == NULL)
		PG_RETURN_NULL();
	else
		PG_RETURN_TEXT_P(cstring_to_text(status));
}
