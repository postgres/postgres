/*-------------------------------------------------------------------------
 *
 * indextuple.c--
 *     This file contains index tuple accessor and mutator routines,
 *     as well as a few various tuple utilities.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/common/indextuple.c,v 1.10 1996/11/03 10:57:21 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/ibit.h"
#include "access/itup.h"
#include "access/relscan.h"
#include "access/tupmacs.h"
#include "utils/palloc.h"

#ifndef HAVE_MEMMOVE
# include "regex/utils.h"
#else
# include <string.h>
#endif

static Size IndexInfoFindDataOffset(unsigned short t_info);

/* ----------------------------------------------------------------
 *		  index_ tuple interface routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *	index_formtuple
 * ----------------
 */
IndexTuple
index_formtuple(TupleDesc tupleDescriptor,
		Datum value[],
		char null[])
{
    register char	*tp;	/* tuple pointer */
    IndexTuple		tuple;	/* return tuple */
    Size		size, hoff;
    int 		i;
    unsigned short      infomask = 0;
    bool		hasnull = false;
    char		tupmask = 0;
    int                 numberOfAttributes = tupleDescriptor->natts;
    
    if (numberOfAttributes > MaxIndexAttributeNumber)
	elog(WARN, "index_formtuple: numberOfAttributes of %d > %d",
	     numberOfAttributes, MaxIndexAttributeNumber);
    
    
    for (i = 0; i < numberOfAttributes && !hasnull; i++) {
	if (null[i] != ' ') hasnull = true;
    }
    
    if (hasnull) infomask |= INDEX_NULL_MASK;
    
    hoff = IndexInfoFindDataOffset(infomask);
    size = hoff
	+ ComputeDataSize(tupleDescriptor,
			  value, null);
    size = DOUBLEALIGN(size);	/* be conservative */
    
    tp = (char *) palloc(size);
    tuple = (IndexTuple) tp;
    memset(tp,0,(int)size);
    
    DataFill((char *)tp + hoff,
	     tupleDescriptor,
	     value,
	     null,
	     &tupmask,
	     (hasnull ? (bits8*)tp + sizeof(*tuple) : NULL));
    
    /*
     * We do this because DataFill wants to initialize a "tupmask" which
     * is used for HeapTuples, but we want an indextuple infomask.  The only
     * "relevent" info is the "has variable attributes" field, which is in
     * mask position 0x02.  We have already set the null mask above.
     */
    
    if (tupmask & 0x02) infomask |= INDEX_VAR_MASK;
    
    /*
     * Here we make sure that we can actually hold the size.  We also want
     * to make sure that size is not aligned oddly.  This actually is a
     * rather odd way to make sure the size is not too large overall.
     */
    
    if (size & 0xE000)
	elog(WARN, "index_formtuple: data takes %d bytes: too big", size);

    
    infomask |= size;
    
    /* ----------------
     * initialize metadata
     * ----------------
     */
    tuple->t_info = infomask;
    return (tuple);
}

/* ----------------
 *	fastgetiattr
 *
 *	This is a newer version of fastgetiattr which attempts to be
 *	faster by caching attribute offsets in the attribute descriptor.
 *
 *	an alternate way to speed things up would be to cache offsets
 *	with the tuple, but that seems more difficult unless you take
 *	the storage hit of actually putting those offsets into the
 *	tuple you send to disk.  Yuck.
 *
 *	This scheme will be slightly slower than that, but should
 *	preform well for queries which hit large #'s of tuples.  After
 *	you cache the offsets once, examining all the other tuples using
 *	the same attribute descriptor will go much quicker. -cim 5/4/91
 * ----------------
 */
