/*-------------------------------------------------------------------------
 *
 * lmgr.h--
 *	  POSTGRES lock manager definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: lmgr.h,v 1.13 1998/07/13 16:34:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LMGR_H
#define LMGR_H

#include <storage/lock.h>
#include <utils/rel.h>
#include <catalog/catname.h>

/*
 * This was moved from pladt.h for the new lock manager.  Want to obsolete
 * all of the old code.
 */
typedef struct LockRelId
{
	Oid			relId;			/* a relation identifier */
	Oid			dbId;			/* a database identifier */
} LockRelId;

typedef struct LockInfoData
{
	bool		initialized;
	LockRelId	lockRelId;
	TransactionId transactionIdData;
	uint16		flags;
} LockInfoData;
typedef LockInfoData *LockInfo;

#define LockInfoIsValid(lockinfo) \
		((PointerIsValid(lockinfo)) &&  ((LockInfo) lockinfo)->initialized)

extern LockRelId VariableRelationLockRelId;
		
/*
 * RelationGetLockRelId --
 *		Returns "lock" relation identifier for a relation.
 */
/* ----------------
 * final condition is a hack to prevent problems during
 * VARIABLE relation initialization
 * ----------------
 */
#define RelationGetLockRelId(relation) \
( \
	AssertMacro(RelationIsValid(relation)), \
	(!LockInfoIsValid((LockInfo)(relation)->lockInfo)) ? \
		RelationInitLockInfo(relation) \
	: \
		(void)NULL, \
	(strcmp(RelationGetRelationName(relation)->data, \
			VariableRelationName) == 0) ? \
		VariableRelationLockRelId \
	: \
		((LockInfo)(relation)->lockInfo)->lockRelId \
)


extern Oid	LockRelIdGetRelationId(LockRelId lockRelId);
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
extern bool SingleLockReln(LockInfo lockinfo, LOCKMODE lockmode, int action);
extern bool
SingleLockPage(LockInfo lockinfo, ItemPointer tidPtr,
			   LOCKMODE lockmode, int action);

/* proc.c */
extern void InitProcGlobal(IPCKey key);

#endif							/* LMGR_H */
