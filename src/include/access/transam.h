/*-------------------------------------------------------------------------
 *
 * transam.h
 *	  postgres transaction access method support code
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: transam.h,v 1.37 2001/08/10 18:57:39 tgl Exp $
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
 *		Object ID (OID) zero is InvalidOid.
 *
 *		OIDs 1-9999 are reserved for manual assignment (see the files
 *		in src/include/catalog/).
 *
 *		OIDS 10000-16383 are reserved for assignment by genbki.sh.
 *
 *		OIDs beginning at 16384 are assigned at runtime from the OID
 *		generator.  (The first few of these will be assigned during initdb,
 *		to objects created after the initial BKI script processing.)
 *
 * The choices of 10000 and 16384 are completely arbitrary, and can be moved
 * if we run low on OIDs in either category.  Changing the macros below
 * should be sufficient to do this.
 *
 * NOTE: if the OID generator wraps around, we should skip over OIDs 0-16383
 * and resume with 16384.  This minimizes the odds of OID conflict, by not
 * reassigning OIDs that might have been assigned during initdb.
 * ----------
 */
#define FirstGenBKIObjectId   10000
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
extern Oid	GetNewObjectId(void);
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
