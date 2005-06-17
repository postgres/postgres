/*-------------------------------------------------------------------------
 *
 * twophase.h
 *	  Two-phase-commit related declarations.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/access/twophase.h,v 1.1 2005/06/17 22:32:48 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TWOPHASE_H
#define TWOPHASE_H

#include "storage/lock.h"


/*
 * GlobalTransactionData is defined in twophase.c; other places have no
 * business knowing the internal definition.
 */
typedef struct GlobalTransactionData *GlobalTransaction;

/* GUC variable */
extern int max_prepared_xacts;

extern int	TwoPhaseShmemSize(void);
extern void TwoPhaseShmemInit(void);

extern PGPROC *TwoPhaseGetDummyProc(TransactionId xid);

extern GlobalTransaction MarkAsPreparing(TransactionId xid, Oid databaseid,
										 char *gid, AclId owner);
extern void MarkAsPrepared(GlobalTransaction gxact);

extern void StartPrepare(GlobalTransaction gxact);
extern void EndPrepare(GlobalTransaction gxact);

extern TransactionId PrescanPreparedTransactions(void);
extern void RecoverPreparedTransactions(void);

extern void RecreateTwoPhaseFile(TransactionId xid, void *content, int len);
extern void RemoveTwoPhaseFile(TransactionId xid, bool giveWarning);

extern void FinishPreparedTransaction(char *gid, bool isCommit);

#endif   /* TWOPHASE_H */
