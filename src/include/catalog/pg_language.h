/*-------------------------------------------------------------------------
 *
 * pg_language.h
 *	  definition of the system "language" relation (pg_language)
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_language.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_LANGUAGE_H
#define PG_LANGUAGE_H

#include "catalog/genbki.h"
#include "catalog/pg_language_d.h"

/* ----------------
 *		pg_language definition.  cpp turns this into
 *		typedef struct FormData_pg_language
 * ----------------
 */
CATALOG(pg_language,2612,LanguageRelationId)
{
	NameData	lanname;		/* Language name */
	Oid			lanowner;		/* Language's owner */
	bool		lanispl;		/* Is a procedural language */
	bool		lanpltrusted;	/* PL is trusted */
	Oid			lanplcallfoid;	/* Call handler for PL */
	Oid			laninline;		/* Optional anonymous-block handler function */
	Oid			lanvalidator;	/* Optional validation function */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	aclitem		lanacl[1];		/* Access privileges */
#endif
} FormData_pg_language;

/* ----------------
 *		Form_pg_language corresponds to a pointer to a tuple with
 *		the format of pg_language relation.
 * ----------------
 */
typedef FormData_pg_language *Form_pg_language;

#endif							/* PG_LANGUAGE_H */
