/*-------------------------------------------------------------------------
 *
 * heap.h--
 *	  prototypes for functions in lib/catalog/heap.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: heap.h,v 1.6 1997/09/08 02:34:50 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAP_H
#define HEAP_H

#include <utils/rel.h>

extern Relation heap_creatr(char *relname, unsigned smgr, TupleDesc att);

extern		Oid
heap_create(char relname[],
			char *typename,
			int arch,
			unsigned smgr, TupleDesc tupdesc);

extern void heap_destroy(char relname[]);
extern void heap_destroyr(Relation r);

extern void InitTempRelList(void);
extern void DestroyTempRels(void);

#endif							/* HEAP_H */
