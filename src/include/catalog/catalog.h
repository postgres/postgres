/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  prototypes for functions in backend/catalog/catalog.c
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: catalog.h,v 1.22 2002/03/22 21:34:44 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATALOG_H
#define CATALOG_H

#include "access/tupdesc.h"

#include "storage/relfilenode.h"

extern char *relpath(RelFileNode rnode);
extern char *GetDatabasePath(Oid tblNode);

extern bool IsSystemRelationName(const char *relname);
extern bool IsToastRelationName(const char *relname);
extern bool IsSharedSystemRelationName(const char *relname);

extern Oid	newoid(void);

#endif   /* CATALOG_H */
