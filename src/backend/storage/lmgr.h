/*-------------------------------------------------------------------------
 *
 * lmgr.h--
 *    POSTGRES lock manager definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lmgr.h,v 1.1.1.1 1996/07/09 06:21:53 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	LMGR_H
#define LMGR_H

#include "postgres.h"

#include "storage/itemptr.h"
#include "storage/lock.h"
#include "utils/rel.h"

/* 
 * This was moved from pladt.h for the new lock manager.  Want to obsolete
 * all of the old code.
 */
typedef struct LRelId {
    Oid	 relId;     /* a relation identifier */
    Oid     dbId;      /* a database identifier */
} LRelId;

typedef struct LockInfoData  {
    bool                    initialized;
    LRelId                  lRelId;
    TransactionId           transactionIdData;
    uint16                  flags;
} LockInfoData;
typedef LockInfoData    *LockInfo;

#define LockInfoIsValid(linfo) \
	((PointerIsValid(linfo)) &&  ((LockInfo) linfo)->initialized)


extern LRelId RelationGetLRelId(Relation relation);
extern Oid LRelIdGetDatabaseId(LRelId lRelId);
extern Oid LRelIdGetRelationId(LRelId lRelId);
extern bool DatabaseIdIsMyDatabaseId(Oid databaseId);
extern bool LRelIdContainsMyDatabaseId(LRelId lRelId);
extern void RelationInitLockInfo(Relation relation);
extern void RelationDiscardLockInfo(Relation relation);
extern void RelationSetLockForDescriptorOpen(Relation relation);
extern void RelationSetLockForRead(Relation relation);
extern void RelationUnsetLockForRead(Relation relation);
extern void RelationSetLockForWrite(Relation relation);
extern void RelationUnsetLockForWrite(Relation relation);
extern void RelationSetLockForTupleRead(Relation relation,
					ItemPointer itemPointer);

/* used in vaccum.c */
extern void RelationSetLockForWritePage(Relation relation,
		       ItemPointer itemPointer);

/* used in nbtpage.c, hashpage.c */
extern void RelationSetSingleWLockPage(Relation relation,
		       ItemPointer itemPointer);
extern void RelationUnsetSingleWLockPage(Relation relation,
		       ItemPointer itemPointer);
extern void RelationSetSingleRLockPage(Relation relation,
		       ItemPointer itemPointer);
extern void RelationUnsetSingleRLockPage(Relation relation,
		       ItemPointer itemPointer);
extern void RelationSetRIntentLock(Relation relation);
extern void RelationUnsetRIntentLock(Relation relation);
extern void RelationSetWIntentLock(Relation relation);
extern void RelationUnsetWIntentLock(Relation relation);
extern void RelationSetLockForExtend(Relation relation);
extern void RelationUnsetLockForExtend(Relation relation);
extern void LRelIdAssign(LRelId *lRelId, Oid dbId, Oid relId);

/* single.c */
extern bool SingleLockReln(LockInfo linfo, LOCKT lockt, int action);
extern bool SingleLockPage(LockInfo linfo, ItemPointer tidPtr,
			   LOCKT lockt, int action);

#endif	/* LMGR_H */
