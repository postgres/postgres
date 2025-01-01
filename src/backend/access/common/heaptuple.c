/*-------------------------------------------------------------------------
 *
 * heaptuple.c
 *	  This file contains heap tuple accessor and mutator routines, as well
 *	  as various tuple utilities.
 *
 * Some notes about varlenas and this code:
 *
 * Before Postgres 8.3 varlenas always had a 4-byte length header, and
 * therefore always needed 4-byte alignment (at least).  This wasted space
 * for short varlenas, for example CHAR(1) took 5 bytes and could need up to
 * 3 additional padding bytes for alignment.
 *
 * Now, a short varlena (up to 126 data bytes) is reduced to a 1-byte header
 * and we don't align it.  To hide this from datatype-specific functions that
 * don't want to deal with it, such a datum is considered "toasted" and will
 * be expanded back to the normal 4-byte-header format by pg_detoast_datum.
 * (In performance-critical code paths we can use pg_detoast_datum_packed
 * and the appropriate access macros to avoid that overhead.)  Note that this
 * conversion is performed directly in heap_form_tuple, without invoking
 * heaptoast.c.
 *
 * This change will break any code that assumes it needn't detoast values
 * that have been put into a tuple but never sent to disk.  Hopefully there
 * are few such places.
 *
 * Varlenas still have alignment INT (or DOUBLE) in pg_type/pg_attribute, since
 * that's the normal requirement for the untoasted format.  But we ignore that
 * for the 1-byte-header format.  This means that the actual start position
 * of a varlena datum may vary depending on which format it has.  To determine
 * what is stored, we have to require that alignment padding bytes be zero.
 * (Postgres actually has always zeroed them, but now it's required!)  Since
 * the first byte of a 1-byte-header varlena can never be zero, we can examine
 * the first byte after the previous datum to tell if it's a pad byte or the
 * start of a 1-byte-header varlena.
 *
 * Note that while formerly we could rely on the first varlena column of a
 * system catalog to be at the offset suggested by the C struct for the
 * catalog, this is now risky: it's only safe if the preceding field is
 * word-aligned, so that there will never be any padding.
 *
 * We don't pack varlenas whose attstorage is PLAIN, since the data type
 * isn't expecting to have to detoast values.  This is used in particular
 * by oidvector and int2vector, which are used in the system catalogs
 * and we'd like to still refer to them via C struct offsets.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/heaptuple.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heaptoast.h"
#include "access/sysattr.h"
#include "access/tupdesc_details.h"
#include "common/hashfn.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"


/*
 * Does att's datatype allow packing into the 1-byte-header varlena format?
 * While functions that use TupleDescAttr() and assign attstorage =
 * TYPSTORAGE_PLAIN cannot use packed varlena headers, functions that call
 * TupleDescInitEntry() use typeForm->typstorage (TYPSTORAGE_EXTENDED) and
 * can use packed varlena headers, e.g.:
 *     CREATE TABLE test(a VARCHAR(10000) STORAGE PLAIN);
 *     INSERT INTO test VALUES (repeat('A',10));
 * This can be verified with pageinspect.
 */
#define ATT_IS_PACKABLE(att) \
	((att)->attlen == -1 && (att)->attstorage != TYPSTORAGE_PLAIN)
/* Use this if it's already known varlena */
#define VARLENA_ATT_IS_PACKABLE(att) \
	((att)->attstorage != TYPSTORAGE_PLAIN)

/* FormData_pg_attribute.attstorage != TYPSTORAGE_PLAIN and an attlen of -1 */
#define COMPACT_ATTR_IS_PACKABLE(att) \
	((att)->attlen == -1 && (att)->attispackable)

/*
 * Setup for caching pass-by-ref missing attributes in a way that survives
 * tupleDesc destruction.
 */

typedef struct
{
	int			len;
	Datum		value;
} missing_cache_key;

static HTAB *missing_cache = NULL;

static uint32
missing_hash(const void *key, Size keysize)
{
	const missing_cache_key *entry = (missing_cache_key *) key;

	return hash_bytes((const unsigned char *) entry->value, entry->len);
}

static int
missing_match(const void *key1, const void *key2, Size keysize)
{
	const missing_cache_key *entry1 = (missing_cache_key *) key1;
	const missing_cache_key *entry2 = (missing_cache_key *) key2;

	if (entry1->len != entry2->len)
		return entry1->len > entry2->len ? 1 : -1;

	return memcmp(DatumGetPointer(entry1->value),
				  DatumGetPointer(entry2->value),
				  entry1->len);
}

static void
init_missing_cache()
{
	HASHCTL		hash_ctl;

	hash_ctl.keysize = sizeof(missing_cache_key);
	hash_ctl.entrysize = sizeof(missing_cache_key);
	hash_ctl.hcxt = TopMemoryContext;
	hash_ctl.hash = missing_hash;
	hash_ctl.match = missing_match;
	missing_cache =
		hash_create("Missing Values Cache",
					32,
					&hash_ctl,
					HASH_ELEM | HASH_CONTEXT | HASH_FUNCTION | HASH_COMPARE);
}

/* ----------------------------------------------------------------
 *						misc support routines
 * ----------------------------------------------------------------
 */

/*
 * Return the missing value of an attribute, or NULL if there isn't one.
 */
