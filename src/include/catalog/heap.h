/*-------------------------------------------------------------------------
 *
 * heap.h
 *	  prototypes for functions in lib/catalog/heap.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: heap.h,v 1.19 1999/05/25 16:13:39 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef HEAP_H
#define HEAP_H

#include <utils/rel.h>

extern Oid	RelnameFindRelid(char *relname);
extern Relation heap_create(char *relname, TupleDesc att,
			bool isnoname, bool istemp);

extern Oid heap_create_with_catalog(char *relname,
						 TupleDesc tupdesc, char relkind, bool istemp);

extern void heap_destroy_with_catalog(char *relname);
extern void heap_destroy(Relation rel);

extern void InitNoNameRelList(void);
extern void DestroyNoNameRels(void);

#endif	 /* HEAP_H */
