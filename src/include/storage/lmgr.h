/*-------------------------------------------------------------------------
 *
 * lmgr.h--
 *	  POSTGRES lock manager definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lmgr.h,v 1.11 1998/06/28 21:17:35 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LMGR_H
#define LMGR_H

#include <storage/lock.h>
#include <utils/rel.h>

/*
 * This was moved from pladt.h for the new lock manager.  Want to obsolete
 * all of the old code.
 */
typedef struct LRelId
{
	Oid			relId;			/* a relation identifier */
	Oid			dbId;			/* a database identifier */
} LRelId;

typedef struct LockInfoData
{
	bool		initialized;
	LRelId		lRelId;
	TransactionId transactionIdData;
	uint16		flags;
} LockInfoData;
typedef LockInfoData *LockInfo;

#define LockInfoIsValid(linfo) \
		((PointerIsValid(linfo)) &&  ((LockInfo) linfo)->initialized)


extern LRelId RelationGetLRelId(Relation relation);
extern Oid	LRelIdGetRelationId(LRelId lRelId);
extern void RelationInitLockInfo(Relation relation);
extern void RelationSetLockForDescriptorOpen(Relation relation);
extern void RelationSetLockForRead(Relation relation);
extern void RelationUnsetLockForRead(Relation relation);
extern void RelationSetLockForWrite(Relation relation);
extern void RelationUnsetLockForWrite(Relation relation);

/* used in vaccum.c */
extern void
RelationSetLockForWritePage(Relation relation,
							ItemPointer itemPointer);

/* used in nbtpage.c, hashpage.c */
extern void
RelationSetSingleWLockPage(Relation relation,
						   ItemPointer itemPointer);
extern void
RelationUnsetSingleWLockPage(Relation relation,
							 ItemPointer itemPointer);
extern void
RelationSetSingleRLockPage(Relation relation,
						   ItemPointer itemPointer);
extern void
RelationUnsetSingleRLockPage(Relation relation,
							 ItemPointer itemPointer);
extern void RelationSetRIntentLock(Relation relation);
extern void RelationUnsetRIntentLock(Relation relation);
extern void RelationSetWIntentLock(Relation relation);
extern void RelationUnsetWIntentLock(Relation relation);

/* single.c */
extern bool SingleLockReln(LockInfo linfo, LOCKTYPE locktype, int action);
extern bool
SingleLockPage(LockInfo linfo, ItemPointer tidPtr,
			   LOCKTYPE locktype, int action);

/* proc.c */
extern void InitProcGlobal(IPCKey key);

#endif							/* LMGR_H */
