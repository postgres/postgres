/*-------------------------------------------------------------------------
 *
 * heap.h--
 *	  prototypes for functions in lib/catalog/heap.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: heap.h,v 1.9 1997/11/28 04:40:40 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAP_H
#define HEAP_H

#include <utils/rel.h>

extern Relation heap_create(char *relname, TupleDesc att);

extern Oid
heap_create_and_catalog(char relname[],	TupleDesc tupdesc);

extern void heap_destroy(char relname[]);
extern void heap_destroyr(Relation r);

extern void InitTempRelList(void);
extern void DestroyTempRels(void);

#endif							/* HEAP_H */
