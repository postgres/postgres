/*-------------------------------------------------------------------------
 *
 * pg_ts_config_map.h
 *	definition of token mappings for configurations of tsearch
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_ts_config_map.h,v 1.3 2008/01/01 19:45:57 momjian Exp $
 *
 * NOTES
 *		the genbki.sh script reads this file and generates .bki
 *		information from the DATA() statements.
 *
 *		XXX do NOT break up DATA() statements into multiple lines!
 *			the scripts are not as smart as you might think...
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TS_CONFIG_MAP_H
#define PG_TS_CONFIG_MAP_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BKI_BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_ts_config_map definition.  cpp turns this into
 *		typedef struct FormData_pg_ts_config_map
 * ----------------
 */
#define TSConfigMapRelationId	3603

CATALOG(pg_ts_config_map,3603) BKI_WITHOUT_OIDS
{
	Oid			mapcfg;			/* OID of configuration owning this entry */
	int4		maptokentype;	/* token type from parser */
	int4		mapseqno;		/* order in which to consult dictionaries */
	Oid			mapdict;		/* dictionary to consult */
} FormData_pg_ts_config_map;

typedef FormData_pg_ts_config_map *Form_pg_ts_config_map;

/* ----------------
 *		compiler constants for pg_ts_config_map
 * ----------------
 */
#define Natts_pg_ts_config_map				4
#define Anum_pg_ts_config_map_mapcfg		1
#define Anum_pg_ts_config_map_maptokentype	2
#define Anum_pg_ts_config_map_mapseqno		3
#define Anum_pg_ts_config_map_mapdict		4

/* ----------------
 *		initial contents of pg_ts_config_map
 * ----------------
 */

DATA(insert ( 3748	1	1	3765 ));
DATA(insert ( 3748	2	1	3765 ));
DATA(insert ( 3748	3	1	3765 ));
DATA(insert ( 3748	4	1	3765 ));
DATA(insert ( 3748	5	1	3765 ));
DATA(insert ( 3748	6	1	3765 ));
DATA(insert ( 3748	7	1	3765 ));
DATA(insert ( 3748	8	1	3765 ));
DATA(insert ( 3748	9	1	3765 ));
DATA(insert ( 3748	10	1	3765 ));
DATA(insert ( 3748	11	1	3765 ));
DATA(insert ( 3748	15	1	3765 ));
DATA(insert ( 3748	16	1	3765 ));
DATA(insert ( 3748	17	1	3765 ));
DATA(insert ( 3748	18	1	3765 ));
DATA(insert ( 3748	19	1	3765 ));
DATA(insert ( 3748	20	1	3765 ));
DATA(insert ( 3748	21	1	3765 ));
DATA(insert ( 3748	22	1	3765 ));

#endif   /* PG_TS_CONFIG_MAP_H */