Datum
getmissingattr(TupleDesc tupleDesc,
			   int attnum, bool *isnull)
{
	CompactAttribute *att;

	Assert(attnum <= tupleDesc->natts);
	Assert(attnum > 0);

	att = TupleDescCompactAttr(tupleDesc, attnum - 1);

	if (att->atthasmissing)
	{
		AttrMissing *attrmiss;

		Assert(tupleDesc->constr);
		Assert(tupleDesc->constr->missing);

		attrmiss = tupleDesc->constr->missing + (attnum - 1);

		if (attrmiss->am_present)
		{
			missing_cache_key key;
			missing_cache_key *entry;
			bool		found;
			MemoryContext oldctx;

			*isnull = false;

			/* no  need to cache by-value attributes */
			if (att->attbyval)
				return attrmiss->am_value;

			/* set up cache if required */
			if (missing_cache == NULL)
				init_missing_cache();

			/* check if there's a cache entry */
			Assert(att->attlen > 0 || att->attlen == -1);
			if (att->attlen > 0)
				key.len = att->attlen;
			else
				key.len = VARSIZE_ANY(attrmiss->am_value);
			key.value = attrmiss->am_value;

			entry = hash_search(missing_cache, &key, HASH_ENTER, &found);

			if (!found)
			{
				/* cache miss, so we need a non-transient copy of the datum */
				oldctx = MemoryContextSwitchTo(TopMemoryContext);
				entry->value =
					datumCopy(attrmiss->am_value, false, att->attlen);
				MemoryContextSwitchTo(oldctx);
			}

			return entry->value;
		}
	}

	*isnull = true;
	return PointerGetDatum(NULL);
}

/*
 * heap_compute_data_size
 *		Determine size of the data area of a tuple to be constructed
 */
Size
heap_compute_data_size(TupleDesc tupleDesc,
					   const Datum *values,
					   const bool *isnull)
{
	Size		data_length = 0;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;

	for (i = 0; i < numberOfAttributes; i++)
	{
		Datum		val;
		CompactAttribute *atti;

		if (isnull[i])
			continue;

		val = values[i];
		atti = TupleDescCompactAttr(tupleDesc, i);

		if (COMPACT_ATTR_IS_PACKABLE(atti) &&
			VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
		{
			/*
			 * we're anticipating converting to a short varlena header, so
			 * adjust length and don't count any alignment
			 */
			data_length += VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(val));
		}
		else if (atti->attlen == -1 &&
				 VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			/*
			 * we want to flatten the expanded value so that the constructed
			 * tuple doesn't depend on it
			 */
			data_length = att_nominal_alignby(data_length, atti->attalignby);
			data_length += EOH_get_flat_size(DatumGetEOHP(val));
		}
		else
		{
			data_length = att_datum_alignby(data_length, atti->attalignby,
											atti->attlen, val);
			data_length = att_addlength_datum(data_length, atti->attlen,
											  val);
		}
	}

	return data_length;
}

/*
 * Per-attribute helper for heap_fill_tuple and other routines building tuples.
 *
 * Fill in either a data value or a bit in the null bitmask
 */
static inline void
fill_val(CompactAttribute *att,
		 bits8 **bit,
		 int *bitmask,
		 char **dataP,
		 uint16 *infomask,
		 Datum datum,
		 bool isnull)
{
	Size		data_length;
	char	   *data = *dataP;

	/*
	 * If we're building a null bitmap, set the appropriate bit for the
	 * current column value here.
	 */
	if (bit != NULL)
	{
		if (*bitmask != HIGHBIT)
			*bitmask <<= 1;
		else
		{
			*bit += 1;
			**bit = 0x0;
			*bitmask = 1;
		}

		if (isnull)
		{
			*infomask |= HEAP_HASNULL;
			return;
		}

		**bit |= *bitmask;
	}

	/*
	 * XXX we use the att_nominal_alignby macro on the pointer value itself,
	 * not on an offset.  This is a bit of a hack.
	 */
	if (att->attbyval)
	{
		/* pass-by-value */
		data = (char *) att_nominal_alignby(data, att->attalignby);
		store_att_byval(data, datum, att->attlen);
		data_length = att->attlen;
	}
	else if (att->attlen == -1)
	{
		/* varlena */
		Pointer		val = DatumGetPointer(datum);

		*infomask |= HEAP_HASVARWIDTH;
		if (VARATT_IS_EXTERNAL(val))
		{
			if (VARATT_IS_EXTERNAL_EXPANDED(val))
			{
				/*
				 * we want to flatten the expanded value so that the
				 * constructed tuple doesn't depend on it
				 */
				ExpandedObjectHeader *eoh = DatumGetEOHP(datum);

				data = (char *) att_nominal_alignby(data, att->attalignby);
				data_length = EOH_get_flat_size(eoh);
				EOH_flatten_into(eoh, data, data_length);
			}
			else
			{
				*infomask |= HEAP_HASEXTERNAL;
				/* no alignment, since it's short by definition */
				data_length = VARSIZE_EXTERNAL(val);
				memcpy(data, val, data_length);
			}
		}
		else if (VARATT_IS_SHORT(val))
		{
			/* no alignment for short varlenas */
			data_length = VARSIZE_SHORT(val);
			memcpy(data, val, data_length);
		}
		else if (att->attispackable && VARATT_CAN_MAKE_SHORT(val))
		{
			/* convert to short varlena -- no alignment */
			data_length = VARATT_CONVERTED_SHORT_SIZE(val);
			SET_VARSIZE_SHORT(data, data_length);
			memcpy(data + 1, VARDATA(val), data_length - 1);
		}
		else
		{
			/* full 4-byte header varlena */
			data = (char *) att_nominal_alignby(data, att->attalignby);
			data_length = VARSIZE(val);
			memcpy(data, val, data_length);
		}
	}
	else if (att->attlen == -2)
	{
		/* cstring ... never needs alignment */
		*infomask |= HEAP_HASVARWIDTH;
		Assert(att->attalignby == sizeof(char));
		data_length = strlen(DatumGetCString(datum)) + 1;
		memcpy(data, DatumGetPointer(datum), data_length);
	}
	else
	{
		/* fixed-length pass-by-reference */
		data = (char *) att_nominal_alignby(data, att->attalignby);
		Assert(att->attlen > 0);
		data_length = att->attlen;
		memcpy(data, DatumGetPointer(datum), data_length);
	}

	data += data_length;
	*dataP = data;
}