char *
fastgetiattr(IndexTuple tup,
	     int attnum,
	     TupleDesc tupleDesc,
	     bool *isnull)
{
    register char		*tp;		/* ptr to att in tuple */
    register char		*bp = NULL;	/* ptr to att in tuple */
    int 			slow;		/* do we have to walk nulls? */
    register int		data_off;	/* tuple data offset */
    
    /* ----------------
     *	sanity checks
     * ----------------
     */
    
    Assert(PointerIsValid(isnull));
    Assert(attnum > 0);
    
    /* ----------------
     *   Three cases:
     * 
     *   1: No nulls and no variable length attributes.
     *   2: Has a null or a varlena AFTER att.
     *   3: Has nulls or varlenas BEFORE att.
     * ----------------
     */
    
    *isnull =  false;
    data_off = IndexTupleHasMinHeader(tup) ? sizeof *tup : 
	IndexInfoFindDataOffset(tup->t_info);
    
    if (IndexTupleNoNulls(tup)) {
	
	/* first attribute is always at position zero */
	
	if (attnum == 1) {
	    return(fetchatt(&(tupleDesc->attrs[0]), (char *) tup + data_off));
	}
	attnum--;
	
	if (tupleDesc->attrs[attnum]->attcacheoff > 0) {
	    return(fetchatt(&(tupleDesc->attrs[attnum]),
			    (char *) tup + data_off + 
			    tupleDesc->attrs[attnum]->attcacheoff));
	}
	
	tp = (char *) tup + data_off;
	
	slow = 0;
    }else { /* there's a null somewhere in the tuple */
	
	bp = (char *) tup + sizeof(*tup); /* "knows" t_bits are here! */
	slow = 0;
	/* ----------------
	 *	check to see if desired att is null
	 * ----------------
	 */
	
	attnum--;
	{
	    if (att_isnull(attnum, bp)) {
		*isnull = true;
		return NULL;
	    }
	}
	/* ----------------
	 *      Now check to see if any preceeding bits are null...
	 * ----------------
	 */
	{
	    register int  i = 0; /* current offset in bp */
	    register int  mask;	 /* bit in byte we're looking at */
	    register char n;	 /* current byte in bp */
	    register int byte, finalbit;
	    
	    byte = attnum >> 3;
	    finalbit = attnum & 0x07;
	    
	    for (; i <= byte; i++) {
		n = bp[i];
		if (i < byte) {
		    /* check for nulls in any "earlier" bytes */
		    if ((~n) != 0) {
			slow++;
			break;
		    }
		} else {
		    /* check for nulls "before" final bit of last byte*/
		    mask = (finalbit << 1) - 1;
		    if ((~n) & mask)
			slow++;
		}
	    }
	}
	tp = (char *) tup + data_off;
    }
    
    /* now check for any non-fixed length attrs before our attribute */
    
    if (!slow) {
	if (tupleDesc->attrs[attnum]->attcacheoff > 0) {
	    return(fetchatt(&(tupleDesc->attrs[attnum]), 
			    tp + tupleDesc->attrs[attnum]->attcacheoff));
	}else if (!IndexTupleAllFixed(tup)) {
	    register int j = 0;
	    
	    for (j = 0; j < attnum && !slow; j++)
		if (tupleDesc->attrs[j]->attlen < 1) slow = 1;
	}
    }
    
    /*
     * if slow is zero, and we got here, we know that we have a tuple with
     * no nulls.  We also know that we have to initialize the remainder of
     * the attribute cached offset values.
     */
    
    if (!slow) {
	register int j = 1;
	register long off;
	
	/*
	 * need to set cache for some atts
	 */
	
	tupleDesc->attrs[0]->attcacheoff = 0;
	
	while (tupleDesc->attrs[j]->attcacheoff > 0) j++;
	
	off = tupleDesc->attrs[j-1]->attcacheoff + 
	      tupleDesc->attrs[j-1]->attlen;
	
	for (; j < attnum + 1; j++) {
	    /*
	     * Fix me when going to a machine with more than a four-byte
	     * word!
	     */
	    
	    switch(tupleDesc->attrs[j]->attlen)
		{
		case -1:
		    off = (tupleDesc->attrs[j]->attalign=='d')?
			DOUBLEALIGN(off):INTALIGN(off);
		    break;
		case sizeof(char):
		    break;
		case sizeof(short):
		    off = SHORTALIGN(off);
		    break;
		case sizeof(int32):
		    off = INTALIGN(off);
		    break;
		default:
		    if (tupleDesc->attrs[j]->attlen > sizeof(int32))
			off = (tupleDesc->attrs[j]->attalign=='d')?
			    DOUBLEALIGN(off) : LONGALIGN(off);
		    else
			elog(WARN, "fastgetiattr: attribute %d has len %d",
			     j, tupleDesc->attrs[j]->attlen);
		    break;
		    
		}
	    
	    tupleDesc->attrs[j]->attcacheoff = off;
	    off += tupleDesc->attrs[j]->attlen;
	}
	
	return(fetchatt( &(tupleDesc->attrs[attnum]), 
			tp + tupleDesc->attrs[attnum]->attcacheoff));
    }else {
	register bool usecache = true;
	register int off = 0;
	register int i;
	
	/*
	 * Now we know that we have to walk the tuple CAREFULLY.
	 */
	
	for (i = 0; i < attnum; i++) {
	    if (!IndexTupleNoNulls(tup)) {
		if (att_isnull(i, bp)) {
		    usecache = false;
		    continue;
		}
	    }
		
	    if (usecache && tupleDesc->attrs[i]->attcacheoff > 0) {
		off = tupleDesc->attrs[i]->attcacheoff;
		if (tupleDesc->attrs[i]->attlen == -1) 
		    usecache = false;
		else
		    continue;
	    }
		    
	    if (usecache) tupleDesc->attrs[i]->attcacheoff = off;
	    switch(tupleDesc->attrs[i]->attlen)
		{
		case sizeof(char):
		    off++;
		    break;
		case sizeof(short):
		    off = SHORTALIGN(off) + sizeof(short);
		    break;
		case -1:
		    usecache = false;
		    off = (tupleDesc->attrs[i]->attalign=='d')?
			DOUBLEALIGN(off):INTALIGN(off);
		    off += VARSIZE(tp + off);
		    break;
		default:
		    if (tupleDesc->attrs[i]->attlen > sizeof(int32))
			off = (tupleDesc->attrs[i]->attalign=='d') ?
			    DOUBLEALIGN(off) + tupleDesc->attrs[i]->attlen :
			    LONGALIGN(off) + tupleDesc->attrs[i]->attlen;
		    else
			elog(WARN, "fastgetiattr2: attribute %d has len %d",
			     i, tupleDesc->attrs[i]->attlen);
		    
		    break;
		}
	}
	
	return(fetchatt(&tupleDesc->attrs[attnum], tp + off));
    }
}

