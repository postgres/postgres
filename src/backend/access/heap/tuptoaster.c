/*-------------------------------------------------------------------------
 *
 * tuptoaster.c
 *	  Support routines for external and compressed storage of
 *	  variable size attributes.
 *
 * Copyright (c) 2000-2005, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/heap/tuptoaster.c,v 1.53.2.3 2006/01/17 17:33:20 tgl Exp $
 *
 *
 * INTERFACE ROUTINES
 *		toast_insert_or_update -
 *			Try to make a given tuple fit into one page by compressing
 *			or moving off attributes
 *
 *		toast_delete -
 *			Reclaim toast storage when a tuple is deleted
 *
 *		heap_tuple_untoast_attr -
 *			Fetch back a given value from the "secondary" relation
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>

#include "access/heapam.h"
#include "access/genam.h"
#include "access/tuptoaster.h"
#include "catalog/catalog.h"
#include "utils/rel.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_lzcompress.h"
#include "utils/typcache.h"


#undef TOAST_DEBUG

static void toast_delete_datum(Relation rel, Datum value);
static Datum toast_save_datum(Relation rel, Datum value);
static varattrib *toast_fetch_datum(varattrib *attr);
static varattrib *toast_fetch_datum_slice(varattrib *attr,
						int32 sliceoffset, int32 length);


/* ----------
 * heap_tuple_fetch_attr -
 *
 *	Public entry point to get back a toasted value
 *	external storage (possibly still in compressed format).
 * ----------
 */
varattrib *
heap_tuple_fetch_attr(varattrib *attr)
{
	varattrib  *result;

	if (VARATT_IS_EXTERNAL(attr))
	{
		/*
		 * This is an external stored plain value
		 */
		result = toast_fetch_datum(attr);
	}
	else
	{
		/*
		 * This is a plain value inside of the main tuple - why am I called?
		 */
		result = attr;
	}

	return result;
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
	varattrib  *result;

	if (VARATT_IS_EXTERNAL(attr))
	{
		if (VARATT_IS_COMPRESSED(attr))
		{
			/* ----------
			 * This is an external stored compressed value
			 * Fetch it from the toast heap and decompress.
			 * ----------
			 */
			varattrib  *tmp;

			tmp = toast_fetch_datum(attr);
			result = (varattrib *) palloc(attr->va_content.va_external.va_rawsize
										  + VARHDRSZ);
			VARATT_SIZEP(result) = attr->va_content.va_external.va_rawsize
				+ VARHDRSZ;
			pglz_decompress((PGLZ_Header *) tmp, VARATT_DATA(result));

			pfree(tmp);
		}
		else
		{
			/*
			 * This is an external stored plain value
			 */
			result = toast_fetch_datum(attr);
		}
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/*
		 * This is a compressed value inside of the main tuple
		 */
		result = (varattrib *) palloc(attr->va_content.va_compressed.va_rawsize
									  + VARHDRSZ);
		VARATT_SIZEP(result) = attr->va_content.va_compressed.va_rawsize
			+ VARHDRSZ;
		pglz_decompress((PGLZ_Header *) attr, VARATT_DATA(result));
	}
	else

		/*
		 * This is a plain value inside of the main tuple - why am I called?
		 */
		return attr;

	return result;
}


/* ----------
 * heap_tuple_untoast_attr_slice -
 *
 *		Public entry point to get back part of a toasted value
 *		from compression or external storage.
 * ----------
 */
varattrib *
heap_tuple_untoast_attr_slice(varattrib *attr, int32 sliceoffset, int32 slicelength)
{
	varattrib  *preslice;
	varattrib  *result;
	int32		attrsize;

	if (VARATT_IS_COMPRESSED(attr))
	{
		varattrib  *tmp;

		if (VARATT_IS_EXTERNAL(attr))
			tmp = toast_fetch_datum(attr);
		else
		{
			tmp = attr;			/* compressed in main tuple */
		}

		preslice = (varattrib *) palloc(attr->va_content.va_external.va_rawsize
										+ VARHDRSZ);
		VARATT_SIZEP(preslice) = attr->va_content.va_external.va_rawsize + VARHDRSZ;
		pglz_decompress((PGLZ_Header *) tmp, VARATT_DATA(preslice));

		if (tmp != attr)
			pfree(tmp);
	}
	else
	{
		/* Plain value */
		if (VARATT_IS_EXTERNAL(attr))
		{
			/* fast path */
			return (toast_fetch_datum_slice(attr, sliceoffset, slicelength));
		}
		else
			preslice = attr;
	}

	/* slicing of datum for compressed cases and plain value */

	attrsize = VARSIZE(preslice) - VARHDRSZ;
	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		slicelength = 0;
	}

	if (((sliceoffset + slicelength) > attrsize) || slicelength < 0)
		slicelength = attrsize - sliceoffset;

	result = (varattrib *) palloc(slicelength + VARHDRSZ);
	VARATT_SIZEP(result) = slicelength + VARHDRSZ;

	memcpy(VARDATA(result), VARDATA(preslice) + sliceoffset, slicelength);

	if (preslice != attr)
		pfree(preslice);

	return result;
}


