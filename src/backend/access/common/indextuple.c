/*-------------------------------------------------------------------------
 *
 * indextuple.c
 *	   This file contains index tuple accessor and mutator routines,
 *	   as well as a few various tuple utilities.
 *
 * Portions Copyright (c) 1996-2000, PostgreSQL, Inc
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/indextuple.c,v 1.49 2000/12/27 23:59:10 tgl Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/tuptoaster.h"
#include "access/itup.h"
#include "catalog/pg_type.h"


/* ----------------------------------------------------------------
 *				  index_ tuple interface routines
 * ----------------------------------------------------------------
 */

/* ----------------
 *		index_formtuple
 * ----------------
 */
IndexTuple
index_formtuple(TupleDesc tupleDescriptor,
				Datum *value,
				char *null)
{
	char	   *tp;				/* tuple pointer */
	IndexTuple	tuple;			/* return tuple */
	Size		size,
				hoff;
	int			i;
	unsigned short infomask = 0;
	bool		hasnull = false;
	uint16		tupmask = 0;
	int			numberOfAttributes = tupleDescriptor->natts;
#ifdef TOAST_INDEX_HACK
	Datum		untoasted_value[INDEX_MAX_KEYS];
	bool		untoasted_free[INDEX_MAX_KEYS];
#endif

	if (numberOfAttributes > INDEX_MAX_KEYS)
		elog(ERROR, "index_formtuple: numberOfAttributes %d > %d",
			 numberOfAttributes, INDEX_MAX_KEYS);

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (null[i] != ' ' || tupleDescriptor->attrs[i]->attlen >= 0)
		{
			untoasted_value[i] = value[i];
			untoasted_free[i] = false;
		}
		else
		{
			if (VARATT_IS_EXTERNAL(value[i]))
			{
				untoasted_value[i] = PointerGetDatum(
						heap_tuple_fetch_attr(
						(varattrib *)DatumGetPointer(value[i])));
				untoasted_free[i] = true;
			}
			else
			{
				untoasted_value[i] = value[i];
				untoasted_free[i] = false;
			}
		}
	}
#endif

	for (i = 0; i < numberOfAttributes; i++)
	{
		if (null[i] != ' ')
		{
			hasnull = true;
			break;
		}
	}

	if (hasnull)
		infomask |= INDEX_NULL_MASK;

	hoff = IndexInfoFindDataOffset(infomask);
#ifdef TOAST_INDEX_HACK
	size = hoff + ComputeDataSize(tupleDescriptor, untoasted_value, null);
#else
	size = hoff + ComputeDataSize(tupleDescriptor, value, null);
#endif
	size = MAXALIGN(size);		/* be conservative */

	tp = (char *) palloc(size);
	tuple = (IndexTuple) tp;
	MemSet(tp, 0, size);

	DataFill((char *) tp + hoff,
			 tupleDescriptor,
#ifdef TOAST_INDEX_HACK
			 untoasted_value,
#else
			 value,
#endif
			 null,
			 &tupmask,
			 (hasnull ? (bits8 *) tp + sizeof(*tuple) : NULL));

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (untoasted_free[i])
			pfree(DatumGetPointer(untoasted_value[i]));
	}
#endif

	/*
	 * We do this because DataFill wants to initialize a "tupmask" which
	 * is used for HeapTuples, but we want an indextuple infomask.	The
	 * only relevant info is the "has variable attributes" field.
	 * We have already set the hasnull bit above.
	 */

	if (tupmask & HEAP_HASVARLENA)
		infomask |= INDEX_VAR_MASK;

	/*
	 * Here we make sure that the size will fit in the field reserved for
	 * it in t_info.
	 */

	if ((size & INDEX_SIZE_MASK) != size)
		elog(ERROR, "index_formtuple: data takes %lu bytes: too big",
			(unsigned long)size);

	infomask |= size;

	/* ----------------
	 * initialize metadata
	 * ----------------
	 */
	tuple->t_info = infomask;
	return tuple;
}

