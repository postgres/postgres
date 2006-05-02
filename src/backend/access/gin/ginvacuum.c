/*-------------------------------------------------------------------------
 *
 * ginvacuum.c
 *    delete & vacuum routines for the postgres GIN
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *          $PostgreSQL: pgsql/src/backend/access/gin/ginvacuum.c,v 1.1 2006/05/02 11:28:54 teodor Exp $
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "access/genam.h"
#include "access/gin.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "miscadmin.h"
#include "storage/freespace.h"
#include "utils/memutils.h"
#include "storage/freespace.h"
#include "storage/smgr.h"
#include "commands/vacuum.h"

typedef struct {
	Relation				index;
	IndexBulkDeleteResult	*result;
	IndexBulkDeleteCallback	callback;
	void					*callback_state;
	GinState				ginstate;
} GinVacuumState;


/*
 * Cleans array of ItemPointer (removes dead pointers)
 * Results are always stored in *cleaned, which will be allocated
 * if its needed. In case of *cleaned!=NULL caller is resposible to 
 * enough space. *cleaned and items may point to the same
 * memory addres.
 */

static uint32
ginVacuumPostingList( GinVacuumState *gvs, ItemPointerData *items, uint32 nitem, ItemPointerData **cleaned ) {
	uint32 i,j=0;

	/*
	 * just scan over ItemPointer array
	 */

	for(i=0;i<nitem;i++) {
		if ( gvs->callback(items+i, gvs->callback_state) ) { 
			gvs->result->tuples_removed += 1;
			if ( !*cleaned )  {
				*cleaned = (ItemPointerData*)palloc(sizeof(ItemPointerData)*nitem);
				if ( i!=0 ) 
					memcpy( *cleaned, items, sizeof(ItemPointerData)*i);
			}
		} else {
			gvs->result->num_index_tuples += 1;
			if (i!=j)
				(*cleaned)[j] = items[i];
			j++;
		}
	}

	return j;
}

/*
 * fills WAL record for vacuum leaf page
 */
static void
xlogVacuumPage(Relation index, Buffer buffer) {
	Page	page = BufferGetPage( buffer );
	XLogRecPtr  recptr;
	XLogRecData rdata[3];
	ginxlogVacuumPage	data;
	char *backup;
	char itups[BLCKSZ];
	uint32 len=0;

	Assert( GinPageIsLeaf( page ) );

	if (index->rd_istemp)
		return; 

	data.node = index->rd_node;
	data.blkno = BufferGetBlockNumber(buffer);

	if ( GinPageIsData( page ) ) {
		backup = GinDataPageGetData( page );
		data.nitem = GinPageGetOpaque( page )->maxoff;
		if ( data.nitem )
			len = MAXALIGN( sizeof(ItemPointerData)*data.nitem );
	} else {
		char *ptr;
		OffsetNumber i;

		ptr = backup = itups;
		for(i=FirstOffsetNumber;i<=PageGetMaxOffsetNumber(page);i++) {
			IndexTuple itup =  (IndexTuple) PageGetItem(page, PageGetItemId(page, i));
			memcpy( ptr, itup, IndexTupleSize( itup ) );
			ptr += MAXALIGN( IndexTupleSize( itup ) );
		}

		data.nitem = PageGetMaxOffsetNumber(page);
		len = ptr-backup;
	}

	rdata[0].buffer = buffer;
	rdata[0].buffer_std = ( GinPageIsData( page ) ) ? FALSE : TRUE;
	rdata[0].len = 0;
	rdata[0].data = NULL;
	rdata[0].next = rdata + 1;

	rdata[1].buffer = InvalidBuffer;
	rdata[1].len = sizeof(ginxlogVacuumPage);
	rdata[1].data = (char*)&data;

	if ( len == 0 ) {
		rdata[1].next = NULL;
	} else {
		rdata[1].next = rdata + 2;

		rdata[2].buffer = InvalidBuffer;
		rdata[2].len = len;
		rdata[2].data = backup;
		rdata[2].next = NULL;
	}

	recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_VACUUM_PAGE, rdata);
	PageSetLSN(page, recptr);
	PageSetTLI(page, ThisTimeLineID);
}

