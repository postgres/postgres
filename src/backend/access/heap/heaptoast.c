/*-------------------------------------------------------------------------
 *
 * heaptoast.c
 *	  Heap-specific definitions for external and compressed storage
 *	  of variable size attributes.
 *
 * Copyright (c) 2000-2023, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/heaptoast.c
 *
 *
 * INTERFACE ROUTINES
 *		heap_toast_insert_or_update -
 *			Try to make a given tuple fit into one page by compressing
 *			or moving off attributes
 *
 *		heap_toast_delete -
 *			Reclaim toast storage when a tuple is deleted
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/heaptoast.h"
#include "access/toast_helper.h"
#include "access/toast_internals.h"
#include "utils/fmgroids.h"


/* ----------
 * heap_toast_delete -
 *
 *	Cascaded delete toast-entries on DELETE
 * ----------
 */
void
heap_toast_delete(Relation rel, HeapTuple oldtup, bool is_speculative)
{
	TupleDesc	tupleDesc;
	Datum		toast_values[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];

	/*
	 * We should only ever be called for tuples of plain relations or
	 * materialized views --- recursing on a toast rel is bad news.
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * Get the tuple descriptor and break down the tuple into fields.
	 *
	 * NOTE: it's debatable whether to use heap_deform_tuple() here or just
	 * heap_getattr() only the varlena columns.  The latter could win if there
	 * are few varlena columns and many non-varlena ones. However,
	 * heap_deform_tuple costs only O(N) while the heap_getattr way would cost
	 * O(N^2) if there are many varlena columns, so it seems better to err on
	 * the side of linear cost.  (We won't even be here unless there's at
	 * least one varlena column, by the way.)
	 */
	tupleDesc = rel->rd_att;

	Assert(tupleDesc->natts <= MaxHeapAttributeNumber);
	heap_deform_tuple(oldtup, tupleDesc, toast_values, toast_isnull);

	/* Do the real work. */
	toast_delete_external(rel, toast_values, toast_isnull, is_speculative);
}


/* ----------
 * heap_toast_insert_or_update -
 *
 *	Delete no-longer-used toast-entries and create new ones to
 *	make the new tuple fit on INSERT or UPDATE
 *
 * Inputs:
 *	newtup: the candidate new tuple to be inserted
 *	oldtup: the old row version for UPDATE, or NULL for INSERT
 *	options: options to be passed to heap_insert() for toast rows
 * Result:
 *	either newtup if no toasting is needed, or a palloc'd modified tuple
 *	that is what should actually get stored
 *
 * NOTE: neither newtup nor oldtup will be modified.  This is a change
 * from the pre-8.1 API of this routine.
 * ----------
 */
