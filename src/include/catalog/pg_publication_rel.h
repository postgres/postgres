/*-------------------------------------------------------------------------
 *
 * pg_publication_rel.h
 *	  definition of the publication to relation map (pg_publication_rel)
  *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_publication_rel.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_PUBLICATION_REL_H
#define PG_PUBLICATION_REL_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_publication_rel definition.  cpp turns this into
 *		typedef struct FormData_pg_publication_rel
 *
 * ----------------
 */
#define PublicationRelRelationId				6106

CATALOG(pg_publication_rel,6106)
{
	Oid			prpubid;		/* Oid of the publication */
	Oid			prrelid;		/* Oid of the relation */
} FormData_pg_publication_rel;

/* ----------------
 *		Form_pg_publication_rel corresponds to a pointer to a tuple with
 *		the format of pg_publication_rel relation.
 * ----------------
 */
typedef FormData_pg_publication_rel *Form_pg_publication_rel;

/* ----------------
 *		compiler constants for pg_publication_rel
 * ----------------
 */

#define Natts_pg_publication_rel				2
#define Anum_pg_publication_rel_prpubid			1
#define Anum_pg_publication_rel_prrelid			2

#endif							/* PG_PUBLICATION_REL_H */
