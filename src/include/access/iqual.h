/*-------------------------------------------------------------------------
 *
 * iqual.h--
 *    Index scan key qualification definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: iqual.h,v 1.3 1996/10/31 09:46:39 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	IQUAL_H
#define IQUAL_H


#include "storage/itemid.h"
#include "utils/rel.h"
#include "access/skey.h"
#include "access/itup.h"

/* ----------------
 *	index tuple qualification support
 * ----------------
 */

extern int NIndexTupleProcessed;

extern bool index_keytest(IndexTuple tuple, TupleDesc tupdesc,
			  int scanKeySize, ScanKey key);

#endif	/* IQUAL_H */
