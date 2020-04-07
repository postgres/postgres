/*-------------------------------------------------------------------------
 *
 * wait.h
 *	  prototypes for commands/wait.c
 *
 * Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * src/include/commands/wait.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef WAIT_H
#define WAIT_H

#include "tcop/dest.h"
#include "nodes/parsenodes.h"

extern bool WaitLSNUtility(XLogRecPtr lsn, const int timeout_ms);
extern Size WaitLSNShmemSize(void);
extern void WaitLSNShmemInit(void);
extern void WaitLSNSetLatch(XLogRecPtr cur_lsn);
extern XLogRecPtr WaitLSNGetMin(void);
extern int	WaitLSNMain(WaitClause *stmt, DestReceiver *dest);
extern void WaitLSNDelete(void);

#endif							/* WAIT_H */
