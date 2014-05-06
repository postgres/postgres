/*-------------------------------------------------------------------------
 *
 * pg_trigger.h
 *	  definition of the system "trigger" relation (pg_trigger)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_trigger.h,v 1.39 2010/01/17 22:56:23 tgl Exp $
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TRIGGER_H
#define PG_TRIGGER_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_trigger definition.  cpp turns this into
 *		typedef struct FormData_pg_trigger
 *
 * Note: when tgconstraint is nonzero, tgconstrrelid, tgconstrindid,
 * tgdeferrable, and tginitdeferred are largely redundant with the referenced
 * pg_constraint entry.  However, it is possible for a non-deferrable trigger
 * to be associated with a deferrable constraint.
 * ----------------
 */
#define TriggerRelationId  2620

CATALOG(pg_trigger,2620)
{
	Oid			tgrelid;		/* relation trigger is attached to */
	NameData	tgname;			/* trigger's name */
	Oid			tgfoid;			/* OID of function to be called */
	int2		tgtype;			/* BEFORE/AFTER UPDATE/DELETE/INSERT
								 * ROW/STATEMENT; see below */
	char		tgenabled;		/* trigger's firing configuration WRT
								 * session_replication_role */
	bool		tgisinternal;	/* trigger is system-generated */
	Oid			tgconstrrelid;	/* constraint's FROM table, if any */
	Oid			tgconstrindid;	/* constraint's supporting index, if any */
	Oid			tgconstraint;	/* associated pg_constraint entry, if any */
	bool		tgdeferrable;	/* constraint trigger is deferrable */
	bool		tginitdeferred; /* constraint trigger is deferred initially */
	int2		tgnargs;		/* # of extra arguments in tgargs */

	/* VARIABLE LENGTH FIELDS (note: tgattr and tgargs must not be null) */
	int2vector	tgattr;			/* column numbers, if trigger is on columns */
	bytea		tgargs;			/* first\000second\000tgnargs\000 */
	text		tgqual;			/* WHEN expression, or NULL if none */
} FormData_pg_trigger;

/* ----------------
 *		Form_pg_trigger corresponds to a pointer to a tuple with
 *		the format of pg_trigger relation.
 * ----------------
 */
typedef FormData_pg_trigger *Form_pg_trigger;

/* ----------------
 *		compiler constants for pg_trigger
 * ----------------
 */
#define Natts_pg_trigger				15
#define Anum_pg_trigger_tgrelid			1
#define Anum_pg_trigger_tgname			2
#define Anum_pg_trigger_tgfoid			3
#define Anum_pg_trigger_tgtype			4
#define Anum_pg_trigger_tgenabled		5
#define Anum_pg_trigger_tgisinternal	6
#define Anum_pg_trigger_tgconstrrelid	7
#define Anum_pg_trigger_tgconstrindid	8
#define Anum_pg_trigger_tgconstraint	9
#define Anum_pg_trigger_tgdeferrable	10
#define Anum_pg_trigger_tginitdeferred	11
#define Anum_pg_trigger_tgnargs			12
#define Anum_pg_trigger_tgattr			13
#define Anum_pg_trigger_tgargs			14
#define Anum_pg_trigger_tgqual			15

/* Bits within tgtype */
#define TRIGGER_TYPE_ROW				(1 << 0)
#define TRIGGER_TYPE_BEFORE				(1 << 1)
#define TRIGGER_TYPE_INSERT				(1 << 2)
#define TRIGGER_TYPE_DELETE				(1 << 3)
#define TRIGGER_TYPE_UPDATE				(1 << 4)
#define TRIGGER_TYPE_TRUNCATE			(1 << 5)

/* Macros for manipulating tgtype */
#define TRIGGER_CLEAR_TYPE(type)		((type) = 0)

#define TRIGGER_SETT_ROW(type)			((type) |= TRIGGER_TYPE_ROW)
#define TRIGGER_SETT_BEFORE(type)		((type) |= TRIGGER_TYPE_BEFORE)
#define TRIGGER_SETT_INSERT(type)		((type) |= TRIGGER_TYPE_INSERT)
#define TRIGGER_SETT_DELETE(type)		((type) |= TRIGGER_TYPE_DELETE)
#define TRIGGER_SETT_UPDATE(type)		((type) |= TRIGGER_TYPE_UPDATE)
#define TRIGGER_SETT_TRUNCATE(type)		((type) |= TRIGGER_TYPE_TRUNCATE)

#define TRIGGER_FOR_ROW(type)			((type) & TRIGGER_TYPE_ROW)
#define TRIGGER_FOR_BEFORE(type)		((type) & TRIGGER_TYPE_BEFORE)
#define TRIGGER_FOR_INSERT(type)		((type) & TRIGGER_TYPE_INSERT)
#define TRIGGER_FOR_DELETE(type)		((type) & TRIGGER_TYPE_DELETE)
#define TRIGGER_FOR_UPDATE(type)		((type) & TRIGGER_TYPE_UPDATE)
#define TRIGGER_FOR_TRUNCATE(type)		((type) & TRIGGER_TYPE_TRUNCATE)

#endif   /* PG_TRIGGER_H */
