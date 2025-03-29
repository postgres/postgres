/*-------------------------------------------------------------------------
 *
 * xactdesc.c
 *	  rmgr descriptor routines for access/transam/xact.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/xactdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "replication/origin.h"
#include "storage/sinval.h"
#include "storage/standbydefs.h"
#include "utils/timestamp.h"

/*
 * Parse the WAL format of an xact commit and abort records into an easier to
 * understand format.
 *
 * These routines are in xactdesc.c because they're accessed in backend (when
 * replaying WAL) and frontend (pg_waldump) code. This file is the only xact
 * specific one shared between both. They're complicated enough that
 * duplication would be bothersome.
 */

void
ParseCommitRecord(uint8 info, xl_xact_commit *xlrec, xl_xact_parsed_commit *parsed)
{
	char	   *data = ((char *) xlrec) + MinSizeOfXactCommit;

	memset(parsed, 0, sizeof(*parsed));

	parsed->xinfo = 0;			/* default, if no XLOG_XACT_HAS_INFO is
								 * present */

	parsed->xact_time = xlrec->xact_time;

	if (info & XLOG_XACT_HAS_INFO)
	{
		xl_xact_xinfo *xl_xinfo = (xl_xact_xinfo *) data;

		parsed->xinfo = xl_xinfo->xinfo;

		data += sizeof(xl_xact_xinfo);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_DBINFO)
	{
		xl_xact_dbinfo *xl_dbinfo = (xl_xact_dbinfo *) data;

		parsed->dbId = xl_dbinfo->dbId;
		parsed->tsId = xl_dbinfo->tsId;

		data += sizeof(xl_xact_dbinfo);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_SUBXACTS)
	{
		xl_xact_subxacts *xl_subxacts = (xl_xact_subxacts *) data;

		parsed->nsubxacts = xl_subxacts->nsubxacts;
		parsed->subxacts = xl_subxacts->subxacts;

		data += MinSizeOfXactSubxacts;
		data += parsed->nsubxacts * sizeof(TransactionId);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_RELFILELOCATORS)
	{
		xl_xact_relfilelocators *xl_rellocators = (xl_xact_relfilelocators *) data;

		parsed->nrels = xl_rellocators->nrels;
		parsed->xlocators = xl_rellocators->xlocators;

		data += MinSizeOfXactRelfileLocators;
		data += xl_rellocators->nrels * sizeof(RelFileLocator);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_DROPPED_STATS)
	{
		xl_xact_stats_items *xl_drops = (xl_xact_stats_items *) data;

		parsed->nstats = xl_drops->nitems;
		parsed->stats = xl_drops->items;

		data += MinSizeOfXactStatsItems;
		data += xl_drops->nitems * sizeof(xl_xact_stats_item);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_INVALS)
	{
		xl_xact_invals *xl_invals = (xl_xact_invals *) data;

		parsed->nmsgs = xl_invals->nmsgs;
		parsed->msgs = xl_invals->msgs;

		data += MinSizeOfXactInvals;
		data += xl_invals->nmsgs * sizeof(SharedInvalidationMessage);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_TWOPHASE)
	{
		xl_xact_twophase *xl_twophase = (xl_xact_twophase *) data;

		parsed->twophase_xid = xl_twophase->xid;

		data += sizeof(xl_xact_twophase);

		if (parsed->xinfo & XACT_XINFO_HAS_GID)
		{
			strlcpy(parsed->twophase_gid, data, sizeof(parsed->twophase_gid));
			data += strlen(data) + 1;
		}
	}

	/* Note: no alignment is guaranteed after this point */

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		xl_xact_origin xl_origin;

		/* no alignment is guaranteed, so copy onto stack */
		memcpy(&xl_origin, data, sizeof(xl_origin));

		parsed->origin_lsn = xl_origin.origin_lsn;
		parsed->origin_timestamp = xl_origin.origin_timestamp;

		data += sizeof(xl_xact_origin);
	}
}

