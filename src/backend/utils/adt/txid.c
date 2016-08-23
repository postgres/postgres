/*-------------------------------------------------------------------------
 * txid.c
 *
 *	Export internal transaction IDs to user level.
 *
 * Note that only top-level transaction IDs are ever converted to TXID.
 * This is important because TXIDs frequently persist beyond the global
 * xmin horizon, or may even be shipped to other machines, so we cannot
 * rely on being able to correlate subtransaction IDs with their parents
 * via functions such as SubTransGetTopmostTransaction().
 *
 *
 *	Copyright (c) 2003-2016, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *	64-bit txids: Marko Kreen, Skype Technologies
 *
 *	src/backend/utils/adt/txid.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "libpq/pqformat.h"
#include "postmaster/postmaster.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"


/* txid will be signed int8 in database, so must limit to 63 bits */
#define MAX_TXID   ((uint64) PG_INT64_MAX)

/* Use unsigned variant internally */
typedef uint64 txid;

/* sprintf format code for uint64 */
#define TXID_FMT UINT64_FORMAT

/*
 * If defined, use bsearch() function for searching for txids in snapshots
 * that have more than the specified number of values.
 */
#define USE_BSEARCH_IF_NXIP_GREATER 30


/*
 * Snapshot containing 8byte txids.
 */
typedef struct
{
	/*
	 * 4-byte length hdr, should not be touched directly.
	 *
	 * Explicit embedding is ok as we want always correct alignment anyway.
	 */
	int32		__varsz;

	uint32		nxip;			/* number of txids in xip array */
	txid		xmin;
	txid		xmax;
	/* in-progress txids, xmin <= xip[i] < xmax: */
	txid		xip[FLEXIBLE_ARRAY_MEMBER];
} TxidSnapshot;

#define TXID_SNAPSHOT_SIZE(nxip) \
	(offsetof(TxidSnapshot, xip) + sizeof(txid) * (nxip))
#define TXID_SNAPSHOT_MAX_NXIP \
	((MaxAllocSize - offsetof(TxidSnapshot, xip)) / sizeof(txid))

/*
 * Epoch values from xact.c
 */
typedef struct
{
	TransactionId last_xid;
	uint32		epoch;
} TxidEpoch;


/*
 * Fetch epoch data from xact.c.
 */
static void
load_xid_epoch(TxidEpoch *state)
{
	GetNextXidAndEpoch(&state->last_xid, &state->epoch);
}

/*
 * do a TransactionId -> txid conversion for an XID near the given epoch
 */
static txid
convert_xid(TransactionId xid, const TxidEpoch *state)
{
	uint64		epoch;

	/* return special xid's as-is */
	if (!TransactionIdIsNormal(xid))
		return (txid) xid;

	/* xid can be on either side when near wrap-around */
	epoch = (uint64) state->epoch;
	if (xid > state->last_xid &&
		TransactionIdPrecedes(xid, state->last_xid))
		epoch--;
	else if (xid < state->last_xid &&
			 TransactionIdFollows(xid, state->last_xid))
		epoch++;

	return (epoch << 32) | xid;
}

/*
 * txid comparator for qsort/bsearch
 */
