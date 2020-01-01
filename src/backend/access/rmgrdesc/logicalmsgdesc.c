/*-------------------------------------------------------------------------
 *
 * logicalmsgdesc.c
 *	  rmgr descriptor routines for replication/logical/message.c
 *
 * Portions Copyright (c) 2015-2020, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/logicalmsgdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "replication/message.h"

void
logicalmsg_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_LOGICAL_MESSAGE)
	{
		xl_logical_message *xlrec = (xl_logical_message *) rec;

		appendStringInfo(buf, "%s message size %zu bytes",
						 xlrec->transactional ? "transactional" : "nontransactional",
						 xlrec->message_size);
	}
}

const char *
logicalmsg_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_LOGICAL_MESSAGE)
		return "MESSAGE";

	return NULL;
}