/*
 * heap_fill_tuple
 *		Load data portion of a tuple from values/isnull arrays
 *
 * We also fill the null bitmap (if any) and set the infomask bits
 * that reflect the tuple's data contents.
 *
 * NOTE: it is now REQUIRED that the caller have pre-zeroed the data area.
 */
void
heap_fill_tuple(TupleDesc tupleDesc,
				const Datum *values, const bool *isnull,
				char *data, Size data_size,
				uint16 *infomask, bits8 *bit)
{
	bits8	   *bitP;
	int			bitmask;
	int			i;
	int			numberOfAttributes = tupleDesc->natts;

#ifdef USE_ASSERT_CHECKING
	char	   *start = data;
#endif

	if (bit != NULL)
	{
		bitP = &bit[-1];
		bitmask = HIGHBIT;
	}
	else
	{
		/* just to keep compiler quiet */
		bitP = NULL;
		bitmask = 0;
	}

	*infomask &= ~(HEAP_HASNULL | HEAP_HASVARWIDTH | HEAP_HASEXTERNAL);

	for (i = 0; i < numberOfAttributes; i++)
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupleDesc, i);

		fill_val(attr,
				 bitP ? &bitP : NULL,
				 &bitmask,
				 &data,
				 infomask,
				 values ? values[i] : PointerGetDatum(NULL),
				 isnull ? isnull[i] : true);
	}

	Assert((data - start) == data_size);
}


/* ----------------------------------------------------------------
 *						heap tuple interface
 * ----------------------------------------------------------------
 */

/* ----------------
 *		heap_attisnull	- returns true iff tuple attribute is not present
 * ----------------
 */
