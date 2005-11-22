/*-------------------------------------------------------------------------
 *
 * heaptuple.c
 *	  This file contains heap tuple accessor and mutator routines, as well
 *	  as various tuple utilities.
 *
 * NOTE: there is massive duplication of code in this module to
 * support both the convention that a null is marked by a bool TRUE,
 * and the convention that a null is marked by a char 'n'.	The latter
 * convention is deprecated but it'll probably be a long time before
 * we can get rid of it entirely.
 *
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/common/heaptuple.c,v 1.102.2.1 2005/11/22 18:23:03 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/tuptoaster.h"
#include "catalog/pg_type.h"
#include "executor/tuptable.h"


/* ----------------------------------------------------------------
 *						misc support routines
 * ----------------------------------------------------------------
 */

/*
 * heap_compute_data_size
 *		Determine size of the data area of a tuple to be constructed
 */
Size
heap_compute_data_size(TupleDesc tupleDesc,
					   Datum *values,
					   bool *isnull)
{
	Size		data_length = 0;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;
	Form_pg_attribute *att = tupleDesc->attrs;

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
			continue;

		data_length = att_align(data_length, att[i]->attalign);
		data_length = att_addlength(data_length, att[i]->attlen, values[i]);
	}

	return data_length;
}

/* ----------------
 *		ComputeDataSize
 *
 * Determine size of the data area of a tuple to be constructed
 *
 * OLD API with char 'n'/' ' convention for indicating nulls
 * ----------------
 */
static Size
ComputeDataSize(TupleDesc tupleDesc,
				Datum *values,
				char *nulls)
{
	Size		data_length = 0;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;
	Form_pg_attribute *att = tupleDesc->attrs;

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (nulls[i] != ' ')
			continue;

		data_length = att_align(data_length, att[i]->attalign);
		data_length = att_addlength(data_length, att[i]->attlen, values[i]);
	}

	return data_length;
}

/*
 * heap_fill_tuple
 *		Load data portion of a tuple from values/isnull arrays
 *
 * We also fill the null bitmap (if any) and set the infomask bits
 * that reflect the tuple's data contents.
 */
void
heap_fill_tuple(TupleDesc tupleDesc,
				Datum *values, bool *isnull,
				char *data, uint16 *infomask, bits8 *bit)
{
	bits8	   *bitP;
	int			bitmask;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;
	Form_pg_attribute *att = tupleDesc->attrs;

	if (bit != NULL)
	{
		bitP = &bit[-1];
		bitmask = CSIGNBIT;
	}
	else
	{
		/* just to keep compiler quiet */
		bitP = NULL;
		bitmask = 0;
	}

	*infomask &= ~(HEAP_HASNULL | HEAP_HASVARWIDTH | HEAP_HASEXTENDED);

	for (i = 0; i < numberOfAttributes; i++)
	{
		Size		data_length;

		if (bit != NULL)
		{
			if (bitmask != CSIGNBIT)
				bitmask <<= 1;
			else
			{
				bitP += 1;
				*bitP = 0x0;
				bitmask = 1;
			}

			if (isnull[i])
			{
				*infomask |= HEAP_HASNULL;
				continue;
			}

			*bitP |= bitmask;
		}

		/* XXX we are aligning the pointer itself, not the offset */
		data = (char *) att_align((long) data, att[i]->attalign);

		if (att[i]->attbyval)
		{
			/* pass-by-value */
			store_att_byval(data, values[i], att[i]->attlen);
			data_length = att[i]->attlen;
		}
		else if (att[i]->attlen == -1)
		{
			/* varlena */
			*infomask |= HEAP_HASVARWIDTH;
			if (VARATT_IS_EXTERNAL(values[i]))
				*infomask |= HEAP_HASEXTERNAL;
			if (VARATT_IS_COMPRESSED(values[i]))
				*infomask |= HEAP_HASCOMPRESSED;
			data_length = VARATT_SIZE(DatumGetPointer(values[i]));
			memcpy(data, DatumGetPointer(values[i]), data_length);
		}
		else if (att[i]->attlen == -2)
		{
			/* cstring */
			*infomask |= HEAP_HASVARWIDTH;
			data_length = strlen(DatumGetCString(values[i])) + 1;
			memcpy(data, DatumGetPointer(values[i]), data_length);
		}
		else
		{
			/* fixed-length pass-by-reference */
			Assert(att[i]->attlen > 0);
			data_length = att[i]->attlen;
			memcpy(data, DatumGetPointer(values[i]), data_length);
		}

		data += data_length;
	}
}

/* ----------------
 *		DataFill
 *
 * Load data portion of a tuple from values/nulls arrays
 *
 * OLD API with char 'n'/' ' convention for indicating nulls
 * ----------------
 */
