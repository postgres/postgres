/*-------------------------------------------------------------------------
 * btsort.c--
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Id: nbtsort.c,v 1.3 1996/10/18 05:21:20 scrappy Exp $
 *
 * NOTES
 *
 * what we do is:
 * - generate a set of initial one-block runs, distributed round-robin
 *   between the output tapes.
 * - for each pass,
 *   - swap input and output tape sets, rewinding both and truncating
 *     the output tapes.
 *   - merge the current run in each input tape to the current output
 *     tape.
 *     - when each input run has been exhausted, switch to another output
 *       tape and start processing another run.
 * - when we have fewer runs than tapes, we know we are ready to start
 *   merging into the btree leaf pages. 
 * - every time we complete a level of the btree, we can construct the
 *   next level up.  when we have only one page on a level, it can be
 *   attached to the btree metapage and we are done.
 *
 * conventions:
 * - external interface routines take in and return "void *" for their
 *   opaque handles.  this is for modularity reasons (i prefer not to
 *   export these structures without good reason).
 *
 * this code is moderately slow (~10% slower) compared to the regular
 * btree (insertion) build code on sorted or well-clustered data.  on
 * random data, however, the insertion build code is unusable -- the
 * difference on a 60MB heap is a factor of 15 because the random
 * probes into the btree thrash the buffer pool.
 *
 * this code currently packs the pages to 100% of capacity.  this is
 * not wise, since *any* insertion will cause splitting.  filling to
 * something like the standard 70% steady-state load factor for btrees
 * would probably be better.
 *
 * somebody desperately needs to figure out how to do a better job of
 * balancing the merge passes -- the fan-in on the final merges can be
 * pretty poor, which is bad for performance.
 *-------------------------------------------------------------------------
 */

#include <stdio.h>

#include "c.h"

#include "access/nbtree.h"

#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "utils/rel.h"
#include "utils/palloc.h"
#include "utils/elog.h"

#ifdef FASTBUILD

#define	MAXTAPES	(7)
#define	TAPEBLCKSZ	(BLCKSZ << 2)
#define	TAPETEMP	"pg_btsortXXXXXX"


/*-------------------------------------------------------------------------
 * sorting comparison routine - returns {-1,0,1} depending on whether
 * the key in the left BTItem is {<,=,>} the key in the right BTItem.
 *
 * we want to use _bt_isortcmp as a comparison function for qsort(3),
 * but it needs extra arguments, so we "pass them in" as global
 * variables.  ick.  fortunately, they are the same throughout the
 * build, so we need do this only once.  this is why you must call
 * _bt_isortcmpinit before the call to qsort(3).
 *
 * a NULL BTItem is always assumed to be greater than any actual
 * value; our heap routines (see below) assume that the smallest
 * element in the heap is returned.  that way, NULL values from the
 * exhausted tapes can sift down to the bottom of the heap.  in point
 * of fact we just don't replace the elements of exhausted tapes, but
 * what the heck.
 * *-------------------------------------------------------------------------
 */
static Relation _bt_sortrel;

static void
_bt_isortcmpinit(Relation index)
{
    _bt_sortrel = index;
}

static int
_bt_isortcmp(const void *bti1p,const void *bti2p)
{
    BTItem bti1 = *(BTItem *)bti1p;
    BTItem bti2 = *(BTItem *)bti2p;

    if (bti1 == (BTItem) NULL) {
	if (bti2 == (BTItem) NULL) {
	    return(0);	/* 1 = 2 */
	}
	return(1);	/* 1 > 2 */
    } else if (bti2 == (BTItem) NULL) {
	return(-1);	/* 1 < 2 */
    } else if (_bt_itemcmp(_bt_sortrel, 1, bti1, bti2,
			   BTGreaterStrategyNumber)) {
	return(1);	/* 1 > 2 */
    } else if (_bt_itemcmp(_bt_sortrel, 1, bti2, bti1,
			   BTGreaterStrategyNumber)) {
	return(-1);	/* 1 < 2 */
    }
    return(0);		/* 1 = 2 */
}

/*-------------------------------------------------------------------------
 * priority queue methods
 *
 * these were more-or-less lifted from the heap section of the 1984
 * edition of gonnet's book on algorithms and data structures.  they
 * are coded so that the smallest element in the heap is returned (we
 * use them for merging sorted runs).
 *
 * XXX these probably ought to be generic library functions.
 *-------------------------------------------------------------------------
 */

typedef struct {
    int		btpqe_tape;	/* tape identifier */
    BTItem	btpqe_item;	/* pointer to BTItem in tape buffer */
} BTPriQueueElem;

#define	MAXELEM	MAXTAPES
typedef struct {
    int			btpq_nelem;
    BTPriQueueElem	btpq_queue[MAXELEM];
    Relation		btpq_rel;
} BTPriQueue;

/* be sure to call _bt_isortcmpinit first */
#define GREATER(a, b) \
    (_bt_isortcmp(&((a)->btpqe_item), &((b)->btpqe_item)) > 0)