/* ----------------
 *		nocache_index_getattr
 *
 *		This gets called from index_getattr() macro, and only in cases
 *		where we can't use cacheoffset and the value is not null.
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
nocache_index_getattr(IndexTuple tup,
					  int attnum,
					  TupleDesc tupleDesc,
					  bool *isnull)
{
	Form_pg_attribute *att = tupleDesc->attrs;
	char	   *tp;				/* ptr to att in tuple */
	bits8	   *bp = NULL;		/* ptr to null bitmask in tuple */
	bool		slow = false;	/* do we have to walk nulls? */
	int			data_off;		/* tuple data offset */

	(void) isnull;				/* not used */
	/* ----------------
	 *	sanity checks
	 * ----------------
	 */

	/* ----------------
	 *	 Three cases:
	 *
	 *	 1: No nulls and no variable length attributes.
	 *	 2: Has a null or a varlena AFTER att.
	 *	 3: Has nulls or varlenas BEFORE att.
	 * ----------------
	 */

#ifdef IN_MACRO
/* This is handled in the macro */
	Assert(PointerIsValid(isnull));
	Assert(attnum > 0);

	*isnull = false;
#endif

	data_off = IndexTupleHasMinHeader(tup) ? sizeof *tup :
		IndexInfoFindDataOffset(tup->t_info);

	attnum--;

	if (IndexTupleNoNulls(tup))
	{
#ifdef IN_MACRO
/* This is handled in the macro */
		if (att[attnum]->attcacheoff != -1)
		{
			return fetchatt(att[attnum],
							(char *) tup + data_off +
							att[attnum]->attcacheoff);
		}
#endif
	}
	else
	{							/* there's a null somewhere in the tuple */
		/* ----------------
		 *		check to see if desired att is null
		 * ----------------
		 */

		/* XXX "knows" t_bits are just after fixed tuple header! */
		bp = (bits8 *) ((char *) tup + sizeof(*tup));

#ifdef IN_MACRO
/* This is handled in the macro */

		if (att_isnull(attnum, bp))
		{
			*isnull = true;
			return (Datum) NULL;
		}
#endif

		/* ----------------
		 *		Now check to see if any preceding bits are null...
		 * ----------------
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

	tp = (char *) tup + data_off;

	/* now check for any non-fixed length attrs before our attribute */

	if (!slow)
	{
		if (att[attnum]->attcacheoff != -1)
		{
			return fetchatt(att[attnum],
							tp + att[attnum]->attcacheoff);
		}
		else if (!IndexTupleAllFixed(tup))
		{
			int			j;

			for (j = 0; j < attnum; j++)
				if (att[j]->attlen <= 0)
				{
					slow = true;
					break;
				}
		}
	}

	/*
	 * If slow is false, and we got here, we know that we have a tuple with
	 * no nulls or varlenas before the target attribute. If possible, we
	 * also want to initialize the remainder of the attribute cached
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

		for (; j <= attnum; j++)
		{

			/*
			 * Fix me when going to a machine with more than a four-byte
			 * word!
			 */

			off = att_align(off, att[j]->attlen, att[j]->attalign);

			att[j]->attcacheoff = off;

			off += att[j]->attlen;
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
		 */

		for (i = 0; i < attnum; i++)
		{
			if (!IndexTupleNoNulls(tup))
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

			if (att[i]->attlen == -1)
			{
				off += VARSIZE(tp + off);
				usecache = false;
			}
			else
			{
				off += att[i]->attlen;
			}
		}

		off = att_align(off, att[attnum]->attlen, att[attnum]->attalign);

		return fetchatt(att[attnum], tp + off);
	}
}

RetrieveIndexResult
FormRetrieveIndexResult(ItemPointer indexItemPointer,
						ItemPointer heapItemPointer)
{
	RetrieveIndexResult result;

	Assert(ItemPointerIsValid(indexItemPointer));
	Assert(ItemPointerIsValid(heapItemPointer));

	result = (RetrieveIndexResult) palloc(sizeof *result);

	result->index_iptr = *indexItemPointer;
	result->heap_iptr = *heapItemPointer;

	return result;
}

/*
 * Copies source into target.  If *target == NULL, we palloc space; otherwise
 * we assume we have space that is already palloc'ed.
 */
void
CopyIndexTuple(IndexTuple source, IndexTuple *target)
{
	Size		size;
	IndexTuple	ret;

	size = IndexTupleSize(source);
	if (*target == NULL)
		*target = (IndexTuple) palloc(size);

	ret = *target;
	memmove((char *) ret, (char *) source, size);
}
