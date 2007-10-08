/*-------------------------------------------------------------------------
 * txid.c
 *
 *	Export backend internal tranasction id's to user level.
 *
 *	Copyright (c) 2003-2007, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *	64-bit txids: Marko Kreen, Skype Technologies
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "funcapi.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#ifdef INT64_IS_BUSTED
#error txid needs working int64
#endif

/* txid will be signed int8 in database */
#define MAX_TXID   UINT64CONST(0x7FFFFFFFFFFFFFFF)

/*
 * If defined, use bsearch() function for searching
 * txid's inside snapshots that have more than given values.
 */
#define USE_BSEARCH_IF_NXIP_GREATER 30

/* format code for uint64 to appendStringInfo */
#define TXID_FMT UINT64_FORMAT

/* Use unsigned variant internally */
typedef uint64 txid;

/*
 * Snapshot for 8byte txids.
 */
typedef struct
{
	/*
	 * 4-byte length hdr, should not be touched directly.
	 *
	 * Explicit embedding is ok as we want always correct
	 * alignment anyway.
	 */
    int32       __varsz;
	
    uint32      nxip;		/* number of txids in xip array */
    txid		xmin;
    txid		xmax;
    txid		xip[1];		/* in-progress txids */
}   TxidSnapshot;

#define TXID_SNAPSHOT_SIZE(nxip) (offsetof(TxidSnapshot, xip) + sizeof(txid) * (nxip))

/*
 * Epoch values from backend.
 */
typedef struct {
	uint64		last_value;
	uint64		epoch;
}	TxidEpoch;

/* public functions */
Datum       txid_snapshot_in(PG_FUNCTION_ARGS);
Datum       txid_snapshot_out(PG_FUNCTION_ARGS);
Datum       txid_current(PG_FUNCTION_ARGS);
Datum       txid_current_snapshot(PG_FUNCTION_ARGS);
Datum       txid_snapshot_xmin(PG_FUNCTION_ARGS);
Datum       txid_snapshot_xmax(PG_FUNCTION_ARGS);
Datum       txid_snapshot_xip(PG_FUNCTION_ARGS);
Datum       txid_visible_in_snapshot(PG_FUNCTION_ARGS);

/* public function tags */
PG_FUNCTION_INFO_V1(txid_snapshot_in);
PG_FUNCTION_INFO_V1(txid_snapshot_out);
PG_FUNCTION_INFO_V1(txid_current);
PG_FUNCTION_INFO_V1(txid_current_snapshot);
PG_FUNCTION_INFO_V1(txid_snapshot_xmin);
PG_FUNCTION_INFO_V1(txid_snapshot_xmax);
PG_FUNCTION_INFO_V1(txid_snapshot_xip);
PG_FUNCTION_INFO_V1(txid_visible_in_snapshot);

/*
 * do a TransactionId -> txid conversion
 */
static txid
convert_xid(TransactionId xid, const TxidEpoch *state)
{
	uint64 epoch;

	/* return special xid's as-is */
	if (xid < FirstNormalTransactionId)
		return xid;

	/* xid can on both sides on wrap-around */
	epoch = state->epoch;
	if (TransactionIdPrecedes(xid, state->last_value)) {
		if (xid > state->last_value)
			epoch--;
	} else if (TransactionIdFollows(xid, state->last_value)) {
		if (xid < state->last_value)
			epoch++;
	}
	return (epoch << 32) | xid;
}

/*
 * Fetch epoch data from backend.
 */
static void
load_xid_epoch(TxidEpoch *state)
{
	TransactionId	xid;
	uint32			epoch;

	GetNextXidAndEpoch(&xid, &epoch);

	state->epoch = epoch;
	state->last_value = xid;
}

/*
 * compare txid in memory.
 */
static int
cmp_txid(const void *aa, const void *bb)
{
	const uint64 *a = aa;
	const uint64 *b = bb;
	if (*a < *b)
		return -1;
	if (*a > *b)
		return 1;
	return 0;
}

/*
 * order txids, for bsearch().
 */
static void
sort_snapshot(TxidSnapshot *snap)
{
	if (snap->nxip > 1)
		qsort(snap->xip, snap->nxip, sizeof(txid), cmp_txid);
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
		void *res;
		res = bsearch(&value, snap->xip, snap->nxip, sizeof(txid), cmp_txid);
		return (res) ? false : true;
	}
#endif
	else
	{
		int i;
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
	StringInfo buf;

	snap.xmin = xmin;
	snap.xmax = xmax;
	snap.nxip = 0;

	buf = makeStringInfo();
	appendBinaryStringInfo(buf, (char *)&snap, TXID_SNAPSHOT_SIZE(0));
	return buf;
}

static void
buf_add_txid(StringInfo buf, txid xid)
{
	TxidSnapshot *snap = (TxidSnapshot *)buf->data;

	/* do it before possible realloc */
	snap->nxip++;

	appendBinaryStringInfo(buf, (char *)&xid, sizeof(xid));
}

