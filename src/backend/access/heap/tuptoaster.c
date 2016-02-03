/*-------------------------------------------------------------------------
 *
 * tuptoaster.c
 *	  Support routines for external and compressed storage of
 *	  variable size attributes.
 *
 * Copyright (c) 2000-2016, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/heap/tuptoaster.c
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

#include "access/genam.h"
#include "access/heapam.h"
#include "access/tuptoaster.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "common/pg_lzcompress.h"
#include "miscadmin.h"
#include "utils/expandeddatum.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/typcache.h"
#include "utils/tqual.h"


#undef TOAST_DEBUG

/*
 *	The information at the start of the compressed toast data.
 */
typedef struct toast_compress_header
{
	int32		vl_len_;		/* varlena header (do not touch directly!) */
	int32		rawsize;
} toast_compress_header;

/*
 * Utilities for manipulation of header information for compressed
 * toast entries.
 */
#define TOAST_COMPRESS_HDRSZ		((int32) sizeof(toast_compress_header))
#define TOAST_COMPRESS_RAWSIZE(ptr) (((toast_compress_header *) (ptr))->rawsize)
#define TOAST_COMPRESS_RAWDATA(ptr) \
	(((char *) (ptr)) + TOAST_COMPRESS_HDRSZ)
#define TOAST_COMPRESS_SET_RAWSIZE(ptr, len) \
	(((toast_compress_header *) (ptr))->rawsize = (len))

static void toast_delete_datum(Relation rel, Datum value);
static Datum toast_save_datum(Relation rel, Datum value,
				 struct varlena * oldexternal, int options);
static bool toastrel_valueid_exists(Relation toastrel, Oid valueid);
static bool toastid_valueid_exists(Oid toastrelid, Oid valueid);
static struct varlena *toast_fetch_datum(struct varlena * attr);
static struct varlena *toast_fetch_datum_slice(struct varlena * attr,
						int32 sliceoffset, int32 length);
static struct varlena *toast_decompress_datum(struct varlena * attr);
static int toast_open_indexes(Relation toastrel,
				   LOCKMODE lock,
				   Relation **toastidxs,
				   int *num_indexes);
static void toast_close_indexes(Relation *toastidxs, int num_indexes,
					LOCKMODE lock);


/* ----------
 * heap_tuple_fetch_attr -
 *
 *	Public entry point to get back a toasted value from
 *	external source (possibly still in compressed format).
 *
 * This will return a datum that contains all the data internally, ie, not
 * relying on external storage or memory, but it can still be compressed or
 * have a short header.  Note some callers assume that if the input is an
 * EXTERNAL datum, the result will be a pfree'able chunk.
 * ----------
 */
struct varlena *
heap_tuple_fetch_attr(struct varlena * attr)
{
	struct varlena *result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/*
		 * This is an external stored plain value
		 */
		result = toast_fetch_datum(attr);
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		/*
		 * This is an indirect pointer --- dereference it
		 */
		struct varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);
		attr = (struct varlena *) redirect.pointer;

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		/* recurse if value is still external in some other way */
		if (VARATT_IS_EXTERNAL(attr))
			return heap_tuple_fetch_attr(attr);

		/*
		 * Copy into the caller's memory context, in case caller tries to
		 * pfree the result.
		 */
		result = (struct varlena *) palloc(VARSIZE_ANY(attr));
		memcpy(result, attr, VARSIZE_ANY(attr));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/*
		 * This is an expanded-object pointer --- get flat format
		 */
		ExpandedObjectHeader *eoh;
		Size		resultsize;

		eoh = DatumGetEOHP(PointerGetDatum(attr));
		resultsize = EOH_get_flat_size(eoh);
		result = (struct varlena *) palloc(resultsize);
		EOH_flatten_into(eoh, (void *) result, resultsize);
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
 *	or external storage.  The result is always non-extended varlena form.
 *
 * Note some callers assume that if the input is an EXTERNAL or COMPRESSED
 * datum, the result will be a pfree'able chunk.
 * ----------
 */
struct varlena *
heap_tuple_untoast_attr(struct varlena * attr)
{
	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/*
		 * This is an externally stored datum --- fetch it back from there
		 */
		attr = toast_fetch_datum(attr);
		/* If it's compressed, decompress it */
		if (VARATT_IS_COMPRESSED(attr))
		{
			struct varlena *tmp = attr;

			attr = toast_decompress_datum(tmp);
			pfree(tmp);
		}
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		/*
		 * This is an indirect pointer --- dereference it
		 */
		struct varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);
		attr = (struct varlena *) redirect.pointer;

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		/* recurse in case value is still extended in some other way */
		attr = heap_tuple_untoast_attr(attr);

		/* if it isn't, we'd better copy it */
		if (attr == (struct varlena *) redirect.pointer)
		{
			struct varlena *result;

			result = (struct varlena *) palloc(VARSIZE_ANY(attr));
			memcpy(result, attr, VARSIZE_ANY(attr));
			attr = result;
		}
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/*
		 * This is an expanded-object pointer --- get flat format
		 */
		attr = heap_tuple_fetch_attr(attr);
		/* flatteners are not allowed to produce compressed/short output */
		Assert(!VARATT_IS_EXTENDED(attr));
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/*
		 * This is a compressed value inside of the main tuple
		 */
		attr = toast_decompress_datum(attr);
	}
	else if (VARATT_IS_SHORT(attr))
	{
		/*
		 * This is a short-header varlena --- convert to 4-byte header format
		 */
		Size		data_size = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT;
		Size		new_size = data_size + VARHDRSZ;
		struct varlena *new_attr;

		new_attr = (struct varlena *) palloc(new_size);
		SET_VARSIZE(new_attr, new_size);
		memcpy(VARDATA(new_attr), VARDATA_SHORT(attr), data_size);
		attr = new_attr;
	}

	return attr;
}


