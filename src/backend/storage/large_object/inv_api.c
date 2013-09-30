/*-------------------------------------------------------------------------
 *
 * inv_api.c
 *	  routines for manipulating inversion fs large objects. This file
 *	  contains the user-level large object application interface routines.
 *
 *
 * Note: we access pg_largeobject.data using its C struct declaration.
 * This is safe because it immediately follows pageno which is an int4 field,
 * and therefore the data field will always be 4-byte aligned, even if it
 * is in the short 1-byte-header format.  We have to detoast it since it's
 * quite likely to be in compressed or short format.  We also need to check
 * for NULLs, since initdb will mark loid and pageno but not data as NOT NULL.
 *
 * Note: many of these routines leak memory in CurrentMemoryContext, as indeed
 * does most of the backend code.  We expect that CurrentMemoryContext will
 * be a short-lived context.  Data that must persist across function calls
 * is kept either in CacheMemoryContext (the Relation structs) or in the
 * memory context given to inv_open (for LargeObjectDesc structs).
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/storage/large_object/inv_api.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/sysattr.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_largeobject_metadata.h"
#include "libpq/libpq-fs.h"
#include "miscadmin.h"
#include "storage/large_object.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"


/*
 * All accesses to pg_largeobject and its index make use of a single Relation
 * reference, so that we only need to open pg_relation once per transaction.
 * To avoid problems when the first such reference occurs inside a
 * subtransaction, we execute a slightly klugy maneuver to assign ownership of
 * the Relation reference to TopTransactionResourceOwner.
 */
static Relation lo_heap_r = NULL;
static Relation lo_index_r = NULL;


/*
 * Open pg_largeobject and its index, if not already done in current xact
 */
static void
open_lo_relation(void)
{
	ResourceOwner currentOwner;

	if (lo_heap_r && lo_index_r)
		return;					/* already open in current xact */

	/* Arrange for the top xact to own these relation references */
	currentOwner = CurrentResourceOwner;
	PG_TRY();
	{
		CurrentResourceOwner = TopTransactionResourceOwner;

		/* Use RowExclusiveLock since we might either read or write */
		if (lo_heap_r == NULL)
			lo_heap_r = heap_open(LargeObjectRelationId, RowExclusiveLock);
		if (lo_index_r == NULL)
			lo_index_r = index_open(LargeObjectLOidPNIndexId, RowExclusiveLock);
	}
	PG_CATCH();
	{
		/* Ensure CurrentResourceOwner is restored on error */
		CurrentResourceOwner = currentOwner;
		PG_RE_THROW();
	}
	PG_END_TRY();
	CurrentResourceOwner = currentOwner;
}

/*
 * Clean up at main transaction end
 */
void
close_lo_relation(bool isCommit)
{
	if (lo_heap_r || lo_index_r)
	{
		/*
		 * Only bother to close if committing; else abort cleanup will handle
		 * it
		 */
		if (isCommit)
		{
			ResourceOwner currentOwner;

			currentOwner = CurrentResourceOwner;
			PG_TRY();
			{
				CurrentResourceOwner = TopTransactionResourceOwner;

				if (lo_index_r)
					index_close(lo_index_r, NoLock);
				if (lo_heap_r)
					heap_close(lo_heap_r, NoLock);
			}
			PG_CATCH();
			{
				/* Ensure CurrentResourceOwner is restored on error */
				CurrentResourceOwner = currentOwner;
				PG_RE_THROW();
			}
			PG_END_TRY();
			CurrentResourceOwner = currentOwner;
		}
		lo_heap_r = NULL;
		lo_index_r = NULL;
	}
}


/*
 * Same as pg_largeobject.c's LargeObjectExists(), except snapshot to
 * read with can be specified.
 */
static bool
myLargeObjectExists(Oid loid, Snapshot snapshot)
{
	Relation	pg_lo_meta;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;
	bool		retval = false;

	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(loid));

	pg_lo_meta = heap_open(LargeObjectMetadataRelationId,
						   AccessShareLock);

	sd = systable_beginscan(pg_lo_meta,
							LargeObjectMetadataOidIndexId, true,
							snapshot, 1, skey);

	tuple = systable_getnext(sd);
	if (HeapTupleIsValid(tuple))
		retval = true;

	systable_endscan(sd);

	heap_close(pg_lo_meta, AccessShareLock);

	return retval;
}


