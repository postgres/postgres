/*-------------------------------------------------------------------------
 *
 * tblspcdesc.c
 *	  rmgr descriptor routines for commands/tablespace.c
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/tblspcdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/tablespace.h"


void
tblspc_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_TBLSPC_CREATE)
	{
		xl_tblspc_create_rec *xlrec = (xl_tblspc_create_rec *) rec;

		appendStringInfo(buf, "%u \"%s\"", xlrec->ts_id, xlrec->ts_path);
	}
	else if (info == XLOG_TBLSPC_DROP)
	{
		xl_tblspc_drop_rec *xlrec = (xl_tblspc_drop_rec *) rec;

		appendStringInfo(buf, "%u", xlrec->ts_id);
	}
}

const char *
tblspc_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_TBLSPC_CREATE:
			id = "CREATE";
			break;
		case XLOG_TBLSPC_DROP:
			id = "DROP";
			break;
	}

	return id;
}
