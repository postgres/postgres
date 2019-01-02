/*-------------------------------------------------------------------------
 *
 * pg_replication_origin.h
 *	  definition of the "replication origin" system catalog
 *	  (pg_replication_origin)
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_replication_origin.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_REPLICATION_ORIGIN_H
#define PG_REPLICATION_ORIGIN_H

#include "catalog/genbki.h"
#include "catalog/pg_replication_origin_d.h"

#include "access/xlogdefs.h"

/* ----------------
 *		pg_replication_origin.  cpp turns this into
 *		typedef struct FormData_pg_replication_origin
 * ----------------
 */
CATALOG(pg_replication_origin,6000,ReplicationOriginRelationId) BKI_SHARED_RELATION
{
	/*
	 * Locally known id that get included into WAL.
	 *
	 * This should never leave the system.
	 *
	 * Needs to fit into an uint16, so we don't waste too much space in WAL
	 * records. For this reason we don't use a normal Oid column here, since
	 * we need to handle allocation of new values manually.
	 */
	Oid			roident;

	/*
	 * Variable-length fields start here, but we allow direct access to
	 * roname.
	 */

	/* external, free-format, name */
	text		roname BKI_FORCE_NOT_NULL;

#ifdef CATALOG_VARLEN			/* further variable-length fields */
#endif
} FormData_pg_replication_origin;

typedef FormData_pg_replication_origin *Form_pg_replication_origin;

#endif							/* PG_REPLICATION_ORIGIN_H */
