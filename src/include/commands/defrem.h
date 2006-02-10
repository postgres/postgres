/*-------------------------------------------------------------------------
 *
 * defrem.h
 *	  POSTGRES define and remove utility definitions.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/commands/defrem.h,v 1.68.2.1 2006/02/10 19:01:22 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef DEFREM_H
#define DEFREM_H

#include "nodes/parsenodes.h"


/* commands/indexcmds.c */
extern void DefineIndex(RangeVar *heapRelation,
			char *indexRelationName,
			Oid indexRelationId,
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
extern void ReindexIndex(RangeVar *indexRelation);
extern void ReindexTable(RangeVar *relation);
extern void ReindexDatabase(const char *databaseName,
				bool do_system, bool do_user);
extern char *makeObjectName(const char *name1, const char *name2,
			   const char *label);
extern char *ChooseRelationName(const char *name1, const char *name2,
				   const char *label, Oid namespace);
extern Oid	GetDefaultOpClass(Oid type_id, Oid am_id);

/* commands/functioncmds.c */
extern void CreateFunction(CreateFunctionStmt *stmt);
extern void RemoveFunction(RemoveFuncStmt *stmt);
extern void RemoveFunctionById(Oid funcOid);
extern void SetFunctionReturnType(Oid funcOid, Oid newRetType);
extern void SetFunctionArgType(Oid funcOid, int argIndex, Oid newArgType);
extern void RenameFunction(List *name, List *argtypes, const char *newname);
extern void AlterFunctionOwner(List *name, List *argtypes, Oid newOwnerId);
extern void AlterFunction(AlterFunctionStmt *stmt);
extern void CreateCast(CreateCastStmt *stmt);
extern void DropCast(DropCastStmt *stmt);
extern void DropCastById(Oid castOid);
extern void AlterFunctionNamespace(List *name, List *argtypes,
					   const char *newschema);

/* commands/operatorcmds.c */
extern void DefineOperator(List *names, List *parameters);
extern void RemoveOperator(RemoveOperStmt *stmt);
extern void RemoveOperatorById(Oid operOid);
extern void AlterOperatorOwner(List *name, TypeName *typeName1,
				   TypeName *typename2, Oid newOwnerId);

/* commands/aggregatecmds.c */
extern void DefineAggregate(List *names, List *parameters);
extern void RemoveAggregate(RemoveAggrStmt *stmt);
extern void RenameAggregate(List *name, TypeName *basetype, const char *newname);
extern void AlterAggregateOwner(List *name, TypeName *basetype, Oid newOwnerId);

/* commands/opclasscmds.c */
extern void DefineOpClass(CreateOpClassStmt *stmt);
extern void RemoveOpClass(RemoveOpClassStmt *stmt);
extern void RemoveOpClassById(Oid opclassOid);
extern void RenameOpClass(List *name, const char *access_method, const char *newname);
extern void AlterOpClassOwner(List *name, const char *access_method, Oid newOwnerId);

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
