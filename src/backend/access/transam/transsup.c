/*-------------------------------------------------------------------------
 *
 * transsup.c--
 *	  postgres transaction access method support code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/transam/Attic/transsup.c,v 1.11 1997/09/08 02:21:18 momjian Exp $
 *
 * NOTES
 *	  This file contains support functions for the high
 *	  level access method interface routines found in transam.c
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <access/transam.h>
#include <utils/bit.h>
#include <access/xact.h>
#include <storage/lmgr.h>

static AbsoluteTime
TransBlockGetCommitTime(Block tblock,
						TransactionId transactionId);
static XidStatus
TransBlockGetXidStatus(Block tblock,
					   TransactionId transactionId);
static void
TransBlockSetCommitTime(Block tblock,
				   TransactionId transactionId, AbsoluteTime commitTime);
static void
TransBlockSetXidStatus(Block tblock,
					   TransactionId transactionId, XidStatus xstatus);

/* ----------------------------------------------------------------
 *					  general support routines
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		AmiTransactionOverride
 *
 *		This function is used to manipulate the bootstrap flag.
 * --------------------------------
 */
void
AmiTransactionOverride(bool flag)
{
	AMI_OVERRIDE = flag;
}

/* --------------------------------
 *		TransComputeBlockNumber
 * --------------------------------
 */
void
TransComputeBlockNumber(Relation relation,		/* relation to test */
						TransactionId transactionId,	/* transaction id to
														 * test */
						BlockNumber * blockNumberOutP)
{
	long		itemsPerBlock = 0;

	/* ----------------
	 *	we calculate the block number of our transaction
	 *	by dividing the transaction id by the number of
	 *	transaction things per block.
	 * ----------------
	 */
	if (relation == LogRelation)
		itemsPerBlock = TP_NumXidStatusPerBlock;
	else if (relation == TimeRelation)
		itemsPerBlock = TP_NumTimePerBlock;
	else
		elog(WARN, "TransComputeBlockNumber: unknown relation");

	/* ----------------
	 *	warning! if the transaction id's get too large
	 *	then a BlockNumber may not be large enough to hold the results
	 *	of our division.
	 *
	 *	XXX  this will all vanish soon when we implement an improved
	 *		 transaction id schema -cim 3/23/90
	 *
	 *	This has vanished now that xid's are 4 bytes (no longer 5).
	 *	-mer 5/24/92
	 * ----------------
	 */
	(*blockNumberOutP) = transactionId / itemsPerBlock;
}


/* ----------------------------------------------------------------
 *					 trans block support routines
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		TransBlockGetLastTransactionIdStatus
 *
 *		This returns the status and transaction id of the last
 *		transaction information recorded on the given TransBlock.
 * --------------------------------
 */

#ifdef NOT_USED
static XidStatus
TransBlockGetLastTransactionIdStatus(Block tblock,
									 TransactionId baseXid,
									 TransactionId * returnXidP)
{
	Index		index;
	Index		maxIndex;
	bits8		bit1;
	bits8		bit2;
	BitIndex	offset;
	XidStatus	xstatus;

	/* ----------------
	 *	sanity check
	 * ----------------
	 */
	Assert((tblock != NULL));

	/* ----------------
	 *	search downward from the top of the block data, looking
	 *	for the first Non-in progress transaction status.  Since we
	 *	are scanning backward, this will be last recorded transaction
	 *	status on the block.
	 * ----------------
	 */
	maxIndex = TP_NumXidStatusPerBlock;
	for (index = maxIndex; index > 0; index--)
	{
		offset = BitIndexOf(index - 1);
		bit1 = ((bits8) BitArrayBitIsSet((BitArray) tblock, offset++)) << 1;
		bit2 = (bits8) BitArrayBitIsSet((BitArray) tblock, offset);

		xstatus = (bit1 | bit2);

		/* ----------------
		 *	here we have the status of some transaction, so test
		 *	if the status is recorded as "in progress".  If so, then
		 *	we save the transaction id in the place specified by the caller.
		 * ----------------
		 */
		if (xstatus != XID_INPROGRESS)
		{
			if (returnXidP != NULL)
			{
				TransactionIdStore(baseXid, returnXidP);
				TransactionIdAdd(returnXidP, index - 1);
			}
			break;
		}
	}

	/* ----------------
	 *	if we get here and index is 0 it means we couldn't find
	 *	a non-inprogress transaction on the block.	For now we just
	 *	return this info to the user.  They can check if the return
	 *	status is "in progress" to know this condition has arisen.
	 * ----------------
	 */
	if (index == 0)
	{
		if (returnXidP != NULL)
			TransactionIdStore(baseXid, returnXidP);
	}

	/* ----------------
	 *	return the status to the user
	 * ----------------
	 */
	return xstatus;
}

