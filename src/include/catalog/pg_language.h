/*-------------------------------------------------------------------------
 *
 * pg_language.h--
 *    definition of the system "language" relation (pg_language)
 *    along with the relation's initial contents.
 *
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: pg_language.h,v 1.1 1996/08/28 01:56:57 scrappy Exp $
 *
 * NOTES
 *    the genbki.sh script reads this file and generates .bki
 *    information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LANGUAGE_H
#define PG_LANGUAGE_H

/* ----------------
 *	postgres.h contains the system type definintions and the
 *	CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *	can be read by both genbki.sh and the C compiler.
 * ----------------
 */
#include "postgres.h"

/* ----------------
 *	pg_language definition.  cpp turns this into
 *	typedef struct FormData_pg_language
 * ----------------
 */ 
CATALOG(pg_language) {
    NameData 	lanname;
    text 	lancompiler;	/* VARIABLE LENGTH FIELD */
} FormData_pg_language;

/* ----------------
 *	Form_pg_language corresponds to a pointer to a tuple with
 *	the format of pg_language relation.
 * ----------------
 */
typedef FormData_pg_language	*Form_pg_language;

/* ----------------
 *	compiler constants for pg_language
 * ----------------
 */
#define Natts_pg_language		2
#define Anum_pg_language_lanname	1
#define Anum_pg_language_lancompiler	2

/* ----------------
 *	initial contents of pg_language
 * ----------------
 */

DATA(insert OID = 11 ( internal "n/a" ));
#define INTERNALlanguageId 11
DATA(insert OID = 12 ( lisp "/usr/ucb/liszt" ));
DATA(insert OID = 13 ( "C" "/bin/cc" ));
#define ClanguageId 13
DATA(insert OID = 14 ( "sql" "postgres"));
#define SQLlanguageId 14

    
#endif /* PG_LANGUAGE_H */







