/*-------------------------------------------------------------------------
 *
 * toast_internals.c
 *	  Functions for internal use by the TOAST system.
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_internals.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

static bool toastrel_valueid_exists(Relation toastrel, Oid8 valueid);
static bool toastid_valueid_exists(Oid toastrelid, Oid8 valueid);

/* ----------
 * toast_compress_datum -
 *
 *	Create a compressed version of a varlena datum
 *
 *	If we fail (ie, compressed result is actually bigger than original)
 *	then return NULL.  We must not use compressed data if it'd expand
 *	the tuple!
 *
 *	We use VAR{SIZE,DATA}_ANY so we can handle short varlenas here without
 *	copying them.  But we can't handle external or compressed datums.
 * ----------
 */
Datum
toast_compress_datum(Datum value, char cmethod)
{
	varlena    *tmp = NULL;
	int32		valsize;
	ToastCompressionId cmid = TOAST_INVALID_COMPRESSION_ID;

	Assert(!VARATT_IS_EXTERNAL(DatumGetPointer(value)));
	Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));

	valsize = VARSIZE_ANY_EXHDR(DatumGetPointer(value));

	/* If the compression method is not valid, use the current default */
	if (!CompressionMethodIsValid(cmethod))
		cmethod = default_toast_compression;

	/*
	 * Call appropriate compression routine for the compression method.
	 */
	switch (cmethod)
	{
		case TOAST_PGLZ_COMPRESSION:
			tmp = pglz_compress_datum((const varlena *) DatumGetPointer(value));
			cmid = TOAST_PGLZ_COMPRESSION_ID;
			break;
		case TOAST_LZ4_COMPRESSION:
			tmp = lz4_compress_datum((const varlena *) DatumGetPointer(value));
			cmid = TOAST_LZ4_COMPRESSION_ID;
			break;
		default:
			elog(ERROR, "invalid compression method %c", cmethod);
	}

	if (tmp == NULL)
		return PointerGetDatum(NULL);

	/*
	 * We recheck the actual size even if compression reports success, because
	 * it might be satisfied with having saved as little as one byte in the
	 * compressed data --- which could turn into a net loss once you consider
	 * header and alignment padding.  Worst case, the compressed format might
	 * require three padding bytes (plus header, which is included in
	 * VARSIZE(tmp)), whereas the uncompressed format would take only one
	 * header byte and no padding if the value is short enough.  So we insist
	 * on a savings of more than 2 bytes to ensure we have a gain.
	 */
	if (VARSIZE(tmp) < valsize - 2)
	{
		/* successful compression */
		Assert(cmid != TOAST_INVALID_COMPRESSION_ID);
		TOAST_COMPRESS_SET_SIZE_AND_COMPRESS_METHOD(tmp, valsize, cmid);
		return PointerGetDatum(tmp);
	}
	else
	{
		/* incompressible data */
		pfree(tmp);
		return PointerGetDatum(NULL);
	}
}

/* ----------
 * toast_preserve_valueid -
 *
 *	During a table rewrite that preserves rd_toastoid, we want to preserve
 *	toast value IDs too.  If the datum previously had an external value from
 *	that same toast table, return its value ID so the caller can re-use it,
 *	otherwise return InvalidOid8.
 *
 *	This works for both Oid and Oid8 value IDs, as the value ID is decoded
 *	independently of the pointer's vartag.
 *
 *	There is a corner case here: the table rewrite might have to copy both
 *	live and recently-dead versions of a row, and those versions could easily
 *	reference the same toast value.  When we copy the second or later version
 *	of such a row, preserving the value ID means we select one that's already
 *	in the new toast table.  We detect that and set *data_todo to 0 so the
 *	caller falls through without writing the data again.
 *
 *	While annoying and ugly-looking, this is a good thing because it ensures
 *	that we wind up with only one copy of the toast value when there is only
 *	one copy in the old toast table.  Before we detected this case, we'd have
 *	made multiple copies, wasting space; and what's worse, the copies
 *	belonging to already-deleted heap tuples would not be reclaimed by VACUUM.
 * ----------
 */
