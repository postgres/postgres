/*-------------------------------------------------------------------------
 *
 * tuptoaster.c
 *	  Support routines for external and compressed storage of
 *	  variable size attributes.
 *
 * Copyright (c) 2000, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/heap/tuptoaster.c,v 1.5 2000/07/03 23:09:19 wieck Exp $
 *
 *
 * INTERFACE ROUTINES
 *		heap_tuple_toast_attrs -
 *			Try to make a given tuple fit into one page by compressing
 *			or moving off attributes
 *
 *		heap_tuple_untoast_attr -
 *			Fetch back a given value from the "secondary" relation
 *
 *-------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "postgres.h"

#include "access/heapam.h"
#include "access/genam.h"
#include "access/tuptoaster.h"
#include "catalog/catalog.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_lzcompress.h"


#ifdef TUPLE_TOASTER_ACTIVE
#undef TOAST_DEBUG

static void			toast_delete(Relation rel, HeapTuple oldtup);
static void			toast_delete_datum(Relation rel, Datum value);
static void			toast_insert_or_update(Relation rel, HeapTuple newtup,
								HeapTuple oldtup);
static Datum		toast_compress_datum(Datum value);
static Datum		toast_save_datum(Relation rel, Oid mainoid, int16 attno, Datum value);
								
static varattrib   *toast_fetch_datum(varattrib *attr);


/* ----------
 * heap_tuple_toast_attrs -
 *
 *	This is the central public entry point for toasting from heapam.
 *
 *	Calls the appropriate event specific action.
 * ----------
 */
void
heap_tuple_toast_attrs(Relation rel, HeapTuple newtup, HeapTuple oldtup)
{
	if (newtup == NULL)
		toast_delete(rel, oldtup);
	else
		toast_insert_or_update(rel, newtup, oldtup);
}


/* ----------
 * heap_tuple_untoast_attr -
 *
 *	Public entry point to get back a toasted value from compression
 *	or external storage.
 * ----------
 */
varattrib *
heap_tuple_untoast_attr(varattrib *attr)
{
	varattrib	*result;

	if (VARATT_IS_EXTERNAL(attr))
	{
		if (VARATT_IS_COMPRESSED(attr))
		{
			/* ----------
			 * This is an external stored compressed value
			 * Fetch it from the toast heap and decompress.
			 * ----------
			 */
			varattrib *tmp;

			tmp = toast_fetch_datum(attr);
			result = (varattrib *)palloc(attr->va_content.va_external.va_rawsize
								+ VARHDRSZ);
			VARATT_SIZEP(result) = attr->va_content.va_external.va_rawsize
								+ VARHDRSZ;
			pglz_decompress((PGLZ_Header *)tmp, VARATT_DATA(result));

			pfree(tmp);
		}
		else
		{
			/* ----------
			 * This is an external stored plain value
			 * ----------
			 */
			result = toast_fetch_datum(attr);
		}
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/* ----------
		 * This is a compressed value inside of the main tuple
		 * ----------
		 */
		result = (varattrib *)palloc(attr->va_content.va_compressed.va_rawsize
							+ VARHDRSZ);
		VARATT_SIZEP(result) = attr->va_content.va_compressed.va_rawsize
							+ VARHDRSZ;
		pglz_decompress((PGLZ_Header *)attr, VARATT_DATA(result));
	}
	else
		/* ----------
		 * This is a plain value inside of the main tuple - why am I called?
		 * ----------
		 */
		return attr;

	return result;
}


/* ----------
 * toast_delete -
 *
 *	Cascaded delete toast-entries on DELETE
 * ----------
 */
