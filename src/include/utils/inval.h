/*-------------------------------------------------------------------------
 *
 * inval.h
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: inval.h,v 1.20 2001/06/19 19:42:16 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INVAL_H
#define INVAL_H

#include "access/htup.h"


extern void AcceptInvalidationMessages(void);

extern void AtEOXactInvalidationMessages(bool isCommit);

extern void CommandEndInvalidationMessages(bool isCommit);

extern void RelationInvalidateHeapTuple(Relation relation, HeapTuple tuple);

extern void RelationMark4RollbackHeapTuple(Relation relation, HeapTuple tuple);

#endif	 /* INVAL_H */