static void
DataFill(char *data,
		 TupleDesc tupleDesc,
		 Datum *values,
		 char *nulls,
		 uint16 *infomask,
		 bits8 *bit)
{
	bits8	   *bitP;
	int			bitmask;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;
	Form_pg_attribute *att = tupleDesc->attrs;

	if (bit != NULL)
	{
		bitP = &bit[-1];
		bitmask = CSIGNBIT;
	}
	else
	{
		/* just to keep compiler quiet */
		bitP = NULL;
		bitmask = 0;
	}

	*infomask &= ~(HEAP_HASNULL | HEAP_HASVARWIDTH | HEAP_HASEXTENDED);

	for (i = 0; i < numberOfAttributes; i++)
	{
		Size		data_length;

		if (bit != NULL)
		{
			if (bitmask != CSIGNBIT)
				bitmask <<= 1;
			else
			{
				bitP += 1;
				*bitP = 0x0;
				bitmask = 1;
			}

			if (nulls[i] == 'n')
			{
				*infomask |= HEAP_HASNULL;
				continue;
			}

			*bitP |= bitmask;
		}

		/* XXX we are aligning the pointer itself, not the offset */
		data = (char *) att_align((long) data, att[i]->attalign);

		if (att[i]->attbyval)
		{
			/* pass-by-value */
			store_att_byval(data, values[i], att[i]->attlen);
			data_length = att[i]->attlen;
		}
		else if (att[i]->attlen == -1)
		{
			/* varlena */
			*infomask |= HEAP_HASVARWIDTH;
			if (VARATT_IS_EXTERNAL(values[i]))
				*infomask |= HEAP_HASEXTERNAL;
			if (VARATT_IS_COMPRESSED(values[i]))
				*infomask |= HEAP_HASCOMPRESSED;
			data_length = VARATT_SIZE(DatumGetPointer(values[i]));
			memcpy(data, DatumGetPointer(values[i]), data_length);
		}
		else if (att[i]->attlen == -2)
		{
			/* cstring */
			*infomask |= HEAP_HASVARWIDTH;
			data_length = strlen(DatumGetCString(values[i])) + 1;
			memcpy(data, DatumGetPointer(values[i]), data_length);
		}
		else
		{
			/* fixed-length pass-by-reference */
			Assert(att[i]->attlen > 0);
			data_length = att[i]->attlen;
			memcpy(data, DatumGetPointer(values[i]), data_length);
		}

		data += data_length;
	}
}

/* ----------------------------------------------------------------
 *						heap tuple interface
 * ----------------------------------------------------------------
 */

/* ----------------
 *		heap_attisnull	- returns TRUE iff tuple attribute is not present
 * ----------------
 */
bool
heap_attisnull(HeapTuple tup, int attnum)
{
	if (attnum > (int) tup->t_data->t_natts)
		return true;

	if (attnum > 0)
	{
		if (HeapTupleNoNulls(tup))
			return false;
		return att_isnull(attnum - 1, tup->t_data->t_bits);
	}

	switch (attnum)
	{
		case TableOidAttributeNumber:
		case SelfItemPointerAttributeNumber:
		case ObjectIdAttributeNumber:
		case MinTransactionIdAttributeNumber:
		case MinCommandIdAttributeNumber:
		case MaxTransactionIdAttributeNumber:
		case MaxCommandIdAttributeNumber:
			/* these are never null */
			break;

		default:
			elog(ERROR, "invalid attnum: %d", attnum);
	}

	return false;
}

/* ----------------
 *		nocachegetattr
 *
 *		This only gets called from fastgetattr() macro, in cases where
 *		we can't use a cacheoffset and the value is not null.
 *
 *		This caches attribute offsets in the attribute descriptor.
 *
 *		An alternate way to speed things up would be to cache offsets
 *		with the tuple, but that seems more difficult unless you take
 *		the storage hit of actually putting those offsets into the
 *		tuple you send to disk.  Yuck.
 *
 *		This scheme will be slightly slower than that, but should
 *		perform well for queries which hit large #'s of tuples.  After
 *		you cache the offsets once, examining all the other tuples using
 *		the same attribute descriptor will go much quicker. -cim 5/4/91
 *
 *		NOTE: if you need to change this code, see also heap_deform_tuple.
 * ----------------
 */
