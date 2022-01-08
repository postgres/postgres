/*-------------------------------------------------------------------------
 *
 * basebackup_copy.c
 *	  send basebackup archives using one COPY OUT operation per
 *	  tablespace, and an additional COPY OUT for the backup manifest
 *
 * Portions Copyright (c) 2010-2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/replication/basebackup_copy.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "replication/basebackup.h"
#include "replication/basebackup_sink.h"

static void bbsink_copytblspc_begin_backup(bbsink *sink);
static void bbsink_copytblspc_begin_archive(bbsink *sink,
											const char *archive_name);
static void bbsink_copytblspc_archive_contents(bbsink *sink, size_t len);
static void bbsink_copytblspc_end_archive(bbsink *sink);
static void bbsink_copytblspc_begin_manifest(bbsink *sink);
static void bbsink_copytblspc_manifest_contents(bbsink *sink, size_t len);
static void bbsink_copytblspc_end_manifest(bbsink *sink);
static void bbsink_copytblspc_end_backup(bbsink *sink, XLogRecPtr endptr,
										 TimeLineID endtli);
static void bbsink_copytblspc_cleanup(bbsink *sink);

static void SendCopyOutResponse(void);
static void SendCopyData(const char *data, size_t len);
static void SendCopyDone(void);
static void SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli);
static void SendTablespaceList(List *tablespaces);
static void send_int8_string(StringInfoData *buf, int64 intval);

const bbsink_ops bbsink_copytblspc_ops = {
	.begin_backup = bbsink_copytblspc_begin_backup,
	.begin_archive = bbsink_copytblspc_begin_archive,
	.archive_contents = bbsink_copytblspc_archive_contents,
	.end_archive = bbsink_copytblspc_end_archive,
	.begin_manifest = bbsink_copytblspc_begin_manifest,
	.manifest_contents = bbsink_copytblspc_manifest_contents,
	.end_manifest = bbsink_copytblspc_end_manifest,
	.end_backup = bbsink_copytblspc_end_backup,
	.cleanup = bbsink_copytblspc_cleanup
};

/*
 * Create a new 'copytblspc' bbsink.
 */
bbsink *
bbsink_copytblspc_new(void)
{
	bbsink	   *sink = palloc0(sizeof(bbsink));

	*((const bbsink_ops **) &sink->bbs_ops) = &bbsink_copytblspc_ops;

	return sink;
}

/*
 * Begin backup.
 */
static void
bbsink_copytblspc_begin_backup(bbsink *sink)
{
	bbsink_state *state = sink->bbs_state;

	/* Create a suitable buffer. */
	sink->bbs_buffer = palloc(sink->bbs_buffer_length);

	/* Tell client the backup start location. */
	SendXlogRecPtrResult(state->startptr, state->starttli);

	/* Send client a list of tablespaces. */
	SendTablespaceList(state->tablespaces);

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");
}

/*
 * Each archive is set as a separate stream of COPY data, and thus begins
 * with a CopyOutResponse message.
 */
static void
bbsink_copytblspc_begin_archive(bbsink *sink, const char *archive_name)
{
	SendCopyOutResponse();
}

/*
 * Each chunk of data within the archive is sent as a CopyData message.
 */
static void
bbsink_copytblspc_archive_contents(bbsink *sink, size_t len)
{
	SendCopyData(sink->bbs_buffer, len);
}

/*
 * The archive is terminated by a CopyDone message.
 */
static void
bbsink_copytblspc_end_archive(bbsink *sink)
{
	SendCopyDone();
}

/*
 * The backup manifest is sent as a separate stream of COPY data, and thus
 * begins with a CopyOutResponse message.
 */
static void
bbsink_copytblspc_begin_manifest(bbsink *sink)
{
	SendCopyOutResponse();
}

/*
 * Each chunk of manifest data is sent using a CopyData message.
 */
static void
bbsink_copytblspc_manifest_contents(bbsink *sink, size_t len)
{
	SendCopyData(sink->bbs_buffer, len);
}

/*
 * When we've finished sending the manifest, send a CopyDone message.
 */
static void
bbsink_copytblspc_end_manifest(bbsink *sink)
{
	SendCopyDone();
}

/*
 * Send end-of-backup wire protocol messages.
 */
static void
bbsink_copytblspc_end_backup(bbsink *sink, XLogRecPtr endptr,
							 TimeLineID endtli)
{
	SendXlogRecPtrResult(endptr, endtli);
}

/*
 * Cleanup.
 */
static void
bbsink_copytblspc_cleanup(bbsink *sink)
{
	/* Nothing to do. */
}