static int32
getbytealen(bytea *data)
{
	Assert(!VARATT_IS_EXTENDED(data));
	if (VARSIZE(data) < VARHDRSZ)
		elog(ERROR, "invalid VARSIZE(data)");
	return (VARSIZE(data) - VARHDRSZ);
}


/*
 *	inv_create -- create a new large object
 *
 *	Arguments:
 *	  lobjId - OID to use for new large object, or InvalidOid to pick one
 *
 *	Returns:
 *	  OID of new object
 *
 * If lobjId is not InvalidOid, then an error occurs if the OID is already
 * in use.
 */
Oid
inv_create(Oid lobjId)
{
	Oid			lobjId_new;

	/*
	 * Create a new largeobject with empty data pages
	 */
	lobjId_new = LargeObjectCreate(lobjId);

	/*
	 * dependency on the owner of largeobject
	 *
	 * The reason why we use LargeObjectRelationId instead of
	 * LargeObjectMetadataRelationId here is to provide backward compatibility
	 * to the applications which utilize a knowledge about internal layout of
	 * system catalogs. OID of pg_largeobject_metadata and loid of
	 * pg_largeobject are same value, so there are no actual differences here.
	 */
	recordDependencyOnOwner(LargeObjectRelationId,
							lobjId_new, GetUserId());

	/* Post creation hook for new large object */
	InvokeObjectPostCreateHook(LargeObjectRelationId, lobjId_new, 0);

	/*
	 * Advance command counter to make new tuple visible to later operations.
	 */
	CommandCounterIncrement();

	return lobjId_new;
}

/*
 *	inv_open -- access an existing large object.
 *
 *		Returns:
 *		  Large object descriptor, appropriately filled in.  The descriptor
 *		  and subsidiary data are allocated in the specified memory context,
 *		  which must be suitably long-lived for the caller's purposes.
 */
LargeObjectDesc *
inv_open(Oid lobjId, int flags, MemoryContext mcxt)
{
	LargeObjectDesc *retval;
	Snapshot	snapshot = NULL;
	int			descflags = 0;

	if (flags & INV_WRITE)
	{
		snapshot = SnapshotNow;
		descflags = IFS_WRLOCK | IFS_RDLOCK;
	}
	else if (flags & INV_READ)
	{
		snapshot = GetActiveSnapshot();
		descflags = IFS_RDLOCK;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid flags for opening a large object: %d",
						flags)));

	/* Can't use LargeObjectExists here because it always uses SnapshotNow */
	if (!myLargeObjectExists(lobjId, snapshot))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("large object %u does not exist", lobjId)));

	/*
	 * We must register the snapshot in TopTransaction's resowner, because
	 * it must stay alive until the LO is closed rather than until the
	 * current portal shuts down. Do this after checking that the LO exists,
	 * to avoid leaking the snapshot if an error is thrown.
	 */
	if (snapshot != SnapshotNow)
		snapshot = RegisterSnapshotOnOwner(snapshot,
										   TopTransactionResourceOwner);

	/* All set, create a descriptor */
	retval = (LargeObjectDesc *) MemoryContextAlloc(mcxt,
													sizeof(LargeObjectDesc));
	retval->id = lobjId;
	retval->subid = GetCurrentSubTransactionId();
	retval->offset = 0;
	retval->snapshot = snapshot;
	retval->flags = descflags;

	return retval;
}

/*
 * Closes a large object descriptor previously made by inv_open(), and
 * releases the long-term memory used by it.
 */
void
inv_close(LargeObjectDesc *obj_desc)
{
	Assert(PointerIsValid(obj_desc));

	if (obj_desc->snapshot != SnapshotNow)
		UnregisterSnapshotFromOwner(obj_desc->snapshot,
									TopTransactionResourceOwner);

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
	ObjectAddress object;

	/*
	 * Delete any comments and dependencies on the large object
	 */
	object.classId = LargeObjectRelationId;
	object.objectId = lobjId;
	object.objectSubId = 0;
	performDeletion(&object, DROP_CASCADE, 0);

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
static uint64
inv_getsize(LargeObjectDesc *obj_desc)
{
	uint64		lastbyte = 0;
	ScanKeyData skey[1];
	SysScanDesc sd;
	HeapTuple	tuple;

	Assert(PointerIsValid(obj_desc));

	open_lo_relation();

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 1, skey);

	/*
	 * Because the pg_largeobject index is on both loid and pageno, but we
	 * constrain only loid, a backwards scan should visit all pages of the
	 * large object in reverse pageno order.  So, it's sufficient to examine
	 * the first valid tuple (== last valid page).
	 */
	tuple = systable_getnext_ordered(sd, BackwardScanDirection);
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_largeobject data;
		bytea	   *datafield;
		bool		pfreeit;

		if (HeapTupleHasNulls(tuple))	/* paranoia */
			elog(ERROR, "null field found in pg_largeobject");
		data = (Form_pg_largeobject) GETSTRUCT(tuple);
		datafield = &(data->data);		/* see note at top of file */
		pfreeit = false;
		if (VARATT_IS_EXTENDED(datafield))
		{
			datafield = (bytea *)
				heap_tuple_untoast_attr((struct varlena *) datafield);
			pfreeit = true;
		}
		lastbyte = (uint64) data->pageno * LOBLKSIZE + getbytealen(datafield);
		if (pfreeit)
			pfree(datafield);
	}

	systable_endscan_ordered(sd);

	return lastbyte;
}