static Oid8
toast_preserve_valueid(Relation toastrel, Oid toastoid,
					   varlena *oldexternal, int32 *data_todo)
{
	toast_external_data old;

	if (!OidIsValid(toastoid) || oldexternal == NULL)
		return InvalidOid8;

	Assert(VARATT_IS_EXTERNAL_ONDISK(oldexternal));
	toast_external_info_get(oldexternal, &old);

	/*
	 * Only re-use the value ID if the old pointer came from this same toast
	 * table.
	 */
	if (old.toastrelid != toastoid)
		return InvalidOid8;

	if (toastrel_valueid_exists(toastrel, old.valueid))
		*data_todo = 0;

	return old.valueid;
}

/* ----------
 * toast_save_datum -
 *
 *	Save one single datum into the secondary relation and return
 *	a Datum reference for it.
 *
 * rel: the main relation we're working with (not the toast rel!)
 * value: datum to be pushed to toast storage
 * oldexternal: if not NULL, toast pointer previously representing the datum
 * options: options to be passed to heap_insert() for toast rows
 * ----------
 */
Datum
toast_save_datum(Relation rel, Datum value,
				 varlena *oldexternal, uint32 options)
{
	Relation	toastrel;
	Relation   *toastidxs;
	TupleDesc	toasttupDesc;
	CommandId	mycid = GetCurrentCommandId(true);
	varlena    *result;
	int32		chunk_seq = 0;
	char	   *data_p;
	int32		data_todo;
	Pointer		dval = DatumGetPointer(value);
	int			num_indexes;
	int			validIndex;
	Oid			toast_typid = RelationGetToastChunkIdType(rel);
	int32		max_chunk_size;

	/* Fields that will be assembled into the TOAST pointer at the end */
	int32		va_rawsize;
	uint32		va_extinfo;
	Oid8		va_valueid;
	Oid			va_toastrelid;

	Assert(!VARATT_IS_EXTERNAL(dval));

	/*
	 * Open the toast relation and its indexes.  We can use the index to check
	 * uniqueness of the OID we assign to the toasted item, even though it has
	 * additional columns besides OID.
	 */
	toastrel = table_open(rel->rd_rel->reltoastrelid, RowExclusiveLock);
	toasttupDesc = toastrel->rd_att;

	/* Open all the toast indexes and look for the valid one */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Get the data pointer and length, and compute va_rawsize and va_extinfo.
	 *
	 * va_rawsize is the size of the equivalent fully uncompressed datum, so
	 * we have to adjust for short headers.
	 *
	 * va_extinfo stored the actual size of the data payload in the toast
	 * records and the compression method in first 2 bits if data is
	 * compressed.
	 */
	if (VARATT_IS_SHORT(dval))
	{
		data_p = VARDATA_SHORT(dval);
		data_todo = VARSIZE_SHORT(dval) - VARHDRSZ_SHORT;
		va_rawsize = data_todo + VARHDRSZ;	/* as if not short */
		va_extinfo = data_todo;
	}
	else if (VARATT_IS_COMPRESSED(dval))
	{
		uint32		cmid = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(dval);

		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		/* rawsize in a compressed datum is just the size of the payload */
		va_rawsize = VARDATA_COMPRESSED_GET_EXTSIZE(dval) + VARHDRSZ;

		/* set external size and compression method */
		Assert(cmid == TOAST_PGLZ_COMPRESSION_ID ||
			   cmid == TOAST_LZ4_COMPRESSION_ID);
		va_extinfo = data_todo | (cmid << VARLENA_EXTSIZE_BITS);

		/* Assert that the numbers look like it's compressed */
		Assert(VARATT_EXTINFO_IS_COMPRESSED(va_extinfo, va_rawsize));
	}
	else
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		va_rawsize = VARSIZE(dval);
		va_extinfo = data_todo;
	}

	/*
	 * Insert the correct table OID into the result TOAST pointer.
	 *
	 * Normally this is the actual OID of the target toast table, but during
	 * table-rewriting operations such as CLUSTER, we have to insert the OID
	 * of the table's real permanent toast table instead.  rd_toastoid is set
	 * if we have to substitute such an OID.
	 */
	if (OidIsValid(rel->rd_toastoid))
		va_toastrelid = rel->rd_toastoid;
	else
		va_toastrelid = RelationGetRelid(toastrel);

	/*
	 * Choose the value ID for this toast value.  During a table rewrite that
	 * preserves rd_toastoid we re-use the prior value ID if we can (see
	 * toast_preserve_valueid); otherwise we pick a fresh one.  For Oid, it
	 * must not conflict with old or new toast values, while for Oid8 the ID
	 * space is large enough that conflicts are not a concern.
	 */
	va_valueid = toast_preserve_valueid(toastrel, rel->rd_toastoid,
										oldexternal, &data_todo);
	if (va_valueid == InvalidOid8)
	{
		if (toast_typid == OID8OID)
		{
			/* The Oid8 space is large enough that we need not check conflicts */
			va_valueid = GetNewObjectId8();
		}
		else
		{
			/*
			 * Choose an unused OID.  During a rewrite we must also avoid IDs
			 * that are still present in the old toast table.
			 */
			do
				va_valueid =
					GetNewOidWithIndex(toastrel,
									   RelationGetRelid(toastidxs[validIndex]),
									   (AttrNumber) 1);
			while (OidIsValid(rel->rd_toastoid) &&
				   toastid_valueid_exists(rel->rd_toastoid, va_valueid));
		}
	}

	max_chunk_size = TOAST_MAX_CHUNK_SIZE(toast_typid);

	/*
	 * Split up the item into chunks
	 */
	while (data_todo > 0)
	{
		HeapTuple	toasttup;
		Datum		t_values[3];
		bool		t_isnull[3] = {0};
		union
		{
			alignas(int32) varlena hdr;

			/* this is to make the union big enough for a chunk: */
			char		data[Max(TOAST_OID_MAX_CHUNK_SIZE,
								 TOAST_OID8_MAX_CHUNK_SIZE) + VARHDRSZ];
		}			chunk_data;
		int32		chunk_size;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Calculate the size of this chunk
		 */
		chunk_size = Min(max_chunk_size, data_todo);

		/*
		 * Build a tuple and store it
		 */
		if (toast_typid == OID8OID)
			t_values[0] = ObjectId8GetDatum(va_valueid);
		else
			t_values[0] = ObjectIdGetDatum((Oid) va_valueid);
		t_values[1] = Int32GetDatum(chunk_seq++);
		SET_VARSIZE(&chunk_data, chunk_size + VARHDRSZ);
		memcpy(VARDATA(&chunk_data), data_p, chunk_size);
		t_values[2] = PointerGetDatum(&chunk_data);

		toasttup = heap_form_tuple(toasttupDesc, t_values, t_isnull);

		heap_insert(toastrel, toasttup, mycid, options, NULL);

		/*
		 * Create the index entry.  We cheat a little here by not using
		 * FormIndexDatum: this relies on the knowledge that the index columns
		 * are the same as the initial columns of the table for all the
		 * indexes.  We also cheat by not providing an IndexInfo: this is okay
		 * for now because btree doesn't need one, but we might have to be
		 * more honest someday.
		 *
		 * Note also that there had better not be any user-created index on
		 * the TOAST table, since we don't bother to update anything else.
		 */
		for (int i = 0; i < num_indexes; i++)
		{
			/* Only index relations marked as ready can be updated */
			if (toastidxs[i]->rd_index->indisready)
				index_insert(toastidxs[i], t_values, t_isnull,
							 &(toasttup->t_self),
							 toastrel,
							 toastidxs[i]->rd_index->indisunique ?
							 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO,
							 false, NULL);
		}

		/*
		 * Free memory
		 */
		heap_freetuple(toasttup);

		/*
		 * Move on to next chunk
		 */
		data_todo -= chunk_size;
		data_p += chunk_size;
	}

	/*
	 * Done - close toast relation and its indexes but keep the lock until
	 * commit, so as a concurrent reindex done directly on the toast relation
	 * would be able to wait for this transaction.
	 */
	toast_close_indexes(toastidxs, num_indexes, NoLock);
	table_close(toastrel, NoLock);

	/*
	 * Create the TOAST pointer value that we'll return
	 */
	if (toast_typid == OID8OID)
	{
		varatt_external_oid8 toast_pointer;

		toast_pointer.va_rawsize = va_rawsize;
		toast_pointer.va_extinfo = va_extinfo;
		VARATT_EXTERNAL_OID8_SET_VALUEID(&toast_pointer, va_valueid);
		toast_pointer.va_toastrelid = va_toastrelid;

		result = (varlena *) palloc(TOAST_OID8_POINTER_SIZE);
		SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK_OID8);
		memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));
	}
	else
	{
		varatt_external_oid toast_pointer;

		toast_pointer.va_rawsize = va_rawsize;
		toast_pointer.va_extinfo = va_extinfo;
		toast_pointer.va_valueid = (Oid) va_valueid;
		toast_pointer.va_toastrelid = va_toastrelid;

		result = (varlena *) palloc(TOAST_OID_POINTER_SIZE);
		SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK_OID);
		memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));
	}

	return PointerGetDatum(result);
}

