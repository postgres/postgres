/*-------------------------------------------------------------------------
 *
 * btutils.c--
 *    Utility code for Postgres btree implementation.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/nbtree/nbtutils.c,v 1.3 1996/10/20 10:53:18 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/pg_attribute.h"
#include "access/attnum.h"
#include "nodes/pg_list.h"
#include "access/tupdesc.h"
#include "storage/fd.h"
#include "catalog/pg_am.h"
#include "catalog/pg_class.h"
#include "nodes/nodes.h"
#include "rewrite/prs2lock.h"
#include "access/skey.h"
#include "access/strat.h"
#include "utils/rel.h"

#include "storage/block.h"
#include "storage/off.h"
#include "storage/itemptr.h"
#include "access/itup.h"
#include "access/funcindex.h"
#include "storage/itemid.h"
#include "storage/item.h"
#include "storage/buf.h"
#include "storage/bufpage.h"
#include <time.h>
#include "utils/nabstime.h"
#include "access/htup.h"
#include "utils/tqual.h"
#include "access/relscan.h"
#include "access/sdir.h"
#include "access/nbtree.h"

#include "access/istrat.h"

ScanKey 
_bt_mkscankey(Relation rel, IndexTuple itup)
{     
    ScanKey skey;
    TupleDesc itupdesc;
    int natts;
    int i;
    Datum arg;
    RegProcedure proc;
    bool null;
    
    natts = rel->rd_rel->relnatts;
    itupdesc = RelationGetTupleDescriptor(rel);
    
    skey = (ScanKey) palloc(natts * sizeof(ScanKeyData));
    
    for (i = 0; i < natts; i++) {
	arg = index_getattr(itup, i + 1, itupdesc, &null);
	proc = index_getprocid(rel, i + 1, BTORDER_PROC);
	ScanKeyEntryInitialize(&skey[i],
			       0x0, (AttrNumber) (i + 1), proc, arg);
    }
    
    return (skey);
}

void
_bt_freeskey(ScanKey skey)
{
    pfree(skey);
}

void
_bt_freestack(BTStack stack)
{
    BTStack ostack;
    
    while (stack != (BTStack) NULL) {
	ostack = stack;
	stack = stack->bts_parent;
	pfree(ostack->bts_btitem);
	pfree(ostack);
    }
}

/*
 *  _bt_orderkeys() -- Put keys in a sensible order for conjunctive quals.
 *
 *	The order of the keys in the qual match the ordering imposed by
 *	the index.  This routine only needs to be called if there are
 *	more than one qual clauses using this index.
 */