/*
 * Send a CopyOutResponse message.
 */
static void
SendCopyOutResponse(void)
{
	StringInfoData buf;

	pq_beginmessage(&buf, 'H');
	pq_sendbyte(&buf, 0);		/* overall format */
	pq_sendint16(&buf, 0);		/* natts */
	pq_endmessage(&buf);
}

/*
 * Send a CopyData message.
 */
static void
SendCopyData(const char *data, size_t len)
{
	pq_putmessage('d', data, len);
}

/*
 * Send a CopyDone message.
 */
static void
SendCopyDone(void)
{
	pq_putemptymessage('c');
}

/*
 * Send a single resultset containing just a single
 * XLogRecPtr record (in text format)
 */
static void
SendXlogRecPtrResult(XLogRecPtr ptr, TimeLineID tli)
{
	StringInfoData buf;
	char		str[MAXFNAMELEN];
	Size		len;

	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint16(&buf, 2);		/* 2 fields */

	/* Field headers */
	pq_sendstring(&buf, "recptr");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */
	pq_sendint32(&buf, TEXTOID);	/* type oid */
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);

	pq_sendstring(&buf, "tli");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */

	/*
	 * int8 may seem like a surprising data type for this, but in theory int4
	 * would not be wide enough for this, as TimeLineID is unsigned.
	 */
	pq_sendint32(&buf, INT8OID);	/* type oid */
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);

	/* Data row */
	pq_beginmessage(&buf, 'D');
	pq_sendint16(&buf, 2);		/* number of columns */

	len = snprintf(str, sizeof(str),
				   "%X/%X", LSN_FORMAT_ARGS(ptr));
	pq_sendint32(&buf, len);
	pq_sendbytes(&buf, str, len);

	len = snprintf(str, sizeof(str), "%u", tli);
	pq_sendint32(&buf, len);
	pq_sendbytes(&buf, str, len);

	pq_endmessage(&buf);

	/* Send a CommandComplete message */
	pq_puttextmessage('C', "SELECT");
}

/*
 * Send a result set via libpq describing the tablespace list.
 */
static void
SendTablespaceList(List *tablespaces)
{
	StringInfoData buf;
	ListCell   *lc;

	/* Construct and send the directory information */
	pq_beginmessage(&buf, 'T'); /* RowDescription */
	pq_sendint16(&buf, 3);		/* 3 fields */

	/* First field - spcoid */
	pq_sendstring(&buf, "spcoid");
	pq_sendint32(&buf, 0);		/* table oid */
	pq_sendint16(&buf, 0);		/* attnum */
	pq_sendint32(&buf, OIDOID); /* type oid */
	pq_sendint16(&buf, 4);		/* typlen */
	pq_sendint32(&buf, 0);		/* typmod */
	pq_sendint16(&buf, 0);		/* format code */

	/* Second field - spclocation */
	pq_sendstring(&buf, "spclocation");
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_sendint32(&buf, TEXTOID);
	pq_sendint16(&buf, -1);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);

	/* Third field - size */
	pq_sendstring(&buf, "size");
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_sendint32(&buf, INT8OID);
	pq_sendint16(&buf, 8);
	pq_sendint32(&buf, 0);
	pq_sendint16(&buf, 0);
	pq_endmessage(&buf);

	foreach(lc, tablespaces)
	{
		tablespaceinfo *ti = lfirst(lc);

		/* Send one datarow message */
		pq_beginmessage(&buf, 'D');
		pq_sendint16(&buf, 3);	/* number of columns */
		if (ti->path == NULL)
		{
			pq_sendint32(&buf, -1); /* Length = -1 ==> NULL */
			pq_sendint32(&buf, -1);
		}
		else
		{
			Size		len;

			len = strlen(ti->oid);
			pq_sendint32(&buf, len);
			pq_sendbytes(&buf, ti->oid, len);

			len = strlen(ti->path);
			pq_sendint32(&buf, len);
			pq_sendbytes(&buf, ti->path, len);
		}
		if (ti->size >= 0)
			send_int8_string(&buf, ti->size / 1024);
		else
			pq_sendint32(&buf, -1); /* NULL */

		pq_endmessage(&buf);
	}
}

/*
 * Send a 64-bit integer as a string via the wire protocol.
 */
static void
send_int8_string(StringInfoData *buf, int64 intval)
{
	char		is[32];

	sprintf(is, INT64_FORMAT, intval);
	pq_sendint32(buf, strlen(is));
	pq_sendbytes(buf, is, strlen(is));
}
