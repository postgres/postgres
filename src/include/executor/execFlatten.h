/*-------------------------------------------------------------------------
 *
 * execFlatten.h
 *	  prototypes for execFlatten.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execFlatten.h,v 1.10 1999/07/16 17:07:31 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECFLATTEN_H
#define EXECFLATTEN_H

#include "nodes/execnodes.h"
#include "nodes/relation.h"

extern Datum ExecEvalIter(Iter *iterNode, ExprContext *econtext, bool *resultIsNull, bool *iterIsDone);

extern void ExecEvalFjoin(TargetEntry *tlist, ExprContext *econtext, bool *isNullVect, bool *fj_isDone);


#endif	 /* EXECFLATTEN_H */