HeapTuple
heap_toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup,
							int options)
{
	HeapTuple	result_tuple;
	TupleDesc	tupleDesc;
	int			numAttrs;

	Size		maxDataLen;
	Size		hoff;

	bool		toast_isnull[MaxHeapAttributeNumber];
	bool		toast_oldisnull[MaxHeapAttributeNumber];
	Datum		toast_values[MaxHeapAttributeNumber];
	Datum		toast_oldvalues[MaxHeapAttributeNumber];
	ToastAttrInfo toast_attr[MaxHeapAttributeNumber];
	ToastTupleContext ttc;

	/*
	 * Ignore the INSERT_SPECULATIVE option. Speculative insertions/super
	 * deletions just normally insert/delete the toast values. It seems
	 * easiest to deal with that here, instead on, potentially, multiple
	 * callers.
	 */
	options &= ~HEAP_INSERT_SPECULATIVE;

	/*
	 * We should only ever be called for tuples of plain relations or
	 * materialized views --- recursing on a toast rel is bad news.
	 */
	Assert(rel->rd_rel->relkind == RELKIND_RELATION ||
		   rel->rd_rel->relkind == RELKIND_MATVIEW);

	/*
	 * Get the tuple descriptor and break down the tuple(s) into fields.
	 */
	tupleDesc = rel->rd_att;
	numAttrs = tupleDesc->natts;

	Assert(numAttrs <= MaxHeapAttributeNumber);
	heap_deform_tuple(newtup, tupleDesc, toast_values, toast_isnull);
	if (oldtup != NULL)
		heap_deform_tuple(oldtup, tupleDesc, toast_oldvalues, toast_oldisnull);

	/* ----------
	 * Prepare for toasting
	 * ----------
	 */
	ttc.ttc_rel = rel;
	ttc.ttc_values = toast_values;
	ttc.ttc_isnull = toast_isnull;
	if (oldtup == NULL)
	{
		ttc.ttc_oldvalues = NULL;
		ttc.ttc_oldisnull = NULL;
	}
	else
	{
		ttc.ttc_oldvalues = toast_oldvalues;
		ttc.ttc_oldisnull = toast_oldisnull;
	}
	ttc.ttc_attr = toast_attr;
	toast_tuple_init(&ttc);

	/* ----------
	 * Compress and/or save external until data fits into target length
	 *
	 *	1: Inline compress attributes with attstorage EXTENDED, and store very
	 *	   large attributes with attstorage EXTENDED or EXTERNAL external
	 *	   immediately
	 *	2: Store attributes with attstorage EXTENDED or EXTERNAL external
	 *	3: Inline compress attributes with attstorage MAIN
	 *	4: Store attributes with attstorage MAIN external
	 * ----------
	 */

	/* compute header overhead --- this should match heap_form_tuple() */
	hoff = SizeofHeapTupleHeader;
	if ((ttc.ttc_flags & TOAST_HAS_NULLS) != 0)
		hoff += BITMAPLEN(numAttrs);
	hoff = MAXALIGN(hoff);
	/* now convert to a limit on the tuple data size */
	maxDataLen = RelationGetToastTupleTarget(rel, TOAST_TUPLE_TARGET) - hoff;

	/*
	 * Look for attributes with attstorage EXTENDED to compress.  Also find
	 * large attributes with attstorage EXTENDED or EXTERNAL, and store them
	 * external.
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, true, false);
		if (biggest_attno < 0)
			break;

		/*
		 * Attempt to compress it inline, if it has attstorage EXTENDED
		 */
		if (TupleDescAttr(tupleDesc, biggest_attno)->attstorage == TYPSTORAGE_EXTENDED)
			toast_tuple_try_compression(&ttc, biggest_attno);
		else
		{
			/*
			 * has attstorage EXTERNAL, ignore on subsequent compression
			 * passes
			 */
			toast_attr[biggest_attno].tai_colflags |= TOASTCOL_INCOMPRESSIBLE;
		}

		/*
		 * If this value is by itself more than maxDataLen (after compression
		 * if any), push it out to the toast table immediately, if possible.
		 * This avoids uselessly compressing other fields in the common case
		 * where we have one long field and several short ones.
		 *
		 * XXX maybe the threshold should be less than maxDataLen?
		 */
		if (toast_attr[biggest_attno].tai_size > maxDataLen &&
			rel->rd_rel->reltoastrelid != InvalidOid)
			toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * Second we look for attributes of attstorage EXTENDED or EXTERNAL that
	 * are still inline, and make them external.  But skip this if there's no
	 * toast table to push them to.
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, false, false);
		if (biggest_attno < 0)
			break;
		toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * Round 3 - this time we take attributes with storage MAIN into
	 * compression
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, true, true);
		if (biggest_attno < 0)
			break;

		toast_tuple_try_compression(&ttc, biggest_attno);
	}

	/*
	 * Finally we store attributes of type MAIN externally.  At this point we
	 * increase the target tuple size, so that MAIN attributes aren't stored
	 * externally unless really necessary.
	 */
	maxDataLen = TOAST_TUPLE_TARGET_MAIN - hoff;

	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno;

		biggest_attno = toast_tuple_find_biggest_attribute(&ttc, false, true);
		if (biggest_attno < 0)
			break;

		toast_tuple_externalize(&ttc, biggest_attno, options);
	}

	/*
	 * In the case we toasted any values, we need to build a new heap tuple
	 * with the changed values.
	 */
	if ((ttc.ttc_flags & TOAST_NEEDS_CHANGE) != 0)
	{
		HeapTupleHeader olddata = newtup->t_data;
		HeapTupleHeader new_data;
		int32		new_header_len;
		int32		new_data_len;
		int32		new_tuple_len;

		/*
		 * Calculate the new size of the tuple.
		 *
		 * Note: we used to assume here that the old tuple's t_hoff must equal
		 * the new_header_len value, but that was incorrect.  The old tuple
		 * might have a smaller-than-current natts, if there's been an ALTER
		 * TABLE ADD COLUMN since it was stored; and that would lead to a
		 * different conclusion about the size of the null bitmap, or even
		 * whether there needs to be one at all.
		 */
		new_header_len = SizeofHeapTupleHeader;
		if ((ttc.ttc_flags & TOAST_HAS_NULLS) != 0)
			new_header_len += BITMAPLEN(numAttrs);
		new_header_len = MAXALIGN(new_header_len);
		new_data_len = heap_compute_data_size(tupleDesc,
											  toast_values, toast_isnull);
		new_tuple_len = new_header_len + new_data_len;

		/*
		 * Allocate and zero the space needed, and fill HeapTupleData fields.
		 */
		result_tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + new_tuple_len);
		result_tuple->t_len = new_tuple_len;
		result_tuple->t_self = newtup->t_self;
		result_tuple->t_tableOid = newtup->t_tableOid;
		new_data = (HeapTupleHeader) ((char *) result_tuple + HEAPTUPLESIZE);
		result_tuple->t_data = new_data;

		/*
		 * Copy the existing tuple header, but adjust natts and t_hoff.
		 */
		memcpy(new_data, olddata, SizeofHeapTupleHeader);
		HeapTupleHeaderSetNatts(new_data, numAttrs);
		new_data->t_hoff = new_header_len;

		/* Copy over the data, and fill the null bitmap if needed */
		heap_fill_tuple(tupleDesc,
						toast_values,
						toast_isnull,
						(char *) new_data + new_header_len,
						new_data_len,
						&(new_data->t_infomask),
						((ttc.ttc_flags & TOAST_HAS_NULLS) != 0) ?
						new_data->t_bits : NULL);
	}
	else
		result_tuple = newtup;

	toast_tuple_cleanup(&ttc);

	return result_tuple;
}