bool
heap_attisnull(HeapTuple tup, int attnum, TupleDesc tupleDesc)
{
	/*
	 * We allow a NULL tupledesc for relations not expected to have missing
	 * values, such as catalog relations and indexes.
	 */
	Assert(!tupleDesc || attnum <= tupleDesc->natts);
	if (attnum > (int) HeapTupleHeaderGetNatts(tup->t_data))
	{
		if (tupleDesc &&
			TupleDescCompactAttr(tupleDesc, attnum - 1)->atthasmissing)
			return false;
		else
			return true;
	}

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
 *		This only gets called from fastgetattr(), in cases where we
 *		can't use a cacheoffset and the value is not null.
 *
 *		This caches attribute offsets in the attribute descriptor.
 *
 *		An alternative way to speed things up would be to cache offsets
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
 *		Also see nocache_index_getattr, which is the same code for index
 *		tuples.
 * ----------------
 */
Datum
nocachegetattr(HeapTuple tup,
			   int attnum,
			   TupleDesc tupleDesc)
{
	HeapTupleHeader td = tup->t_data;
	char	   *tp;				/* ptr to data part of tuple */
	bits8	   *bp = td->t_bits;	/* ptr to null bitmap in tuple */
	bool		slow = false;	/* do we have to walk attrs? */
	int			off;			/* current offset within data */

	/* ----------------
	 *	 Three cases:
	 *
	 *	 1: No nulls and no variable-width attributes.
	 *	 2: Has a null or a var-width AFTER att.
	 *	 3: Has nulls or var-widths BEFORE att.
	 * ----------------
	 */

	attnum--;

	if (!HeapTupleNoNulls(tup))
	{
		/*
		 * there's a null somewhere in the tuple
		 *
		 * check to see if any preceding bits are null...
		 */
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

	tp = (char *) td + td->t_hoff;

	if (!slow)
	{
		CompactAttribute *att;

		/*
		 * If we get here, there are no nulls up to and including the target
		 * attribute.  If we have a cached offset, we can use it.
		 */
		att = TupleDescCompactAttr(tupleDesc, attnum);
		if (att->attcacheoff >= 0)
			return fetchatt(att, tp + att->attcacheoff);

		/*
		 * Otherwise, check for non-fixed-length attrs up to and including
		 * target.  If there aren't any, it's safe to cheaply initialize the
		 * cached offsets for these attrs.
		 */
		if (HeapTupleHasVarWidth(tup))
		{
			int			j;

			for (j = 0; j <= attnum; j++)
			{
				if (TupleDescCompactAttr(tupleDesc, j)->attlen <= 0)
				{
					slow = true;
					break;
				}
			}
		}
	}

	if (!slow)
	{
		int			natts = tupleDesc->natts;
		int			j = 1;

		/*
		 * If we get here, we have a tuple with no nulls or var-widths up to
		 * and including the target attribute, so we can use the cached offset
		 * ... only we don't have it yet, or we'd not have got here.  Since
		 * it's cheap to compute offsets for fixed-width columns, we take the
		 * opportunity to initialize the cached offsets for *all* the leading
		 * fixed-width columns, in hope of avoiding future visits to this
		 * routine.
		 */
		TupleDescCompactAttr(tupleDesc, 0)->attcacheoff = 0;

		/* we might have set some offsets in the slow path previously */
		while (j < natts && TupleDescCompactAttr(tupleDesc, j)->attcacheoff > 0)
			j++;

		off = TupleDescCompactAttr(tupleDesc, j - 1)->attcacheoff +
			TupleDescCompactAttr(tupleDesc, j - 1)->attlen;

		for (; j < natts; j++)
		{
			CompactAttribute *att = TupleDescCompactAttr(tupleDesc, j);

			if (att->attlen <= 0)
				break;

			off = att_nominal_alignby(off, att->attalignby);

			att->attcacheoff = off;

			off += att->attlen;
		}

		Assert(j > attnum);

		off = TupleDescCompactAttr(tupleDesc, attnum)->attcacheoff;
	}
	else
	{
		bool		usecache = true;
		int			i;

		/*
		 * Now we know that we have to walk the tuple CAREFULLY.  But we still
		 * might be able to cache some offsets for next time.
		 *
		 * Note - This loop is a little tricky.  For each non-null attribute,
		 * we have to first account for alignment padding before the attr,
		 * then advance over the attr based on its length.  Nulls have no
		 * storage and no alignment padding either.  We can use/set
		 * attcacheoff until we reach either a null or a var-width attribute.
		 */
		off = 0;
		for (i = 0;; i++)		/* loop exit is at "break" */
		{
			CompactAttribute *att = TupleDescCompactAttr(tupleDesc, i);

			if (HeapTupleHasNulls(tup) && att_isnull(i, bp))
			{
				usecache = false;
				continue;		/* this cannot be the target att */
			}

			/* If we know the next offset, we can skip the rest */
			if (usecache && att->attcacheoff >= 0)
				off = att->attcacheoff;
			else if (att->attlen == -1)
			{
				/*
				 * We can only cache the offset for a varlena attribute if the
				 * offset is already suitably aligned, so that there would be
				 * no pad bytes in any case: then the offset will be valid for
				 * either an aligned or unaligned value.
				 */
				if (usecache &&
					off == att_nominal_alignby(off, att->attalignby))
					att->attcacheoff = off;
				else
				{
					off = att_pointer_alignby(off, att->attalignby, -1,
											  tp + off);
					usecache = false;
				}
			}
			else
			{
				/* not varlena, so safe to use att_nominal_alignby */
				off = att_nominal_alignby(off, att->attalignby);

				if (usecache)
					att->attcacheoff = off;
			}

			if (i == attnum)
				break;

			off = att_addlength_pointer(off, att->attlen, tp + off);

			if (usecache && att->attlen <= 0)
				usecache = false;
		}
	}

	return fetchatt(TupleDescCompactAttr(tupleDesc, attnum), tp + off);
}

/* ----------------
 *		heap_getsysattr
 *
 *		Fetch the value of a system attribute for a tuple.
 *
 * This is a support routine for heap_getattr().  The function has already
 * determined that the attnum refers to a system attribute.
 * ----------------
 */
Datum
heap_getsysattr(HeapTuple tup, int attnum, TupleDesc tupleDesc, bool *isnull)
{
	Datum		result;

	Assert(tup);

	/* Currently, no sys attribute ever reads as NULL. */
	*isnull = false;

	switch (attnum)
	{
		case SelfItemPointerAttributeNumber:
			/* pass-by-reference datatype */
			result = PointerGetDatum(&(tup->t_self));
			break;
		case MinTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(HeapTupleHeaderGetRawXmin(tup->t_data));
			break;
		case MaxTransactionIdAttributeNumber:
			result = TransactionIdGetDatum(HeapTupleHeaderGetRawXmax(tup->t_data));
			break;
		case MinCommandIdAttributeNumber:
		case MaxCommandIdAttributeNumber:

			/*
			 * cmin and cmax are now both aliases for the same field, which
			 * can in fact also be a combo command id.  XXX perhaps we should
			 * return the "real" cmin or cmax if possible, that is if we are
			 * inside the originating transaction?
			 */
			result = CommandIdGetDatum(HeapTupleHeaderGetRawCommandId(tup->t_data));
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
	newTuple->t_data = (HeapTupleHeader) ((char *) newTuple + HEAPTUPLESIZE);
	memcpy((char *) newTuple->t_data, (char *) tuple->t_data, tuple->t_len);
	return newTuple;
}

/* ----------------
 *		heap_copytuple_with_tuple
 *
 *		copy a tuple into a caller-supplied HeapTuple management struct
 *
 * Note that after calling this function, the "dest" HeapTuple will not be
 * allocated as a single palloc() block (unlike with heap_copytuple()).
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
	dest->t_data = (HeapTupleHeader) palloc(src->t_len);
	memcpy((char *) dest->t_data, (char *) src->t_data, src->t_len);
}

/*
 * Expand a tuple which has fewer attributes than required. For each attribute
 * not present in the sourceTuple, if there is a missing value that will be
 * used. Otherwise the attribute will be set to NULL.
 *
 * The source tuple must have fewer attributes than the required number.
 *
 * Only one of targetHeapTuple and targetMinimalTuple may be supplied. The
 * other argument must be NULL.
 */
static void
expand_tuple(HeapTuple *targetHeapTuple,
			 MinimalTuple *targetMinimalTuple,
			 HeapTuple sourceTuple,
			 TupleDesc tupleDesc)
{
	AttrMissing *attrmiss = NULL;
	int			attnum;
	int			firstmissingnum;
	bool		hasNulls = HeapTupleHasNulls(sourceTuple);
	HeapTupleHeader targetTHeader;
	HeapTupleHeader sourceTHeader = sourceTuple->t_data;
	int			sourceNatts = HeapTupleHeaderGetNatts(sourceTHeader);
	int			natts = tupleDesc->natts;
	int			sourceNullLen;
	int			targetNullLen;
	Size		sourceDataLen = sourceTuple->t_len - sourceTHeader->t_hoff;
	Size		targetDataLen;
	Size		len;
	int			hoff;
	bits8	   *nullBits = NULL;
	int			bitMask = 0;
	char	   *targetData;
	uint16	   *infoMask;

	Assert((targetHeapTuple && !targetMinimalTuple)
		   || (!targetHeapTuple && targetMinimalTuple));

	Assert(sourceNatts < natts);

	sourceNullLen = (hasNulls ? BITMAPLEN(sourceNatts) : 0);

	targetDataLen = sourceDataLen;

	if (tupleDesc->constr &&
		tupleDesc->constr->missing)
	{
		/*
		 * If there are missing values we want to put them into the tuple.
		 * Before that we have to compute the extra length for the values
		 * array and the variable length data.
		 */
		attrmiss = tupleDesc->constr->missing;

		/*
		 * Find the first item in attrmiss for which we don't have a value in
		 * the source. We can ignore all the missing entries before that.
		 */
		for (firstmissingnum = sourceNatts;
			 firstmissingnum < natts;
			 firstmissingnum++)
		{
			if (attrmiss[firstmissingnum].am_present)
				break;
			else
				hasNulls = true;
		}

		/*
		 * Now walk the missing attributes. If there is a missing value make
		 * space for it. Otherwise, it's going to be NULL.
		 */
		for (attnum = firstmissingnum;
			 attnum < natts;
			 attnum++)
		{
			if (attrmiss[attnum].am_present)
			{
				CompactAttribute *att = TupleDescCompactAttr(tupleDesc, attnum);

				targetDataLen = att_datum_alignby(targetDataLen,
												  att->attalignby,
												  att->attlen,
												  attrmiss[attnum].am_value);

				targetDataLen = att_addlength_pointer(targetDataLen,
													  att->attlen,
													  attrmiss[attnum].am_value);
			}
			else
			{
				/* no missing value, so it must be null */
				hasNulls = true;
			}
		}
	}							/* end if have missing values */
	else
	{
		/*
		 * If there are no missing values at all then NULLS must be allowed,
		 * since some of the attributes are known to be absent.
		 */
		hasNulls = true;
	}

	len = 0;

	if (hasNulls)
	{
		targetNullLen = BITMAPLEN(natts);
		len += targetNullLen;
	}
	else
		targetNullLen = 0;

	/*
	 * Allocate and zero the space needed.  Note that the tuple body and
	 * HeapTupleData management structure are allocated in one chunk.
	 */
	if (targetHeapTuple)
	{
		len += offsetof(HeapTupleHeaderData, t_bits);
		hoff = len = MAXALIGN(len); /* align user data safely */
		len += targetDataLen;

		*targetHeapTuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
		(*targetHeapTuple)->t_data
			= targetTHeader
			= (HeapTupleHeader) ((char *) *targetHeapTuple + HEAPTUPLESIZE);
		(*targetHeapTuple)->t_len = len;
		(*targetHeapTuple)->t_tableOid = sourceTuple->t_tableOid;
		(*targetHeapTuple)->t_self = sourceTuple->t_self;

		targetTHeader->t_infomask = sourceTHeader->t_infomask;
		targetTHeader->t_hoff = hoff;
		HeapTupleHeaderSetNatts(targetTHeader, natts);
		HeapTupleHeaderSetDatumLength(targetTHeader, len);
		HeapTupleHeaderSetTypeId(targetTHeader, tupleDesc->tdtypeid);
		HeapTupleHeaderSetTypMod(targetTHeader, tupleDesc->tdtypmod);
		/* We also make sure that t_ctid is invalid unless explicitly set */
		ItemPointerSetInvalid(&(targetTHeader->t_ctid));
		if (targetNullLen > 0)
			nullBits = (bits8 *) ((char *) (*targetHeapTuple)->t_data
								  + offsetof(HeapTupleHeaderData, t_bits));
		targetData = (char *) (*targetHeapTuple)->t_data + hoff;
		infoMask = &(targetTHeader->t_infomask);
	}
	else
	{
		len += SizeofMinimalTupleHeader;
		hoff = len = MAXALIGN(len); /* align user data safely */
		len += targetDataLen;

		*targetMinimalTuple = (MinimalTuple) palloc0(len);
		(*targetMinimalTuple)->t_len = len;
		(*targetMinimalTuple)->t_hoff = hoff + MINIMAL_TUPLE_OFFSET;
		(*targetMinimalTuple)->t_infomask = sourceTHeader->t_infomask;
		/* Same macro works for MinimalTuples */
		HeapTupleHeaderSetNatts(*targetMinimalTuple, natts);
		if (targetNullLen > 0)
			nullBits = (bits8 *) ((char *) *targetMinimalTuple
								  + offsetof(MinimalTupleData, t_bits));
		targetData = (char *) *targetMinimalTuple + hoff;
		infoMask = &((*targetMinimalTuple)->t_infomask);
	}

	if (targetNullLen > 0)
	{
		if (sourceNullLen > 0)
		{
			/* if bitmap pre-existed copy in - all is set */
			memcpy(nullBits,
				   ((char *) sourceTHeader)
				   + offsetof(HeapTupleHeaderData, t_bits),
				   sourceNullLen);
			nullBits += sourceNullLen - 1;
		}
		else
		{
			sourceNullLen = BITMAPLEN(sourceNatts);
			/* Set NOT NULL for all existing attributes */
			memset(nullBits, 0xff, sourceNullLen);

			nullBits += sourceNullLen - 1;

			if (sourceNatts & 0x07)
			{
				/* build the mask (inverted!) */
				bitMask = 0xff << (sourceNatts & 0x07);
				/* Voila */
				*nullBits = ~bitMask;
			}
		}

		bitMask = (1 << ((sourceNatts - 1) & 0x07));
	}							/* End if have null bitmap */

	memcpy(targetData,
		   ((char *) sourceTuple->t_data) + sourceTHeader->t_hoff,
		   sourceDataLen);

	targetData += sourceDataLen;

	/* Now fill in the missing values */
	for (attnum = sourceNatts; attnum < natts; attnum++)
	{
		CompactAttribute *attr = TupleDescCompactAttr(tupleDesc, attnum);

		if (attrmiss && attrmiss[attnum].am_present)
		{
			fill_val(attr,
					 nullBits ? &nullBits : NULL,
					 &bitMask,
					 &targetData,
					 infoMask,
					 attrmiss[attnum].am_value,
					 false);
		}
		else
		{
			fill_val(attr,
					 &nullBits,
					 &bitMask,
					 &targetData,
					 infoMask,
					 (Datum) 0,
					 true);
		}
	}							/* end loop over missing attributes */
}

/*
 * Fill in the missing values for a minimal HeapTuple
 */
MinimalTuple
minimal_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc)
{
	MinimalTuple minimalTuple;

	expand_tuple(NULL, &minimalTuple, sourceTuple, tupleDesc);
	return minimalTuple;
}

/*
 * Fill in the missing values for an ordinary HeapTuple
 */
HeapTuple
heap_expand_tuple(HeapTuple sourceTuple, TupleDesc tupleDesc)
{
	HeapTuple	heapTuple;

	expand_tuple(&heapTuple, NULL, sourceTuple, tupleDesc);
	return heapTuple;
}

/* ----------------
 *		heap_copy_tuple_as_datum
 *
 *		copy a tuple as a composite-type Datum
 * ----------------
 */
Datum
heap_copy_tuple_as_datum(HeapTuple tuple, TupleDesc tupleDesc)
{
	HeapTupleHeader td;

	/*
	 * If the tuple contains any external TOAST pointers, we have to inline
	 * those fields to meet the conventions for composite-type Datums.
	 */
	if (HeapTupleHasExternal(tuple))
		return toast_flatten_tuple_to_datum(tuple->t_data,
											tuple->t_len,
											tupleDesc);

	/*
	 * Fast path for easy case: just make a palloc'd copy and insert the
	 * correct composite-Datum header fields (since those may not be set if
	 * the given tuple came from disk, rather than from heap_form_tuple).
	 */
	td = (HeapTupleHeader) palloc(tuple->t_len);
	memcpy((char *) td, (char *) tuple->t_data, tuple->t_len);

	HeapTupleHeaderSetDatumLength(td, tuple->t_len);
	HeapTupleHeaderSetTypeId(td, tupleDesc->tdtypeid);
	HeapTupleHeaderSetTypMod(td, tupleDesc->tdtypmod);

	return PointerGetDatum(td);
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
				const Datum *values,
				const bool *isnull)
{
	HeapTuple	tuple;			/* return tuple */
	HeapTupleHeader td;			/* tuple data */
	Size		len,
				data_len;
	int			hoff;
	bool		hasnull = false;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	if (numberOfAttributes > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						numberOfAttributes, MaxTupleAttributeNumber)));

	/*
	 * Check for nulls
	 */
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	/*
	 * Determine total space needed
	 */
	len = offsetof(HeapTupleHeaderData, t_bits);

	if (hasnull)
		len += BITMAPLEN(numberOfAttributes);

	hoff = len = MAXALIGN(len); /* align user data safely */

	data_len = heap_compute_data_size(tupleDescriptor, values, isnull);

	len += data_len;

	/*
	 * Allocate and zero the space needed.  Note that the tuple body and
	 * HeapTupleData management structure are allocated in one chunk.
	 */
	tuple = (HeapTuple) palloc0(HEAPTUPLESIZE + len);
	tuple->t_data = td = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);

	/*
	 * And fill in the information.  Note we fill the Datum fields even though
	 * this tuple may never become a Datum.  This lets HeapTupleHeaderGetDatum
	 * identify the tuple type if needed.
	 */
	tuple->t_len = len;
	ItemPointerSetInvalid(&(tuple->t_self));
	tuple->t_tableOid = InvalidOid;

	HeapTupleHeaderSetDatumLength(td, len);
	HeapTupleHeaderSetTypeId(td, tupleDescriptor->tdtypeid);
	HeapTupleHeaderSetTypMod(td, tupleDescriptor->tdtypmod);
	/* We also make sure that t_ctid is invalid unless explicitly set */
	ItemPointerSetInvalid(&(td->t_ctid));

	HeapTupleHeaderSetNatts(td, numberOfAttributes);
	td->t_hoff = hoff;

	heap_fill_tuple(tupleDescriptor,
					values,
					isnull,
					(char *) td + hoff,
					data_len,
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
				  const Datum *replValues,
				  const bool *replIsnull,
				  const bool *doReplace)
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
	 * heap_getattr() only the non-replaced columns.  The latter could win if
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
	 * copy the identification info of the old tuple: t_ctid, t_self
	 */
	newTuple->t_data->t_ctid = tuple->t_data->t_ctid;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;

	return newTuple;
}

