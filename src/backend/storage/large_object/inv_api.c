/*-------------------------------------------------------------------------
 *
 * inv_api.c
 *	  routines for manipulating inversion fs large objects. This file
 *	  contains the user-level large object application interface routines.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/large_object/inv_api.c,v 1.77 2000/10/21 15:55:24 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/nbtree.h"
#include "access/htup.h"
#include "catalog/catalog.h"
#include "catalog/catname.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_type.h"
#include "libpq/libpq-fs.h"
#include "miscadmin.h"
#include "storage/large_object.h"
#include "storage/smgr.h"
#include "utils/fmgroids.h"
#include "utils/builtins.h"

#include <errno.h>

#define	IBLKSIZE		(MaxTupleSize - MinHeapTupleBitmapSize - sizeof(int32) * 3)

/* Defined in backend/storage/catalog/large_object.c */
bytea *_byteain(const char *data, int32 size);

static int32 getbytealen(bytea *data) {
	if (VARSIZE(data) < VARHDRSZ)
		elog(ERROR, "getbytealen: VARSIZE(data) < VARHDRSZ. This is internal error.");
  return (VARSIZE(data) - VARHDRSZ);
}

/*
 *	inv_create -- create a new large object.
 *
 *		Arguments:
 *		  flags -- was archive, smgr
 *
 *		Returns:
 *		  large object descriptor, appropriately filled in.
 */

LargeObjectDesc *
inv_create(int flags)
{
	int		file_oid;
	LargeObjectDesc *retval;
	
	/*
	 * add one here since the pg_class tuple created will have the next
	 * oid and we want to have the relation name to correspond to the
	 * tuple OID
	 */
	file_oid = newoid() + 1;

	if (LargeobjectFind(file_oid) == 1)
		elog(ERROR, "inv_create: large object %d already exists. This is internal error.", file_oid);

	retval = (LargeObjectDesc *) palloc(sizeof(LargeObjectDesc));

	if (flags & INV_WRITE) {
		retval->flags = IFS_WRLOCK | IFS_RDLOCK;
		retval->heap_r = heap_openr(LargeobjectRelationName, RowExclusiveLock);
	} else if (flags & INV_READ) {
		retval->flags = IFS_RDLOCK;
		retval->heap_r = heap_openr(LargeobjectRelationName, AccessShareLock);
	} else
		elog(ERROR, "inv_create: invalid flags: %d", flags);

	retval->flags |= IFS_ATEOF;
	retval->index_r = index_openr(LargeobjectLOIdPNIndex);
	retval->offset = 0;
	retval->id = file_oid;
	(void)LargeobjectCreate(file_oid);
	return retval;
}

LargeObjectDesc *
inv_open(Oid lobjId, int flags)
{
	LargeObjectDesc *retval;

	if (LargeobjectFind(lobjId) == 0)
		elog(ERROR, "inv_open: large object %d not found", lobjId);
	
	retval = (LargeObjectDesc *)palloc(sizeof(LargeObjectDesc));

	if (flags & INV_WRITE) {
		retval->flags = IFS_WRLOCK | IFS_RDLOCK;
		retval->heap_r = heap_openr(LargeobjectRelationName, RowExclusiveLock);
	} else if (flags & INV_READ) {
		retval->flags = IFS_RDLOCK;
		retval->heap_r = heap_openr(LargeobjectRelationName, AccessShareLock);
	} else
		elog(ERROR, "inv_open: invalid flags: %d", flags);

	retval->index_r = index_openr(LargeobjectLOIdPNIndex);
	retval->offset = 0;
	retval->id = lobjId;

	return retval;
}

/*
 * Closes an existing large object descriptor.
 */
void
inv_close(LargeObjectDesc *obj_desc)
{
	Assert(PointerIsValid(obj_desc));

	if (obj_desc->flags & IFS_WRLOCK)
		heap_close(obj_desc->heap_r, RowExclusiveLock);
	else if (obj_desc->flags & IFS_RDLOCK)
		heap_close(obj_desc->heap_r, AccessShareLock);
	index_close(obj_desc->index_r);
	pfree(obj_desc);
}

/*
 * Destroys an existing large object, and frees its associated pointers.
 *
 * returns -1 if failed
 */
int
inv_drop(Oid lobjId)
{
	LargeobjectDrop(lobjId);
	return 1;
}

