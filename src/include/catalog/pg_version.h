/*-------------------------------------------------------------------------
 *
 * pg_version.h
 *	  definition of the system "version" relation (pg_version)
 *	  along with the relation's initial contents.
 *
 * NOTE: this table has nothing to do with the overall Postgres system
 * version or anything like that.  It is for defining individual relations
 * that have multiple concurrently-existing versions.  Yes, there used to
 * be such a feature in Postgres, but it's been broken for a long time
 * (see src/backend/commands/_deadcode/version.c).	The pg_version table
 * isn't even created at present.
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_version.h,v 1.17 2003/08/04 02:40:12 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_VERSION_H
#define PG_VERSION_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_version definition.	cpp turns this into
 *		typedef struct FormData_pg_version
 * ----------------
 */
CATALOG(pg_version)
{
	Oid			verrelid;
	Oid			verbaseid;
	int4		vertime;		/* really should be some abstime */
} FormData_pg_version;

/* ----------------
 *		Form_pg_version corresponds to a pointer to a tuple with
 *		the format of pg_version relation.
 * ----------------
 */
typedef FormData_pg_version *Form_pg_version;

/* ----------------
 *		compiler constants for pg_version
 * ----------------
 */
#define Natts_pg_version				3
#define Anum_pg_version_verrelid		1
#define Anum_pg_version_verbaseid		2
#define Anum_pg_version_vertime			3

#endif   /* PG_VERSION_H */
