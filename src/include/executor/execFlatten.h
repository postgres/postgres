/*-------------------------------------------------------------------------
 *
 * execFlatten.h--
 *	  prototypes for execFlatten.c.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execFlatten.h,v 1.7 1998/09/01 04:35:44 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXECFLATTEN_H
#define EXECFLATTEN_H

#include "nodes/relation.h"
#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"

extern Datum ExecEvalIter(Iter *iterNode, ExprContext *econtext, bool *resultIsNull, bool *iterIsDone);

extern void ExecEvalFjoin(TargetEntry *tlist, ExprContext *econtext, bool *isNullVect, bool *fj_isDone);


#endif	 /* EXECFLATTEN_H */
