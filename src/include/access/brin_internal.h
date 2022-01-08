/*
 * brin_internal.h
 *		internal declarations for BRIN indexes
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/include/access/brin_internal.h
 */
#ifndef BRIN_INTERNAL_H
#define BRIN_INTERNAL_H

#include "access/amapi.h"
#include "storage/bufpage.h"
#include "utils/typcache.h"


/*
 * A BrinDesc is a struct designed to enable decoding a BRIN tuple from the
 * on-disk format to an in-memory tuple and vice-versa.
 */

/* struct returned by "OpcInfo" amproc */
typedef struct BrinOpcInfo
{
	/* Number of columns stored in an index column of this opclass */
	uint16		oi_nstored;

	/* Regular processing of NULLs in BrinValues? */
	bool		oi_regular_nulls;

	/* Opaque pointer for the opclass' private use */
	void	   *oi_opaque;

	/* Type cache entries of the stored columns */
	TypeCacheEntry *oi_typcache[FLEXIBLE_ARRAY_MEMBER];
} BrinOpcInfo;

/* the size of a BrinOpcInfo for the given number of columns */
#define SizeofBrinOpcInfo(ncols) \
	(offsetof(BrinOpcInfo, oi_typcache) + sizeof(TypeCacheEntry *) * ncols)

typedef struct BrinDesc
{
	/* Containing memory context */
	MemoryContext bd_context;

	/* the index relation itself */
	Relation	bd_index;

	/* tuple descriptor of the index relation */
	TupleDesc	bd_tupdesc;

	/* cached copy for on-disk tuples; generated at first use */
	TupleDesc	bd_disktdesc;

	/* total number of Datum entries that are stored on-disk for all columns */
	int			bd_totalstored;

	/* per-column info; bd_tupdesc->natts entries long */
	BrinOpcInfo *bd_info[FLEXIBLE_ARRAY_MEMBER];
} BrinDesc;

/*
 * Globally-known function support numbers for BRIN indexes.  Individual
 * opclasses can define more function support numbers, which must fall into
 * BRIN_FIRST_OPTIONAL_PROCNUM .. BRIN_LAST_OPTIONAL_PROCNUM.
 */
#define BRIN_PROCNUM_OPCINFO		1
#define BRIN_PROCNUM_ADDVALUE		2
#define BRIN_PROCNUM_CONSISTENT		3
#define BRIN_PROCNUM_UNION			4
#define BRIN_MANDATORY_NPROCS		4
#define BRIN_PROCNUM_OPTIONS 		5	/* optional */
/* procedure numbers up to 10 are reserved for BRIN future expansion */
#define BRIN_FIRST_OPTIONAL_PROCNUM 11
#define BRIN_LAST_OPTIONAL_PROCNUM	15

#undef BRIN_DEBUG

#ifdef BRIN_DEBUG
#define BRIN_elog(args)			elog args
#else
#define BRIN_elog(args)			((void) 0)
#endif

/* brin.c */
extern BrinDesc *brin_build_desc(Relation rel);
extern void brin_free_desc(BrinDesc *bdesc);
extern IndexBuildResult *brinbuild(Relation heap, Relation index,
								   struct IndexInfo *indexInfo);
extern void brinbuildempty(Relation index);
extern bool brininsert(Relation idxRel, Datum *values, bool *nulls,
					   ItemPointer heaptid, Relation heapRel,
					   IndexUniqueCheck checkUnique,
					   bool indexUnchanged,
					   struct IndexInfo *indexInfo);
extern IndexScanDesc brinbeginscan(Relation r, int nkeys, int norderbys);
extern int64 bringetbitmap(IndexScanDesc scan, TIDBitmap *tbm);
extern void brinrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
					   ScanKey orderbys, int norderbys);
extern void brinendscan(IndexScanDesc scan);
extern IndexBulkDeleteResult *brinbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);
extern IndexBulkDeleteResult *brinvacuumcleanup(IndexVacuumInfo *info,
												IndexBulkDeleteResult *stats);
extern bytea *brinoptions(Datum reloptions, bool validate);

/* brin_validate.c */
extern bool brinvalidate(Oid opclassoid);

#endif							/* BRIN_INTERNAL_H */