/* ----------
 * toast_valueid_scankey_init -
 *
 *	Initialize a scan key that matches the value ID column of a TOAST table.
 * ----------
 */
void
toast_valueid_scankey_init(ScanKey entry, Oid toast_typid, Oid8 valueid)
{
	Assert(toast_typid == OIDOID || toast_typid == OID8OID);
	if (toast_typid == OID8OID)
		ScanKeyInit(entry, (AttrNumber) 1,
					BTEqualStrategyNumber, F_OID8EQ,
					ObjectId8GetDatum(valueid));
	else
		ScanKeyInit(entry, (AttrNumber) 1,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum((Oid) valueid));
}

/* ----------
 * toast_delete_datum -
 *
 *	Delete a single external stored value.
 * ----------
 */
void
toast_delete_datum(Relation rel, Datum value, bool is_speculative)
{
	varlena    *attr = (varlena *) DatumGetPointer(value);
	toast_external_data toast_ext_data;
	Relation	toastrel;
	Relation   *toastidxs;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	HeapTuple	toasttup;
	int			num_indexes;
	int			validIndex;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		return;

	/*
	 * Decode the pointer to get the toast relation OID and value ID. The
	 * vartag tells us everything we need - no TOAST table schema lookup
	 * required.
	 */
	toast_external_info_get(attr, &toast_ext_data);

	/*
	 * Open the toast relation and its indexes
	 */
	toastrel = table_open(toast_ext_data.toastrelid, RowExclusiveLock);

	/* Fetch valid relation used for process */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Setup a scan key to find chunks with matching va_valueid
	 */
	toast_valueid_scankey_init(&toastkey,
							   TupleDescAttr(toastrel->rd_att, 0)->atttypid,
							   toast_ext_data.valueid);

	/*
	 * Find all the chunks.  (We don't actually care whether we see them in
	 * sequence or not, but since we've already locked the index we might as
	 * well use systable_beginscan_ordered.)
	 */
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   get_toast_snapshot(), 1, &toastkey);
	while ((toasttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, delete it
		 */
		if (is_speculative)
			heap_abort_speculative(toastrel, &toasttup->t_self);
		else
			simple_heap_delete(toastrel, &toasttup->t_self);
	}

	/*
	 * End scan and close relations but keep the lock until commit, so as a
	 * concurrent reindex done directly on the toast relation would be able to
	 * wait for this transaction.
	 */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, NoLock);
	table_close(toastrel, NoLock);
}

