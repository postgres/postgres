/*-------------------------------------------------------------------------
 *
 * large_object.h
 *	  file of info for Postgres large objects. POSTGRES 4.2 supports
 *	  zillions of large objects (internal, external, jaquith, inversion).
 *	  Now we only support inversion.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: large_object.h,v 1.18 2000/10/24 01:38:43 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LARGE_OBJECT_H
#define LARGE_OBJECT_H

#include "utils/rel.h"


/*----------
 * Data about a currently-open large object.
 *
 * id is the logical OID of the large object
 * offset is the current seek offset within the LO
 * heap_r holds an open-relation reference to pg_largeobject
 * index_r holds an open-relation reference to pg_largeobject_loid_pn_index
 *
 * NOTE: before 7.1, heap_r and index_r held references to the separate
 * table and index of a specific large object.  Now they all live in one rel.
 *----------
 */
typedef struct LargeObjectDesc {
	Oid			id;
	uint32		offset;			/* current seek pointer */
	int			flags;			/* locking info, etc */

/* flag bits: */
#define IFS_RDLOCK		(1 << 0)
#define IFS_WRLOCK		(1 << 1)

	Relation	heap_r;
	Relation	index_r;
} LargeObjectDesc;


/*
 * Each "page" (tuple) of a large object can hold this much data
 *
 * Calculation is max tuple size less tuple header, loid field (Oid),
 * pageno field (int32), and varlena header of data (int32).  Note we
 * assume none of the fields will be NULL, hence no need for null bitmap.
 */
#define	LOBLKSIZE		(MaxTupleSize \
						 - MAXALIGN(offsetof(HeapTupleHeaderData, t_bits)) \
						 - sizeof(Oid) - sizeof(int32) * 2)


/*
 * Function definitions...
 */

/* inversion stuff in inv_api.c */
extern LargeObjectDesc *inv_create(int flags);
extern LargeObjectDesc *inv_open(Oid lobjId, int flags);
extern void inv_close(LargeObjectDesc *obj_desc);
extern int	inv_drop(Oid lobjId);
extern int	inv_seek(LargeObjectDesc *obj_desc, int offset, int whence);
extern int	inv_tell(LargeObjectDesc *obj_desc);
extern int	inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes);
extern int	inv_write(LargeObjectDesc *obj_desc, char *buf, int nbytes);

#endif	 /* LARGE_OBJECT_H */