/*
 * heap_modify_tuple_by_cols
 *		form a new tuple from an old tuple and a set of replacement values.
 *
 * This is like heap_modify_tuple, except that instead of specifying which
 * column(s) to replace by a boolean map, an array of target column numbers
 * is used.  This is often more convenient when a fixed number of columns
 * are to be replaced.  The replCols, replValues, and replIsnull arrays must
 * be of length nCols.  Target column numbers are indexed from 1.
 *
 * The result is allocated in the current memory context.
 */
HeapTuple
heap_modify_tuple_by_cols(HeapTuple tuple,
						  TupleDesc tupleDesc,
						  int nCols,
						  const int *replCols,
						  const Datum *replValues,
						  const bool *replIsnull)
{
	int			numberOfAttributes = tupleDesc->natts;
	Datum	   *values;
	bool	   *isnull;
	HeapTuple	newTuple;
	int			i;

	/*
	 * allocate and fill values and isnull arrays from the tuple, then replace
	 * selected columns from the input arrays.
	 */
	values = (Datum *) palloc(numberOfAttributes * sizeof(Datum));
	isnull = (bool *) palloc(numberOfAttributes * sizeof(bool));

	heap_deform_tuple(tuple, tupleDesc, values, isnull);

	for (i = 0; i < nCols; i++)
	{
		int			attnum = replCols[i];

		if (attnum <= 0 || attnum > numberOfAttributes)
			elog(ERROR, "invalid column number %d", attnum);
		values[attnum - 1] = replValues[i];
		isnull[attnum - 1] = replIsnull[i];
	}

	/*
	 * create a new tuple from the values and isnull arrays
	 */
	newTuple = heap_form_tuple(tupleDesc, values, isnull);

	pfree(values);
	pfree(isnull);

	/*
	 * copy the identification info of the old tuple: t_ctid, t_self
	 */
	newTuple->t_data->t_ctid = tuple->t_data->t_ctid;
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;

	return newTuple;
}

