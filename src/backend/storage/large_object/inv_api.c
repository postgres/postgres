/*-------------------------------------------------------------------------
 *
 * inv_api.c
 *	  routines for manipulating inversion fs large objects. This file
 *	  contains the user-level large object application interface routines.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/storage/large_object/inv_api.c,v 1.85 2001/02/10 02:31:26 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>
#include <sys/types.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/nbtree.h"
#include "access/tuptoaster.h"
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


static int32
getbytealen(bytea *data)
{
	Assert(! VARATT_IS_EXTENDED(data));
	if (VARSIZE(data) < VARHDRSZ)
		elog(ERROR, "getbytealen: VARSIZE(data) < VARHDRSZ. This is internal error.");
	return (VARSIZE(data) - VARHDRSZ);
}

/*
 *	inv_create -- create a new large object.
 *
 *		Arguments:
 *		  flags
 *
 *		Returns:
 *		  large object descriptor, appropriately filled in.
 */
LargeObjectDesc *
inv_create(int flags)
{
	Oid			file_oid;
	LargeObjectDesc *retval;

	/*
	 * Allocate an OID to be the LO's identifier.
	 */
	file_oid = newoid();

	/* Check for duplicate (shouldn't happen) */
	if (LargeObjectExists(file_oid))
		elog(ERROR, "inv_create: large object %u already exists. This is internal error.", file_oid);

	/*
	 * Create the LO by writing an empty first page for it in pg_largeobject
	 */
	(void) LargeObjectCreate(file_oid);

	/*
	 * Advance command counter so that new tuple will be seen by later
	 * large-object operations in this transaction.
	 */
	CommandCounterIncrement();

	/*
	 * Prepare LargeObjectDesc data structure for accessing LO
	 */
	retval = (LargeObjectDesc *) palloc(sizeof(LargeObjectDesc));

	retval->id = file_oid;
	retval->offset = 0;

	if (flags & INV_WRITE) {
		retval->flags = IFS_WRLOCK | IFS_RDLOCK;
		retval->heap_r = heap_openr(LargeObjectRelationName, RowExclusiveLock);
	} else if (flags & INV_READ) {
		retval->flags = IFS_RDLOCK;
		retval->heap_r = heap_openr(LargeObjectRelationName, AccessShareLock);
	} else
		elog(ERROR, "inv_create: invalid flags: %d", flags);

	retval->index_r = index_openr(LargeObjectLOidPNIndex);

	return retval;
}

/*
 *	inv_open -- access an existing large object.
 *
 *		Returns:
 *		  large object descriptor, appropriately filled in.
 */
