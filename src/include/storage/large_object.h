/*-------------------------------------------------------------------------
 *
 * large_object.h
 *	  file of info for Postgres large objects. POSTGRES 4.2 supports
 *	  zillions of large objects (internal, external, jaquith, inversion).
 *	  Now we only support inversion.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id: large_object.h,v 1.26 2003/08/04 02:40:14 momjian Exp $
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
 * table and index of a specific large object.	Now they all live in one rel.
 *----------
 */
typedef struct LargeObjectDesc
{
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
 * We could set this as high as BLCKSZ less some overhead, but it seems
 * better to make it a smaller value, so that not as much space is used
 * up when a page-tuple is updated.  Note that the value is deliberately
 * chosen large enough to trigger the tuple toaster, so that we will
 * attempt to compress page tuples in-line.  (But they won't be moved off
 * unless the user creates a toast-table for pg_largeobject...)
 *
 * Also, it seems to be a smart move to make the page size be a power of 2,
 * since clients will often be written to send data in power-of-2 blocks.
 * This avoids unnecessary tuple updates caused by partial-page writes.
 */
#define LOBLKSIZE		(BLCKSZ / 4)


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

#endif   /* LARGE_OBJECT_H */
