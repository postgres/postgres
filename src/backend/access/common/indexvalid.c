/*-------------------------------------------------------------------------
 *
 * indexvalid.c--
 *    index tuple qualification validity checking code
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *    $Header: /cvsroot/pgsql/src/backend/access/common/Attic/indexvalid.c,v 1.8 1996/10/31 07:48:37 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */

#include <time.h>

#include "postgres.h"
#include "access/attnum.h"
#include "catalog/pg_attribute.h"
#include "executor/execdebug.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "storage/off.h"
#include "storage/block.h"
#include "utils/nabstime.h"

#include "access/skey.h"
#include "access/tupdesc.h"
#include "storage/itemptr.h"

#include "access/htup.h"
#include "access/itup.h"


/* ----------------------------------------------------------------
 *		  index scan key qualification code
 * ----------------------------------------------------------------
 */
int	NIndexTupleProcessed;

/* ----------------
 *	index_keytest
 *
 * old comments
 *	May eventually combine with other tests (like timeranges)?
 *	Should have Buffer buffer; as an argument and pass it to amgetattr.
 * ----------------
 */
bool
index_keytest(IndexTuple tuple,
	      TupleDesc tupdesc,
	      int scanKeySize,
	      ScanKey key)
{
    bool	    isNull;
    Datum	    datum;
    int		    test;
    
    IncrIndexProcessed();
    
    while (scanKeySize > 0) {
	datum = index_getattr(tuple,
			      1,
			      tupdesc,
			      &isNull);
	
	if (isNull) {
	    /* XXX eventually should check if SK_ISNULL */
	    return (false);
	}
	
	if (key[0].sk_flags & SK_ISNULL) {
	    return (false);
	}

	if (key[0].sk_flags & SK_COMMUTE) {
	    test = (int) (*(key[0].sk_func))
		(DatumGetPointer(key[0].sk_argument),
		 datum);
	} else {
	    test = (int) (*(key[0].sk_func))
		(datum,
		 DatumGetPointer(key[0].sk_argument));
	}
	
	if (!test == !(key[0].sk_flags & SK_NEGATE)) {
	    return (false);
	}
	
	scanKeySize -= 1;
	key++;
    }
    
    return (true);
}

