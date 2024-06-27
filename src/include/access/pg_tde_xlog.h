/*-------------------------------------------------------------------------
 *
 * pg_tde_xlog.h
 *	  TDE XLog resource manager
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TDE_XLOG_H
#define PG_TDE_XLOG_H

#include "postgres.h"
#include "access/xlog.h"
#include "access/xlog_internal.h"
#ifdef PERCONA_FORK
#include "access/xlog_smgr.h"
#endif

/* TDE XLOG resource manager */
#define XLOG_TDE_ADD_RELATION_KEY	0x00
#define XLOG_TDE_ADD_PRINCIPAL_KEY		0x10
#define XLOG_TDE_CLEAN_PRINCIPAL_KEY	0x20
#define XLOG_TDE_ROTATE_KEY			0x30

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

#ifdef PERCONA_FORK

/* XLog encryption staff */

extern Size TDEXLogEncryptBuffSize(void);

#define XLOG_TDE_ENC_BUFF_ALIGNED_SIZE	add_size(TDEXLogEncryptBuffSize(), PG_IO_ALIGN_SIZE)

extern void TDEXLogShmemInit(void);

extern ssize_t pg_tde_xlog_seg_read(int fd, void *buf, size_t count, off_t offset);
extern ssize_t pg_tde_xlog_seg_write(int fd, const void *buf, size_t count, off_t offset);

static const XLogSmgr tde_xlog_smgr = {
	.seg_read = pg_tde_xlog_seg_read,
	.seg_write = pg_tde_xlog_seg_write,
};

extern void TDEXLogSmgrInit(void);

extern void XLogInitGUC(void);

#endif

#endif							/* PG_TDE_XLOG_H */