/* ----------
 * toastrel_valueid_exists -
 *
 *	Test whether a toast value with the given ID exists in the toast relation.
 *	For safety, we consider a value to exist if there are either live or dead
 *	toast rows with that ID; see notes for GetNewOidWithIndex().
 * ----------
 */
static bool
toastrel_valueid_exists(Relation toastrel, Oid8 valueid)
{
	bool		result = false;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	int			num_indexes;
	int			validIndex;
	Relation   *toastidxs;
	Oid			toast_typid;

	/* Fetch a valid index relation */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	toast_typid = TupleDescAttr(toastrel->rd_att, 0)->atttypid;

	/*
	 * Setup a scan key to find chunks with matching va_valueid
	 */
	toast_valueid_scankey_init(&toastkey, toast_typid, valueid);

	/*
	 * Is there any such chunk?
	 */
	toastscan = systable_beginscan(toastrel,
								   RelationGetRelid(toastidxs[validIndex]),
								   true, SnapshotAny, 1, &toastkey);

	if (systable_getnext(toastscan) != NULL)
		result = true;

	systable_endscan(toastscan);

	/* Clean up */
	toast_close_indexes(toastidxs, num_indexes, RowExclusiveLock);

	return result;
}

/* ----------
 * toastid_valueid_exists -
 *
 *	As above, but work from toast rel's OID not an open relation
 * ----------
 */
