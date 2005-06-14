/*-------------------------------------------------------------------------
 *
 * gistxlog.c
 *	  WAL replay logic for GiST.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *           $PostgreSQL: pgsql/src/backend/access/gist/gistxlog.c,v 1.1 2005/06/14 11:45:13 teodor Exp $
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/gist_private.h"
#include "access/gistscan.h"
#include "access/heapam.h"
#include "catalog/index.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "utils/memutils.h"

typedef struct {
	gistxlogEntryUpdate	*data;
	int			len;
	IndexTuple		*itup;
	BlockNumber		*path;
} EntryUpdateRecord;

typedef struct {
	gistxlogPage	*header;
	OffsetNumber	*offnum;

	/* to work with */
	Page	page;
	Buffer	buffer;
	bool	is_ok;
} NewPage;

typedef struct {
	gistxlogPageSplit	*data;
	NewPage			*page;
	IndexTuple		*itup;
	BlockNumber		*path;
} PageSplitRecord;

/* track for incomplete inserts, idea was taken from nbtxlog.c */

typedef struct gistIncompleteInsert {
	RelFileNode	node;
	ItemPointerData	key;
	int		lenblk;
	BlockNumber	*blkno;
	int		pathlen;
	BlockNumber	*path;
} gistIncompleteInsert;


MemoryContext opCtx;
MemoryContext insertCtx;
static List *incomplete_inserts;


#define ItemPointerEQ( a, b )	\
	( \
	ItemPointerGetOffsetNumber(a) == ItemPointerGetOffsetNumber(a) && \
	ItemPointerGetBlockNumber (a) == ItemPointerGetBlockNumber(b) \
        )

static void
pushIncompleteInsert(RelFileNode node, ItemPointerData key,
		BlockNumber *blkno, int lenblk,
		BlockNumber *path,  int pathlen,
		PageSplitRecord *xlinfo /* to extract blkno info */ ) {
	MemoryContext oldCxt = MemoryContextSwitchTo(insertCtx);
	gistIncompleteInsert *ninsert = (gistIncompleteInsert*)palloc( sizeof(gistIncompleteInsert) );

	ninsert->node = node;
	ninsert->key  = key;

	if ( lenblk && blkno ) {	
		ninsert->lenblk = lenblk;
		ninsert->blkno = (BlockNumber*)palloc( sizeof(BlockNumber)*ninsert->lenblk );
		memcpy(ninsert->blkno, blkno, sizeof(BlockNumber)*ninsert->lenblk);
	} else {
		int i;

		Assert( xlinfo );
		ninsert->lenblk = xlinfo->data->npage;
		ninsert->blkno = (BlockNumber*)palloc( sizeof(BlockNumber)*ninsert->lenblk );
		for(i=0;i<ninsert->lenblk;i++)
			ninsert->blkno[i] = xlinfo->page[i].header->blkno;
	}
	Assert( ninsert->lenblk>0 );
	
	if ( path && ninsert->pathlen ) {
		ninsert->pathlen = pathlen;
		ninsert->path = (BlockNumber*)palloc( sizeof(BlockNumber)*ninsert->pathlen );
		memcpy(ninsert->path, path, sizeof(BlockNumber)*ninsert->pathlen);
	} else { 
		ninsert->pathlen = 0;
		ninsert->path = NULL;
	}

	incomplete_inserts = lappend(incomplete_inserts, ninsert);
	MemoryContextSwitchTo(oldCxt);
}

static void
forgetIncompleteInsert(RelFileNode node, ItemPointerData key) {
	ListCell   *l;

	foreach(l, incomplete_inserts) {
		gistIncompleteInsert	*insert = (gistIncompleteInsert*) lfirst(l);

		if (  RelFileNodeEquals(node, insert->node) && ItemPointerEQ( &(insert->key), &(key) ) ) {
			
			/* found */
			if ( insert->path ) pfree( insert->path );
			pfree( insert->blkno );
			incomplete_inserts = list_delete_ptr(incomplete_inserts, insert);
			pfree( insert );
			break;
		} 
	}
}