static bool
ginVacuumPostingTreeLeaves( GinVacuumState *gvs, BlockNumber blkno, bool isRoot, Buffer *rootBuffer ) { 
	Buffer 	buffer = ReadBuffer( gvs->index, blkno );
	Page	page   = BufferGetPage( buffer );
	bool 	hasVoidPage = FALSE;	

	/* 
	 * We should be sure that we don't concurrent with inserts, insert process
	 * never release root page until end (but it can unlock it and lock again).
	 * If we lock root with with LockBufferForCleanup, new scan process can't begin,
	 * but previous may run. 
	 * ginmarkpos/start* keeps buffer pinned, so we will wait for it.
	 * We lock only one posting tree in whole index, so, it's concurrent enough..
	 * Side effect: after this is full complete, tree is unused by any other process 
	 */

	LockBufferForCleanup( buffer );

	Assert( GinPageIsData(page) );

	if ( GinPageIsLeaf(page) ) {
		OffsetNumber newMaxOff, oldMaxOff = GinPageGetOpaque(page)->maxoff;
		ItemPointerData *cleaned = NULL;

		newMaxOff = ginVacuumPostingList( gvs, 
				(ItemPointer)GinDataPageGetData(page), oldMaxOff, &cleaned );

		/* saves changes about deleted tuple ... */
		if ( oldMaxOff != newMaxOff ) {

			START_CRIT_SECTION();

			if ( newMaxOff > 0 ) 
				memcpy( GinDataPageGetData(page), cleaned, sizeof(ItemPointerData) * newMaxOff );
			pfree( cleaned );
			GinPageGetOpaque(page)->maxoff = newMaxOff;

			xlogVacuumPage(gvs->index,  buffer);

			MarkBufferDirty( buffer );
			END_CRIT_SECTION();
			
			/* if root is a leaf page, we don't desire futher processing */ 
			if ( !isRoot && GinPageGetOpaque(page)->maxoff < FirstOffsetNumber )
				hasVoidPage = TRUE;
		}
	} else {
		OffsetNumber i;
		bool isChildHasVoid = FALSE;

		for( i=FirstOffsetNumber ; i <= GinPageGetOpaque(page)->maxoff ; i++ ) {
			PostingItem *pitem = (PostingItem*)GinDataPageGetItem(page, i);
			if ( ginVacuumPostingTreeLeaves( gvs, PostingItemGetBlockNumber(pitem), FALSE, NULL ) )
				isChildHasVoid = TRUE;
		}

		if ( isChildHasVoid )
			hasVoidPage = TRUE;
	}

	/* if we have root and theres void pages in tree, then we don't release lock
	   to go further processing and guarantee that tree is unused */
	if ( !(isRoot && hasVoidPage) ) {
		UnlockReleaseBuffer( buffer );
	} else {
		Assert( rootBuffer );
		*rootBuffer = buffer;
	}

	return hasVoidPage;
}

static void
ginDeletePage(GinVacuumState *gvs, BlockNumber deleteBlkno, BlockNumber leftBlkno, 
		BlockNumber parentBlkno, OffsetNumber myoff, bool isParentRoot ) {
	Buffer	dBuffer = ReadBuffer( gvs->index, deleteBlkno );
	Buffer	lBuffer	= (leftBlkno==InvalidBlockNumber) ? InvalidBuffer : ReadBuffer( gvs->index, leftBlkno );
	Buffer	pBuffer	= ReadBuffer( gvs->index, parentBlkno );
	Page page, parentPage;

	LockBuffer( dBuffer, GIN_EXCLUSIVE );
	if ( !isParentRoot ) /* parent is already locked by LockBufferForCleanup() */
		LockBuffer( pBuffer, GIN_EXCLUSIVE );

	START_CRIT_SECTION();

	if ( leftBlkno!= InvalidBlockNumber ) {
		BlockNumber rightlink;

		LockBuffer( lBuffer, GIN_EXCLUSIVE );

		page = BufferGetPage( dBuffer );
		rightlink = GinPageGetOpaque(page)->rightlink;

		page = BufferGetPage( lBuffer );
		GinPageGetOpaque(page)->rightlink = rightlink;
	}

	parentPage = BufferGetPage( pBuffer );
	PageDeletePostingItem(parentPage, myoff);

	page = BufferGetPage( dBuffer );
	GinPageGetOpaque(page)->flags = GIN_DELETED;

	if (!gvs->index->rd_istemp) {
		XLogRecPtr  recptr;
		XLogRecData rdata[4];
		ginxlogDeletePage	 data;
		int n;

		data.node = gvs->index->rd_node;
		data.blkno = deleteBlkno;
		data.parentBlkno = parentBlkno;
		data.parentOffset = myoff;
		data.leftBlkno = leftBlkno; 
		data.rightLink = GinPageGetOpaque(page)->rightlink; 

		rdata[0].buffer = dBuffer;
		rdata[0].buffer_std = FALSE;
		rdata[0].data = NULL;
		rdata[0].len = 0;
		rdata[0].next = rdata + 1;

		rdata[1].buffer = pBuffer;
		rdata[1].buffer_std = FALSE;
		rdata[1].data = NULL;
		rdata[1].len = 0;
		rdata[1].next = rdata + 2;

		if ( leftBlkno!= InvalidBlockNumber ) { 	
			rdata[2].buffer = lBuffer;
			rdata[2].buffer_std = FALSE;
			rdata[2].data = NULL;
			rdata[2].len = 0;
			rdata[2].next = rdata + 3;
			n = 3;
		} else
			n = 2;

		rdata[n].buffer = InvalidBuffer;
		rdata[n].buffer_std = FALSE;
		rdata[n].len = sizeof(ginxlogDeletePage);
		rdata[n].data = (char*)&data;
		rdata[n].next = NULL;

		recptr = XLogInsert(RM_GIN_ID, XLOG_GIN_DELETE_PAGE, rdata);
		PageSetLSN(page, recptr);
		PageSetTLI(page, ThisTimeLineID);
		PageSetLSN(parentPage, recptr);
		PageSetTLI(parentPage, ThisTimeLineID);
		if ( leftBlkno!= InvalidBlockNumber ) {
			page = BufferGetPage( lBuffer );
			PageSetLSN(page, recptr);
			PageSetTLI(page, ThisTimeLineID);
		}
	}

	MarkBufferDirty( pBuffer );
	if ( !isParentRoot )
		LockBuffer( pBuffer, GIN_UNLOCK );
	ReleaseBuffer( pBuffer );

	if ( leftBlkno!= InvalidBlockNumber ) {
		MarkBufferDirty( lBuffer );
		UnlockReleaseBuffer( lBuffer );
	}

	MarkBufferDirty( dBuffer );
	UnlockReleaseBuffer( dBuffer );

	END_CRIT_SECTION();

	gvs->result->pages_deleted++;
}

