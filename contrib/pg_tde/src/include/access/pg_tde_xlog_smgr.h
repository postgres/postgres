/*
 * Encrypted XLog storage manager
 */

#ifndef PG_TDE_XLOGSMGR_H
#define PG_TDE_XLOGSMGR_H

#include "postgres.h"

#include "access/pg_tde_xlog_keys.h"

extern Size TDEXLogEncryptStateSize(void);
extern void TDEXLogShmemInit(void);
extern void TDEXLogSmgrInit(void);
extern void TDEXLogSmgrInitWrite(bool encrypt_xlog);
extern void TDEXLogSmgrInitWriteOldKeys(void);

extern void TDEXLogCryptBuffer(const void *buf, void *out_buf, size_t count, off_t offset,
							   TimeLineID tli, XLogSegNo segno, int segSize);

extern bool tde_ensure_xlog_key_location(WalLocation loc);

#endif							/* PG_TDE_XLOGSMGR_H */
