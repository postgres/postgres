/*-------------------------------------------------------------------------
 *
 * tablespace.h
 *	  prototypes for tablespace.c.
 *
 *
 * Portions Copyright (c) 1996-2004, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/tablespace.h,v 1.1 2004/06/18 06:14:08 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLESPACE_H
#define TABLESPACE_H

#include "nodes/parsenodes.h"

extern void CreateTableSpace(CreateTableSpaceStmt *stmt);

extern void DropTableSpace(DropTableSpaceStmt *stmt);

extern void TablespaceCreateDbspace(Oid spcNode, Oid dbNode);

extern Oid	get_tablespace_oid(const char *tablespacename);

extern char *get_tablespace_name(Oid spc_oid);

#endif   /* TABLESPACE_H */