/*
 *	inv_stat() -- do a stat on an inversion file.
 *
 *		For the time being, this is an insanely expensive operation.  In
 *		order to find the size of the file, we seek to the last block in
 *		it and compute the size from that.	We scan pg_class to determine
 *		the file's owner and create time.  We don't maintain mod time or
 *		access time, yet.
 *
 *		These fields aren't stored in a table anywhere because they're
 *		updated so frequently, and postgres only appends tuples at the
 *		end of relations.  Once clustering works, we should fix this.
 */
#ifdef NOT_USED

struct pgstat
{								/* just the fields we need from stat
								 * structure */
	int			st_ino;
	int			st_mode;
	unsigned int st_size;
	unsigned int st_sizehigh;	/* high order bits */
/* 2^64 == 1.8 x 10^20 bytes */
	int			st_uid;
	int			st_atime_s;		/* just the seconds */
	int			st_mtime_s;		/* since SysV and the new BSD both have */
	int			st_ctime_s;		/* usec fields.. */
};

int
inv_stat(LargeObjectDesc *obj_desc, struct pgstat * stbuf)
{
	Assert(PointerIsValid(obj_desc));
	Assert(stbuf != NULL);

	/* need read lock for stat */
	if (!(obj_desc->flags & IFS_RDLOCK))
	{
		LockRelation(obj_desc->heap_r, ShareLock);
		obj_desc->flags |= IFS_RDLOCK;
	}

	stbuf->st_ino = RelationGetRelid(obj_desc->heap_r);
#if 1
	stbuf->st_mode = (S_IFREG | 0666);	/* IFREG|rw-rw-rw- */
#else
	stbuf->st_mode = 100666;	/* IFREG|rw-rw-rw- */
#endif
	stbuf->st_size = _inv_getsize(obj_desc->heap_r,
								  obj_desc->hdesc,
								  obj_desc->index_r);

	stbuf->st_uid = obj_desc->heap_r->rd_rel->relowner;

	/* we have no good way of computing access times right now */
	stbuf->st_atime_s = stbuf->st_mtime_s = stbuf->st_ctime_s = 0;

	return 0;
}

#endif

static uint32 inv_getsize(LargeObjectDesc *obj_desc) {
	uint32			found = 0;
	uint32			lastbyte = 0;
	ScanKeyData		skey;
	IndexScanDesc		sd = (IndexScanDesc) NULL;
	RetrieveIndexResult	indexRes;
	HeapTupleData		tuple;
	Buffer			buffer;
	Form_pg_largeobject	data;

	Assert(PointerIsValid(obj_desc));

	ScanKeyEntryInitialize(&skey,
					    (bits16) 0x0,
					    (AttrNumber) 1,
					    (RegProcedure) F_OIDEQ,
					    ObjectIdGetDatum(obj_desc->id));

	sd = index_beginscan(obj_desc->index_r, true, 1, &skey);
	tuple.t_datamcxt = CurrentMemoryContext;
	tuple.t_data = NULL;
	while ((indexRes = index_getnext(sd, ForwardScanDirection))) {
		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(obj_desc->heap_r, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);
		if (tuple.t_data == NULL)
			continue;
		found++;
		data = (Form_pg_largeobject) GETSTRUCT(&tuple);
		lastbyte = data->pageno * IBLKSIZE + getbytealen(&(data->data));
		ReleaseBuffer(buffer);
		break;
	}
	
	index_endscan(sd);

	if (found == 0)
		elog(ERROR, "inv_getsize: large object %d not found", obj_desc->id);
	return lastbyte;
}

int
inv_seek(LargeObjectDesc *obj_desc, int offset, int whence)
{
	Assert(PointerIsValid(obj_desc));

	switch (whence) {
		case SEEK_SET:
			if (offset < 0)
				elog(ERROR, "inv_seek: invalid offset: %d", offset);
			obj_desc->offset = offset;
			break;
		case SEEK_CUR:
			if ((obj_desc->offset + offset) < 0)
				elog(ERROR, "inv_seek: invalid offset: %d", offset);
			obj_desc->offset += offset;
			break;
		case SEEK_END:
			{
				int4 size = inv_getsize(obj_desc);
				if (offset > size)
					elog(ERROR, "inv_seek: invalid offset");
				obj_desc->offset = size - offset;
			}
			break;
		default:
			elog(ERROR, "inv_seek: invalid whence: %d", whence);
	}
	return obj_desc->offset;
}