static void
_bt_pqsift(BTPriQueue *q, int parent)
{
    int child;
    BTPriQueueElem e;

    for (child = parent * 2 + 1;
	 child < q->btpq_nelem;
	 child = parent * 2 + 1) {
	if (child < q->btpq_nelem - 1) {
	    if (GREATER(&(q->btpq_queue[child]), &(q->btpq_queue[child+1]))) {
		++child;
	    }
	}
	if (GREATER(&(q->btpq_queue[parent]), &(q->btpq_queue[child]))) {
	    e = q->btpq_queue[child];				/* struct = */
	    q->btpq_queue[child] = q->btpq_queue[parent];	/* struct = */
	    q->btpq_queue[parent] = e;				/* struct = */
	    parent = child;
	} else {
	    parent = child + 1;
	}
    }
}

static int
_bt_pqnext(BTPriQueue *q, BTPriQueueElem *e)
{
    if (q->btpq_nelem < 1) {	/* already empty */
	return(-1);
    }
    *e = q->btpq_queue[0];					/* struct = */

    if (--q->btpq_nelem < 1) {	/* now empty, don't sift */
	return(0);
    }
    q->btpq_queue[0] = q->btpq_queue[q->btpq_nelem];		/* struct = */
    _bt_pqsift(q, 0);
    return(0);
}

static void
_bt_pqadd(BTPriQueue *q, BTPriQueueElem *e)
{
    int child, parent;

    if (q->btpq_nelem >= MAXELEM) {
	elog(WARN, "_bt_pqadd: queue overflow");
    }

    child = q->btpq_nelem++;
    while (child > 0) {
	parent = child / 2;
	if (GREATER(e, &(q->btpq_queue[parent]))) {
	    break;
	} else {
	    q->btpq_queue[child] = q->btpq_queue[parent];	/* struct = */
	    child = parent;
	}
    }

    q->btpq_queue[child] = *e;					/* struct = */
}

/*-------------------------------------------------------------------------
 * tape methods
 *-------------------------------------------------------------------------
 */

#define	BTITEMSZ(btitem) \
    ((btitem) ? \
     (IndexTupleDSize((btitem)->bti_itup) + \
      (sizeof(BTItemData) - sizeof(IndexTupleData))) : \
     0)
#define	SPCLEFT(tape) \
    (sizeof((tape)->bttb_data) - (tape)->bttb_top)
#define	EMPTYTAPE(tape) \
    ((tape)->bttb_ntup <= 0)
#define	BTTAPEMAGIC	0x19660226

/*
 * this is what we use to shovel BTItems in and out of memory.  it's
 * bigger than a standard block because we are doing a lot of strictly
 * sequential i/o.  this is obviously something of a tradeoff since we
 * are potentially reading a bunch of zeroes off of disk in many
 * cases.
 *
 * BTItems are packed in and DOUBLEALIGN'd.
 *
 * the fd should not be going out to disk, strictly speaking, but it's
 * the only thing like that so i'm not going to worry about wasting a
 * few bytes.
 */
typedef struct {
    int		bttb_magic;	/* magic number */
    int		bttb_fd;	/* file descriptor */
    int		bttb_top;	/* top of free space within bttb_data */
    short	bttb_ntup;	/* number of tuples in this block */
    short	bttb_eor;	/* End-Of-Run marker */
    char	bttb_data[TAPEBLCKSZ - 2 * sizeof(double)];
} BTTapeBlock;


/*
 * reset the tape header for its next use without doing anything to
 * the physical tape file.  (setting bttb_top to 0 makes the block
 * empty.)
 */
static void
_bt_tapereset(BTTapeBlock *tape)
{
    tape->bttb_eor = 0;
    tape->bttb_top = 0;
    tape->bttb_ntup = 0;
}

/*
 * rewind the physical tape file.
 */
static void
_bt_taperewind(BTTapeBlock *tape)
{
    (void) FileSeek(tape->bttb_fd, 0, SEEK_SET);
}

/*
 * destroy the contents of the physical tape file without destroying
 * the tape data structure or removing the physical tape file.
 *
 * we use the VFD version of ftruncate(2) to do this rather than
 * unlinking and recreating the file.  you still have to wait while
 * the OS frees up all of the file system blocks and stuff, but at
 * least you don't have to delete and reinsert the directory entries.
 */
static void
_bt_tapeclear(BTTapeBlock *tape)
{
    /* blow away the contents of the old file */
    _bt_taperewind(tape);
#if 0
    FileSync(tape->bttb_fd);
#endif
    FileTruncate(tape->bttb_fd, 0);

    /* reset the buffer */
    _bt_tapereset(tape);
}

/*
 * create a new BTTapeBlock, allocating memory for the data structure
 * as well as opening a physical tape file.
 */
