/*-------------------------------------------------------------------------
 *
 * pg_zstd_dictionaries.h
 *	  definition of the "zstd dictionay" system catalog (pg_zstd_dictionaries)
 *
 * src/include/catalog/pg_zstd_dictionaries.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ZSTD_DICTIONARIES_H
#define PG_ZSTD_DICTIONARIES_H

#include "catalog/genbki.h"
#include "catalog/pg_zstd_dictionaries_d.h"
#include "access/attnum.h"

/* ----------------
 *		pg_zstd_dictionaries definition.  cpp turns this into
 *		typedef struct FormData_pg_zstd_dictionaries
 * ----------------
 */

CATALOG(pg_zstd_dictionaries,9946,ZstdDictionariesRelationId)
{
    Oid         dictid  BKI_FORCE_NOT_NULL;
    
    /*
	 * variable-length fields start here, but we allow direct access to dict
	 */
    bytea       dict    BKI_FORCE_NOT_NULL; 
} FormData_pg_zstd_dictionaries;

/* Pointer type to a tuple with the format of pg_zstd_dictionaries relation */
typedef FormData_pg_zstd_dictionaries *Form_pg_zstd_dictionaries;

DECLARE_TOAST(pg_zstd_dictionaries, 9947, 9948);

DECLARE_UNIQUE_INDEX(pg_zstd_dictionaries_dictid_index, 9949, ZstdDictidIndexId, pg_zstd_dictionaries, btree(dictid oid_ops));

MAKE_SYSCACHE(ZSTDDICTIDOID, pg_zstd_dictionaries_dictid_index, 128);

typedef struct ZstdTrainingData
{
    char    *sample_buffer;      /* Pointer to the raw sample buffer */
    size_t  *sample_sizes;       /* Array of sample sizes */
    int     nitems;              /* Number of sample sizes */
} ZstdTrainingData;

#endif  /* PG_ZSTD_DICTIONARIES_H */
