/*-------------------------------------------------------------------------
 *
 * heaptuple.c
 *	  This file contains heap tuple accessor and mutator routines, as well
 *	  as various tuple utilities.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/heaptuple.c,v 1.71 2001/03/22 06:16:06 momjian Exp $
 *
 * NOTES
 *	  The old interface functions have been converted to macros
 *	  and moved to heapam.h
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "catalog/pg_type.h"


/* ----------------------------------------------------------------
 *						misc support routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		ComputeDataSize
 * ----------------
 */
Size
ComputeDataSize(TupleDesc tupleDesc,
				Datum *value,
				char *nulls)
{
	uint32		data_length = 0;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;
	Form_pg_attribute *att = tupleDesc->attrs;

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (nulls[i] != ' ')
			continue;

		data_length = att_align(data_length, att[i]->attlen, att[i]->attalign);
		data_length = att_addlength(data_length, att[i]->attlen, value[i]);
	}

	return data_length;
}

/* ----------------
 *		DataFill
 * ----------------
 */
void
DataFill(char *data,
		 TupleDesc tupleDesc,
		 Datum *value,
		 char *nulls,
		 uint16 *infomask,
		 bits8 *bit)
{
	bits8	   *bitP = 0;
	int			bitmask = 0;
	uint32		data_length;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;
	Form_pg_attribute *att = tupleDesc->attrs;

	if (bit != NULL)
	{
		bitP = &bit[-1];
		bitmask = CSIGNBIT;
	}

	*infomask &= HEAP_XACT_MASK;

	for (i = 0; i < numberOfAttributes; i++)
	{
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
		data = (char *) att_align((long) data, att[i]->attlen, att[i]->attalign);

		if (att[i]->attbyval)
		{
			/* pass-by-value */
			store_att_byval(data, value[i], att[i]->attlen);
		}
		else if (att[i]->attlen == -1)
		{
			/* varlena */
			*infomask |= HEAP_HASVARLENA;
			if (VARATT_IS_EXTERNAL(value[i]))
				*infomask |= HEAP_HASEXTERNAL;
			if (VARATT_IS_COMPRESSED(value[i]))
				*infomask |= HEAP_HASCOMPRESSED;
			data_length = VARATT_SIZE(DatumGetPointer(value[i]));
			memcpy(data, DatumGetPointer(value[i]), data_length);
		}
		else
		{
			/* fixed-length pass-by-reference */
			Assert(att[i]->attlen >= 0);
			memcpy(data, DatumGetPointer(value[i]),
				   (size_t) (att[i]->attlen));
		}

		data = (char *) att_addlength((long) data, att[i]->attlen, value[i]);
	}
}

/* ----------------------------------------------------------------
 *						heap tuple interface
 * ----------------------------------------------------------------
 */

/* ----------------
 *		heap_attisnull	- returns 1 iff tuple attribute is not present
 * ----------------
 */
