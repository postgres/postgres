/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in lib/catalog/catalog.c
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catalog.h,v 1.11 2000/04/09 04:43:14 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "access/tupdesc.h"

extern char *relpath(const char *relname);
extern char *relpath_blind(const char *dbname, const char *relname,
						   Oid dbid, Oid relid);
extern bool IsSystemRelationName(const char *relname);
extern bool IsSharedSystemRelationName(const char *relname);
extern Oid	newoid(void);
extern void fillatt(TupleDesc att);

#endif	 /* CATALOG_H */