static BTTapeBlock *
_bt_tapecreate(char *fname)
{
    BTTapeBlock *tape = (BTTapeBlock *) palloc(sizeof(BTTapeBlock));

    if (tape == (BTTapeBlock *) NULL) {
	elog(WARN, "_bt_tapecreate: out of memory");
    }

    tape->bttb_magic = BTTAPEMAGIC;

    tape->bttb_fd = FileNameOpenFile(fname, O_RDWR|O_CREAT|O_TRUNC, 0600);
    Assert(tape->bttb_fd >= 0);

    /* initialize the buffer */
    _bt_tapereset(tape);

    return(tape);
}

/*
 * destroy the BTTapeBlock structure and its physical tape file.
 */
static void
_bt_tapedestroy(BTTapeBlock *tape)
{
    FileUnlink(tape->bttb_fd);
    pfree((void *) tape);
}

/*
 * flush the tape block to the file, marking End-Of-Run if requested.
 */
static void
_bt_tapewrite(BTTapeBlock *tape, int eor)
{
    tape->bttb_eor = eor;
    FileWrite(tape->bttb_fd, (char*)tape, TAPEBLCKSZ);
    _bt_tapereset(tape);
}

/*
 * read a tape block from the file, overwriting the current contents
 * of the buffer.
 *
 * returns:
 * - 0 if there are no more blocks in the tape or in this run (call
 *   _bt_tapereset to clear the End-Of-Run marker)
 * - 1 if a valid block was read
 */
static int
_bt_taperead(BTTapeBlock *tape)
{
    int fd;
    int nread;

    if (tape->bttb_eor) {
	return(0);		/* we are at End-Of-Run */
    }

    /*
     * we're clobbering the old tape block, but we do need to save the
     * VFD (the one in the block we're reading is bogus).
     */
    fd = tape->bttb_fd;
    nread = FileRead(fd, (char*) tape, TAPEBLCKSZ);
    tape->bttb_fd = fd;

    if (nread != TAPEBLCKSZ) {
	Assert(nread == 0);	/* we are at EOF */
	return(0);
    }
    Assert(tape->bttb_magic == BTTAPEMAGIC);
    return(1);
}

/*
 * get the next BTItem from a tape block.
 *
 * returns:
 * - NULL if we have run out of BTItems
 * - a pointer to the BTItemData in the block otherwise
 *
 * side effects:
 * - sets 'pos' to the current position within the block.
 */
static BTItem
_bt_tapenext(BTTapeBlock *tape, char **pos)
{
    Size itemsz;
    BTItem bti;

    if (*pos >= tape->bttb_data + tape->bttb_top) {
	return((BTItem) NULL);
    }
    bti = (BTItem) *pos;
    itemsz = BTITEMSZ(bti);
    *pos += DOUBLEALIGN(itemsz);
    return(bti);
}

/*
 * copy a BTItem into a tape block.
 *
 * assumes that we have already checked to see if the block has enough
 * space for the item.
 *
 * side effects:
 *
 * - advances the 'top' pointer in the tape block header to point to
 * the beginning of free space.
 */
static void
_bt_tapeadd(BTTapeBlock *tape, BTItem item, int itemsz)
{
    (void) memcpy(tape->bttb_data + tape->bttb_top, item, itemsz);
    ++tape->bttb_ntup;
    tape->bttb_top += DOUBLEALIGN(itemsz);
}

/*-------------------------------------------------------------------------
 * spool methods
 *-------------------------------------------------------------------------
 */

/*
 * this structure holds the bookkeeping for a simple balanced multiway
 * merge.  (polyphase merging is hairier than i want to get into right
 * now, and i don't see why i have to care how many "tapes" i use
 * right now.  though if psort was in a condition that i could hack it
 * to do this, you bet i would.)
 */
typedef struct {
    int		bts_ntapes;
    int		bts_tape;
    BTTapeBlock	**bts_itape;	/* input tape blocks */
    BTTapeBlock	**bts_otape;	/* output tape blocks */
} BTSpool;

/*
 * create and initialize a spool structure, including the underlying
 * files.
 */
void *
_bt_spoolinit(Relation index, int ntapes)
{
    char *mktemp();

    BTSpool *btspool = (BTSpool *) palloc(sizeof(BTSpool));
    int i;
    char *fname = (char *) palloc(sizeof(TAPETEMP) + 1);

    if (btspool == (BTSpool *) NULL || fname == (char *) NULL) {
	elog(WARN, "_bt_spoolinit: out of memory");
    }
    (void) memset((char *) btspool, 0, sizeof(BTSpool));
    btspool->bts_ntapes = ntapes;
    btspool->bts_tape = 0;

    btspool->bts_itape =
	(BTTapeBlock **) palloc(sizeof(BTTapeBlock *) * ntapes);
    btspool->bts_otape =
	(BTTapeBlock **) palloc(sizeof(BTTapeBlock *) * ntapes);
    if (btspool->bts_itape == (BTTapeBlock **) NULL ||
	btspool->bts_otape == (BTTapeBlock **) NULL) {
	elog(WARN, "_bt_spoolinit: out of memory");
    }

    for (i = 0; i < ntapes; ++i) {
	btspool->bts_itape[i] =
	    _bt_tapecreate(mktemp(strcpy(fname, TAPETEMP)));
	btspool->bts_otape[i] =
	    _bt_tapecreate(mktemp(strcpy(fname, TAPETEMP)));
    }
    pfree((void *) fname);

    _bt_isortcmpinit(index);

    return((void *) btspool);
}