/* ----------
 * heap_tuple_untoast_attr_slice -
 *
 *		Public entry point to get back part of a toasted value
 *		from compression or external storage.
 * ----------
 */
struct varlena *
heap_tuple_untoast_attr_slice(struct varlena * attr,
							  int32 sliceoffset, int32 slicelength)
{
	struct varlena *preslice;
	struct varlena *result;
	char	   *attrdata;
	int32		attrsize;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* fast path for non-compressed external datums */
		if (!VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
			return toast_fetch_datum_slice(attr, sliceoffset, slicelength);

		/* fetch it back (compressed marker will get set automatically) */
		preslice = toast_fetch_datum(attr);
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		struct varatt_indirect redirect;

		VARATT_EXTERNAL_GET_POINTER(redirect, attr);

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(redirect.pointer));

		return heap_tuple_untoast_attr_slice(redirect.pointer,
											 sliceoffset, slicelength);
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		/* pass it off to heap_tuple_fetch_attr to flatten */
		preslice = heap_tuple_fetch_attr(attr);
	}
	else
		preslice = attr;

	Assert(!VARATT_IS_EXTERNAL(preslice));

	if (VARATT_IS_COMPRESSED(preslice))
	{
		struct varlena *tmp = preslice;

		preslice = toast_decompress_datum(tmp);

		if (tmp != attr)
			pfree(tmp);
	}

	if (VARATT_IS_SHORT(preslice))
	{
		attrdata = VARDATA_SHORT(preslice);
		attrsize = VARSIZE_SHORT(preslice) - VARHDRSZ_SHORT;
	}
	else
	{
		attrdata = VARDATA(preslice);
		attrsize = VARSIZE(preslice) - VARHDRSZ;
	}

	/* slicing of datum for compressed cases and plain value */

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		slicelength = 0;
	}

	if (((sliceoffset + slicelength) > attrsize) || slicelength < 0)
		slicelength = attrsize - sliceoffset;

	result = (struct varlena *) palloc(slicelength + VARHDRSZ);
	SET_VARSIZE(result, slicelength + VARHDRSZ);

	memcpy(VARDATA(result), attrdata + sliceoffset, slicelength);

	if (preslice != attr)
		pfree(preslice);

	return result;
}


/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 *	(including the VARHDRSZ header)
 * ----------
 */
