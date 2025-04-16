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
#include "catalog/tde_keyring.h"
#include "storage/bufmgr.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include "access/pg_tde_xlog.h"
#include "encryption/enc_tde.h"

static void tdeheap_rmgr_redo(XLogReaderState *record);
static void tdeheap_rmgr_desc(StringInfo buf, XLogReaderState *record);
static const char *tdeheap_rmgr_identify(uint8 info);

static const RmgrData tdeheap_rmgr = {
	.rm_name = "pg_tde",
	.rm_redo = tdeheap_rmgr_redo,
	.rm_desc = tdeheap_rmgr_desc,
	.rm_identify = tdeheap_rmgr_identify,
};

void
RegisterTdeRmgr(void)
{
	RegisterCustomRmgr(RM_TDERMGR_ID, &tdeheap_rmgr);
}

static void
tdeheap_rmgr_redo(XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_TDE_ADD_RELATION_KEY)
	{
		XLogRelKey *xlrec = (XLogRelKey *) XLogRecGetData(record);

		pg_tde_create_smgr_key_perm_redo(&xlrec->rlocator);
	}
	else if (info == XLOG_TDE_ADD_PRINCIPAL_KEY)
	{
		TDESignedPrincipalKeyInfo *mkey = (TDESignedPrincipalKeyInfo *) XLogRecGetData(record);

		pg_tde_save_principal_key_redo(mkey);
	}
	else if (info == XLOG_TDE_ROTATE_PRINCIPAL_KEY)
	{
		XLogPrincipalKeyRotate *xlrec = (XLogPrincipalKeyRotate *) XLogRecGetData(record);

		xl_tde_perform_rotate_key(xlrec);
	}
	else if (info == XLOG_TDE_WRITE_KEY_PROVIDER)
	{
		KeyringProviderRecordInFile *xlrec = (KeyringProviderRecordInFile *) XLogRecGetData(record);

		redo_key_provider_info(xlrec);
	}
	else if (info == XLOG_TDE_INSTALL_EXTENSION)
	{
		XLogExtensionInstall *xlrec = (XLogExtensionInstall *) XLogRecGetData(record);

		extension_install_redo(xlrec);
	}
	else
	{
		elog(PANIC, "pg_tde_redo: unknown op code %u", info);
	}
}

static void
tdeheap_rmgr_desc(StringInfo buf, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_TDE_ADD_RELATION_KEY)
	{
		XLogRelKey *xlrec = (XLogRelKey *) XLogRecGetData(record);

		appendStringInfo(buf, "rel: %u/%u/%u", xlrec->rlocator.spcOid, xlrec->rlocator.dbOid, xlrec->rlocator.relNumber);
	}
	else if (info == XLOG_TDE_ADD_PRINCIPAL_KEY)
	{
		TDEPrincipalKeyInfo *xlrec = (TDEPrincipalKeyInfo *) XLogRecGetData(record);

		appendStringInfo(buf, "db: %u", xlrec->databaseId);
	}
	else if (info == XLOG_TDE_ROTATE_PRINCIPAL_KEY)
	{
		XLogPrincipalKeyRotate *xlrec = (XLogPrincipalKeyRotate *) XLogRecGetData(record);

		appendStringInfo(buf, "db: %u", xlrec->databaseId);
	}
	else if (info == XLOG_TDE_WRITE_KEY_PROVIDER)
	{
		KeyringProviderRecordInFile *xlrec = (KeyringProviderRecordInFile *) XLogRecGetData(record);

		appendStringInfo(buf, "db: %u, provider id: %d", xlrec->database_id, xlrec->provider.provider_id);
	}
	else if (info == XLOG_TDE_INSTALL_EXTENSION)
	{
		XLogExtensionInstall *xlrec = (XLogExtensionInstall *) XLogRecGetData(record);

		appendStringInfo(buf, "db: %u", xlrec->database_id);
	}
}

static const char *
tdeheap_rmgr_identify(uint8 info)
{
	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_TDE_ADD_RELATION_KEY:
			return "ADD_RELATION_KEY";
		case XLOG_TDE_ADD_PRINCIPAL_KEY:
			return "ADD_PRINCIPAL_KEY";
		case XLOG_TDE_ROTATE_PRINCIPAL_KEY:
			return "ROTATE_PRINCIPAL_KEY";
		case XLOG_TDE_WRITE_KEY_PROVIDER:
			return "WRITE_KEY_PROVIDER";
		case XLOG_TDE_INSTALL_EXTENSION:
			return "INSTALL_EXTENSION";
		default:
			return NULL;
	}
}
