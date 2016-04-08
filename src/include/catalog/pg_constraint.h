/*-------------------------------------------------------------------------
 *
 * pg_constraint.h
 *	  definition of the system "constraint" relation (pg_constraint)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_constraint.h
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
	bool		convalidated;	/* constraint has been validated? */

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
	int32		coninhcount;

	/* Has a local definition and cannot be inherited */
	bool		connoinherit;

#ifdef CATALOG_VARLEN			/* variable-length fields start here */

	/*
	 * Columns of conrelid that the constraint applies to, if known (this is
	 * NULL for trigger constraints)
	 */
	int16		conkey[1];

	/*
	 * If a foreign key, the referenced columns of confrelid
	 */
	int16		confkey[1];

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
	pg_node_tree conbin;

	/*
	 * If a check constraint, source-text representation of expression
	 */
	text		consrc;
#endif
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
#define Natts_pg_constraint					24
#define Anum_pg_constraint_conname			1
#define Anum_pg_constraint_connamespace		2
#define Anum_pg_constraint_contype			3
#define Anum_pg_constraint_condeferrable	4
#define Anum_pg_constraint_condeferred		5
#define Anum_pg_constraint_convalidated		6
#define Anum_pg_constraint_conrelid			7
#define Anum_pg_constraint_contypid			8
#define Anum_pg_constraint_conindid			9
#define Anum_pg_constraint_confrelid		10
#define Anum_pg_constraint_confupdtype		11
#define Anum_pg_constraint_confdeltype		12
#define Anum_pg_constraint_confmatchtype	13
#define Anum_pg_constraint_conislocal		14
#define Anum_pg_constraint_coninhcount		15
#define Anum_pg_constraint_connoinherit		16
#define Anum_pg_constraint_conkey			17
#define Anum_pg_constraint_confkey			18
#define Anum_pg_constraint_conpfeqop		19
#define Anum_pg_constraint_conppeqop		20
#define Anum_pg_constraint_conffeqop		21
#define Anum_pg_constraint_conexclop		22
#define Anum_pg_constraint_conbin			23
#define Anum_pg_constraint_consrc			24

/* ----------------
 *		initial contents of pg_constraint
 * ----------------
 */

/* nothing, at present */


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

#endif   /* PG_CONSTRAINT_H */
