/*-------------------------------------------------------------------------
 *
 * twophase.h
 *	  Two-phase-commit related declarations.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/twophase.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TWOPHASE_H
#define TWOPHASE_H

#include "access/xact.h"
#include "access/xlogdefs.h"
#include "datatype/timestamp.h"
#include "storage/lock.h"

/*
 * GlobalTransactionData is defined in twophase.c; other places have no
 * business knowing the internal definition.
 */
typedef struct GlobalTransactionData *GlobalTransaction;

/* GUC variable */
extern PGDLLIMPORT int max_prepared_xacts;

extern Size TwoPhaseShmemSize(void);
extern void TwoPhaseShmemInit(void);

extern void AtAbort_Twophase(void);
extern void PostPrepare_Twophase(void);

extern TransactionId TwoPhaseGetXidByVirtualXID(VirtualTransactionId vxid,
												bool *have_more);
extern PGPROC *TwoPhaseGetDummyProc(FullTransactionId fxid, bool lock_held);
extern int	TwoPhaseGetDummyProcNumber(FullTransactionId fxid, bool lock_held);

extern GlobalTransaction MarkAsPreparing(FullTransactionId fxid, const char *gid,
										 TimestampTz prepared_at,
										 Oid owner, Oid databaseid);

extern void StartPrepare(GlobalTransaction gxact);
extern void EndPrepare(GlobalTransaction gxact);
extern bool StandbyTransactionIdIsPrepared(TransactionId xid);

extern TransactionId PrescanPreparedTransactions(TransactionId **xids_p,
												 int *nxids_p);
extern void StandbyRecoverPreparedTransactions(void);
extern void RecoverPreparedTransactions(void);

extern void CheckPointTwoPhase(XLogRecPtr redo_horizon);

extern void FinishPreparedTransaction(const char *gid, bool isCommit);

extern void PrepareRedoAdd(FullTransactionId fxid, char *buf,
						   XLogRecPtr start_lsn, XLogRecPtr end_lsn,
						   RepOriginId origin_id);
extern void PrepareRedoRemove(TransactionId xid, bool giveWarning);
extern void restoreTwoPhaseData(void);
extern bool LookupGXact(const char *gid, XLogRecPtr prepare_end_lsn,
						TimestampTz origin_prepare_timestamp);

extern void TwoPhaseTransactionGid(Oid subid, TransactionId xid, char *gid_res,
								   int szgid);
extern bool LookupGXactBySubid(Oid subid);

#endif							/* TWOPHASE_H */