typedef struct DataPageDeleteStack {
	struct DataPageDeleteStack	*child;	
	struct DataPageDeleteStack	*parent;

	BlockNumber					blkno;
	bool						isRoot;
} DataPageDeleteStack;

/*
 * scans posting tree and deletes empty pages
 */
static bool
ginScanToDelete( GinVacuumState *gvs, BlockNumber blkno, bool isRoot, DataPageDeleteStack *parent, OffsetNumber myoff ) {
	DataPageDeleteStack	*me;
	Buffer	buffer;
	Page	page;
	bool	meDelete = FALSE;

	if ( isRoot ) {
		me = parent;
	} else {
		if ( ! parent->child ) {
			me  = (DataPageDeleteStack*)palloc0(sizeof(DataPageDeleteStack));
			me->parent=parent;
			parent->child = me;
			me->blkno = InvalidBlockNumber;
		} else
			me = parent->child;
	}

	buffer = ReadBuffer( gvs->index, blkno ); 
	page = BufferGetPage( buffer );

	Assert( GinPageIsData(page) );

	if ( !GinPageIsLeaf(page) ) {
		OffsetNumber i;

		for(i=FirstOffsetNumber;i<=GinPageGetOpaque(page)->maxoff;i++) {
			PostingItem *pitem = (PostingItem*)GinDataPageGetItem(page, i);

			if ( ginScanToDelete( gvs, PostingItemGetBlockNumber(pitem), FALSE, me, i ) )
				i--;
		}
	}

	if ( GinPageGetOpaque(page)->maxoff < FirstOffsetNumber ) {
		if ( !( me->blkno == InvalidBlockNumber && GinPageRightMost(page) ) ) {
			/* we never delete right most branch */
			Assert( !isRoot );
			if ( GinPageGetOpaque(page)->maxoff < FirstOffsetNumber )  {
				ginDeletePage( gvs, blkno, me->blkno, me->parent->blkno, myoff, me->parent->isRoot );
				meDelete = TRUE;
			}
		}
	}

	ReleaseBuffer( buffer );

	if ( !meDelete )
		me->blkno = blkno;

	return meDelete;
}

static void
ginVacuumPostingTree( GinVacuumState *gvs, BlockNumber rootBlkno ) {
	Buffer	rootBuffer = InvalidBuffer;
	DataPageDeleteStack	root, *ptr, *tmp;

	if ( ginVacuumPostingTreeLeaves(gvs, rootBlkno, TRUE, &rootBuffer)==FALSE ) {
		Assert( rootBuffer == InvalidBuffer );
		return;
	}

	memset(&root,0,sizeof(DataPageDeleteStack));
	root.blkno = rootBlkno;
	root.isRoot = TRUE;

	vacuum_delay_point();

	ginScanToDelete( gvs, rootBlkno, TRUE, &root, InvalidOffsetNumber ); 

	ptr = root.child;
	while( ptr ) {
		tmp = ptr->child;
		pfree( ptr );
		ptr = tmp;
	}

	UnlockReleaseBuffer( rootBuffer );
}

