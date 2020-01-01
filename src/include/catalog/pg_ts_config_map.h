/*-------------------------------------------------------------------------
 *
 * pg_ts_config_map.h
 *	  definition of the system catalog for text search token mappings
 *	  (pg_ts_config_map)
 *
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_ts_config_map.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TS_CONFIG_MAP_H
#define PG_TS_CONFIG_MAP_H

#include "catalog/genbki.h"
#include "catalog/pg_ts_config_map_d.h"

/* ----------------
 *		pg_ts_config_map definition.  cpp turns this into
 *		typedef struct FormData_pg_ts_config_map
 * ----------------
 */
CATALOG(pg_ts_config_map,3603,TSConfigMapRelationId)
{
	/* OID of configuration owning this entry */
	Oid			mapcfg BKI_LOOKUP(pg_ts_config);

	/* token type from parser */
	int32		maptokentype;

	/* order in which to consult dictionaries */
	int32		mapseqno;

	/* dictionary to consult */
	Oid			mapdict BKI_LOOKUP(pg_ts_dict);
} FormData_pg_ts_config_map;

typedef FormData_pg_ts_config_map *Form_pg_ts_config_map;

#endif							/* PG_TS_CONFIG_MAP_H */