static int
cmp_txid(const void *aa, const void *bb)
{
	txid		a = *(const txid *) aa;
	txid		b = *(const txid *) bb;

	if (a < b)
		return -1;
	if (a > b)
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
sort_snapshot(TxidSnapshot *snap)
{
	txid		last = 0;
	int			nxip,
				idx1,
				idx2;

	if (snap->nxip > 1)
	{
		qsort(snap->xip, snap->nxip, sizeof(txid), cmp_txid);

		/* remove duplicates */
		nxip = snap->nxip;
		idx1 = idx2 = 0;
		while (idx1 < nxip)
		{
			if (snap->xip[idx1] != last)
				last = snap->xip[idx2++] = snap->xip[idx1];
			else
				snap->nxip--;
			idx1++;
		}
	}
}

/*
 * check txid visibility.
 */
static bool
is_visible_txid(txid value, const TxidSnapshot *snap)
{
	if (value < snap->xmin)
		return true;
	else if (value >= snap->xmax)
		return false;
#ifdef USE_BSEARCH_IF_NXIP_GREATER
	else if (snap->nxip > USE_BSEARCH_IF_NXIP_GREATER)
	{
		void	   *res;

		res = bsearch(&value, snap->xip, snap->nxip, sizeof(txid), cmp_txid);
		/* if found, transaction is still in progress */
		return (res) ? false : true;
	}
#endif
	else
	{
		uint32		i;

		for (i = 0; i < snap->nxip; i++)
		{
			if (value == snap->xip[i])
				return false;
		}
		return true;
	}
}

/*
 * helper functions to use StringInfo for TxidSnapshot creation.
 */

static StringInfo
buf_init(txid xmin, txid xmax)
{
	TxidSnapshot snap;
	StringInfo	buf;

	snap.xmin = xmin;
	snap.xmax = xmax;
	snap.nxip = 0;

	buf = makeStringInfo();
	appendBinaryStringInfo(buf, (char *) &snap, TXID_SNAPSHOT_SIZE(0));
	return buf;
}

static void
buf_add_txid(StringInfo buf, txid xid)
{
	TxidSnapshot *snap = (TxidSnapshot *) buf->data;

	/* do this before possible realloc */
	snap->nxip++;

	appendBinaryStringInfo(buf, (char *) &xid, sizeof(xid));
}

static TxidSnapshot *
buf_finalize(StringInfo buf)
{
	TxidSnapshot *snap = (TxidSnapshot *) buf->data;

	SET_VARSIZE(snap, buf->len);

	/* buf is not needed anymore */
	buf->data = NULL;
	pfree(buf);

	return snap;
}

/*
 * simple number parser.
 *
 * We return 0 on error, which is invalid value for txid.
 */
static txid
str2txid(const char *s, const char **endp)
{
	txid		val = 0;
	txid		cutoff = MAX_TXID / 10;
	txid		cutlim = MAX_TXID % 10;

	for (; *s; s++)
	{
		unsigned	d;

		if (*s < '0' || *s > '9')
			break;
		d = *s - '0';

		/*
		 * check for overflow
		 */
		if (val > cutoff || (val == cutoff && d > cutlim))
		{
			val = 0;
			break;
		}

		val = val * 10 + d;
	}
	if (endp)
		*endp = s;
	return val;
}

/*
 * parse snapshot from cstring
 */
static TxidSnapshot *
parse_snapshot(const char *str)
{
	txid		xmin;
	txid		xmax;
	txid		last_val = 0,
				val;
	const char *str_start = str;
	const char *endp;
	StringInfo	buf;

	xmin = str2txid(str, &endp);
	if (*endp != ':')
		goto bad_format;
	str = endp + 1;

	xmax = str2txid(str, &endp);
	if (*endp != ':')
		goto bad_format;
	str = endp + 1;

	/* it should look sane */
	if (xmin == 0 || xmax == 0 || xmin > xmax)
		goto bad_format;

	/* allocate buffer */
	buf = buf_init(xmin, xmax);

	/* loop over values */
	while (*str != '\0')
	{
		/* read next value */
		val = str2txid(str, &endp);
		str = endp;

		/* require the input to be in order */
		if (val < xmin || val >= xmax || val < last_val)
			goto bad_format;

		/* skip duplicates */
		if (val != last_val)
			buf_add_txid(buf, val);
		last_val = val;

		if (*str == ',')
			str++;
		else if (*str != '\0')
			goto bad_format;
	}

	return buf_finalize(buf);

bad_format:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
			 errmsg("invalid input syntax for type txid_snapshot: \"%s\"",
					str_start)));
	return NULL;				/* keep compiler quiet */
}

/*
 * Public functions.
 *
 * txid_current() and txid_current_snapshot() are the only ones that
 * communicate with core xid machinery.  All the others work on data
 * returned by them.
 */

/*
 * txid_current() returns int8
 *
 *	Return the current toplevel transaction ID as TXID
 *	If the current transaction does not have one, one is assigned.
 */
