/*-------------------------------------------------------------------------
 *
 * transam.h--
 *    postgres transaction access method support code header
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: transam.h,v 1.1 1996/08/27 21:50:26 scrappy Exp $
 *
 *   NOTES
 *	Transaction System Version 101 now support proper oid
 *	generation and recording in the variable relation.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRANSAM_H
#define TRANSAM_H

/* ----------------
 *	transaction system version id
 *
 *	this is stored on the first page of the log, time and variable
 *	relations on the first 4 bytes.  This is so that if we improve
 *	the format of the transaction log after postgres version 2, then
 *	people won't have to rebuild their databases.
 *
 *	TRANS_SYSTEM_VERSION 100 means major version 1 minor version 0.
 *	Two databases with the same major version should be compatible,
 *	even if their minor versions differ.
 * ----------------
 */
#define TRANS_SYSTEM_VERSION	101

/* ----------------
 *	transaction id status values
 *
 *	someday we will use "11" = 3 = XID_INVALID to mean the
 *	starting of run-length encoded log data.
 * ----------------
 */
#define XID_COMMIT      2       		/* transaction commited */
#define XID_ABORT       1       		/* transaction aborted */
#define XID_INPROGRESS  0       		/* transaction in progress */
#define XID_INVALID     3       		/* other */

typedef unsigned char XidStatus; 		/* (2 bits) */

/* ----------
 *      note: we reserve the first 16384 object ids for internal use.
 *      oid's less than this appear in the .bki files.  the choice of
 *      16384 is completely arbitrary.
 * ----------
 */
#define BootstrapObjectIdData 16384

/* ----------------
 *   	BitIndexOf computes the index of the Nth xid on a given block
 * ----------------
 */
#define BitIndexOf(N)   ((N) * 2)

/* ----------------
 *	transaction page definitions
 * ----------------
 */
#define TP_DataSize		BLCKSZ
#define TP_NumXidStatusPerBlock	(TP_DataSize * 4)
#define TP_NumTimePerBlock	(TP_DataSize / 4)

/* ----------------
 *	LogRelationContents structure
 *
 *	This structure describes the storage of the data in the
 *	first 128 bytes of the log relation.  This storage is never
 *	used for transaction status because transaction id's begin
 *	their numbering at 512.
 *
 *	The first 4 bytes of this relation store the version
 *	number of the transction system.
 * ----------------
 */
typedef struct LogRelationContentsData {
    int			TransSystemVersion;
} LogRelationContentsData;

typedef LogRelationContentsData *LogRelationContents;

/* ----------------
 *	TimeRelationContents structure
 *
 *	This structure describes the storage of the data in the
 *	first 2048 bytes of the time relation.  This storage is never
 *	used for transaction commit times because transaction id's begin
 *	their numbering at 512.
 *
 *	The first 4 bytes of this relation store the version
 *	number of the transction system.
 * ----------------
 */
typedef struct TimeRelationContentsData {
    int			TransSystemVersion;
} TimeRelationContentsData;

typedef TimeRelationContentsData *TimeRelationContents;

/* ----------------
 *	VariableRelationContents structure
 *
 *	The variable relation is a special "relation" which
 *	is used to store various system "variables" persistantly.
 *	Unlike other relations in the system, this relation
 *	is updated in place whenever the variables change.
 *
 *	The first 4 bytes of this relation store the version
 *	number of the transction system.
 *
 *	Currently, the relation has only one page and the next
 *	available xid, the last committed xid and the next
 *	available oid are stored there.
 * ----------------
 */
typedef struct VariableRelationContentsData {
    int			TransSystemVersion;
    TransactionId	nextXidData;
    TransactionId	lastXidData;
    Oid			nextOid;
} VariableRelationContentsData;

typedef VariableRelationContentsData *VariableRelationContents;

/* ----------------
 *	extern declarations
 * ----------------
 */

/*
 * prototypes for functions in transam/transam.c
 */
