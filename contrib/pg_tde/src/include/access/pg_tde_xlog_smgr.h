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

#endif							/* PG_TDE_XLOGSMGR_H */
