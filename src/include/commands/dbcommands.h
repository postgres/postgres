/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: dbcommands.h,v 1.21 2002/03/01 22:45:17 petere Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

#include <nodes/parsenodes.h>

extern void createdb(const char *dbname, const char *dbowner,
					 const char *dbpath, const char *dbtemplate,
					 int encoding);
extern void dropdb(const char *dbname);
extern void AlterDatabaseSet(AlterDatabaseSetStmt *stmt);

#endif   /* DBCOMMANDS_H */