extern int RecoveryCheckingEnabled();
extern void SetRecoveryCheckingEnabled(bool state);
extern bool TransactionLogTest(TransactionId transactionId, XidStatus status);
extern void TransactionLogUpdate(TransactionId transactionId,
				 XidStatus status);
extern AbsoluteTime TransactionIdGetCommitTime(TransactionId transactionId);
extern void TransRecover(Relation logRelation);
extern void InitializeTransactionLog();
extern bool TransactionIdDidCommit(TransactionId transactionId);
extern bool TransactionIdDidAbort(TransactionId transactionId);
extern bool TransactionIdIsInProgress(TransactionId transactionId);
extern void TransactionIdCommit(TransactionId transactionId);
extern void TransactionIdAbort(TransactionId transactionId);
extern void TransactionIdSetInProgress(TransactionId transactionId);

/* in transam/transsup.c */
extern void AmiTransactionOverride(bool flag);
extern void TransComputeBlockNumber(Relation relation,
	TransactionId transactionId, BlockNumber *blockNumberOutP);
extern XidStatus TransBlockGetLastTransactionIdStatus(Block tblock,
	TransactionId baseXid, TransactionId *returnXidP);
extern XidStatus TransBlockGetXidStatus(Block tblock,
					TransactionId transactionId);
extern void TransBlockSetXidStatus(Block tblock,
	TransactionId transactionId, XidStatus xstatus);
extern AbsoluteTime TransBlockGetCommitTime(Block tblock,
	TransactionId transactionId);
extern void TransBlockSetCommitTime(Block tblock,
	TransactionId transactionId, AbsoluteTime commitTime);
extern XidStatus TransBlockNumberGetXidStatus(Relation relation,
	BlockNumber blockNumber, TransactionId xid, bool *failP);
extern void TransBlockNumberSetXidStatus(Relation relation,
	BlockNumber blockNumber, TransactionId xid, XidStatus xstatus,
	bool *failP);
extern AbsoluteTime TransBlockNumberGetCommitTime(Relation relation,
	BlockNumber blockNumber, TransactionId xid, bool *failP);
extern void TransBlockNumberSetCommitTime(Relation relation,
	BlockNumber blockNumber, TransactionId xid, AbsoluteTime xtime,
	bool *failP);
extern void TransGetLastRecordedTransaction(Relation relation,
	TransactionId xid, bool *failP);

/* in transam/varsup.c */
extern void VariableRelationGetNextXid(TransactionId *xidP);
extern void VariableRelationGetLastXid(TransactionId *xidP);
extern void VariableRelationPutNextXid(TransactionId xid);
extern void VariableRelationPutLastXid(TransactionId xid);
extern void VariableRelationGetNextOid(Oid *oid_return);
extern void VariableRelationPutNextOid(Oid *oidP);
extern void GetNewTransactionId(TransactionId *xid);
extern void UpdateLastCommittedXid(TransactionId xid);
extern void GetNewObjectIdBlock(Oid *oid_return, int oid_block_size);
extern void GetNewObjectId(Oid *oid_return);
extern void CheckMaxObjectId(Oid assigned_oid);

/* ----------------
 *	global variable extern declarations
 * ----------------
 */

/* in transam.c */
extern Relation	LogRelation;
extern Relation	TimeRelation;
extern Relation	VariableRelation;

extern TransactionId	cachedGetCommitTimeXid;
extern AbsoluteTime	cachedGetCommitTime;
extern TransactionId	cachedTestXid;
extern XidStatus	cachedTestXidStatus;

extern TransactionId NullTransactionId;
extern TransactionId AmiTransactionId;
extern TransactionId FirstTransactionId;

extern int RecoveryCheckingEnableState;

/* in transsup.c */
extern bool AMI_OVERRIDE;	

/* in varsup.c */
extern int OidGenLockId;

#endif /* TRAMSAM_H */
