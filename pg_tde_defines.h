/*-------------------------------------------------------------------------
 *
 * pg_tde_io.h
 *    POSTGRES heap access method input/output definitions.
 *
 *
 * Portions Copyright (c) 1996-2023, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/hio.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_TDE_DEFINES_H
#define PG_TDE_DEFINES_H

/* ----------
 * Defines for functions that may need to replace later on
 * ----------
*/

#define pg_tde_fill_tuple heap_fill_tuple
#define pg_tde_form_tuple heap_form_tuple
#define pg_tde_deform_tuple heap_deform_tuple
#define pg_tde_freetuple heap_freetuple
#define pg_tde_compute_data_size heap_compute_data_size

#define pgstat_count_pg_tde_scan pgstat_count_heap_scan
#define pgstat_count_pg_tde_fetch pgstat_count_heap_fetch
#define pgstat_count_pg_tde_getnext pgstat_count_heap_getnext
#define pgstat_count_pg_tde_update pgstat_count_heap_update
#define pgstat_count_pg_tde_delete pgstat_count_heap_delete
#define pgstat_count_pg_tde_insert pgstat_count_heap_insert
#define pg_tde_getattr heap_getattr

#define GetPGTdeamTableAmRoutine GetHeapamTableAmRoutine

/* ---------- */

#endif                          /* PG_TDE_DEFINES_H */
