/*-------------------------------------------------------------------------
 *
 * palloc.h--
 *	  POSTGRES memory allocator definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: palloc.h,v 1.5 1997/09/08 02:39:53 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PALLOC_H
#define PALLOC_H

#include <c.h>

extern void *palloc(Size size);
extern void pfree(void *pointer);
extern void *repalloc(void *pointer, Size size);

/* like strdup except uses palloc */
extern char *pstrdup(char *pointer);

#endif							/* PALLOC_H */
