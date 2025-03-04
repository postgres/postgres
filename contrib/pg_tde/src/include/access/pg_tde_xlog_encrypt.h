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
#include "access/xlog_smgr.h"

extern Size TDEXLogEncryptBuffSize(void);

extern Size TDEXLogEncryptStateSize(void);
extern void TDEXLogShmemInit(void);

extern ssize_t tdeheap_xlog_seg_read(int fd, void *buf, size_t count, off_t offset,
									 TimeLineID tli, XLogSegNo segno, int segSize);
extern ssize_t tdeheap_xlog_seg_write(int fd, const void *buf, size_t count,
									  off_t offset, TimeLineID tli,
									  XLogSegNo segno);

extern void TDEXLogSmgrInit(void);
extern void XLogInitGUC(void);
#ifndef FRONTEND
extern void TDEXlogCheckSane(void);
#endif

#endif							/* PG_TDE_XLOGENCRYPT_H */
