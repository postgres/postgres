/*-------------------------------------------------------------------------
 *
 * psort.h
 *	  Polyphase merge sort.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: psort.h,v 1.23 1999/10/16 19:49:28 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSORT_H
#define PSORT_H

#include "access/htup.h"
#include "access/skey.h"
#include "nodes/plannodes.h"

extern bool psort_begin(Sort *node, int nkeys, ScanKey key);
extern HeapTuple psort_grabtuple(Sort *node, bool *should_free);
extern void psort_markpos(Sort *node);
extern void psort_restorepos(Sort *node);
extern void psort_end(Sort *node);
extern void psort_rescan(Sort *node);

#endif	 /* PSORT_H */
