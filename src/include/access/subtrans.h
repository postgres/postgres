/*
 * subtrans.h
 *
 * PostgreSQL subtrans-log manager
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/subtrans.h,v 1.2 2004/08/22 02:41:58 tgl Exp $
 */
#ifndef SUBTRANS_H
#define SUBTRANS_H

#include "access/xlog.h"

/* exported because lwlock.c needs it */
/* cannot be different from NUM_CLOG_BUFFERS without slru.c changes */
#define NUM_SUBTRANS_BUFFERS	NUM_CLOG_BUFFERS

extern void SubTransSetParent(TransactionId xid, TransactionId parent);
extern TransactionId SubTransGetParent(TransactionId xid);
extern TransactionId SubTransGetTopmostTransaction(TransactionId xid);

extern int	SUBTRANSShmemSize(void);
extern void SUBTRANSShmemInit(void);
extern void BootStrapSUBTRANS(void);
extern void StartupSUBTRANS(void);
extern void ShutdownSUBTRANS(void);
extern void CheckPointSUBTRANS(void);
extern void ExtendSUBTRANS(TransactionId newestXact);
extern void TruncateSUBTRANS(TransactionId oldestXact);
extern void subtrans_zeropage_redo(int pageno);

#endif   /* SUBTRANS_H */
