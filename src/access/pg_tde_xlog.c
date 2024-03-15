/*-------------------------------------------------------------------------
 *
 * pg_tde_xlog.c
 *	  TDE XLog resource manager
 *
 *
 * IDENTIFICATION
 *	  src/access/pg_tde_xlog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"

#include "access/pg_tde_tdemap.h"
#include "access/pg_tde_xlog.h"
#include "catalog/tde_master_key.h"

/*
 * TDE fork XLog
 */
void
pg_tde_rmgr_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_TDE_ADD_RELATION_KEY)
	{
		XLogRelKey *xlrec = (XLogRelKey *) XLogRecGetData(record);

		pg_tde_write_key_map_entry(&xlrec->rlocator, &xlrec->relKey, NULL);
	}
	else if (info == XLOG_TDE_ADD_MASTER_KEY)
	{
		TDEMasterKeyInfo *mkey = (TDEMasterKeyInfo *) XLogRecGetData(record);

		save_master_key_info(mkey);
	}
	else if (info == XLOG_TDE_CLEAN_MASTER_KEY)
	{
		XLogMasterKeyCleanup *xlrec = (XLogMasterKeyCleanup *) XLogRecGetData(record);

		cleanup_master_key_info(xlrec->databaseId, xlrec->tablespaceId);
	}
	else
	{
		elog(PANIC, "pg_tde_redo: unknown op code %u", info);
	}
}

void
pg_tde_rmgr_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_TDE_ADD_RELATION_KEY)
	{
		XLogRelKey *xlrec = (XLogRelKey *) XLogRecGetData(record);

		appendStringInfo(buf, "add tde internal key for relation %u/%u", xlrec->rlocator.dbOid, xlrec->rlocator.relNumber);
	}
	if (info == XLOG_TDE_ADD_MASTER_KEY)
	{
		TDEMasterKeyInfo *xlrec = (TDEMasterKeyInfo *) XLogRecGetData(record);

		appendStringInfo(buf, "add tde master key for db %u/%u", xlrec->databaseId, xlrec->tablespaceId);
	}
	if (info == XLOG_TDE_CLEAN_MASTER_KEY)
	{
		XLogMasterKeyCleanup *xlrec = (XLogMasterKeyCleanup *) XLogRecGetData(record);

		appendStringInfo(buf, "cleanup tde master key info for db %u/%u", xlrec->databaseId, xlrec->tablespaceId);
	}
}

const char *
pg_tde_rmgr_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_TDE_ADD_RELATION_KEY)
		return "XLOG_TDE_ADD_RELATION_KEY";

	if ((info & ~XLR_INFO_MASK) == XLOG_TDE_ADD_MASTER_KEY)
		return "XLOG_TDE_ADD_MASTER_KEY";

	if ((info & ~XLR_INFO_MASK) == XLOG_TDE_CLEAN_MASTER_KEY)
		return "XLOG_TDE_CLEAN_MASTER_KEY";

	return NULL;
}