Datum
txid_current(PG_FUNCTION_ARGS)
{
	txid		val;
	TxidEpoch	state;

	/*
	 * Must prevent during recovery because if an xid is not assigned we try
	 * to assign one, which would fail. Programs already rely on this function
	 * to always return a valid current xid, so we should not change this to
	 * return NULL or similar invalid xid.
	 */
	PreventCommandDuringRecovery("txid_current()");

	load_xid_epoch(&state);

	val = convert_xid(GetTopTransactionId(), &state);

	PG_RETURN_INT64(val);
}

/*
 * Same as txid_current() but doesn't assign a new xid if there isn't one
 * yet.
 */
Datum
txid_current_if_assigned(PG_FUNCTION_ARGS)
{
	txid		val;
	TxidEpoch	state;
	TransactionId	topxid = GetTopTransactionIdIfAny();

	if (topxid == InvalidTransactionId)
		PG_RETURN_NULL();

	load_xid_epoch(&state);

	val = convert_xid(topxid, &state);

	PG_RETURN_INT64(val);
}

/*
 * txid_current_snapshot() returns txid_snapshot
 *
 *		Return current snapshot in TXID format
 *
 * Note that only top-transaction XIDs are included in the snapshot.
 */
Datum
txid_current_snapshot(PG_FUNCTION_ARGS)
{
	TxidSnapshot *snap;
	uint32		nxip,
				i;
	TxidEpoch	state;
	Snapshot	cur;

	cur = GetActiveSnapshot();
	if (cur == NULL)
		elog(ERROR, "no active snapshot set");

	load_xid_epoch(&state);

	/*
	 * Compile-time limits on the procarray (MAX_BACKENDS processes plus
	 * MAX_BACKENDS prepared transactions) guarantee nxip won't be too large.
	 */
	StaticAssertStmt(MAX_BACKENDS * 2 <= TXID_SNAPSHOT_MAX_NXIP,
					 "possible overflow in txid_current_snapshot()");

	/* allocate */
	nxip = cur->xcnt;
	snap = palloc(TXID_SNAPSHOT_SIZE(nxip));

	/* fill */
	snap->xmin = convert_xid(cur->xmin, &state);
	snap->xmax = convert_xid(cur->xmax, &state);
	snap->nxip = nxip;
	for (i = 0; i < nxip; i++)
		snap->xip[i] = convert_xid(cur->xip[i], &state);

	/*
	 * We want them guaranteed to be in ascending order.  This also removes
	 * any duplicate xids.  Normally, an XID can only be assigned to one
	 * backend, but when preparing a transaction for two-phase commit, there
	 * is a transient state when both the original backend and the dummy
	 * PGPROC entry reserved for the prepared transaction hold the same XID.
	 */
	sort_snapshot(snap);

	/* set size after sorting, because it may have removed duplicate xips */
	SET_VARSIZE(snap, TXID_SNAPSHOT_SIZE(snap->nxip));

	PG_RETURN_POINTER(snap);
}

/*
 * txid_snapshot_in(cstring) returns txid_snapshot
 *
 *		input function for type txid_snapshot
 */
Datum
txid_snapshot_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	TxidSnapshot *snap;

	snap = parse_snapshot(str);

	PG_RETURN_POINTER(snap);
}

/*
 * txid_snapshot_out(txid_snapshot) returns cstring
 *
 *		output function for type txid_snapshot
 */
Datum
txid_snapshot_out(PG_FUNCTION_ARGS)
{
	TxidSnapshot *snap = (TxidSnapshot *) PG_GETARG_VARLENA_P(0);
	StringInfoData str;
	uint32		i;

	initStringInfo(&str);

	appendStringInfo(&str, TXID_FMT ":", snap->xmin);
	appendStringInfo(&str, TXID_FMT ":", snap->xmax);

	for (i = 0; i < snap->nxip; i++)
	{
		if (i > 0)
			appendStringInfoChar(&str, ',');
		appendStringInfo(&str, TXID_FMT, snap->xip[i]);
	}

	PG_RETURN_CSTRING(str.data);
}

/*
 * txid_snapshot_recv(internal) returns txid_snapshot
 *
 *		binary input function for type txid_snapshot
 *
 *		format: int4 nxip, int8 xmin, int8 xmax, int8 xip
 */
