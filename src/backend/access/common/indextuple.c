/*-------------------------------------------------------------------------
 *
 * indextuple.c
 *	   This file contains index tuple accessor and mutator routines,
 *	   as well as various tuple utilities.
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header: /cvsroot/pgsql/src/backend/access/common/indextuple.c,v 1.68 2003/09/25 06:57:56 petere Exp $
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
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of index columns (%d) exceeds limit (%d)",
						numberOfAttributes, INDEX_MAX_KEYS)));

#ifdef TOAST_INDEX_HACK
	for (i = 0; i < numberOfAttributes; i++)
	{
		Form_pg_attribute att = tupleDescriptor->attrs[i];

		untoasted_value[i] = value[i];
		untoasted_free[i] = false;

		/* Do nothing if value is NULL or not of varlena type */
		if (null[i] != ' ' || att->attlen != -1)
			continue;

		/*
		 * If value is stored EXTERNAL, must fetch it so we are not
		 * depending on outside storage.  This should be improved someday.
		 */
		if (VARATT_IS_EXTERNAL(value[i]))
		{
			untoasted_value[i] = PointerGetDatum(
												 heap_tuple_fetch_attr(
							   (varattrib *) DatumGetPointer(value[i])));
			untoasted_free[i] = true;
		}

		/*
		 * If value is above size target, and is of a compressible
		 * datatype, try to compress it in-line.
		 */
		if (VARATT_SIZE(untoasted_value[i]) > TOAST_INDEX_TARGET &&
			!VARATT_IS_EXTENDED(untoasted_value[i]) &&
			(att->attstorage == 'x' || att->attstorage == 'm'))
		{
			Datum		cvalue = toast_compress_datum(untoasted_value[i]);

			if (DatumGetPointer(cvalue) != NULL)
			{
				/* successful compression */
				if (untoasted_free[i])
					pfree(DatumGetPointer(untoasted_value[i]));
				untoasted_value[i] = cvalue;
				untoasted_free[i] = true;
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

	tp = (char *) palloc0(size);
	tuple = (IndexTuple) tp;

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
	 * only relevant info is the "has variable attributes" field. We have
	 * already set the hasnull bit above.
	 */

	if (tupmask & HEAP_HASVARWIDTH)
		infomask |= INDEX_VAR_MASK;

	/*
	 * Here we make sure that the size will fit in the field reserved for
	 * it in t_info.
	 */
	if ((size & INDEX_SIZE_MASK) != size)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			errmsg("index row requires %lu bytes, maximum size is %lu",
				   (unsigned long) size,
				   (unsigned long) INDEX_SIZE_MASK)));

	infomask |= size;

	/*
	 * initialize metadata
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

	/*
	 * sanity checks
	 */

	/* ----------------
	 *	 Three cases:
	 *
	 *	 1: No nulls and no variable-width attributes.
	 *	 2: Has a null or a var-width AFTER att.
	 *	 3: Has nulls or var-widths BEFORE att.
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

	if (!IndexTupleHasNulls(tup))
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
	{
		/*
		 * there's a null somewhere in the tuple
		 *
		 * check to see if desired att is null
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

	tp = (char *) tup + data_off;

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
		else if (IndexTupleHasVarwidths(tup))
		{
			int			j;

			for (j = 0; j < attnum; j++)
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
	 * with no nulls or var-widths before the target attribute. If
	 * possible, we also want to initialize the remainder of the attribute
	 * cached offset values.
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
			off = att_align(off, att[j]->attalign);

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
			if (IndexTupleHasNulls(tup))
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

/*
 * Create a palloc'd copy of an index tuple.
 */
IndexTuple
CopyIndexTuple(IndexTuple source)
{
	IndexTuple	result;
	Size		size;

	size = IndexTupleSize(source);
	result = (IndexTuple) palloc(size);
	memcpy(result, source, size);
	return result;
}