LargeObjectDesc *
inv_open(Oid lobjId, int flags)
{
	LargeObjectDesc *retval;

	if (! LargeObjectExists(lobjId))
		elog(ERROR, "inv_open: large object %u not found", lobjId);
	
	retval = (LargeObjectDesc *) palloc(sizeof(LargeObjectDesc));

	retval->id = lobjId;
	retval->offset = 0;

	if (flags & INV_WRITE) {
		retval->flags = IFS_WRLOCK | IFS_RDLOCK;
		retval->heap_r = heap_openr(LargeObjectRelationName, RowExclusiveLock);
	} else if (flags & INV_READ) {
		retval->flags = IFS_RDLOCK;
		retval->heap_r = heap_openr(LargeObjectRelationName, AccessShareLock);
	} else
		elog(ERROR, "inv_open: invalid flags: %d", flags);

	retval->index_r = index_openr(LargeObjectLOidPNIndex);

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
 * Destroys an existing large object (not to be confused with a descriptor!)
 *
 * returns -1 if failed
 */
int
inv_drop(Oid lobjId)
{
	LargeObjectDrop(lobjId);

	/*
	 * Advance command counter so that tuple removal will be seen by later
	 * large-object operations in this transaction.
	 */
	CommandCounterIncrement();

	return 1;
}

/*
 * Determine size of a large object
 *
 * NOTE: LOs can contain gaps, just like Unix files.  We actually return
 * the offset of the last byte + 1.
 */
static uint32
inv_getsize(LargeObjectDesc *obj_desc)
{
	bool			found = false;
	uint32			lastbyte = 0;
	ScanKeyData		skey[1];
	IndexScanDesc	sd;
	RetrieveIndexResult	indexRes;
	HeapTupleData	tuple;
	Buffer			buffer;
	Form_pg_largeobject	data;
	bytea		   *datafield;
	bool			pfreeit;

	Assert(PointerIsValid(obj_desc));

	ScanKeyEntryInitialize(&skey[0],
						   (bits16) 0x0,
						   (AttrNumber) 1,
						   (RegProcedure) F_OIDEQ,
						   ObjectIdGetDatum(obj_desc->id));

	sd = index_beginscan(obj_desc->index_r, true, 1, skey);

	tuple.t_datamcxt = CurrentMemoryContext;
	tuple.t_data = NULL;

	/*
	 * Because the pg_largeobject index is on both loid and pageno,
	 * but we constrain only loid, a backwards scan should visit all
	 * pages of the large object in reverse pageno order.  So, it's
	 * sufficient to examine the first valid tuple (== last valid page).
	 */
	while ((indexRes = index_getnext(sd, BackwardScanDirection)))
	{
		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(obj_desc->heap_r, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);
		if (tuple.t_data == NULL)
			continue;
		found = true;
		data = (Form_pg_largeobject) GETSTRUCT(&tuple);
		datafield = &(data->data);
		pfreeit = false;
		if (VARATT_IS_EXTENDED(datafield))
		{
			datafield = (bytea *)
				heap_tuple_untoast_attr((varattrib *) datafield);
			pfreeit = true;
		}
		lastbyte = data->pageno * LOBLKSIZE + getbytealen(datafield);
		if (pfreeit)
			pfree(datafield);
		ReleaseBuffer(buffer);
		break;
	}
	
	index_endscan(sd);

	if (!found)
		elog(ERROR, "inv_getsize: large object %u not found", obj_desc->id);
	return lastbyte;
}

int
inv_seek(LargeObjectDesc *obj_desc, int offset, int whence)
{
	Assert(PointerIsValid(obj_desc));

	switch (whence)
	{
		case SEEK_SET:
			if (offset < 0)
				elog(ERROR, "inv_seek: invalid offset: %d", offset);
			obj_desc->offset = offset;
			break;
		case SEEK_CUR:
			if (offset < 0 && obj_desc->offset < ((uint32) (- offset)))
				elog(ERROR, "inv_seek: invalid offset: %d", offset);
			obj_desc->offset += offset;
			break;
		case SEEK_END:
			{
				uint32	size = inv_getsize(obj_desc);

				if (offset < 0 && size < ((uint32) (- offset)))
					elog(ERROR, "inv_seek: invalid offset: %d", offset);
				obj_desc->offset = size + offset;
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
	int				nread = 0;
	int				n;
	int				off;
	int				len;
	int32			pageno = (int32) (obj_desc->offset / LOBLKSIZE);
	uint32			pageoff;
	ScanKeyData		skey[2];
	IndexScanDesc	sd;
	RetrieveIndexResult	indexRes;
	HeapTupleData	tuple;
	Buffer			buffer;
	Form_pg_largeobject	data;
	bytea		   *datafield;
	bool			pfreeit;

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	if (nbytes <= 0)
		return 0;

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

	while ((indexRes = index_getnext(sd, ForwardScanDirection)))
	{
		tuple.t_self = indexRes->heap_iptr;
		heap_fetch(obj_desc->heap_r, SnapshotNow, &tuple, &buffer);
		pfree(indexRes);

		if (tuple.t_data == NULL)
			continue;
		
		data = (Form_pg_largeobject) GETSTRUCT(&tuple);

		/*
		 * We assume the indexscan will deliver pages in order.  However,
		 * there may be missing pages if the LO contains unwritten "holes".
		 * We want missing sections to read out as zeroes.
		 */
		pageoff = ((uint32) data->pageno) * LOBLKSIZE;
		if (pageoff > obj_desc->offset)
		{
			n = pageoff - obj_desc->offset;
			n = (n <= (nbytes - nread)) ? n : (nbytes - nread);
			MemSet(buf + nread, 0, n);
			nread += n;
			obj_desc->offset += n;
		}

		if (nread < nbytes)
		{
			Assert(obj_desc->offset >= pageoff);
			off = (int) (obj_desc->offset - pageoff);
			Assert(off >= 0 && off < LOBLKSIZE);

			datafield = &(data->data);
			pfreeit = false;
			if (VARATT_IS_EXTENDED(datafield))
			{
				datafield = (bytea *)
					heap_tuple_untoast_attr((varattrib *) datafield);
				pfreeit = true;
			}
			len = getbytealen(datafield);
			if (len > off)
			{
				n = len - off;
				n = (n <= (nbytes - nread)) ? n : (nbytes - nread);
				memcpy(buf + nread, VARDATA(datafield) + off, n);
				nread += n;
				obj_desc->offset += n;
			}
			if (pfreeit)
				pfree(datafield);
		}

		ReleaseBuffer(buffer);
		if (nread >= nbytes)
			break;
	}

	index_endscan(sd);

	return nread;
}

int
inv_write(LargeObjectDesc *obj_desc, char *buf, int nbytes)
{
	int				nwritten = 0;
	int				n;
	int				off;
	int				len;
	int32			pageno = (int32) (obj_desc->offset / LOBLKSIZE);
	ScanKeyData		skey[2];
	IndexScanDesc	sd;
	RetrieveIndexResult	indexRes;
	HeapTupleData	oldtuple;
	Buffer			buffer;
	Form_pg_largeobject	olddata;
	bool			neednextpage;
	bytea		   *datafield;
	bool			pfreeit;
	char			workbuf[LOBLKSIZE + VARHDRSZ];
	char		   *workb = VARATT_DATA(workbuf);
	HeapTuple		newtup;
	Datum			values[Natts_pg_largeobject];
	char			nulls[Natts_pg_largeobject];
	char			replace[Natts_pg_largeobject];
	bool			write_indices;
	Relation		idescs[Num_pg_largeobject_indices];

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	if (nbytes <= 0)
		return 0;

	write_indices = ! IsIgnoringSystemIndexes();
	if (write_indices)
		CatalogOpenIndices(Num_pg_largeobject_indices,
						   Name_pg_largeobject_indices,
						   idescs);

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

	oldtuple.t_datamcxt = CurrentMemoryContext;
	oldtuple.t_data = NULL;
	olddata = NULL;
	buffer = InvalidBuffer;
	neednextpage = true;

	while (nwritten < nbytes)
	{
		/*
		 * If possible, get next pre-existing page of the LO.  We assume
		 * the indexscan will deliver these in order --- but there may be
		 * holes.
		 */
		if (neednextpage)
		{
			while ((indexRes = index_getnext(sd, ForwardScanDirection)))
			{
				oldtuple.t_self = indexRes->heap_iptr;
				heap_fetch(obj_desc->heap_r, SnapshotNow, &oldtuple, &buffer);
				pfree(indexRes);
				if (oldtuple.t_data != NULL)
				{
					olddata = (Form_pg_largeobject) GETSTRUCT(&oldtuple);
					Assert(olddata->pageno >= pageno);
					break;
				}
			}
			neednextpage = false;
		}
		/*
		 * If we have a pre-existing page, see if it is the page we want
		 * to write, or a later one.
		 */
		if (olddata != NULL && olddata->pageno == pageno)
		{
			/*
			 * Update an existing page with fresh data.
			 *
			 * First, load old data into workbuf
			 */
			datafield = &(olddata->data);
			pfreeit = false;
			if (VARATT_IS_EXTENDED(datafield))
			{
				datafield = (bytea *)
					heap_tuple_untoast_attr((varattrib *) datafield);
				pfreeit = true;
			}
			len = getbytealen(datafield);
			Assert(len <= LOBLKSIZE);
			memcpy(workb, VARDATA(datafield), len);
			if (pfreeit)
				pfree(datafield);
			/*
			 * Fill any hole
			 */
			off = (int) (obj_desc->offset % LOBLKSIZE);
			if (off > len)
				MemSet(workb + len, 0, off - len);
			/*
			 * Insert appropriate portion of new data
			 */
			n = LOBLKSIZE - off;
			n = (n <= (nbytes - nwritten)) ? n : (nbytes - nwritten);
			memcpy(workb + off, buf + nwritten, n);
			nwritten += n;
			obj_desc->offset += n;
			off += n;
			/* compute valid length of new page */
			len = (len >= off) ? len : off;
			VARATT_SIZEP(workbuf) = len + VARHDRSZ;
			/*
			 * Form and insert updated tuple
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, ' ', sizeof(nulls));
			memset(replace, ' ', sizeof(replace));
			values[Anum_pg_largeobject_data - 1] = PointerGetDatum(workbuf);
			replace[Anum_pg_largeobject_data - 1] = 'r';
			newtup = heap_modifytuple(&oldtuple, obj_desc->heap_r,
									  values, nulls, replace);
			simple_heap_update(obj_desc->heap_r, &newtup->t_self, newtup);
			if (write_indices)
				CatalogIndexInsert(idescs, Num_pg_largeobject_indices,
								   obj_desc->heap_r, newtup);
			heap_freetuple(newtup);
			/*
			 * We're done with this old page.
			 */
			ReleaseBuffer(buffer);
			oldtuple.t_datamcxt = CurrentMemoryContext;
			oldtuple.t_data = NULL;
			olddata = NULL;
			neednextpage = true;
		}
		else
		{
			/*
			 * Write a brand new page.
			 *
			 * First, fill any hole
			 */
			off = (int) (obj_desc->offset % LOBLKSIZE);
			if (off > 0)
				MemSet(workb, 0, off);
			/*
			 * Insert appropriate portion of new data
			 */
			n = LOBLKSIZE - off;
			n = (n <= (nbytes - nwritten)) ? n : (nbytes - nwritten);
			memcpy(workb + off, buf + nwritten, n);
			nwritten += n;
			obj_desc->offset += n;
			/* compute valid length of new page */
			len = off + n;
			VARATT_SIZEP(workbuf) = len + VARHDRSZ;
			/*
			 * Form and insert updated tuple
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, ' ', sizeof(nulls));
			values[Anum_pg_largeobject_loid - 1] = ObjectIdGetDatum(obj_desc->id);
			values[Anum_pg_largeobject_pageno - 1] = Int32GetDatum(pageno);
			values[Anum_pg_largeobject_data - 1] = PointerGetDatum(workbuf);
			newtup = heap_formtuple(obj_desc->heap_r->rd_att, values, nulls);
			heap_insert(obj_desc->heap_r, newtup);
			if (write_indices)
				CatalogIndexInsert(idescs, Num_pg_largeobject_indices,
								   obj_desc->heap_r, newtup);
			heap_freetuple(newtup);
		}
		pageno++;
	}

	if (olddata != NULL)
		ReleaseBuffer(buffer);

	index_endscan(sd);

	if (write_indices)
		CatalogCloseIndices(Num_pg_largeobject_indices, idescs);

	/*
	 * Advance command counter so that my tuple updates will be seen by later
	 * large-object operations in this transaction.
	 */
	CommandCounterIncrement();

	return nwritten;
}
