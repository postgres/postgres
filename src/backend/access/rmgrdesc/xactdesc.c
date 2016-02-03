/*-------------------------------------------------------------------------
 *
 * xactdesc.c
 *	  rmgr descriptor routines for access/transam/xact.c
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
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
#include "catalog/catalog.h"
#include "storage/sinval.h"
#include "utils/timestamp.h"

/*
 * Parse the WAL format of an xact commit and abort records into an easier to
 * understand format.
 *
 * This routines are in xactdesc.c because they're accessed in backend (when
 * replaying WAL) and frontend (pg_xlogdump) code. This file is the only xact
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

	if (parsed->xinfo & XACT_XINFO_HAS_RELFILENODES)
	{
		xl_xact_relfilenodes *xl_relfilenodes = (xl_xact_relfilenodes *) data;

		parsed->nrels = xl_relfilenodes->nrels;
		parsed->xnodes = xl_relfilenodes->xnodes;

		data += MinSizeOfXactRelfilenodes;
		data += xl_relfilenodes->nrels * sizeof(RelFileNode);
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
	}

	if (parsed->xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		xl_xact_origin xl_origin;

		/* we're only guaranteed 4 byte alignment, so copy onto stack */
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

	if (parsed->xinfo & XACT_XINFO_HAS_SUBXACTS)
	{
		xl_xact_subxacts *xl_subxacts = (xl_xact_subxacts *) data;

		parsed->nsubxacts = xl_subxacts->nsubxacts;
		parsed->subxacts = xl_subxacts->subxacts;

		data += MinSizeOfXactSubxacts;
		data += parsed->nsubxacts * sizeof(TransactionId);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_RELFILENODES)
	{
		xl_xact_relfilenodes *xl_relfilenodes = (xl_xact_relfilenodes *) data;

		parsed->nrels = xl_relfilenodes->nrels;
		parsed->xnodes = xl_relfilenodes->xnodes;

		data += MinSizeOfXactRelfilenodes;
		data += xl_relfilenodes->nrels * sizeof(RelFileNode);
	}

	if (parsed->xinfo & XACT_XINFO_HAS_TWOPHASE)
	{
		xl_xact_twophase *xl_twophase = (xl_xact_twophase *) data;

		parsed->twophase_xid = xl_twophase->xid;

		data += sizeof(xl_xact_twophase);
	}
}

static void
xact_desc_commit(StringInfo buf, uint8 info, xl_xact_commit *xlrec, RepOriginId origin_id)
{
	xl_xact_parsed_commit parsed;
	int			i;

	ParseCommitRecord(info, xlrec, &parsed);

	/* If this is a prepared xact, show the xid of the original xact */
	if (TransactionIdIsValid(parsed.twophase_xid))
		appendStringInfo(buf, "%u: ", parsed.twophase_xid);

	appendStringInfoString(buf, timestamptz_to_str(xlrec->xact_time));

	if (parsed.nrels > 0)
	{
		appendStringInfoString(buf, "; rels:");
		for (i = 0; i < parsed.nrels; i++)
		{
			char	   *path = relpathperm(parsed.xnodes[i], MAIN_FORKNUM);

			appendStringInfo(buf, " %s", path);
			pfree(path);
		}
	}
	if (parsed.nsubxacts > 0)
	{
		appendStringInfoString(buf, "; subxacts:");
		for (i = 0; i < parsed.nsubxacts; i++)
			appendStringInfo(buf, " %u", parsed.subxacts[i]);
	}
	if (parsed.nmsgs > 0)
	{
		if (XactCompletionRelcacheInitFileInval(parsed.xinfo))
			appendStringInfo(buf, "; relcache init file inval dbid %u tsid %u",
							 parsed.dbId, parsed.tsId);

		appendStringInfoString(buf, "; inval msgs:");
		for (i = 0; i < parsed.nmsgs; i++)
		{
			SharedInvalidationMessage *msg = &parsed.msgs[i];

			if (msg->id >= 0)
				appendStringInfo(buf, " catcache %d", msg->id);
			else if (msg->id == SHAREDINVALCATALOG_ID)
				appendStringInfo(buf, " catalog %u", msg->cat.catId);
			else if (msg->id == SHAREDINVALRELCACHE_ID)
				appendStringInfo(buf, " relcache %u", msg->rc.relId);
			/* not expected, but print something anyway */
			else if (msg->id == SHAREDINVALSMGR_ID)
				appendStringInfoString(buf, " smgr");
			/* not expected, but print something anyway */
			else if (msg->id == SHAREDINVALRELMAP_ID)
				appendStringInfoString(buf, " relmap");
			else if (msg->id == SHAREDINVALSNAPSHOT_ID)
				appendStringInfo(buf, " snapshot %u", msg->sn.relId);
			else
				appendStringInfo(buf, " unknown id %d", msg->id);
		}
	}

	if (XactCompletionForceSyncCommit(parsed.xinfo))
		appendStringInfoString(buf, "; sync");

	if (parsed.xinfo & XACT_XINFO_HAS_ORIGIN)
	{
		appendStringInfo(buf, "; origin: node %u, lsn %X/%X, at %s",
						 origin_id,
						 (uint32) (parsed.origin_lsn >> 32),
						 (uint32) parsed.origin_lsn,
						 timestamptz_to_str(parsed.origin_timestamp));
	}
}

static void
xact_desc_abort(StringInfo buf, uint8 info, xl_xact_abort *xlrec)
{
	xl_xact_parsed_abort parsed;
	int			i;

	ParseAbortRecord(info, xlrec, &parsed);

	/* If this is a prepared xact, show the xid of the original xact */
	if (TransactionIdIsValid(parsed.twophase_xid))
		appendStringInfo(buf, "%u: ", parsed.twophase_xid);

	appendStringInfoString(buf, timestamptz_to_str(xlrec->xact_time));
	if (parsed.nrels > 0)
	{
		appendStringInfoString(buf, "; rels:");
		for (i = 0; i < parsed.nrels; i++)
		{
			char	   *path = relpathperm(parsed.xnodes[i], MAIN_FORKNUM);

			appendStringInfo(buf, " %s", path);
			pfree(path);
		}
	}

	if (parsed.nsubxacts > 0)
	{
		appendStringInfoString(buf, "; subxacts:");
		for (i = 0; i < parsed.nsubxacts; i++)
			appendStringInfo(buf, " %u", parsed.subxacts[i]);
	}
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

		xact_desc_abort(buf, XLogRecGetInfo(record), xlrec);
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
	}

	return id;
}
