/*-------------------------------------------------------------------------
 *
 * transam.h
 *	  postgres transaction access method support code
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: transam.h,v 1.39 2001/08/25 18:52:42 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TRANSAM_H
#define TRANSAM_H

#include "storage/spin.h"


/* ----------------
 *		Special transaction ID values
 *
 * BootstrapTransactionId is the XID for "bootstrap" operations, and
 * FrozenTransactionId is used for very old tuples.  Both should
 * always be considered valid.
 *
 * FirstNormalTransactionId is the first "normal" transaction id.
 * ----------------
 */
#define InvalidTransactionId		((TransactionId) 0)
#define BootstrapTransactionId		((TransactionId) 1)
#define FrozenTransactionId			((TransactionId) 2)
#define FirstNormalTransactionId	((TransactionId) 3)

/* ----------------
 *		transaction ID manipulation macros
 * ----------------
 */
#define TransactionIdIsValid(xid)		((xid) != InvalidTransactionId)
#define TransactionIdIsNormal(xid)		((xid) >= FirstNormalTransactionId)
#define TransactionIdEquals(id1, id2)			((id1) == (id2))
#define TransactionIdPrecedes(id1, id2)			((id1) < (id2))
#define TransactionIdPrecedesOrEquals(id1, id2)	((id1) <= (id2))
#define TransactionIdFollows(id1, id2)			((id1) > (id2))
#define TransactionIdFollowsOrEquals(id1, id2)	((id1) >= (id2))
#define TransactionIdStore(xid, dest)	(*(dest) = (xid))
#define StoreInvalidTransactionId(dest)	(*(dest) = InvalidTransactionId)
/* advance a transaction ID variable, handling wraparound correctly */
#define TransactionIdAdvance(dest)	\
	do { \
		(dest)++; \
		if ((dest) < FirstNormalTransactionId) \
			(dest) = FirstNormalTransactionId; \
	} while(0)


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
extern void AmiTransactionOverride(bool flag);
extern bool TransactionIdDidCommit(TransactionId transactionId);
extern bool TransactionIdDidAbort(TransactionId transactionId);
extern void TransactionIdCommit(TransactionId transactionId);
extern void TransactionIdAbort(TransactionId transactionId);

/* in transam/varsup.c */
extern TransactionId GetNewTransactionId(void);
extern TransactionId ReadNewTransactionId(void);
extern Oid	GetNewObjectId(void);
extern void CheckMaxObjectId(Oid assigned_oid);

/* ----------------
 *		global variable extern declarations
 * ----------------
 */

/* in xact.c */
extern bool AMI_OVERRIDE;

/* in varsup.c */
extern SPINLOCK OidGenLockId;
extern SPINLOCK XidGenLockId;
extern VariableCache ShmemVariableCache;

#endif	 /* TRAMSAM_H */
