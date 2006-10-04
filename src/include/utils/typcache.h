/*-------------------------------------------------------------------------
 *
 * typcache.h
 *	  Type cache definitions.
 *
 * The type cache exists to speed lookup of certain information about data
 * types that is not directly available from a type's pg_type row.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/utils/typcache.h,v 1.12 2006/10/04 00:30:11 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPCACHE_H
#define TYPCACHE_H

#include "access/tupdesc.h"
#include "fmgr.h"


typedef struct TypeCacheEntry
{
	/* typeId is the hash lookup key and MUST BE FIRST */
	Oid			type_id;		/* OID of the data type */

	/* some subsidiary information copied from the pg_type row */
	int16		typlen;
	bool		typbyval;
	char		typalign;
	char		typtype;
	Oid			typrelid;

	/*
	 * Information obtained from opclass entries
	 *
	 * These will be InvalidOid if no match could be found, or if the
	 * information hasn't yet been requested.
	 */
	Oid			btree_opc;		/* OID of the default btree opclass */
	Oid			hash_opc;		/* OID of the default hash opclass */
	Oid			eq_opr;			/* OID of the equality operator */
	Oid			lt_opr;			/* OID of the less-than operator */
	Oid			gt_opr;			/* OID of the greater-than operator */
	Oid			cmp_proc;		/* OID of the btree comparison function */

	/*
	 * Pre-set-up fmgr call info for the equality operator and the btree
	 * comparison function.  These are kept in the type cache to avoid
	 * problems with memory leaks in repeated calls to array_eq and array_cmp.
	 * There is not currently a need to maintain call info for the lt_opr or
	 * gt_opr.
	 */
	FmgrInfo	eq_opr_finfo;
	FmgrInfo	cmp_proc_finfo;

	/*
	 * Tuple descriptor if it's a composite type (row type).  NULL if not
	 * composite or information hasn't yet been requested.  (NOTE: this is a
	 * reference-counted tupledesc.)
	 */
	TupleDesc	tupDesc;
} TypeCacheEntry;

/* Bit flags to indicate which fields a given caller needs to have set */
#define TYPECACHE_EQ_OPR			0x0001
#define TYPECACHE_LT_OPR			0x0002
#define TYPECACHE_GT_OPR			0x0004
#define TYPECACHE_CMP_PROC			0x0008
#define TYPECACHE_EQ_OPR_FINFO		0x0010
#define TYPECACHE_CMP_PROC_FINFO	0x0020
#define TYPECACHE_TUPDESC			0x0040

extern TypeCacheEntry *lookup_type_cache(Oid type_id, int flags);

extern TupleDesc lookup_rowtype_tupdesc(Oid type_id, int32 typmod);

extern TupleDesc lookup_rowtype_tupdesc_noerror(Oid type_id, int32 typmod,
							   bool noError);

extern TupleDesc lookup_rowtype_tupdesc_copy(Oid type_id, int32 typmod);

extern void assign_record_type_typmod(TupleDesc tupDesc);

extern void flush_rowtype_cache(Oid type_id);

#endif   /* TYPCACHE_H */
