/*
 * Encrypted XLog storage manager
 */

#ifndef PG_TDE_XLOGSMGR_H
#define PG_TDE_XLOGSMGR_H

#include "postgres.h"

extern Size TDEXLogEncryptStateSize(void);
extern void TDEXLogShmemInit(void);
extern void TDEXLogSmgrInit(void);
extern void TDEXLogSmgrInitWrite(bool encrypt_xlog);
extern void TDEXLogSmgrInitWriteOldKeys(void);

extern void TDEXLogCryptBuffer(const void *buf, void *out_buf, size_t count, off_t offset,
							   TimeLineID tli, XLogSegNo segno, int segSize);

#endif							/* PG_TDE_XLOGSMGR_H */