int
inv_tell(LargeObjectDesc *obj_desc)
{
	Assert(PointerIsValid(obj_desc));

	return obj_desc->offset;
}

int
inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes)
{
	uint32			nread = 0;
	uint32			n;
	uint32			off;
	uint32			len;
	uint32			found = 0;
	uint32			pageno = obj_desc->offset / IBLKSIZE; 
	ScanKeyData		skey[2];
	IndexScanDesc		sd = (IndexScanDesc) NULL;
	RetrieveIndexResult	indexRes;
	HeapTupleData		tuple;
	Buffer			buffer;
	Form_pg_largeobject	data;

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	ScanKeyEntryInitialize(&skey[0],
					    (bits16) 0x0,
					    (AttrNumber) 1,
					    (RegProcedure) F_OIDEQ,
					    ObjectIdGetDatum(obj_desc->id));

	ScanKeyEntryInitialize(&skey[1],
					    (bits16) 0x0,
					    (AttrNumber) 2,
					    (RegProcedure) F_INT4GE,
					    Int32GetDatum(pageno));

	sd = index_beginscan(obj_desc->index_r, false, 2, skey);
	tuple.t_datamcxt = CurrentMemoryContext;
	tuple.t_data = NULL;
	while ((indexRes = index_getnext(sd, ForwardScanDirection))) {
		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(obj_desc->heap_r, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);

		if (tuple.t_data == NULL)
			continue;
		
		found++;
		data = (Form_pg_largeobject) GETSTRUCT(&tuple);
		if (data->pageno != pageno) {
			ReleaseBuffer(buffer);
			index_endscan(sd);
			return 0;
		}

		len = getbytealen(&(data->data));
		off = obj_desc->offset % IBLKSIZE;
		if (off == len) {
			ReleaseBuffer(buffer);
			break;
		}
		if (off > len) {
			ReleaseBuffer(buffer);
			index_endscan(sd);
			return 0;
		}
		n = len - off;

		n = (n < (nbytes - nread)) ? n : (nbytes - nread);
		memcpy(buf + nread, VARDATA(&(data->data)) + off, n);
		nread += n;
		obj_desc->offset += n;

		ReleaseBuffer(buffer);
		pageno++;
		if (nread == nbytes)
			break;
	}

	index_endscan(sd);

	if (found == 0)
		return 0;

	return nread;
}