#endif

/* --------------------------------
 *		TransBlockGetXidStatus
 *
 *		This returns the status of the desired transaction
 * --------------------------------
 */

static XidStatus
TransBlockGetXidStatus(Block tblock,
					   TransactionId transactionId)
{
	Index		index;
	bits8		bit1;
	bits8		bit2;
	BitIndex	offset;

	/* ----------------
	 *	sanity check
	 * ----------------
	 */
	if (tblock == NULL)
	{
		return XID_INVALID;
	}

	/* ----------------
	 *	calculate the index into the transaction data where
	 *	our transaction status is located
	 *
	 *	XXX this will be replaced soon when we move to the
	 *		new transaction id scheme -cim 3/23/90
	 *
	 *	The old system has now been replaced. -mer 5/24/92
	 * ----------------
	 */
	index = transactionId % TP_NumXidStatusPerBlock;

	/* ----------------
	 *	get the data at the specified index
	 * ----------------
	 */
	offset = BitIndexOf(index);
	bit1 = ((bits8) BitArrayBitIsSet((BitArray) tblock, offset++)) << 1;
	bit2 = (bits8) BitArrayBitIsSet((BitArray) tblock, offset);

	/* ----------------
	 *	return the transaction status to the caller
	 * ----------------
	 */
	return (XidStatus)
		(bit1 | bit2);
}

/* --------------------------------
 *		TransBlockSetXidStatus
 *
 *		This sets the status of the desired transaction
 * --------------------------------
 */
static void
TransBlockSetXidStatus(Block tblock,
					   TransactionId transactionId,
					   XidStatus xstatus)
{
	Index		index;
	BitIndex	offset;

	/* ----------------
	 *	sanity check
	 * ----------------
	 */
	if (tblock == NULL)
		return;

	/* ----------------
	 *	calculate the index into the transaction data where
	 *	we sould store our transaction status.
	 *
	 *	XXX this will be replaced soon when we move to the
	 *		new transaction id scheme -cim 3/23/90
	 *
	 *	The new scheme is here -mer 5/24/92
	 * ----------------
	 */
	index = transactionId % TP_NumXidStatusPerBlock;

	offset = BitIndexOf(index);

	/* ----------------
	 *	store the transaction value at the specified offset
	 * ----------------
	 */
	switch (xstatus)
	{
		case XID_COMMIT:		/* set 10 */
			BitArraySetBit((BitArray) tblock, offset);
			BitArrayClearBit((BitArray) tblock, offset + 1);
			break;
		case XID_ABORT: /* set 01 */
			BitArrayClearBit((BitArray) tblock, offset);
			BitArraySetBit((BitArray) tblock, offset + 1);
			break;
		case XID_INPROGRESS:	/* set 00 */
			BitArrayClearBit((BitArray) tblock, offset);
			BitArrayClearBit((BitArray) tblock, offset + 1);
			break;
		default:
			elog(NOTICE,
				 "TransBlockSetXidStatus: invalid status: %d (ignored)",
				 xstatus);
			break;
	}
}

/* --------------------------------
 *		TransBlockGetCommitTime
 *
 *		This returns the transaction commit time for the
 *		specified transaction id in the trans block.
 * --------------------------------
 */
static AbsoluteTime
TransBlockGetCommitTime(Block tblock,
						TransactionId transactionId)
{
	Index		index;
	AbsoluteTime *timeArray;

	/* ----------------
	 *	sanity check
	 * ----------------
	 */
	if (tblock == NULL)
		return INVALID_ABSTIME;

	/* ----------------
	 *	calculate the index into the transaction data where
	 *	our transaction commit time is located
	 *
	 *	XXX this will be replaced soon when we move to the
	 *		new transaction id scheme -cim 3/23/90
	 *
	 *	The new scheme is here. -mer 5/24/92
	 * ----------------
	 */
	index = transactionId % TP_NumTimePerBlock;

	/* ----------------
	 *	return the commit time to the caller
	 * ----------------
	 */
	timeArray = (AbsoluteTime *) tblock;
	return (AbsoluteTime)
		timeArray[index];
}

/* --------------------------------
 *		TransBlockSetCommitTime
 *
 *		This sets the commit time of the specified transaction
 * --------------------------------
 */