/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 * ----------
 */
Size
toast_raw_datum_size(Datum value)
{
	varattrib  *attr = (varattrib *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_COMPRESSED(attr))
	{
		/*
		 * va_rawsize shows the original data size, whether the datum is
		 * external or not.
		 */
		result = attr->va_content.va_compressed.va_rawsize + VARHDRSZ;
	}
	else if (VARATT_IS_EXTERNAL(attr))
	{
		/*
		 * an uncompressed external attribute has rawsize including the header
		 * (not too consistent!)
		 */
		result = attr->va_content.va_external.va_rawsize;
	}
	else
	{
		/* plain untoasted datum */
		result = VARSIZE(attr);
	}
	return result;
}

/* ----------
 * toast_datum_size
 *
 *	Return the physical storage size (possibly compressed) of a varlena datum
 * ----------
 */
Size
toast_datum_size(Datum value)
{
	varattrib  *attr = (varattrib *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL(attr))
	{
		/*
		 * Attribute is stored externally - return the extsize whether
		 * compressed or not.  We do not count the size of the toast pointer
		 * ... should we?
		 */
		result = attr->va_content.va_external.va_extsize;
	}
	else
	{
		/*
		 * Attribute is stored inline either compressed or not, just calculate
		 * the size of the datum in either case.
		 */
		result = VARSIZE(attr);
	}
	return result;
}


/* ----------
 * toast_delete -
 *
 *	Cascaded delete toast-entries on DELETE
 * ----------
 */
void
toast_delete(Relation rel, HeapTuple oldtup)
{
	TupleDesc	tupleDesc;
	Form_pg_attribute *att;
	int			numAttrs;
	int			i;
	Datum		toast_values[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];

	/*
	 * Get the tuple descriptor and break down the tuple into fields.
	 *
	 * NOTE: it's debatable whether to use heap_deformtuple() here or just
	 * heap_getattr() only the varlena columns.  The latter could win if there
	 * are few varlena columns and many non-varlena ones. However,
	 * heap_deformtuple costs only O(N) while the heap_getattr way would cost
	 * O(N^2) if there are many varlena columns, so it seems better to err on
	 * the side of linear cost.  (We won't even be here unless there's at
	 * least one varlena column, by the way.)
	 */
	tupleDesc = rel->rd_att;
	att = tupleDesc->attrs;
	numAttrs = tupleDesc->natts;

	Assert(numAttrs <= MaxHeapAttributeNumber);
	heap_deform_tuple(oldtup, tupleDesc, toast_values, toast_isnull);

	/*
	 * Check for external stored attributes and delete them from the secondary
	 * relation.
	 */
	for (i = 0; i < numAttrs; i++)
	{
		if (att[i]->attlen == -1)
		{
			Datum		value = toast_values[i];

			if (!toast_isnull[i] && VARATT_IS_EXTERNAL(value))
				toast_delete_datum(rel, value);
		}
	}
}


/* ----------
 * toast_insert_or_update -
 *
 *	Delete no-longer-used toast-entries and create new ones to
 *	make the new tuple fit on INSERT or UPDATE
 *
 * Inputs:
 *	newtup: the candidate new tuple to be inserted
 *	oldtup: the old row version for UPDATE, or NULL for INSERT
 * Result:
 *	either newtup if no toasting is needed, or a palloc'd modified tuple
 *	that is what should actually get stored
 *
 * NOTE: neither newtup nor oldtup will be modified.  This is a change
 * from the pre-8.1 API of this routine.
 * ----------
 */
