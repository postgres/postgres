/*-------------------------------------------------------------------------
 *
 * pg_ts_dict.h
 *	definition of dictionaries for tsearch
 *
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
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
	NameData	dictname;		/* dictionary name */
	Oid			dictnamespace;	/* name space */
	Oid			dictowner;		/* owner */
	Oid			dicttemplate;	/* dictionary's template */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		dictinitoption; /* options passed to dict_init() */
#endif
} FormData_pg_ts_dict;

typedef FormData_pg_ts_dict *Form_pg_ts_dict;

#endif							/* PG_TS_DICT_H */