/*
 * heap_deform_tuple
 *		Given a tuple, extract data into values/isnull arrays; this is
 *		the inverse of heap_form_tuple.
 *
 *		Storage for the values/isnull arrays is provided by the caller;
 *		it should be sized according to tupleDesc->natts not
 *		HeapTupleHeaderGetNatts(tuple->t_data).
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
	int			tdesc_natts = tupleDesc->natts;
	int			natts;			/* number of atts to extract */
	int			attnum;
	char	   *tp;				/* ptr to tuple data */
	uint32		off;			/* offset in tuple data */
	bits8	   *bp = tup->t_bits;	/* ptr to null bitmap in tuple */
	bool		slow = false;	/* can we use/set attcacheoff? */

	natts = HeapTupleHeaderGetNatts(tup);

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
		CompactAttribute *thisatt = TupleDescCompactAttr(tupleDesc, attnum);

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
		else if (thisatt->attlen == -1)
		{
			/*
			 * We can only cache the offset for a varlena attribute if the
			 * offset is already suitably aligned, so that there would be no
			 * pad bytes in any case: then the offset will be valid for either
			 * an aligned or unaligned value.
			 */
			if (!slow &&
				off == att_nominal_alignby(off, thisatt->attalignby))
				thisatt->attcacheoff = off;
			else
			{
				off = att_pointer_alignby(off, thisatt->attalignby, -1,
										  tp + off);
				slow = true;
			}
		}
		else
		{
			/* not varlena, so safe to use att_nominal_alignby */
			off = att_nominal_alignby(off, thisatt->attalignby);

			if (!slow)
				thisatt->attcacheoff = off;
		}

		values[attnum] = fetchatt(thisatt, tp + off);

		off = att_addlength_pointer(off, thisatt->attlen, tp + off);

		if (thisatt->attlen <= 0)
			slow = true;		/* can't use attcacheoff anymore */
	}

	/*
	 * If tuple doesn't have all the atts indicated by tupleDesc, read the
	 * rest as nulls or missing values as appropriate.
	 */
	for (; attnum < tdesc_natts; attnum++)
		values[attnum] = getmissingattr(tupleDesc, attnum + 1, &isnull[attnum]);
}