static void
TransBlockSetCommitTime(Block tblock,
						TransactionId transactionId,
						AbsoluteTime commitTime)
{
	Index		index;
	AbsoluteTime *timeArray;

	/* ----------------
	 *	sanity check
	 * ----------------
	 */
	if (tblock == NULL)
		return;


	/* ----------------
	 *	calculate the index into the transaction data where
	 *	we sould store our transaction status.
	 *
	 *	XXX this will be replaced soon when we move to the
	 *		new transaction id scheme -cim 3/23/90
	 *
	 *	The new scheme is here.  -mer 5/24/92
	 * ----------------
	 */
	index = transactionId % TP_NumTimePerBlock;

	/* ----------------
	 *	store the transaction commit time at the specified index
	 * ----------------
	 */
	timeArray = (AbsoluteTime *) tblock;
	timeArray[index] = commitTime;
}

/* ----------------------------------------------------------------
 *				   transam i/o support routines
 * ----------------------------------------------------------------
 */

/* --------------------------------
 *		TransBlockNumberGetXidStatus
 * --------------------------------
 */
XidStatus
TransBlockNumberGetXidStatus(Relation relation,
							 BlockNumber blockNumber,
							 TransactionId xid,
							 bool * failP)
{
	Buffer		buffer;			/* buffer associated with block */
	Block		block;			/* block containing xstatus */
	XidStatus	xstatus;		/* recorded status of xid */
	bool		localfail;		/* bool used if failP = NULL */

	/* ----------------
	 *	SOMEDAY place a read lock on the log relation
	 *	That someday is today 5 Aug 1991 -mer
	 * ----------------
	 */
	RelationSetLockForRead(relation);

	/* ----------------
	 *	get the page containing the transaction information
	 * ----------------
	 */
	buffer = ReadBuffer(relation, blockNumber);
	block = BufferGetBlock(buffer);

	/* ----------------
	 *	get the status from the block.	note, for now we always
	 *	return false in failP.
	 * ----------------
	 */
	if (failP == NULL)
		failP = &localfail;
	(*failP) = false;

	xstatus = TransBlockGetXidStatus(block, xid);

	/* ----------------
	 *	release the buffer and return the status
	 * ----------------
	 */
	ReleaseBuffer(buffer);

	/* ----------------
	 *	SOMEDAY release our lock on the log relation
	 * ----------------
	 */
	RelationUnsetLockForRead(relation);

	return
		xstatus;
}

/* --------------------------------
 *		TransBlockNumberSetXidStatus
 * --------------------------------
 */
void
TransBlockNumberSetXidStatus(Relation relation,
							 BlockNumber blockNumber,
							 TransactionId xid,
							 XidStatus xstatus,
							 bool * failP)
{
	Buffer		buffer;			/* buffer associated with block */
	Block		block;			/* block containing xstatus */
	bool		localfail;		/* bool used if failP = NULL */

	/* ----------------
	 *	SOMEDAY gain exclusive access to the log relation
	 *
	 *	That someday is today 5 Aug 1991 -mer
	 * ----------------
	 */
	RelationSetLockForWrite(relation);

	/* ----------------
	 *	get the block containing the transaction status
	 * ----------------
	 */
	buffer = ReadBuffer(relation, blockNumber);
	block = BufferGetBlock(buffer);

	/* ----------------
	 *	attempt to update the status of the transaction on the block.
	 *	if we are successful, write the block. otherwise release the buffer.
	 *	note, for now we always return false in failP.
	 * ----------------
	 */
	if (failP == NULL)
		failP = &localfail;
	(*failP) = false;

	TransBlockSetXidStatus(block, xid, xstatus);

	if ((*failP) == false)
		WriteBuffer(buffer);
	else
		ReleaseBuffer(buffer);

	/* ----------------
	 *	SOMEDAY release our lock on the log relation
	 * ----------------
	 */
	RelationUnsetLockForWrite(relation);
}

/* --------------------------------
 *		TransBlockNumberGetCommitTime
 * --------------------------------
 */