/*
 * returns modified page or NULL if page isn't modified.
 * Function works with original page until first change is occured,
 * then page is copied into temprorary one.
 */
static Page
ginVacuumEntryPage(GinVacuumState *gvs, Buffer buffer, BlockNumber *roots, uint32 *nroot) {
	Page	origpage = BufferGetPage( buffer ), tmppage;
	OffsetNumber i, maxoff = PageGetMaxOffsetNumber( origpage );

	tmppage = origpage;

	*nroot=0;

	for(i=FirstOffsetNumber; i<= maxoff; i++) {
		IndexTuple itup = (IndexTuple) PageGetItem(tmppage, PageGetItemId(tmppage, i));

		if ( GinIsPostingTree(itup) ) {
			/* store posting tree's roots for further processing,
			   we can't vacuum it just now due to risk of deadlocks with scans/inserts */
			roots[ *nroot ] = GinItemPointerGetBlockNumber(&itup->t_tid);
			(*nroot)++;
		} else if ( GinGetNPosting(itup) > 0 ) {
			/* if we already create temrorary page, we will make changes in place */
			ItemPointerData	*cleaned = (tmppage==origpage) ? NULL : GinGetPosting(itup );
			uint32  newN = ginVacuumPostingList( gvs, GinGetPosting(itup), GinGetNPosting(itup), &cleaned );
			
			if ( GinGetNPosting(itup) != newN ) {
				bool	isnull;
				Datum value;

				/*
				 * Some ItemPointers was deleted, so we should remake our tuple 
				 */

				if ( tmppage==origpage ) {
					/*
					 * On first difference we create temprorary page in memory
					 * and copies content in to it.
					 */
					tmppage=GinPageGetCopyPage ( origpage );

					if ( newN > 0 ) { 
						Size	pos = ((char*)GinGetPosting(itup)) - ((char*)origpage);
						memcpy( tmppage+pos, cleaned, sizeof(ItemPointerData)*newN );
					}

					pfree( cleaned );

					/* set itup pointer to new page */
					itup = (IndexTuple) PageGetItem(tmppage, PageGetItemId(tmppage, i));
				}

				value = index_getattr(itup, FirstOffsetNumber, gvs->ginstate.tupdesc, &isnull);
				itup = GinFormTuple(&gvs->ginstate, value, GinGetPosting(itup), newN);
				PageIndexTupleDelete(tmppage, i);

				if ( PageAddItem( tmppage, (Item)itup, IndexTupleSize(itup), i, LP_USED ) != i )
						elog(ERROR, "failed to add item to index page in \"%s\"",
								RelationGetRelationName(gvs->index));

				pfree( itup );
			}
		}
	}

	return 	( tmppage==origpage ) ? NULL : tmppage;
}

