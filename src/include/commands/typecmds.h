/*-------------------------------------------------------------------------
 *
 * typecmds.h
 *	  prototypes for typecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
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
#include "nodes/parsenodes.h"


#define DEFAULT_TYPDELIM		','

extern void DefineType(List *names, List *parameters);
extern void RemoveTypeById(Oid typeOid);
extern void DefineDomain(CreateDomainStmt *stmt);
extern void DefineEnum(CreateEnumStmt *stmt);
extern void DefineRange(CreateRangeStmt *stmt);
extern void AlterEnum(AlterEnumStmt *stmt);
extern Oid	DefineCompositeType(RangeVar *typevar, List *coldeflist);
extern Oid	AssignTypeArrayOid(void);

extern void AlterDomainDefault(List *names, Node *defaultRaw);
extern void AlterDomainNotNull(List *names, bool notNull);
extern void AlterDomainAddConstraint(List *names, Node *constr);
extern void AlterDomainValidateConstraint(List *names, char *constrName);
extern void AlterDomainDropConstraint(List *names, const char *constrName,
						  DropBehavior behavior, bool missing_ok);

extern void checkDomainOwner(HeapTuple tup);

extern List *GetDomainConstraints(Oid typeOid);

extern void RenameType(RenameStmt *stmt);
extern void AlterTypeOwner(List *names, Oid newOwnerId, ObjectType objecttype);
extern void AlterTypeOwnerInternal(Oid typeOid, Oid newOwnerId,
					   bool hasDependEntry);
extern void AlterTypeNamespace(List *names, const char *newschema, ObjectType objecttype);
extern Oid	AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, ObjectAddresses *objsMoved);
extern Oid	AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
						   bool isImplicitArray,
						   bool errorOnTableType,
						   ObjectAddresses *objsMoved);

#endif   /* TYPECMDS_H */
