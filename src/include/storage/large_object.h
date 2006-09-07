/*-------------------------------------------------------------------------
 *
 * large_object.h
 *	  Declarations for PostgreSQL large objects.  POSTGRES 4.2 supported
 *	  zillions of large objects (internal, external, jaquith, inversion).
 *	  Now we only support inversion.
 *
 * Portions Copyright (c) 1996-2006, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/storage/large_object.h,v 1.35 2006/09/07 15:37:25 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef LARGE_OBJECT_H
#define LARGE_OBJECT_H

#include "utils/tqual.h"


/*----------
 * Data about a currently-open large object.
 *
 * id is the logical OID of the large object
 * snapshot is the snapshot to use for read/write operations
 * subid is the subtransaction that opened the desc (or currently owns it)
 * offset is the current seek offset within the LO
 * flags contains some flag bits
 *
 * NOTE: before 7.1, we also had to store references to the separate table
 * and index of a specific large object.  Now they all live in pg_largeobject
 * and are accessed via a common relation descriptor.
 *----------
 */
typedef struct LargeObjectDesc
{
	Oid			id;				/* LO's identifier */
	Snapshot	snapshot;		/* snapshot to use */
	SubTransactionId subid;		/* owning subtransaction ID */
	uint32		offset;			/* current seek pointer */
	int			flags;			/* locking info, etc */

/* flag bits: */
#define IFS_RDLOCK		(1 << 0)
#define IFS_WRLOCK		(1 << 1)

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
extern void close_lo_relation(bool isCommit);
extern Oid	inv_create(Oid lobjId);
extern LargeObjectDesc *inv_open(Oid lobjId, int flags, MemoryContext mcxt);
extern void inv_close(LargeObjectDesc *obj_desc);
extern int	inv_drop(Oid lobjId);
extern int	inv_seek(LargeObjectDesc *obj_desc, int offset, int whence);
extern int	inv_tell(LargeObjectDesc *obj_desc);
extern int	inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes);
extern int	inv_write(LargeObjectDesc *obj_desc, const char *buf, int nbytes);

#endif   /* LARGE_OBJECT_H */