Datum
ginbulkdelete(PG_FUNCTION_ARGS) {
	Relation    index = (Relation) PG_GETARG_POINTER(0);
	IndexBulkDeleteCallback callback = (IndexBulkDeleteCallback) PG_GETARG_POINTER(1);
	void       *callback_state = (void *) PG_GETARG_POINTER(2);
	BlockNumber	blkno = GIN_ROOT_BLKNO;
	GinVacuumState	gvs;
	Buffer 		buffer;
	BlockNumber	rootOfPostingTree[ BLCKSZ/ (sizeof(IndexTupleData)+sizeof(ItemId)) ];
	uint32 nRoot;

	gvs.index = index;
	gvs.result = (IndexBulkDeleteResult *) palloc0(sizeof(IndexBulkDeleteResult));
	gvs.callback = callback;
	gvs.callback_state = callback_state;
	initGinState(&gvs.ginstate, index);

	buffer = ReadBuffer( index, blkno );

	/* find leaf page */
	for(;;) {
		Page page = BufferGetPage( buffer );
    	IndexTuple  itup;

		LockBuffer(buffer,GIN_SHARE);

		Assert( !GinPageIsData(page) );

		if ( GinPageIsLeaf(page) ) {
			LockBuffer(buffer,GIN_UNLOCK);
			LockBuffer(buffer,GIN_EXCLUSIVE);

			if ( blkno==GIN_ROOT_BLKNO && !GinPageIsLeaf(page) ) {
				LockBuffer(buffer,GIN_UNLOCK);
				continue; /* check it one more */
			}
			break;	
		}

		Assert( PageGetMaxOffsetNumber(page) >= FirstOffsetNumber );

		itup = (IndexTuple) PageGetItem(page, PageGetItemId(page, FirstOffsetNumber));
		blkno = GinItemPointerGetBlockNumber(&(itup)->t_tid);
		Assert( blkno!= InvalidBlockNumber );

		LockBuffer(buffer,GIN_UNLOCK);
		buffer = ReleaseAndReadBuffer( buffer, index, blkno );
	}

	/* right now we found leftmost page in entry's BTree */

	for(;;) {
		Page	page = BufferGetPage( buffer );
		Page	resPage;
		uint32 	i;

		Assert( !GinPageIsData(page) );

		resPage = ginVacuumEntryPage(&gvs, buffer, rootOfPostingTree, &nRoot);

		blkno = GinPageGetOpaque( page )->rightlink;

		if ( resPage ) {
			START_CRIT_SECTION();
			PageRestoreTempPage( resPage, page );
			xlogVacuumPage(gvs.index,  buffer);
			MarkBufferDirty( buffer );
			UnlockReleaseBuffer(buffer);
			END_CRIT_SECTION();
		} else {
			UnlockReleaseBuffer(buffer);
		}

		vacuum_delay_point();

		for(i=0; i<nRoot; i++) { 
			ginVacuumPostingTree( &gvs, rootOfPostingTree[i] );
			vacuum_delay_point();
		}

		if ( blkno==InvalidBlockNumber ) /*rightmost page*/
			break;

		buffer = ReadBuffer( index, blkno );
		LockBuffer(buffer,GIN_EXCLUSIVE);
	}

	PG_RETURN_POINTER(gvs.result);
}

Datum 
ginvacuumcleanup(PG_FUNCTION_ARGS) {
	Relation    index = (Relation) PG_GETARG_POINTER(0);
	IndexVacuumCleanupInfo *info = (IndexVacuumCleanupInfo *) PG_GETARG_POINTER(1);
	IndexBulkDeleteResult *stats = (IndexBulkDeleteResult *) PG_GETARG_POINTER(2);
	bool	 needLock = !RELATION_IS_LOCAL(index);
    BlockNumber npages,
				blkno;
	BlockNumber nFreePages,
			   *freePages,
			   maxFreePages;
	BlockNumber lastBlock = GIN_ROOT_BLKNO,
				   lastFilledBlock = GIN_ROOT_BLKNO;


	if (info->vacuum_full) {
		LockRelation(index, AccessExclusiveLock);
		needLock = false;
	}

	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);
	npages = RelationGetNumberOfBlocks(index);
	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	maxFreePages = npages;
	if (maxFreePages > MaxFSMPages)
		maxFreePages = MaxFSMPages;

	nFreePages = 0;
	freePages = (BlockNumber *) palloc(sizeof(BlockNumber) * maxFreePages);

	for (blkno = GIN_ROOT_BLKNO + 1; blkno < npages; blkno++) {
		Buffer buffer;
		Page   page;

		vacuum_delay_point();
	
		buffer = ReadBuffer(index, blkno);
		LockBuffer(buffer, GIN_SHARE);
		page = (Page) BufferGetPage(buffer);

		if ( GinPageIsDeleted(page) ) {
			if (nFreePages < maxFreePages)
				freePages[nFreePages++] = blkno;
		} else
			lastFilledBlock = blkno;

		UnlockReleaseBuffer(buffer);
	}
	lastBlock = npages - 1;

	if (info->vacuum_full && nFreePages > 0) {
		/* try to truncate index */
		int         i;
		for (i = 0; i < nFreePages; i++) 
			if (freePages[i] >= lastFilledBlock) {
				nFreePages = i;
				break;
			}

		if (lastBlock > lastFilledBlock)
			RelationTruncate(index, lastFilledBlock + 1);

		stats->pages_removed = lastBlock - lastFilledBlock;
	}

	RecordIndexFreeSpace(&index->rd_node, nFreePages, freePages);
	stats->pages_free = nFreePages;

	if (needLock)
		LockRelationForExtension(index, ExclusiveLock);
	stats->num_pages = RelationGetNumberOfBlocks(index);
	if (needLock)
		UnlockRelationForExtension(index, ExclusiveLock);

	if (info->vacuum_full)
		UnlockRelation(index, AccessExclusiveLock);

	PG_RETURN_POINTER(stats);
}

