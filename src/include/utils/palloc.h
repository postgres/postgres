/*-------------------------------------------------------------------------
 *
 * palloc.h--
 *    POSTGRES memory allocator definitions.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: palloc.h,v 1.2 1996/10/31 09:51:28 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	PALLOC_H
#define PALLOC_H

extern void*   palloc(Size size);
extern void    pfree(void *pointer); 
extern void *repalloc(void *pointer, Size size);

/* like strdup except uses palloc */
extern char* pstrdup(char* pointer);

#endif	/* PALLOC_H */

