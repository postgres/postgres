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

//#define ENCRYPTION_DEBUG 1
//#define KEYRING_DEBUG 1
//#define TDE_FORK_DEBUG 1
// #define TDE_XLOG_DEBUG 1

#define tdeheap_fill_tuple heap_fill_tuple
#define tdeheap_form_tuple heap_form_tuple
#define tdeheap_deform_tuple heap_deform_tuple
#define tdeheap_freetuple heap_freetuple
#define tdeheap_compute_data_size heap_compute_data_size
#define tdeheap_getattr heap_getattr
#define tdeheap_copytuple heap_copytuple
#define tdeheap_getsysattr heap_getsysattr

#define pgstat_count_tdeheap_scan pgstat_count_heap_scan
#define pgstat_count_tdeheap_fetch pgstat_count_heap_fetch
#define pgstat_count_tdeheap_getnext pgstat_count_heap_getnext
#define pgstat_count_tdeheap_update pgstat_count_heap_update
#define pgstat_count_tdeheap_delete pgstat_count_heap_delete
#define pgstat_count_tdeheap_insert pgstat_count_heap_insert

#define TDE_PageAddItem(rel, oid, blkno, page, item, size, offsetNumber, overwrite, is_heap) \
	PGTdePageAddItemExtended(rel, oid, blkno, page, item, size, offsetNumber, \
						((overwrite) ? PAI_OVERWRITE : 0) | \
						((is_heap) ? PAI_IS_HEAP : 0))

/* ---------- */

#endif                          /* PG_TDE_DEFINES_H */