/* ----------------
 *	index_getattr
 * ----------------
 */
Datum
index_getattr(IndexTuple tuple,
	      AttrNumber attNum,
	      TupleDesc tupDesc,
	      bool *isNullOutP)
{
    Assert (attNum > 0);

    return (Datum)
	fastgetiattr(tuple, attNum, tupDesc, isNullOutP);
}

RetrieveIndexResult
FormRetrieveIndexResult(ItemPointer indexItemPointer,
			ItemPointer heapItemPointer)
{
    RetrieveIndexResult	result;
    
    Assert(ItemPointerIsValid(indexItemPointer));
    Assert(ItemPointerIsValid(heapItemPointer));
    
    result = (RetrieveIndexResult) palloc(sizeof *result);
    
    result->index_iptr = *indexItemPointer;
    result->heap_iptr = *heapItemPointer;
    
    return (result);
}

/*
 * Takes an infomask as argument (primarily because this needs to be usable
 * at index_formtuple time so enough space is allocated).
 *
 * Change me if adding an attribute to IndexTuples!!!!!!!!!!!
 */
static Size
IndexInfoFindDataOffset(unsigned short t_info)
{
    if (!(t_info & INDEX_NULL_MASK))
	return((Size) sizeof(IndexTupleData));
    else {
	Size size = sizeof(IndexTupleData);
	
	if (t_info & INDEX_NULL_MASK) {
	    size += sizeof(IndexAttributeBitMapData);
	}
	return DOUBLEALIGN(size);	/* be conservative */
    }
}

/*
 * Copies source into target.  If *target == NULL, we palloc space; otherwise
 * we assume we have space that is already palloc'ed.
 */
void
CopyIndexTuple(IndexTuple source, IndexTuple *target)
{
    Size size;
    IndexTuple ret;
    
    size = IndexTupleSize(source);
    if (*target == NULL) {
	*target = (IndexTuple) palloc(size);
    }
    
    ret = *target;
    memmove((char*)ret, (char*)source, size);
}