HeapTuple
toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup)
{
	HeapTuple	result_tuple;
	TupleDesc	tupleDesc;
	Form_pg_attribute *att;
	int			numAttrs;
	int			i;

	bool		need_change = false;
	bool		need_free = false;
	bool		need_delold = false;
	bool		has_nulls = false;

	Size		maxDataLen;

	char		toast_action[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];
	bool		toast_oldisnull[MaxHeapAttributeNumber];
	Datum		toast_values[MaxHeapAttributeNumber];
	Datum		toast_oldvalues[MaxHeapAttributeNumber];
	int32		toast_sizes[MaxHeapAttributeNumber];
	bool		toast_free[MaxHeapAttributeNumber];
	bool		toast_delold[MaxHeapAttributeNumber];

	/*
	 * Get the tuple descriptor and break down the tuple(s) into fields.
	 */
	tupleDesc = rel->rd_att;
	att = tupleDesc->attrs;
	numAttrs = tupleDesc->natts;

	Assert(numAttrs <= MaxHeapAttributeNumber);
	heap_deform_tuple(newtup, tupleDesc, toast_values, toast_isnull);
	if (oldtup != NULL)
		heap_deform_tuple(oldtup, tupleDesc, toast_oldvalues, toast_oldisnull);

	/* ----------
	 * Then collect information about the values given
	 *
	 * NOTE: toast_action[i] can have these values:
	 *		' '		default handling
	 *		'p'		already processed --- don't touch it
	 *		'x'		incompressible, but OK to move off
	 *
	 * NOTE: toast_sizes[i] is only made valid for varlena attributes with
	 *		toast_action[i] different from 'p'.
	 * ----------
	 */
	memset(toast_action, ' ', numAttrs * sizeof(char));
	memset(toast_free, 0, numAttrs * sizeof(bool));
	memset(toast_delold, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		varattrib  *old_value;
		varattrib  *new_value;

		if (oldtup != NULL)
		{
			/*
			 * For UPDATE get the old and new values of this attribute
			 */
			old_value = (varattrib *) DatumGetPointer(toast_oldvalues[i]);
			new_value = (varattrib *) DatumGetPointer(toast_values[i]);

			/*
			 * If the old value is an external stored one, check if it has
			 * changed so we have to delete it later.
			 */
			if (att[i]->attlen == -1 && !toast_oldisnull[i] &&
				VARATT_IS_EXTERNAL(old_value))
			{
				if (toast_isnull[i] || !VARATT_IS_EXTERNAL(new_value) ||
					old_value->va_content.va_external.va_valueid !=
					new_value->va_content.va_external.va_valueid ||
					old_value->va_content.va_external.va_toastrelid !=
					new_value->va_content.va_external.va_toastrelid)
				{
					/*
					 * The old external stored value isn't needed any more
					 * after the update
					 */
					toast_delold[i] = true;
					need_delold = true;
				}
				else
				{
					/*
					 * This attribute isn't changed by this update so we reuse
					 * the original reference to the old value in the new
					 * tuple.
					 */
					toast_action[i] = 'p';
					toast_sizes[i] = VARATT_SIZE(toast_values[i]);
					continue;
				}
			}
		}
		else
		{
			/*
			 * For INSERT simply get the new value
			 */
			new_value = (varattrib *) DatumGetPointer(toast_values[i]);
		}

		/*
		 * Handle NULL attributes
		 */
		if (toast_isnull[i])
		{
			toast_action[i] = 'p';
			has_nulls = true;
			continue;
		}

		/*
		 * Now look at varlena attributes
		 */
		if (att[i]->attlen == -1)
		{
			/*
			 * If the table's attribute says PLAIN always, force it so.
			 */
			if (att[i]->attstorage == 'p')
				toast_action[i] = 'p';

			/*
			 * We took care of UPDATE above, so any external value we find
			 * still in the tuple must be someone else's we cannot reuse.
			 * Expand it to plain (and, probably, toast it again below).
			 */
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = heap_tuple_untoast_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
				need_change = true;
				need_free = true;
			}

			/*
			 * Remember the size of this attribute
			 */
			toast_sizes[i] = VARATT_SIZE(new_value);
		}
		else
		{
			/*
			 * Not a varlena attribute, plain storage always
			 */
			toast_action[i] = 'p';
		}
	}

	/* ----------
	 * Compress and/or save external until data fits into target length
	 *
	 *	1: Inline compress attributes with attstorage 'x'
	 *	2: Store attributes with attstorage 'x' or 'e' external
	 *	3: Inline compress attributes with attstorage 'm'
	 *	4: Store attributes with attstorage 'm' external
	 * ----------
	 */
	maxDataLen = offsetof(HeapTupleHeaderData, t_bits);
	if (has_nulls)
		maxDataLen += BITMAPLEN(numAttrs);
	maxDataLen = TOAST_TUPLE_TARGET - MAXALIGN(maxDataLen);

	/*
	 * Look for attributes with attstorage 'x' to compress
	 */
	while (MAXALIGN(heap_compute_data_size(tupleDesc,
										   toast_values, toast_isnull)) >
		   maxDataLen)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(sizeof(varattrib));
		Datum		old_value;
		Datum		new_value;

		/*
		 * Search for the biggest yet uncompressed internal attribute
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] != ' ')
				continue;
			if (VARATT_IS_EXTENDED(toast_values[i]))
				continue;
			if (att[i]->attstorage != 'x')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
		 * Attempt to compress it inline
		 */
		i = biggest_attno;
		old_value = toast_values[i];
		new_value = toast_compress_datum(old_value);

		if (DatumGetPointer(new_value) != NULL)
		{
			/* successful compression */
			if (toast_free[i])
				pfree(DatumGetPointer(old_value));
			toast_values[i] = new_value;
			toast_free[i] = true;
			toast_sizes[i] = VARATT_SIZE(toast_values[i]);
			need_change = true;
			need_free = true;
		}
		else
		{
			/*
			 * incompressible data, ignore on subsequent compression passes
			 */
			toast_action[i] = 'x';
		}
	}

	/*
	 * Second we look for attributes of attstorage 'x' or 'e' that are still
	 * inline.
	 */
	while (MAXALIGN(heap_compute_data_size(tupleDesc,
										   toast_values, toast_isnull)) >
		   maxDataLen && rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(sizeof(varattrib));
		Datum		old_value;

		/*------
		 * Search for the biggest yet inlined attribute with
		 * attstorage equals 'x' or 'e'
		 *------
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
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
		 * Store this external
		 */
		i = biggest_attno;
		old_value = toast_values[i];
		toast_action[i] = 'p';
		toast_values[i] = toast_save_datum(rel, toast_values[i]);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));

		toast_free[i] = true;
		toast_sizes[i] = VARATT_SIZE(toast_values[i]);

		need_change = true;
		need_free = true;
	}

	/*
	 * Round 3 - this time we take attributes with storage 'm' into
	 * compression
	 */
	while (MAXALIGN(heap_compute_data_size(tupleDesc,
										   toast_values, toast_isnull)) >
		   maxDataLen)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(sizeof(varattrib));
		Datum		old_value;
		Datum		new_value;

		/*
		 * Search for the biggest yet uncompressed internal attribute
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] != ' ')
				continue;
			if (VARATT_IS_EXTENDED(toast_values[i]))
				continue;
			if (att[i]->attstorage != 'm')
				continue;
			if (toast_sizes[i] > biggest_size)
			{
				biggest_attno = i;
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
		 * Attempt to compress it inline
		 */
		i = biggest_attno;
		old_value = toast_values[i];
		new_value = toast_compress_datum(old_value);

		if (DatumGetPointer(new_value) != NULL)
		{
			/* successful compression */
			if (toast_free[i])
				pfree(DatumGetPointer(old_value));
			toast_values[i] = new_value;
			toast_free[i] = true;
			toast_sizes[i] = VARATT_SIZE(toast_values[i]);
			need_change = true;
			need_free = true;
		}
		else
		{
			/*
			 * incompressible data, ignore on subsequent compression passes
			 */
			toast_action[i] = 'x';
		}
	}

	/*
	 * Finally we store attributes of type 'm' external
	 */
	while (MAXALIGN(heap_compute_data_size(tupleDesc,
										   toast_values, toast_isnull)) >
		   maxDataLen && rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(sizeof(varattrib));
		Datum		old_value;

		/*--------
		 * Search for the biggest yet inlined attribute with
		 * attstorage = 'm'
		 *--------
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
				biggest_size = toast_sizes[i];
			}
		}

		if (biggest_attno < 0)
			break;

		/*
		 * Store this external
		 */
		i = biggest_attno;
		old_value = toast_values[i];
		toast_action[i] = 'p';
		toast_values[i] = toast_save_datum(rel, toast_values[i]);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));

		toast_free[i] = true;
		toast_sizes[i] = VARATT_SIZE(toast_values[i]);

		need_change = true;
		need_free = true;
	}

	/*
	 * In the case we toasted any values, we need to build a new heap tuple
	 * with the changed values.
	 */
	if (need_change)
	{
		HeapTupleHeader olddata = newtup->t_data;
		HeapTupleHeader new_data;
		int32		new_len;

		/*
		 * Calculate the new size of the tuple.  Header size should not
		 * change, but data size might.
		 */
		new_len = offsetof(HeapTupleHeaderData, t_bits);
		if (has_nulls)
			new_len += BITMAPLEN(numAttrs);
		if (olddata->t_infomask & HEAP_HASOID)
			new_len += sizeof(Oid);
		new_len = MAXALIGN(new_len);
		Assert(new_len == olddata->t_hoff);
		new_len += heap_compute_data_size(tupleDesc,
										  toast_values, toast_isnull);

		/*
		 * Allocate and zero the space needed, and fill HeapTupleData fields.
		 */
		result_tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + new_len);
		result_tuple->t_len = new_len;
		result_tuple->t_self = newtup->t_self;
		result_tuple->t_tableOid = newtup->t_tableOid;
		result_tuple->t_datamcxt = CurrentMemoryContext;
		new_data = (HeapTupleHeader) ((char *) result_tuple + HEAPTUPLESIZE);
		result_tuple->t_data = new_data;

		/*
		 * Put the existing tuple header and the changed values into place
		 */
		memcpy(new_data, olddata, olddata->t_hoff);

		heap_fill_tuple(tupleDesc,
						toast_values,
						toast_isnull,
						(char *) new_data + olddata->t_hoff,
						&(new_data->t_infomask),
						has_nulls ? new_data->t_bits : NULL);
	}
	else
		result_tuple = newtup;

	/*
	 * Free allocated temp values
	 */
	if (need_free)
		for (i = 0; i < numAttrs; i++)
			if (toast_free[i])
				pfree(DatumGetPointer(toast_values[i]));

	/*
	 * Delete external values from the old tuple
	 */
	if (need_delold)
		for (i = 0; i < numAttrs; i++)
			if (toast_delold[i])
				toast_delete_datum(rel, toast_oldvalues[i]);

	return result_tuple;
}


