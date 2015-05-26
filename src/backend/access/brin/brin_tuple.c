/*
 * brin_tuples.c
 *		Method implementations for tuples in BRIN indexes.
 *
 * Intended usage is that code outside this file only deals with
 * BrinMemTuples, and convert to and from the on-disk representation through
 * functions in this file.
 *
 * NOTES
 *
 * A BRIN tuple is similar to a heap tuple, with a few key differences.  The
 * first interesting difference is that the tuple header is much simpler, only
 * containing its total length and a small area for flags.  Also, the stored
 * data does not match the relation tuple descriptor exactly: for each
 * attribute in the descriptor, the index tuple carries an arbitrary number
 * of values, depending on the opclass.
 *
 * Also, for each column of the index relation there are two null bits: one
 * (hasnulls) stores whether any tuple within the page range has that column
 * set to null; the other one (allnulls) stores whether the column values are
 * all null.  If allnulls is true, then the tuple data area does not contain
 * values for that column at all; whereas it does if the hasnulls is set.
 * Note the size of the null bitmask may not be the same as that of the
 * datum array.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/brin/brin_tuple.c
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/brin_tuple.h"
#include "access/tupdesc.h"
#include "access/tupmacs.h"
#include "utils/datum.h"
#include "utils/memutils.h"


static inline void brin_deconstruct_tuple(BrinDesc *brdesc,
					   char *tp, bits8 *nullbits, bool nulls,
					   Datum *values, bool *allnulls, bool *hasnulls);


/*
 * Return a tuple descriptor used for on-disk storage of BRIN tuples.
 */
static TupleDesc
brtuple_disk_tupdesc(BrinDesc *brdesc)
{
	/* We cache these in the BrinDesc */
	if (brdesc->bd_disktdesc == NULL)
	{
		int			i;
		int			j;
		AttrNumber	attno = 1;
		TupleDesc	tupdesc;
		MemoryContext oldcxt;

		/* make sure it's in the bdesc's context */
		oldcxt = MemoryContextSwitchTo(brdesc->bd_context);

		tupdesc = CreateTemplateTupleDesc(brdesc->bd_totalstored, false);

		for (i = 0; i < brdesc->bd_tupdesc->natts; i++)
		{
			for (j = 0; j < brdesc->bd_info[i]->oi_nstored; j++)
				TupleDescInitEntry(tupdesc, attno++, NULL,
								 brdesc->bd_info[i]->oi_typcache[j]->type_id,
								   -1, 0);
		}

		MemoryContextSwitchTo(oldcxt);

		brdesc->bd_disktdesc = tupdesc;
	}

	return brdesc->bd_disktdesc;
}

/*
 * Generate a new on-disk tuple to be inserted in a BRIN index.
 *
 * See brin_form_placeholder_tuple if you touch this.
 */