static int inv_write_existing(LargeObjectDesc *obj_desc, char *buf, int nbytes, int *found) {
	uint32			n = 0;
	uint32			off;
	uint32			len;
	int			i;
	HeapTupleData		tuple;
	HeapTuple		newtup;
	Buffer			buffer;
	Form_pg_largeobject	data;
	ScanKeyData		skey[2];
	IndexScanDesc		sd = (IndexScanDesc) NULL;
	RetrieveIndexResult	indexRes;
	Relation		idescs[Num_pg_largeobject_indices];
	Datum			values[Natts_pg_largeobject];
	char			nulls[Natts_pg_largeobject];
	char			replace[Natts_pg_largeobject];

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	ScanKeyEntryInitialize(&skey[0],
					    (bits16) 0,
					    (AttrNumber) 1,
					    (RegProcedure) F_OIDEQ,
					    ObjectIdGetDatum(obj_desc->id));

	ScanKeyEntryInitialize(&skey[1],
					    (bits16) 0x0,
					    (AttrNumber) 2,
					    (RegProcedure) F_INT4EQ,
					    Int32GetDatum(obj_desc->offset / IBLKSIZE));

	CommandCounterIncrement();
	sd = index_beginscan(obj_desc->index_r, false, 2, skey);
	tuple.t_datamcxt = CurrentMemoryContext;
	tuple.t_data = NULL;
	while ((indexRes = index_getnext(sd, ForwardScanDirection))) {
		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(obj_desc->heap_r, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);
		if (tuple.t_data != NULL)
			break;
	}

	index_endscan(sd);
	if (tuple.t_data == NULL)
		return 0;
	
	(*found)++;
	data = (Form_pg_largeobject) GETSTRUCT(&tuple);
	off = obj_desc->offset % IBLKSIZE;
	len = getbytealen(&(data->data));

	if (len > IBLKSIZE) {
		ReleaseBuffer(buffer);
		elog(FATAL, "Internal error: len > IBLKSIZE");
	}

	for (i=0; i<Natts_pg_largeobject; i++) {
		nulls[i] = ' ';
		replace[i] = ' ';
		values[i] = (Datum)NULL;
	}

	i = 0;
	{
		char b[IBLKSIZE];
		int4 rest = len - off;

		memset(b, 0, IBLKSIZE); /* Can optimize later */
		if ((off > 0) && (len > 0)) /* We start in the middle of the tuple */
			memcpy(b, VARDATA(&(data->data)), (off > len) ? len : off);
		
		if ((nbytes <= rest) || (len == IBLKSIZE)) {
			/* We will update inside existing tuple size */
			if (nbytes < rest)
				n = rest;
	 		else
				n = nbytes;
			memcpy(b + off, buf, n);
			if (n < rest) /* There's a rest of the tuple left */
				memcpy(b + off + n, VARDATA(&(data->data)) + off + n, rest - n);
			/* Update data only */
			replace[2] = 'r';
			values[2] = (Datum) _byteain(b, len);
		 } else {
			/* We will extend tuple */
			/* Do we fit into max tuple size */
			if (nbytes <= (IBLKSIZE - off))
				len = off + nbytes;
			else
				len = IBLKSIZE;
			n = len - off;
			memcpy(b + off, buf, n);
			/* Update data */
			replace[2] = 'r';
			values[2] = (Datum) _byteain(b, len);
		 }

		newtup = heap_modifytuple(&tuple, obj_desc->heap_r,
					  values, nulls, replace);

		heap_update(obj_desc->heap_r, &newtup->t_self, newtup, NULL);
		if (!IsIgnoringSystemIndexes()) {
			CatalogOpenIndices(Num_pg_largeobject_indices, Name_pg_largeobject_indices, idescs);
			CatalogIndexInsert(idescs, Num_pg_largeobject_indices, obj_desc->heap_r, newtup);
			CatalogCloseIndices(Num_pg_largeobject_indices, idescs);
		}
		heap_freetuple(newtup);
	}
	ReleaseBuffer(buffer);
	
	return n;
}

static int inv_write_append(LargeObjectDesc *obj_desc, char *buf, int nbytes) {
	HeapTuple	ntup = (HeapTuple) palloc(sizeof(HeapTupleData));
	Relation		idescs[Num_pg_largeobject_indices];
	Datum		values[Natts_pg_largeobject];
	char		nulls[Natts_pg_largeobject];
	int		i;
	uint32		len;
	
	for (i=0; i<Natts_pg_largeobject; i++) {
		nulls[i] = ' ';
		values[i] = (Datum)NULL;
	}

	i = 0;
	values[i++] = ObjectIdGetDatum(obj_desc->id);
	len = (nbytes > IBLKSIZE) ? IBLKSIZE : nbytes;
		
	values[i++] = Int32GetDatum(obj_desc->offset / IBLKSIZE);
	values[i++] = (Datum) _byteain(buf, len);
	
	ntup = heap_formtuple(obj_desc->heap_r->rd_att, values, nulls);
	heap_insert(obj_desc->heap_r, ntup);

	if (!IsIgnoringSystemIndexes()) {
		CatalogOpenIndices(Num_pg_largeobject_indices, Name_pg_largeobject_indices, idescs);
		CatalogIndexInsert(idescs, Num_pg_largeobject_indices, obj_desc->heap_r, ntup);
		CatalogCloseIndices(Num_pg_largeobject_indices, idescs);
	}
	
	heap_freetuple(ntup);
	
	return len;
}
	
static int inv_write_int(LargeObjectDesc *obj_desc, char *buf, int nbytes) {
	int			nwritten = 0;
	int			found = 0;
	
	if (nbytes == 0)
		return 0;

	nwritten = inv_write_existing(obj_desc, buf, nbytes, &found);
	if (found > 0) {
		obj_desc->offset += nwritten;
		return nwritten;
	}
	/* Looks like we are beyond the end of the file */
	nwritten = inv_write_append(obj_desc, buf, nbytes);
	obj_desc->offset += nwritten;
	return nwritten;
}

static int count = 0;

int
inv_write(LargeObjectDesc *obj_desc, char *buf, int nbytes) {
	int nwritten = 0;
	while (nwritten < nbytes)
		nwritten += inv_write_int(obj_desc, buf + nwritten, nbytes - nwritten);

	return nwritten;
}