/* ----------
 * toast_flatten_tuple_attribute -
 *
 *	If a Datum is of composite type, "flatten" it to contain no toasted fields.
 *	This must be invoked on any potentially-composite field that is to be
 *	inserted into a tuple.	Doing this preserves the invariant that toasting
 *	goes only one level deep in a tuple.
 * ----------
 */
Datum
toast_flatten_tuple_attribute(Datum value,
							  Oid typeId, int32 typeMod)
{
	TupleDesc	tupleDesc;
	HeapTupleHeader olddata;
	HeapTupleHeader new_data;
	int32		new_len;
	HeapTupleData tmptup;
	Form_pg_attribute *att;
	int			numAttrs;
	int			i;
	bool		need_change = false;
	bool		has_nulls = false;
	Datum		toast_values[MaxTupleAttributeNumber];
	bool		toast_isnull[MaxTupleAttributeNumber];
	bool		toast_free[MaxTupleAttributeNumber];

	/*
	 * See if it's a composite type, and get the tupdesc if so.
	 */
	tupleDesc = lookup_rowtype_tupdesc_noerror(typeId, typeMod, true);
	if (tupleDesc == NULL)
		return value;			/* not a composite type */

	tupleDesc = CreateTupleDescCopy(tupleDesc);
	att = tupleDesc->attrs;
	numAttrs = tupleDesc->natts;

	/*
	 * Break down the tuple into fields.
	 */
	olddata = DatumGetHeapTupleHeader(value);
	Assert(typeId == HeapTupleHeaderGetTypeId(olddata));
	Assert(typeMod == HeapTupleHeaderGetTypMod(olddata));
	/* Build a temporary HeapTuple control structure */
	tmptup.t_len = HeapTupleHeaderGetDatumLength(olddata);
	ItemPointerSetInvalid(&(tmptup.t_self));
	tmptup.t_tableOid = InvalidOid;
	tmptup.t_data = olddata;

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
		else if (att[i]->attlen == -1)
		{
			varattrib  *new_value;

			new_value = (varattrib *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTENDED(new_value))
			{
				new_value = heap_tuple_untoast_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
				need_change = true;
			}
		}
	}

	/*
	 * If nothing to untoast, just return the original tuple.
	 */
	if (!need_change)
	{
		FreeTupleDesc(tupleDesc);
		return value;
	}

	/*
	 * Calculate the new size of the tuple.  Header size should not change,
	 * but data size might.
	 */
	new_len = offsetof(HeapTupleHeaderData, t_bits);
	if (has_nulls)
		new_len += BITMAPLEN(numAttrs);
	if (olddata->t_infomask & HEAP_HASOID)
		new_len += sizeof(Oid);
	new_len = MAXALIGN(new_len);
	Assert(new_len == olddata->t_hoff);
	new_len += heap_compute_data_size(tupleDesc, toast_values, toast_isnull);

	new_data = (HeapTupleHeader) palloc0(new_len);

	/*
	 * Put the tuple header and the changed values into place
	 */
	memcpy(new_data, olddata, olddata->t_hoff);

	HeapTupleHeaderSetDatumLength(new_data, new_len);

	heap_fill_tuple(tupleDesc,
					toast_values,
					toast_isnull,
					(char *) new_data + olddata->t_hoff,
					&(new_data->t_infomask),
					has_nulls ? new_data->t_bits : NULL);

	/*
	 * Free allocated temp values
	 */
	for (i = 0; i < numAttrs; i++)
		if (toast_free[i])
			pfree(DatumGetPointer(toast_values[i]));
	FreeTupleDesc(tupleDesc);

	return PointerGetDatum(new_data);
}