/*
 * clean up a spool structure and its substructures.
 */
void
_bt_spooldestroy(void *spool)
{
    BTSpool *btspool = (BTSpool *) spool;
    int i;

    for (i = 0; i < btspool->bts_ntapes; ++i) {
	_bt_tapedestroy(btspool->bts_otape[i]);
	_bt_tapedestroy(btspool->bts_itape[i]);
    }
    pfree((void *) btspool);
}

/*
 * flush out any dirty output tape blocks
 */
static void
_bt_spoolflush(BTSpool *btspool)
{
    int i;

    for (i = 0; i < btspool->bts_ntapes; ++i) {
	if (!EMPTYTAPE(btspool->bts_otape[i])) {
	    _bt_tapewrite(btspool->bts_otape[i], 1);
	}
    }
}

/*
 * swap input tapes and output tapes by swapping their file
 * descriptors.  additional preparation for the next merge pass
 * includes rewinding the new input tapes and clearing out the new
 * output tapes.
 */
static void
_bt_spoolswap(BTSpool *btspool)
{
    File tmpfd;
    BTTapeBlock *itape;
    BTTapeBlock *otape;
    int i;

    for (i = 0; i < btspool->bts_ntapes; ++i) {
	itape = btspool->bts_itape[i];
	otape = btspool->bts_otape[i];

	/*
	 * swap the input and output VFDs.
	 */
	tmpfd = itape->bttb_fd;
	itape->bttb_fd = otape->bttb_fd;
	otape->bttb_fd = tmpfd;

	/*
	 * rewind the new input tape.
	 */
	_bt_taperewind(itape);
	_bt_tapereset(itape);

	/*
	 * clear the new output tape -- it's ok to throw away the old
	 * inputs.
	 */
	_bt_tapeclear(otape);
    }    
}

/*-------------------------------------------------------------------------
 * sorting routines
 *-------------------------------------------------------------------------
 */

/*
 * spool 'btitem' into an initial run.  as tape blocks are filled, the
 * block BTItems are qsorted and written into some output tape (it
 * doesn't matter which; we go round-robin for simplicity).  the
 * initial runs are therefore always just one block.
 */
void
_bt_spool(Relation index, BTItem btitem, void *spool)
{
    BTSpool *btspool = (BTSpool *) spool;
    BTTapeBlock *itape;
    Size itemsz;

    itape = btspool->bts_itape[btspool->bts_tape];
    itemsz = BTITEMSZ(btitem);
    itemsz = DOUBLEALIGN(itemsz);

    /*
     * if this buffer is too full for this BTItemData, or if we have
     * run out of BTItems, we need to sort the buffer and write it
     * out.  in this case, the BTItemData will go into the next tape's
     * buffer.
     */
    if (btitem == (BTItem) NULL || SPCLEFT(itape) < itemsz) {
	BTItem *parray;
	BTTapeBlock *otape;
	BTItem bti;
	char *pos;
	int btisz;
	int i;

	/*
	 * build an array of pointers to the BTItemDatas on the input
	 * block.
	 */
	parray = (BTItem *) palloc(itape->bttb_ntup * sizeof(BTItem));
	if (parray == (BTItem *) NULL) {
	    elog(WARN, "_bt_spool: out of memory");
	}
	pos = itape->bttb_data;
	for (i = 0; i < itape->bttb_ntup; ++i) {
	    parray[i] = _bt_tapenext(itape, &pos);
	}

	/*
	 * qsort the pointer array.
	 */
	_bt_isortcmpinit(index);
	qsort((void *) parray, itape->bttb_ntup, sizeof(BTItem), _bt_isortcmp);

	/*
	 * write the spooled run into the output tape.  we copy the
	 * BTItemDatas in the order dictated by the sorted array of
	 * BTItems, not the original order.
	 *
	 * (since everything was DOUBLEALIGN'd and is all on a single
	 * page, everything had *better* still fit on one page..)
	 */
	otape = btspool->bts_otape[btspool->bts_tape];
	for (i = 0; i < itape->bttb_ntup; ++i) {
	    bti = parray[i];
	    btisz = BTITEMSZ(bti);
	    btisz = DOUBLEALIGN(btisz);
	    _bt_tapeadd(otape, bti, btisz);
#ifdef FASTBUILD_DEBUG
	    {
		bool isnull;
		Datum d = index_getattr(&(bti->bti_itup), 1,
					RelationGetTupleDescriptor(index),
					&isnull);
		printf("_bt_spool: inserted <%x> into output tape %d\n",
		       d, btspool->bts_tape);
	    }
#endif /* FASTBUILD_DEBUG */
	}

	/*
	 * the initial runs are always single tape blocks.  flush the
	 * output block, marking End-Of-Run.
	 */
	_bt_tapewrite(otape, 1);

	/*
	 * reset the input buffer for the next run.  we don't have to
	 * write it out or anything -- we only use it to hold the
	 * unsorted BTItemDatas, the output tape contains all the
	 * sorted stuff.
	 *
	 * changing bts_tape changes the output tape and input tape;
	 * we change itape for the code below.
	 */
	_bt_tapereset(itape);
	btspool->bts_tape = (btspool->bts_tape + 1) % btspool->bts_ntapes;
	itape = btspool->bts_itape[btspool->bts_tape];

	/*
	 * destroy the pointer array.
	 */
	pfree((void *) parray);
    }

    /* insert this item into the current buffer */
    if (btitem != (BTItem) NULL) {
	_bt_tapeadd(itape, btitem, itemsz);
    }
}

