/*-------------------------------------------------------------------------
 *
 * large_object.h--
 *    file of info for Postgres large objects. POSTGRES 4.2 supports
 *    zillions of large objects (internal, external, jaquith, inversion).
 *    Now we only support inversion.
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 * $Id: large_object.h,v 1.1 1996/08/28 01:58:12 scrappy Exp $
 *
 *-------------------------------------------------------------------------
 */
#ifndef	LARGE_OBJECT_H
#define	LARGE_OBJECT_H

#include "c.h"
#include "utils/rel.h"
#include "access/relscan.h"

/*
 * This structure will eventually have lots more stuff associated with it.
 */
typedef struct LargeObjectDesc
{
    Relation heap_r;		/* heap relation */
    Relation index_r;		/* index relation on seqno attribute */
    IndexScanDesc iscan; 	/* index scan we're using */
    TupleDesc hdesc; 		/* heap relation tuple desc */
    TupleDesc idesc; 		/* index relation tuple desc */
    uint32 lowbyte;		/* low byte on the current page */
    uint32 highbyte;		/* high byte on the current page */
    uint32 offset;		/* current seek pointer */
    ItemPointerData htid; 	/* tid of current heap tuple */

#define IFS_RDLOCK	(1 << 0)
#define IFS_WRLOCK	(1 << 1)
#define IFS_ATEOF	(1 << 2)

    u_long flags;		/* locking info, etc */
} LargeObjectDesc;

/*
 * Function definitions...
 */

/* inversion stuff in inv_api.c */
extern LargeObjectDesc *inv_create(int flags);
extern LargeObjectDesc *inv_open(Oid lobjId, int flags);
extern void inv_close(LargeObjectDesc *obj_desc);
extern int inv_destroy(Oid lobjId);
extern int inv_stat(LargeObjectDesc *obj_desc, struct pgstat *stbuf);
extern int inv_seek(LargeObjectDesc *obj_desc, int offset, int whence);
extern int inv_tell(LargeObjectDesc *obj_desc);
extern int inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes);
extern int inv_write(LargeObjectDesc *obj_desc, char *buf, int nbytes);

#endif	/* LARGE_OBJECT_H */
