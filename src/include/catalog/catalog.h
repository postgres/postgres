/*-------------------------------------------------------------------------
 *
 * catalog.h--
 *    prototypes for functions in lib/catalog/catalog.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catalog.h,v 1.2 1996/11/05 10:37:14 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include <access/tupdesc.h>

extern char *relpath(char relname[]);
extern bool IsSystemRelationName(char *relname);
extern bool IsSharedSystemRelationName(char *relname);
extern Oid newoid(void);
extern void fillatt(TupleDesc att);

#endif	/* CATALOG_H */
