/*-------------------------------------------------------------------------
 *
 * inval.h
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: inval.h,v 1.24 2002/03/03 17:47:56 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INVAL_H
#define INVAL_H

#include "access/htup.h"


extern void AcceptInvalidationMessages(void);

extern void AtEOXactInvalidationMessages(bool isCommit);

extern void CommandEndInvalidationMessages(bool isCommit);

extern void CacheInvalidateHeapTuple(Relation relation, HeapTuple tuple);

extern void CacheInvalidateRelcache(Oid relationId);

#endif   /* INVAL_H */
