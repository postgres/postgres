/*-------------------------------------------------------------------------
 *
 * hio.c--
 *    POSTGRES heap access method input/output code.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Id: hio.c,v 1.1.1.1 1996/07/09 06:21:11 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <string.h>

#include "c.h"

#include "access/heapam.h"
#include "access/hio.h"
#include "access/htup.h"

#include "storage/block.h"
#include "storage/buf.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"
#include "storage/itemptr.h"
#include "storage/off.h"

#include "utils/memutils.h"
#include "utils/elog.h"
#include "utils/rel.h"

/*
 * amputunique	- place tuple at tid
 *   Currently on errors, calls elog.  Perhaps should return -1?
 *   Possible errors include the addition of a tuple to the page
 *   between the time the linep is chosen and the page is L_UP'd.
 *
 *   This should be coordinated with the B-tree code.
 *   Probably needs to have an amdelunique to allow for
 *   internal index records to be deleted and reordered as needed.
 *   For the heap AM, this should never be needed.
 */
void
RelationPutHeapTuple(Relation relation,
		     BlockNumber blockIndex,
		     HeapTuple tuple)
{
    Buffer		buffer;
    Page		pageHeader;
    BlockNumber		numberOfBlocks;
    OffsetNumber	offnum;
    unsigned int	len;
    ItemId		itemId;
    Item		item;
    
    /* ----------------
     *	increment access statistics
     * ----------------
     */
    IncrHeapAccessStat(local_RelationPutHeapTuple);
    IncrHeapAccessStat(global_RelationPutHeapTuple);
    
    Assert(RelationIsValid(relation));
    Assert(HeapTupleIsValid(tuple));
    
    numberOfBlocks = RelationGetNumberOfBlocks(relation);
    Assert(blockIndex < numberOfBlocks);
    
    buffer = ReadBuffer(relation, blockIndex);
#ifndef NO_BUFFERISVALID
    if (!BufferIsValid(buffer)) {
	elog(WARN, "RelationPutHeapTuple: no buffer for %ld in %s",
	     blockIndex, &relation->rd_rel->relname);
    }
#endif
    
    pageHeader = (Page)BufferGetPage(buffer);
    len = (unsigned)DOUBLEALIGN(tuple->t_len);	/* be conservative */
    Assert((int)len <= PageGetFreeSpace(pageHeader));
    
    offnum = PageAddItem((Page)pageHeader, (Item)tuple,
			 tuple->t_len, InvalidOffsetNumber, LP_USED);
    
    itemId = PageGetItemId((Page)pageHeader, offnum);
    item = PageGetItem((Page)pageHeader, itemId);
    
    ItemPointerSet(&((HeapTuple)item)->t_ctid, blockIndex, offnum);
    
    WriteBuffer(buffer);
    /* return an accurate tuple */
    ItemPointerSet(&tuple->t_ctid, blockIndex, offnum);
}

/*
 * The heap_insert routines "know" that a buffer page is initialized to
 * zero when a BlockExtend operation is performed. 
 */

#define PageIsNew(page) ((page)->pd_upper == 0)

/*
 * This routine is another in the series of attempts to reduce the number
 * of I/O's and system calls executed in the various benchmarks.  In
 * particular, this routine is used to append data to the end of a relation
 * file without excessive lseeks.  This code should do no more than 2 semops
 * in the ideal case.
 *
 * Eventually, we should cache the number of blocks in a relation somewhere.
 * Until that time, this code will have to do an lseek to determine the number
 * of blocks in a relation.
 * 
 * This code should ideally do at most 4 semops, 1 lseek, and possibly 1 write
 * to do an append; it's possible to eliminate 2 of the semops if we do direct
 * buffer stuff (!); the lseek and the write can go if we get
 * RelationGetNumberOfBlocks to be useful.
 *
 * NOTE: This code presumes that we have a write lock on the relation.
 *
 * Also note that this routine probably shouldn't have to exist, and does
 * screw up the call graph rather badly, but we are wasting so much time and
 * system resources being massively general that we are losing badly in our
 * performance benchmarks.
 */
void
RelationPutHeapTupleAtEnd(Relation relation, HeapTuple tuple)
{
    Buffer		buffer;
    Page		pageHeader;
    BlockNumber		lastblock;
    OffsetNumber	offnum;
    unsigned int	len;
    ItemId		itemId;
    Item		item;
    
    Assert(RelationIsValid(relation));
    Assert(HeapTupleIsValid(tuple));
    
    /*
     * XXX This does an lseek - VERY expensive - but at the moment it
     * is the only way to accurately determine how many blocks are in
     * a relation.  A good optimization would be to get this to actually
     * work properly.
     */
    
    lastblock = RelationGetNumberOfBlocks(relation);
    
    if (lastblock == 0)
	{
	    buffer = ReadBuffer(relation, lastblock);
	    pageHeader = (Page)BufferGetPage(buffer);
	    if (PageIsNew((PageHeader) pageHeader))
		{
		    buffer = ReleaseAndReadBuffer(buffer, relation, P_NEW);
		    pageHeader = (Page)BufferGetPage(buffer);
		    PageInit(pageHeader, BufferGetPageSize(buffer), 0);
		}
	}
    else
	buffer = ReadBuffer(relation, lastblock - 1);
    
    pageHeader = (Page)BufferGetPage(buffer);
    len = (unsigned)DOUBLEALIGN(tuple->t_len);	/* be conservative */
    
    /*
     * Note that this is true if the above returned a bogus page, which
     * it will do for a completely empty relation.
     */
    
    if (len > PageGetFreeSpace(pageHeader))
	{
	    buffer = ReleaseAndReadBuffer(buffer, relation, P_NEW);
	    pageHeader = (Page)BufferGetPage(buffer);
	    PageInit(pageHeader, BufferGetPageSize(buffer), 0);
	    
	    if (len > PageGetFreeSpace(pageHeader))
		elog(WARN, "Tuple is too big: size %d", len);
	}
    
    offnum = PageAddItem((Page)pageHeader, (Item)tuple,
			 tuple->t_len, InvalidOffsetNumber, LP_USED);
    
    itemId = PageGetItemId((Page)pageHeader, offnum);
    item = PageGetItem((Page)pageHeader, itemId);
    
    lastblock = BufferGetBlockNumber(buffer);
    
    ItemPointerSet(&((HeapTuple)item)->t_ctid, lastblock, offnum);
    
    /* return an accurate tuple */
    ItemPointerSet(&tuple->t_ctid, lastblock, offnum);
    
    WriteBuffer(buffer);
}
