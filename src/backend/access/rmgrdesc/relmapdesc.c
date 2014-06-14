/*-------------------------------------------------------------------------
 *
 * relmapdesc.c
 *	  rmgr descriptor routines for utils/cache/relmapper.c
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/relmapdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/relmapper.h"

void
relmap_desc(StringInfo buf, XLogRecord *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = record->xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_RELMAP_UPDATE)
	{
		xl_relmap_update *xlrec = (xl_relmap_update *) rec;

		appendStringInfo(buf, "update relmap: database %u tablespace %u size %u",
						 xlrec->dbid, xlrec->tsid, xlrec->nbytes);
	}
	else
		appendStringInfoString(buf, "UNKNOWN");
}