Datum
txid_snapshot_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	TxidSnapshot *snap;
	txid		last = 0;
	int			nxip;
	int			i;
	txid		xmin,
				xmax;

	/* load and validate nxip */
	nxip = pq_getmsgint(buf, 4);
	if (nxip < 0 || nxip > TXID_SNAPSHOT_MAX_NXIP)
		goto bad_format;

	xmin = pq_getmsgint64(buf);
	xmax = pq_getmsgint64(buf);
	if (xmin == 0 || xmax == 0 || xmin > xmax || xmax > MAX_TXID)
		goto bad_format;

	snap = palloc(TXID_SNAPSHOT_SIZE(nxip));
	snap->xmin = xmin;
	snap->xmax = xmax;

	for (i = 0; i < nxip; i++)
	{
		txid		cur = pq_getmsgint64(buf);

		if (cur < last || cur < xmin || cur >= xmax)
			goto bad_format;

		/* skip duplicate xips */
		if (cur == last)
		{
			i--;
			nxip--;
			continue;
		}

		snap->xip[i] = cur;
		last = cur;
	}
	snap->nxip = nxip;
	SET_VARSIZE(snap, TXID_SNAPSHOT_SIZE(nxip));
	PG_RETURN_POINTER(snap);

bad_format:
	ereport(ERROR,
			(errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
			 errmsg("invalid external txid_snapshot data")));
	PG_RETURN_POINTER(NULL);	/* keep compiler quiet */
}

/*
 * txid_snapshot_send(txid_snapshot) returns bytea
 *
 *		binary output function for type txid_snapshot
 *
 *		format: int4 nxip, int8 xmin, int8 xmax, int8 xip
 */
Datum
txid_snapshot_send(PG_FUNCTION_ARGS)
{
	TxidSnapshot *snap = (TxidSnapshot *) PG_GETARG_VARLENA_P(0);
	StringInfoData buf;
	uint32		i;

	pq_begintypsend(&buf);
	pq_sendint(&buf, snap->nxip, 4);
	pq_sendint64(&buf, snap->xmin);
	pq_sendint64(&buf, snap->xmax);
	for (i = 0; i < snap->nxip; i++)
		pq_sendint64(&buf, snap->xip[i]);
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * txid_visible_in_snapshot(int8, txid_snapshot) returns bool
 *
 *		is txid visible in snapshot ?
 */
Datum
txid_visible_in_snapshot(PG_FUNCTION_ARGS)
{
	txid		value = PG_GETARG_INT64(0);
	TxidSnapshot *snap = (TxidSnapshot *) PG_GETARG_VARLENA_P(1);

	PG_RETURN_BOOL(is_visible_txid(value, snap));
}

/*
 * txid_snapshot_xmin(txid_snapshot) returns int8
 *
 *		return snapshot's xmin
 */
Datum
txid_snapshot_xmin(PG_FUNCTION_ARGS)
{
	TxidSnapshot *snap = (TxidSnapshot *) PG_GETARG_VARLENA_P(0);

	PG_RETURN_INT64(snap->xmin);
}

/*
 * txid_snapshot_xmax(txid_snapshot) returns int8
 *
 *		return snapshot's xmax
 */
Datum
txid_snapshot_xmax(PG_FUNCTION_ARGS)
{
	TxidSnapshot *snap = (TxidSnapshot *) PG_GETARG_VARLENA_P(0);

	PG_RETURN_INT64(snap->xmax);
}

/*
 * txid_snapshot_xip(txid_snapshot) returns setof int8
 *
 *		return in-progress TXIDs in snapshot.
 */
Datum
txid_snapshot_xip(PG_FUNCTION_ARGS)
{
	FuncCallContext *fctx;
	TxidSnapshot *snap;
	txid		value;

	/* on first call initialize snap_state and get copy of snapshot */
	if (SRF_IS_FIRSTCALL())
	{
		TxidSnapshot *arg = (TxidSnapshot *) PG_GETARG_VARLENA_P(0);

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
		SRF_RETURN_NEXT(fctx, Int64GetDatum(value));
	}
	else
	{
		SRF_RETURN_DONE(fctx);
	}
}
