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
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
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
#include "access/xlogdefs.h"
#include "catalog/pg_type_d.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/expandeddatum.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/typcache.h"


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


static void heap_datum_check_coercion(HeapTupleHeader tup, TupleDesc targetDesc);


/* GUC variable */
int			composite_type_mismatch = COMPOSITE_TYPE_MISMATCH_ERROR;


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
	const missing_cache_key *entry = key;

	return hash_bytes((const unsigned char *) DatumGetPointer(entry->value), entry->len);
}

static int
missing_match(const void *key1, const void *key2, Size keysize)
{
	const missing_cache_key *entry1 = key1;
	const missing_cache_key *entry2 = key2;

	if (entry1->len != entry2->len)
		return entry1->len > entry2->len ? 1 : -1;

	return memcmp(DatumGetPointer(entry1->value),
				  DatumGetPointer(entry2->value),
				  entry1->len);
}

static void
init_missing_cache(void)
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
				key.len = VARSIZE_ANY(DatumGetPointer(attrmiss->am_value));
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
	return heap_compute_data_size_ext(tupleDesc, values, isnull, false);
}

/*
 * heap_compute_data_size_ext
 *		Determine size of the data area of a tuple to be constructed,
 *		potentially over MaxAllocSize
 *
 * If allow_huge is false, then we raise an error if any intermediate
 * result goes beyond MaxAllocSize; in 32-bit architectures we also
 * do that when the result of an addition overflows Size (which is 32-bit)
 */