Datum
nocachegetattr(HeapTuple tuple,
			   int attnum,
			   TupleDesc tupleDesc,
			   bool *isnull)
{
	HeapTupleHeader tup = tuple->t_data;
	Form_pg_attribute *att = tupleDesc->attrs;
	char	   *tp;				/* ptr to att in tuple */
	bits8	   *bp = tup->t_bits;		/* ptr to null bitmap in tuple */
	bool		slow = false;	/* do we have to walk nulls? */

	(void) isnull;				/* not used */
#ifdef IN_MACRO
/* This is handled in the macro */
	Assert(attnum > 0);

	if (isnull)
		*isnull = false;
#endif

	attnum--;

	/* ----------------
	 *	 Three cases:
	 *
	 *	 1: No nulls and no variable-width attributes.
	 *	 2: Has a null or a var-width AFTER att.
	 *	 3: Has nulls or var-widths BEFORE att.
	 * ----------------
	 */

	if (HeapTupleNoNulls(tuple))
	{
#ifdef IN_MACRO
/* This is handled in the macro */
		if (att[attnum]->attcacheoff != -1)
		{
			return fetchatt(att[attnum],
							(char *) tup + tup->t_hoff +
							att[attnum]->attcacheoff);
		}
#endif
	}
	else
	{
		/*
		 * there's a null somewhere in the tuple
		 *
		 * check to see if desired att is null
		 */

#ifdef IN_MACRO
/* This is handled in the macro */
		if (att_isnull(attnum, bp))
		{
			if (isnull)
				*isnull = true;
			return (Datum) NULL;
		}
#endif

		/*
		 * Now check to see if any preceding bits are null...
		 */
		{
			int			byte = attnum >> 3;
			int			finalbit = attnum & 0x07;

			/* check for nulls "before" final bit of last byte */
			if ((~bp[byte]) & ((1 << finalbit) - 1))
				slow = true;
			else
			{
				/* check for nulls in any "earlier" bytes */
				int			i;

				for (i = 0; i < byte; i++)
				{
					if (bp[i] != 0xFF)
					{
						slow = true;
						break;
					}
				}
			}
		}
	}

	tp = (char *) tup + tup->t_hoff;

	/*
	 * now check for any non-fixed length attrs before our attribute
	 */
	if (!slow)
	{
		if (att[attnum]->attcacheoff != -1)
		{
			return fetchatt(att[attnum],
							tp + att[attnum]->attcacheoff);
		}
		else if (HeapTupleHasVarWidth(tuple))
		{
			int			j;

			/*
			 * In for(), we test <= and not < because we want to see if we can
			 * go past it in initializing offsets.
			 */
			for (j = 0; j <= attnum; j++)
			{
				if (att[j]->attlen <= 0)
				{
					slow = true;
					break;
				}
			}
		}
	}

	/*
	 * If slow is false, and we got here, we know that we have a tuple with no
	 * nulls or var-widths before the target attribute. If possible, we also
	 * want to initialize the remainder of the attribute cached offset values.
	 */
	if (!slow)
	{
		int			j = 1;
		long		off;

		/*
		 * need to set cache for some atts
		 */

		att[0]->attcacheoff = 0;

		while (j < attnum && att[j]->attcacheoff > 0)
			j++;

		off = att[j - 1]->attcacheoff + att[j - 1]->attlen;

		for (; j <= attnum ||
		/* Can we compute more?  We will probably need them */
			 (j < tup->t_natts &&
			  att[j]->attcacheoff == -1 &&
			  (HeapTupleNoNulls(tuple) || !att_isnull(j, bp)) &&
			  (HeapTupleAllFixed(tuple) || att[j]->attlen > 0)); j++)
		{
			off = att_align(off, att[j]->attalign);

			att[j]->attcacheoff = off;

			off = att_addlength(off, att[j]->attlen, tp + off);
		}

		return fetchatt(att[attnum], tp + att[attnum]->attcacheoff);
	}
	else
	{
		bool		usecache = true;
		int			off = 0;
		int			i;

		/*
		 * Now we know that we have to walk the tuple CAREFULLY.
		 *
		 * Note - This loop is a little tricky.  For each non-null attribute,
		 * we have to first account for alignment padding before the attr,
		 * then advance over the attr based on its length.	Nulls have no
		 * storage and no alignment padding either.  We can use/set
		 * attcacheoff until we pass either a null or a var-width attribute.
		 */

		for (i = 0; i < attnum; i++)
		{
			if (HeapTupleHasNulls(tuple) && att_isnull(i, bp))
			{
				usecache = false;
				continue;
			}

			/* If we know the next offset, we can skip the alignment calc */
			if (usecache && att[i]->attcacheoff != -1)
				off = att[i]->attcacheoff;
			else
			{
				off = att_align(off, att[i]->attalign);

				if (usecache)
					att[i]->attcacheoff = off;
			}

			off = att_addlength(off, att[i]->attlen, tp + off);

			if (usecache && att[i]->attlen <= 0)
				usecache = false;
		}

		off = att_align(off, att[attnum]->attalign);

		return fetchatt(att[attnum], tp + off);
	}
}

/* ----------------
 *		heap_getsysattr
 *
 *		Fetch the value of a system attribute for a tuple.
 *
 * This is a support routine for the heap_getattr macro.  The macro
 * has already determined that the attnum refers to a system attribute.
 * ----------------
 */
Datum
heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Datum		result;

	Assert(tup);

	/* Currently, no sys attribute ever reads as NULL. */
	if (isnull)
		*isnull = false;

	switch (attnum)
	{
		case SelfItemPointerAttributeNumber:
			/* pass-by-reference datatype */
			result = PointerGetDatum(&(tup->t_self));
			break;
		case ObjectIdAttributeNumber:
			result = ObjectIdGetDatum(HeapTupleGetOid(tup));
			break;
		case MinTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(HeapTupleHeaderGetXmin(tup->t_data));
			break;
		case MinCommandIdAttributeNumber:
			result = CommandIdGetDatum(HeapTupleHeaderGetCmin(tup->t_data));
			break;
		case MaxTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(HeapTupleHeaderGetXmax(tup->t_data));
			break;
		case MaxCommandIdAttributeNumber:
			result = CommandIdGetDatum(HeapTupleHeaderGetCmax(tup->t_data));
			break;
		case TableOidAttributeNumber:
			result = ObjectIdGetDatum(tup->t_tableOid);
			break;
		default:
			elog(ERROR, "invalid attnum: %d", attnum);
			result = 0;			/* keep compiler quiet */
			break;
	}
	return result;
}

/* ----------------
 *		heap_copytuple
 *
 *		returns a copy of an entire tuple
 *
 * The HeapTuple struct, tuple header, and tuple data are all allocated
 * as a single palloc() block.
 * ----------------
 */
HeapTuple
heap_copytuple(HeapTuple tuple)
{
	HeapTuple	newTuple;

	if (!HeapTupleIsValid(tuple) || tuple->t_data == NULL)
		return NULL;

	newTuple = (HeapTuple) palloc(HEAPTUPLESIZE + tuple->t_len);
	newTuple->t_len = tuple->t_len;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;
	newTuple->t_datamcxt = CurrentMemoryContext;
	newTuple->t_data = (HeapTupleHeader) ((char *) newTuple + HEAPTUPLESIZE);
	memcpy((char *) newTuple->t_data, (char *) tuple->t_data, tuple->t_len);
	return newTuple;
}

