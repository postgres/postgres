/*-------------------------------------------------------------------------
 *
 * varsup.c
 *	  postgres variable relation support routines
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/varsup.c,v 1.32 2000/11/08 22:09:55 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifdef XLOG

#include "xlog_varsup.c"

#else

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catname.h"
#include "storage/proc.h"

static void GetNewObjectIdBlock(Oid *oid_return, int oid_block_size);
static void VariableRelationGetNextOid(Oid *oid_return);
static void VariableRelationGetNextXid(TransactionId *xidP);
static void VariableRelationPutNextOid(Oid oid);

/* ---------------------
 *		spin lock for oid generation
 * ---------------------
 */
int			OidGenLockId;

/* ---------------------
 *		pointer to "variable cache" in shared memory (set up by shmem.c)
 * ---------------------
 */
VariableCache ShmemVariableCache = NULL;


/* ----------------------------------------------------------------
 *			  variable relation query/update routines
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		VariableRelationGetNextXid
 * --------------------------------
 */
static void
VariableRelationGetNextXid(TransactionId *xidP)
{
	Buffer		buf;
	VariableRelationContents var;

	/* ----------------
	 * We assume that a spinlock has been acquired to guarantee
	 * exclusive access to the variable relation.
	 * ----------------
	 */

	/* ----------------
	 *	do nothing before things are initialized
	 * ----------------
	 */
	if (!RelationIsValid(VariableRelation))
		return;

	/* ----------------
	 *	read the variable page, get the the nextXid field and
	 *	release the buffer
	 * ----------------
	 */
	buf = ReadBuffer(VariableRelation, 0);

	if (!BufferIsValid(buf))
	{
		SpinRelease(OidGenLockId);
		elog(ERROR, "VariableRelationGetNextXid: ReadBuffer failed");
	}

	var = (VariableRelationContents) BufferGetBlock(buf);

	TransactionIdStore(var->nextXidData, xidP);

	ReleaseBuffer(buf);
}

/* --------------------------------
 *		VariableRelationPutNextXid
 * --------------------------------
 */
void
VariableRelationPutNextXid(TransactionId xid)
{
	Buffer		buf;
	VariableRelationContents var;

	/* ----------------
	 * We assume that a spinlock has been acquired to guarantee
	 * exclusive access to the variable relation.
	 * ----------------
	 */

	/* ----------------
	 *	do nothing before things are initialized
	 * ----------------
	 */
	if (!RelationIsValid(VariableRelation))
		return;

	/* ----------------
	 *	read the variable page, update the nextXid field and
	 *	write the page back out to disk (with immediate write).
	 * ----------------
	 */
	buf = ReadBuffer(VariableRelation, 0);

	if (!BufferIsValid(buf))
	{
		SpinRelease(OidGenLockId);
		elog(ERROR, "VariableRelationPutNextXid: ReadBuffer failed");
	}

	var = (VariableRelationContents) BufferGetBlock(buf);

	TransactionIdStore(xid, &(var->nextXidData));

	FlushBuffer(buf, true, true);
}

/* --------------------------------
 *		VariableRelationGetNextOid
 * --------------------------------
 */
static void
VariableRelationGetNextOid(Oid *oid_return)
{
	Buffer		buf;
	VariableRelationContents var;

	/* ----------------
	 * We assume that a spinlock has been acquired to guarantee
	 * exclusive access to the variable relation.
	 * ----------------
	 */

	/* ----------------
	 *	if the variable relation is not initialized, then we
	 *	assume we are running at bootstrap time and so we return
	 *	an invalid object id (this path should never be taken, probably).
	 * ----------------
	 */
	if (!RelationIsValid(VariableRelation))
	{
		(*oid_return) = InvalidOid;
		return;
	}

	/* ----------------
	 *	read the variable page, get the the nextOid field and
	 *	release the buffer
	 * ----------------
	 */
	buf = ReadBuffer(VariableRelation, 0);

	if (!BufferIsValid(buf))
	{
		SpinRelease(OidGenLockId);
		elog(ERROR, "VariableRelationGetNextOid: ReadBuffer failed");
	}

	var = (VariableRelationContents) BufferGetBlock(buf);

	(*oid_return) = var->nextOid;

	ReleaseBuffer(buf);
}

/* --------------------------------
 *		VariableRelationPutNextOid
 * --------------------------------
 */