void
ParseAbortRecord(uint8 info, xl_xact_abort *xlrec, xl_xact_parsed_abort *parsed)
{
	char	   *data = ((char *) xlrec) + MinSizeOfXactAbort;

	memset(parsed, 0, sizeof(*parsed));

	parsed->xinfo = 0;			/* default, if no XLOG_XACT_HAS_INFO is
								 * present */

	parsed->xact_time = xlrec->xact_time;

	if (info & XLOG_XACT_HAS_INFO)
	{
		xl_xact_xinfo *xl_xinfo = (xl_xact_xinfo *) data;

		parsed->xinfo = xl_xinfo->xinfo;

		data += sizeof(xl_xact_xinfo);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_DBINFO)
	{
		xl_xact_dbinfo *xl_dbinfo = (xl_xact_dbinfo *) data;

		parsed->dbId = xl_dbinfo->dbId;
		parsed->tsId = xl_dbinfo->tsId;

		data += sizeof(xl_xact_dbinfo);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_SUBXACTS)
	{
		xl_xact_subxacts *xl_subxacts = (xl_xact_subxacts *) data;

		parsed->nsubxacts = xl_subxacts->nsubxacts;
		parsed->subxacts = xl_subxacts->subxacts;

		data += MinSizeOfXactSubxacts;
		data += parsed->nsubxacts * sizeof(TransactionId);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_RELFILELOCATORS)
	{
		xl_xact_relfilelocators *xl_rellocator = (xl_xact_relfilelocators *) data;

		parsed->nrels = xl_rellocator->nrels;
		parsed->xlocators = xl_rellocator->xlocators;

		data += MinSizeOfXactRelfileLocators;
		data += xl_rellocator->nrels * sizeof(RelFileLocator);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_DROPPED_STATS)
	{
		xl_xact_stats_items *xl_drops = (xl_xact_stats_items *) data;

		parsed->nstats = xl_drops->nitems;
		parsed->stats = xl_drops->items;

		data += MinSizeOfXactStatsItems;
		data += xl_drops->nitems * sizeof(xl_xact_stats_item);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_TWOPHASE)
	{
		xl_xact_twophase *xl_twophase = (xl_xact_twophase *) data;

		parsed->twophase_xid = xl_twophase->xid;

		data += sizeof(xl_xact_twophase);

		if (parsed->xinfo & XACT_XINFO_HAS_GID)
		{
			strlcpy(parsed->twophase_gid, data, sizeof(parsed->twophase_gid));
			data += strlen(data) + 1;
		}
	}

	/* Note: no alignment is guaranteed after this point */

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		xl_xact_origin xl_origin;

		/* no alignment is guaranteed, so copy onto stack */
		memcpy(&xl_origin, data, sizeof(xl_origin));

		parsed->origin_lsn = xl_origin.origin_lsn;
		parsed->origin_timestamp = xl_origin.origin_timestamp;

		data += sizeof(xl_xact_origin);
	}
}

/*
 * ParsePrepareRecord
 */
void
ParsePrepareRecord(uint8 info, xl_xact_prepare *xlrec, xl_xact_parsed_prepare *parsed)
{
	char	   *bufptr;

	bufptr = ((char *) xlrec) + MAXALIGN(sizeof(xl_xact_prepare));

	memset(parsed, 0, sizeof(*parsed));

	parsed->xact_time = xlrec->prepared_at;
	parsed->origin_lsn = xlrec->origin_lsn;
	parsed->origin_timestamp = xlrec->origin_timestamp;
	parsed->twophase_xid = xlrec->xid;
	parsed->dbId = xlrec->database;
	parsed->nsubxacts = xlrec->nsubxacts;
	parsed->nrels = xlrec->ncommitrels;
	parsed->nabortrels = xlrec->nabortrels;
	parsed->nmsgs = xlrec->ninvalmsgs;

	strncpy(parsed->twophase_gid, bufptr, xlrec->gidlen);
	bufptr += MAXALIGN(xlrec->gidlen);

	parsed->subxacts = (TransactionId *) bufptr;
	bufptr += MAXALIGN(xlrec->nsubxacts * sizeof(TransactionId));

	parsed->xlocators = (RelFileLocator *) bufptr;
	bufptr += MAXALIGN(xlrec->ncommitrels * sizeof(RelFileLocator));

	parsed->abortlocators = (RelFileLocator *) bufptr;
	bufptr += MAXALIGN(xlrec->nabortrels * sizeof(RelFileLocator));

	parsed->stats = (xl_xact_stats_item *) bufptr;
	bufptr += MAXALIGN(xlrec->ncommitstats * sizeof(xl_xact_stats_item));

	parsed->abortstats = (xl_xact_stats_item *) bufptr;
	bufptr += MAXALIGN(xlrec->nabortstats * sizeof(xl_xact_stats_item));

	parsed->msgs = (SharedInvalidationMessage *) bufptr;
	bufptr += MAXALIGN(xlrec->ninvalmsgs * sizeof(SharedInvalidationMessage));
}