/*
 * heap_freetuple
 */
void
heap_freetuple(HeapTuple htup)
{
	pfree(htup);
}


/*
 * heap_form_minimal_tuple
 *		construct a MinimalTuple from the given values[] and isnull[] arrays,
 *		which are of the length indicated by tupleDescriptor->natts
 *
 * This is exactly like heap_form_tuple() except that the result is a
 * "minimal" tuple lacking a HeapTupleData header as well as room for system
 * columns.
 *
 * The result is allocated in the current memory context.
 */
MinimalTuple
heap_form_minimal_tuple(TupleDesc tupleDescriptor,
						const Datum *values,
						const bool *isnull)
{
	MinimalTuple tuple;			/* return tuple */
	Size		len,
				data_len;
	int			hoff;
	bool		hasnull = false;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	if (numberOfAttributes > MaxTupleAttributeNumber)
		ereport(ERROR,
				(errcode(ERRCODE_TOO_MANY_COLUMNS),
				 errmsg("number of columns (%d) exceeds limit (%d)",
						numberOfAttributes, MaxTupleAttributeNumber)));

	/*
	 * Check for nulls
	 */
	for (i = 0; i < numberOfAttributes; i++)
	{
		if (isnull[i])
		{
			hasnull = true;
			break;
		}
	}

	/*
	 * Determine total space needed
	 */
	len = SizeofMinimalTupleHeader;

	if (hasnull)
		len += BITMAPLEN(numberOfAttributes);

	hoff = len = MAXALIGN(len); /* align user data safely */

	data_len = heap_compute_data_size(tupleDescriptor, values, isnull);

	len += data_len;

	/*
	 * Allocate and zero the space needed.
	 */
	tuple = (MinimalTuple) palloc0(len);

	/*
	 * And fill in the information.
	 */
	tuple->t_len = len;
	HeapTupleHeaderSetNatts(tuple, numberOfAttributes);
	tuple->t_hoff = hoff + MINIMAL_TUPLE_OFFSET;

	heap_fill_tuple(tupleDescriptor,
					values,
					isnull,
					(char *) tuple + hoff,
					data_len,
					&tuple->t_infomask,
					(hasnull ? tuple->t_bits : NULL));

	return tuple;
}

