/*-------------------------------------------------------------------------
 *
 * schemacmds.h
 *	  prototypes for schemacmds.c.
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/schemacmds.h,v 1.18 2008/01/01 19:45:57 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#ifndef SCHEMACMDS_H
#define SCHEMACMDS_H

#include "nodes/parsenodes.h"

extern void CreateSchemaCommand(CreateSchemaStmt *parsetree,
					const char *queryString);

extern void RemoveSchema(List *names, DropBehavior behavior, bool missing_ok);
extern void RemoveSchemaById(Oid schemaOid);

extern void RenameSchema(const char *oldname, const char *newname);
extern void AlterSchemaOwner(const char *name, Oid newOwnerId);
extern void AlterSchemaOwner_oid(Oid schemaOid, Oid newOwnerId);

#endif   /* SCHEMACMDS_H */
