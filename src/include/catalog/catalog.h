/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in lib/catalog/catalog.c
 *
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catalog.h,v 1.14 2000/12/27 23:59:13 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "access/tupdesc.h"

#ifdef OLD_FILE_NAMING

extern char *relpath(const char *relname);
extern char *relpath_blind(const char *dbname, const char *relname,
			  Oid dbid, Oid relid);
#else
#include "storage/relfilenode.h"

extern char *relpath(RelFileNode rnode);
extern char *GetDatabasePath(Oid tblNode);
#endif

extern bool IsSystemRelationName(const char *relname);
extern bool IsSharedSystemRelationName(const char *relname);

extern Oid	newoid(void);

#endif	 /* CATALOG_H */