static void
decodeEntryUpdateRecord(EntryUpdateRecord *decoded, XLogRecord *record) {
	char *begin = XLogRecGetData(record), *ptr;
	int i=0, addpath=0;

	decoded->data = (gistxlogEntryUpdate*)begin;

	if ( decoded->data->pathlen ) {
		addpath = sizeof(BlockNumber) * decoded->data->pathlen;
		decoded->path = (BlockNumber*)(begin+sizeof( gistxlogEntryUpdate ));
	} else 
		decoded->path = NULL;

	decoded->len=0;
	ptr=begin+sizeof( gistxlogEntryUpdate ) + addpath;
	while( ptr - begin < record->xl_len ) {
		decoded->len++;
		ptr += IndexTupleSize( (IndexTuple)ptr );
	}  

	decoded->itup=(IndexTuple*)palloc( sizeof( IndexTuple ) * decoded->len );
	
	ptr=begin+sizeof( gistxlogEntryUpdate ) + addpath;
	while( ptr - begin < record->xl_len ) {
		decoded->itup[i] = (IndexTuple)ptr;
		ptr += IndexTupleSize( decoded->itup[i] );
		i++;
	}
}


static void
gistRedoEntryUpdateRecord(XLogRecPtr lsn, XLogRecord *record, bool isnewroot) {
	EntryUpdateRecord	xlrec;
	Relation        reln;
	Buffer          buffer;
	Page            page;

	decodeEntryUpdateRecord( &xlrec, record );

	reln = XLogOpenRelation(xlrec.data->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer(false, reln, xlrec.data->blkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "gistRedoEntryUpdateRecord: block unfound");
	page = (Page) BufferGetPage(buffer);

	if ( isnewroot ) {
		if ( !PageIsNew((PageHeader) page) && XLByteLE(lsn, PageGetLSN(page)) ) {
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			return;
		}
	} else { 
		if ( PageIsNew((PageHeader) page) )
			elog(PANIC, "gistRedoEntryUpdateRecord: uninitialized page");
		if (XLByteLE(lsn, PageGetLSN(page))) {
			LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
			ReleaseBuffer(buffer);
			return;
		}
	}

	if ( isnewroot )
		GISTInitBuffer(buffer, 0);
	else if ( xlrec.data->todeleteoffnum != InvalidOffsetNumber ) 
		PageIndexTupleDelete(page, xlrec.data->todeleteoffnum);

	/* add tuples */
	if ( xlrec.len > 0 ) {
                OffsetNumber off = (PageIsEmpty(page)) ?  
                        FirstOffsetNumber
                        :
                        OffsetNumberNext(PageGetMaxOffsetNumber(page));

		gistfillbuffer(reln, page, xlrec.itup, xlrec.len, off);
	}

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);

	if ( ItemPointerIsValid( &(xlrec.data->key) ) ) {
		if ( incomplete_inserts != NIL )
			forgetIncompleteInsert(xlrec.data->node, xlrec.data->key);

		if ( !isnewroot && xlrec.data->blkno!=GIST_ROOT_BLKNO )
			pushIncompleteInsert(xlrec.data->node, xlrec.data->key, 
				&(xlrec.data->blkno), 1,
				xlrec.path, xlrec.data->pathlen,
				NULL);
	}
}

static void
decodePageSplitRecord(PageSplitRecord *decoded, XLogRecord *record) {
	char *begin = XLogRecGetData(record), *ptr;
	int i=0, addpath = 0;

	decoded->data = (gistxlogPageSplit*)begin;
	decoded->page = (NewPage*)palloc( sizeof(NewPage) * decoded->data->npage );
	decoded->itup = (IndexTuple*)palloc( sizeof(IndexTuple) * decoded->data->nitup );

	if ( decoded->data->pathlen ) {
		addpath = sizeof(BlockNumber) * decoded->data->pathlen;
		decoded->path = (BlockNumber*)(begin+sizeof( gistxlogEntryUpdate ));
	} else 
		decoded->path = NULL;

	ptr=begin+sizeof( gistxlogPageSplit ) + addpath;
	for(i=0;i<decoded->data->nitup;i++) {
		Assert( ptr - begin < record->xl_len );
		decoded->itup[i] = (IndexTuple)ptr;
		ptr += IndexTupleSize( decoded->itup[i] );
	}

	for(i=0;i<decoded->data->npage;i++) {
		Assert( ptr - begin < record->xl_len );
		decoded->page[i].header = (gistxlogPage*)ptr;
		ptr += sizeof(gistxlogPage);

		Assert( ptr - begin < record->xl_len );
		decoded->page[i].offnum = (OffsetNumber*)ptr;
		ptr += MAXALIGN( sizeof(OffsetNumber) * decoded->page[i].header->num );
	}
}
	