AbsoluteTime
TransBlockNumberGetCommitTime(Relation relation,
							  BlockNumber blockNumber,
							  TransactionId xid,
							  bool * failP)
{
	Buffer		buffer;			/* buffer associated with block */
	Block		block;			/* block containing commit time */
	bool		localfail;		/* bool used if failP = NULL */
	AbsoluteTime xtime;			/* commit time */

	/* ----------------
	 *	SOMEDAY place a read lock on the time relation
	 *
	 *	That someday is today 5 Aug. 1991 -mer
	 * ----------------
	 */
	RelationSetLockForRead(relation);

	/* ----------------
	 *	get the block containing the transaction information
	 * ----------------
	 */
	buffer = ReadBuffer(relation, blockNumber);
	block = BufferGetBlock(buffer);

	/* ----------------
	 *	get the commit time from the block
	 *	note, for now we always return false in failP.
	 * ----------------
	 */
	if (failP == NULL)
		failP = &localfail;
	(*failP) = false;

	xtime = TransBlockGetCommitTime(block, xid);

	/* ----------------
	 *	release the buffer and return the commit time
	 * ----------------
	 */
	ReleaseBuffer(buffer);

	/* ----------------
	 *	SOMEDAY release our lock on the time relation
	 * ----------------
	 */
	RelationUnsetLockForRead(relation);

	if ((*failP) == false)
		return xtime;
	else
		return INVALID_ABSTIME;

}

/* --------------------------------
 *		TransBlockNumberSetCommitTime
 * --------------------------------
 */
void
TransBlockNumberSetCommitTime(Relation relation,
							  BlockNumber blockNumber,
							  TransactionId xid,
							  AbsoluteTime xtime,
							  bool * failP)
{
	Buffer		buffer;			/* buffer associated with block */
	Block		block;			/* block containing commit time */
	bool		localfail;		/* bool used if failP = NULL */

	/* ----------------
	 *	SOMEDAY gain exclusive access to the time relation
	 *
	 *	That someday is today 5 Aug. 1991 -mer
	 * ----------------
	 */
	RelationSetLockForWrite(relation);

	/* ----------------
	 *	get the block containing our commit time
	 * ----------------
	 */
	buffer = ReadBuffer(relation, blockNumber);
	block = BufferGetBlock(buffer);

	/* ----------------
	 *	attempt to update the commit time of the transaction on the block.
	 *	if we are successful, write the block. otherwise release the buffer.
	 *	note, for now we always return false in failP.
	 * ----------------
	 */
	if (failP == NULL)
		failP = &localfail;
	(*failP) = false;

	TransBlockSetCommitTime(block, xid, xtime);

	if ((*failP) == false)
		WriteBuffer(buffer);
	else
		ReleaseBuffer(buffer);

	/* ----------------
	 *	SOMEDAY release our lock on the time relation
	 * ----------------
	 */
	RelationUnsetLockForWrite(relation);

}

/* --------------------------------
 *		TransGetLastRecordedTransaction
 * --------------------------------
 */
#ifdef NOT_USED
void
TransGetLastRecordedTransaction(Relation relation,
								TransactionId xid,		/* return: transaction
														 * id */
								bool * failP)
{
	BlockNumber blockNumber;	/* block number */
	Buffer		buffer;			/* buffer associated with block */
	Block		block;			/* block containing xid status */
	BlockNumber n;				/* number of blocks in the relation */
	TransactionId baseXid;

	(*failP) = false;

	/* ----------------
	 *	SOMEDAY gain exclusive access to the log relation
	 *
	 *	That someday is today 5 Aug. 1991 -mer
	 *	It looks to me like we only need to set a read lock here, despite
	 *	the above comment about exclusive access.  The block is never
	 *	actually written into, we only check status bits.
	 * ----------------
	 */
	RelationSetLockForRead(relation);

	/* ----------------
	 *	we assume the last block of the log contains the last
	 *	recorded transaction.  If the relation is empty we return
	 *	failure to the user.
	 * ----------------
	 */
	n = RelationGetNumberOfBlocks(relation);
	if (n == 0)
	{
		(*failP) = true;
		return;
	}

	/* ----------------
	 *	get the block containing the transaction information
	 * ----------------
	 */
	blockNumber = n - 1;
	buffer = ReadBuffer(relation, blockNumber);
	block = BufferGetBlock(buffer);

	/* ----------------
	 *	get the last xid on the block
	 * ----------------
	 */
	baseXid = blockNumber * TP_NumXidStatusPerBlock;

/* XXX ???? xid won't get returned! - AY '94 */
	TransBlockGetLastTransactionIdStatus(block, baseXid, &xid);

	ReleaseBuffer(buffer);

	/* ----------------
	 *	SOMEDAY release our lock on the log relation
	 * ----------------
	 */
	RelationUnsetLockForRead(relation);
}

#endif
