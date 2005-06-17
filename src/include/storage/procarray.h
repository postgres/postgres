/*-------------------------------------------------------------------------
 *
 * procarray.h
 *	  POSTGRES process array definitions.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/procarray.h,v 1.2 2005/06/17 22:32:50 tgl Exp $
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

extern void XidCacheRemoveRunningXids(TransactionId xid,
						  int nxids, TransactionId *xids);

#endif   /* PROCARRAY_H */
