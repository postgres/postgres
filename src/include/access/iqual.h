/*-------------------------------------------------------------------------
 *
 * iqual.h
 *	  Index scan key qualification definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: iqual.h,v 1.13 1999/07/16 17:07:27 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef IQUAL_H
#define IQUAL_H

#include "access/itup.h"
#include "access/skey.h"


/* ----------------
 *		index tuple qualification support
 * ----------------
 */

extern int	NIndexTupleProcessed;

extern bool index_keytest(IndexTuple tuple, TupleDesc tupdesc,
			  int scanKeySize, ScanKey key);

#endif	 /* IQUAL_H */
