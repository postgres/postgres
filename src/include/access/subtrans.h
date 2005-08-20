/*
 * subtrans.h
 *
 * PostgreSQL subtransaction-log manager
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/subtrans.h,v 1.7 2005/08/20 23:26:29 tgl Exp $
 */
#ifndef SUBTRANS_H
#define SUBTRANS_H

extern void SubTransSetParent(TransactionId xid, TransactionId parent);
extern TransactionId SubTransGetParent(TransactionId xid);
extern TransactionId SubTransGetTopmostTransaction(TransactionId xid);

extern Size SUBTRANSShmemSize(void);
extern void SUBTRANSShmemInit(void);
extern void BootStrapSUBTRANS(void);
extern void StartupSUBTRANS(TransactionId oldestActiveXID);
extern void ShutdownSUBTRANS(void);
extern void CheckPointSUBTRANS(void);
extern void ExtendSUBTRANS(TransactionId newestXact);
extern void TruncateSUBTRANS(TransactionId oldestXact);

#endif   /* SUBTRANS_H */
