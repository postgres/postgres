/*-------------------------------------------------------------------------
 *
 * xact.h--
 *    postgres transaction system header
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: xact.h,v 1.4 1996/11/10 03:04:42 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef XACT_H
#define XACT_H

#include <utils/nabstime.h>

/* ----------------
 *	transaction state structure
 * ----------------
 */
typedef struct TransactionStateData {
    TransactionId	transactionIdData;
    CommandId		commandId;
    AbsoluteTime		startTime;
    int			state;
    int			blockState;
} TransactionStateData;

/* ----------------
 *	transaction states
 * ----------------
 */
#define TRANS_DEFAULT		0
#define TRANS_START		1
#define TRANS_INPROGRESS	2
#define TRANS_COMMIT		3
#define TRANS_ABORT		4
#define TRANS_DISABLED		5

/* ----------------
 *	transaction block states
 * ----------------
 */
#define TBLOCK_DEFAULT		0
#define TBLOCK_BEGIN		1
#define TBLOCK_INPROGRESS	2
#define TBLOCK_END		3
#define TBLOCK_ABORT		4
#define TBLOCK_ENDABORT		5

typedef TransactionStateData *TransactionState;

/* ----------------
 *	extern definitions
 * ----------------
 */
extern int TransactionFlushEnabled(void);
extern void SetTransactionFlushEnabled(bool state);

extern bool IsTransactionState(void);
extern bool IsAbortedTransactionBlockState(void);
extern void OverrideTransactionSystem(bool flag);
extern TransactionId GetCurrentTransactionId(void);
extern CommandId GetCurrentCommandId(void);
extern AbsoluteTime GetCurrentTransactionStartTime(void);
extern bool TransactionIdIsCurrentTransactionId(TransactionId xid);
extern bool CommandIdIsCurrentCommandId(CommandId cid);
extern void ClearCommandIdCounterOverflowFlag(void);
extern void CommandCounterIncrement(void);
extern void InitializeTransactionSystem(void);
extern void AtStart_Cache(void);
extern void AtStart_Locks(void);
extern void AtStart_Memory(void);
extern void RecordTransactionCommit(void);
extern void AtCommit_Cache(void);
extern void AtCommit_Locks(void);
extern void AtCommit_Memory(void);
extern void RecordTransactionAbort(void);
extern void AtAbort_Cache(void);
extern void AtAbort_Locks(void);
extern void AtAbort_Memory(void);
extern void StartTransaction(void);
extern bool CurrentXactInProgress(void);
extern void CommitTransaction(void);
extern void AbortTransaction(void);
extern void StartTransactionCommand(void);
extern void CommitTransactionCommand(void);
extern void AbortCurrentTransaction(void);
extern void BeginTransactionBlock(void);
extern void EndTransactionBlock(void);
extern void AbortTransactionBlock(void);
extern bool IsTransactionBlock(void);
extern void UserAbortTransactionBlock(void);

extern TransactionId DisabledTransactionId;

/* defined in xid.c */
extern TransactionId xidin(char *representation);
extern char *xidout(TransactionId transactionId);
extern bool xideq(TransactionId xid1, TransactionId xid2);
extern bool TransactionIdIsValid(TransactionId transactionId);
extern void StoreInvalidTransactionId(TransactionId *destination);
extern void TransactionIdStore(TransactionId transactionId,
			       TransactionId *destination);
extern bool TransactionIdEquals(TransactionId id1, TransactionId id2);
extern bool TransactionIdIsLessThan(TransactionId id1, TransactionId id2);
extern void TransactionIdIncrement(TransactionId *transactionId);
extern void TransactionIdAdd(TransactionId *xid, int value);

#endif	/* XACT_H */