/*
 * heap_free_minimal_tuple
 */
void
heap_free_minimal_tuple(MinimalTuple mtup)
{
	pfree(mtup);
}

/*
 * heap_copy_minimal_tuple
 *		copy a MinimalTuple
 *
 * The result is allocated in the current memory context.
 */
MinimalTuple
heap_copy_minimal_tuple(MinimalTuple mtup)
{
	MinimalTuple result;

	result = (MinimalTuple) palloc(mtup->t_len);
	memcpy(result, mtup, mtup->t_len);
	return result;
}

/*
 * heap_tuple_from_minimal_tuple
 *		create a HeapTuple by copying from a MinimalTuple;
 *		system columns are filled with zeroes
 *
 * The result is allocated in the current memory context.
 * The HeapTuple struct, tuple header, and tuple data are all allocated
 * as a single palloc() block.
 */
HeapTuple
heap_tuple_from_minimal_tuple(MinimalTuple mtup)
{
	HeapTuple	result;
	uint32		len = mtup->t_len + MINIMAL_TUPLE_OFFSET;

	result = (HeapTuple) palloc(HEAPTUPLESIZE + len);
	result->t_len = len;
	ItemPointerSetInvalid(&(result->t_self));
	result->t_tableOid = InvalidOid;
	result->t_data = (HeapTupleHeader) ((char *) result + HEAPTUPLESIZE);
	memcpy((char *) result->t_data + MINIMAL_TUPLE_OFFSET, mtup, mtup->t_len);
	memset(result->t_data, 0, offsetof(HeapTupleHeaderData, t_infomask2));
	return result;
}

/*
 * minimal_tuple_from_heap_tuple
 *		create a MinimalTuple by copying from a HeapTuple
 *
 * The result is allocated in the current memory context.
 */
MinimalTuple
minimal_tuple_from_heap_tuple(HeapTuple htup)
{
	MinimalTuple result;
	uint32		len;

	Assert(htup->t_len > MINIMAL_TUPLE_OFFSET);
	len = htup->t_len - MINIMAL_TUPLE_OFFSET;
	result = (MinimalTuple) palloc(len);
	memcpy(result, (char *) htup->t_data + MINIMAL_TUPLE_OFFSET, len);
	result->t_len = len;
	return result;
}

/*
 * This mainly exists so JIT can inline the definition, but it's also
 * sometimes useful in debugging sessions.
 */
size_t
varsize_any(void *p)
{
	return VARSIZE_ANY(p);
}
