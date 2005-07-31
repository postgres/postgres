/*-------------------------------------------------------------------------
 *
 * procarray.h
 *	  POSTGRES process array definitions.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/procarray.h,v 1.3 2005/07/31 17:19:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCARRAY_H
#define PROCARRAY_H

#include "storage/lock.h"


extern int	ProcArrayShmemSize(void);
extern void CreateSharedProcArray(void);
extern void ProcArrayAdd(PGPROC *proc);
extern void ProcArrayRemove(PGPROC *proc);

extern bool TransactionIdIsInProgress(TransactionId xid);
extern bool TransactionIdIsActive(TransactionId xid);
extern TransactionId GetOldestXmin(bool allDbs);

extern PGPROC *BackendPidGetProc(int pid);
extern bool IsBackendPid(int pid);
extern bool DatabaseHasActiveBackends(Oid databaseId, bool ignoreMyself);

extern int	CountActiveBackends(void);
extern int	CountDBBackends(Oid databaseid);
extern int	CountUserBackends(Oid roleid);

extern void XidCacheRemoveRunningXids(TransactionId xid,
						  int nxids, TransactionId *xids);

#endif   /* PROCARRAY_H */