/* ----------------
 *		heap_copytuple_with_tuple
 *
 *		copy a tuple into a caller-supplied HeapTuple management struct
 * ----------------
 */
void
heap_copytuple_with_tuple(HeapTuple src, HeapTuple dest)
{
	if (!HeapTupleIsValid(src) || src->t_data == NULL)
	{
		dest->t_data = NULL;
		return;
	}

	dest->t_len = src->t_len;
	dest->t_self = src->t_self;
	dest->t_tableOid = src->t_tableOid;
	dest->t_datamcxt = CurrentMemoryContext;
	dest->t_data = (HeapTupleHeader) palloc(src->t_len);
	memcpy((char *) dest->t_data, (char *) src->t_data, src->t_len);
}

/*
 * heap_form_tuple
 *		construct a tuple from the given values[] and isnull[] arrays,
 *		which are of the length indicated by tupleDescriptor->natts
 *
 * The result is allocated in the current memory context.
 */
HeapTuple
heap_form_tuple(TupleDesc tupleDescriptor,
				Datum *values,
				bool *isnull)
{
	HeapTuple	tuple;			/* return tuple */
	HeapTupleHeader td;			/* tuple data */
	unsigned long len;
	int			hoff;
	bool		hasnull = false;
	Form_pg_attribute *att = tupleDescriptor->attrs;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	if (numberOfAttributes > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						numberOfAttributes, MaxTupleAttributeNumber)));

	/*
	 * Check for nulls and embedded tuples; expand any toasted attributes in
	 * embedded tuples.  This preserves the invariant that toasting can only
	 * go one level deep.
	 *
	 * We can skip calling toast_flatten_tuple_attribute() if the attribute
	 * couldn't possibly be of composite type.  All composite datums are
	 * varlena and have alignment 'd'; furthermore they aren't arrays. Also,
	 * if an attribute is already toasted, it must have been sent to disk
	 * already and so cannot contain toasted attributes.
	 */
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
			hasnull = true;
		else if (att[i]->attlen == -1 &&
				 att[i]->attalign == 'd' &&
				 att[i]->attndims == 0 &&
				 !VARATT_IS_EXTENDED(values[i]))
		{
			values[i] = toast_flatten_tuple_attribute(values[i],
													  att[i]->atttypid,
													  att[i]->atttypmod);
		}
	}

	/*
	 * Determine total space needed
	 */
	len = offsetof(HeapTupleHeaderData, t_bits);

	if (hasnull)
		len += BITMAPLEN(numberOfAttributes);

	if (tupleDescriptor->tdhasoid)
		len += sizeof(Oid);

	hoff = len = MAXALIGN(len); /* align user data safely */

	len += heap_compute_data_size(tupleDescriptor, values, isnull);

	/*
	 * Allocate and zero the space needed.	Note that the tuple body and
	 * HeapTupleData management structure are allocated in one chunk.
	 */
	tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
	tuple->t_datamcxt = CurrentMemoryContext;
	tuple->t_data = td = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	/*
	 * And fill in the information.  Note we fill the Datum fields even though
	 * this tuple may never become a Datum.
	 */
	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;

	HeapTupleHeaderSetDatumLength(td, len);
	HeapTupleHeaderSetTypeId(td, tupleDescriptor->tdtypeid);
	HeapTupleHeaderSetTypMod(td, tupleDescriptor->tdtypmod);

	td->t_natts = numberOfAttributes;
	td->t_hoff = hoff;

	if (tupleDescriptor->tdhasoid)		/* else leave infomask = 0 */
		td->t_infomask = HEAP_HASOID;

	heap_fill_tuple(tupleDescriptor,
					values,
					isnull,
					(char *) td + hoff,
					&td->t_infomask,
					(hasnull ? td->t_bits : NULL));

	return tuple;
}

/* ----------------
 *		heap_formtuple
 *
 *		construct a tuple from the given values[] and nulls[] arrays
 *
 *		Null attributes are indicated by a 'n' in the appropriate byte
 *		of nulls[]. Non-null attributes are indicated by a ' ' (space).
 *
 * OLD API with char 'n'/' ' convention for indicating nulls
 * ----------------
 */