static void
VariableRelationPutNextOid(Oid oid)
{
	Buffer		buf;
	VariableRelationContents var;

	/* ----------------
	 * We assume that a spinlock has been acquired to guarantee
	 * exclusive access to the variable relation.
	 * ----------------
	 */

	/* ----------------
	 *	do nothing before things are initialized
	 * ----------------
	 */
	if (!RelationIsValid(VariableRelation))
		return;

	/* ----------------
	 *	read the variable page, update the nextXid field and
	 *	write the page back out to disk.
	 * ----------------
	 */
	buf = ReadBuffer(VariableRelation, 0);

	if (!BufferIsValid(buf))
	{
		SpinRelease(OidGenLockId);
		elog(ERROR, "VariableRelationPutNextOid: ReadBuffer failed");
	}

	var = (VariableRelationContents) BufferGetBlock(buf);

	var->nextOid = oid;

	WriteBuffer(buf);
}

/* ----------------------------------------------------------------
 *				transaction id generation support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		GetNewTransactionId
 *
 *		Transaction IDs are allocated via a cache in shared memory.
 *		Each time we need more IDs, we advance the "next XID" value
 *		in pg_variable by VAR_XID_PREFETCH and set the cache to
 *		show that many XIDs as available.  Then, allocating those XIDs
 *		requires just a spinlock and not a buffer read/write cycle.
 *
 *		Since the cache is shared across all backends, cached but unused
 *		XIDs are not lost when a backend exits, only when the postmaster
 *		quits or forces shared memory reinit.  So we can afford to have
 *		a pretty big value of VAR_XID_PREFETCH.
 *
 *		This code does not worry about initializing the transaction counter
 *		(see transam.c's InitializeTransactionLog() for that).  We also
 *		ignore the possibility that the counter could someday wrap around.
 * ----------------
 */

#define VAR_XID_PREFETCH		1024

void
GetNewTransactionId(TransactionId *xid)
{

	/* ----------------
	 *	during bootstrap initialization, we return the special
	 *	bootstrap transaction id.
	 * ----------------
	 */
	if (AMI_OVERRIDE)
	{
		TransactionIdStore(AmiTransactionId, xid);
		return;
	}

	SpinAcquire(OidGenLockId);	/* not good for concurrency... */

	if (ShmemVariableCache->xid_count == 0)
	{
		TransactionId nextid;

		VariableRelationGetNextXid(&nextid);
		TransactionIdStore(nextid, &(ShmemVariableCache->nextXid));
		ShmemVariableCache->xid_count = VAR_XID_PREFETCH;
		TransactionIdAdd(&nextid, VAR_XID_PREFETCH);
		VariableRelationPutNextXid(nextid);
	}

	TransactionIdStore(ShmemVariableCache->nextXid, xid);
	TransactionIdAdd(&(ShmemVariableCache->nextXid), 1);
	(ShmemVariableCache->xid_count)--;

	if (MyProc != (PROC *) NULL)
		MyProc->xid = *xid;

	SpinRelease(OidGenLockId);
}

/*
 * Like GetNewTransactionId reads nextXid but don't fetch it.
 */
void
ReadNewTransactionId(TransactionId *xid)
{

	/* ----------------
	 *	during bootstrap initialization, we return the special
	 *	bootstrap transaction id.
	 * ----------------
	 */
	if (AMI_OVERRIDE)
	{
		TransactionIdStore(AmiTransactionId, xid);
		return;
	}

	SpinAcquire(OidGenLockId);	/* not good for concurrency... */

	/*
	 * Note that we don't check is ShmemVariableCache->xid_count equal to
	 * 0 or not. This will work as long as we don't call
	 * ReadNewTransactionId() before GetNewTransactionId().
	 */
	if (ShmemVariableCache->nextXid == 0)
		elog(ERROR, "ReadNewTransactionId: ShmemVariableCache->nextXid is not initialized");

	TransactionIdStore(ShmemVariableCache->nextXid, xid);

	SpinRelease(OidGenLockId);
}

/* ----------------------------------------------------------------
 *					object id generation support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		GetNewObjectIdBlock
 *
 *		This support function is used to allocate a block of object ids
 *		of the given size.
 * ----------------
 */
static void
GetNewObjectIdBlock(Oid *oid_return,	/* place to return the first new
										 * object id */
					int oid_block_size) /* number of oids desired */
{
	Oid			firstfreeoid;
	Oid			nextoid;

	/* ----------------
	 *  Obtain exclusive access to the variable relation page
	 * ----------------
	 */
	SpinAcquire(OidGenLockId);

	/* ----------------
	 *	get the "next" oid from the variable relation
	 * ----------------
	 */
	VariableRelationGetNextOid(&firstfreeoid);

	/* ----------------
	 *	Allocate the range of OIDs to be returned to the caller.
	 *
	 *	There are two things going on here.
	 *
	 *	One: in a virgin database pg_variable will initially contain zeroes,
	 *	so we will read out firstfreeoid = InvalidOid.  We want to start
	 *	allocating OIDs at BootstrapObjectIdData instead (OIDs below that
	 *	are reserved for static assignment in the initial catalog data).
	 *
	 *	Two: if a database is run long enough, the OID counter will wrap
	 *	around.  We must not generate an invalid OID when that happens,
	 *	and it seems wise not to generate anything in the reserved range.
	 *	Therefore we advance to BootstrapObjectIdData in this case too.
	 *
	 *	The comparison here assumes that Oid is an unsigned type.
	 */
	nextoid = firstfreeoid + oid_block_size;

	if (! OidIsValid(firstfreeoid) || nextoid < firstfreeoid)
	{
		/* Initialization or wraparound time, force it up to safe range */
		firstfreeoid = BootstrapObjectIdData;
		nextoid = firstfreeoid + oid_block_size;
	}

	(*oid_return) = firstfreeoid;

	/* ----------------
	 *	Update the variable relation to show the block range as used.
	 * ----------------
	 */
	VariableRelationPutNextOid(nextoid);

	/* ----------------
	 *	Relinquish our lock on the variable relation page
	 * ----------------
	 */
	SpinRelease(OidGenLockId);
}