static void
xact_desc_relations(StringInfo buf, char *label, int nrels,
					RelFileLocator *xlocators)
{
	int			i;

	if (nrels > 0)
	{
		appendStringInfo(buf, "; %s:", label);
		for (i = 0; i < nrels; i++)
		{
			appendStringInfo(buf, " %s",
							 relpathperm(xlocators[i], MAIN_FORKNUM).str);
		}
	}
}

static void
xact_desc_subxacts(StringInfo buf, int nsubxacts, TransactionId *subxacts)
{
	int			i;

	if (nsubxacts > 0)
	{
		appendStringInfoString(buf, "; subxacts:");
		for (i = 0; i < nsubxacts; i++)
			appendStringInfo(buf, " %u", subxacts[i]);
	}
}

static void
xact_desc_stats(StringInfo buf, const char *label,
				int ndropped, xl_xact_stats_item *dropped_stats)
{
	int			i;

	if (ndropped > 0)
	{
		appendStringInfo(buf, "; %sdropped stats:", label);
		for (i = 0; i < ndropped; i++)
		{
			uint64		objid =
				((uint64) dropped_stats[i].objid_hi) << 32 | dropped_stats[i].objid_lo;

			appendStringInfo(buf, " %d/%u/%" PRIu64,
							 dropped_stats[i].kind,
							 dropped_stats[i].dboid,
							 objid);
		}
	}
}

static void
xact_desc_commit(StringInfo buf, uint8 info, xl_xact_commit *xlrec, RepOriginId origin_id)
{
	xl_xact_parsed_commit parsed;

	ParseCommitRecord(info, xlrec, &parsed);

	/* If this is a prepared xact, show the xid of the original xact */
	if (TransactionIdIsValid(parsed.twophase_xid))
		appendStringInfo(buf, "%u: ", parsed.twophase_xid);

	appendStringInfoString(buf, timestamptz_to_str(xlrec->xact_time));

	xact_desc_relations(buf, "rels", parsed.nrels, parsed.xlocators);
	xact_desc_subxacts(buf, parsed.nsubxacts, parsed.subxacts);
	xact_desc_stats(buf, "", parsed.nstats, parsed.stats);

	standby_desc_invalidations(buf, parsed.nmsgs, parsed.msgs, parsed.dbId,
							   parsed.tsId,
							   XactCompletionRelcacheInitFileInval(parsed.xinfo));

	if (XactCompletionApplyFeedback(parsed.xinfo))
		appendStringInfoString(buf, "; apply_feedback");

	if (XactCompletionForceSyncCommit(parsed.xinfo))
		appendStringInfoString(buf, "; sync");

	if (parsed.xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		appendStringInfo(buf, "; origin: node %u, lsn %X/%X, at %s",
						 origin_id,
						 LSN_FORMAT_ARGS(parsed.origin_lsn),
						 timestamptz_to_str(parsed.origin_timestamp));
	}
}

static void
xact_desc_abort(StringInfo buf, uint8 info, xl_xact_abort *xlrec, RepOriginId origin_id)
{
	xl_xact_parsed_abort parsed;

	ParseAbortRecord(info, xlrec, &parsed);

	/* If this is a prepared xact, show the xid of the original xact */
	if (TransactionIdIsValid(parsed.twophase_xid))
		appendStringInfo(buf, "%u: ", parsed.twophase_xid);

	appendStringInfoString(buf, timestamptz_to_str(xlrec->xact_time));

	xact_desc_relations(buf, "rels", parsed.nrels, parsed.xlocators);
	xact_desc_subxacts(buf, parsed.nsubxacts, parsed.subxacts);

	if (parsed.xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		appendStringInfo(buf, "; origin: node %u, lsn %X/%X, at %s",
						 origin_id,
						 LSN_FORMAT_ARGS(parsed.origin_lsn),
						 timestamptz_to_str(parsed.origin_timestamp));
	}

	xact_desc_stats(buf, "", parsed.nstats, parsed.stats);
}

