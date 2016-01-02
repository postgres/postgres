/*-------------------------------------------------------------------------
 *
 * extension.h
 *		Extension management commands (create/drop extension).
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/extension.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENSION_H
#define EXTENSION_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"


/*
 * creating_extension is only true while running a CREATE EXTENSION command.
 * It instructs recordDependencyOnCurrentExtension() to register a dependency
 * on the current pg_extension object for each SQL object created by its
 * installation script.
 */
extern PGDLLIMPORT bool creating_extension;
extern Oid	CurrentExtensionObject;


extern ObjectAddress CreateExtension(CreateExtensionStmt *stmt);

extern void RemoveExtensionById(Oid extId);

extern ObjectAddress InsertExtensionTuple(const char *extName, Oid extOwner,
					 Oid schemaOid, bool relocatable, const char *extVersion,
					 Datum extConfig, Datum extCondition,
					 List *requiredExtensions);

extern ObjectAddress ExecAlterExtensionStmt(AlterExtensionStmt *stmt);

extern ObjectAddress ExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt,
							   ObjectAddress *objAddress);

extern Oid	get_extension_oid(const char *extname, bool missing_ok);
extern char *get_extension_name(Oid ext_oid);

extern ObjectAddress AlterExtensionNamespace(List *names, const char *newschema,
						Oid *oldschema);

extern void AlterExtensionOwner_oid(Oid extensionOid, Oid newOwnerId);

#endif   /* EXTENSION_H */
