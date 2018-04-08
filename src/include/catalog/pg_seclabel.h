/* -------------------------------------------------------------------------
 *
 * pg_seclabel.h
 *	  definition of the system "security label" relation (pg_seclabel)
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SECLABEL_H
#define PG_SECLABEL_H

#include "catalog/genbki.h"
#include "catalog/pg_seclabel_d.h"

/* ----------------
 *		pg_seclabel definition.  cpp turns this into
 *		typedef struct FormData_pg_seclabel
 * ----------------
 */
CATALOG(pg_seclabel,3596,SecLabelRelationId) BKI_WITHOUT_OIDS
{
	Oid			objoid;			/* OID of the object itself */
	Oid			classoid;		/* OID of table containing the object */
	int32		objsubid;		/* column number, or 0 if not used */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		provider BKI_FORCE_NOT_NULL;	/* name of label provider */
	text		label BKI_FORCE_NOT_NULL;	/* security label of the object */
#endif
} FormData_pg_seclabel;

#endif							/* PG_SECLABEL_H */