Size
heap_compute_data_size_ext(TupleDesc tupleDesc,
						   const Datum *values,
						   const bool *isnull,
						   bool allow_huge)
{
	Size		data_length = 0;
	int			numberOfAttributes = tupleDesc->natts;

	for (int i = 0; i < numberOfAttributes; i++)
	{
		Datum		val;
		CompactAttribute *atti;
		bool		ovfl;

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
			ovfl = pg_add_size_overflow(data_length,
										VARATT_CONVERTED_SHORT_SIZE(DatumGetPointer(val)),
										&data_length);
		}
		else if (atti->attlen == -1 &&
				 VARATT_IS_EXTERNAL_EXPANDED(DatumGetPointer(val)))
		{
			/*
			 * we want to flatten the expanded value so that the constructed
			 * tuple doesn't depend on it
			 */
			data_length = att_nominal_alignby(data_length, atti->attalignby);
			ovfl = pg_add_size_overflow(data_length,
										EOH_get_flat_size(DatumGetEOHP(val)),
										&data_length);
		}
		else
		{
			data_length = att_datum_alignby(data_length, atti->attalignby,
											atti->attlen, val);
			data_length = att_addlength_datum_overflow(data_length, atti->attlen,
													   val, &ovfl);
		}

		/*
		 * Unless caller allows huge values, prevent going beyond the palloc
		 * limit.  In cases where caller does allow huge values, we can't
		 * trust the AllocSizeIsValid() test alone on 32-bit platforms, which
		 * is why we use overflow-detecting addition above.
		 */
		if (ovfl || (!allow_huge && !AllocSizeIsValid(data_length)))
			ereport(ERROR,
					errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					errmsg("tuple is too big"));
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
		 uint8 **bit,
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
				uint16 *infomask, uint8 *bit)
{
	uint8	   *bitP;
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
 *		can't use the attcacheoff and the value is not null.
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
	CompactAttribute *cattr;
	HeapTupleHeader td = tup->t_data;
	char	   *tp;				/* ptr to data part of tuple */
	uint8	   *bp = td->t_bits;	/* ptr to null bitmap in tuple */
	Size		off;			/* current offset within data */
	int			startAttr;
	int			firstNullAttr;
	int			i;
	bool		hasnulls = HeapTupleHasNulls(tup);

	/*
	 * It shouldn't be possible to allocate overlength tuples, so it's not
	 * possible to get in wraparound problems in the arithmetic below.
	 */
	Assert(AllocSizeIsValid(tup->t_len));

	/* Did someone forget to call TupleDescFinalize()? */
	Assert(tupleDesc->firstNonCachedOffsetAttr >= 0);

	attnum--;

	/*
	 * To minimize the number of attributes we need to look at, start walking
	 * the tuple at the attribute with the highest attcacheoff prior to attnum
	 * or the first NULL attribute prior to attnum, whichever comes first.
	 */
	if (hasnulls)
		firstNullAttr = first_null_attr(bp, attnum);
	else
		firstNullAttr = attnum;

	if (tupleDesc->firstNonCachedOffsetAttr > 0 && firstNullAttr > 0)
	{
		/*
		 * Try to start with the highest attribute with an attcacheoff that's
		 * prior to the one we're looking for, or with the attribute prior to
		 * the first NULL attribute, if there is one.
		 */
		startAttr = Min(tupleDesc->firstNonCachedOffsetAttr - 1, firstNullAttr - 1);
		off = TupleDescCompactAttr(tupleDesc, startAttr)->attcacheoff;
	}
	else
	{
		/* Otherwise, start at the beginning... */
		startAttr = 0;
		off = 0;
	}

	tp = (char *) td + td->t_hoff;

	/*
	 * Calculate 'off' up to the first NULL attr.  We use two cheaper loops
	 * when the tuple has no variable-width columns.  When variable-width
	 * columns exists, we use att_addlength_pointer() to move the offset
	 * beyond the current attribute.
	 */
	if (!HeapTupleHasVarWidth(tup))
	{
		for (i = startAttr; i < firstNullAttr; i++)
		{
			cattr = TupleDescCompactAttr(tupleDesc, i);

			off = att_nominal_alignby(off, cattr->attalignby);
			off += cattr->attlen;
		}

		for (; i < attnum; i++)
		{
			if (att_isnull(i, bp))
				continue;

			cattr = TupleDescCompactAttr(tupleDesc, i);

			off = att_nominal_alignby(off, cattr->attalignby);
			off += cattr->attlen;
		}
	}
	else
	{
		for (i = startAttr; i < firstNullAttr; i++)
		{
			int			attlen;

			cattr = TupleDescCompactAttr(tupleDesc, i);
			attlen = cattr->attlen;
			off = att_pointer_alignby(off,
									  cattr->attalignby,
									  attlen,
									  tp + off);
			off = att_addlength_pointer(off, attlen, tp + off);
		}

		for (; i < attnum; i++)
		{
			int			attlen;

			if (att_isnull(i, bp))
				continue;

			cattr = TupleDescCompactAttr(tupleDesc, i);
			attlen = cattr->attlen;
			off = att_pointer_alignby(off, cattr->attalignby, attlen,
									  tp + off);
			off = att_addlength_pointer(off, attlen, tp + off);
		}
	}

	cattr = TupleDescCompactAttr(tupleDesc, attnum);
	off = att_pointer_alignby(off,
							  cattr->attalignby,
							  cattr->attlen,
							  tp + off);

	return fetchatt(cattr, tp + off);
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
	memcpy(newTuple->t_data, tuple->t_data, tuple->t_len);
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
	memcpy(dest->t_data, src->t_data, src->t_len);
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
	uint8	   *nullBits = NULL;
	int			bitMask = 0;
	char	   *targetData;
	uint16	   *infoMask;

	Assert((targetHeapTuple && !targetMinimalTuple)
		   || (!targetHeapTuple && targetMinimalTuple));

	if (HeapTupleHeaderIsVersionedDatum(sourceTHeader))
		heap_datum_check(sourceTHeader, tupleDesc);

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

				targetDataLen = att_addlength_datum(targetDataLen,
													att->attlen,
													attrmiss[attnum].am_value);

				if (!AllocSizeIsValid(targetDataLen))
					ereport(ERROR,
							errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("tuple is too big"));
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
		/* invalidates t_ctid */
		heap_tuple_set_datum(targetTHeader, tupleDesc, len);

		if (targetNullLen > 0)
			nullBits = (uint8 *) ((char *) (*targetHeapTuple)->t_data
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

		/* like HeapTupleHeaderClearDatum() */
		(*targetMinimalTuple)->t_infomask &= ~HEAP_IS_DATUM;

		if (targetNullLen > 0)
			nullBits = (uint8 *) ((char *) *targetMinimalTuple
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
	memcpy(td, tuple->t_data, tuple->t_len);

	/* if wrongly marked, set the type from scratch */
	if (HeapTupleHeaderReallyIsDatum(td))
		heap_datum_update_type(td, tupleDesc);
	else
		heap_tuple_set_datum(td, tupleDesc, tuple->t_len);

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

	/*
	 * Mark the tuple as a datum-style tuple. This is safe even if we
	 * eventually use it as a row type tuple, as e.g. inserting it into a
	 * table will reset the relevant state.
	 *
	 * The reason we always set the datum fields is that there are a lot of
	 * callers to heap_form_tuple() and the vast majority should have the
	 * datum information. It's easier to call HeapTupleHeaderClearDatum() in
	 * the few other cases.
	 */
	HeapTupleHeaderSetNatts(td, numberOfAttributes);
	td->t_hoff = hoff;

	heap_tuple_set_datum(td, tupleDescriptor, len);

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
	values = palloc_array(Datum, numberOfAttributes);
	isnull = palloc_array(bool, numberOfAttributes);

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
	 * copy the identification info of the old tuple: t_self, t_tableOid.
	 *
	 * We used to also set t_data->t_ctid here, but that would invalidate the
	 * version included in datum tuples. There are no known callers that need
	 * t_ctid copied.
	 */
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
	values = palloc_array(Datum, numberOfAttributes);
	isnull = palloc_array(bool, numberOfAttributes);

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

	/* see equivalent code in heap_modify_tuple() */
	newTuple->t_self = tuple->t_self;
	newTuple->t_tableOid = tuple->t_tableOid;

	return newTuple;
}

/*
 * Mark the tuple as a datum-style tuple.
 *
 * This includes setting the datum's length, typeid and version information.
 *
 * The caller already needs to have done HeapTupleHeaderSetNatts().
 *
 * Note that the caller needs to be sure that it is safe to use tupleDesc to
 * deform tup, since this function may remove information that could otherwise
 * have been used to detect unsafe deforming.
 *
 * See also src/backend/catalog/README.attver.md
 */
void
heap_tuple_set_datum(HeapTupleHeader tup, TupleDesc tupleDesc, uint32 len)
{
	HeapTupleHeaderSetDatumLength(tup, len);
	HeapTupleHeaderSetTypeId(tup, tupleDesc->tdtypeid);
	HeapTupleHeaderSetDatum(tup);

	if (tupleDesc->tdtypeid == RECORDOID)
	{
		/*
		 * RECORDOID tuples are not protected by versioning (and do not need
		 * to be).
		 */
		HeapTupleHeaderSetTypMod(tup, tupleDesc->tdtypmod);

		/*
		 * Invalidating t_ctid makes the tuple an unversioned datum, even if
		 * it previously was a versioned one.
		 */
		ItemPointerSetInvalid(&tup->t_ctid);
	}
	else
	{
		int			natts = HeapTupleHeaderGetNatts(tup);
		XLogRecPtr	version;

		/*
		 * Stamp the datum with the version of the last attribute it actually
		 * contains, not the version of the last attribute in the tuple desc.
		 * That is important in case the tuple has fewer attributes than the
		 * descriptor.
		 *
		 * A zero-attribute tuple contains no data to decode, so it is always
		 * safe to deform; give it an explicit version of InvalidXLogRecPtr,
		 * so it can be deformed if columns are added later (as the version is
		 * stored in pg_attribute, a rowtype without columns has no version).
		 *
		 * It is never valid to have fewer attributes in the descriptor than
		 * the tuple when forming a tuple, so error out in that case.
		 * Hopefully unreachable.
		 */
		if (natts == 0)
			version = InvalidXLogRecPtr;
		else if (natts == tupleDesc->natts)
			version = TupleDescGetVersion(tupleDesc);
		else if (natts < tupleDesc->natts)
			version = TupleDescGetAttrVersion(tupleDesc, natts);
		else
			elog(ERROR, "datum's descriptor has fewer (%d) attributes than tuple (%d)",
				 tupleDesc->natts, natts);

		HeapTupleHeaderSetVersion(tup, version);
	}
}

/*
 * Update the type of a datum tuple to the type of tupleDesc.
 *
 * If the tuple already is a versioned datum, it is verified to be compatible
 * with tupleDesc first, as relabeling it would otherwise hide a
 * mismatch.
 *
 * Additionally, if the current type and the target type differ, the types are
 * verified to be compatible (if possible).
 */
void
heap_datum_update_type(HeapTupleHeader tup, TupleDesc tupleDesc)
{
	Assert(HeapTupleHeaderReallyIsDatum(tup));

	heap_datum_check_coercion(tup, tupleDesc);

	heap_tuple_set_datum(tup, tupleDesc,
						 HeapTupleHeaderGetDatumLength(tup));
}

/*
 * Verify that the datum tuple "tup" can be deformed with "deformingDesc",
 * the descriptor the caller wants to use.
 *
 * "datumDesc" is the descriptor of the datum's own row type, against which
 * the datum's version has already been checked (if it is versioned).  Only
 * the attributes present in the tuple are compared, so this does not
 * establish that the two descriptors are interchangeable for other tuples.
 */
static void
verify_tupledesc_compatibility(HeapTupleHeader tup,
							   TupleDesc datumDesc, TupleDesc deformingDesc)
{
	int			datum_natts = HeapTupleHeaderGetNatts(tup);
	int			error_level =
		composite_type_mismatch == COMPOSITE_TYPE_MISMATCH_WARN ?
		WARNING : ERROR;

	/*
	 * Should never get here with a datumDesc that has insufficient
	 * attributes.
	 */
	if (datumDesc->natts < datum_natts)
	{
		elog(error_level, "datum's descriptor has fewer (%d) attributes than tuple (%d)",
			 datumDesc->natts, datum_natts);
		return;
	}

	/*
	 * We sometimes may get called with a deformingDesc that has extra
	 * attributes for resjunk columns. That's safe iff the tuple itself does
	 * not have those columns (the expected case). But we can't accept cases
	 * where the datum has more attributes than deformingDesc.
	 */
	if (unlikely(deformingDesc->natts < datum_natts))
	{
		ereport(error_level,
				errcode(ERRCODE_DATATYPE_MISMATCH),
				errmsg("tuple descriptor has fewer (%d) attributes than tuple (%d)",
					   deformingDesc->natts, datum_natts));
		return;
	}

	/*
	 * The message isn't translatable, due to the interpolated format strings.
	 * That's OK, as this is just a defensive layer that shouldn't be reached.
	 *
	 * With composite_type_mismatch = warn, only the first mismatch is
	 * reported.
	 */
#define check_attribute_aspect(aspect, format) \
	if (unlikely(attr_datum->aspect != attr_deform->aspect)) \
	{ \
		ereport(error_level, \
				errcode(ERRCODE_DATATYPE_MISMATCH), \
				errmsg_internal("datum attribute \"%s\" at position %d does not match deforming descriptor's %s ("format" vs "format")", \
								NameStr(attr_datum->attname), \
								i + 1, \
								CppAsString(aspect), \
								attr_datum->aspect, attr_deform->aspect) \
			); \
		return; \
	}

	for (int i = 0; i < datum_natts; i++)
	{
		Form_pg_attribute attr_datum = TupleDescAttr(datumDesc, i);
		Form_pg_attribute attr_deform = TupleDescAttr(deformingDesc, i);

		/* don't care about the attribute's name being the same */

		if (unlikely(attr_datum->attisdropped != attr_deform->attisdropped))
		{
			/*
			 * Targetlists can represent dropped columns as NULL::int4.  A
			 * NULL attribute consumes neither bytes nor alignment, so its
			 * type and storage metadata need not match for deforming to be
			 * safe.
			 *
			 * See eval-plan-qual-dropped-column.spec for a test exercising
			 * this path.
			 */
			bool		isnull = (tup->t_infomask & HEAP_HASNULL) &&
				att_isnull(i, tup->t_bits);

			/*
			 * XXX: At some point in the future we should also check that
			 * attr_deform's NOT NULLness matches, due to the optimized
			 * deforming slots can use for NOT NULL columns. But right now we
			 * don't know if that's guaranteed here (and the constraints
			 * applying to the tuple might not yet have been validated). We
			 * also, at the moment, won't get here for slots with that
			 * optimization. Therefore delay this for later.
			 */
			if (isnull)
				continue;
		}

		/* Other dropped-column mismatches require explicit conversion. */
		check_attribute_aspect(attisdropped, "%u");

		/*
		 * While, with an atttypid mismatch, the types could be binary
		 * coercible, we can't know that here, and in any case, it should have
		 * led to attribute mapping being used.
		 */
		if (!attr_datum->attisdropped)
			check_attribute_aspect(atttypid, "%u");
		check_attribute_aspect(attlen, "%d");
		check_attribute_aspect(attalign, "%c");

		/*
		 * If the attribute is dropped, byval does not matter. It'd generally
		 * be correct to check byval regardless, but check_attrmap_match()
		 * doesn't currently, so that could cause spurious errors.
		 */
		if (!attr_datum->attisdropped)
		{
			/*
			 * Theoretically bool could be wider than char, so I'm using %u
			 * here.
			 */
			check_attribute_aspect(attbyval, "%u");
		}

		/*
		 * It does not seem necessary to compare atttypmod, attcollation, etc,
		 * they are not required to match for correct deforming.
		 */
	}
#undef check_attribute_aspect
}

/*
 * Report accessing a tuple with an incompatible tuple descriptor, due to a
 * version mismatch.  descVersion is the version the tuple was compared with,
 * see datum_attribute_version_ok().  This is out-of-line because it can be
 * reached via several paths.
 */
pg_noinline static void
heap_datum_version_mismatch_error(Oid tupTypeid, Oid descTypeid,
								  XLogRecPtr tupVersion, XLogRecPtr descVersion)
{
	int			error_level =
		composite_type_mismatch == COMPOSITE_TYPE_MISMATCH_WARN ?
		WARNING : ERROR;

	/*
	 * NB: We can't embed the versions in the primary message without making
	 * test output unstable. Putting them into DETAIL would require the tests
	 * to use \set VERBOSITY terse, which would also hide the other DETAIL and
	 * CONTEXT lines that many of them benefit from. Therefore the versions
	 * are only reported in the server log. That's not pretty, but ...
	 */

	/* log details about the mismatch */
	ereport(LOG_SERVER_ONLY,
			errcode(ERRCODE_DATUM_VERSION_MISMATCH),
			errmsg("tuple descriptor for type %s (OID %u), version %X/%X is incompatible with tuple type %s (OID %u), version %X/%X",
				   format_type_be(descTypeid), descTypeid,
				   LSN_FORMAT_ARGS(descVersion),
				   format_type_be(tupTypeid), tupTypeid,
				   LSN_FORMAT_ARGS(tupVersion)));

	/* raise the actual error / warning */
	ereport(error_level,
			errcode(ERRCODE_DATUM_VERSION_MISMATCH),
			tupVersion < descVersion ?
			errmsg("row type of datum tuple was altered after the datum's creation")
			:
			errmsg("using outdated tuple descriptor when deforming row type"),
			errhint("See server log for additional details."));
}

/*
 * Check if the tuple can be safely decoded, despite a mismatch between the
 * descriptor's version and the tuple's, due to the tuple having fewer
 * attributes and that subset having a matching version.
 *
 * This can happen legitimately if the tuple has fewer attributes than the
 * descriptor, and only attributes not included in the tuple have been
 * altered.  To handle that case, we just need to compare the tuple's version
 * with the version number of attribute tupNatts in the tuple descriptor.
 *
 * *cmpVersion is set to the version the tuple was compared with, for the
 * caller to report a mismatch.
 */
static bool
datum_attribute_version_ok(TupleDesc tupleDesc, int tupNatts,
						   XLogRecPtr tupVersion, XLogRecPtr *cmpVersion)
{
	*cmpVersion = TupleDescGetVersion(tupleDesc);

	/* should only be called if the aggregate version does not match */
	Assert(*cmpVersion != tupVersion);

	/* always safe to deform no columns */
	if (tupNatts == 0)
		return true;

	/*
	 * Iff the tuple is lacking columns in the descriptor, we can check the
	 * version of the last column present.
	 *
	 * We do not allow cases where the tuple descriptor has fewer columns than
	 * the tuple, as that indicates using a too old descriptor. It might be
	 * safe to allow that in some cases, but there don't seem to be any
	 * convincing cases that'd benefit.
	 */
	if (tupleDesc->natts > tupNatts)
	{
		*cmpVersion = TupleDescGetAttrVersion(tupleDesc, tupNatts);
		return tupVersion == *cmpVersion;
	}

	return false;
}

/*
 * Fetch and return a pinned TupleDesc for the type/typmod combination.
 *
 * This switches to CurTransactionContext during the fetch, in case we get
 * called from within something like a tuplesort with a bump context (which
 * does not support freeing), which could be a problem during a syscache
 * miss. Because syscache misses can leak some memory, it should be a context
 * with a somewhat bounded lifetime. Using the current transaction context
 * seems like the most obvious choice.
 */
static TupleDesc
heap_datum_fetch_tupledesc(Oid tupTypeid, int32 tupTypmod)
{
	MemoryContext oldContext = MemoryContextSwitchTo(CurTransactionContext);
	TupleDesc	descForTuple = lookup_rowtype_tupdesc(tupTypeid, tupTypmod);

	MemoryContextSwitchTo(oldContext);

	return descForTuple;
}

/*
 * Fetch the tuple descriptor for the tuple using the tuple's typeid, check
 * that the tuple has the same version as that fetched descriptor and finally
 * verify that the callerDesc is compatible with that fetched TupleDesc.
 *
 * Normally this should not be needed, but we have a few codepaths that use
 * tuple descriptors that are verified to be compatible, with mismatching
 * typeids. In the places this function is used, we can't be sure that the
 * mismatch is harmless, so we re-verify compatibility.
 *
 * NB: This is not cheap, so it's worth making sure that this path is only
 * reached for cases with significant per-row costs.
 */
static void
heap_datum_fetch_compare_tupledesc(HeapTupleHeader tup,
								   Oid tupTypeid, int32 tupTypmod,
								   TupleDesc callerDesc)
{
	int			tupNatts = HeapTupleHeaderGetNatts(tup);
	TupleDesc	descForTuple = heap_datum_fetch_tupledesc(tupTypeid, tupTypmod);
	XLogRecPtr	descForTupleVersion = TupleDescGetVersion(descForTuple);

	/*
	 * First check if the tuple descriptor we just looked up has a compatible
	 * version. It's possible there have been incompatible alterations since
	 * the tuple was formed.
	 */
	if (HeapTupleHeaderIsVersionedDatum(tup))
	{
		XLogRecPtr	tupVersion = HeapTupleHeaderGetVersion(tup);
		XLogRecPtr	cmpVersion;

		if (unlikely(descForTupleVersion != tupVersion) &&
			!datum_attribute_version_ok(descForTuple, tupNatts, tupVersion,
										&cmpVersion))
			heap_datum_version_mismatch_error(tupTypeid, descForTuple->tdtypeid,
											  tupVersion, cmpVersion);
	}

	/*
	 * Now check that the caller's descriptor can decode this particular
	 * tuple, including any physically NULL dropped-column placeholders.
	 */
	verify_tupledesc_compatibility(tup, descForTuple, callerDesc);

	ReleaseTupleDesc(descForTuple);
}

/*
 * Try to verify that it is safe to coerce tup to have targetDesc's type.
 *
 * This will often be used when the types don't actually differ, so we try to
 * avoid unnecessary checks.
 */
static void
heap_datum_check_coercion(HeapTupleHeader tup, TupleDesc targetDesc)
{
	Oid			tupTypeid = HeapTupleHeaderGetTypeId(tup);
	int32		tupTypmod = HeapTupleHeaderGetTypMod(tup);

	Assert(HeapTupleHeaderIsDatum(tup));

	if (unlikely(composite_type_mismatch == COMPOSITE_TYPE_MISMATCH_DISABLED))
		return;

	/* can't check if this is wrongly marked as a datum tuple */
	if (!HeapTupleHeaderReallyIsDatum(tup))
		return;

	/*
	 * Source tuple has an ephemeral row type, so we can't fetch the tuple's
	 * desc to cross-check against.
	 */
	if (tupTypeid == RECORDOID && tupTypmod == -1)
		return;

	if (tupTypeid != targetDesc->tdtypeid ||
		(targetDesc->tdtypeid == RECORDOID &&
		 tupTypmod != targetDesc->tdtypmod))
	{
		/*
		 * The tuple's type and the target type differ, cross check the tuple
		 * descriptors against each other.
		 */
		heap_datum_fetch_compare_tupledesc(tup, tupTypeid, tupTypmod, targetDesc);
	}
	else if (HeapTupleHeaderIsVersionedDatum(tup) &&
			 TupleDescGetVersion(targetDesc) != HeapTupleHeaderGetVersion(tup))
	{
		/*
		 * While the tuple's type and the target type are the same, their
		 * versions differ. Make sure that's OK.
		 */
		heap_datum_check(tup, targetDesc);
	}
}

/*
 * Check that a versioned datum tuple can be safely decoded with the
 * tupleDesc, raise an error if not.
 *
 * It is an error to call this for an unversioned datum, callers need to check
 * HeapTupleHeaderIsVersionedDatum() first.
 *
 * This function implements the attribute versioning checks described in
 * src/backend/catalog/README.attver.md.  For partially historical reasons, it
 * needs to handle some corner cases, described below.
 *
 * In addition to the versioning checks, this also, if the descriptor and the
 * tuple are for different types, tries to check that the datum's and the
 * descriptor's types are compatible.  We should work towards making such
 * mismatches errors, but until then the additional checks are helpful.
 */
void
heap_datum_check(HeapTupleHeader tup, TupleDesc tupleDesc)
{
	XLogRecPtr	descVersion = TupleDescGetVersion(tupleDesc);
	Oid			tupTypeid = HeapTupleHeaderGetTypeId(tup);
	int32		tupTypmod = HeapTupleHeaderGetTypMod(tup);
	XLogRecPtr	tupVersion;

	if (unlikely(composite_type_mismatch == COMPOSITE_TYPE_MISMATCH_DISABLED))
		return;

	/*
	 * This should not be reachable, but given this is new infrastructure and
	 * out-of-tree code might be doing unexpected things, it seems better to
	 * verify with a runtime check than just an Assert().
	 */
	if (unlikely(!HeapTupleHeaderIsVersionedDatum(tup)))
		elog(ERROR, "heap_datum_check() called on an unversioned datum");

	/* can't check if this is wrongly marked as a datum tuple */
	if (unlikely(!HeapTupleHeaderReallyIsDatum(tup)))
		return;

	tupVersion = HeapTupleHeaderGetVersion(tup);

	if (unlikely(tupleDesc->tdtypeid == RECORDOID))
	{
		/*
		 * It's possible that we will get called with a RECORDOID tupledesc
		 * but the datum has a different type, e.g. because some function just
		 * updated the descriptor's type to RECORDOID.  In that case it's
		 * really the caller's responsibility to have made sure that the type
		 * is right. But we better make sure anyway.
		 *
		 * While it's not cheap, we can make sure this is safe by first
		 * fetching the tuple descriptor for the tuple, verifying that its
		 * version matches, and then checking that the two tuple descriptors
		 * are compatible.
		 *
		 * The known paths reaching here involve per-row triggers, and
		 * EvalPlanQual rechecks of whole-row references to function scans,
		 * where the cost of the additional checking is acceptable.
		 */
		heap_datum_fetch_compare_tupledesc(tup, tupTypeid, tupTypmod,
										   tupleDesc);
	}
	else if (unlikely(tupTypeid == InvalidOid))
	{
		/*
		 * This could happen if code does a heap_form_tuple() and then
		 * overwrites typeid etc with invalid fields. However most such code
		 * is already handled via the HeapTupleHeaderReallyIsDatum() above.
		 *
		 * NB: In contrast to some other paths in this function, we can't
		 * fetch the tuple's "real" tuple descriptor, we lack a valid type
		 * oid.
		 */
		ereport(ERROR,
				errmsg_internal("datum tuple without type information"),
				errhint_internal("An extension may need to be updated."));
	}
	else if (unlikely(tupTypeid == RECORDOID))
	{
		/*
		 * The tupledesc has a version number, but the tuple is a record
		 * tuple, for which we do not store a version number (since it has to
		 * be a blessed tupledesc that can never change and there are no
		 * catalog entries to associate the version with either).
		 *
		 * This should not be reached, we don't mark a datum as versioned for
		 * RECORDOID, but could be reached if some caller relabels a versioned
		 * datum as RECORDOID without going through heap_tuple_set_datum().
		 */
		elog(ERROR, "processing record datum (%u, %d) with rowtype tupdesc (%u, %d, %X/%X)",
			 tupTypeid, tupTypmod,
			 tupleDesc->tdtypeid, tupleDesc->tdtypmod,
			 LSN_FORMAT_ARGS(descVersion));
	}
	else if (unlikely(tupleDesc->tdtypeid != tupTypeid))
	{
		/*
		 * In some partitioned workloads, we currently use the parent
		 * tupledesc to deform a child tuple, or vice versa. Typically the
		 * caller will have verified that no conversion is necessary (c.f.
		 * build_attrmap_by_name_if_req()), but we can't verify that that
		 * happened here.
		 *
		 * XXX: This code pattern seems problematic, but it's probably not
		 * feasible to fix this in the back branches. Medium term I think the
		 * tuple conversion paths should update the tuple's typeid even when
		 * otherwise no conversion is needed.
		 *
		 * To make sure this cannot lead to unsafe deforming, we check that
		 * the tuple descriptor for the tuple (identified by typeid and
		 * version) and the one used by the caller are compatible.
		 *
		 * The known cases leading here are DML with triggers and/or ON
		 * CONFLICT, where the per-row cost of this check is acceptable.
		 */
		heap_datum_fetch_compare_tupledesc(tup, tupTypeid, tupTypmod,
										   tupleDesc);
	}
	else if (unlikely(tupVersion != descVersion))
	{
		XLogRecPtr	cmpVersion;

		/*
		 * The tuple's version does not match the tuple descriptor's.
		 *
		 * Check if that's solely because the tuple is shorter (i.e. only new
		 * columns have been added since the tuple was formed).
		 *
		 * Otherwise report the mismatch.
		 */
		if (!datum_attribute_version_ok(tupleDesc, HeapTupleHeaderGetNatts(tup),
										tupVersion, &cmpVersion))
			heap_datum_version_mismatch_error(tupTypeid, tupleDesc->tdtypeid,
											  tupVersion, cmpVersion);
	}
}

/*
 * Helper for DatumGetHeapTupleHeader / DatumGetHeapTupleHeaderCopy.
 *
 * This implements the "Upgrading from pre-versioning composite datums" logic
 * in src/backend/catalog/README.attver.md.
 *
 * Due to requiring a copy of the input tuple, this isn't exactly cheap, but
 * there should be fewer and fewer unversioned tuples in clusters over time, as
 * such datums get re-formed or deleted. We unfortunately can't just fix them
 * in-place without causing larger issues.
 */
static inline HeapTupleHeader
DatumGetHeapTupleHeaderImpl(Datum t, bool copy, bool *was_datum)
{
	struct varlena *tpr = (struct varlena *) DatumGetPointer(t);
	HeapTupleHeader tp;
	Oid			tupTypeid;

	if (copy)
		tp = (HeapTupleHeader) pg_detoast_datum_copy(tpr);
	else
	{
		tp = (HeapTupleHeader) pg_detoast_datum(tpr);
		copy = ((struct varlena *) tp) != tpr;
	}

	tupTypeid = HeapTupleHeaderGetTypeId(tp);

	if (was_datum)
		*was_datum = true;

	if (!HeapTupleHeaderIsDatum(tp))
	{
		Assert(HeapTupleHeaderGetDatumLength(tp) > 0);

		if (was_datum)
			*was_datum = false;

		/*
		 * We need to modify the tuple header but it hasn't been copied yet
		 * (not detoasted, not called by DatumGetHeapTupleHeaderCopy), copy it
		 * now.
		 */
		if (!copy)
		{
			Size		sz = HeapTupleHeaderGetDatumLength(tp);
			HeapTupleHeader tp2;

			copy = true;

			tp2 = (HeapTupleHeader) palloc(sz);
			memcpy(tp2, tp, sz);
			tp = tp2;
		}

		/*
		 * Mark as a datum tuple, versioned at version InvalidXLogRecPtr
		 * unless it's a RECORDOID style datum.
		 */
		HeapTupleHeaderSetDatum(tp);
		if (tupTypeid != RECORDOID)
			HeapTupleHeaderSetVersion(tp, InvalidXLogRecPtr);
	}

	Assert(HeapTupleHeaderReallyIsDatum(tp));

	return tp;
}

HeapTupleHeader
DatumGetHeapTupleHeader(Datum t)
{
	return DatumGetHeapTupleHeaderImpl(t, false, NULL);
}

/*
 * Like DatumGetHeapTupleHeader but returns whether the datum was already
 * marked as a datum in *was_datum.
 */
HeapTupleHeader
DatumGetHeapTupleHeaderExt(Datum t, bool *was_datum)
{
	return DatumGetHeapTupleHeaderImpl(t, false, was_datum);
}

HeapTupleHeader
DatumGetHeapTupleHeaderCopy(Datum t)
{
	return DatumGetHeapTupleHeaderImpl(t, true, NULL);
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
	CompactAttribute *cattr;
	bool		hasnulls = HeapTupleHasNulls(tuple);
	int			tdesc_natts = tupleDesc->natts;
	int			natts;			/* number of atts to extract */
	int			attnum;
	char	   *tp;				/* ptr to tuple data */
	uint32		off;			/* offset in tuple data */
	uint8	   *bp = tup->t_bits;	/* ptr to null bitmap in tuple */
	int			firstNonCacheOffsetAttr;
	int			firstNullAttr;

	if (HeapTupleHeaderIsVersionedDatum(tup))
		heap_datum_check(tup, tupleDesc);

	natts = HeapTupleHeaderGetNatts(tup);

	/* Did someone forget to call TupleDescFinalize()? */
	Assert(tupleDesc->firstNonCachedOffsetAttr >= 0);

	/*
	 * In inheritance situations, it is possible that the given tuple actually
	 * has more fields than the caller is expecting.  Don't run off the end of
	 * the caller's arrays.
	 */
	natts = Min(natts, tdesc_natts);
	firstNonCacheOffsetAttr = Min(tupleDesc->firstNonCachedOffsetAttr, natts);

	if (hasnulls)
	{
		firstNullAttr = first_null_attr(bp, natts);

		/*
		 * XXX: it'd be nice to use populate_isnull_array() here, but that
		 * requires that the isnull array's size is rounded up to the next
		 * multiple of 8.  Doing that would require adjusting many locations
		 * that allocate the array.
		 */
		firstNonCacheOffsetAttr = Min(firstNonCacheOffsetAttr, firstNullAttr);
	}
	else
		firstNullAttr = natts;

	tp = (char *) tup + tup->t_hoff;
	attnum = 0;

	if (firstNonCacheOffsetAttr > 0)
	{
#ifdef USE_ASSERT_CHECKING
		/* In Assert enabled builds, verify attcacheoff is correct */
		int			offcheck = 0;
#endif
		do
		{
			isnull[attnum] = false;
			cattr = TupleDescCompactAttr(tupleDesc, attnum);
			off = cattr->attcacheoff;

#ifdef USE_ASSERT_CHECKING
			offcheck = att_nominal_alignby(offcheck, cattr->attalignby);
			Assert(offcheck == cattr->attcacheoff);
			offcheck += cattr->attlen;
#endif

			values[attnum] = fetch_att_noerr(tp + off,
											 cattr->attbyval,
											 cattr->attlen);
		} while (++attnum < firstNonCacheOffsetAttr);
		off += cattr->attlen;
	}
	else
		off = 0;

	for (; attnum < firstNullAttr; attnum++)
	{
		isnull[attnum] = false;
		cattr = TupleDescCompactAttr(tupleDesc, attnum);
		values[attnum] = align_fetch_then_add(tp,
											  &off,
											  cattr->attbyval,
											  cattr->attlen,
											  cattr->attalignby);
	}

	for (; attnum < natts; attnum++)
	{
		Assert(hasnulls);

		if (att_isnull(attnum, bp))
		{
			values[attnum] = (Datum) 0;
			isnull[attnum] = true;
			continue;
		}

		isnull[attnum] = false;
		cattr = TupleDescCompactAttr(tupleDesc, attnum);

		/* align 'off', fetch the attr's value, and increment off beyond it */
		values[attnum] = align_fetch_then_add(tp,
											  &off,
											  cattr->attbyval,
											  cattr->attlen,
											  cattr->attalignby);
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
						const bool *isnull,
						Size extra)
{
	MinimalTuple tuple;			/* return tuple */
	char	   *mem;
	Size		len,
				data_len;
	int			hoff;
	bool		hasnull = false;
	int			numberOfAttributes = tupleDescriptor->natts;
	int			i;

	Assert(extra == MAXALIGN(extra));

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
	mem = palloc0(len + extra);
	tuple = (MinimalTuple) (mem + extra);

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
heap_copy_minimal_tuple(MinimalTuple mtup, Size extra)
{
	MinimalTuple result;
	char	   *mem;

	Assert(extra == MAXALIGN(extra));
	mem = palloc(mtup->t_len + extra);
	memset(mem, 0, extra);
	result = (MinimalTuple) (mem + extra);
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
minimal_tuple_from_heap_tuple(HeapTuple htup, Size extra)
{
	MinimalTuple result;
	char	   *mem;
	uint32		len;

	Assert(extra == MAXALIGN(extra));
	Assert(htup->t_len > MINIMAL_TUPLE_OFFSET);
	len = htup->t_len - MINIMAL_TUPLE_OFFSET;
	mem = palloc(len + extra);
	memset(mem, 0, extra);
	result = (MinimalTuple) (mem + extra);
	memcpy(result, (char *) htup->t_data + MINIMAL_TUPLE_OFFSET, len);

	/*
	 * A minimal tuple won't have the necessary fields to be treated as a
	 * datum tuple, so we better reset those fields lest they trigger assert
	 * failures at a later point.
	 *
	 * XXX: It'd be good to call heap_datum_check() before losing the ability
	 * to do so, but we don't have the tupledesc, so we can't trivially do so.
	 * We could try to fetch the tupledesc from typeid and verify that way?
	 * But that's not cheap.
	 */
	if (HeapTupleHeaderIsDatum(htup->t_data))
	{
		/* like HeapTupleHeaderClearDatum() */
		result->t_infomask &= ~HEAP_IS_DATUM;
	}

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
