/*-------------------------------------------------------------------------
 *
 * varsup.c--
 *	  postgres variable relation support routines
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/varsup.c,v 1.16 1998/07/21 06:17:13 vadim Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/transam.h>
#include <storage/spin.h>
#include <access/xact.h>
#include <access/heapam.h>
#include <catalog/catname.h>

static void GetNewObjectIdBlock(Oid *oid_return, int oid_block_size);
static void VariableRelationGetNextOid(Oid *oid_return);
static void VariableRelationGetNextXid(TransactionId *xidP);
static void VariableRelationPutNextOid(Oid *oidP);

/* ---------------------
 *		spin lock for oid generation
 * ---------------------
 */
int			OidGenLockId;

VariableCache	ShmemVariableCache = NULL;

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
	 * We assume that a spinlock has been acquire to guarantee
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
	int			flushmode;

	/* ----------------
	 * We assume that a spinlock has been acquire to guarantee
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
		elog(ERROR, "VariableRelationPutNextXid: ReadBuffer failed");
	}

	var = (VariableRelationContents) BufferGetBlock(buf);

	TransactionIdStore(xid, &(var->nextXidData));

	flushmode = SetBufferWriteMode(BUFFER_FLUSH_WRITE);
	WriteBuffer(buf);
	SetBufferWriteMode(flushmode);
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
	 * We assume that a spinlock has been acquire to guarantee
	 * exclusive access to the variable relation.
	 * ----------------
	 */

	/* ----------------
	 *	if the variable relation is not initialized, then we
	 *	assume we are running at bootstrap time and so we return
	 *	an invalid object id -- during this time GetNextBootstrapObjectId
	 *	should be called instead..
	 * ----------------
	 */
	if (!RelationIsValid(VariableRelation))
	{
		if (PointerIsValid(oid_return))
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
		elog(ERROR, "VariableRelationGetNextXid: ReadBuffer failed");
	}

	var = (VariableRelationContents) BufferGetBlock(buf);

	if (PointerIsValid(oid_return))
	{

		/* ----------------
		 * nothing up my sleeve...	what's going on here is that this code
		 * is guaranteed never to be called until all files in data/base/
		 * are created, and the template database exists.  at that point,
		 * we want to append a pg_database tuple.  the first time we do
		 * this, the oid stored in pg_variable will be bogus, so we use
		 * a bootstrap value defined at the top of this file.
		 *
		 * this comment no longer holds true.  This code is called before
		 * all of the files in data/base are created and you can't rely
		 * on system oid's to be less than BootstrapObjectIdData. mer 9/18/91
		 * ----------------
		 */
		if (OidIsValid(var->nextOid))
			(*oid_return) = var->nextOid;
		else
			(*oid_return) = BootstrapObjectIdData;
	}

	ReleaseBuffer(buf);
}

/* --------------------------------
 *		VariableRelationPutNextOid
 * --------------------------------
 */
static void
VariableRelationPutNextOid(Oid *oidP)
{
	Buffer		buf;
	VariableRelationContents var;

	/* ----------------
	 * We assume that a spinlock has been acquire to guarantee
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
	 *	sanity check
	 * ----------------
	 */
	if (!PointerIsValid(oidP))
	{
		SpinRelease(OidGenLockId);
		elog(ERROR, "VariableRelationPutNextOid: invalid oid pointer");
	}

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

	var->nextOid = (*oidP);

	WriteBuffer(buf);
}

/* ----------------------------------------------------------------
 *				transaction id generation support
 * ----------------------------------------------------------------
 */

/* ----------------
 *		GetNewTransactionId
 *
 *		In the version 2 transaction system, transaction id's are
 *		restricted in several ways.
 *
 *		-- Old comments removed --
 *
 *		Second, since we may someday preform compression of the data
 *		in the log and time relations, we cause the numbering of the
 *		transaction ids to begin at 512.  This means that some space
 *		on the page of the log and time relations corresponding to
 *		transaction id's 0 - 510 will never be used.  This space is
 *		in fact used to store the version number of the postgres
 *		transaction log and will someday store compression information
 *		about the log.	-- this is also old comments...
 *
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
 *		of the given size.	applications wishing to do their own object
 *		id assignments should use this
 * ----------------
 */
static void
GetNewObjectIdBlock(Oid *oid_return,	/* place to return the new object
										 * id */
					int oid_block_size) /* number of oids desired */
{
	Oid			nextoid;

	/* ----------------
	 *	SOMEDAY obtain exclusive access to the variable relation page
	 *	That someday is today -mer 6 Aug 1992
	 * ----------------
	 */
	SpinAcquire(OidGenLockId);

	/* ----------------
	 *	get the "next" oid from the variable relation
	 *	and give it to the caller.
	 * ----------------
	 */
	VariableRelationGetNextOid(&nextoid);
	if (PointerIsValid(oid_return))
		(*oid_return) = nextoid;

	/* ----------------
	 *	now increment the variable relation's next oid
	 *	field by the size of the oid block requested.
	 * ----------------
	 */
	nextoid += oid_block_size;
	VariableRelationPutNextOid(&nextoid);

	/* ----------------
	 *	SOMEDAY relinquish our lock on the variable relation page
	 *	That someday is today -mer 6 Apr 1992
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
 *			   number to be too large because if when the backend
 *			   terminates, we lose the oids we cached.
 *
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
		 *		during bootstrap time, we want to allocate oids
		 *		one at a time.	Otherwise there might be some
		 *		bootstrap oid's left in the block we prefetch which
		 *		would be passed out after the variable relation was
		 *		initialized.  This would be bad.
		 * ----------------
		 */
		if (!RelationIsValid(VariableRelation))
			VariableRelation = heap_openr(VariableRelationName);

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
	Oid			pass_oid;


	if (prefetched_oid_count == 0)		/* make sure next/max is set, or
										 * reload */
		GetNewObjectId(&pass_oid);

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
	 *
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
	 *
	 * We now:
	 *	  set the variable relation with the new max oid
	 *	  force the backend to reload its oid cache
	 *
	 * We use the oid cache so we don't have to update the variable
	 * relation every time
	 *
	 * ----------------
	 */

	pass_oid = assigned_oid;
	VariableRelationPutNextOid(&pass_oid);		/* not modified */
	prefetched_oid_count = 0;	/* force reload */
	pass_oid = assigned_oid;
	GetNewObjectId(&pass_oid);	/* throw away returned oid */

}
