/*-------------------------------------------------------------------------
 *
 * dbasedesc.c
 *	  rmgr descriptor routines for commands/dbcommands.c
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/dbasedesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/dbcommands.h"
#include "lib/stringinfo.h"


void
dbase_desc(StringInfo buf, uint8 xl_info, char *rec)
{
	uint8		info = xl_info & ~XLR_INFO_MASK;

	if (info == XLOG_DBASE_CREATE)
	{
		xl_dbase_create_rec *xlrec = (xl_dbase_create_rec *) rec;

		appendStringInfo(buf, "create db: copy dir %u/%u to %u/%u",
						 xlrec->src_db_id, xlrec->src_tablespace_id,
						 xlrec->db_id, xlrec->tablespace_id);
	}
	else if (info == XLOG_DBASE_DROP)
	{
		xl_dbase_drop_rec *xlrec = (xl_dbase_drop_rec *) rec;

		appendStringInfo(buf, "drop db: dir %u/%u",
						 xlrec->db_id, xlrec->tablespace_id);
	}
	else
		appendStringInfoString(buf, "UNKNOWN");
}
