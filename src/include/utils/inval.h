/*-------------------------------------------------------------------------
 *
 * inval.h
 *	  POSTGRES cache invalidation dispatcher definitions.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: inval.h,v 1.17 2000/06/08 19:51:06 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef INVAL_H
#define INVAL_H

#include "access/htup.h"

extern void DiscardInvalid(void);

extern void RegisterInvalid(bool send);

extern void ImmediateLocalInvalidation(bool send);

extern void RelationInvalidateHeapTuple(Relation relation, HeapTuple tuple);

extern void RelationMark4RollbackHeapTuple(Relation relation, HeapTuple tuple);

extern void ImmediateInvalidateSharedHeapTuple(Relation relation, HeapTuple tuple);

#endif	 /* INVAL_H */