/* ----------
 * toast_flatten_tuple -
 *
 *	"Flatten" a tuple to contain no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 *
 *	Note: we expect the caller already checked HeapTupleHasExternal(tup),
 *	so there is no need for a short-circuit path.
 * ----------
 */
HeapTuple
toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc)
{
	HeapTuple	new_tuple;
	int			numAttrs = tupleDesc->natts;
	int			i;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/*
	 * Break down the tuple into fields.
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(tup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * Look at non-null varlena attributes
		 */
		if (!toast_isnull[i] && TupleDescAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = detoast_external_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * Form the reconfigured tuple.
	 */
	new_tuple = heap_form_tuple(tupleDesc, toast_values, toast_isnull);

	/*
	 * Be sure to copy the tuple's identity fields.  We also make a point of
	 * copying visibility info, just in case anybody looks at those fields in
	 * a syscache entry.
	 */
	new_tuple->t_self = tup->t_self;
	new_tuple->t_tableOid = tup->t_tableOid;

	new_tuple->t_data->t_choice = tup->t_data->t_choice;
	new_tuple->t_data->t_ctid = tup->t_data->t_ctid;
	new_tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask |=
		tup->t_data->t_infomask & HEAP_XACT_MASK;
	new_tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
	new_tuple->t_data->t_infomask2 |=
		tup->t_data->t_infomask2 & HEAP2_XACT_MASK;

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return new_tuple;
}


/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *	"Flatten" a tuple containing out-of-line toasted fields into a Datum.
 *	The result is always palloc'd in the current memory context.
 *
 *	We have a general rule that Datums of container types (rows, arrays,
 *	ranges, etc) must not contain any external TOAST pointers.  Without
 *	this rule, we'd have to look inside each Datum when preparing a tuple
 *	for storage, which would be expensive and would fail to extend cleanly
 *	to new sorts of container types.
 *
 *	However, we don't want to say that tuples represented as HeapTuples
 *	can't contain toasted fields, so instead this routine should be called
 *	when such a HeapTuple is being converted into a Datum.
 *
 *	While we're at it, we decompress any compressed fields too.  This is not
 *	necessary for correctness, but reflects an expectation that compression
 *	will be more effective if applied to the whole tuple not individual
 *	fields.  We are not so concerned about that that we want to deconstruct
 *	and reconstruct tuples just to get rid of compressed fields, however.
 *	So callers typically won't call this unless they see that the tuple has
 *	at least one external field.
 *
 *	On the other hand, in-line short-header varlena fields are left alone.
 *	If we "untoasted" them here, they'd just get changed back to short-header
 *	format anyway within heap_fill_tuple.
 * ----------
 */
Datum
toast_flatten_tuple_to_datum(HeapTupleHeader tup,
							 uint32 tup_len,
							 TupleDesc tupleDesc)
{
	HeapTupleHeader new_data;
	int32		new_header_len;
	int32		new_data_len;
	int32		new_tuple_len;
	HeapTupleData tmptup;
	int			numAttrs = tupleDesc->natts;
	int			i;
	bool		has_nulls = false;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = tup_len;
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = tup;

	/*
	 * Break down the tuple into fields.
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	heap_deform_tuple(&tmptup, tupleDesc, toast_values, toast_isnull);

	memset(toast_free, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * Look at non-null varlena attributes
		 */
		if (toast_isnull[i])
			has_nulls = true;
		else if (TupleDescAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value) ||
				VARATT_IS_COMPRESSED(new_value))
			{
				new_value = detoast_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * Calculate the new size of the tuple.
	 *
	 * This should match the reconstruction code in
	 * heap_toast_insert_or_update.
	 */
	new_header_len = SizeofHeapTupleHeader;
	if (has_nulls)
		new_header_len += BITMAPLEN(numAttrs);
	new_header_len = MAXALIGN(new_header_len);
	new_data_len = heap_compute_data_size(tupleDesc,
										  toast_values, toast_isnull);
	new_tuple_len = new_header_len + new_data_len;

	new_data = (HeapTupleHeader) palloc0(new_tuple_len);

	/*
	 * Copy the existing tuple header, but adjust natts and t_hoff.
	 */
	memcpy(new_data, tup, SizeofHeapTupleHeader);
	HeapTupleHeaderSetNatts(new_data, numAttrs);
	new_data->t_hoff = new_header_len;

	/* Set the composite-Datum header fields correctly */
	HeapTupleHeaderSetDatumLength(new_data, new_tuple_len);
	HeapTupleHeaderSetTypeId(new_data, tupleDesc->tdtypeid);
	HeapTupleHeaderSetTypMod(new_data, tupleDesc->tdtypmod);

	/* Copy over the data, and fill the null bitmap if needed */
	heap_fill_tuple(tupleDesc,
					toast_values,
					toast_isnull,
					(char *) new_data + new_header_len,
					new_data_len,
					&(new_data->t_infomask),
					has_nulls ? new_data->t_bits : NULL);

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));

	return PointerGetDatum(new_data);
}


