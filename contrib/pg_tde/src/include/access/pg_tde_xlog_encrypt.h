/*-------------------------------------------------------------------------
 *
 * pg_tde_xlog_encrypt.h
 *	   Encrypted XLog storage manager
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TDE_XLOGENCRYPT_H
#define PG_TDE_XLOGENCRYPT_H

#include "postgres.h"
#ifdef PERCONA_EXT
#include "access/xlog_smgr.h"

extern Size TDEXLogEncryptBuffSize(void);

#define XLOG_TDE_ENC_BUFF_ALIGNED_SIZE	add_size(TDEXLogEncryptBuffSize(), PG_IO_ALIGN_SIZE)

extern void TDEXLogShmemInit(void);

extern ssize_t tdeheap_xlog_seg_read(int fd, void *buf, size_t count, off_t offset);
extern ssize_t tdeheap_xlog_seg_write(int fd, const void *buf, size_t count, off_t offset);

extern void TDEXLogSmgrInit(void);
extern void XLogInitGUC(void);
#ifndef FRONTEND
extern void TDEXlogCheckSane(void);
#endif

#endif							/* PERCONA_EXT */

#endif							/* PG_TDE_XLOGENCRYPT_H */
