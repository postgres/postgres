/*-------------------------------------------------------------------------
 *
 * alter.h
 *	  prototypes for commands/alter.c
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/alter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ALTER_H
#define ALTER_H

#include "nodes/parsenodes.h"
#include "utils/relcache.h"

extern void ExecRenameStmt(RenameStmt *stmt);
extern void ExecAlterObjectSchemaStmt(AlterObjectSchemaStmt *stmt);
extern Oid	AlterObjectNamespace_oid(Oid classId, Oid objid, Oid nspOid);
extern Oid	AlterObjectNamespace_internal(Relation rel, Oid objid, Oid nspOid);
extern void ExecAlterOwnerStmt(AlterOwnerStmt *stmt);

#endif   /* ALTER_H */
