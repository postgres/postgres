/*-------------------------------------------------------------------------
 *
 * dbcommands.h
 *		Database management commands (create/drop database).
 *
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/dbcommands.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DBCOMMANDS_H
#define DBCOMMANDS_H

#include "access/xlog.h"
#include "nodes/parsenodes.h"

/* XLOG stuff */
#define XLOG_DBASE_CREATE		0x00
#define XLOG_DBASE_DROP			0x10

typedef struct xl_dbase_create_rec_old
{
	/* Records copying of a single subdirectory incl. contents */
	Oid			db_id;
	char		src_path[1];	/* VARIABLE LENGTH STRING */
	/* dst_path follows src_path */
}	xl_dbase_create_rec_old;

typedef struct xl_dbase_drop_rec_old
{
	/* Records dropping of a single subdirectory incl. contents */
	Oid			db_id;
	char		dir_path[1];	/* VARIABLE LENGTH STRING */
}	xl_dbase_drop_rec_old;

typedef struct xl_dbase_create_rec
{
	/* Records copying of a single subdirectory incl. contents */
	Oid			db_id;
	Oid			tablespace_id;
	Oid			src_db_id;
	Oid			src_tablespace_id;
} xl_dbase_create_rec;

typedef struct xl_dbase_drop_rec
{
	/* Records dropping of a single subdirectory incl. contents */
	Oid			db_id;
	Oid			tablespace_id;
} xl_dbase_drop_rec;

extern Oid	createdb(const CreatedbStmt *stmt);
extern void dropdb(const char *dbname, bool missing_ok);
extern Oid	RenameDatabase(const char *oldname, const char *newname);
extern Oid	AlterDatabase(AlterDatabaseStmt *stmt, bool isTopLevel);
extern Oid	AlterDatabaseSet(AlterDatabaseSetStmt *stmt);
extern Oid	AlterDatabaseOwner(const char *dbname, Oid newOwnerId);

extern Oid	get_database_oid(const char *dbname, bool missingok);
extern char *get_database_name(Oid dbid);

extern void dbase_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void dbase_desc(StringInfo buf, uint8 xl_info, char *rec);

extern void check_encoding_locale_matches(int encoding, const char *collate, const char *ctype);

#endif   /* DBCOMMANDS_H */