int
heap_attisnull(HeapTuple tup, int attnum)
{
	if (attnum > (int) tup->t_data->t_natts)
		return 1;

	if (HeapTupleNoNulls(tup))
		return 0;

	if (attnum > 0)
		return att_isnull(attnum - 1, tup->t_data->t_bits);
	else
		switch (attnum)
		{
			case TableOidAttributeNumber:
			case SelfItemPointerAttributeNumber:
			case ObjectIdAttributeNumber:
			case MinTransactionIdAttributeNumber:
			case MinCommandIdAttributeNumber:
			case MaxTransactionIdAttributeNumber:
			case MaxCommandIdAttributeNumber:
				break;

			case 0:
				elog(ERROR, "heap_attisnull: zero attnum disallowed");

			default:
				elog(ERROR, "heap_attisnull: undefined negative attnum");
		}

	return 0;
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
	bits8	   *bp = tup->t_bits;		/* ptr to null bitmask in tuple */
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
	 *	 1: No nulls and no variable length attributes.
	 *	 2: Has a null or a varlena AFTER att.
	 *	 3: Has nulls or varlenas BEFORE att.
	 * ----------------
	 */

	if (HeapTupleNoNulls(tuple))
	{
#ifdef IN_MACRO
/* This is handled in the macro */
		if (att[attnum]->attcacheoff != -1)
		{
			return fetchatt(att[attnum],
				  (char *) tup + tup->t_hoff + att[attnum]->attcacheoff);
		}
#endif
	}
	else
	{

		/*
		 * there's a null somewhere in the tuple
		 */

		/*
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
		else if (!HeapTupleAllFixed(tuple))
		{
			int			j;

			/*
			 * In for(), we test <= and not < because we want to see if we
			 * can go past it in initializing offsets.
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
	 * If slow is false, and we got here, we know that we have a tuple
	 * with no nulls or varlenas before the target attribute. If possible,
	 * we also want to initialize the remainder of the attribute cached
	 * offset values.
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

			/*
			 * Fix me when going to a machine with more than a four-byte
			 * word!
			 */
			off = att_align(off, att[j]->attlen, att[j]->attalign);

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
		 * Note - This loop is a little tricky.  On iteration i we first set
		 * the offset for attribute i and figure out how much the offset
		 * should be incremented.  Finally, we need to align the offset
		 * based on the size of attribute i+1 (for which the offset has
		 * been computed). -mer 12 Dec 1991
		 */

		for (i = 0; i < attnum; i++)
		{
			if (!HeapTupleNoNulls(tuple))
			{
				if (att_isnull(i, bp))
				{
					usecache = false;
					continue;
				}
			}

			/* If we know the next offset, we can skip the rest */
			if (usecache && att[i]->attcacheoff != -1)
				off = att[i]->attcacheoff;
			else
			{
				off = att_align(off, att[i]->attlen, att[i]->attalign);

				if (usecache)
					att[i]->attcacheoff = off;
			}

			off = att_addlength(off, att[i]->attlen, tp + off);

			if (usecache && att[i]->attlen == -1)
				usecache = false;
		}

		off = att_align(off, att[attnum]->attlen, att[attnum]->attalign);

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
heap_getsysattr(HeapTuple tup, int attnum, bool *isnull)
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
			result = ObjectIdGetDatum(tup->t_data->t_oid);
			break;
		case MinTransactionIdAttributeNumber:
			/* XXX should have a TransactionIdGetDatum macro */
			result = (Datum) (tup->t_data->t_xmin);
			break;
		case MinCommandIdAttributeNumber:
			/* XXX should have a CommandIdGetDatum macro */
			result = (Datum) (tup->t_data->t_cmin);
			break;
		case MaxTransactionIdAttributeNumber:
			/* XXX should have a TransactionIdGetDatum macro */
			result = (Datum) (tup->t_data->t_xmax);
			break;
		case MaxCommandIdAttributeNumber:
			/* XXX should have a CommandIdGetDatum macro */
			result = (Datum) (tup->t_data->t_cmax);
			break;
		case TableOidAttributeNumber:
			result = ObjectIdGetDatum(tup->t_tableOid);
			break;
		default:
			elog(ERROR, "heap_getsysattr: invalid attnum %d", attnum);
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

#ifdef NOT_USED
/* ----------------
 *		heap_deformtuple
 *
 *		the inverse of heap_formtuple (see below)
 * ----------------
 */
void
heap_deformtuple(HeapTuple tuple,
				 TupleDesc tdesc,
				 Datum *values,
				 char *nulls)
{
	int			i;
	int			natts;

	Assert(HeapTupleIsValid(tuple));

	natts = tuple->t_natts;
	for (i = 0; i < natts; i++)
	{
		bool		isnull;

		values[i] = heap_getattr(tuple,
								 i + 1,
								 tdesc,
								 &isnull);
		if (isnull)
			nulls[i] = 'n';
		else
			nulls[i] = ' ';
	}
}

#endif

/* ----------------
 *		heap_formtuple
 *
 *		constructs a tuple from the given *value and *null arrays
 *
 * old comments
 *		Handles alignment by aligning 2 byte attributes on short boundries
 *		and 3 or 4 byte attributes on long word boundries on a vax; and
 *		aligning non-byte attributes on short boundries on a sun.  Does
 *		not properly align fixed length arrays of 1 or 2 byte types (yet).
 *
 *		Null attributes are indicated by a 'n' in the appropriate byte
 *		of the *null.	Non-null attributes are indicated by a ' ' (space).
 *
 *		Fix me.  (Figure that must keep context if debug--allow give oid.)
 *		Assumes in order.
 * ----------------
 */
HeapTuple
heap_formtuple(TupleDesc tupleDescriptor,
			   Datum *value,
			   char *nulls)
{
	HeapTuple	tuple;			/* return tuple */
	HeapTupleHeader td;			/* tuple data */
	int			bitmaplen;
	unsigned long len;
	int			hoff;
	bool		hasnull = false;
	int			i;
	int			numberOfAttributes = tupleDescriptor->natts;

	if (numberOfAttributes > MaxHeapAttributeNumber)
		elog(ERROR, "heap_formtuple: numberOfAttributes of %d > %d",
			 numberOfAttributes, MaxHeapAttributeNumber);

	len = offsetof(HeapTupleHeaderData, t_bits);

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (nulls[i] != ' ')
		{
			hasnull = true;
			break;
		}
	}

	if (hasnull)
	{
		bitmaplen = BITMAPLEN(numberOfAttributes);
		len += bitmaplen;
	}

	hoff = len = MAXALIGN(len); /* be conservative here */

	len += ComputeDataSize(tupleDescriptor, value, nulls);

	tuple = (HeapTuple) palloc(HEAPTUPLESIZE + len);
	tuple->t_datamcxt = CurrentMemoryContext;
	td = tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	MemSet((char *) td, 0, len);

	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;
	td->t_natts = numberOfAttributes;
	td->t_hoff = hoff;

	DataFill((char *) td + td->t_hoff,
			 tupleDescriptor,
			 value,
			 nulls,
			 &td->t_infomask,
			 (hasnull ? td->t_bits : NULL));

	td->t_infomask |= HEAP_XMAX_INVALID;

	return tuple;
}

/* ----------------
 *		heap_modifytuple
 *
 *		forms a new tuple from an old tuple and a set of replacement values.
 *		returns a new palloc'ed tuple.
 * ----------------
 */
HeapTuple
heap_modifytuple(HeapTuple tuple,
				 Relation relation,
				 Datum *replValue,
				 char *replNull,
				 char *repl)
{
	int			attoff;
	int			numberOfAttributes;
	Datum	   *value;
	char	   *nulls;
	bool		isNull;
	HeapTuple	newTuple;
	uint8		infomask;

	/*
	 * sanity checks
	 */
	Assert(HeapTupleIsValid(tuple));
	Assert(RelationIsValid(relation));
	Assert(PointerIsValid(replValue));
	Assert(PointerIsValid(replNull));
	Assert(PointerIsValid(repl));

	numberOfAttributes = RelationGetForm(relation)->relnatts;

	/*
	 * allocate and fill *value and *nulls arrays from either the tuple or
	 * the repl information, as appropriate.
	 */
	value = (Datum *) palloc(numberOfAttributes * sizeof *value);
	nulls = (char *) palloc(numberOfAttributes * sizeof *nulls);

	for (attoff = 0;
		 attoff < numberOfAttributes;
		 attoff += 1)
	{

		if (repl[attoff] == ' ')
		{
			value[attoff] = heap_getattr(tuple,
										 AttrOffsetGetAttrNumber(attoff),
										 RelationGetDescr(relation),
										 &isNull);
			nulls[attoff] = (isNull) ? 'n' : ' ';

		}
		else if (repl[attoff] != 'r')
			elog(ERROR, "heap_modifytuple: repl is \\%3d", repl[attoff]);
		else
		{						/* == 'r' */
			value[attoff] = replValue[attoff];
			nulls[attoff] = replNull[attoff];
		}
	}

	/*
	 * create a new tuple from the *values and *nulls arrays
	 */
	newTuple = heap_formtuple(RelationGetDescr(relation),
							  value,
							  nulls);

	/*
	 * copy the header except for t_len, t_natts, t_hoff, t_bits,
	 * t_infomask
	 */
	infomask = newTuple->t_data->t_infomask;
	memmove((char *) &newTuple->t_data->t_oid,	/* XXX */
			(char *) &tuple->t_data->t_oid,
			((char *) &tuple->t_data->t_hoff -
			 (char *) &tuple->t_data->t_oid));	/* XXX */
	newTuple->t_data->t_infomask = infomask;
	newTuple->t_data->t_natts = numberOfAttributes;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;

	return newTuple;
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


/* ----------------------------------------------------------------
 *						other misc functions
 * ----------------------------------------------------------------
 */

HeapTuple
heap_addheader(uint32 natts,	/* max domain index */
			   int structlen,	/* its length */
			   char *structure) /* pointer to the struct */
{
	HeapTuple	tuple;
	HeapTupleHeader td;			/* tuple data */
	unsigned long len;
	int			hoff;

	AssertArg(natts > 0);

	len = offsetof(HeapTupleHeaderData, t_bits);

	hoff = len = MAXALIGN(len); /* be conservative */
	len += structlen;
	tuple = (HeapTuple) palloc(HEAPTUPLESIZE + len);
	tuple->t_datamcxt = CurrentMemoryContext;
	td = tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;

	MemSet((char *) td, 0, len);

	td->t_hoff = hoff;
	td->t_natts = natts;
	td->t_infomask = 0;
	td->t_infomask |= HEAP_XMAX_INVALID;

	if (structlen > 0)
		memcpy((char *) td + hoff, structure, (size_t) structlen);

	return tuple;
}
