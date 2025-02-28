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

extern Size TDEXLogEncryptStateSize(void);
extern void TDEXLogShmemInit(void);
extern void TDEXLogSmgrInit(void);
extern void XLogInitGUC(void);
#ifndef FRONTEND
extern void TDEXlogCheckSane(void);
#endif

#endif							/* PG_TDE_XLOGENCRYPT_H */
