/*-------------------------------------------------------------------------
 *
 * tablespace.h
 *		Tablespace management commands (create/drop tablespace).
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/tablespace.h,v 1.19 2008/01/01 19:45:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLESPACE_H
#define TABLESPACE_H

#include "access/xlog.h"
#include "nodes/parsenodes.h"

/* XLOG stuff */
#define XLOG_TBLSPC_CREATE		0x00
#define XLOG_TBLSPC_DROP		0x10

typedef struct xl_tblspc_create_rec
{
	Oid			ts_id;
	char		ts_path[1];		/* VARIABLE LENGTH STRING */
} xl_tblspc_create_rec;

typedef struct xl_tblspc_drop_rec
{
	Oid			ts_id;
} xl_tblspc_drop_rec;


extern void CreateTableSpace(CreateTableSpaceStmt *stmt);
extern void DropTableSpace(DropTableSpaceStmt *stmt);
extern void RenameTableSpace(const char *oldname, const char *newname);
extern void AlterTableSpaceOwner(const char *name, Oid newOwnerId);

extern void TablespaceCreateDbspace(Oid spcNode, Oid dbNode, bool isRedo);

extern Oid	GetDefaultTablespace(bool forTemp);

extern void PrepareTempTablespaces(void);

extern Oid	get_tablespace_oid(const char *tablespacename);
extern char *get_tablespace_name(Oid spc_oid);

extern bool directory_is_empty(const char *path);

extern void tblspc_redo(XLogRecPtr lsn, XLogRecord *rptr);
extern void tblspc_desc(StringInfo buf, uint8 xl_info, char *rec);

#endif   /* TABLESPACE_H */