static void
toast_delete(Relation rel, HeapTuple oldtup)
{
	TupleDesc			tupleDesc;
	Form_pg_attribute  *att;
	int					numAttrs;
	int					i;
	Datum				value;
	bool				isnull;

	/* ----------
	 * Get the tuple descriptor, the number of and attribute
	 * descriptors.
	 * ----------
	 */
	tupleDesc	= rel->rd_att;
	numAttrs	= tupleDesc->natts;
	att			= tupleDesc->attrs;

	/* ----------
	 * Check for external stored attributes and delete them
	 * from the secondary relation.
	 * ----------
	 */
	for (i = 0; i < numAttrs; i++)
	{
		value = heap_getattr(oldtup, i + 1, tupleDesc, &isnull);
		if (!isnull && att[i]->attlen == -1)
			if (VARATT_IS_EXTERNAL(value))
				toast_delete_datum(rel, value);
	}
}


/* ----------
 * toast_insert_or_update -
 *
 *	Delete no more used toast-entries and create new ones to
 *	make the new tuple fit on INSERT or UPDATE
 * ----------
 */
static void
toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup)
{
	TupleDesc			tupleDesc;
	Form_pg_attribute  *att;
	int					numAttrs;
	int					i;
	bool				old_isnull;
	bool				new_isnull;

	bool				need_change = false;
	bool				need_free   = false;
	bool				need_delold = false;
	bool				has_nulls   = false;

	Size				maxDataLen;

	char				toast_action[MaxHeapAttributeNumber];
	char				toast_nulls[MaxHeapAttributeNumber];
	Datum				toast_values[MaxHeapAttributeNumber];
	int32				toast_sizes[MaxHeapAttributeNumber];
	bool				toast_free[MaxHeapAttributeNumber];
	bool				toast_delold[MaxHeapAttributeNumber];

	/* ----------
	 * Get the tuple descriptor, the number of and attribute
	 * descriptors and the location of the tuple values.
	 * ----------
	 */
	tupleDesc	= rel->rd_att;
	numAttrs	= tupleDesc->natts;
	att			= tupleDesc->attrs;

	/* ----------
	 * Then collect information about the values given
	 * ----------
	 */
	memset(toast_action, ' ', numAttrs * sizeof(char));
	memset(toast_nulls,  ' ', numAttrs * sizeof(char));
	memset(toast_free,   0,   numAttrs * sizeof(bool));
	memset(toast_delold, 0,   numAttrs * sizeof(bool));
	for (i = 0; i < numAttrs; i++)
	{
		varattrib	   *old_value;
		varattrib	   *new_value;

		if (oldtup != NULL)
		{
			/* ----------
			 * For UPDATE get the old and new values of this attribute
			 * ----------
			 */
			old_value = (varattrib *)DatumGetPointer(
						heap_getattr(oldtup, i + 1, tupleDesc, &old_isnull));
			toast_values[i] = 
						heap_getattr(newtup, i + 1, tupleDesc, &new_isnull);
			new_value = (varattrib *)DatumGetPointer(toast_values[i]);

			/* ----------
			 * If the old value is an external stored one, check if it
			 * has changed so we have to detele it later.
			 * ----------
			 */
			if (!old_isnull && att[i]->attlen == -1 && 
						VARATT_IS_EXTERNAL(old_value))
			{
				if (new_isnull || !VARATT_IS_EXTERNAL(new_value) ||
						old_value->va_content.va_external.va_rowid !=
						new_value->va_content.va_external.va_rowid ||
						old_value->va_content.va_external.va_attno !=
						new_value->va_content.va_external.va_attno)
				{
					/* ----------
					 * The old external store value isn't needed any
					 * more after the update
					 * ----------
					 */
					toast_delold[i] = true;
					need_delold = true;
				}
				else
				{
					/* ----------
					 * This attribute isn't changed by this update
					 * so we reuse the original reference to the old
					 * value in the new tuple.
					 * ----------
					 */
					toast_action[i] = 'p';
					toast_sizes[i] = VARATT_SIZE(toast_values[i]);
					continue;
				}
			}
		}
		else
		{
			/* ----------
			 * For INSERT simply get the new value
			 * ----------
			 */
			toast_values[i] = 
						heap_getattr(newtup, i + 1, tupleDesc, &new_isnull);
		}

		/* ----------
		 * Handle NULL attributes
		 * ----------
		 */
		if (new_isnull)
		{
			toast_action[i] = 'p';
			toast_nulls[i] = 'n';
			has_nulls = true;
			continue;
		}

		/* ----------
		 * Now look at varsize attributes
		 * ----------
		 */
		if (att[i]->attlen == -1)
		{
			/* ----------
			 * If the tables attribute say's PLAIN allways, we
			 * do so below.
			 * ----------
			 */
			if (att[i]->attstorage == 'p')
				toast_action[i] = 'p';

			/* ----------
			 * We're running for UPDATE, so any TOASTed value we find
			 * still in the tuple must be someone elses we cannot reuse.
			 * Expand it to plain and eventually toast it again below.
			 * ----------
			 */
			if (VARATT_IS_EXTENDED(DatumGetPointer(toast_values[i])))
			{
				toast_values[i] = PointerGetDatum(heap_tuple_untoast_attr(
					(varattrib *)DatumGetPointer(toast_values[i])));
				toast_free[i] = true;
				need_change = true;
				need_free = true;
			}

			/* ----------
			 * Remember the size of this attribute
			 * ----------
			 */
			toast_sizes[i]  = VARATT_SIZE(DatumGetPointer(toast_values[i]));
		}
		else
		{
			/* ----------
			 * Not a variable size attribute, plain storage allways
			 * ----------
			 */
			toast_action[i] = 'p';
			toast_sizes[i]  = att[i]->attlen;
		}
	}

	/* ----------
	 * Compress and/or save external until data fits
	 *
	 *	1: Inline compress attributes with attstorage 'x'
	 *	2: Store attributes with attstorage 'x' or 'e' external
	 *  3: Inline compress attributes with attstorage 'm'
	 *	4: Store attributes with attstorage 'm' external
	 * ----------
	 */
	maxDataLen = offsetof(HeapTupleHeaderData, t_bits);
	if (has_nulls)
		maxDataLen += BITMAPLEN(numAttrs);
	maxDataLen = (MaxTupleSize / 4) - MAXALIGN(maxDataLen);

	/* ----------
	 * Look for attributes with attstorage 'x' to compress
	 * ----------
	 */
	while (MAXALIGN(ComputeDataSize(tupleDesc, toast_values, toast_nulls)) >
				maxDataLen)
	{
		int		biggest_attno = -1;
		int32	biggest_size  = MAXALIGN(sizeof(varattrib));
		Datum	old_value;

		/* ----------
		 * Search for the biggest yet uncompressed internal attribute
		 * ----------
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] == 'p')
				continue;
			if (VARATT_IS_EXTENDED(toast_values[i]))
				continue;
			if (att[i]->attstorage != 'x')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size  = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/* ----------
		 * Compress it inline
		 * ----------
		 */
		i					= biggest_attno;
		old_value			= toast_values[i];

		toast_values[i]		= toast_compress_datum(toast_values[i]);
									
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));

		toast_free[i]		= true;
		toast_sizes[i]		= VARATT_SIZE(toast_values[i]);

		need_change = true;
		need_free   = true;
	}

	/* ----------
	 * Second we look for attributes of attstorage 'x' or 'e' that
	 * are still inline.
	 * ----------
	 */
	while (MAXALIGN(ComputeDataSize(tupleDesc, toast_values, toast_nulls)) >
				maxDataLen && rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int		biggest_attno = -1;
		int32	biggest_size  = MAXALIGN(sizeof(varattrib));
		Datum	old_value;

		/* ----------
		 * Search for the biggest yet inlined attribute with
		 * attstorage = 'x' or 'e'
		 * ----------
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] == 'p')
				continue;
			if (VARATT_IS_EXTERNAL(toast_values[i]))
				continue;
			if (att[i]->attstorage != 'x' && att[i]->attstorage != 'e')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size  = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/* ----------
		 * Store this external
		 * ----------
		 */
		i					= biggest_attno;
		old_value			= toast_values[i];
		toast_action[i]		= 'p';
		toast_values[i]		= toast_save_datum(rel,
									newtup->t_data->t_oid,
									i + 1,
									toast_values[i]);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));

		toast_free[i]		= true;
		toast_sizes[i]		= VARATT_SIZE(toast_values[i]);

		need_change = true;
		need_free   = true;
	}

	/* ----------
	 * Round 3 - this time we take attributes with storage
	 * 'm' into compression
	 * ----------
	 */
	while (MAXALIGN(ComputeDataSize(tupleDesc, toast_values, toast_nulls)) >
				maxDataLen)
	{
		int		biggest_attno = -1;
		int32	biggest_size  = MAXALIGN(sizeof(varattrib));
		Datum	old_value;

		/* ----------
		 * Search for the biggest yet uncompressed internal attribute
		 * ----------
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] == 'p')
				continue;
			if (VARATT_IS_EXTENDED(toast_values[i]))
				continue;
			if (att[i]->attstorage != 'm')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size  = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/* ----------
		 * Compress it inline
		 * ----------
		 */
		i					= biggest_attno;
		old_value			= toast_values[i];

		toast_values[i]		= toast_compress_datum(toast_values[i]);
									
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));

		toast_free[i]		= true;
		toast_sizes[i]		= VARATT_SIZE(toast_values[i]);

		need_change = true;
		need_free   = true;
	}

	/* ----------
	 * Finally we store attributes of type 'm' external
	 * ----------
	 */
	while (MAXALIGN(ComputeDataSize(tupleDesc, toast_values, toast_nulls)) >
				maxDataLen && rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int		biggest_attno = -1;
		int32	biggest_size  = MAXALIGN(sizeof(varattrib));
		Datum	old_value;

		/* ----------
		 * Search for the biggest yet inlined attribute with
		 * attstorage = 'x' or 'e'
		 * ----------
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] == 'p')
				continue;
			if (VARATT_IS_EXTERNAL(toast_values[i]))
				continue;
			if (att[i]->attstorage != 'm')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size  = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/* ----------
		 * Store this external
		 * ----------
		 */
		i					= biggest_attno;
		old_value			= toast_values[i];
		toast_action[i]		= 'p';
		toast_values[i]		= toast_save_datum(rel,
									newtup->t_data->t_oid,
									i + 1,
									toast_values[i]);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));

		toast_free[i]		= true;
		toast_sizes[i]		= VARATT_SIZE(toast_values[i]);

		need_change = true;
		need_free   = true;
	}

	/* ----------
	 * In the case we toasted any values, we need to build
	 * a new heap tuple with the changed values.
	 * ----------
	 */
	if (need_change)
	{
		char		   *new_data;
		int32			new_len;
		MemoryContext	oldcxt;
		HeapTupleHeader	olddata;

		/* ----------
		 * Calculate the new size of the tuple
		 * ----------
		 */
		new_len = offsetof(HeapTupleHeaderData, t_bits);
		if (has_nulls)
			new_len += BITMAPLEN(numAttrs);
		new_len = MAXALIGN(new_len);
		new_len += ComputeDataSize(tupleDesc, toast_values, toast_nulls);

		/* ----------
		 * Remember the old memory location of the tuple (for below),
		 * switch to the memory context of the HeapTuple structure
		 * and allocate the new tuple.
		 * ----------
		 */
		olddata = newtup->t_data;
		oldcxt = MemoryContextSwitchTo(newtup->t_datamcxt);
		new_data = palloc(new_len);

		/* ----------
		 * Put the tuple header and the changed values into place
		 * ----------
		 */
		memcpy(new_data, newtup->t_data, newtup->t_data->t_hoff);
		newtup->t_data = (HeapTupleHeader)new_data;
		newtup->t_len = new_len;

		DataFill((char *)(MAXALIGN((long)new_data +
						offsetof(HeapTupleHeaderData, t_bits) + 
						((has_nulls) ? BITMAPLEN(numAttrs) : 0))),
				tupleDesc,
				toast_values,
				toast_nulls,
				&(newtup->t_data->t_infomask),
				HeapTupleNoNulls(newtup) ? NULL : new_data);

		/* ----------
		 * In the case we modified a previously modified tuple again,
		 * free the memory from the previous run
		 * ----------
		 */
		if ((char *)olddata != ((char *)newtup + HEAPTUPLESIZE))
			pfree(olddata);

		/* ----------
		 * Switch back to the old memory context
		 * ----------
		 */
		MemoryContextSwitchTo(oldcxt);
	}


	/* ----------
	 * Free allocated temp values
	 * ----------
	 */
	if (need_free)
		for (i = 0; i < numAttrs; i++)
			if (toast_free[i])
				pfree(DatumGetPointer(toast_values[i]));

	/* ----------
	 * Delete external values from the old tuple
	 * ----------
	 */
	if (need_delold)
		for (i = 0; i < numAttrs; i++)
			if (toast_delold[i])
				toast_delete_datum(rel,
					heap_getattr(oldtup, i + 1, tupleDesc, &old_isnull));

	return;
}