static void
xact_desc_prepare(StringInfo buf, uint8 info, xl_xact_prepare *xlrec, RepOriginId origin_id)
{
	xl_xact_parsed_prepare parsed;

	ParsePrepareRecord(info, xlrec, &parsed);

	appendStringInfo(buf, "gid %s: ", parsed.twophase_gid);
	appendStringInfoString(buf, timestamptz_to_str(parsed.xact_time));

	xact_desc_relations(buf, "rels(commit)", parsed.nrels, parsed.xlocators);
	xact_desc_relations(buf, "rels(abort)", parsed.nabortrels,
						parsed.abortlocators);
	xact_desc_stats(buf, "commit ", parsed.nstats, parsed.stats);
	xact_desc_stats(buf, "abort ", parsed.nabortstats, parsed.abortstats);
	xact_desc_subxacts(buf, parsed.nsubxacts, parsed.subxacts);

	standby_desc_invalidations(buf, parsed.nmsgs, parsed.msgs, parsed.dbId,
							   parsed.tsId, xlrec->initfileinval);

	/*
	 * Check if the replication origin has been set in this record in the same
	 * way as PrepareRedoAdd().
	 */
	if (origin_id != InvalidRepOriginId)
		appendStringInfo(buf, "; origin: node %u, lsn %X/%X, at %s",
						 origin_id,
						 LSN_FORMAT_ARGS(parsed.origin_lsn),
						 timestamptz_to_str(parsed.origin_timestamp));
}

static void
xact_desc_assignment(StringInfo buf, xl_xact_assignment *xlrec)
{
	int			i;

	appendStringInfoString(buf, "subxacts:");

	for (i = 0; i < xlrec->nsubxacts; i++)
		appendStringInfo(buf, " %u", xlrec->xsub[i]);
}

void
xact_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & XLOG_XACT_OPMASK;

	if (info == XLOG_XACT_COMMIT || info == XLOG_XACT_COMMIT_PREPARED)
	{
		xl_xact_commit *xlrec = (xl_xact_commit *) rec;

		xact_desc_commit(buf, XLogRecGetInfo(record), xlrec,
						 XLogRecGetOrigin(record));
	}
	else if (info == XLOG_XACT_ABORT || info == XLOG_XACT_ABORT_PREPARED)
	{
		xl_xact_abort *xlrec = (xl_xact_abort *) rec;

		xact_desc_abort(buf, XLogRecGetInfo(record), xlrec,
						XLogRecGetOrigin(record));
	}
	else if (info == XLOG_XACT_PREPARE)
	{
		xl_xact_prepare *xlrec = (xl_xact_prepare *) rec;

		xact_desc_prepare(buf, XLogRecGetInfo(record), xlrec,
						  XLogRecGetOrigin(record));
	}
	else if (info == XLOG_XACT_ASSIGNMENT)
	{
		xl_xact_assignment *xlrec = (xl_xact_assignment *) rec;

		/*
		 * Note that we ignore the WAL record's xid, since we're more
		 * interested in the top-level xid that issued the record and which
		 * xids are being reported here.
		 */
		appendStringInfo(buf, "xtop %u: ", xlrec->xtop);
		xact_desc_assignment(buf, xlrec);
	}
	else if (info == XLOG_XACT_INVALIDATIONS)
	{
		xl_xact_invals *xlrec = (xl_xact_invals *) rec;

		standby_desc_invalidations(buf, xlrec->nmsgs, xlrec->msgs, InvalidOid,
								   InvalidOid, false);
	}
}

const char *
xact_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & XLOG_XACT_OPMASK)
	{
		case XLOG_XACT_COMMIT:
			id = "COMMIT";
			break;
		case XLOG_XACT_PREPARE:
			id = "PREPARE";
			break;
		case XLOG_XACT_ABORT:
			id = "ABORT";
			break;
		case XLOG_XACT_COMMIT_PREPARED:
			id = "COMMIT_PREPARED";
			break;
		case XLOG_XACT_ABORT_PREPARED:
			id = "ABORT_PREPARED";
			break;
		case XLOG_XACT_ASSIGNMENT:
			id = "ASSIGNMENT";
			break;
		case XLOG_XACT_INVALIDATIONS:
			id = "INVALIDATION";
			break;
	}

	return id;
}
