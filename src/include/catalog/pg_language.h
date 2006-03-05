/*-------------------------------------------------------------------------
 *
 * pg_language.h
 *	  definition of the system "language" relation (pg_language)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_language.h,v 1.28 2006/03/05 15:58:54 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LANGUAGE_H
#define PG_LANGUAGE_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_language definition.  cpp turns this into
 *		typedef struct FormData_pg_language
 * ----------------
 */
#define LanguageRelationId	2612

CATALOG(pg_language,2612)
{
	NameData	lanname;
	bool		lanispl;		/* Is a procedural language */
	bool		lanpltrusted;	/* PL is trusted */
	Oid			lanplcallfoid;	/* Call handler for PL */
	Oid			lanvalidator;	/* optional validation function */
	aclitem		lanacl[1];		/* Access privileges */
} FormData_pg_language;

/* ----------------
 *		Form_pg_language corresponds to a pointer to a tuple with
 *		the format of pg_language relation.
 * ----------------
 */
typedef FormData_pg_language *Form_pg_language;

/* ----------------
 *		compiler constants for pg_language
 * ----------------
 */
#define Natts_pg_language				6
#define Anum_pg_language_lanname		1
#define Anum_pg_language_lanispl		2
#define Anum_pg_language_lanpltrusted		3
#define Anum_pg_language_lanplcallfoid		4
#define Anum_pg_language_lanvalidator		5
#define Anum_pg_language_lanacl			6

/* ----------------
 *		initial contents of pg_language
 * ----------------
 */

DATA(insert OID = 12 ( "internal" f f 0 2246 _null_ ));
DESCR("Built-in functions");
#define INTERNALlanguageId 12
DATA(insert OID = 13 ( "c" f f 0 2247 _null_ ));
DESCR("Dynamically-loaded C functions");
#define ClanguageId 13
DATA(insert OID = 14 ( "sql" f t 0 2248 _null_ ));
DESCR("SQL-language functions");
#define SQLlanguageId 14

#endif   /* PG_LANGUAGE_H */
