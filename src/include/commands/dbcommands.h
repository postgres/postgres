/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/dbcommands.h,v 1.32 2004/06/25 21:55:58 tgl Exp $
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
extern void AlterDatabaseOwner(const char *dbname, AclId newOwnerSysId);

extern Oid	get_database_oid(const char *dbname);
extern char *get_database_name(Oid dbid);

#endif   /* DBCOMMANDS_H */
