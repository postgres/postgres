/*-------------------------------------------------------------------------
 *
 * valid.h--
 *    POSTGRES tuple qualification validity definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: valid.h,v 1.4 1997/03/28 07:05:54 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	VALID_H
#define VALID_H

#include <utils/tqual.h>
#include <storage/bufpage.h>
#include <utils/rel.h>

/* ----------------
 *	extern decl's
 * ----------------
 */

extern bool heap_keytest(HeapTuple t, TupleDesc tupdesc,
			 int nkeys, ScanKey keys);

extern HeapTuple heap_tuple_satisfies(ItemId itemId, Relation relation,
				      Buffer buffer, PageHeader disk_page, 
				      TimeQual qual, int nKeys, 
				      ScanKey key);

extern bool TupleUpdatedByCurXactAndCmd(HeapTuple t);

#endif	/* VALID_H */
