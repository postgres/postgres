/*-------------------------------------------------------------------------
 *
 * pg_index.h
 *	  definition of the "index" system catalog (pg_index)
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_index.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_INDEX_H
#define PG_INDEX_H

#include "catalog/genbki.h"
#include "catalog/pg_index_d.h"

/* ----------------
 *		pg_index definition.  cpp turns this into
 *		typedef struct FormData_pg_index.
 * ----------------
 */
CATALOG(pg_index,2610,IndexRelationId) BKI_SCHEMA_MACRO
{
	Oid			indexrelid;		/* OID of the index */
	Oid			indrelid;		/* OID of the relation it indexes */
	int16		indnatts;		/* total number of columns in index */
	int16		indnkeyatts;	/* number of key columns in index */
	bool		indisunique;	/* is this a unique index? */
	bool		indisprimary;	/* is this index for primary key? */
	bool		indisexclusion; /* is this index for exclusion constraint? */
	bool		indimmediate;	/* is uniqueness enforced immediately? */
	bool		indisclustered; /* is this the index last clustered by? */
	bool		indisvalid;		/* is this index valid for use by queries? */
	bool		indcheckxmin;	/* must we wait for xmin to be old? */
	bool		indisready;		/* is this index ready for inserts? */
	bool		indislive;		/* is this index alive at all? */
	bool		indisreplident; /* is this index the identity for replication? */

	/* variable-length fields start here, but we allow direct access to indkey */
	int2vector	indkey;			/* column numbers of indexed cols, or 0 */

#ifdef CATALOG_VARLEN
	oidvector	indcollation;	/* collation identifiers */
	oidvector	indclass;		/* opclass identifiers */
	int2vector	indoption;		/* per-column flags (AM-specific meanings) */
	pg_node_tree indexprs;		/* expression trees for index attributes that
								 * are not simple column references; one for
								 * each zero entry in indkey[] */
	pg_node_tree indpred;		/* expression tree for predicate, if a partial
								 * index; else NULL */
#endif
} FormData_pg_index;

/* ----------------
 *		Form_pg_index corresponds to a pointer to a tuple with
 *		the format of pg_index relation.
 * ----------------
 */
typedef FormData_pg_index *Form_pg_index;

#ifdef EXPOSE_TO_CLIENT_CODE

/*
 * Index AMs that support ordered scans must support these two indoption
 * bits.  Otherwise, the content of the per-column indoption fields is
 * open for future definition.
 */
#define INDOPTION_DESC			0x0001	/* values are in reverse order */
#define INDOPTION_NULLS_FIRST	0x0002	/* NULLs are first instead of last */

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_INDEX_H */