/* ----------
 * toast_build_flattened_tuple -
 *
 *	Build a tuple containing no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 *
 *	This is essentially just like heap_form_tuple, except that it will
 *	expand any external-data pointers beforehand.
 *
 *	It's not very clear whether it would be preferable to decompress
 *	in-line compressed datums while at it.  For now, we don't.
 * ----------
 */
HeapTuple
toast_build_flattened_tuple(TupleDesc tupleDesc,
							Datum *values,
							bool *isnull)
{
	HeapTuple	new_tuple;
	int			numAttrs = tupleDesc->natts;
	int			num_to_free;
	int			i;
	Datum		new_values[MaxTupleAttributeNumber];
	Pointer		freeable_values[MaxTupleAttributeNumber];

	/*
	 * We can pass the caller's isnull array directly to heap_form_tuple, but
	 * we potentially need to modify the values array.
	 */
	Assert(numAttrs <= MaxTupleAttributeNumber);
	memcpy(new_values, values, numAttrs * sizeof(Datum));

	num_to_free = 0;
	for (i = 0; i < numAttrs; i++)
	{
		/*
		 * Look at non-null varlena attributes
		 */
		if (!isnull[i] && TupleDescAttr(tupleDesc, i)->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(new_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = detoast_external_attr(new_value);
				new_values[i] = PointerGetDatum(new_value);
				freeable_values[num_to_free++] = (Pointer) new_value;
			}
		}
	}

	/*
	 * Form the reconfigured tuple.
	 */
	new_tuple = heap_form_tuple(tupleDesc, new_values, isnull);

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < num_to_free; i++)
		pfree(freeable_values[i]);

	return new_tuple;
}

/*
 * Fetch a TOAST slice from a heap table.
 *
 * toastrel is the relation from which chunks are to be fetched.
 * valueid identifies the TOAST value from which chunks are being fetched.
 * attrsize is the total size of the TOAST value.
 * sliceoffset is the byte offset within the TOAST value from which to fetch.
 * slicelength is the number of bytes to be fetched from the TOAST value.
 * result is the varlena into which the results should be written.
 */