BrinTuple *
brin_form_tuple(BrinDesc *brdesc, BlockNumber blkno, BrinMemTuple *tuple,
				Size *size)
{
	Datum	   *values;
	bool	   *nulls;
	bool		anynulls = false;
	BrinTuple  *rettuple;
	int			keyno;
	int			idxattno;
	uint16		phony_infomask = 0;
	bits8	   *phony_nullbitmap;
	Size		len,
				hoff,
				data_len;

	Assert(brdesc->bd_totalstored > 0);

	values = (Datum *) palloc(sizeof(Datum) * brdesc->bd_totalstored);
	nulls = (bool *) palloc0(sizeof(bool) * brdesc->bd_totalstored);
	phony_nullbitmap = (bits8 *)
		palloc(sizeof(bits8) * BITMAPLEN(brdesc->bd_totalstored));

	/*
	 * Set up the values/nulls arrays for heap_fill_tuple
	 */
	idxattno = 0;
	for (keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
	{
		int			datumno;

		/*
		 * "allnulls" is set when there's no nonnull value in any row in the
		 * column; when this happens, there is no data to store.  Thus set the
		 * nullable bits for all data elements of this column and we're done.
		 */
		if (tuple->bt_columns[keyno].bv_allnulls)
		{
			for (datumno = 0;
				 datumno < brdesc->bd_info[keyno]->oi_nstored;
				 datumno++)
				nulls[idxattno++] = true;
			anynulls = true;
			continue;
		}

		/*
		 * The "hasnulls" bit is set when there are some null values in the
		 * data.  We still need to store a real value, but the presence of
		 * this means we need a null bitmap.
		 */
		if (tuple->bt_columns[keyno].bv_hasnulls)
			anynulls = true;

		for (datumno = 0;
			 datumno < brdesc->bd_info[keyno]->oi_nstored;
			 datumno++)
			values[idxattno++] = tuple->bt_columns[keyno].bv_values[datumno];
	}

	/* Assert we did not overrun temp arrays */
	Assert(idxattno <= brdesc->bd_totalstored);

	/* compute total space needed */
	len = SizeOfBrinTuple;
	if (anynulls)
	{
		/*
		 * We need a double-length bitmap on an on-disk BRIN index tuple; the
		 * first half stores the "allnulls" bits, the second stores
		 * "hasnulls".
		 */
		len += BITMAPLEN(brdesc->bd_tupdesc->natts * 2);
	}

	len = hoff = MAXALIGN(len);

	data_len = heap_compute_data_size(brtuple_disk_tupdesc(brdesc),
									  values, nulls);
	len += data_len;

	len = MAXALIGN(len);

	rettuple = palloc0(len);
	rettuple->bt_blkno = blkno;
	rettuple->bt_info = hoff;

	/* Assert that hoff fits in the space available */
	Assert((rettuple->bt_info & BRIN_OFFSET_MASK) == hoff);

	/*
	 * The infomask and null bitmap as computed by heap_fill_tuple are useless
	 * to us.  However, that function will not accept a null infomask; and we
	 * need to pass a valid null bitmap so that it will correctly skip
	 * outputting null attributes in the data area.
	 */
	heap_fill_tuple(brtuple_disk_tupdesc(brdesc),
					values,
					nulls,
					(char *) rettuple + hoff,
					data_len,
					&phony_infomask,
					phony_nullbitmap);

	/* done with these */
	pfree(values);
	pfree(nulls);
	pfree(phony_nullbitmap);

	/*
	 * Now fill in the real null bitmasks.  allnulls first.
	 */
	if (anynulls)
	{
		bits8	   *bitP;
		int			bitmask;

		rettuple->bt_info |= BRIN_NULLS_MASK;

		/*
		 * Note that we reverse the sense of null bits in this module: we
		 * store a 1 for a null attribute rather than a 0.  So we must reverse
		 * the sense of the att_isnull test in br_deconstruct_tuple as well.
		 */
		bitP = ((bits8 *) ((char *) rettuple + SizeOfBrinTuple)) - 1;
		bitmask = HIGHBIT;
		for (keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
		{
			if (bitmask != HIGHBIT)
				bitmask <<= 1;
			else
			{
				bitP += 1;
				*bitP = 0x0;
				bitmask = 1;
			}

			if (!tuple->bt_columns[keyno].bv_allnulls)
				continue;

			*bitP |= bitmask;
		}
		/* hasnulls bits follow */
		for (keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
		{
			if (bitmask != HIGHBIT)
				bitmask <<= 1;
			else
			{
				bitP += 1;
				*bitP = 0x0;
				bitmask = 1;
			}

			if (!tuple->bt_columns[keyno].bv_hasnulls)
				continue;

			*bitP |= bitmask;
		}
		bitP = ((bits8 *) (rettuple + SizeOfBrinTuple)) - 1;
	}

	if (tuple->bt_placeholder)
		rettuple->bt_info |= BRIN_PLACEHOLDER_MASK;

	*size = len;
	return rettuple;
}

/*
 * Generate a new on-disk tuple with no data values, marked as placeholder.
 *
 * This is a cut-down version of brin_form_tuple.
 */
BrinTuple *
brin_form_placeholder_tuple(BrinDesc *brdesc, BlockNumber blkno, Size *size)
{
	Size		len;
	Size		hoff;
	BrinTuple  *rettuple;
	int			keyno;
	bits8	   *bitP;
	int			bitmask;

	/* compute total space needed: always add nulls */
	len = SizeOfBrinTuple;
	len += BITMAPLEN(brdesc->bd_tupdesc->natts * 2);
	len = hoff = MAXALIGN(len);

	rettuple = palloc0(len);
	rettuple->bt_blkno = blkno;
	rettuple->bt_info = hoff;
	rettuple->bt_info |= BRIN_NULLS_MASK | BRIN_PLACEHOLDER_MASK;

	bitP = ((bits8 *) ((char *) rettuple + SizeOfBrinTuple)) - 1;
	bitmask = HIGHBIT;
	/* set allnulls true for all attributes */
	for (keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
	{
		if (bitmask != HIGHBIT)
			bitmask <<= 1;
		else
		{
			bitP += 1;
			*bitP = 0x0;
			bitmask = 1;
		}

		*bitP |= bitmask;
	}
	/* no need to set hasnulls */

	*size = len;
	return rettuple;
}

/*
 * Free a tuple created by brin_form_tuple
 */
void
brin_free_tuple(BrinTuple *tuple)
{
	pfree(tuple);
}

/*
 * Create a palloc'd copy of a BrinTuple.
 */
BrinTuple *
brin_copy_tuple(BrinTuple *tuple, Size len)
{
	BrinTuple  *newtup;

	newtup = palloc(len);
	memcpy(newtup, tuple, len);

	return newtup;
}

/*
 * Return whether two BrinTuples are bitwise identical.
 */
bool
brin_tuples_equal(const BrinTuple *a, Size alen, const BrinTuple *b, Size blen)
{
	if (alen != blen)
		return false;
	if (memcmp(a, b, alen) != 0)
		return false;
	return true;
}

/*
 * Create a new BrinMemTuple from scratch, and initialize it to an empty
 * state.
 *
 * Note: we don't provide any means to free a deformed tuple, so make sure to
 * use a temporary memory context.
 */
BrinMemTuple *
brin_new_memtuple(BrinDesc *brdesc)
{
	BrinMemTuple *dtup;
	char	   *currdatum;
	long		basesize;
	int			i;

	basesize = MAXALIGN(sizeof(BrinMemTuple) +
						sizeof(BrinValues) * brdesc->bd_tupdesc->natts);
	dtup = palloc0(basesize + sizeof(Datum) * brdesc->bd_totalstored);
	currdatum = (char *) dtup + basesize;
	for (i = 0; i < brdesc->bd_tupdesc->natts; i++)
	{
		dtup->bt_columns[i].bv_attno = i + 1;
		dtup->bt_columns[i].bv_allnulls = true;
		dtup->bt_columns[i].bv_hasnulls = false;
		dtup->bt_columns[i].bv_values = (Datum *) currdatum;
		currdatum += sizeof(Datum) * brdesc->bd_info[i]->oi_nstored;
	}

	dtup->bt_context = AllocSetContextCreate(CurrentMemoryContext,
											 "brin dtuple",
											 ALLOCSET_DEFAULT_MINSIZE,
											 ALLOCSET_DEFAULT_INITSIZE,
											 ALLOCSET_DEFAULT_MAXSIZE);
	return dtup;
}

/*
 * Reset a BrinMemTuple to initial state
 */
void
brin_memtuple_initialize(BrinMemTuple *dtuple, BrinDesc *brdesc)
{
	int			i;

	MemoryContextReset(dtuple->bt_context);
	for (i = 0; i < brdesc->bd_tupdesc->natts; i++)
	{
		dtuple->bt_columns[i].bv_allnulls = true;
		dtuple->bt_columns[i].bv_hasnulls = false;
	}
}

/*
 * Convert a BrinTuple back to a BrinMemTuple.  This is the reverse of
 * brin_form_tuple.
 *
 * Note we don't need the "on disk tupdesc" here; we rely on our own routine to
 * deconstruct the tuple from the on-disk format.
 */
BrinMemTuple *
brin_deform_tuple(BrinDesc *brdesc, BrinTuple *tuple)
{
	BrinMemTuple *dtup;
	Datum	   *values;
	bool	   *allnulls;
	bool	   *hasnulls;
	char	   *tp;
	bits8	   *nullbits;
	int			keyno;
	int			valueno;
	MemoryContext oldcxt;

	dtup = brin_new_memtuple(brdesc);

	if (BrinTupleIsPlaceholder(tuple))
		dtup->bt_placeholder = true;
	dtup->bt_blkno = tuple->bt_blkno;

	values = palloc(sizeof(Datum) * brdesc->bd_totalstored);
	allnulls = palloc(sizeof(bool) * brdesc->bd_tupdesc->natts);
	hasnulls = palloc(sizeof(bool) * brdesc->bd_tupdesc->natts);

	tp = (char *) tuple + BrinTupleDataOffset(tuple);

	if (BrinTupleHasNulls(tuple))
		nullbits = (bits8 *) ((char *) tuple + SizeOfBrinTuple);
	else
		nullbits = NULL;
	brin_deconstruct_tuple(brdesc,
						   tp, nullbits, BrinTupleHasNulls(tuple),
						   values, allnulls, hasnulls);

	/*
	 * Iterate to assign each of the values to the corresponding item in the
	 * values array of each column.  The copies occur in the tuple's context.
	 */
	oldcxt = MemoryContextSwitchTo(dtup->bt_context);
	for (valueno = 0, keyno = 0; keyno < brdesc->bd_tupdesc->natts; keyno++)
	{
		int			i;

		if (allnulls[keyno])
		{
			valueno += brdesc->bd_info[keyno]->oi_nstored;
			continue;
		}

		/*
		 * We would like to skip datumCopy'ing the values datum in some cases,
		 * caller permitting ...
		 */
		for (i = 0; i < brdesc->bd_info[keyno]->oi_nstored; i++)
			dtup->bt_columns[keyno].bv_values[i] =
				datumCopy(values[valueno++],
						  brdesc->bd_info[keyno]->oi_typcache[i]->typbyval,
						  brdesc->bd_info[keyno]->oi_typcache[i]->typlen);

		dtup->bt_columns[keyno].bv_hasnulls = hasnulls[keyno];
		dtup->bt_columns[keyno].bv_allnulls = false;
	}

	MemoryContextSwitchTo(oldcxt);

	pfree(values);
	pfree(allnulls);
	pfree(hasnulls);

	return dtup;
}

/*
 * brin_deconstruct_tuple
 *		Guts of attribute extraction from an on-disk BRIN tuple.
 *
 * Its arguments are:
 *	brdesc		BRIN descriptor for the stored tuple
 *	tp			pointer to the tuple data area
 *	nullbits	pointer to the tuple nulls bitmask
 *	nulls		"has nulls" bit in tuple infomask
 *	values		output values, array of size brdesc->bd_totalstored
 *	allnulls	output "allnulls", size brdesc->bd_tupdesc->natts
 *	hasnulls	output "hasnulls", size brdesc->bd_tupdesc->natts
 *
 * Output arrays must have been allocated by caller.
 */
static inline void
brin_deconstruct_tuple(BrinDesc *brdesc,
					   char *tp, bits8 *nullbits, bool nulls,
					   Datum *values, bool *allnulls, bool *hasnulls)
{
	int			attnum;
	int			stored;
	TupleDesc	diskdsc;
	long		off;

	/*
	 * First iterate to natts to obtain both null flags for each attribute.
	 * Note that we reverse the sense of the att_isnull test, because we store
	 * 1 for a null value (rather than a 1 for a not null value as is the
	 * att_isnull convention used elsewhere.)  See brin_form_tuple.
	 */
	for (attnum = 0; attnum < brdesc->bd_tupdesc->natts; attnum++)
	{
		/*
		 * the "all nulls" bit means that all values in the page range for
		 * this column are nulls.  Therefore there are no values in the tuple
		 * data area.
		 */
		allnulls[attnum] = nulls && !att_isnull(attnum, nullbits);

		/*
		 * the "has nulls" bit means that some tuples have nulls, but others
		 * have not-null values.  Therefore we know the tuple contains data
		 * for this column.
		 *
		 * The hasnulls bits follow the allnulls bits in the same bitmask.
		 */
		hasnulls[attnum] =
			nulls && !att_isnull(brdesc->bd_tupdesc->natts + attnum, nullbits);
	}

	/*
	 * Iterate to obtain each attribute's stored values.  Note that since we
	 * may reuse attribute entries for more than one column, we cannot cache
	 * offsets here.
	 */
	diskdsc = brtuple_disk_tupdesc(brdesc);
	stored = 0;
	off = 0;
	for (attnum = 0; attnum < brdesc->bd_tupdesc->natts; attnum++)
	{
		int			datumno;

		if (allnulls[attnum])
		{
			stored += brdesc->bd_info[attnum]->oi_nstored;
			continue;
		}

		for (datumno = 0;
			 datumno < brdesc->bd_info[attnum]->oi_nstored;
			 datumno++)
		{
			Form_pg_attribute thisatt = diskdsc->attrs[stored];

			if (thisatt->attlen == -1)
			{
				off = att_align_pointer(off, thisatt->attalign, -1,
										tp + off);
			}
			else
			{
				/* not varlena, so safe to use att_align_nominal */
				off = att_align_nominal(off, thisatt->attalign);
			}

			values[stored++] = fetchatt(thisatt, tp + off);

			off = att_addlength_pointer(off, thisatt->attlen, tp + off);
		}
	}
}