int64
inv_seek(LargeObjectDesc *obj_desc, int64 offset, int whence)
{
	int64		newoffset;

	Assert(PointerIsValid(obj_desc));

	/*
	 * Note: overflow in the additions is possible, but since we will reject
	 * negative results, we don't need any extra test for that.
	 */
	switch (whence)
	{
		case SEEK_SET:
			newoffset = offset;
			break;
		case SEEK_CUR:
			newoffset = obj_desc->offset + offset;
			break;
		case SEEK_END:
			newoffset = inv_getsize(obj_desc) + offset;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid whence setting: %d", whence)));
			newoffset = 0;		/* keep compiler quiet */
			break;
	}

	/*
	 * use errmsg_internal here because we don't want to expose INT64_FORMAT
	 * in translatable strings; doing better is not worth the trouble
	 */
	if (newoffset < 0 || newoffset > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
		   errmsg_internal("invalid large object seek target: " INT64_FORMAT,
						   newoffset)));

	obj_desc->offset = newoffset;
	return newoffset;
}

int64
inv_tell(LargeObjectDesc *obj_desc)
{
	Assert(PointerIsValid(obj_desc));

	return obj_desc->offset;
}

int
inv_read(LargeObjectDesc *obj_desc, char *buf, int nbytes)
{
	int			nread = 0;
	int64		n;
	int64		off;
	int			len;
	int32		pageno = (int32) (obj_desc->offset / LOBLKSIZE);
	uint64		pageoff;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	tuple;

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	if (nbytes <= 0)
		return 0;

	open_lo_relation();

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	while ((tuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
	{
		Form_pg_largeobject data;
		bytea	   *datafield;
		bool		pfreeit;

		if (HeapTupleHasNulls(tuple))	/* paranoia */
			elog(ERROR, "null field found in pg_largeobject");
		data = (Form_pg_largeobject) GETSTRUCT(tuple);

		/*
		 * We expect the indexscan will deliver pages in order.  However,
		 * there may be missing pages if the LO contains unwritten "holes". We
		 * want missing sections to read out as zeroes.
		 */
		pageoff = ((uint64) data->pageno) * LOBLKSIZE;
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

			datafield = &(data->data);	/* see note at top of file */
			pfreeit = false;
			if (VARATT_IS_EXTENDED(datafield))
			{
				datafield = (bytea *)
					heap_tuple_untoast_attr((struct varlena *) datafield);
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

		if (nread >= nbytes)
			break;
	}

	systable_endscan_ordered(sd);

	return nread;
}

int
inv_write(LargeObjectDesc *obj_desc, const char *buf, int nbytes)
{
	int			nwritten = 0;
	int			n;
	int			off;
	int			len;
	int32		pageno = (int32) (obj_desc->offset / LOBLKSIZE);
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	Form_pg_largeobject olddata;
	bool		neednextpage;
	bytea	   *datafield;
	bool		pfreeit;
	struct
	{
		bytea		hdr;
		char		data[LOBLKSIZE];	/* make struct big enough */
		int32		align_it;	/* ensure struct is aligned well enough */
	}			workbuf;
	char	   *workb = VARDATA(&workbuf.hdr);
	HeapTuple	newtup;
	Datum		values[Natts_pg_largeobject];
	bool		nulls[Natts_pg_largeobject];
	bool		replace[Natts_pg_largeobject];
	CatalogIndexState indstate;

	Assert(PointerIsValid(obj_desc));
	Assert(buf != NULL);

	/* enforce writability because snapshot is probably wrong otherwise */
	Assert(obj_desc->flags & IFS_WRLOCK);

	if (nbytes <= 0)
		return 0;

	/* this addition can't overflow because nbytes is only int32 */
	if ((nbytes + obj_desc->offset) > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid large object write request size: %d",
						nbytes)));

	open_lo_relation();

	indstate = CatalogOpenIndexes(lo_heap_r);

	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	oldtuple = NULL;
	olddata = NULL;
	neednextpage = true;

	while (nwritten < nbytes)
	{
		/*
		 * If possible, get next pre-existing page of the LO.  We expect the
		 * indexscan will deliver these in order --- but there may be holes.
		 */
		if (neednextpage)
		{
			if ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
			{
				if (HeapTupleHasNulls(oldtuple))		/* paranoia */
					elog(ERROR, "null field found in pg_largeobject");
				olddata = (Form_pg_largeobject) GETSTRUCT(oldtuple);
				Assert(olddata->pageno >= pageno);
			}
			neednextpage = false;
		}

		/*
		 * If we have a pre-existing page, see if it is the page we want to
		 * write, or a later one.
		 */
		if (olddata != NULL && olddata->pageno == pageno)
		{
			/*
			 * Update an existing page with fresh data.
			 *
			 * First, load old data into workbuf
			 */
			datafield = &(olddata->data);		/* see note at top of file */
			pfreeit = false;
			if (VARATT_IS_EXTENDED(datafield))
			{
				datafield = (bytea *)
					heap_tuple_untoast_attr((struct varlena *) datafield);
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
			SET_VARSIZE(&workbuf.hdr, len + VARHDRSZ);

			/*
			 * Form and insert updated tuple
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, false, sizeof(nulls));
			memset(replace, false, sizeof(replace));
			values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
			replace[Anum_pg_largeobject_data - 1] = true;
			newtup = heap_modify_tuple(oldtuple, RelationGetDescr(lo_heap_r),
									   values, nulls, replace);
			simple_heap_update(lo_heap_r, &newtup->t_self, newtup);
			CatalogIndexInsert(indstate, newtup);
			heap_freetuple(newtup);

			/*
			 * We're done with this old page.
			 */
			oldtuple = NULL;
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
			SET_VARSIZE(&workbuf.hdr, len + VARHDRSZ);

			/*
			 * Form and insert updated tuple
			 */
			memset(values, 0, sizeof(values));
			memset(nulls, false, sizeof(nulls));
			values[Anum_pg_largeobject_loid - 1] = ObjectIdGetDatum(obj_desc->id);
			values[Anum_pg_largeobject_pageno - 1] = Int32GetDatum(pageno);
			values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
			newtup = heap_form_tuple(lo_heap_r->rd_att, values, nulls);
			simple_heap_insert(lo_heap_r, newtup);
			CatalogIndexInsert(indstate, newtup);
			heap_freetuple(newtup);
		}
		pageno++;
	}

	systable_endscan_ordered(sd);

	CatalogCloseIndexes(indstate);

	/*
	 * Advance command counter so that my tuple updates will be seen by later
	 * large-object operations in this transaction.
	 */
	CommandCounterIncrement();

	return nwritten;
}

void
inv_truncate(LargeObjectDesc *obj_desc, int64 len)
{
	int32		pageno = (int32) (len / LOBLKSIZE);
	int32		off;
	ScanKeyData skey[2];
	SysScanDesc sd;
	HeapTuple	oldtuple;
	Form_pg_largeobject olddata;
	struct
	{
		bytea		hdr;
		char		data[LOBLKSIZE];	/* make struct big enough */
		int32		align_it;	/* ensure struct is aligned well enough */
	}			workbuf;
	char	   *workb = VARDATA(&workbuf.hdr);
	HeapTuple	newtup;
	Datum		values[Natts_pg_largeobject];
	bool		nulls[Natts_pg_largeobject];
	bool		replace[Natts_pg_largeobject];
	CatalogIndexState indstate;

	Assert(PointerIsValid(obj_desc));

	/* enforce writability because snapshot is probably wrong otherwise */
	Assert(obj_desc->flags & IFS_WRLOCK);

	/*
	 * use errmsg_internal here because we don't want to expose INT64_FORMAT
	 * in translatable strings; doing better is not worth the trouble
	 */
	if (len < 0 || len > MAX_LARGE_OBJECT_SIZE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg_internal("invalid large object truncation target: " INT64_FORMAT,
								 len)));

	open_lo_relation();

	indstate = CatalogOpenIndexes(lo_heap_r);

	/*
	 * Set up to find all pages with desired loid and pageno >= target
	 */
	ScanKeyInit(&skey[0],
				Anum_pg_largeobject_loid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(obj_desc->id));

	ScanKeyInit(&skey[1],
				Anum_pg_largeobject_pageno,
				BTGreaterEqualStrategyNumber, F_INT4GE,
				Int32GetDatum(pageno));

	sd = systable_beginscan_ordered(lo_heap_r, lo_index_r,
									obj_desc->snapshot, 2, skey);

	/*
	 * If possible, get the page the truncation point is in. The truncation
	 * point may be beyond the end of the LO or in a hole.
	 */
	olddata = NULL;
	if ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
	{
		if (HeapTupleHasNulls(oldtuple))		/* paranoia */
			elog(ERROR, "null field found in pg_largeobject");
		olddata = (Form_pg_largeobject) GETSTRUCT(oldtuple);
		Assert(olddata->pageno >= pageno);
	}

	/*
	 * If we found the page of the truncation point we need to truncate the
	 * data in it.	Otherwise if we're in a hole, we need to create a page to
	 * mark the end of data.
	 */
	if (olddata != NULL && olddata->pageno == pageno)
	{
		/* First, load old data into workbuf */
		bytea	   *datafield = &(olddata->data);		/* see note at top of
														 * file */
		bool		pfreeit = false;
		int			pagelen;

		if (VARATT_IS_EXTENDED(datafield))
		{
			datafield = (bytea *)
				heap_tuple_untoast_attr((struct varlena *) datafield);
			pfreeit = true;
		}
		pagelen = getbytealen(datafield);
		Assert(pagelen <= LOBLKSIZE);
		memcpy(workb, VARDATA(datafield), pagelen);
		if (pfreeit)
			pfree(datafield);

		/*
		 * Fill any hole
		 */
		off = len % LOBLKSIZE;
		if (off > pagelen)
			MemSet(workb + pagelen, 0, off - pagelen);

		/* compute length of new page */
		SET_VARSIZE(&workbuf.hdr, off + VARHDRSZ);

		/*
		 * Form and insert updated tuple
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		memset(replace, false, sizeof(replace));
		values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
		replace[Anum_pg_largeobject_data - 1] = true;
		newtup = heap_modify_tuple(oldtuple, RelationGetDescr(lo_heap_r),
								   values, nulls, replace);
		simple_heap_update(lo_heap_r, &newtup->t_self, newtup);
		CatalogIndexInsert(indstate, newtup);
		heap_freetuple(newtup);
	}
	else
	{
		/*
		 * If the first page we found was after the truncation point, we're in
		 * a hole that we'll fill, but we need to delete the later page
		 * because the loop below won't visit it again.
		 */
		if (olddata != NULL)
		{
			Assert(olddata->pageno > pageno);
			simple_heap_delete(lo_heap_r, &oldtuple->t_self);
		}

		/*
		 * Write a brand new page.
		 *
		 * Fill the hole up to the truncation point
		 */
		off = len % LOBLKSIZE;
		if (off > 0)
			MemSet(workb, 0, off);

		/* compute length of new page */
		SET_VARSIZE(&workbuf.hdr, off + VARHDRSZ);

		/*
		 * Form and insert new tuple
		 */
		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));
		values[Anum_pg_largeobject_loid - 1] = ObjectIdGetDatum(obj_desc->id);
		values[Anum_pg_largeobject_pageno - 1] = Int32GetDatum(pageno);
		values[Anum_pg_largeobject_data - 1] = PointerGetDatum(&workbuf);
		newtup = heap_form_tuple(lo_heap_r->rd_att, values, nulls);
		simple_heap_insert(lo_heap_r, newtup);
		CatalogIndexInsert(indstate, newtup);
		heap_freetuple(newtup);
	}

	/*
	 * Delete any pages after the truncation point.  If the initial search
	 * didn't find a page, then of course there's nothing more to do.
	 */
	if (olddata != NULL)
	{
		while ((oldtuple = systable_getnext_ordered(sd, ForwardScanDirection)) != NULL)
		{
			simple_heap_delete(lo_heap_r, &oldtuple->t_self);
		}
	}

	systable_endscan_ordered(sd);

	CatalogCloseIndexes(indstate);

	/*
	 * Advance command counter so that tuple updates will be seen by later
	 * large-object operations in this transaction.
	 */
	CommandCounterIncrement();
}