HeapTuple
heap_formtuple(TupleDesc tupleDescriptor,
			   Datum *values,
			   char *nulls)
{
	HeapTuple	tuple;			/* return tuple */
	HeapTupleHeader td;			/* tuple data */
	unsigned long len;
	int			hoff;
	bool		hasnull = false;
	Form_pg_attribute *att = tupleDescriptor->attrs;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	if (numberOfAttributes > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						numberOfAttributes, MaxTupleAttributeNumber)));

	/*
	 * Check for nulls and embedded tuples; expand any toasted attributes in
	 * embedded tuples.  This preserves the invariant that toasting can only
	 * go one level deep.
	 *
	 * We can skip calling toast_flatten_tuple_attribute() if the attribute
	 * couldn't possibly be of composite type.  All composite datums are
	 * varlena and have alignment 'd'; furthermore they aren't arrays. Also,
	 * if an attribute is already toasted, it must have been sent to disk
	 * already and so cannot contain toasted attributes.
	 */
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (nulls[i] != ' ')
			hasnull = true;
		else if (att[i]->attlen == -1 &&
				 att[i]->attalign == 'd' &&
				 att[i]->attndims == 0 &&
				 !VARATT_IS_EXTENDED(values[i]))
		{
			values[i] = toast_flatten_tuple_attribute(values[i],
													  att[i]->atttypid,
													  att[i]->atttypmod);
		}
	}

	/*
	 * Determine total space needed
	 */
	len = offsetof(HeapTupleHeaderData, t_bits);

	if (hasnull)
		len += BITMAPLEN(numberOfAttributes);

	if (tupleDescriptor->tdhasoid)
		len += sizeof(Oid);

	hoff = len = MAXALIGN(len); /* align user data safely */

	len += ComputeDataSize(tupleDescriptor, values, nulls);

	/*
	 * Allocate and zero the space needed.	Note that the tuple body and
	 * HeapTupleData management structure are allocated in one chunk.
	 */
	tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
	tuple->t_datamcxt = CurrentMemoryContext;
	tuple->t_data = td = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	/*
	 * And fill in the information.  Note we fill the Datum fields even though
	 * this tuple may never become a Datum.
	 */
	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;

	HeapTupleHeaderSetDatumLength(td, len);
	HeapTupleHeaderSetTypeId(td, tupleDescriptor->tdtypeid);
	HeapTupleHeaderSetTypMod(td, tupleDescriptor->tdtypmod);

	td->t_natts = numberOfAttributes;
	td->t_hoff = hoff;

	if (tupleDescriptor->tdhasoid)		/* else leave infomask = 0 */
		td->t_infomask = HEAP_HASOID;

	DataFill((char *) td + hoff,
			 tupleDescriptor,
			 values,
			 nulls,
			 &td->t_infomask,
			 (hasnull ? td->t_bits : NULL));

	return tuple;
}

/*
 * heap_modify_tuple
 *		form a new tuple from an old tuple and a set of replacement values.
 *
 * The replValues, replIsnull, and doReplace arrays must be of the length
 * indicated by tupleDesc->natts.  The new tuple is constructed using the data
 * from replValues/replIsnull at columns where doReplace is true, and using
 * the data from the old tuple at columns where doReplace is false.
 *
 * The result is allocated in the current memory context.
 */
HeapTuple
heap_modify_tuple(HeapTuple tuple,
				  TupleDesc tupleDesc,
				  Datum *replValues,
				  bool *replIsnull,
				  bool *doReplace)
{
	int			numberOfAttributes = tupleDesc->natts;
	int			attoff;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	newTuple;

	/*
	 * allocate and fill values and isnull arrays from either the tuple or the
	 * repl information, as appropriate.
	 *
	 * NOTE: it's debatable whether to use heap_deform_tuple() here or just
	 * heap_getattr() only the non-replaced colums.  The latter could win if
	 * there are many replaced columns and few non-replaced ones. However,
	 * heap_deform_tuple costs only O(N) while the heap_getattr way would cost
	 * O(N^2) if there are many non-replaced columns, so it seems better to
	 * err on the side of linear cost.
	 */
	values = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	isnull = (bool *) palloc(numberOfAttributes * sizeof(bool));

	heap_deform_tuple(tuple, tupleDesc, values, isnull);

	for (attoff = 0; attoff < numberOfAttributes; attoff++)
	{
		if (doReplace[attoff])
		{
			values[attoff] = replValues[attoff];
			isnull[attoff] = replIsnull[attoff];
		}
	}

	/*
	 * create a new tuple from the values and isnull arrays
	 */
	newTuple = heap_form_tuple(tupleDesc, values, isnull);

	pfree(values);
	pfree(isnull);

	/*
	 * copy the identification info of the old tuple: t_ctid, t_self, and OID
	 * (if any)
	 */
	newTuple->t_data->t_ctid = tuple->t_data->t_ctid;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;
	if (tupleDesc->tdhasoid)
		HeapTupleSetOid(newTuple, HeapTupleGetOid(tuple));

	return newTuple;
}

/* ----------------
 *		heap_modifytuple
 *
 *		forms a new tuple from an old tuple and a set of replacement values.
 *		returns a new palloc'ed tuple.
 *
 * OLD API with char 'n'/' ' convention for indicating nulls, and
 * char 'r'/' ' convention for indicating whether to replace columns.
 * ----------------
 */
HeapTuple
heap_modifytuple(HeapTuple tuple,
				 TupleDesc tupleDesc,
				 Datum *replValues,
				 char *replNulls,
				 char *replActions)
{
	int			numberOfAttributes = tupleDesc->natts;
	int			attoff;
	Datum	   *values;
	char	   *nulls;
	HeapTuple	newTuple;

	/*
	 * allocate and fill values and nulls arrays from either the tuple or the
	 * repl information, as appropriate.
	 *
	 * NOTE: it's debatable whether to use heap_deformtuple() here or just
	 * heap_getattr() only the non-replaced colums.  The latter could win if
	 * there are many replaced columns and few non-replaced ones. However,
	 * heap_deformtuple costs only O(N) while the heap_getattr way would cost
	 * O(N^2) if there are many non-replaced columns, so it seems better to
	 * err on the side of linear cost.
	 */
	values = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	nulls = (char *) palloc(numberOfAttributes * sizeof(char));

	heap_deformtuple(tuple, tupleDesc, values, nulls);

	for (attoff = 0; attoff < numberOfAttributes; attoff++)
	{
		if (replActions[attoff] == 'r')
		{
			values[attoff] = replValues[attoff];
			nulls[attoff] = replNulls[attoff];
		}
		else if (replActions[attoff] != ' ')
			elog(ERROR, "unrecognized replace flag: %d",
				 (int) replActions[attoff]);
	}

	/*
	 * create a new tuple from the values and nulls arrays
	 */
	newTuple = heap_formtuple(tupleDesc, values, nulls);

	pfree(values);
	pfree(nulls);

	/*
	 * copy the identification info of the old tuple: t_ctid, t_self, and OID
	 * (if any)
	 */
	newTuple->t_data->t_ctid = tuple->t_data->t_ctid;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;
	if (tupleDesc->tdhasoid)
		HeapTupleSetOid(newTuple, HeapTupleGetOid(tuple));

	return newTuple;
}

