/*-------------------------------------------------------------------------
 *
 * sinval.h
 *	  POSTGRES shared cache invalidation communication definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: sinval.h,v 1.24 2001/10/28 06:26:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef SINVAL_H
#define SINVAL_H

#include "storage/backendid.h"
#include "storage/itemptr.h"


/*
 * We currently support two types of shared-invalidation messages: one that
 * invalidates an entry in a catcache, and one that invalidates a relcache
 * entry.  More types could be added if needed.  The message type is
 * identified by the first "int16" field of the message struct.  Zero or
 * positive means a catcache inval message (and also serves as the catcache
 * ID field).  -1 means a relcache inval message.  Other negative values
 * are available to identify other inval message types.
 *
 * Shared-inval events are initially driven by detecting tuple inserts,
 * updates and deletions in system catalogs (see RelationInvalidateHeapTuple
 * and RelationMark4RollbackHeapTuple).  Note that some system catalogs have
 * multiple caches on them (with different indexes).  On detecting a tuple
 * invalidation in such a catalog, a separate catcache inval message must be
 * generated for each of its caches.  The catcache inval message carries the
 * hash index for the target tuple, so that the catcache only needs to search
 * one hash chain not all its chains.  Of course this assumes that all the
 * backends are using identical hashing code, but that should be OK.
 */

typedef struct
{
	int16		id;				/* cache ID --- must be first */
	uint16		hashIndex;		/* hashchain index within this catcache */
	Oid			dbId;			/* database ID, or 0 if a shared relation */
	ItemPointerData tuplePtr;	/* tuple identifier in cached relation */
} SharedInvalCatcacheMsg;

#define SHAREDINVALRELCACHE_ID	(-1)

typedef struct
{
	int16		id;				/* type field --- must be first */
	Oid			dbId;			/* database ID, or 0 if a shared relation */
	Oid			relId;			/* relation ID */
} SharedInvalRelcacheMsg;

typedef union
{
	int16		id;				/* type field --- must be first */
	SharedInvalCatcacheMsg cc;
	SharedInvalRelcacheMsg rc;
} SharedInvalidationMessage;


extern int	SInvalShmemSize(int maxBackends);
extern void CreateSharedInvalidationState(int maxBackends);
extern void InitBackendSharedInvalidationState(void);
extern void SendSharedInvalidMessage(SharedInvalidationMessage *msg);
extern void ReceiveSharedInvalidMessages(
				  void (*invalFunction) (SharedInvalidationMessage *msg),
							 void (*resetFunction) (void));

extern bool DatabaseHasActiveBackends(Oid databaseId, bool ignoreMyself);
extern bool TransactionIdIsInProgress(TransactionId xid);
extern TransactionId GetOldestXmin(bool allDbs);
extern int	CountActiveBackends(void);

/* Use "struct PROC", not PROC, to avoid including proc.h here */
extern struct PROC *BackendIdGetProc(BackendId procId);

#endif	 /* SINVAL_H */
