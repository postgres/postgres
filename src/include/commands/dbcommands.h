/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Portions Copyright (c) 1996-2002, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.27 2003/06/27 14:45:31 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

#include "nodes/parsenodes.h"

extern void createdb(const CreatedbStmt *stmt);
extern void dropdb(const char *dbname);
extern void RenameDatabase(const char *oldname, const char *newname);
extern void AlterDatabaseSet(AlterDatabaseSetStmt *stmt);

extern Oid get_database_oid(const char *dbname);
extern char * get_database_name(Oid dbid);

#endif   /* DBCOMMANDS_H */