/*
 * heap_deform_tuple
 *		Given a tuple, extract data into values/isnull arrays; this is
 *		the inverse of heap_form_tuple.
 *
 *		Storage for the values/isnull arrays is provided by the caller;
 *		it should be sized according to tupleDesc->natts not tuple->t_natts.
 *
 *		Note that for pass-by-reference datatypes, the pointer placed
 *		in the Datum will point into the given tuple.
 *
 *		When all or most of a tuple's fields need to be extracted,
 *		this routine will be significantly quicker than a loop around
 *		heap_getattr; the loop will become O(N^2) as soon as any
 *		noncacheable attribute offsets are involved.
 */
void
heap_deform_tuple(HeapTuple tuple, TupleDesc tupleDesc,
				  Datum *values, bool *isnull)
{
	HeapTupleHeader tup = tuple->t_data;
	bool		hasnulls = HeapTupleHasNulls(tuple);
	Form_pg_attribute *att = tupleDesc->attrs;
	int			tdesc_natts = tupleDesc->natts;
	int			natts;			/* number of atts to extract */
	int			attnum;
	char	   *tp;				/* ptr to tuple data */
	long		off;			/* offset in tuple data */
	bits8	   *bp = tup->t_bits;		/* ptr to null bitmap in tuple */
	bool		slow = false;	/* can we use/set attcacheoff? */

	natts = tup->t_natts;

	/*
	 * In inheritance situations, it is possible that the given tuple actually
	 * has more fields than the caller is expecting.  Don't run off the end of
	 * the caller's arrays.
	 */
	natts = Min(natts, tdesc_natts);

	tp = (char *) tup + tup->t_hoff;

	off = 0;

	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = att[attnum];

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;		/* can't use attcacheoff anymore */
			continue;
		}

		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else
		{
			off = att_align(off, thisatt->attalign);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* can't use attcacheoff anymore */
	}

	/*
	 * If tuple doesn't have all the atts indicated by tupleDesc, read the
	 * rest as null
	 */
	for (; attnum < tdesc_natts; attnum++)
	{
		values[attnum] = (Datum) 0;
		isnull[attnum] = true;
	}
}

/* ----------------
 *		heap_deformtuple
 *
 *		Given a tuple, extract data into values/nulls arrays; this is
 *		the inverse of heap_formtuple.
 *
 *		Storage for the values/nulls arrays is provided by the caller;
 *		it should be sized according to tupleDesc->natts not tuple->t_natts.
 *
 *		Note that for pass-by-reference datatypes, the pointer placed
 *		in the Datum will point into the given tuple.
 *
 *		When all or most of a tuple's fields need to be extracted,
 *		this routine will be significantly quicker than a loop around
 *		heap_getattr; the loop will become O(N^2) as soon as any
 *		noncacheable attribute offsets are involved.
 *
 * OLD API with char 'n'/' ' convention for indicating nulls
 * ----------------
 */
void
heap_deformtuple(HeapTuple tuple,
				 TupleDesc tupleDesc,
				 Datum *values,
				 char *nulls)
{
	HeapTupleHeader tup = tuple->t_data;
	bool		hasnulls = HeapTupleHasNulls(tuple);
	Form_pg_attribute *att = tupleDesc->attrs;
	int			tdesc_natts = tupleDesc->natts;
	int			natts;			/* number of atts to extract */
	int			attnum;
	char	   *tp;				/* ptr to tuple data */
	long		off;			/* offset in tuple data */
	bits8	   *bp = tup->t_bits;		/* ptr to null bitmap in tuple */
	bool		slow = false;	/* can we use/set attcacheoff? */

	natts = tup->t_natts;

	/*
	 * In inheritance situations, it is possible that the given tuple actually
	 * has more fields than the caller is expecting.  Don't run off the end of
	 * the caller's arrays.
	 */
	natts = Min(natts, tdesc_natts);

	tp = (char *) tup + tup->t_hoff;

	off = 0;

	for (attnum = 0; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = att[attnum];

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			nulls[attnum] = 'n';
			slow = true;		/* can't use attcacheoff anymore */
			continue;
		}

		nulls[attnum] = ' ';

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else
		{
			off = att_align(off, thisatt->attalign);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* can't use attcacheoff anymore */
	}

	/*
	 * If tuple doesn't have all the atts indicated by tupleDesc, read the
	 * rest as null
	 */
	for (; attnum < tdesc_natts; attnum++)
	{
		values[attnum] = (Datum) 0;
		nulls[attnum] = 'n';
	}
}

