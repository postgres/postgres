/*-------------------------------------------------------------------------
 *
 * pg_ts_dict.h
 *	  definition of the "text search dictionary" system catalog (pg_ts_dict)
 *
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_ts_dict.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TS_DICT_H
#define PG_TS_DICT_H

#include "catalog/genbki.h"
#include "catalog/pg_ts_dict_d.h"

/* ----------------
 *		pg_ts_dict definition.  cpp turns this into
 *		typedef struct FormData_pg_ts_dict
 * ----------------
 */
CATALOG(pg_ts_dict,3600,TSDictionaryRelationId)
{
	/* oid */
	Oid			oid;

	/* dictionary name */
	NameData	dictname;

	/* name space */
	Oid			dictnamespace BKI_DEFAULT(PGNSP);

	/* owner */
	Oid			dictowner BKI_DEFAULT(PGUID);

	/* dictionary's template */
	Oid			dicttemplate BKI_LOOKUP(pg_ts_template);

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	/* options passed to dict_init() */
	text		dictinitoption;
#endif
} FormData_pg_ts_dict;

typedef FormData_pg_ts_dict *Form_pg_ts_dict;

#endif							/* PG_TS_DICT_H */
