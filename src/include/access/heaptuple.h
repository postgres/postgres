/*-------------------------------------------------------------------------
 *
 * heaptuple.h--
 *    
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: heaptuple.h,v 1.1 1996/10/18 17:58:33 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	HEAPTUPLE_H
#define HEAPTUPLE_H

extern char *heap_getattr(HeapTuple tup,
                          Buffer b,
                          int attnum,
                          TupleDesc tupleDesc,
                          bool *isnull);

#endif	/* HEAP_TUPLE_H */ 