/* ----------
 * toast_compress_datum -
 *
 *	Create a compressed version of a datum
 * ----------
 */
static Datum
toast_compress_datum(Datum value)
{
	varattrib	   *tmp;

	tmp = (varattrib *)palloc(sizeof(PGLZ_Header) + VARATT_SIZE(value));
	pglz_compress(VARATT_DATA(value), VARATT_SIZE(value) - VARHDRSZ,
				(PGLZ_Header *)tmp,
				PGLZ_strategy_default);
	VARATT_SIZEP(tmp) |= VARATT_FLAG_COMPRESSED;

	return PointerGetDatum(tmp);
}


/* ----------
 * toast_save_datum -
 *
 *	Save one single datum into the secondary relation and return
 *	a varattrib reference for it.
 * ----------
 */
static Datum
toast_save_datum(Relation rel, Oid mainoid, int16 attno, Datum value)
{
	Relation			toastrel;
	Relation			toastidx;
	HeapTuple			toasttup;
	InsertIndexResult	idxres;
	TupleDesc			toasttupDesc;
	Datum				t_values[3];
	char				t_nulls[4];
	varattrib		   *result;
	char				chunk_data[MaxTupleSize];
	int32				chunk_size;
	int32				chunk_seq = 0;
	char			   *data_p;
	int32				data_todo;

	/* ----------
	 * Create the varattrib reference
	 * ----------
	 */
	result = (varattrib *)palloc(sizeof(varattrib));

	result->va_header	= sizeof(varattrib) | VARATT_FLAG_EXTERNAL;
	if (VARATT_IS_COMPRESSED(value))
	{
		result->va_header |= VARATT_FLAG_COMPRESSED;
		result->va_content.va_external.va_rawsize = 
					((varattrib *)value)->va_content.va_compressed.va_rawsize;
	}
	else
		result->va_content.va_external.va_rawsize = VARATT_SIZE(value);
					
	result->va_content.va_external.va_extsize		= 
					VARATT_SIZE(value) - VARHDRSZ;
	result->va_content.va_external.va_valueid		= newoid();
	result->va_content.va_external.va_toastrelid	= 
					rel->rd_rel->reltoastrelid;
	result->va_content.va_external.va_toastidxid	= 
					rel->rd_rel->reltoastidxid;
	result->va_content.va_external.va_rowid			= mainoid;
	result->va_content.va_external.va_attno			= attno;

	/* ----------
	 * Initialize constant parts of the tuple data
	 * ----------
	 */
	t_values[0] = ObjectIdGetDatum(result->va_content.va_external.va_valueid);
	t_values[2] = PointerGetDatum(chunk_data);
	t_nulls[0] = ' ';
	t_nulls[1] = ' ';
	t_nulls[2] = ' ';
	t_nulls[3] = '\0';

	/* ----------
	 * Get the data to process
	 * ----------
	 */
	data_p		= VARATT_DATA(value);
	data_todo	= VARATT_SIZE(value) - VARHDRSZ;

	/* ----------
	 * Open the toast relation
	 * ----------
	 */
	toastrel = heap_open(rel->rd_rel->reltoastrelid, RowExclusiveLock);
	if (toastrel == NULL)
		elog(ERROR, "Failed to open secondary relation of %s",
					nameout(&(rel->rd_rel->relname)));
	toasttupDesc = toastrel->rd_att;
	toastidx = index_open(rel->rd_rel->reltoastidxid);
	if (toastidx == NULL)
		elog(ERROR, "Failed to open index for secondary relation of %s",
					nameout(&(rel->rd_rel->relname)));
	
	/* ----------
	 * Split up the item into chunks 
	 * ----------
	 */
	while (data_todo > 0)
	{
		/* ----------
		 * Calculate the size of this chunk
		 * ----------
		 */
		chunk_size = (TOAST_MAX_CHUNK_SIZE < data_todo) ? 
						TOAST_MAX_CHUNK_SIZE : data_todo;

		/* ----------
		 * Build a tuple
		 * ----------
		 */
		t_values[1] = (Datum)(chunk_seq++);
		VARATT_SIZEP(chunk_data) = chunk_size + VARHDRSZ;
		memcpy(VARATT_DATA(chunk_data), data_p, chunk_size);
		toasttup = heap_formtuple(toasttupDesc, t_values, t_nulls);
		if (!HeapTupleIsValid(toasttup))
			elog(ERROR, "Failed to build TOAST tuple");

		/* ----------
		 * Store it and create the index entry
		 * ----------
		 */
		heap_insert(toastrel, toasttup);
		idxres = index_insert(toastidx, t_values, t_nulls,
						&(toasttup->t_self),
						toastrel);
		if (idxres == NULL)
			elog(ERROR, "Failed to insert index entry for TOAST tuple");

		/* ----------
		 * Free memory
		 * ----------
		 */
		heap_freetuple(toasttup);
		pfree(idxres);

		/* ----------
		 * Move on to next chunk
		 * ----------
		 */
		data_todo -= chunk_size;
		data_p += chunk_size;
	}

	/* ----------
	 * Done - close toast relation and return the reference
	 * ----------
	 */
	index_close(toastidx);
	heap_close(toastrel, RowExclusiveLock);

	return PointerGetDatum(result);
}