/*
 * allocate a new, clean btree page, not linked to any siblings.
 */
static void
_bt_blnewpage(Relation index, Buffer *buf, Page *page, int flags)
{
    BTPageOpaque opaque;

    *buf = _bt_getbuf(index, P_NEW, BT_WRITE);
    *page = BufferGetPage(*buf);
    _bt_pageinit(*page, BufferGetPageSize(*buf));
    opaque = (BTPageOpaque) PageGetSpecialPointer(*page);
    opaque->btpo_prev = opaque->btpo_next = P_NONE;
    opaque->btpo_flags = flags;
}

/*
 * slide an array of ItemIds back one slot (from P_FIRSTKEY to
 * P_HIKEY).  we need to do this when we discover that we have built
 * an ItemId array in what has turned out to be a P_RIGHTMOST page.
 */
static void
_bt_slideleft(Relation index, Buffer buf, Page page)
{
    OffsetNumber off;
    OffsetNumber maxoff;
    ItemId previi;
    ItemId thisii;

    maxoff = PageGetMaxOffsetNumber(page);
    previi = PageGetItemId(page, P_HIKEY);
    for (off = P_FIRSTKEY; off <= maxoff; off = OffsetNumberNext(off)) {
	thisii = PageGetItemId(page, off);
	*previi = *thisii;
	previi = thisii;
    }
    ((PageHeader) page)->pd_lower -= sizeof(ItemIdData);
}

typedef struct {
    Buffer		btps_buf;
    Page		btps_page;
    BTItem		btps_lastbti;
    OffsetNumber	btps_lastoff;
    OffsetNumber	btps_firstoff;
} BTPageState;

/*
 * add an item to a disk page from a merge tape block.
 *
 * we must be careful to observe the following restrictions, placed
 * upon us by the conventions in nbtsearch.c:
 * - rightmost pages start data items at P_HIKEY instead of at
 *   P_FIRSTKEY.
 * - duplicates cannot be split among pages unless the chain of
 *   duplicates starts at the first data item.
 *
 * a leaf page being built looks like:
 *
 * +----------------+---------------------------------+
 * | PageHeaderData | linp0 linp1 linp2 ...           |
 * +-----------+----+---------------------------------+
 * | ... linpN |                  ^ first             |
 * +-----------+--------------------------------------+
 * |     ^ last                                       |
 * |                                                  |
 * |               v last                             |
 * +-------------+------------------------------------+
 * |             | itemN ...                          |
 * +-------------+------------------+-----------------+
 * |          ... item3 item2 item1 | "special space" |
 * +--------------------------------+-----------------+
 *                      ^ first
 *
 * contrast this with the diagram in bufpage.h; note the mismatch
 * between linps and items.  this is because we reserve linp0 as a
 * placeholder for the pointer to the "high key" item; when we have
 * filled up the page, we will set linp0 to point to itemN and clear
 * linpN.
 *
 * 'last' pointers indicate the last offset/item added to the page.
 * 'first' pointers indicate the first offset/item that is part of a
 * chain of duplicates extending from 'first' to 'last'.
 *
 * if all keys are unique, 'first' will always be the same as 'last'.
 */
