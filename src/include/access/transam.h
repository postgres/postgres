/*-------------------------------------------------------------------------
 *
 * transam.h
 *	  postgres transaction access method support code
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: transam.h,v 1.36 2001/07/12 04:11:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRANSAM_H
#define TRANSAM_H

#include "storage/bufmgr.h"


/* ----------------
 *		Special transaction ID values
 *
 * We do not use any transaction IDs less than 512 --- this leaves the first
 * 128 bytes of pg_log available for special purposes such as version number
 * storage.  (Currently, we do not actually use them for anything.)
 *
 * AmiTransactionId is the XID for "bootstrap" operations.  It should always
 * be considered valid.
 *
 * FirstTransactionId is the first "normal" transaction id.
 * ----------------
 */
#define NullTransactionId		((TransactionId) 0)
#define DisabledTransactionId	((TransactionId) 1)
#define AmiTransactionId		((TransactionId) 512)
#define FirstTransactionId		((TransactionId) 514)

/* ----------------
 *		transaction ID manipulation macros
 * ----------------
 */
#define TransactionIdIsValid(xid)		((bool) ((xid) != NullTransactionId))
#define TransactionIdIsSpecial(xid)		((bool) ((xid) < FirstTransactionId))
#define TransactionIdEquals(id1, id2)	((bool) ((id1) == (id2)))
#define TransactionIdPrecedes(id1, id2)	((bool) ((id1) < (id2)))
#define TransactionIdStore(xid, dest)	\
	(*((TransactionId*) (dest)) = (TransactionId) (xid))
#define StoreInvalidTransactionId(dest) \
	(*((TransactionId*) (dest)) = NullTransactionId)

/* ----------------
 *		transaction status values
 *
 *		someday we will use "11" = 3 = XID_COMMIT_CHILD to mean the
 *		commiting of child xactions.
 * ----------------
 */
#define XID_INPROGRESS		0	/* transaction in progress */
#define XID_ABORT			1	/* transaction aborted */
#define XID_COMMIT			2	/* transaction commited */
#define XID_COMMIT_CHILD	3	/* child xact commited */

typedef unsigned char XidStatus;	/* (2 bits) */

/* ----------
 *		We reserve the first 16384 object ids for manual assignment.
 *		oid's less than this appear in the .bki files.  the choice of
 *		16384 is completely arbitrary.
 * ----------
 */
#define BootstrapObjectIdData 16384

/*
 * VariableCache is placed in shmem and used by
 * backends to get next available XID & OID.
 */
typedef struct VariableCacheData
{
	TransactionId nextXid;		/* next XID to assign */
	Oid			nextOid;		/* next OID to assign */
	uint32		oidCount;		/* OIDs available before must do XLOG work */
} VariableCacheData;

typedef VariableCacheData *VariableCache;


/* ----------------
 *		extern declarations
 * ----------------
 */

/*
 * prototypes for functions in transam/transam.c
 */
extern void InitializeTransactionLog(void);
extern bool TransactionIdDidCommit(TransactionId transactionId);
extern bool TransactionIdDidAbort(TransactionId transactionId);
extern void TransactionIdCommit(TransactionId transactionId);
extern void TransactionIdAbort(TransactionId transactionId);

/* in transam/transsup.c */
extern void AmiTransactionOverride(bool flag);
extern void TransComputeBlockNumber(Relation relation,
			  TransactionId transactionId, BlockNumber *blockNumberOutP);
extern XidStatus TransBlockNumberGetXidStatus(Relation relation,
				BlockNumber blockNumber, TransactionId xid, bool *failP);
extern void TransBlockNumberSetXidStatus(Relation relation,
		   BlockNumber blockNumber, TransactionId xid, XidStatus xstatus,
							 bool *failP);

/* in transam/varsup.c */
extern void GetNewTransactionId(TransactionId *xid);
extern void ReadNewTransactionId(TransactionId *xid);
extern void GetNewObjectId(Oid *oid_return);
extern void CheckMaxObjectId(Oid assigned_oid);

/* ----------------
 *		global variable extern declarations
 * ----------------
 */

/* in transam.c */
extern Relation LogRelation;

/* in xact.c */
extern bool AMI_OVERRIDE;

/* in varsup.c */
extern SPINLOCK OidGenLockId;
extern SPINLOCK XidGenLockId;
extern VariableCache ShmemVariableCache;

#endif	 /* TRAMSAM_H */
