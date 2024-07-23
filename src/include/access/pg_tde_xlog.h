/*-------------------------------------------------------------------------
 *
 * tdeheap_xlog.h
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
#define XLOG_TDE_ADD_RELATION_KEY		0x00
#define XLOG_TDE_ADD_PRINCIPAL_KEY		0x10
#define XLOG_TDE_EXTENSION_INSTALL_KEY	0x20
#define XLOG_TDE_ROTATE_KEY				0x30
#define XLOG_TDE_ADD_KEY_PROVIDER_KEY 	0x40

/* TODO: ID has to be registedred and changed: https://wiki.postgresql.org/wiki/CustomWALResourceManagers */
#define RM_TDERMGR_ID	RM_EXPERIMENTAL_ID
#define RM_TDERMGR_NAME	"test_tdeheap_custom_rmgr"

extern void tdeheap_rmgr_redo(XLogReaderState *record);
extern void tdeheap_rmgr_desc(StringInfo buf, XLogReaderState *record);
extern const char *tdeheap_rmgr_identify(uint8 info);

static const RmgrData tdeheap_rmgr = {
	.rm_name = RM_TDERMGR_NAME,
	.rm_redo = tdeheap_rmgr_redo,
	.rm_desc = tdeheap_rmgr_desc,
	.rm_identify = tdeheap_rmgr_identify
};

#ifdef PERCONA_FORK

/* XLog encryption staff */

extern Size TDEXLogEncryptBuffSize(void);

#define XLOG_TDE_ENC_BUFF_ALIGNED_SIZE	add_size(TDEXLogEncryptBuffSize(), PG_IO_ALIGN_SIZE)

extern void TDEXLogShmemInit(void);

extern ssize_t tdeheap_xlog_seg_read(int fd, void *buf, size_t count, off_t offset);
extern ssize_t tdeheap_xlog_seg_write(int fd, const void *buf, size_t count, off_t offset);

static const XLogSmgr tde_xlog_smgr = {
	.seg_read = tdeheap_xlog_seg_read,
	.seg_write = tdeheap_xlog_seg_write,
};

extern void TDEXLogSmgrInit(void);

extern void XLogInitGUC(void);

#endif

#endif							/* PG_TDE_XLOG_H */