static void
_bt_buildadd(Relation index, BTPageState *state, BTItem bti, int flags)
{
    Buffer nbuf;
    Page npage;
    BTItem last_bti;
    OffsetNumber first_off;
    OffsetNumber last_off;
    OffsetNumber off;
    Size pgspc;
    Size btisz;

    nbuf = state->btps_buf;
    npage = state->btps_page;
    first_off = state->btps_firstoff;
    last_off = state->btps_lastoff;
    last_bti = state->btps_lastbti;

    pgspc = PageGetFreeSpace(npage);
    btisz = BTITEMSZ(bti);
    btisz = DOUBLEALIGN(btisz);
    if (pgspc < btisz) {
	Buffer obuf = nbuf;
	Page opage = npage;
	OffsetNumber o, n;
	ItemId ii;
	ItemId hii;

	_bt_blnewpage(index, &nbuf, &npage, flags);

	/*
	 * if 'last' is part of a chain of duplicates that does not
	 * start at the beginning of the old page, the entire chain is
	 * copied to the new page; we delete all of the duplicates
	 * from the old page except the first, which becomes the high
	 * key item of the old page.
	 *
	 * if the chain starts at the beginning of the page or there
	 * is no chain ('first' == 'last'), we need only copy 'last'
	 * to the new page.  again, 'first' (== 'last') becomes the
	 * high key of the old page.
	 *
	 * note that in either case, we copy at least one item to the
	 * new page, so 'last_bti' will always be valid.  'bti' will
	 * never be the first data item on the new page.
	 */
	if (first_off == P_FIRSTKEY) {
	    Assert(last_off != P_FIRSTKEY);
	    first_off = last_off;
	}
	for (o = first_off, n = P_FIRSTKEY;
	     o <= last_off;
	     o = OffsetNumberNext(o), n = OffsetNumberNext(n)) {
	    ii = PageGetItemId(opage, o);
	    (void) PageAddItem(npage, PageGetItem(opage, ii),
			       ii->lp_len, n, LP_USED);
#ifdef FASTBUILD_DEBUG
	    {
		bool isnull;
		BTItem tmpbti =
		    (BTItem) PageGetItem(npage, PageGetItemId(npage, n));
		Datum d = index_getattr(&(tmpbti->bti_itup), 1,
					RelationGetTupleDescriptor(index),
					&isnull);
		printf("_bt_buildadd: moved <%x> to offset %d\n",
		       d, n);
	    }
#endif /* FASTBUILD_DEBUG */
	}
	for (o = last_off; o > first_off; o = OffsetNumberPrev(o)) {
	    PageIndexTupleDelete(opage, o);
	}
	hii = PageGetItemId(opage, P_HIKEY);
	ii = PageGetItemId(opage, first_off);
	*hii = *ii;
	ii->lp_flags &= ~LP_USED;
	((PageHeader) opage)->pd_lower -= sizeof(ItemIdData);

	first_off = P_FIRSTKEY;
	last_off = PageGetMaxOffsetNumber(npage);
	last_bti = (BTItem) PageGetItem(npage, PageGetItemId(npage, last_off));

	/*
	 * set the page (side link) pointers.
	 */
	{
	    BTPageOpaque oopaque = (BTPageOpaque) PageGetSpecialPointer(opage);
	    BTPageOpaque nopaque = (BTPageOpaque) PageGetSpecialPointer(npage);

	    oopaque->btpo_next = BufferGetBlockNumber(nbuf);
	    nopaque->btpo_prev = BufferGetBlockNumber(obuf);
	    nopaque->btpo_next = P_NONE;
	}

	/*
	 * write out the old stuff.  we never want to see it again, so
	 * we can give up our lock (if we had one; BuildingBtree is
	 * set, so we aren't locking).
	 */
	_bt_wrtbuf(index, obuf);
    }
    
    /*
     * if this item is different from the last item added, we start a
     * new chain of duplicates.
     */
    off = OffsetNumberNext(last_off);
    (void) PageAddItem(npage, (Item) bti, btisz, off, LP_USED);
#ifdef FASTBUILD_DEBUG
    {
	bool isnull;
	Datum d = index_getattr(&(bti->bti_itup), 1, 
				RelationGetTupleDescriptor(index),
				&isnull);
	printf("_bt_buildadd: inserted <%x> at offset %d\n",
	       d, off);
    }
#endif /* FASTBUILD_DEBUG */
    if (last_bti == (BTItem) NULL) {
	first_off = P_FIRSTKEY;
    } else if (!_bt_itemcmp(index, 1, bti, last_bti, BTEqualStrategyNumber)) {
	first_off = off;
    }
    last_off = off;
    last_bti = (BTItem) PageGetItem(npage, PageGetItemId(npage, off));

    state->btps_buf = nbuf;
    state->btps_page = npage;
    state->btps_lastbti = last_bti;
    state->btps_lastoff = last_off;
    state->btps_firstoff = first_off;
}

/*
 * take the input tapes stored by 'btspool' and perform successive
 * merging passes until at most one run is left in each tape.  at that
 * point, merge the final tape runs into a set of btree leaves.
 *
 * XXX three nested loops?  gross.  cut me up into smaller routines.
 */
