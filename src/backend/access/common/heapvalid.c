/*-------------------------------------------------------------------------
 *
 * heapvalid.c--
 *    heap tuple qualification validity checking code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/common/Attic/heapvalid.c,v 1.11 1996/11/05 07:42:41 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <postgres.h>

#include <fmgr.h>
#include <access/heaptuple.h>
#include <access/xact.h>
#include <storage/bufpage.h>
#include <utils/rel.h>
#include <utils/tqual.h>

/* ----------------
 *	heap_keytest
 *
 *	Test a heap tuple with respect to a scan key.
 * ----------------
 */
bool
heap_keytest(HeapTuple t,
	     TupleDesc tupdesc,
	     int nkeys,
	     ScanKey keys)
{
    bool	isnull;
    Datum	atp;
    int		test;
    
    for (; nkeys--; keys++) {
	atp = (Datum)heap_getattr(t, InvalidBuffer,
				  keys->sk_attno, 
				  tupdesc,
				  &isnull);
	
	if (isnull)
	    /* XXX eventually should check if SK_ISNULL */
	    return false;
	
	if (keys->sk_flags & SK_ISNULL) {
	    return (false);
	}

	if (keys->sk_flags & SK_COMMUTE)
	    test = (long) FMGR_PTR2(keys->sk_func, keys->sk_procedure,
				    keys->sk_argument, atp);
	else
	    test = (long) FMGR_PTR2(keys->sk_func, keys->sk_procedure,
				    atp, keys->sk_argument);
	
	if (!test == !(keys->sk_flags & SK_NEGATE))
	    return false;
    }
    
    return true;
}

/* ----------------
 *	heap_tuple_satisfies
 *
 *  Returns a valid HeapTuple if it satisfies the timequal and keytest.
 *  Returns NULL otherwise.  Used to be heap_satisifies (sic) which
 *  returned a boolean.  It now returns a tuple so that we can avoid doing two
 *  PageGetItem's per tuple.
 *
 *	Complete check of validity including LP_CTUP and keytest.
 *	This should perhaps be combined with valid somehow in the
 *	future.  (Also, additional rule tests/time range tests.)
 *
 *  on 8/21/92 mao says:  i rearranged the tests here to do keytest before
 *  SatisfiesTimeQual.  profiling indicated that even for vacuumed relations,
 *  time qual checking was more expensive than key testing.  time qual is
 *  least likely to fail, too.  we should really add the time qual test to
 *  the restriction and optimize it in the normal way.  this has interactions
 *  with joey's expensive function work.
 * ----------------
 */
HeapTuple
heap_tuple_satisfies(ItemId itemId,
		     Relation relation,
		     PageHeader disk_page,
		     TimeQual	qual,
		     int nKeys,
		     ScanKey key)
{
    HeapTuple	tuple;
    bool res;
    
    if (! ItemIdIsUsed(itemId))
	return NULL;
    
    tuple = (HeapTuple) PageGetItem((Page) disk_page, itemId);
    
    if (key != NULL)
	res = heap_keytest(tuple, RelationGetTupleDescriptor(relation), 
			   nKeys, key);
    else
	res = TRUE;
    
    if (res && (relation->rd_rel->relkind == RELKIND_UNCATALOGED
		|| HeapTupleSatisfiesTimeQual(tuple,qual)))
	return tuple;
    
    return (HeapTuple) NULL;
}

/*
 *  TupleUpdatedByCurXactAndCmd() -- Returns true if this tuple has
 *	already been updated once by the current transaction/command
 *	pair.
 */
bool
TupleUpdatedByCurXactAndCmd(HeapTuple t)
{
    if (TransactionIdEquals(t->t_xmax,
			    GetCurrentTransactionId()) &&
	t->t_cmax == GetCurrentCommandId())
	return true;
    
    return false;
}
