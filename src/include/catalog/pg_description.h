/*-------------------------------------------------------------------------
 *
 * pg_description.h
 *	  definition of the system "description" relation (pg_description)
 *
 * NOTE: an object is identified by the OID of the row that primarily
 * defines the object, plus the OID of the table that that row appears in.
 * For example, a function is identified by the OID of its pg_proc row
 * plus the pg_class OID of table pg_proc.	This allows unique identification
 * of objects without assuming that OIDs are unique across tables.
 *
 * Since attributes don't have OIDs of their own, we identify an attribute
 * comment by the objoid+classoid of its parent table, plus an "objsubid"
 * giving the attribute column number.	"objsubid" must be zero in a comment
 * for a table itself, so that it is distinct from any column comment.
 * Currently, objsubid is unused and zero for all other kinds of objects,
 * but perhaps it might be useful someday to associate comments with
 * constituent elements of other kinds of objects (arguments of a function,
 * for example).
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_description.h,v 1.19 2003/08/04 02:40:12 momjian Exp $
 *
 * NOTES
 *		the genbki.sh script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DESCRIPTION_H
#define PG_DESCRIPTION_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_description definition.	cpp turns this into
 *		typedef struct FormData_pg_description
 * ----------------
 */
CATALOG(pg_description) BKI_WITHOUT_OIDS
{
	Oid			objoid;			/* OID of object itself */
	Oid			classoid;		/* OID of table containing object */
	int4		objsubid;		/* column number, or 0 if not used */
	text		description;	/* description of object */
} FormData_pg_description;

/* ----------------
 *		Form_pg_description corresponds to a pointer to a tuple with
 *		the format of pg_description relation.
 * ----------------
 */
typedef FormData_pg_description *Form_pg_description;

/* ----------------
 *		compiler constants for pg_description
 * ----------------
 */
#define Natts_pg_description			4
#define Anum_pg_description_objoid		1
#define Anum_pg_description_classoid	2
#define Anum_pg_description_objsubid	3
#define Anum_pg_description_description 4

/* ----------------
 *		initial contents of pg_description
 * ----------------
 */

/*
 *	Because the contents of this table are taken from the other *.h files,
 *	there is no initialization here.  The initial contents are extracted
 *	by genbki.sh and loaded during initdb.
 */

#endif   /* PG_DESCRIPTION_H */