static BlockNumber
_bt_merge(Relation index, BTSpool *btspool)
{
    BTPageState state;
    BlockNumber firstblk;
    BTPriQueue q;
    BTPriQueueElem e;
    BTItem bti;
    BTTapeBlock *itape;
    BTTapeBlock *otape;
    char *tapepos[MAXTAPES];
    int tapedone[MAXTAPES];
    int t;
    int goodtapes;
    int nruns;
    Size btisz;
    bool doleaf = false;

    /*
     * initialize state needed for the merge into the btree leaf pages.
     */
    (void) memset((char *) &state, 0, sizeof(BTPageState));
    _bt_blnewpage(index, &(state.btps_buf), &(state.btps_page), BTP_LEAF);
    state.btps_lastoff = P_HIKEY;
    state.btps_lastbti = (BTItem) NULL;
    firstblk = BufferGetBlockNumber(state.btps_buf);

    do {							/* pass */
	/*
	 * each pass starts by flushing the previous outputs and
	 * swapping inputs and outputs.  this process also clears the
	 * new output tapes and rewinds the new input tapes.
	 */
	btspool->bts_tape = btspool->bts_ntapes - 1;
	_bt_spoolflush(btspool);
	_bt_spoolswap(btspool);

	nruns = 0;

	for (;;) {						/* run */
	    /*
	     * each run starts by selecting a new output tape.  the
	     * merged results of a given run are always sent to this
	     * one tape.
	     */
	    btspool->bts_tape = (btspool->bts_tape + 1) % btspool->bts_ntapes;
	    otape = btspool->bts_otape[btspool->bts_tape];

	    /*
	     * initialize the priority queue by loading it with the
	     * first element of the given run in each tape.  since we
	     * are starting a new run, we reset the tape (clearing the
	     * End-Of-Run marker) before reading it.  this means that
	     * _bt_taperead will return 0 only if the tape is actually
	     * at EOF.
	     */
	    (void) memset((char *) &q, 0, sizeof(BTPriQueue));
	    goodtapes = 0;
	    for (t = 0; t < btspool->bts_ntapes; ++t) {
		itape = btspool->bts_itape[t];
		tapepos[t] = itape->bttb_data;
		_bt_tapereset(itape);
		if (_bt_taperead(itape) == 0) {
		    tapedone[t] = 1;
		} else {
		    ++goodtapes;
		    tapedone[t] = 0;
		    e.btpqe_tape = t;
		    e.btpqe_item = _bt_tapenext(itape, &tapepos[t]);
		    if (e.btpqe_item != (BTItem) NULL) {
			_bt_pqadd(&q, &e);
		    }
		}
	    }
	    /*
	     * if we don't have any tapes with any input (i.e., they
	     * are all at EOF), we must be done with this pass.
	     */
	    if (goodtapes == 0) {
		break;	/* for */
	    }
	    ++nruns;
	
	    /*
	     * output the smallest element from the queue until there are no
	     * more.
	     */
	    while (_bt_pqnext(&q, &e) >= 0) {			/* item */
		/*
		 * replace the element taken from priority queue,
		 * fetching a new block if needed.  a tape can run out
		 * if it hits either End-Of-Run or EOF.
		 */
		t = e.btpqe_tape;
		bti = e.btpqe_item;
		if (bti != (BTItem) NULL) {
		    btisz = BTITEMSZ(bti);
		    btisz = DOUBLEALIGN(btisz);
		    if (doleaf) {
			_bt_buildadd(index, &state, bti, BTP_LEAF);
#ifdef FASTBUILD_DEBUG
			{
			    bool isnull;
			    Datum d = index_getattr(&(bti->bti_itup), 1,
				    RelationGetTupleDescriptor(index),
						    &isnull);
			    printf("_bt_merge: inserted <%x> into block %d\n",
				   d, BufferGetBlockNumber(state.btps_buf));
			}
#endif /* FASTBUILD_DEBUG */
		    } else {
			if (SPCLEFT(otape) < btisz) {
			    /*
			     * if it's full, write it out and add the
			     * item to the next block.  (since we know
			     * there will be at least one more block,
			     * we know we do *not* want to set
			     * End-Of-Run here!)
			     */
			    _bt_tapewrite(otape, 0);
			}
			_bt_tapeadd(otape, bti, btisz);
#ifdef FASTBUILD_DEBUG
			{
			    bool isnull;
			    Datum d = index_getattr(&(bti->bti_itup), 1,
				  RelationGetTupleDescriptor(index), &isnull);
			    printf("_bt_merge: inserted <%x> into tape %d\n",
				   d, btspool->bts_tape);
			}
#endif /* FASTBUILD_DEBUG */
		    }
		}
#ifdef FASTBUILD_DEBUG
		{
		    bool isnull;
		    Datum d = index_getattr(&(bti->bti_itup), 1,
					   RelationGetTupleDescriptor(index),
					    &isnull);
		    printf("_bt_merge: got <%x> from tape %d\n", d, t);
		}
#endif /* FASTBUILD_DEBUG */

		itape = btspool->bts_itape[t];
		if (!tapedone[t]) {
		    BTItem newbti = _bt_tapenext(itape, &tapepos[t]);

		    if (newbti == (BTItem) NULL) {
			if (_bt_taperead(itape) == 0) {
			    tapedone[t] = 1;
			} else {
			    tapepos[t] = itape->bttb_data;
			    newbti = _bt_tapenext(itape, &tapepos[t]);
			}
		    }
		    if (newbti != (BTItem) NULL) {
			BTPriQueueElem nexte;
			
			nexte.btpqe_tape = t;
			nexte.btpqe_item = newbti;
			_bt_pqadd(&q, &nexte);
		    }
		}
	    } 							/* item */
	}							/* run */
	
	/*
	 * we are here because we ran out of input on all of the input
	 * tapes.
	 *
	 * if this pass did not generate more actual output runs than
	 * we have tapes, we know we have at most one run in each
	 * tape.  this means that we are ready to merge into the final
	 * btree leaf pages instead of merging into a tape file.
	 */
	if (nruns <= btspool->bts_ntapes) {
	    doleaf = true;
	}
    } while (nruns > 0);					/* pass */

    /*
     * this is the rightmost page, so the ItemId array needs to be
     * slid back one slot.
     */
    _bt_slideleft(index, state.btps_buf, state.btps_page);
    _bt_wrtbuf(index, state.btps_buf);

    return(firstblk);
}


