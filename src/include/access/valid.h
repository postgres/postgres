/*-------------------------------------------------------------------------
 *
 * valid.h--
 *    POSTGRES tuple qualification validity definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: valid.h,v 1.2 1996/10/31 09:46:44 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	VALID_H
#define VALID_H

#include "access/skey.h"
#include "storage/buf.h"
#include "utils/tqual.h"
#include "access/tupdesc.h"
#include "utils/rel.h"
#include "storage/bufpage.h"

/* ----------------
 *	extern decl's
 * ----------------
 */

extern bool heap_keytest(HeapTuple t, TupleDesc tupdesc,
			 int nkeys, ScanKey keys);

extern HeapTuple heap_tuple_satisfies(ItemId itemId, Relation relation,
       PageHeader disk_page, TimeQual qual, int nKeys, ScanKey key);

extern bool TupleUpdatedByCurXactAndCmd(HeapTuple t);

#endif	/* VALID_H */
