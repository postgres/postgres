/*-------------------------------------------------------------------------
 *
 * defrem.h
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/defrem.h,v 1.63 2005/03/14 00:19:37 neilc Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEFREM_H
#define DEFREM_H

#include "nodes/parsenodes.h"


/* commands/indexcmds.c */
extern void DefineIndex(RangeVar *heapRelation,
			char *indexRelationName,
			char *accessMethodName,
			char *tableSpaceName,
			List *attributeList,
			Expr *predicate,
			List *rangetable,
			bool unique,
			bool primary,
			bool isconstraint,
			bool is_alter_table,
			bool check_rights,
			bool skip_build,
			bool quiet);
extern void RemoveIndex(RangeVar *relation, DropBehavior behavior);
extern void ReindexIndex(RangeVar *indexRelation, bool force);
extern void ReindexTable(RangeVar *relation, bool force);
extern void ReindexDatabase(const char *databaseName, bool force, bool all);
extern char *makeObjectName(const char *name1, const char *name2,
			   const char *label);
extern char *ChooseRelationName(const char *name1, const char *name2,
				   const char *label, Oid namespace);

/* commands/functioncmds.c */
extern void CreateFunction(CreateFunctionStmt *stmt);
extern void RemoveFunction(RemoveFuncStmt *stmt);
extern void RemoveFunctionById(Oid funcOid);
extern void SetFunctionReturnType(Oid funcOid, Oid newRetType);
extern void SetFunctionArgType(Oid funcOid, int argIndex, Oid newArgType);
extern void RenameFunction(List *name, List *argtypes, const char *newname);
extern void AlterFunctionOwner(List *name, List *argtypes, AclId newOwnerSysId);
extern void AlterFunction(AlterFunctionStmt *stmt);
extern void CreateCast(CreateCastStmt *stmt);
extern void DropCast(DropCastStmt *stmt);
extern void DropCastById(Oid castOid);

/* commands/operatorcmds.c */
extern void DefineOperator(List *names, List *parameters);
extern void RemoveOperator(RemoveOperStmt *stmt);
extern void RemoveOperatorById(Oid operOid);
extern void AlterOperatorOwner(List *name, TypeName *typeName1,
				   TypeName *typename2, AclId newOwnerSysId);

/* commands/aggregatecmds.c */
extern void DefineAggregate(List *names, List *parameters);
extern void RemoveAggregate(RemoveAggrStmt *stmt);
extern void RenameAggregate(List *name, TypeName *basetype, const char *newname);
extern void AlterAggregateOwner(List *name, TypeName *basetype, AclId newOwnerSysId);

/* commands/opclasscmds.c */
extern void DefineOpClass(CreateOpClassStmt *stmt);
extern void RemoveOpClass(RemoveOpClassStmt *stmt);
extern void RemoveOpClassById(Oid opclassOid);
extern void RenameOpClass(List *name, const char *access_method, const char *newname);
extern void AlterOpClassOwner(List *name, const char *access_method, AclId newOwnerSysId);

/* support routines in commands/define.c */

extern char *case_translate_language_name(const char *input);

extern char *defGetString(DefElem *def);
extern double defGetNumeric(DefElem *def);
extern bool defGetBoolean(DefElem *def);
extern int64 defGetInt64(DefElem *def);
extern List *defGetQualifiedName(DefElem *def);
extern TypeName *defGetTypeName(DefElem *def);
extern int	defGetTypeLength(DefElem *def);

#endif   /* DEFREM_H */
