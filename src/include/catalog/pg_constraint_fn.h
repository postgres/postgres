/*-------------------------------------------------------------------------
 *
 * pg_constraint_fn.h
 *	 prototypes for functions in catalog/pg_constraint.c
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_constraint_fn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CONSTRAINT_FN_H
#define PG_CONSTRAINT_FN_H

#include "catalog/dependency.h"
#include "nodes/pg_list.h"

/*
 * Identify constraint type for lookup purposes
 */
typedef enum ConstraintCategory
{
	CONSTRAINT_RELATION,
	CONSTRAINT_DOMAIN,
	CONSTRAINT_ASSERTION		/* for future expansion */
} ConstraintCategory;

extern Oid CreateConstraintEntry(const char *constraintName,
					  Oid constraintNamespace,
					  char constraintType,
					  bool isDeferrable,
					  bool isDeferred,
					  bool isValidated,
					  Oid relId,
					  const int16 *constraintKey,
					  int constraintNKeys,
					  Oid domainId,
					  Oid indexRelId,
					  Oid foreignRelId,
					  const int16 *foreignKey,
					  const Oid *pfEqOp,
					  const Oid *ppEqOp,
					  const Oid *ffEqOp,
					  int foreignNKeys,
					  char foreignUpdateType,
					  char foreignDeleteType,
					  char foreignMatchType,
					  const Oid *exclOp,
					  Node *conExpr,
					  const char *conBin,
					  const char *conSrc,
					  bool conIsLocal,
					  int conInhCount,
					  bool conNoInherit,
					  bool is_internal);

extern void RemoveConstraintById(Oid conId);
extern void RenameConstraintById(Oid conId, const char *newname);
extern void SetValidatedConstraintById(Oid conId);

extern bool ConstraintNameIsUsed(ConstraintCategory conCat, Oid objId,
					 Oid objNamespace, const char *conname);
extern char *ChooseConstraintName(const char *name1, const char *name2,
					 const char *label, Oid namespaceid,
					 List *others);

extern void AlterConstraintNamespaces(Oid ownerId, Oid oldNspId,
					  Oid newNspId, bool isType, ObjectAddresses *objsMoved);
extern Oid	get_relation_constraint_oid(Oid relid, const char *conname, bool missing_ok);
extern Oid	get_domain_constraint_oid(Oid typid, const char *conname, bool missing_ok);

extern Bitmapset *get_primary_key_attnos(Oid relid, bool deferrableOk,
					   Oid *constraintOid);

extern bool check_functional_grouping(Oid relid,
						  Index varno, Index varlevelsup,
						  List *grouping_columns,
						  List **constraintDeps);

#endif   /* PG_CONSTRAINT_FN_H */
