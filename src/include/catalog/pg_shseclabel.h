/* -------------------------------------------------------------------------
 *
 * pg_shseclabel.h
 *	  definition of the system "security label" relation (pg_shseclabel)
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * -------------------------------------------------------------------------
 */
#ifndef PG_SHSECLABEL_H
#define PG_SHSECLABEL_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_shseclabel definition. cpp turns this into
 *		typedef struct FormData_pg_shseclabel
 * ----------------
 */
#define SharedSecLabelRelationId		3592

CATALOG(pg_shseclabel,3592) BKI_SHARED_RELATION BKI_WITHOUT_OIDS
{
	Oid			objoid;			/* OID of the shared object itself */
	Oid			classoid;		/* OID of table containing the shared object */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text provider BKI_FORCE_NOT_NULL;	/* name of label provider */
	text label	BKI_FORCE_NOT_NULL;		/* security label of the object */
#endif
} FormData_pg_shseclabel;

/* ----------------
 *		compiler constants for pg_shseclabel
 * ----------------
 */
#define Natts_pg_shseclabel				4
#define Anum_pg_shseclabel_objoid		1
#define Anum_pg_shseclabel_classoid		2
#define Anum_pg_shseclabel_provider		3
#define Anum_pg_shseclabel_label		4

#endif   /* PG_SHSECLABEL_H */