Size
toast_raw_datum_size(Datum value)
{
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/* va_rawsize is the size of the original datum -- including header */
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		result = toast_pointer.va_rawsize;
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		struct varatt_indirect toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(toast_pointer.pointer));

		return toast_raw_datum_size(PointerGetDatum(toast_pointer.pointer));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		result = EOH_get_flat_size(DatumGetEOHP(value));
	}
	else if (VARATT_IS_COMPRESSED(attr))
	{
		/* here, va_rawsize is just the payload size */
		result = VARRAWSIZE_4B_C(attr) + VARHDRSZ;
	}
	else if (VARATT_IS_SHORT(attr))
	{
		/*
		 * we have to normalize the header length to VARHDRSZ or else the
		 * callers of this function will be confused.
		 */
		result = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT + VARHDRSZ;
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
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	Size		result;

	if (VARATT_IS_EXTERNAL_ONDISK(attr))
	{
		/*
		 * Attribute is stored externally - return the extsize whether
		 * compressed or not.  We do not count the size of the toast pointer
		 * ... should we?
		 */
		struct varatt_external toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);
		result = toast_pointer.va_extsize;
	}
	else if (VARATT_IS_EXTERNAL_INDIRECT(attr))
	{
		struct varatt_indirect toast_pointer;

		VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

		/* nested indirect Datums aren't allowed */
		Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

		return toast_datum_size(PointerGetDatum(toast_pointer.pointer));
	}
	else if (VARATT_IS_EXTERNAL_EXPANDED(attr))
	{
		result = EOH_get_flat_size(DatumGetEOHP(value));
	}
	else if (VARATT_IS_SHORT(attr))
	{
		result = VARSIZE_SHORT(attr);
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

			if (toast_isnull[i])
				continue;
			else if (VARATT_IS_EXTERNAL_ONDISK(PointerGetDatum(value)))
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
toast_insert_or_update(Relation rel, HeapTuple newtup, HeapTuple oldtup,
					   int options)
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
	Size		hoff;

	char		toast_action[MaxHeapAttributeNumber];
	bool		toast_isnull[MaxHeapAttributeNumber];
	bool		toast_oldisnull[MaxHeapAttributeNumber];
	Datum		toast_values[MaxHeapAttributeNumber];
	Datum		toast_oldvalues[MaxHeapAttributeNumber];
	struct varlena *toast_oldexternal[MaxHeapAttributeNumber];
	int32		toast_sizes[MaxHeapAttributeNumber];
	bool		toast_free[MaxHeapAttributeNumber];
	bool		toast_delold[MaxHeapAttributeNumber];

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
	memset(toast_oldexternal, 0, numAttrs * sizeof(struct varlena *));
	memset(toast_free, 0, numAttrs * sizeof(bool));
	memset(toast_delold, 0, numAttrs * sizeof(bool));

	for (i = 0; i < numAttrs; i++)
	{
		struct varlena *old_value;
		struct varlena *new_value;

		if (oldtup != NULL)
		{
			/*
			 * For UPDATE get the old and new values of this attribute
			 */
			old_value = (struct varlena *) DatumGetPointer(toast_oldvalues[i]);
			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);

			/*
			 * If the old value is stored on disk, check if it has changed so
			 * we have to delete it later.
			 */
			if (att[i]->attlen == -1 && !toast_oldisnull[i] &&
				VARATT_IS_EXTERNAL_ONDISK(old_value))
			{
				if (toast_isnull[i] || !VARATT_IS_EXTERNAL_ONDISK(new_value) ||
					memcmp((char *) old_value, (char *) new_value,
						   VARSIZE_EXTERNAL(old_value)) != 0)
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
					continue;
				}
			}
		}
		else
		{
			/*
			 * For INSERT simply get the new value
			 */
			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
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
			 * still in the tuple must be someone else's that we cannot reuse
			 * (this includes the case of an out-of-line in-memory datum).
			 * Fetch it back (without decompression, unless we are forcing
			 * PLAIN storage).  If necessary, we'll push it out as a new
			 * external value below.
			 */
			if (VARATT_IS_EXTERNAL(new_value))
			{
				toast_oldexternal[i] = new_value;
				if (att[i]->attstorage == 'p')
					new_value = heap_tuple_untoast_attr(new_value);
				else
					new_value = heap_tuple_fetch_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
				need_change = true;
				need_free = true;
			}

			/*
			 * Remember the size of this attribute
			 */
			toast_sizes[i] = VARSIZE_ANY(new_value);
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
	 *	1: Inline compress attributes with attstorage 'x', and store very
	 *	   large attributes with attstorage 'x' or 'e' external immediately
	 *	2: Store attributes with attstorage 'x' or 'e' external
	 *	3: Inline compress attributes with attstorage 'm'
	 *	4: Store attributes with attstorage 'm' external
	 * ----------
	 */

	/* compute header overhead --- this should match heap_form_tuple() */
	hoff = SizeofHeapTupleHeader;
	if (has_nulls)
		hoff += BITMAPLEN(numAttrs);
	if (newtup->t_data->t_infomask & HEAP_HASOID)
		hoff += sizeof(Oid);
	hoff = MAXALIGN(hoff);
	/* now convert to a limit on the tuple data size */
	maxDataLen = TOAST_TUPLE_TARGET - hoff;

	/*
	 * Look for attributes with attstorage 'x' to compress.  Also find large
	 * attributes with attstorage 'x' or 'e', and store them external.
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum		old_value;
		Datum		new_value;

		/*
		 * Search for the biggest yet unprocessed internal attribute
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] != ' ')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue;		/* can't happen, toast_action would be 'p' */
			if (VARATT_IS_COMPRESSED(DatumGetPointer(toast_values[i])))
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
		 * Attempt to compress it inline, if it has attstorage 'x'
		 */
		i = biggest_attno;
		if (att[i]->attstorage == 'x')
		{
			old_value = toast_values[i];
			new_value = toast_compress_datum(old_value);

			if (DatumGetPointer(new_value) != NULL)
			{
				/* successful compression */
				if (toast_free[i])
					pfree(DatumGetPointer(old_value));
				toast_values[i] = new_value;
				toast_free[i] = true;
				toast_sizes[i] = VARSIZE(DatumGetPointer(toast_values[i]));
				need_change = true;
				need_free = true;
			}
			else
			{
				/* incompressible, ignore on subsequent compression passes */
				toast_action[i] = 'x';
			}
		}
		else
		{
			/* has attstorage 'e', ignore on subsequent compression passes */
			toast_action[i] = 'x';
		}

		/*
		 * If this value is by itself more than maxDataLen (after compression
		 * if any), push it out to the toast table immediately, if possible.
		 * This avoids uselessly compressing other fields in the common case
		 * where we have one long field and several short ones.
		 *
		 * XXX maybe the threshold should be less than maxDataLen?
		 */
		if (toast_sizes[i] > maxDataLen &&
			rel->rd_rel->reltoastrelid != InvalidOid)
		{
			old_value = toast_values[i];
			toast_action[i] = 'p';
			toast_values[i] = toast_save_datum(rel, toast_values[i],
											   toast_oldexternal[i], options);
			if (toast_free[i])
				pfree(DatumGetPointer(old_value));
			toast_free[i] = true;
			need_change = true;
			need_free = true;
		}
	}

	/*
	 * Second we look for attributes of attstorage 'x' or 'e' that are still
	 * inline.  But skip this if there's no toast table to push them to.
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
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
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue;		/* can't happen, toast_action would be 'p' */
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
		toast_values[i] = toast_save_datum(rel, toast_values[i],
										   toast_oldexternal[i], options);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));
		toast_free[i] = true;

		need_change = true;
		need_free = true;
	}

	/*
	 * Round 3 - this time we take attributes with storage 'm' into
	 * compression
	 */
	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
		Datum		old_value;
		Datum		new_value;

		/*
		 * Search for the biggest yet uncompressed internal attribute
		 */
		for (i = 0; i < numAttrs; i++)
		{
			if (toast_action[i] != ' ')
				continue;
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue;		/* can't happen, toast_action would be 'p' */
			if (VARATT_IS_COMPRESSED(DatumGetPointer(toast_values[i])))
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
			toast_sizes[i] = VARSIZE(DatumGetPointer(toast_values[i]));
			need_change = true;
			need_free = true;
		}
		else
		{
			/* incompressible, ignore on subsequent compression passes */
			toast_action[i] = 'x';
		}
	}

	/*
	 * Finally we store attributes of type 'm' externally.  At this point we
	 * increase the target tuple size, so that 'm' attributes aren't stored
	 * externally unless really necessary.
	 */
	maxDataLen = TOAST_TUPLE_TARGET_MAIN - hoff;

	while (heap_compute_data_size(tupleDesc,
								  toast_values, toast_isnull) > maxDataLen &&
		   rel->rd_rel->reltoastrelid != InvalidOid)
	{
		int			biggest_attno = -1;
		int32		biggest_size = MAXALIGN(TOAST_POINTER_SIZE);
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
			if (VARATT_IS_EXTERNAL(DatumGetPointer(toast_values[i])))
				continue;		/* can't happen, toast_action would be 'p' */
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
		toast_values[i] = toast_save_datum(rel, toast_values[i],
										   toast_oldexternal[i], options);
		if (toast_free[i])
			pfree(DatumGetPointer(old_value));
		toast_free[i] = true;

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
		if (has_nulls)
			new_header_len += BITMAPLEN(numAttrs);
		if (olddata->t_infomask & HEAP_HASOID)
			new_header_len += sizeof(Oid);
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
		if (olddata->t_infomask & HEAP_HASOID)
			HeapTupleHeaderSetOid(new_data, HeapTupleHeaderGetOid(olddata));

		/* Copy over the data, and fill the null bitmap if needed */
		heap_fill_tuple(tupleDesc,
						toast_values,
						toast_isnull,
						(char *) new_data + new_header_len,
						new_data_len,
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
	Form_pg_attribute *att = tupleDesc->attrs;
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
		if (!toast_isnull[i] && att[i]->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value))
			{
				new_value = heap_tuple_fetch_attr(new_value);
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
	 * Be sure to copy the tuple's OID and identity fields.  We also make a
	 * point of copying visibility info, just in case anybody looks at those
	 * fields in a syscache entry.
	 */
	if (tupleDesc->tdhasoid)
		HeapTupleSetOid(new_tuple, HeapTupleGetOid(tup));

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
	Form_pg_attribute *att = tupleDesc->attrs;
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
		else if (att[i]->attlen == -1)
		{
			struct varlena *new_value;

			new_value = (struct varlena *) DatumGetPointer(toast_values[i]);
			if (VARATT_IS_EXTERNAL(new_value) ||
				VARATT_IS_COMPRESSED(new_value))
			{
				new_value = heap_tuple_untoast_attr(new_value);
				toast_values[i] = PointerGetDatum(new_value);
				toast_free[i] = true;
			}
		}
	}

	/*
	 * Calculate the new size of the tuple.
	 *
	 * This should match the reconstruction code in toast_insert_or_update.
	 */
	new_header_len = SizeofHeapTupleHeader;
	if (has_nulls)
		new_header_len += BITMAPLEN(numAttrs);
	if (tup->t_infomask & HEAP_HASOID)
		new_header_len += sizeof(Oid);
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
	if (tup->t_infomask & HEAP_HASOID)
		HeapTupleHeaderSetOid(new_data, HeapTupleHeaderGetOid(tup));

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
toast_compress_datum(Datum value)
{
	struct varlena *tmp;
	int32		valsize = VARSIZE_ANY_EXHDR(DatumGetPointer(value));
	int32		len;

	Assert(!VARATT_IS_EXTERNAL(DatumGetPointer(value)));
	Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));

	/*
	 * No point in wasting a palloc cycle if value size is out of the allowed
	 * range for compression
	 */
	if (valsize < PGLZ_strategy_default->min_input_size ||
		valsize > PGLZ_strategy_default->max_input_size)
		return PointerGetDatum(NULL);

	tmp = (struct varlena *) palloc(PGLZ_MAX_OUTPUT(valsize) +
									TOAST_COMPRESS_HDRSZ);

	/*
	 * We recheck the actual size even if pglz_compress() reports success,
	 * because it might be satisfied with having saved as little as one byte
	 * in the compressed data --- which could turn into a net loss once you
	 * consider header and alignment padding.  Worst case, the compressed
	 * format might require three padding bytes (plus header, which is
	 * included in VARSIZE(tmp)), whereas the uncompressed format would take
	 * only one header byte and no padding if the value is short enough.  So
	 * we insist on a savings of more than 2 bytes to ensure we have a gain.
	 */
	len = pglz_compress(VARDATA_ANY(DatumGetPointer(value)),
						valsize,
						TOAST_COMPRESS_RAWDATA(tmp),
						PGLZ_strategy_default);
	if (len >= 0 &&
		len + TOAST_COMPRESS_HDRSZ < valsize - 2)
	{
		TOAST_COMPRESS_SET_RAWSIZE(tmp, valsize);
		SET_VARSIZE_COMPRESSED(tmp, len + TOAST_COMPRESS_HDRSZ);
		/* successful compression */
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
	toastrel = heap_open(toastoid, lock);

	/* Look for the valid index of the toast relation */
	validIndex = toast_open_indexes(toastrel,
									lock,
									&toastidxs,
									&num_indexes);
	validIndexOid = RelationGetRelid(toastidxs[validIndex]);

	/* Close the toast relation and all its indexes */
	toast_close_indexes(toastidxs, num_indexes, lock);
	heap_close(toastrel, lock);

	return validIndexOid;
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
static Datum
toast_save_datum(Relation rel, Datum value,
				 struct varlena * oldexternal, int options)
{
	Relation	toastrel;
	Relation   *toastidxs;
	HeapTuple	toasttup;
	TupleDesc	toasttupDesc;
	Datum		t_values[3];
	bool		t_isnull[3];
	CommandId	mycid = GetCurrentCommandId(true);
	struct varlena *result;
	struct varatt_external toast_pointer;
	union
	{
		struct varlena hdr;
		/* this is to make the union big enough for a chunk: */
		char		data[TOAST_MAX_CHUNK_SIZE + VARHDRSZ];
		/* ensure union is aligned well enough: */
		int32		align_it;
	}			chunk_data;
	int32		chunk_size;
	int32		chunk_seq = 0;
	char	   *data_p;
	int32		data_todo;
	Pointer		dval = DatumGetPointer(value);
	int			num_indexes;
	int			validIndex;

	Assert(!VARATT_IS_EXTERNAL(value));

	/*
	 * Open the toast relation and its indexes.  We can use the index to check
	 * uniqueness of the OID we assign to the toasted item, even though it has
	 * additional columns besides OID.
	 */
	toastrel = heap_open(rel->rd_rel->reltoastrelid, RowExclusiveLock);
	toasttupDesc = toastrel->rd_att;

	/* Open all the toast indexes and look for the valid one */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Get the data pointer and length, and compute va_rawsize and va_extsize.
	 *
	 * va_rawsize is the size of the equivalent fully uncompressed datum, so
	 * we have to adjust for short headers.
	 *
	 * va_extsize is the actual size of the data payload in the toast records.
	 */
	if (VARATT_IS_SHORT(dval))
	{
		data_p = VARDATA_SHORT(dval);
		data_todo = VARSIZE_SHORT(dval) - VARHDRSZ_SHORT;
		toast_pointer.va_rawsize = data_todo + VARHDRSZ;		/* as if not short */
		toast_pointer.va_extsize = data_todo;
	}
	else if (VARATT_IS_COMPRESSED(dval))
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		/* rawsize in a compressed datum is just the size of the payload */
		toast_pointer.va_rawsize = VARRAWSIZE_4B_C(dval) + VARHDRSZ;
		toast_pointer.va_extsize = data_todo;
		/* Assert that the numbers look like it's compressed */
		Assert(VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));
	}
	else
	{
		data_p = VARDATA(dval);
		data_todo = VARSIZE(dval) - VARHDRSZ;
		toast_pointer.va_rawsize = VARSIZE(dval);
		toast_pointer.va_extsize = data_todo;
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
		toast_pointer.va_toastrelid = rel->rd_toastoid;
	else
		toast_pointer.va_toastrelid = RelationGetRelid(toastrel);

	/*
	 * Choose an OID to use as the value ID for this toast value.
	 *
	 * Normally we just choose an unused OID within the toast table.  But
	 * during table-rewriting operations where we are preserving an existing
	 * toast table OID, we want to preserve toast value OIDs too.  So, if
	 * rd_toastoid is set and we had a prior external value from that same
	 * toast table, re-use its value ID.  If we didn't have a prior external
	 * value (which is a corner case, but possible if the table's attstorage
	 * options have been changed), we have to pick a value ID that doesn't
	 * conflict with either new or existing toast value OIDs.
	 */
	if (!OidIsValid(rel->rd_toastoid))
	{
		/* normal case: just choose an unused OID */
		toast_pointer.va_valueid =
			GetNewOidWithIndex(toastrel,
							   RelationGetRelid(toastidxs[validIndex]),
							   (AttrNumber) 1);
	}
	else
	{
		/* rewrite case: check to see if value was in old toast table */
		toast_pointer.va_valueid = InvalidOid;
		if (oldexternal != NULL)
		{
			struct varatt_external old_toast_pointer;

			Assert(VARATT_IS_EXTERNAL_ONDISK(oldexternal));
			/* Must copy to access aligned fields */
			VARATT_EXTERNAL_GET_POINTER(old_toast_pointer, oldexternal);
			if (old_toast_pointer.va_toastrelid == rel->rd_toastoid)
			{
				/* This value came from the old toast table; reuse its OID */
				toast_pointer.va_valueid = old_toast_pointer.va_valueid;

				/*
				 * There is a corner case here: the table rewrite might have
				 * to copy both live and recently-dead versions of a row, and
				 * those versions could easily reference the same toast value.
				 * When we copy the second or later version of such a row,
				 * reusing the OID will mean we select an OID that's already
				 * in the new toast table.  Check for that, and if so, just
				 * fall through without writing the data again.
				 *
				 * While annoying and ugly-looking, this is a good thing
				 * because it ensures that we wind up with only one copy of
				 * the toast value when there is only one copy in the old
				 * toast table.  Before we detected this case, we'd have made
				 * multiple copies, wasting space; and what's worse, the
				 * copies belonging to already-deleted heap tuples would not
				 * be reclaimed by VACUUM.
				 */
				if (toastrel_valueid_exists(toastrel,
											toast_pointer.va_valueid))
				{
					/* Match, so short-circuit the data storage loop below */
					data_todo = 0;
				}
			}
		}
		if (toast_pointer.va_valueid == InvalidOid)
		{
			/*
			 * new value; must choose an OID that doesn't conflict in either
			 * old or new toast table
			 */
			do
			{
				toast_pointer.va_valueid =
					GetNewOidWithIndex(toastrel,
									 RelationGetRelid(toastidxs[validIndex]),
									   (AttrNumber) 1);
			} while (toastid_valueid_exists(rel->rd_toastoid,
											toast_pointer.va_valueid));
		}
	}

	/*
	 * Initialize constant parts of the tuple data
	 */
	t_values[0] = ObjectIdGetDatum(toast_pointer.va_valueid);
	t_values[2] = PointerGetDatum(&chunk_data);
	t_isnull[0] = false;
	t_isnull[1] = false;
	t_isnull[2] = false;

	/*
	 * Split up the item into chunks
	 */
	while (data_todo > 0)
	{
		int			i;

		CHECK_FOR_INTERRUPTS();

		/*
		 * Calculate the size of this chunk
		 */
		chunk_size = Min(TOAST_MAX_CHUNK_SIZE, data_todo);

		/*
		 * Build a tuple and store it
		 */
		t_values[1] = Int32GetDatum(chunk_seq++);
		SET_VARSIZE(&chunk_data, chunk_size + VARHDRSZ);
		memcpy(VARDATA(&chunk_data), data_p, chunk_size);
		toasttup = heap_form_tuple(toasttupDesc, t_values, t_isnull);

		heap_insert(toastrel, toasttup, mycid, options, NULL);

		/*
		 * Create the index entry.  We cheat a little here by not using
		 * FormIndexDatum: this relies on the knowledge that the index columns
		 * are the same as the initial columns of the table for all the
		 * indexes.
		 *
		 * Note also that there had better not be any user-created index on
		 * the TOAST table, since we don't bother to update anything else.
		 */
		for (i = 0; i < num_indexes; i++)
		{
			/* Only index relations marked as ready can be updated */
			if (IndexIsReady(toastidxs[i]->rd_index))
				index_insert(toastidxs[i], t_values, t_isnull,
							 &(toasttup->t_self),
							 toastrel,
							 toastidxs[i]->rd_index->indisunique ?
							 UNIQUE_CHECK_YES : UNIQUE_CHECK_NO);
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
	 * Done - close toast relation and its indexes
	 */
	toast_close_indexes(toastidxs, num_indexes, RowExclusiveLock);
	heap_close(toastrel, RowExclusiveLock);

	/*
	 * Create the TOAST pointer value that we'll return
	 */
	result = (struct varlena *) palloc(TOAST_POINTER_SIZE);
	SET_VARTAG_EXTERNAL(result, VARTAG_ONDISK);
	memcpy(VARDATA_EXTERNAL(result), &toast_pointer, sizeof(toast_pointer));

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
	struct varlena *attr = (struct varlena *) DatumGetPointer(value);
	struct varatt_external toast_pointer;
	Relation	toastrel;
	Relation   *toastidxs;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	HeapTuple	toasttup;
	int			num_indexes;
	int			validIndex;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		return;

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/*
	 * Open the toast relation and its indexes
	 */
	toastrel = heap_open(toast_pointer.va_toastrelid, RowExclusiveLock);

	/* Fetch valid relation used for process */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Setup a scan key to find chunks with matching va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
	 * Find all the chunks.  (We don't actually care whether we see them in
	 * sequence or not, but since we've already locked the index we might as
	 * well use systable_beginscan_ordered.)
	 */
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   SnapshotToast, 1, &toastkey);
	while ((toasttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, delete it
		 */
		simple_heap_delete(toastrel, &toasttup->t_self);
	}

	/*
	 * End scan and close relations
	 */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, RowExclusiveLock);
	heap_close(toastrel, RowExclusiveLock);
}


/* ----------
 * toastrel_valueid_exists -
 *
 *	Test whether a toast value with the given ID exists in the toast relation
 * ----------
 */
static bool
toastrel_valueid_exists(Relation toastrel, Oid valueid)
{
	bool		result = false;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	int			num_indexes;
	int			validIndex;
	Relation   *toastidxs;

	/* Fetch a valid index relation */
	validIndex = toast_open_indexes(toastrel,
									RowExclusiveLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Setup a scan key to find chunks with matching va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(valueid));

	/*
	 * Is there any such chunk?
	 */
	toastscan = systable_beginscan(toastrel,
								   RelationGetRelid(toastidxs[validIndex]),
								   true, SnapshotToast, 1, &toastkey);

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
toastid_valueid_exists(Oid toastrelid, Oid valueid)
{
	bool		result;
	Relation	toastrel;

	toastrel = heap_open(toastrelid, AccessShareLock);

	result = toastrel_valueid_exists(toastrel, valueid);

	heap_close(toastrel, AccessShareLock);

	return result;
}


/* ----------
 * toast_fetch_datum -
 *
 *	Reconstruct an in memory Datum from the chunks saved
 *	in the toast relation
 * ----------
 */
static struct varlena *
toast_fetch_datum(struct varlena * attr)
{
	Relation	toastrel;
	Relation   *toastidxs;
	ScanKeyData toastkey;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	TupleDesc	toasttupDesc;
	struct varlena *result;
	struct varatt_external toast_pointer;
	int32		ressize;
	int32		residx,
				nextidx;
	int32		numchunks;
	Pointer		chunk;
	bool		isnull;
	char	   *chunkdata;
	int32		chunksize;
	int			num_indexes;
	int			validIndex;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		elog(ERROR, "toast_fetch_datum shouldn't be called for non-ondisk datums");

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	ressize = toast_pointer.va_extsize;
	numchunks = ((ressize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;

	result = (struct varlena *) palloc(ressize + VARHDRSZ);

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
		SET_VARSIZE_COMPRESSED(result, ressize + VARHDRSZ);
	else
		SET_VARSIZE(result, ressize + VARHDRSZ);

	/*
	 * Open the toast relation and its indexes
	 */
	toastrel = heap_open(toast_pointer.va_toastrelid, AccessShareLock);
	toasttupDesc = toastrel->rd_att;

	/* Look for the valid index of the toast relation */
	validIndex = toast_open_indexes(toastrel,
									AccessShareLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Setup a scan key to fetch from the index by va_valueid
	 */
	ScanKeyInit(&toastkey,
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

	/*
	 * Read the chunks by index
	 *
	 * Note that because the index is actually on (valueid, chunkidx) we will
	 * see the chunks in chunkidx order, even though we didn't explicitly ask
	 * for it.
	 */
	nextidx = 0;

	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										   SnapshotToast, 1, &toastkey);
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		residx = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
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
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));
			chunksize = 0;		/* keep compiler quiet */
			chunkdata = NULL;
		}

		/*
		 * Some checks on the data we've found
		 */
		if (residx != nextidx)
			elog(ERROR, "unexpected chunk number %d (expected %d) for toast value %u in %s",
				 residx, nextidx,
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));
		if (residx < numchunks - 1)
		{
			if (chunksize != TOAST_MAX_CHUNK_SIZE)
				elog(ERROR, "unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s",
					 chunksize, (int) TOAST_MAX_CHUNK_SIZE,
					 residx, numchunks,
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
		}
		else if (residx == numchunks - 1)
		{
			if ((residx * TOAST_MAX_CHUNK_SIZE + chunksize) != ressize)
				elog(ERROR, "unexpected chunk size %d (expected %d) in final chunk %d for toast value %u in %s",
					 chunksize,
					 (int) (ressize - residx * TOAST_MAX_CHUNK_SIZE),
					 residx,
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
		}
		else
			elog(ERROR, "unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
				 residx,
				 0, numchunks - 1,
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));

		/*
		 * Copy the data into proper place in our result
		 */
		memcpy(VARDATA(result) + residx * TOAST_MAX_CHUNK_SIZE,
			   chunkdata,
			   chunksize);

		nextidx++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (nextidx != numchunks)
		elog(ERROR, "missing chunk number %d for toast value %u in %s",
			 nextidx,
			 toast_pointer.va_valueid,
			 RelationGetRelationName(toastrel));

	/*
	 * End scan and close relations
	 */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
	heap_close(toastrel, AccessShareLock);

	return result;
}

/* ----------
 * toast_fetch_datum_slice -
 *
 *	Reconstruct a segment of a Datum from the chunks saved
 *	in the toast relation
 * ----------
 */
static struct varlena *
toast_fetch_datum_slice(struct varlena * attr, int32 sliceoffset, int32 length)
{
	Relation	toastrel;
	Relation   *toastidxs;
	ScanKeyData toastkey[3];
	int			nscankeys;
	SysScanDesc toastscan;
	HeapTuple	ttup;
	TupleDesc	toasttupDesc;
	struct varlena *result;
	struct varatt_external toast_pointer;
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
	char	   *chunkdata;
	int32		chunksize;
	int32		chcpystrt;
	int32		chcpyend;
	int			num_indexes;
	int			validIndex;

	if (!VARATT_IS_EXTERNAL_ONDISK(attr))
		elog(ERROR, "toast_fetch_datum_slice shouldn't be called for non-ondisk datums");

	/* Must copy to access aligned fields */
	VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

	/*
	 * It's nonsense to fetch slices of a compressed datum -- this isn't lo_*
	 * we can't return a compressed datum which is meaningful to toast later
	 */
	Assert(!VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer));

	attrsize = toast_pointer.va_extsize;
	totalchunks = ((attrsize - 1) / TOAST_MAX_CHUNK_SIZE) + 1;

	if (sliceoffset >= attrsize)
	{
		sliceoffset = 0;
		length = 0;
	}

	if (((sliceoffset + length) > attrsize) || length < 0)
		length = attrsize - sliceoffset;

	result = (struct varlena *) palloc(length + VARHDRSZ);

	if (VARATT_EXTERNAL_IS_COMPRESSED(toast_pointer))
		SET_VARSIZE_COMPRESSED(result, length + VARHDRSZ);
	else
		SET_VARSIZE(result, length + VARHDRSZ);

	if (length == 0)
		return result;			/* Can save a lot of work at this point! */

	startchunk = sliceoffset / TOAST_MAX_CHUNK_SIZE;
	endchunk = (sliceoffset + length - 1) / TOAST_MAX_CHUNK_SIZE;
	numchunks = (endchunk - startchunk) + 1;

	startoffset = sliceoffset % TOAST_MAX_CHUNK_SIZE;
	endoffset = (sliceoffset + length - 1) % TOAST_MAX_CHUNK_SIZE;

	/*
	 * Open the toast relation and its indexes
	 */
	toastrel = heap_open(toast_pointer.va_toastrelid, AccessShareLock);
	toasttupDesc = toastrel->rd_att;

	/* Look for the valid index of toast relation */
	validIndex = toast_open_indexes(toastrel,
									AccessShareLock,
									&toastidxs,
									&num_indexes);

	/*
	 * Setup a scan key to fetch from the index. This is either two keys or
	 * three depending on the number of chunks.
	 */
	ScanKeyInit(&toastkey[0],
				(AttrNumber) 1,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(toast_pointer.va_valueid));

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
	toastscan = systable_beginscan_ordered(toastrel, toastidxs[validIndex],
										 SnapshotToast, nscankeys, toastkey);
	while ((ttup = systable_getnext_ordered(toastscan, ForwardScanDirection)) != NULL)
	{
		/*
		 * Have a chunk, extract the sequence number and the data
		 */
		residx = DatumGetInt32(fastgetattr(ttup, 2, toasttupDesc, &isnull));
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
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));
			chunksize = 0;		/* keep compiler quiet */
			chunkdata = NULL;
		}

		/*
		 * Some checks on the data we've found
		 */
		if ((residx != nextidx) || (residx > endchunk) || (residx < startchunk))
			elog(ERROR, "unexpected chunk number %d (expected %d) for toast value %u in %s",
				 residx, nextidx,
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));
		if (residx < totalchunks - 1)
		{
			if (chunksize != TOAST_MAX_CHUNK_SIZE)
				elog(ERROR, "unexpected chunk size %d (expected %d) in chunk %d of %d for toast value %u in %s when fetching slice",
					 chunksize, (int) TOAST_MAX_CHUNK_SIZE,
					 residx, totalchunks,
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
		}
		else if (residx == totalchunks - 1)
		{
			if ((residx * TOAST_MAX_CHUNK_SIZE + chunksize) != attrsize)
				elog(ERROR, "unexpected chunk size %d (expected %d) in final chunk %d for toast value %u in %s when fetching slice",
					 chunksize,
					 (int) (attrsize - residx * TOAST_MAX_CHUNK_SIZE),
					 residx,
					 toast_pointer.va_valueid,
					 RelationGetRelationName(toastrel));
		}
		else
			elog(ERROR, "unexpected chunk number %d (out of range %d..%d) for toast value %u in %s",
				 residx,
				 0, totalchunks - 1,
				 toast_pointer.va_valueid,
				 RelationGetRelationName(toastrel));

		/*
		 * Copy the data into proper place in our result
		 */
		chcpystrt = 0;
		chcpyend = chunksize - 1;
		if (residx == startchunk)
			chcpystrt = startoffset;
		if (residx == endchunk)
			chcpyend = endoffset;

		memcpy(VARDATA(result) +
			   (residx * TOAST_MAX_CHUNK_SIZE - sliceoffset) + chcpystrt,
			   chunkdata + chcpystrt,
			   (chcpyend - chcpystrt) + 1);

		nextidx++;
	}

	/*
	 * Final checks that we successfully fetched the datum
	 */
	if (nextidx != (endchunk + 1))
		elog(ERROR, "missing chunk number %d for toast value %u in %s",
			 nextidx,
			 toast_pointer.va_valueid,
			 RelationGetRelationName(toastrel));

	/*
	 * End scan and close relations
	 */
	systable_endscan_ordered(toastscan);
	toast_close_indexes(toastidxs, num_indexes, AccessShareLock);
	heap_close(toastrel, AccessShareLock);

	return result;
}