static void
gistRedoPageSplitRecord(XLogRecPtr lsn, XLogRecord *record ) {
	PageSplitRecord	xlrec;
	Relation        reln;
	Buffer          buffer;
	Page            page;
	int 		i, len=0;
	IndexTuple	*itup, *institup;
	GISTPageOpaque opaque;
	bool release=true;

	decodePageSplitRecord( &xlrec, record );

	reln = XLogOpenRelation(xlrec.data->node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer( false, reln, xlrec.data->origblkno);
	if (!BufferIsValid(buffer))
		elog(PANIC, "gistRedoEntryUpdateRecord: block unfound");
	page = (Page) BufferGetPage(buffer);
	if (PageIsNew((PageHeader) page))
		elog(PANIC, "gistRedoEntryUpdateRecord: uninitialized page");

	if (XLByteLE(lsn, PageGetLSN(page))) {
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}
	
	if ( xlrec.data->todeleteoffnum != InvalidOffsetNumber )
		PageIndexTupleDelete(page, xlrec.data->todeleteoffnum);

	itup = gistextractbuffer(buffer, &len);
	itup = gistjoinvector(itup, &len, xlrec.itup, xlrec.data->nitup);
	institup = (IndexTuple*)palloc( sizeof(IndexTuple) * len );
        opaque = (GISTPageOpaque) PageGetSpecialPointer(page);

	for(i=0;i<xlrec.data->npage;i++) {
		int j;
		NewPage *newpage = xlrec.page + i; 

		/* prepare itup vector */
		for(j=0;j<newpage->header->num;j++)
			institup[j] = itup[ newpage->offnum[j] - 1 ];

		if ( newpage->header->blkno == xlrec.data->origblkno ) {
			/* IncrBufferRefCount(buffer); */
			newpage->page = (Page) PageGetTempPage(page, sizeof(GISTPageOpaqueData));
			newpage->buffer = buffer;
			newpage->is_ok=false; 
		} else {
			newpage->buffer = XLogReadBuffer(true, reln, newpage->header->blkno);
			if (!BufferIsValid(newpage->buffer))
				elog(PANIC, "gistRedoPageSplitRecord: lost page");
			newpage->page = (Page) BufferGetPage(newpage->buffer);
			if (!PageIsNew((PageHeader) page) && XLByteLE(lsn, PageGetLSN(newpage->page))) {
				LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
				ReleaseBuffer(buffer);
				newpage->is_ok=true;
				continue; /* good page */
			} else {
				newpage->is_ok=false;
				GISTInitBuffer(newpage->buffer, opaque->flags & F_LEAF);
			}
		}
		gistfillbuffer(reln, newpage->page, institup, newpage->header->num, FirstOffsetNumber);
	}

	for(i=0;i<xlrec.data->npage;i++) {
		NewPage *newpage = xlrec.page + i;

		if ( newpage->is_ok )
			continue;

		if ( newpage->header->blkno == xlrec.data->origblkno ) { 
			PageRestoreTempPage(newpage->page, page);
			release = false;
		}

		PageSetLSN(newpage->page, lsn);
		PageSetTLI(newpage->page, ThisTimeLineID);
		LockBuffer(newpage->buffer, BUFFER_LOCK_UNLOCK);
		WriteBuffer(newpage->buffer);	
	}

	if ( release ) {
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	if ( ItemPointerIsValid( &(xlrec.data->key) ) ) {
		if ( incomplete_inserts != NIL )
			forgetIncompleteInsert(xlrec.data->node, xlrec.data->key);

		pushIncompleteInsert(xlrec.data->node, xlrec.data->key, 
				NULL, 0,
				xlrec.path, xlrec.data->pathlen,
				&xlrec);
	}
}

static void
gistRedoCreateIndex(XLogRecPtr lsn, XLogRecord *record) {
	RelFileNode	*node = (RelFileNode*)XLogRecGetData(record);
	Relation        reln;
	Buffer          buffer;
	Page            page;

	reln = XLogOpenRelation(*node);
	if (!RelationIsValid(reln))
		return;
	buffer = XLogReadBuffer( true, reln, GIST_ROOT_BLKNO);
	if (!BufferIsValid(buffer))
		elog(PANIC, "gistRedoCreateIndex: block unfound");
	page = (Page) BufferGetPage(buffer);

	if (!PageIsNew((PageHeader) page) && XLByteLE(lsn, PageGetLSN(page))) {
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
		return;
	}

	GISTInitBuffer(buffer, F_LEAF);

	PageSetLSN(page, lsn);
	PageSetTLI(page, ThisTimeLineID);
	LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
	WriteBuffer(buffer);	
}

void
gist_redo(XLogRecPtr lsn, XLogRecord *record)
{
	uint8           info = record->xl_info & ~XLR_INFO_MASK;

	MemoryContext oldCxt;
	oldCxt = MemoryContextSwitchTo(opCtx);
	switch (info) {
		case	XLOG_GIST_ENTRY_UPDATE:
		case	XLOG_GIST_ENTRY_DELETE:
			gistRedoEntryUpdateRecord(lsn, record,false);
			break;
		case	XLOG_GIST_NEW_ROOT:
			gistRedoEntryUpdateRecord(lsn, record,true);
			break;
		case	XLOG_GIST_PAGE_SPLIT:
			gistRedoPageSplitRecord(lsn, record);	
			break;
		case	XLOG_GIST_CREATE_INDEX:
			gistRedoCreateIndex(lsn, record);
			break;
		case	XLOG_GIST_INSERT_COMPLETE:
			forgetIncompleteInsert( ((gistxlogInsertComplete*)XLogRecGetData(record))->node, 
				((gistxlogInsertComplete*)XLogRecGetData(record))->key );
			break;
		default:
			elog(PANIC, "gist_redo: unknown op code %u", info);
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}

static void
out_target(char *buf, RelFileNode node, ItemPointerData key)
{
        sprintf(buf + strlen(buf), "rel %u/%u/%u; tid %u/%u",
                 node.spcNode, node.dbNode, node.relNode,
                        ItemPointerGetBlockNumber(&key),
                        ItemPointerGetOffsetNumber(&key));
}

static void
out_gistxlogEntryUpdate(char *buf, gistxlogEntryUpdate *xlrec) {
	out_target(buf, xlrec->node, xlrec->key);
	sprintf(buf + strlen(buf), "; block number %u; update offset %u;", 
		xlrec->blkno, xlrec->todeleteoffnum);
}

static void
out_gistxlogPageSplit(char *buf, gistxlogPageSplit *xlrec) {
	strcat(buf, "page_split: ");
	out_target(buf, xlrec->node, xlrec->key);
	sprintf(buf + strlen(buf), "; block number %u; update offset %u; add %d tuples; split to %d pages", 
		xlrec->origblkno, xlrec->todeleteoffnum,
		xlrec->nitup, xlrec->npage);
}

void
gist_desc(char *buf, uint8 xl_info, char *rec)
{
	uint8           info = xl_info & ~XLR_INFO_MASK;

	switch (info) {
		case	XLOG_GIST_ENTRY_UPDATE:
			strcat(buf, "entry_update: ");
			out_gistxlogEntryUpdate(buf, (gistxlogEntryUpdate*)rec);
			break;
		case	XLOG_GIST_ENTRY_DELETE:
			strcat(buf, "entry_delete: ");
			out_gistxlogEntryUpdate(buf, (gistxlogEntryUpdate*)rec);
			break;
		case	XLOG_GIST_NEW_ROOT:
			strcat(buf, "new_root: ");
			out_target(buf, ((gistxlogEntryUpdate*)rec)->node, ((gistxlogEntryUpdate*)rec)->key);
			break;
		case	XLOG_GIST_PAGE_SPLIT:
			out_gistxlogPageSplit(buf, (gistxlogPageSplit*)rec);
			break;
		case	XLOG_GIST_CREATE_INDEX:
			sprintf(buf + strlen(buf), "create_index: rel %u/%u/%u", 
				((RelFileNode*)rec)->spcNode, 
				((RelFileNode*)rec)->dbNode, 
				((RelFileNode*)rec)->relNode);
			break;
		case	XLOG_GIST_INSERT_COMPLETE:
			strcat(buf, "insert_complete: ");
			out_target(buf, ((gistxlogInsertComplete*)rec)->node, ((gistxlogInsertComplete*)rec)->key); 
		default:
			elog(PANIC, "gist_desc: unknown op code %u", info);
	}
}


#ifdef GIST_INCOMPLETE_INSERT 
static void
gistContinueInsert(gistIncompleteInsert *insert) {
	GISTSTATE	giststate;
	GISTInsertState	state;
	int i;
	MemoryContext oldCxt;
	oldCxt = MemoryContextSwitchTo(opCtx);
	
	state.r = XLogOpenRelation(insert->node);
	if (!RelationIsValid(state.r))
		return;

	initGISTstate(&giststate, state.r);

	state.needInsertComplete=false;
	ItemPointerSetInvalid( &(state.key) );
	state.path=NULL;
	state.pathlen=0;
	state.xlog_mode = true;

	/* form union tuples */
	state.itup = (IndexTuple*)palloc(sizeof(IndexTuple)*insert->lenblk);
	state.ituplen = insert->lenblk; 
	for(i=0;i<insert->lenblk;i++) {
		int len=0;
		IndexTuple *itup;
		Buffer	buffer;
		Page	page;

		buffer = XLogReadBuffer(false, state.r, insert->blkno[i]);
		if (!BufferIsValid(buffer))
			elog(PANIC, "gistContinueInsert: block unfound");
		page = (Page) BufferGetPage(buffer);
		if ( PageIsNew((PageHeader)page) )
			elog(PANIC, "gistContinueInsert: uninitialized page");

		itup = gistextractbuffer(buffer, &len);
		state.itup[i] = gistunion(state.r, itup, len, &giststate);

		ItemPointerSet( &(state.itup[i]->t_tid), insert->blkno[i], FirstOffsetNumber );
		
		LockBuffer(buffer, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(buffer);
	}

	if ( insert->pathlen==0 ) { 
		/*it  was split root, so we should only make new root*/
		gistnewroot(state.r, state.itup, state.ituplen, &(state.key), true);
		MemoryContextSwitchTo(oldCxt);
		MemoryContextReset(opCtx);
		return;
	}

	/* form stack */
	state.stack=NULL;
	for(i=0;i<insert->pathlen;i++) {
		int j,len=0;
		IndexTuple *itup;
		GISTInsertStack *top = (GISTInsertStack*)palloc( sizeof(GISTInsertStack) );

		top->blkno = insert->path[i];
		top->buffer = XLogReadBuffer(false, state.r, top->blkno);
		if (!BufferIsValid(top->buffer))
			elog(PANIC, "gistContinueInsert: block unfound");
		top->page = (Page) BufferGetPage(top->buffer);
		if ( PageIsNew((PageHeader)(top->page)) )
			elog(PANIC, "gistContinueInsert: uninitialized page");

		top->todelete = false;	

		/* find childoffnum */
		itup = gistextractbuffer(top->buffer, &len);
		top->childoffnum=InvalidOffsetNumber;
		for(j=0;j<len && top->childoffnum==InvalidOffsetNumber;j++) {
			BlockNumber blkno = ItemPointerGetBlockNumber( &(itup[j]->t_tid) ); 
			
			if ( i==0 ) {
				int k; 
				for(k=0;k<insert->lenblk;k++)
					if ( insert->blkno[k] == blkno ) {
						top->childoffnum = j+1;
						break;
					}
			} else if ( insert->path[i-1]==blkno )
					top->childoffnum = j+1;
		}

		if ( top->childoffnum==InvalidOffsetNumber ) {
			elog(WARNING, "gistContinueInsert: unknown parent, REINDEX GiST Indexes");
			return;
		}

		if ( i==0 ) 
			PageIndexTupleDelete(top->page, top->childoffnum);
			
		/* install item on right place in stack */
		top->parent=NULL;
		if ( state.stack ) {
			GISTInsertStack *ptr = state.stack;
			while( ptr->parent )
				ptr = ptr->parent;
			ptr->parent=top;
		} else
			state.stack = top;
	}

	/* Good. Now we can continue insert */

	gistmakedeal(&state, &giststate);

	MemoryContextSwitchTo(oldCxt);
	MemoryContextReset(opCtx);
}
#endif

void
gist_xlog_startup(void) {
	incomplete_inserts=NIL;
	insertCtx = AllocSetContextCreate(CurrentMemoryContext,
		"GiST insert in xlog  temporary context",	
                                 ALLOCSET_DEFAULT_MINSIZE,
                                 ALLOCSET_DEFAULT_INITSIZE,
                                 ALLOCSET_DEFAULT_MAXSIZE);
	opCtx = createTempGistContext();
}

void
gist_xlog_cleanup(void) {
	ListCell   *l;

	foreach(l, incomplete_inserts) {
		gistIncompleteInsert	*insert = (gistIncompleteInsert*) lfirst(l);
		char buf[1024];

		*buf='\0';
		out_target(buf, insert->node, insert->key);
		elog(LOG,"Incomplete insert: %s; It's needed to reindex", buf);
#ifdef GIST_INCOMPLETE_INSERT 
		gistContinueInsert(insert);
#endif
	}
	MemoryContextDelete(opCtx);
	MemoryContextDelete(insertCtx);	
}