/* ----------------
 *		GetNewObjectId
 *
 *		This function allocates and parses out object ids.	Like
 *		GetNewTransactionId(), it "prefetches" 32 object ids by
 *		incrementing the nextOid stored in the var relation by 32 and then
 *		returning these id's one at a time until they are exhausted.
 *		This means we reduce the number of accesses to the variable
 *		relation by 32 for each backend.
 *
 *		Note:  32 has no special significance.	We don't want the
 *			   number to be too large because when the backend
 *			   terminates, we lose the oids we cached.
 *
 *		Question: couldn't we use a shared-memory cache just like XIDs?
 *		That would allow a larger interval between pg_variable updates
 *		without cache losses.  Note, however, that we can assign an OID
 *		without even a spinlock from the backend-local OID cache.
 *		Maybe two levels of caching would be good.
 * ----------------
 */

#define VAR_OID_PREFETCH		32

static int	prefetched_oid_count = 0;
static Oid	next_prefetched_oid;

void
GetNewObjectId(Oid *oid_return) /* place to return the new object id */
{
	/* ----------------
	 *	if we run out of prefetched oids, then we get some
	 *	more before handing them out to the caller.
	 * ----------------
	 */

	if (prefetched_oid_count == 0)
	{
		int			oid_block_size = VAR_OID_PREFETCH;

		/* ----------------
		 *		Make sure pg_variable is open.
		 * ----------------
		 */
		if (!RelationIsValid(VariableRelation))
			VariableRelation = heap_openr(VariableRelationName, NoLock);

		/* ----------------
		 *		get a new block of prefetched object ids.
		 * ----------------
		 */
		GetNewObjectIdBlock(&next_prefetched_oid, oid_block_size);

		/* ----------------
		 *		now reset the prefetched_oid_count.
		 * ----------------
		 */
		prefetched_oid_count = oid_block_size;
	}

	/* ----------------
	 *	return the next prefetched oid in the pointer passed by
	 *	the user and decrement the prefetch count.
	 * ----------------
	 */
	if (PointerIsValid(oid_return))
		(*oid_return) = next_prefetched_oid;

	next_prefetched_oid++;
	prefetched_oid_count--;
}

void
CheckMaxObjectId(Oid assigned_oid)
{
	Oid			temp_oid;

	if (prefetched_oid_count == 0)		/* make sure next/max is set, or
										 * reload */
		GetNewObjectId(&temp_oid);

	/* ----------------
	 *	If we are below prefetched limits, do nothing
	 * ----------------
	 */

	if (assigned_oid < next_prefetched_oid)
		return;

	/* ----------------
	 *	If we are here, we are coming from a 'copy from' with oid's
	 *
	 *	If we are in the prefetched oid range, just bump it up
	 * ----------------
	 */

	if (assigned_oid <= next_prefetched_oid + prefetched_oid_count - 1)
	{
		prefetched_oid_count -= assigned_oid - next_prefetched_oid + 1;
		next_prefetched_oid = assigned_oid + 1;
		return;
	}

	/* ----------------
	 *	We have exceeded the prefetch oid range
	 *
	 *	We should lock the database and kill all other backends
	 *	but we are loading oid's that we can not guarantee are unique
	 *	anyway, so we must rely on the user
	 *
	 * We now:
	 *	  set the variable relation with the new max oid
	 *	  force the backend to reload its oid cache
	 *
	 * By reloading the oid cache, we don't have to update the variable
	 * relation every time when sequential OIDs are being loaded by COPY.
	 * ----------------
	 */

	SpinAcquire(OidGenLockId);
	VariableRelationPutNextOid(assigned_oid);
	SpinRelease(OidGenLockId);

	prefetched_oid_count = 0;	/* force reload */
	GetNewObjectId(&temp_oid);	/* cause target OID to be allocated */
}

#endif	/* !XLOG */
