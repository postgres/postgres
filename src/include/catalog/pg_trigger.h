/*-------------------------------------------------------------------------
 *
 * pg_trigger.h--
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TRIGGER_H
#define PG_TRIGGER_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *	pg_trigger definition.  cpp turns this into
 *	typedef struct FormData_pg_trigger
 * ----------------
 */ 
CATALOG(pg_trigger) BOOTSTRAP {
    Oid		tgrelid;	/* triggered relation */
    NameData	tgname;		/* trigger' name */
    NameData	tgfunc;		/* name of function to be called */
    Oid		tglang;		/* Language. Only ClanguageId currently */
    int2	tgtype;		/* BEFORE/AFTER UPDATE/DELETE/INSERT ROW/STATEMENT */
    int2	tgnargs;	/* # of extra arguments in tgargs */
    int28	tgattr;		/* UPDATE of attr1, attr2 ... (NI) */
    bytea	tgtext;		/* currently, where to find .so */
    bytea	tgargs;		/* first\000second\000tgnargs\000 */
    text	tgwhen;		/* when (a > 10 or b = 3) fire trigger (NI) */
} FormData_pg_trigger;

/* ----------------
 *	Form_pg_trigger corresponds to a pointer to a tuple with
 *	the format of pg_trigger relation.
 * ----------------
 */
typedef FormData_pg_trigger	*Form_pg_trigger;

/* ----------------
 *	compiler constants for pg_trigger
 * ----------------
 */
#define Natts_pg_trigger		10
#define Anum_pg_trigger_tgrelid		1
#define Anum_pg_trigger_tgname		2
#define Anum_pg_trigger_tgfunc		3
#define Anum_pg_trigger_tglang		4
#define Anum_pg_trigger_tgtype		5
#define Anum_pg_trigger_tgnargs		6
#define Anum_pg_trigger_tgattr		7
#define Anum_pg_trigger_tgtext		8
#define Anum_pg_trigger_tgargs		9
#define Anum_pg_trigger_tgwhen		10

#define TRIGGER_TYPE_ROW		(1 << 0)
#define TRIGGER_TYPE_BEFORE		(1 << 1)
#define TRIGGER_TYPE_INSERT		(1 << 2)
#define TRIGGER_TYPE_DELETE		(1 << 3)
#define TRIGGER_TYPE_UPDATE		(1 << 4)

#define TRIGGER_CLEAR_TYPE(type)	(type = 0)

#define TRIGGER_SETT_ROW(type)		(type |= TRIGGER_TYPE_ROW)
#define TRIGGER_SETT_BEFORE(type)	(type |= TRIGGER_TYPE_BEFORE)
#define TRIGGER_SETT_INSERT(type)	(type |= TRIGGER_TYPE_INSERT)
#define TRIGGER_SETT_DELETE(type)	(type |= TRIGGER_TYPE_DELETE)
#define TRIGGER_SETT_UPDATE(type)	(type |= TRIGGER_TYPE_UPDATE)

#define TRIGGER_FOR_ROW(type)		(type & TRIGGER_TYPE_ROW)
#define TRIGGER_FOR_BEFORE(type)	(type & TRIGGER_TYPE_BEFORE)
#define TRIGGER_FOR_INSERT(type)	(type & TRIGGER_TYPE_INSERT)
#define TRIGGER_FOR_DELETE(type)	(type & TRIGGER_TYPE_DELETE)
#define TRIGGER_FOR_UPDATE(type)	(type & TRIGGER_TYPE_UPDATE)

#endif /* PG_TRIGGER_H */
