/*-------------------------------------------------------------------------
 *
 * tdeheap_xlog.c
 *	  TDE XLog resource manager
 *
 *
 * IDENTIFICATION
 *	  src/access/pg_tde_xlog.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "pg_tde.h"
#include "pg_tde_defines.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xloginsert.h"
#include "catalog/pg_tablespace_d.h"
#include "catalog/tde_keyring.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "access/pg_tde_xlog.h"
#include "encryption/enc_tde.h"

/*
 * TDE fork XLog
 */
void
tdeheap_rmgr_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_TDE_ADD_RELATION_KEY)
	{
		TDEPrincipalKeyInfo *pk = NULL;
		XLogRelKey *xlrec = (XLogRelKey *) XLogRecGetData(record);

		if (xlrec->pkInfo.databaseId != 0)
			pk = &xlrec->pkInfo;

		LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
		pg_tde_write_key_map_entry(&xlrec->rlocator, xlrec->entry_type, &xlrec->relKey, pk);
		LWLockRelease(tde_lwlock_enc_keys());
	}
	else if (info == XLOG_TDE_ADD_PRINCIPAL_KEY)
	{
		TDEPrincipalKeyInfo *mkey = (TDEPrincipalKeyInfo *) XLogRecGetData(record);

		LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
		save_principal_key_info(mkey);
		LWLockRelease(tde_lwlock_enc_keys());
	}
	else if (info == XLOG_TDE_EXTENSION_INSTALL_KEY)
	{
		XLogExtensionInstall *xlrec = (XLogExtensionInstall *)XLogRecGetData(record);

		extension_install_redo(xlrec);
	}

	else if (info == XLOG_TDE_ADD_KEY_PROVIDER_KEY)
	{
		KeyringProviderXLRecord *xlrec = (KeyringProviderXLRecord *)XLogRecGetData(record);
		redo_key_provider_info(xlrec);
	}

	else if (info == XLOG_TDE_ROTATE_KEY)
	{
		XLogPrincipalKeyRotate *xlrec = (XLogPrincipalKeyRotate *) XLogRecGetData(record);

		LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
		xl_tde_perform_rotate_key(xlrec);
		LWLockRelease(tde_lwlock_enc_keys());
	}

	else if (info == XLOG_TDE_FREE_MAP_ENTRY)
	{
		off_t		offset = 0;
		RelFileLocator *xlrec = (RelFileLocator *) XLogRecGetData(record);

		LWLockAcquire(tde_lwlock_enc_keys(), LW_EXCLUSIVE);
		pg_tde_free_key_map_entry(xlrec, offset);
		LWLockRelease(tde_lwlock_enc_keys());
	}
	else
	{
		elog(PANIC, "pg_tde_redo: unknown op code %u", info);
	}
}

void
tdeheap_rmgr_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_TDE_ADD_RELATION_KEY)
	{
		XLogRelKey *xlrec = (XLogRelKey *) XLogRecGetData(record);

		appendStringInfo(buf, "add tde internal key for relation %u/%u", xlrec->rlocator.dbOid, xlrec->rlocator.relNumber);
	}
	if (info == XLOG_TDE_ADD_PRINCIPAL_KEY)
	{
		TDEPrincipalKeyInfo *xlrec = (TDEPrincipalKeyInfo *) XLogRecGetData(record);

		appendStringInfo(buf, "add tde principal key for db %u/%u", xlrec->databaseId, xlrec->tablespaceId);
	}
	if (info == XLOG_TDE_EXTENSION_INSTALL_KEY)
	{
		XLogExtensionInstall *xlrec = (XLogExtensionInstall *)XLogRecGetData(record);

		appendStringInfo(buf, "tde extension install for db %u/%u", xlrec->database_id, xlrec->tablespace_id);
	}
	if (info == XLOG_TDE_ROTATE_KEY)
	{
		XLogPrincipalKeyRotate *xlrec = (XLogPrincipalKeyRotate *) XLogRecGetData(record);

		appendStringInfo(buf, "rotate principal key for %u", xlrec->databaseId);
	}
	if (info == XLOG_TDE_ADD_KEY_PROVIDER_KEY)
	{
		KeyringProviderXLRecord *xlrec = (KeyringProviderXLRecord *)XLogRecGetData(record);

		appendStringInfo(buf, "add key provider %s for %u", xlrec->provider.provider_name, xlrec->database_id);
	}
}

const char *
tdeheap_rmgr_identify(uint8 info)
{
	if ((info & ~XLR_INFO_MASK) == XLOG_TDE_ADD_RELATION_KEY)
		return "XLOG_TDE_ADD_RELATION_KEY";

	if ((info & ~XLR_INFO_MASK) == XLOG_TDE_ADD_PRINCIPAL_KEY)
		return "XLOG_TDE_ADD_PRINCIPAL_KEY";

	if ((info & ~XLR_INFO_MASK) == XLOG_TDE_EXTENSION_INSTALL_KEY)
		return "XLOG_TDE_EXTENSION_INSTALL_KEY";

	return NULL;
}
