/*
 * clog.h
 *
 * PostgreSQL transaction-commit-log manager
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: clog.h,v 1.7 2003/08/04 02:40:10 momjian Exp $
 */
#ifndef CLOG_H
#define CLOG_H

#include "access/xlog.h"

/*
 * Possible transaction statuses --- note that all-zeroes is the initial
 * state.
 */
typedef int XidStatus;

#define TRANSACTION_STATUS_IN_PROGRESS		0x00
#define TRANSACTION_STATUS_COMMITTED		0x01
#define TRANSACTION_STATUS_ABORTED			0x02
/* 0x03 is available without changing commit log space allocation */

/* exported because lwlock.c needs it */
#define NUM_CLOG_BUFFERS	8


extern void TransactionIdSetStatus(TransactionId xid, XidStatus status);
extern XidStatus TransactionIdGetStatus(TransactionId xid);

extern int	CLOGShmemSize(void);
extern void CLOGShmemInit(void);
extern void BootStrapCLOG(void);
extern void StartupCLOG(void);
extern void ShutdownCLOG(void);
extern void CheckPointCLOG(void);
extern void ExtendCLOG(TransactionId newestXact);
extern void TruncateCLOG(TransactionId oldestXact);

/* XLOG stuff */
#define CLOG_ZEROPAGE		0x00

extern void clog_redo(XLogRecPtr lsn, XLogRecord *record);
extern void clog_undo(XLogRecPtr lsn, XLogRecord *record);
extern void clog_desc(char *buf, uint8 xl_info, char *rec);

#endif   /* CLOG_H */