/* ----------
 * toast_delete_datum -
 *
 *	Delete a single external stored value.
 * ----------
 */
static void
toast_delete_datum(Relation rel, Datum value)
{
	register varattrib	   *attr = (varattrib *)value;
	Relation				toastrel;
	Relation				toastidx;
	ScanKeyData				toastkey;
	IndexScanDesc			toastscan;
	HeapTupleData			toasttup;
	RetrieveIndexResult		indexRes;
	Buffer					buffer;

	if (!VARATT_IS_EXTERNAL(attr))
		return;

	/* ----------
	 * Open the toast relation and it's index
	 * ----------
	 */
	toastrel	= heap_open(attr->va_content.va_external.va_toastrelid,
					RowExclusiveLock);
	if (toastrel == NULL)
		elog(ERROR, "Failed to open secondary relation at TOAST fetch");
	toastidx = index_open(attr->va_content.va_external.va_toastidxid);
	if (toastidx == NULL)
		elog(ERROR, "Failed to open index of secondary relation at TOAST fetch");

	/* ----------
	 * Setup a scan key to fetch from the index by va_valueid
	 * ----------
	 */
	ScanKeyEntryInitialize(&toastkey,
					(bits16) 0, 
					(AttrNumber) 1, 
					(RegProcedure) F_OIDEQ, 
					ObjectIdGetDatum(attr->va_content.va_external.va_valueid));

	/* ----------
	 * Read the chunks by index
	 * ----------
	 */
	toastscan = index_beginscan(toastidx, false, 1, &toastkey);
	while ((indexRes = index_getnext(toastscan, ForwardScanDirection)) != NULL)
	{
		toasttup.t_self = indexRes->heap_iptr;
		heap_fetch(toastrel, SnapshotAny, &toasttup, &buffer);
		pfree(indexRes);

		if (!toasttup.t_data)
			continue;

		/* ----------
		 * Have a chunk, delete it
		 * ----------
		 */
		heap_delete(toastrel, &toasttup.t_self, NULL);

		ReleaseBuffer(buffer);
	}

	/* ----------
	 * End scan and close relations
	 * ----------
	 */
	index_endscan(toastscan);
	index_close(toastidx);
	heap_close(toastrel, RowExclusiveLock);

	return;
}


