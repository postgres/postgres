/*-------------------------------------------------------------------------
 *
 * typecmds.h
 *	  prototypes for typecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/typecmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPECMDS_H
#define TYPECMDS_H

#include "access/htup.h"
#include "catalog/dependency.h"
#include "parser/parse_node.h"


#define DEFAULT_TYPDELIM		','

extern ObjectAddress DefineType(ParseState *pstate, List *names, List *parameters);
extern void RemoveTypeById(Oid typeOid);
extern ObjectAddress DefineDomain(ParseState *pstate, CreateDomainStmt *stmt);
extern ObjectAddress DefineEnum(CreateEnumStmt *stmt);
extern ObjectAddress DefineRange(ParseState *pstate, CreateRangeStmt *stmt);
extern ObjectAddress AlterEnum(AlterEnumStmt *stmt);
extern ObjectAddress DefineCompositeType(RangeVar *typevar, List *coldeflist);
extern Oid	AssignTypeArrayOid(void);
extern Oid	AssignTypeMultirangeOid(void);
extern Oid	AssignTypeMultirangeArrayOid(void);

extern ObjectAddress AlterDomainDefault(List *names, Node *defaultRaw);
extern ObjectAddress AlterDomainNotNull(List *names, bool notNull);
extern ObjectAddress AlterDomainAddConstraint(List *names, Node *newConstraint,
											  ObjectAddress *constrAddr);
extern ObjectAddress AlterDomainValidateConstraint(List *names, const char *constrName);
extern ObjectAddress AlterDomainDropConstraint(List *names, const char *constrName,
											   DropBehavior behavior, bool missing_ok);

extern void checkDomainOwner(HeapTuple tup);

extern ObjectAddress RenameType(RenameStmt *stmt);

extern ObjectAddress AlterTypeOwner(List *names, Oid newOwnerId, ObjectType objecttype);
extern void AlterTypeOwner_oid(Oid typeOid, Oid newOwnerId, bool hasDependEntry);
extern void AlterTypeOwnerInternal(Oid typeOid, Oid newOwnerId);

extern ObjectAddress AlterTypeNamespace(List *names, const char *newschema,
										ObjectType objecttype, Oid *oldschema);
extern Oid	AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, bool ignoreDependent,
								   ObjectAddresses *objsMoved);
extern Oid	AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
									   bool isImplicitArray,
									   bool ignoreDependent,
									   bool errorOnTableType,
									   ObjectAddresses *objsMoved);

extern ObjectAddress AlterType(AlterTypeStmt *stmt);

#endif							/* TYPECMDS_H */
