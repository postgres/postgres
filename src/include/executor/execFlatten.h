/*-------------------------------------------------------------------------
 *
 * execFlatten.h
 *	  prototypes for execFlatten.c.
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execFlatten.h,v 1.12 2000/08/24 03:29:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECFLATTEN_H
#define EXECFLATTEN_H

#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"


extern Datum ExecEvalIter(Iter *iterNode, ExprContext *econtext,
						  bool *isNull, ExprDoneCond *isDone);

extern void ExecEvalFjoin(TargetEntry *tlist, ExprContext *econtext,
						  bool *isNullVect, ExprDoneCond *fj_isDone);


#endif	 /* EXECFLATTEN_H */