void
_bt_orderkeys(Relation relation, uint16 *numberOfKeys, ScanKey key, uint16 *qual_ok)
{
    ScanKey xform;
    ScanKeyData *cur;
    StrategyMap map;
    int nbytes;
    long test;
    int i, j;
    int init[BTMaxStrategyNumber+1];
    
    /* haven't looked at any strategies yet */
    for (i = 0; i <= BTMaxStrategyNumber; i++)
	init[i] = 0;
    
    /* get space for the modified array of keys */
    nbytes = BTMaxStrategyNumber * sizeof(ScanKeyData);
    xform = (ScanKey) palloc(nbytes);
    memset(xform, 0, nbytes); 
    
    
    /* get the strategy map for this index/attribute pair */
    /*
     *  XXX
     *  When we support multiple keys in a single index, this is what
     *  we'll want to do.  At present, the planner is hosed, so we
     *  hard-wire the attribute number below.  Postgres only does single-
     *  key indices...
     * map = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
     * 				    BTMaxStrategyNumber,
     * 				    key->data[0].attributeNumber);
     */
    map = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
				      BTMaxStrategyNumber,
				      1 /* XXX */ );
    
    /* check each key passed in */
    for (i = *numberOfKeys; --i >= 0; ) {
	cur = &key[i];
	for (j = BTMaxStrategyNumber; --j >= 0; ) {
	    if (cur->sk_procedure == map->entry[j].sk_procedure)
		break;
	}
	
	/* have we seen one of these before? */
	if (init[j]) {
	    /* yup, use the appropriate value */
	    test =
		(long) FMGR_PTR2(cur->sk_func, cur->sk_procedure,
				 cur->sk_argument, xform[j].sk_argument);
	    if (test)
		xform[j].sk_argument = cur->sk_argument;
	    else if ( j == (BTEqualStrategyNumber - 1) )
	    	*qual_ok = 0;		/* key == a && key == b, but a != b */
	} else {
	    /* nope, use this value */
	    memmove(&xform[j], cur, sizeof(*cur));
	   
	    init[j] = 1;
	}
    }
    
    /* if = has been specified, no other key will be used */
    /*
     * XXX
     * But in case of key < 2 && key == 1 and so on 
     * we have to set qual_ok to 0
     */
    if (init[BTEqualStrategyNumber - 1]) {

	ScanKeyData *eq, *chk;

	eq = &xform[BTEqualStrategyNumber - 1];

	for (j = BTMaxStrategyNumber; --j >= 0; )
	{
		if ( j == (BTEqualStrategyNumber - 1) || init[j] == 0 )
			continue;

		chk = &xform[j];

		test = (long) fmgr(chk->sk_procedure, eq->sk_argument, chk->sk_argument);
	
		if (!test)
			*qual_ok = 0;
	}

	init[BTLessStrategyNumber - 1] = 0;
	init[BTLessEqualStrategyNumber - 1] = 0;
	init[BTGreaterEqualStrategyNumber - 1] = 0;
	init[BTGreaterStrategyNumber - 1] = 0;
    }
    
    /* only one of <, <= */
    if (init[BTLessStrategyNumber - 1]
	&& init[BTLessEqualStrategyNumber - 1]) {
	
	ScanKeyData *lt, *le;
	
	lt = &xform[BTLessStrategyNumber - 1];
	le = &xform[BTLessEqualStrategyNumber - 1];
	
	/*
	 *  DO NOT use the cached function stuff here -- this is key
	 *  ordering, happens only when the user expresses a hokey
	 *  qualification, and gets executed only once, anyway.  The
	 *  transform maps are hard-coded, and can't be initialized
	 *  in the correct way.
	 */
	
	test = (long) fmgr(le->sk_procedure, lt->sk_argument, le->sk_argument);
	
	if (test)
	    init[BTLessEqualStrategyNumber - 1] = 0;
	else
	    init[BTLessStrategyNumber - 1] = 0;
    }
    
    /* only one of >, >= */
    if (init[BTGreaterStrategyNumber - 1]
	&& init[BTGreaterEqualStrategyNumber - 1]) {
	
	ScanKeyData *gt, *ge;
	
	gt = &xform[BTGreaterStrategyNumber - 1];
	ge = &xform[BTGreaterEqualStrategyNumber - 1];
	
	/* see note above on function cache */
	test = (long) fmgr(ge->sk_procedure, gt->sk_argument, ge->sk_argument);
	
	if (test)
	    init[BTGreaterEqualStrategyNumber - 1] = 0;
	else
	    init[BTGreaterStrategyNumber - 1] = 0;
    }
    
    /* okay, reorder and count */
    j = 0;
    
    for (i = BTMaxStrategyNumber; --i >= 0; )
	if (init[i])
	    key[j++] = xform[i];
    
    *numberOfKeys = j;
    
    pfree(xform);
}

bool
_bt_checkqual(IndexScanDesc scan, IndexTuple itup)
{
    if (scan->numberOfKeys > 0)
	return (index_keytest(itup, RelationGetTupleDescriptor(scan->relation),
			      scan->numberOfKeys, scan->keyData));
    else
	return (true);
}

BTItem
_bt_formitem(IndexTuple itup)
{
    int nbytes_btitem;
    BTItem btitem;
    Size tuplen;
    extern Oid newoid();
    
    /* disallow nulls in btree keys */
    if (itup->t_info & INDEX_NULL_MASK)
	elog(WARN, "btree indices cannot include null keys");
    
    /* make a copy of the index tuple with room for the sequence number */
    tuplen = IndexTupleSize(itup);
    nbytes_btitem = tuplen +
	(sizeof(BTItemData) - sizeof(IndexTupleData));
    
    btitem = (BTItem) palloc(nbytes_btitem);
    memmove((char *) &(btitem->bti_itup), (char *) itup, tuplen);
    
    btitem->bti_oid = newoid();
    return (btitem);
}