void
heap_fetch_toast_slice(Relation toastrel, Oid valueid, int32 attrsize,
					   int32 sliceoffset, int32 slicelength,
					   struct varlena *result)
{
	Relation   *toastidxs;
	ScanKeyData toastkey[3];
	TupleDesc	toasttupDesc = toastrel->rd_att;
	int			nscankeys;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	int32		expectedchunk;
	int32		totalchunks = ((attrsize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;
	int			startchunk;
	int			endchunk;
	int			num_indexes;
	int			validIndex;
	SnapshotData SnapshotToast;

	/* Look for the valid index of toast relation */
	validIndex = toast_open_indexes(toastrel,
									AccessShareLock,
									&toastidxs,
									&num_indexes);

	startchunk = sliceoffset / TOAST_MAX_CHUNK_SIZE;
	endchunk = (sliceoffset + slicelength - 1) / TOAST_MAX_CHUNK_SIZE;
	Assert(endchunk <= totalchunks);

	/* Set up a scan key to fetch from the index. */
	ScanKeyInit(&toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(valueid));

	/*
	 * No additional condition if fetching all chunks. Otherwise, use an
	 * equality condition for one chunk, and a range condition otherwise.
	 */
	if (startchunk == 0 && endchunk == totalchunks - 1)
		nscankeys = 1;
	else if (startchunk == endchunk)
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTEqualStrategyNumber, F_INT4EQ,
					Int32GetDatum(startchunk));
		nscankeys = 2;
	}
	else
	{
		ScanKeyInit(&toastkey[1],
					(AttrNumber) 2,
					BTGreaterEqualStrategyNumber, F_INT4GE,
					Int32GetDatum(startchunk));
		ScanKeyInit(&toastkey[2],
					(AttrNumber) 2,
					BTLessEqualStrategyNumber, F_INT4LE,
					Int32GetDatum(endchunk));
		nscankeys = 3;
	}

	/* Prepare for scan */
	init_toast_snapshot(&SnapshotToast);
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   &SnapshotToast, nscankeys, toastkey);

	/*
	 * Read the chunks by index
	 *
	 * The index is on (valueid, chunkidx) so they will come in order
	 */
	expectedchunk = startchunk;
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		int32		curchunk;
		Pointer		chunk;
		bool		isnull;
		char	   *chunkdata;
		int32		chunksize;
		int32		expected_size;
		int32		chcpystrt;
		int32		chcpyend;

		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		curchunk = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		if (!VARATT_IS_EXTENDED(chunk))
		{
			chunksize = VARSIZE(chunk) - VARHDRSZ;
			chunkdata = VARDATA(chunk);
		}
		else if (VARATT_IS_SHORT(chunk))
		{
			/* could happen due to heap_form_tuple doing its thing */
			chunksize = VARSIZE_SHORT(chunk) - VARHDRSZ_SHORT;
			chunkdata = VARDATA_SHORT(chunk);
		}
		else
		{
			/* should never happen */
			elog(ERROR, "found toasted toast chunk for toast value %u in %s",
				 valueid, RelationGetRelationName(toastrel));
			chunksize = 0;		/* keep compiler quiet */
			chunkdata = NULL;
		}

		/*
		 * Some checks on the data we've found
		 */
		if (curchunk != expectedchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk number %d (expected %d) for toast value %u in %s",
									 curchunk, expectedchunk, valueid,
									 RelationGetRelationName(toastrel))));
		if (curchunk > endchunk)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
									 curchunk,
									 startchunk, endchunk, valueid,
									 RelationGetRelationName(toastrel))));
		expected_size = curchunk < totalchunks - 1 ? TOAST_MAX_CHUNK_SIZE
			: attrsize - ((totalchunks - 1) * TOAST_MAX_CHUNK_SIZE);
		if (chunksize != expected_size)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg_internal("unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s",
									 chunksize, expected_size,
									 curchunk, totalchunks, valueid,
									 RelationGetRelationName(toastrel))));

		/*
		 * Copy the data into proper place in our result
		 */
		chcpystrt = 0;
		chcpyend = chunksize - 1;
		if (curchunk == startchunk)
			chcpystrt = sliceoffset % TOAST_MAX_CHUNK_SIZE;
		if (curchunk == endchunk)
			chcpyend = (sliceoffset + slicelength - 1) % TOAST_MAX_CHUNK_SIZE;

		memcpy(VARDATA(result) +
			   (curchunk * TOAST_MAX_CHUNK_SIZE - sliceoffset) + chcpystrt,
			   chunkdata + chcpystrt,
			   (chcpyend - chcpystrt) + 1);

		expectedchunk++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (expectedchunk != (endchunk + 1))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg_internal("missing chunk number %d for toast value %u in %s",
								 expectedchunk, valueid,
								 RelationGetRelationName(toastrel))));

	/* End scan and close indexes. */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
}