static bool
toastid_valueid_exists(Oid toastrelid, Oid8 valueid)
{
	bool		result;
	Relation	toastrel;

	toastrel = table_open(toastrelid, AccessShareLock);

	result = toastrel_valueid_exists(toastrel, valueid);

	table_close(toastrel, AccessShareLock);

	return result;
}

/* ----------
 * toast_get_valid_index
 *
 *	Get OID of valid index associated to given toast relation. A toast
 *	relation can have only one valid index at the same time.
 */
Oid
toast_get_valid_index(Oid toastoid, LOCKMODE lock)
{
	int			num_indexes;
	int			validIndex;
	Oid			validIndexOid;
	Relation   *toastidxs;
	Relation	toastrel;

	/* Open the toast relation */
	toastrel = table_open(toastoid, lock);

	/* Look for the valid index of the toast relation */
	validIndex = toast_open_indexes(toastrel,
									lock,
									&toastidxs,
									&num_indexes);
	validIndexOid = RelationGetRelid(toastidxs[validIndex]);

	/* Close the toast relation and all its indexes */
	toast_close_indexes(toastidxs, num_indexes, NoLock);
	table_close(toastrel, NoLock);

	return validIndexOid;
}

/* ----------
 * toast_open_indexes
 *
 *	Get an array of the indexes associated to the given toast relation
 *	and return as well the position of the valid index used by the toast
 *	relation in this array. It is the responsibility of the caller of this
 *	function to close the indexes as well as free them.
 */
int
toast_open_indexes(Relation toastrel,
				   LOCKMODE lock,
				   Relation **toastidxs,
				   int *num_indexes)
{
	int			i = 0;
	int			res = 0;
	bool		found = false;
	List	   *indexlist;
	ListCell   *lc;

	/* Get index list of the toast relation */
	indexlist = RelationGetIndexList(toastrel);
	Assert(indexlist != NIL);

	*num_indexes = list_length(indexlist);

	/* Open all the index relations */
	*toastidxs = palloc_array(Relation, *num_indexes);
	foreach(lc, indexlist)
		(*toastidxs)[i++] = index_open(lfirst_oid(lc), lock);

	/* Fetch the first valid index in list */
	for (i = 0; i < *num_indexes; i++)
	{
		Relation	toastidx = (*toastidxs)[i];

		if (toastidx->rd_index->indisvalid)
		{
			res = i;
			found = true;
			break;
		}
	}

	/*
	 * Free index list, not necessary anymore as relations are opened and a
	 * valid index has been found.
	 */
	list_free(indexlist);

	/*
	 * The toast relation should have one valid index, so something is going
	 * wrong if there is nothing.
	 */
	if (!found)
		elog(ERROR, "no valid index found for toast relation with Oid %u",
			 RelationGetRelid(toastrel));

	return res;
}

/* ----------
 * toast_close_indexes
 *
 *	Close an array of indexes for a toast relation and free it. This should
 *	be called for a set of indexes opened previously with toast_open_indexes.
 */
void
toast_close_indexes(Relation *toastidxs, int num_indexes, LOCKMODE lock)
{
	int			i;

	/* Close relations and clean up things */
	for (i = 0; i < num_indexes; i++)
		index_close(toastidxs[i], lock);
	pfree(toastidxs);
}

/* ----------
 * get_toast_snapshot
 *
 *	Return the TOAST snapshot. Detoasting *must* happen in the same
 *	transaction that originally fetched the toast pointer.
 */
Snapshot
get_toast_snapshot(void)
{
	/*
	 * We cannot directly check that detoasting happens in the same
	 * transaction that originally fetched the toast pointer, but at least
	 * check that the session has some active snapshots. It might not if, for
	 * example, a procedure fetches a toasted value into a local variable,
	 * commits, and then tries to detoast the value. Such coding is unsafe,
	 * because once we commit there is nothing to prevent the toast data from
	 * being deleted. (This is not very much protection, because in many
	 * scenarios the procedure would have already created a new transaction
	 * snapshot, preventing us from detecting the problem. But it's better
	 * than nothing.)
	 */
	if (!HaveRegisteredOrActiveSnapshot())
		elog(ERROR, "cannot fetch toast data without an active snapshot");

	return &SnapshotToastData;
}
