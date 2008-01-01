/*-------------------------------------------------------------------------
 *
 * pg_autovacuum.h
 *	  definition of the system "autovacuum" relation (pg_autovacuum)
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_autovacuum.h,v 1.8 2008/01/01 19:45:56 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AUTOVACUUM_H
#define PG_AUTOVACUUM_H

/* ----------------
 *		postgres.h contains the system type definitions and the
 *		CATALOG(), BOOTSTRAP and DATA() sugar words so this file
 *		can be read by both genbki.sh and the C compiler.
 * ----------------
 */

/* ----------------
 *		pg_autovacuum definition.	cpp turns this into
 *		typedef struct FormData_pg_autovacuum
 * ----------------
 */
#define AutovacuumRelationId	1248
CATALOG(pg_autovacuum,1248) BKI_WITHOUT_OIDS
{
	Oid			vacrelid;		/* OID of table */
	bool		enabled;		/* enabled for this table? */
	int4		vac_base_thresh;	/* base threshold value */
	float4		vac_scale_factor;		/* reltuples scaling factor */
	int4		anl_base_thresh;	/* base threshold value */
	float4		anl_scale_factor;		/* reltuples scaling factor */
	int4		vac_cost_delay; /* vacuum cost-based delay */
	int4		vac_cost_limit; /* vacuum cost limit */
	int4		freeze_min_age; /* vacuum min freeze age */
	int4		freeze_max_age; /* max age before forcing vacuum */
} FormData_pg_autovacuum;

/* ----------------
 *		Form_pg_autovacuum corresponds to a pointer to a tuple with
 *		the format of pg_autovacuum relation.
 * ----------------
 */
typedef FormData_pg_autovacuum *Form_pg_autovacuum;

/* ----------------
 *		compiler constants for pg_autovacuum
 * ----------------
 */
#define Natts_pg_autovacuum							10
#define Anum_pg_autovacuum_vacrelid					1
#define Anum_pg_autovacuum_enabled					2
#define Anum_pg_autovacuum_vac_base_thresh			3
#define Anum_pg_autovacuum_vac_scale_factor			4
#define Anum_pg_autovacuum_anl_base_thresh			5
#define Anum_pg_autovacuum_anl_scale_factor			6
#define Anum_pg_autovacuum_vac_cost_delay			7
#define Anum_pg_autovacuum_vac_cost_limit			8
#define Anum_pg_autovacuum_freeze_min_age			9
#define Anum_pg_autovacuum_freeze_max_age			10

/* There are no preloaded tuples in pg_autovacuum.h */

#endif   /* PG_AUTOVACUUM_H */
