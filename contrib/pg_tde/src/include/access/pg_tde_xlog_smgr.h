/*-------------------------------------------------------------------------
 *
 * pg_tde_xlog_smgr.h
 *	   Encrypted XLog storage manager
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_TDE_XLOGSMGR_H
#define PG_TDE_XLOGSMGR_H

#include "postgres.h"

extern Size TDEXLogEncryptStateSize(void);
extern void TDEXLogShmemInit(void);
extern void TDEXLogSmgrInit(void);

#endif							/* PG_TDE_XLOGSMGR_H */
