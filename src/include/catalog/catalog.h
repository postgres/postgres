/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in lib/catalog/catalog.c
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catalog.h,v 1.9 2000/01/16 20:04:57 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "access/tupdesc.h"

extern char *relpath(const char *relname);
extern bool IsSystemRelationName(const char *relname);
extern bool IsSharedSystemRelationName(const char *relname);
extern Oid	newoid(void);
extern void fillatt(TupleDesc att);

#endif	 /* CATALOG_H */
