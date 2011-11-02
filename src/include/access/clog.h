/*
 * clog.h
 *
 * PostgreSQL transaction-commit-log manager
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/clog.h,v 1.25 2010/01/02 16:58:00 momjian Exp $
 */
#ifndef CLOG_H
#define CLOG_H

#include "access/xlog.h"

/*
 * Possible transaction statuses --- note that all-zeroes is the initial
 * state.
 *
 * A "subcommitted" transaction is a committed subtransaction whose parent
 * hasn't committed or aborted yet.
 */
typedef int XidStatus;

#define TRANSACTION_STATUS_IN_PROGRESS		0x00
#define TRANSACTION_STATUS_COMMITTED		0x01
#define TRANSACTION_STATUS_ABORTED			0x02
#define TRANSACTION_STATUS_SUB_COMMITTED	0x03


/* Number of SLRU buffers to use for clog */
#define NUM_CLOG_BUFFERS	8


extern void TransactionIdSetTreeStatus(TransactionId xid, int nsubxids,
				   TransactionId *subxids, XidStatus status, XLogRecPtr lsn);
extern XidStatus TransactionIdGetStatus(TransactionId xid, XLogRecPtr *lsn);

extern Size CLOGShmemSize(void);
extern void CLOGShmemInit(void);
extern void BootStrapCLOG(void);
extern void StartupCLOG(void);
extern void TrimCLOG(void);
extern void ShutdownCLOG(void);
extern void CheckPointCLOG(void);
extern void ExtendCLOG(TransactionId newestXact);
extern void TruncateCLOG(TransactionId oldestXact);

/* XLOG stuff */
#define CLOG_ZEROPAGE		0x00
#define CLOG_TRUNCATE		0x10

extern void clog_redo(XLogRecPtr lsn, XLogRecord *record);
extern void clog_desc(StringInfo buf, uint8 xl_info, char *rec);

#endif   /* CLOG_H */