/*
 * slot_deform_tuple
 *		Given a TupleTableSlot, extract data from the slot's physical tuple
 *		into its Datum/isnull arrays.  Data is extracted up through the
 *		natts'th column (caller must ensure this is a legal column number).
 *
 *		This is essentially an incremental version of heap_deform_tuple:
 *		on each call we extract attributes up to the one needed, without
 *		re-computing information about previously extracted attributes.
 *		slot->tts_nvalid is the number of attributes already extracted.
 */
static void
slot_deform_tuple(TupleTableSlot *slot, int natts)
{
	HeapTuple	tuple = slot->tts_tuple;
	TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
	Datum	   *values = slot->tts_values;
	bool	   *isnull = slot->tts_isnull;
	HeapTupleHeader tup = tuple->t_data;
	bool		hasnulls = HeapTupleHasNulls(tuple);
	Form_pg_attribute *att = tupleDesc->attrs;
	int			attnum;
	char	   *tp;				/* ptr to tuple data */
	long		off;			/* offset in tuple data */
	bits8	   *bp = tup->t_bits;		/* ptr to null bitmap in tuple */
	bool		slow;			/* can we use/set attcacheoff? */

	/*
	 * Check whether the first call for this tuple, and initialize or restore
	 * loop state.
	 */
	attnum = slot->tts_nvalid;
	if (attnum == 0)
	{
		/* Start from the first attribute */
		off = 0;
		slow = false;
	}
	else
	{
		/* Restore state from previous execution */
		off = slot->tts_off;
		slow = slot->tts_slow;
	}

	tp = (char *) tup + tup->t_hoff;

	for (; attnum < natts; attnum++)
	{
		Form_pg_attribute thisatt = att[attnum];

		if (hasnulls && att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			slow = true;		/* can't use attcacheoff anymore */
			continue;
		}

		isnull[attnum] = false;

		if (!slow && thisatt->attcacheoff >= 0)
			off = thisatt->attcacheoff;
		else
		{
			off = att_align(off, thisatt->attalign);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* can't use attcacheoff anymore */
	}

	/*
	 * Save state for next execution
	 */
	slot->tts_nvalid = attnum;
	slot->tts_off = off;
	slot->tts_slow = slow;
}

/*
 * slot_getattr
 *		This function fetches an attribute of the slot's current tuple.
 *		It is functionally equivalent to heap_getattr, but fetches of
 *		multiple attributes of the same tuple will be optimized better,
 *		because we avoid O(N^2) behavior from multiple calls of
 *		nocachegetattr(), even when attcacheoff isn't usable.
 *
 *		A difference from raw heap_getattr is that attnums beyond the
 *		slot's tupdesc's last attribute will be considered NULL even
 *		when the physical tuple is longer than the tupdesc.
 */
Datum
slot_getattr(TupleTableSlot *slot, int attnum, bool *isnull)
{
	HeapTuple	tuple = slot->tts_tuple;
	TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
	HeapTupleHeader tup;

	/*
	 * system attributes are handled by heap_getsysattr
	 */
	if (attnum <= 0)
	{
		if (tuple == NULL)		/* internal error */
			elog(ERROR, "cannot extract system attribute from virtual tuple");
		return heap_getsysattr(tuple, attnum, tupleDesc, isnull);
	}

	/*
	 * fast path if desired attribute already cached
	 */
	if (attnum <= slot->tts_nvalid)
	{
		*isnull = slot->tts_isnull[attnum - 1];
		return slot->tts_values[attnum - 1];
	}

	/*
	 * return NULL if attnum is out of range according to the tupdesc
	 */
	if (attnum > tupleDesc->natts)
	{
		*isnull = true;
		return (Datum) 0;
	}

	/*
	 * otherwise we had better have a physical tuple (tts_nvalid should equal
	 * natts in all virtual-tuple cases)
	 */
	if (tuple == NULL)			/* internal error */
		elog(ERROR, "cannot extract attribute from empty tuple slot");

	/*
	 * return NULL if attnum is out of range according to the tuple
	 *
	 * (We have to check this separately because of various inheritance and
	 * table-alteration scenarios: the tuple could be either longer or shorter
	 * than the tupdesc.)
	 */
	tup = tuple->t_data;
	if (attnum > tup->t_natts)
	{
		*isnull = true;
		return (Datum) 0;
	}

	/*
	 * check if target attribute is null: no point in groveling through tuple
	 */
	if (HeapTupleHasNulls(tuple) && att_isnull(attnum - 1, tup->t_bits))
	{
		*isnull = true;
		return (Datum) 0;
	}

	/*
	 * If the attribute's column has been dropped, we force a NULL result.
	 * This case should not happen in normal use, but it could happen if we
	 * are executing a plan cached before the column was dropped.
	 */
	if (tupleDesc->attrs[attnum - 1]->attisdropped)
	{
		*isnull = true;
		return (Datum) 0;
	}

	/*
	 * Extract the attribute, along with any preceding attributes.
	 */
	slot_deform_tuple(slot, attnum);

	/*
	 * The result is acquired from tts_values array.
	 */
	*isnull = slot->tts_isnull[attnum - 1];
	return slot->tts_values[attnum - 1];
}

/*
 * slot_getallattrs
 *		This function forces all the entries of the slot's Datum/isnull
 *		arrays to be valid.  The caller may then extract data directly
 *		from those arrays instead of using slot_getattr.
 */
void
slot_getallattrs(TupleTableSlot *slot)
{
	int			tdesc_natts = slot->tts_tupleDescriptor->natts;
	int			attnum;
	HeapTuple	tuple;

	/* Quick out if we have 'em all already */
	if (slot->tts_nvalid == tdesc_natts)
		return;

	/*
	 * otherwise we had better have a physical tuple (tts_nvalid should equal
	 * natts in all virtual-tuple cases)
	 */
	tuple = slot->tts_tuple;
	if (tuple == NULL)			/* internal error */
		elog(ERROR, "cannot extract attribute from empty tuple slot");

	/*
	 * load up any slots available from physical tuple
	 */
	attnum = tuple->t_data->t_natts;
	attnum = Min(attnum, tdesc_natts);

	slot_deform_tuple(slot, attnum);

	/*
	 * If tuple doesn't have all the atts indicated by tupleDesc, read the
	 * rest as null
	 */
	for (; attnum < tdesc_natts; attnum++)
	{
		slot->tts_values[attnum] = (Datum) 0;
		slot->tts_isnull[attnum] = true;
	}
	slot->tts_nvalid = tdesc_natts;
}

/*
 * slot_getsomeattrs
 *		This function forces the entries of the slot's Datum/isnull
 *		arrays to be valid at least up through the attnum'th entry.
 */
void
slot_getsomeattrs(TupleTableSlot *slot, int attnum)
{
	HeapTuple	tuple;
	int			attno;

	/* Quick out if we have 'em all already */
	if (slot->tts_nvalid >= attnum)
		return;

	/* Check for caller error */
	if (attnum <= 0 || attnum > slot->tts_tupleDescriptor->natts)
		elog(ERROR, "invalid attribute number %d", attnum);

	/*
	 * otherwise we had better have a physical tuple (tts_nvalid should equal
	 * natts in all virtual-tuple cases)
	 */
	tuple = slot->tts_tuple;
	if (tuple == NULL)			/* internal error */
		elog(ERROR, "cannot extract attribute from empty tuple slot");

	/*
	 * load up any slots available from physical tuple
	 */
	attno = tuple->t_data->t_natts;
	attno = Min(attno, attnum);

	slot_deform_tuple(slot, attno);

	/*
	 * If tuple doesn't have all the atts indicated by tupleDesc, read the
	 * rest as null
	 */
	for (; attno < attnum; attno++)
	{
		slot->tts_values[attno] = (Datum) 0;
		slot->tts_isnull[attno] = true;
	}
	slot->tts_nvalid = attnum;
}

/*
 * slot_attisnull
 *		Detect whether an attribute of the slot is null, without
 *		actually fetching it.
 */
bool
slot_attisnull(TupleTableSlot *slot, int attnum)
{
	HeapTuple	tuple = slot->tts_tuple;
	TupleDesc	tupleDesc = slot->tts_tupleDescriptor;

	/*
	 * system attributes are handled by heap_attisnull
	 */
	if (attnum <= 0)
	{
		if (tuple == NULL)		/* internal error */
			elog(ERROR, "cannot extract system attribute from virtual tuple");
		return heap_attisnull(tuple, attnum);
	}

	/*
	 * fast path if desired attribute already cached
	 */
	if (attnum <= slot->tts_nvalid)
		return slot->tts_isnull[attnum - 1];

	/*
	 * return NULL if attnum is out of range according to the tupdesc
	 */
	if (attnum > tupleDesc->natts)
		return true;

	/*
	 * otherwise we had better have a physical tuple (tts_nvalid should equal
	 * natts in all virtual-tuple cases)
	 */
	if (tuple == NULL)			/* internal error */
		elog(ERROR, "cannot extract attribute from empty tuple slot");

	/* and let the tuple tell it */
	return heap_attisnull(tuple, attnum);
}

/* ----------------
 *		heap_freetuple
 * ----------------
 */
void
heap_freetuple(HeapTuple htup)
{
	if (htup->t_data != NULL)
		if (htup->t_datamcxt != NULL && (char *) (htup->t_data) !=
			((char *) htup + HEAPTUPLESIZE))
			pfree(htup->t_data);

	pfree(htup);
}


/* ----------------
 *		heap_addheader
 *
 * This routine forms a HeapTuple by copying the given structure (tuple
 * data) and adding a generic header.  Note that the tuple data is
 * presumed to contain no null fields and no varlena fields.
 *
 * This routine is really only useful for certain system tables that are
 * known to be fixed-width and null-free.  It is used in some places for
 * pg_class, but that is a gross hack (it only works because relacl can
 * be omitted from the tuple entirely in those places).
 * ----------------
 */
HeapTuple
heap_addheader(int natts,		/* max domain index */
			   bool withoid,	/* reserve space for oid */
			   Size structlen,	/* its length */
			   void *structure) /* pointer to the struct */
{
	HeapTuple	tuple;
	HeapTupleHeader td;
	Size		len;
	int			hoff;

	AssertArg(natts > 0);

	/* header needs no null bitmap */
	hoff = offsetof(HeapTupleHeaderData, t_bits);
	if (withoid)
		hoff += sizeof(Oid);
	hoff = MAXALIGN(hoff);
	len = hoff + structlen;

	tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
	tuple->t_datamcxt = CurrentMemoryContext;
	tuple->t_data = td = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;

	/* we don't bother to fill the Datum fields */

	td->t_natts = natts;
	td->t_hoff = hoff;

	if (withoid)				/* else leave infomask = 0 */
		td->t_infomask = HEAP_HASOID;

	memcpy((char *) td + hoff, structure, structlen);

	return tuple;
}