/* ----------
 * toast_compress_datum -
 *
 *	Create a compressed version of a varlena datum
 *
 *	If we fail (ie, compressed result is actually bigger than original)
 *	then return NULL.  We must not use compressed data if it'd expand
 *	the tuple!
 * ----------
 */
Datum
toast_compress_datum(Datum value)
{
	varattrib  *tmp;

	tmp = (varattrib *) palloc(sizeof(PGLZ_Header) + VARATT_SIZE(value));
	pglz_compress(VARATT_DATA(value), VARATT_SIZE(value) - VARHDRSZ,
				  (PGLZ_Header *) tmp,
				  PGLZ_strategy_default);
	if (VARATT_SIZE(tmp) < VARATT_SIZE(value))
	{
		/* successful compression */
		VARATT_SIZEP(tmp) |= VARATT_FLAG_COMPRESSED;
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
 * toast_save_datum -
 *
 *	Save one single datum into the secondary relation and return
 *	a varattrib reference for it.
 * ----------
 */
static Datum
toast_save_datum(Relation rel, Datum value)
{
	Relation	toastrel;
	Relation	toastidx;
	HeapTuple	toasttup;
	TupleDesc	toasttupDesc;
	Datum		t_values[3];
	bool		t_isnull[3];
	varattrib  *result;
	struct
	{
		struct varlena hdr;
		char		data[TOAST_MAX_CHUNK_SIZE];
	}			chunk_data;
	int32		chunk_size;
	int32		chunk_seq = 0;
	char	   *data_p;
	int32		data_todo;

	/*
	 * Open the toast relation and its index.  We can use the index to check
	 * uniqueness of the OID we assign to the toasted item, even though it has
	 * additional columns besides OID.
	 */
	toastrel = heap_open(rel->rd_rel->reltoastrelid, RowExclusiveLock);
	toasttupDesc = toastrel->rd_att;
	toastidx = index_open(toastrel->rd_rel->reltoastidxid);

	/*
	 * Create the varattrib reference
	 */
	result = (varattrib *) palloc(sizeof(varattrib));

	result->va_header = sizeof(varattrib) | VARATT_FLAG_EXTERNAL;
	if (VARATT_IS_COMPRESSED(value))
	{
		result->va_header |= VARATT_FLAG_COMPRESSED;
		result->va_content.va_external.va_rawsize =
			((varattrib *) value)->va_content.va_compressed.va_rawsize;
	}
	else
		result->va_content.va_external.va_rawsize = VARATT_SIZE(value);

	result->va_content.va_external.va_extsize =
		VARATT_SIZE(value) - VARHDRSZ;
	result->va_content.va_external.va_valueid =
		GetNewOidWithIndex(toastrel, toastidx);
	result->va_content.va_external.va_toastrelid =
		rel->rd_rel->reltoastrelid;

	/*
	 * Initialize constant parts of the tuple data
	 */
	t_values[0] = ObjectIdGetDatum(result->va_content.va_external.va_valueid);
	t_values[2] = PointerGetDatum(&chunk_data);
	t_isnull[0] = false;
	t_isnull[1] = false;
	t_isnull[2] = false;

	/*
	 * Get the data to process
	 */
	data_p = VARATT_DATA(value);
	data_todo = VARATT_SIZE(value) - VARHDRSZ;

	/*
	 * We must explicitly lock the toast index because we aren't using an
	 * index scan here.
	 */
	LockRelation(toastidx, RowExclusiveLock);

	/*
	 * Split up the item into chunks
	 */
	while (data_todo > 0)
	{
		/*
		 * Calculate the size of this chunk
		 */
		chunk_size = Min(TOAST_MAX_CHUNK_SIZE, data_todo);

		/*
		 * Build a tuple and store it
		 */
		t_values[1] = Int32GetDatum(chunk_seq++);
		VARATT_SIZEP(&chunk_data) = chunk_size + VARHDRSZ;
		memcpy(VARATT_DATA(&chunk_data), data_p, chunk_size);
		toasttup = heap_form_tuple(toasttupDesc, t_values, t_isnull);
		if (!HeapTupleIsValid(toasttup))
			elog(ERROR, "failed to build TOAST tuple");

		simple_heap_insert(toastrel, toasttup);

		/*
		 * Create the index entry.	We cheat a little here by not using
		 * FormIndexDatum: this relies on the knowledge that the index columns
		 * are the same as the initial columns of the table.
		 *
		 * Note also that there had better not be any user-created index on
		 * the TOAST table, since we don't bother to update anything else.
		 */
		index_insert(toastidx, t_values, t_isnull,
					 &(toasttup->t_self),
					 toastrel, toastidx->rd_index->indisunique);

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
	 * Done - close toast relation and return the reference
	 */
	UnlockRelation(toastidx, RowExclusiveLock);
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
	varattrib  *attr = (varattrib *) DatumGetPointer(value);
	Relation	toastrel;
	Relation	toastidx;
	ScanKeyData toastkey;
	IndexScanDesc toastscan;
	HeapTuple	toasttup;

	if (!VARATT_IS_EXTERNAL(attr))
		return;

	/*
	 * Open the toast relation and it's index
	 */
	toastrel = heap_open(attr->va_content.va_external.va_toastrelid,
						 RowExclusiveLock);
	toastidx = index_open(toastrel->rd_rel->reltoastidxid);

	/*
	 * Setup a scan key to fetch from the index by va_valueid (we don't
	 * particularly care whether we see them in sequence or not)
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attr->va_content.va_external.va_valueid));

	/*
	 * Find the chunks by index
	 */
	toastscan = index_beginscan(toastrel, toastidx, SnapshotToast,
								1, &toastkey);
	while ((toasttup = index_getnext(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, delete it
		 */
		simple_heap_delete(toastrel, &toasttup->t_self);
	}

	/*
	 * End scan and close relations
	 */
	index_endscan(toastscan);
	index_close(toastidx);
	heap_close(toastrel, RowExclusiveLock);
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
	Relation	toastrel;
	Relation	toastidx;
	ScanKeyData toastkey;
	IndexScanDesc toastscan;
	HeapTuple	ttup;
	TupleDesc	toasttupDesc;
	varattrib  *result;
	int32		ressize;
	int32		residx,
				nextidx;
	int32		numchunks;
	Pointer		chunk;
	bool		isnull;
	int32		chunksize;

	ressize = attr->va_content.va_external.va_extsize;
	numchunks = ((ressize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;

	result = (varattrib *) palloc(ressize + VARHDRSZ);
	VARATT_SIZEP(result) = ressize + VARHDRSZ;
	if (VARATT_IS_COMPRESSED(attr))
		VARATT_SIZEP(result) |= VARATT_FLAG_COMPRESSED;

	/*
	 * Open the toast relation and its index
	 */
	toastrel = heap_open(attr->va_content.va_external.va_toastrelid,
						 AccessShareLock);
	toasttupDesc = toastrel->rd_att;
	toastidx = index_open(toastrel->rd_rel->reltoastidxid);

	/*
	 * Setup a scan key to fetch from the index by va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attr->va_content.va_external.va_valueid));

	/*
	 * Read the chunks by index
	 *
	 * Note that because the index is actually on (valueid, chunkidx) we will
	 * see the chunks in chunkidx order, even though we didn't explicitly ask
	 * for it.
	 */
	nextidx = 0;

	toastscan = index_beginscan(toastrel, toastidx, SnapshotToast,
								1, &toastkey);
	while ((ttup = index_getnext(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		residx = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		chunksize = VARATT_SIZE(chunk) - VARHDRSZ;

		/*
		 * Some checks on the data we've found
		 */
		if (residx != nextidx)
			elog(ERROR, "unexpected chunk number %d (expected %d) for toast value %u",
				 residx, nextidx,
				 attr->va_content.va_external.va_valueid);
		if (residx < numchunks - 1)
		{
			if (chunksize != TOAST_MAX_CHUNK_SIZE)
				elog(ERROR, "unexpected chunk size %d in chunk %d for toast value %u",
					 chunksize, residx,
					 attr->va_content.va_external.va_valueid);
		}
		else if (residx < numchunks)
		{
			if ((residx * TOAST_MAX_CHUNK_SIZE + chunksize) != ressize)
				elog(ERROR, "unexpected chunk size %d in chunk %d for toast value %u",
					 chunksize, residx,
					 attr->va_content.va_external.va_valueid);
		}
		else
			elog(ERROR, "unexpected chunk number %d for toast value %u",
				 residx,
				 attr->va_content.va_external.va_valueid);

		/*
		 * Copy the data into proper place in our result
		 */
		memcpy(((char *) VARATT_DATA(result)) + residx * TOAST_MAX_CHUNK_SIZE,
			   VARATT_DATA(chunk),
			   chunksize);

		nextidx++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (nextidx != numchunks)
		elog(ERROR, "missing chunk number %d for toast value %u",
			 nextidx,
			 attr->va_content.va_external.va_valueid);

	/*
	 * End scan and close relations
	 */
	index_endscan(toastscan);
	index_close(toastidx);
	heap_close(toastrel, AccessShareLock);

	return result;
}

/* ----------
 * toast_fetch_datum_slice -
 *
 *	Reconstruct a segment of a varattrib from the chunks saved
 *	in the toast relation
 * ----------
 */
static varattrib *
toast_fetch_datum_slice(varattrib *attr, int32 sliceoffset, int32 length)
{
	Relation	toastrel;
	Relation	toastidx;
	ScanKeyData toastkey[3];
	int			nscankeys;
	IndexScanDesc toastscan;
	HeapTuple	ttup;
	TupleDesc	toasttupDesc;
	varattrib  *result;
	int32		attrsize;
	int32		residx;
	int32		nextidx;
	int			numchunks;
	int			startchunk;
	int			endchunk;
	int32		startoffset;
	int32		endoffset;
	int			totalchunks;
	Pointer		chunk;
	bool		isnull;
	int32		chunksize;
	int32		chcpystrt;
	int32		chcpyend;

	attrsize = attr->va_content.va_external.va_extsize;
	totalchunks = ((attrsize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		length = 0;
	}

	if (((sliceoffset + length) > attrsize) || length < 0)
		length = attrsize - sliceoffset;

	result = (varattrib *) palloc(length + VARHDRSZ);
	VARATT_SIZEP(result) = length + VARHDRSZ;

	if (VARATT_IS_COMPRESSED(attr))
		VARATT_SIZEP(result) |= VARATT_FLAG_COMPRESSED;

	if (length == 0)
		return (result);		/* Can save a lot of work at this point! */

	startchunk = sliceoffset / TOAST_MAX_CHUNK_SIZE;
	endchunk = (sliceoffset + length - 1) / TOAST_MAX_CHUNK_SIZE;
	numchunks = (endchunk - startchunk) + 1;

	startoffset = sliceoffset % TOAST_MAX_CHUNK_SIZE;
	endoffset = (sliceoffset + length - 1) % TOAST_MAX_CHUNK_SIZE;

	/*
	 * Open the toast relation and it's index
	 */
	toastrel = heap_open(attr->va_content.va_external.va_toastrelid,
						 AccessShareLock);
	toasttupDesc = toastrel->rd_att;
	toastidx = index_open(toastrel->rd_rel->reltoastidxid);

	/*
	 * Setup a scan key to fetch from the index. This is either two keys or
	 * three depending on the number of chunks.
	 */
	ScanKeyInit(&toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(attr->va_content.va_external.va_valueid));

	/*
	 * Use equality condition for one chunk, a range condition otherwise:
	 */
	if (numchunks == 1)
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

	/*
	 * Read the chunks by index
	 *
	 * The index is on (valueid, chunkidx) so they will come in order
	 */
	nextidx = startchunk;
	toastscan = index_beginscan(toastrel, toastidx, SnapshotToast,
								nscankeys, toastkey);
	while ((ttup = index_getnext(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		residx = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
		Assert(!isnull);
		chunk = DatumGetPointer(fastgetattr(ttup, 3, toasttupDesc, &isnull));
		Assert(!isnull);
		chunksize = VARATT_SIZE(chunk) - VARHDRSZ;

		/*
		 * Some checks on the data we've found
		 */
		if ((residx != nextidx) || (residx > endchunk) || (residx < startchunk))
			elog(ERROR, "unexpected chunk number %d (expected %d) for toast value %u",
				 residx, nextidx,
				 attr->va_content.va_external.va_valueid);
		if (residx < totalchunks - 1)
		{
			if (chunksize != TOAST_MAX_CHUNK_SIZE)
				elog(ERROR, "unexpected chunk size %d in chunk %d for toast value %u",
					 chunksize, residx,
					 attr->va_content.va_external.va_valueid);
		}
		else
		{
			if ((residx * TOAST_MAX_CHUNK_SIZE + chunksize) != attrsize)
				elog(ERROR, "unexpected chunk size %d in chunk %d for toast value %u",
					 chunksize, residx,
					 attr->va_content.va_external.va_valueid);
		}

		/*
		 * Copy the data into proper place in our result
		 */
		chcpystrt = 0;
		chcpyend = chunksize - 1;
		if (residx == startchunk)
			chcpystrt = startoffset;
		if (residx == endchunk)
			chcpyend = endoffset;

		memcpy(((char *) VARATT_DATA(result)) +
			   (residx * TOAST_MAX_CHUNK_SIZE - sliceoffset) + chcpystrt,
			   VARATT_DATA(chunk) + chcpystrt,
			   (chcpyend - chcpystrt) + 1);

		nextidx++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (nextidx != (endchunk + 1))
		elog(ERROR, "missing chunk number %d for toast value %u",
			 nextidx,
			 attr->va_content.va_external.va_valueid);

	/*
	 * End scan and close relations
	 */
	index_endscan(toastscan);
	index_close(toastidx);
	heap_close(toastrel, AccessShareLock);

	return result;
}