static TxidSnapshot *
buf_finalize(StringInfo buf)
{
	TxidSnapshot *snap = (TxidSnapshot *)buf->data;
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
	txid val = 0;
	txid cutoff = MAX_TXID / 10;
	txid cutlim = MAX_TXID % 10;

	for (; *s; s++)
	{
		unsigned d;

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
	txid		last_val = 0, val;
	const char *str_start = str;
	const char *endp;
	StringInfo  buf;

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
		if (val < xmin || val >= xmax || val <= last_val)
			goto bad_format;
		
		buf_add_txid(buf, val);
		last_val = val;

		if (*str == ',')
			str++;
		else if (*str != '\0')
			goto bad_format;
	}

	return buf_finalize(buf);

bad_format:
	elog(ERROR, "invalid input for txid_snapshot: \"%s\"", str_start);
	return NULL;
}

/*
 * Public functions
 */

/*
 * txid_current() returns int8
 *
 *		Return the current transaction ID
 */
Datum
txid_current(PG_FUNCTION_ARGS)
{
	txid val;
	TxidEpoch state;

	load_xid_epoch(&state);

	val = convert_xid(GetTopTransactionId(), &state);

	PG_RETURN_INT64(val);
}

/*
 * txid_current_snapshot() returns txid_snapshot
 *
 *		Return current snapshot
 */
Datum
txid_current_snapshot(PG_FUNCTION_ARGS)
{
	TxidSnapshot *snap;
	unsigned nxip, i, size;
	TxidEpoch state;
	Snapshot cur;

	cur = SerializableSnapshot;
	if (cur == NULL)
		elog(ERROR, "get_current_snapshot: SerializableSnapshot == NULL");

	load_xid_epoch(&state);

	/* allocate */
	nxip = cur->xcnt;
	size = TXID_SNAPSHOT_SIZE(nxip);
	snap = palloc(size);
	SET_VARSIZE(snap, size);

	/* fill */
	snap->xmin = convert_xid(cur->xmin, &state);
	snap->xmax = convert_xid(cur->xmax, &state);
	snap->nxip = nxip;
	for (i = 0; i < nxip; i++)
		snap->xip[i] = convert_xid(cur->xip[i], &state);

	/* we want them guaranteed ascending order */
	sort_snapshot(snap);

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
	TxidSnapshot *snap;
	char	   *str = PG_GETARG_CSTRING(0);

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
	TxidSnapshot   *snap;
	StringInfoData	str;
	int				i;

	snap = (TxidSnapshot *) PG_GETARG_VARLENA_P(0);

	initStringInfo(&str);

	appendStringInfo(&str, TXID_FMT ":", snap->xmin);
	appendStringInfo(&str, TXID_FMT ":", snap->xmax);

	for (i = 0; i < snap->nxip; i++)
	{
		appendStringInfo(&str, "%s" TXID_FMT,
						 ((i > 0) ? "," : ""),
						 snap->xip[i]);
	}

	PG_FREE_IF_COPY(snap, 0);

	PG_RETURN_CSTRING(str.data);
}


/*
 * txid_visible_in_snapshot(int8, txid_snapshot) returns bool
 *
 *		is txid visible in snapshot ?
 */
Datum
txid_visible_in_snapshot(PG_FUNCTION_ARGS)
{
	txid value = PG_GETARG_INT64(0);
	TxidSnapshot *snap = (TxidSnapshot *) PG_GETARG_VARLENA_P(1);
	int			res;
	
	res = is_visible_txid(value, snap) ? true : false;

	PG_FREE_IF_COPY(snap, 1);
	PG_RETURN_BOOL(res);
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
	txid res = snap->xmin;
	PG_FREE_IF_COPY(snap, 0);
	PG_RETURN_INT64(res);
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
	txid res = snap->xmax;
	PG_FREE_IF_COPY(snap, 0);
	PG_RETURN_INT64(res);
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
	txid value;

	/* on first call initialize snap_state and get copy of snapshot */
	if (SRF_IS_FIRSTCALL()) {
		TxidSnapshot *arg;

		fctx = SRF_FIRSTCALL_INIT();

		/* make a copy of user snapshot */
		arg = (TxidSnapshot *) PG_GETARG_VARLENA_P(0);
		snap = MemoryContextAlloc(fctx->multi_call_memory_ctx, VARSIZE(arg));
		memcpy(snap, arg, VARSIZE(arg));
		PG_FREE_IF_COPY(arg, 0);

		fctx->user_fctx = snap;
	}

	/* return values one-by-one */
	fctx = SRF_PERCALL_SETUP();
	snap = fctx->user_fctx;
	if (fctx->call_cntr < snap->nxip) {
		value = snap->xip[fctx->call_cntr];
		SRF_RETURN_NEXT(fctx, Int64GetDatum(value));
	} else {
		SRF_RETURN_DONE(fctx);
	}
}

