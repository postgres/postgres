/*-------------------------------------------------------------------------
 *
 * pg_constraint.h
 *	  definition of the system "constraint" relation (pg_constraint)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_constraint.h,v 1.39 2010/03/11 03:36:22 tgl Exp $
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CONSTRAINT_H
#define PG_CONSTRAINT_H

#include "catalog/genbki.h"
#include "nodes/pg_list.h"

/* ----------------
 *		pg_constraint definition.  cpp turns this into
 *		typedef struct FormData_pg_constraint
 * ----------------
 */
#define ConstraintRelationId  2606

CATALOG(pg_constraint,2606)
{
	/*
	 * conname + connamespace is deliberately not unique; we allow, for
	 * example, the same name to be used for constraints of different
	 * relations.  This is partly for backwards compatibility with past
	 * Postgres practice, and partly because we don't want to have to obtain a
	 * global lock to generate a globally unique name for a nameless
	 * constraint.  We associate a namespace with constraint names only for
	 * SQL-spec compatibility.
	 */
	NameData	conname;		/* name of this constraint */
	Oid			connamespace;	/* OID of namespace containing constraint */
	char		contype;		/* constraint type; see codes below */
	bool		condeferrable;	/* deferrable constraint? */
	bool		condeferred;	/* deferred by default? */

	/*
	 * conrelid and conkey are only meaningful if the constraint applies to a
	 * specific relation (this excludes domain constraints and assertions).
	 * Otherwise conrelid is 0 and conkey is NULL.
	 */
	Oid			conrelid;		/* relation this constraint constrains */

	/*
	 * contypid links to the pg_type row for a domain if this is a domain
	 * constraint.  Otherwise it's 0.
	 *
	 * For SQL-style global ASSERTIONs, both conrelid and contypid would be
	 * zero. This is not presently supported, however.
	 */
	Oid			contypid;		/* domain this constraint constrains */

	/*
	 * conindid links to the index supporting the constraint, if any;
	 * otherwise it's 0.  This is used for unique, primary-key, and exclusion
	 * constraints, and less obviously for foreign-key constraints (where the
	 * index is a unique index on the referenced relation's referenced
	 * columns).  Notice that the index is on conrelid in the first case but
	 * confrelid in the second.
	 */
	Oid			conindid;		/* index supporting this constraint */

	/*
	 * These fields, plus confkey, are only meaningful for a foreign-key
	 * constraint.  Otherwise confrelid is 0 and the char fields are spaces.
	 */
	Oid			confrelid;		/* relation referenced by foreign key */
	char		confupdtype;	/* foreign key's ON UPDATE action */
	char		confdeltype;	/* foreign key's ON DELETE action */
	char		confmatchtype;	/* foreign key's match type */

	/* Has a local definition (hence, do not drop when coninhcount is 0) */
	bool		conislocal;

	/* Number of times inherited from direct parent relation(s) */
	int4		coninhcount;

	/*
	 * VARIABLE LENGTH FIELDS start here.  These fields may be NULL, too.
	 */

	/*
	 * Columns of conrelid that the constraint applies to, if known (this is
	 * NULL for trigger constraints)
	 */
	int2		conkey[1];

	/*
	 * If a foreign key, the referenced columns of confrelid
	 */
	int2		confkey[1];

	/*
	 * If a foreign key, the OIDs of the PK = FK equality operators for each
	 * column of the constraint
	 */
	Oid			conpfeqop[1];

	/*
	 * If a foreign key, the OIDs of the PK = PK equality operators for each
	 * column of the constraint (i.e., equality for the referenced columns)
	 */
	Oid			conppeqop[1];

	/*
	 * If a foreign key, the OIDs of the FK = FK equality operators for each
	 * column of the constraint (i.e., equality for the referencing columns)
	 */
	Oid			conffeqop[1];

	/*
	 * If an exclusion constraint, the OIDs of the exclusion operators for
	 * each column of the constraint
	 */
	Oid			conexclop[1];

	/*
	 * If a check constraint, nodeToString representation of expression
	 */
	text		conbin;

	/*
	 * If a check constraint, source-text representation of expression
	 */
	text		consrc;
} FormData_pg_constraint;

/* ----------------
 *		Form_pg_constraint corresponds to a pointer to a tuple with
 *		the format of pg_constraint relation.
 * ----------------
 */
typedef FormData_pg_constraint *Form_pg_constraint;

/* ----------------
 *		compiler constants for pg_constraint
 * ----------------
 */
#define Natts_pg_constraint					22
#define Anum_pg_constraint_conname			1
#define Anum_pg_constraint_connamespace		2
#define Anum_pg_constraint_contype			3
#define Anum_pg_constraint_condeferrable	4
#define Anum_pg_constraint_condeferred		5
#define Anum_pg_constraint_conrelid			6
#define Anum_pg_constraint_contypid			7
#define Anum_pg_constraint_conindid			8
#define Anum_pg_constraint_confrelid		9
#define Anum_pg_constraint_confupdtype		10
#define Anum_pg_constraint_confdeltype		11
#define Anum_pg_constraint_confmatchtype	12
#define Anum_pg_constraint_conislocal		13
#define Anum_pg_constraint_coninhcount		14
#define Anum_pg_constraint_conkey			15
#define Anum_pg_constraint_confkey			16
#define Anum_pg_constraint_conpfeqop		17
#define Anum_pg_constraint_conppeqop		18
#define Anum_pg_constraint_conffeqop		19
#define Anum_pg_constraint_conexclop		20
#define Anum_pg_constraint_conbin			21
#define Anum_pg_constraint_consrc			22


/* Valid values for contype */
#define CONSTRAINT_CHECK			'c'
#define CONSTRAINT_FOREIGN			'f'
#define CONSTRAINT_PRIMARY			'p'
#define CONSTRAINT_UNIQUE			'u'
#define CONSTRAINT_TRIGGER			't'
#define CONSTRAINT_EXCLUSION		'x'

/*
 * Valid values for confupdtype and confdeltype are the FKCONSTR_ACTION_xxx
 * constants defined in parsenodes.h.  Valid values for confmatchtype are
 * the FKCONSTR_MATCH_xxx constants defined in parsenodes.h.
 */

/*
 * Identify constraint type for lookup purposes
 */
typedef enum ConstraintCategory
{
	CONSTRAINT_RELATION,
	CONSTRAINT_DOMAIN,
	CONSTRAINT_ASSERTION		/* for future expansion */
} ConstraintCategory;

/*
 * prototypes for functions in pg_constraint.c
 */
extern Oid CreateConstraintEntry(const char *constraintName,
					  Oid constraintNamespace,
					  char constraintType,
					  bool isDeferrable,
					  bool isDeferred,
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
					  int conInhCount);

extern void RemoveConstraintById(Oid conId);
extern void RenameConstraintById(Oid conId, const char *newname);

extern bool ConstraintNameIsUsed(ConstraintCategory conCat, Oid objId,
					 Oid objNamespace, const char *conname);
extern char *ChooseConstraintName(const char *name1, const char *name2,
					 const char *label, Oid namespaceid,
					 List *others);

extern void AlterConstraintNamespaces(Oid ownerId, Oid oldNspId,
						  Oid newNspId, bool isType);
extern Oid	GetConstraintByName(Oid relid, const char *conname);
extern void get_constraint_relation_oids(Oid constraint_oid, Oid *conrelid, Oid *confrelid);

#endif   /* PG_CONSTRAINT_H */
