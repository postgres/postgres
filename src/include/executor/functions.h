/*-------------------------------------------------------------------------
 *
 * functions.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: functions.h,v 1.1 1996/08/28 07:22:12 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	FUNCTIONS_H
#define FUNCTIONS_H

extern Datum ProjectAttribute(TupleDesc TD, TargetEntry *tlist,
			      HeapTuple tup, bool *isnullP);

extern Datum postquel_function(Func *funcNode, char **args,
			       bool *isNull, bool *isDone);

#endif /* FUNCTIONS_H */