/* ----------
 * toast_fetch_datum -
 *
 *	Reconstruct an in memory varattrib from the chunks saved
 *	in the toast relation
 * ----------
 */
static varattrib *
toast_fetch_datum(varattrib *attr)
{
	Relation				toastrel;
	Relation				toastidx;
	ScanKeyData				toastkey;
	IndexScanDesc			toastscan;
	HeapTupleData			toasttup;
	HeapTuple				ttup;
	TupleDesc				toasttupDesc;
	RetrieveIndexResult		indexRes;
	Buffer					buffer;

	varattrib			   *result;
	int32					ressize;
	int32					residx;
	int						numchunks;
	Datum					chunk;
	bool					isnull;

	ressize = attr->va_content.va_external.va_extsize;
    numchunks = (ressize / TOAST_MAX_CHUNK_SIZE) + 1;

	result = (varattrib *)palloc(ressize + VARHDRSZ);
	VARATT_SIZEP(result) = ressize + VARHDRSZ;
	if (VARATT_IS_COMPRESSED(attr))
		VARATT_SIZEP(result) |= VARATT_FLAG_COMPRESSED;

	/* ----------
	 * Open the toast relation and it's index
	 * ----------
	 */
	toastrel	= heap_open(attr->va_content.va_external.va_toastrelid,
					AccessShareLock);
	if (toastrel == NULL)
		elog(ERROR, "Failed to open secondary relation at TOAST fetch");
	toasttupDesc = toastrel->rd_att;
	toastidx = index_open(attr->va_content.va_external.va_toastidxid);
	if (toastidx == NULL)
		elog(ERROR, "Failed to open index of secondary relation at TOAST fetch");

	/* ----------
	 * Setup a scan key to fetch from the index by va_valueid
	 * ----------
	 */
	ScanKeyEntryInitialize(&toastkey,
					(bits16) 0, 
					(AttrNumber) 1, 
					(RegProcedure) F_OIDEQ, 
					ObjectIdGetDatum(attr->va_content.va_external.va_valueid));

	/* ----------
	 * Read the chunks by index
	 * ----------
	 */
	toastscan = index_beginscan(toastidx, false, 1, &toastkey);
	while ((indexRes = index_getnext(toastscan, ForwardScanDirection)) != NULL)
	{
		toasttup.t_self = indexRes->heap_iptr;
		heap_fetch(toastrel, SnapshotAny, &toasttup, &buffer);
		pfree(indexRes);

		if (!toasttup.t_data)
			continue;
		ttup = &toasttup;

		/* ----------
		 * Have a chunk, extract the sequence number and the data
		 * ----------
		 */
		residx = (int32)heap_getattr(ttup, 2, toasttupDesc, &isnull);
		chunk = heap_getattr(ttup, 3, toasttupDesc, &isnull);

		/* ----------
		 * Copy the data into our result
		 * ----------
		 */
		memcpy(((char *)VARATT_DATA(result)) + residx * TOAST_MAX_CHUNK_SIZE,
					VARATT_DATA(chunk),
					VARATT_SIZE(chunk) - VARHDRSZ);

		ReleaseBuffer(buffer);
	}

	/* ----------
	 * End scan and close relations
	 * ----------
	 */
	index_endscan(toastscan);
	index_close(toastidx);
	heap_close(toastrel, AccessShareLock);

	return result;
}


#endif	 /* TUPLE_TOASTER_ACTIVE */