/*
 * given the block number 'blk' of the first page of a set of linked
 * siblings (i.e., the start of an entire level of the btree),
 * construct the corresponding next level of the btree.  we do this by
 * placing minimum keys from each page into this page.  the format of
 * the internal pages is otherwise the same as for leaf pages.
 */
void
_bt_upperbuild(Relation index, BlockNumber blk, int level)
{
    Buffer rbuf;
    Page rpage;
    BTPageOpaque ropaque;
    BTPageState state;
    BlockNumber firstblk;
    BTItem bti;
    BTItem nbti;
    OffsetNumber off;

    rbuf = _bt_getbuf(index, blk, BT_WRITE);
    rpage = BufferGetPage(rbuf);
    ropaque = (BTPageOpaque) PageGetSpecialPointer(rpage);

    /*
     * if we only have one page on a level, we can just make it the
     * root.
     */
    if (P_RIGHTMOST(ropaque)) {
	ropaque->btpo_flags |= BTP_ROOT;
	_bt_wrtbuf(index, rbuf);
	_bt_metaproot(index, blk);
	return;
    }
    _bt_relbuf(index, rbuf, BT_WRITE);
	
    (void) memset((char *) &state, 0, sizeof(BTPageState));
    _bt_blnewpage(index, &(state.btps_buf), &(state.btps_page), 0);
    state.btps_lastoff = P_HIKEY;
    state.btps_lastbti = (BTItem) NULL;
    firstblk = BufferGetBlockNumber(state.btps_buf);
    
    /* for each page... */
    do {
	rbuf = _bt_getbuf(index, blk, BT_READ);
	rpage = BufferGetPage(rbuf);
	ropaque = (BTPageOpaque) PageGetSpecialPointer(rpage);
	
	/* for each item... */
	if (!PageIsEmpty(rpage)) {
	    /*
	     * form a new index tuple corresponding to the minimum key
	     * of the lower page and insert it into a page at this
	     * level.
	     */
	    off = P_RIGHTMOST(ropaque) ? P_HIKEY : P_FIRSTKEY;
	    bti = (BTItem) PageGetItem(rpage, PageGetItemId(rpage, off));
	    nbti = _bt_formitem(&(bti->bti_itup));
	    ItemPointerSet(&(nbti->bti_itup.t_tid), blk, P_HIKEY);
#ifdef FASTBUILD_DEBUG
	    {
		bool isnull;
		Datum d = index_getattr(&(nbti->bti_itup), 1, 
					RelationGetTupleDescriptor(index),
					&isnull);
		printf("_bt_upperbuild: inserting <%x> at %d\n",
		       d, level);
	    }
#endif /* FASTBUILD_DEBUG */
	    _bt_buildadd(index, &state, nbti, 0);
	    pfree((void *) nbti);
	}
	blk = ropaque->btpo_next;
	_bt_relbuf(index, rbuf, BT_READ);
    } while (blk != P_NONE);
	
    /*
     * this is the rightmost page, so the ItemId array needs to be
     * slid back one slot.
     */
    _bt_slideleft(index, state.btps_buf, state.btps_page);
    _bt_wrtbuf(index, state.btps_buf);
    
    _bt_upperbuild(index, firstblk, level + 1);
}

/*
 * given a spool loading by successive calls to _bt_spool, create an
 * entire btree.
 */
void
_bt_leafbuild(Relation index, void *spool)
{
    BTSpool *btspool = (BTSpool *) spool;
    BlockNumber firstblk;

    /*
     * merge the runs into btree leaf pages.
     */
    firstblk = _bt_merge(index, btspool);

    /*
     * build the upper levels of the btree.
     */
    _bt_upperbuild(index, firstblk, 0);
}

#else /* !FASTBUILD */

void *_bt_spoolinit(Relation index, int ntapes) { return((void *) NULL); }
void _bt_spooldestroy(void *spool) { }
void _bt_spool(Relation index, BTItem btitem, void *spool) { }
void _bt_upperbuild(Relation index, BlockNumber blk, int level) { }
void _bt_leafbuild(Relation index, void *spool) { }

#endif /* !FASTBUILD */
