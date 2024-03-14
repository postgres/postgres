/*-------------------------------------------------------------------------
 *
 * pg_tde_xlog.h
 *	  TDE XLog resource manager
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TDE_XLOG_H
#define PG_TDE_XLOG_H

#include "access/xlog_internal.h"

/* TDE XLOG resource manager */
#define XLOG_TDE_ADD_RELATION_KEY	0x00
#define XLOG_TDE_ADD_MASTER_KEY		0x10
#define XLOG_TDE_CLEAN_MASTER_KEY	0x20
/* TODO: ID has to be registedred and changed: https://wiki.postgresql.org/wiki/CustomWALResourceManagers */
#define RM_TDERMGR_ID	RM_EXPERIMENTAL_ID
#define RM_TDERMGR_NAME	"test_pg_tde_custom_rmgr"

extern void pg_tde_rmgr_redo(XLogReaderState *record);
extern void pg_tde_rmgr_desc(StringInfo buf, XLogReaderState *record);
extern const char *pg_tde_rmgr_identify(uint8 info);

static const RmgrData pg_tde_rmgr = {
	.rm_name = RM_TDERMGR_NAME,
	.rm_redo = pg_tde_rmgr_redo,
	.rm_desc = pg_tde_rmgr_desc,
	.rm_identify = pg_tde_rmgr_identify
};

#endif							/* PG_TDE_XLOG_H */