/* ----------
 * toast_decompress_datum -
 *
 * Decompress a compressed version of a varlena datum
 */
static struct varlena *
toast_decompress_datum(struct varlena * attr)
{
	struct varlena *result;

	Assert(VARATT_IS_COMPRESSED(attr));

	result = (struct varlena *)
		palloc(TOAST_COMPRESS_RAWSIZE(attr) + VARHDRSZ);
	SET_VARSIZE(result, TOAST_COMPRESS_RAWSIZE(attr) + VARHDRSZ);

	if (pglz_decompress(TOAST_COMPRESS_RAWDATA(attr),
						VARSIZE(attr) - TOAST_COMPRESS_HDRSZ,
						VARDATA(result),
						TOAST_COMPRESS_RAWSIZE(attr)) < 0)
		elog(ERROR, "compressed data is corrupted");

	return result;
}


/* ----------
 * toast_open_indexes
 *
 *	Get an array of the indexes associated to the given toast relation
 *	and return as well the position of the valid index used by the toast
 *	relation in this array. It is the responsibility of the caller of this
 *	function to close the indexes as well as free them.
 */
static int
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
	*toastidxs = (Relation *) palloc(*num_indexes * sizeof(Relation));
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
static void
toast_close_indexes(Relation *toastidxs, int num_indexes, LOCKMODE lock)
{
	int			i;

	/* Close relations and clean up things */
	for (i = 0; i < num_indexes; i++)
		index_close(toastidxs[i], lock);
	pfree(toastidxs);
}
