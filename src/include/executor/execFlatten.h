/*-------------------------------------------------------------------------
 *
 * execFlatten.h
 *	  prototypes for execFlatten.c.
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: execFlatten.h,v 1.16 2001/11/05 17:46:33 momjian Exp $
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

#endif   /* EXECFLATTEN_H */
