/* -------------------------------------------------------------------------
 *
 * pg_seclabel.h
 *	  definition of the system "security label" relation (pg_seclabel)
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SECLABEL_H
#define PG_SECLABEL_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_seclabel definition.  cpp turns this into
 *		typedef struct FormData_pg_seclabel
 * ----------------
 */
#define SecLabelRelationId		3596

CATALOG(pg_seclabel,3596) BKI_WITHOUT_OIDS
{
	Oid			objoid;			/* OID of the object itself */
	Oid			classoid;		/* OID of table containing the object */
	int32		objsubid;		/* column number, or 0 if not used */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		provider;		/* name of label provider */
	text		label;			/* security label of the object */
#endif
} FormData_pg_seclabel;

/* ----------------
 *		compiler constants for pg_seclabel
 * ----------------
 */
#define Natts_pg_seclabel			5
#define Anum_pg_seclabel_objoid		1
#define Anum_pg_seclabel_classoid	2
#define Anum_pg_seclabel_objsubid	3
#define Anum_pg_seclabel_provider	4
#define Anum_pg_seclabel_label		5

#endif   /* PG_SECLABEL_H */
