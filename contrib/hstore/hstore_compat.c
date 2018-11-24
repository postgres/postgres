/*
 * contrib/hstore/hstore_compat.c
 *
 * Notes on old/new hstore format disambiguation.
 *
 * There are three formats to consider:
 * 1) old contrib/hstore (referred to as hstore-old)
 * 2) prerelease pgfoundry hstore
 * 3) new contrib/hstore
 *
 * (2) and (3) are identical except for the HS_FLAG_NEWVERSION
 * bit, which is set in (3) but not (2).
 *
 * Values that are already in format (3), or which are
 * unambiguously in format (2), are handled by the first
 * "return immediately" test in hstoreUpgrade().
 *
 * To stress a point: we ONLY get here with possibly-ambiguous
 * values if we're doing some sort of in-place migration from an
 * old prerelease pgfoundry hstore-new; and we explicitly don't
 * support that without fixing up any potentially padded values
 * first. Most of the code here is serious overkill, but the
 * performance penalty isn't serious (especially compared to the
 * palloc() that we have to do anyway) and the belt-and-braces
 * validity checks provide some reassurance. (If for some reason
 * we get a value that would have worked on the old code, but
 * which would be botched by the conversion code, the validity
 * checks will fail it first so we get an error rather than bad
 * data.)
 *
 * Note also that empty hstores are the same in (2) and (3), so
 * there are some special-case paths for them.
 *
 * We tell the difference between formats (2) and (3) as follows (but
 * note that there are some edge cases where we can't tell; see
 * comments in hstoreUpgrade):
 *
 * First, since there must be at least one entry, we look at
 * how the bits line up. The new format looks like:
 *
 * 10kkkkkkkkkkkkkkkkkkkkkkkkkkkkkk  (k..k = keylen)
 * 0nvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv  (v..v = keylen+vallen)
 *
 * The old format looks like one of these, depending on endianness
 * and bitfield layout: (k..k = keylen, v..v = vallen, p..p = pos,
 * n = isnull)
 *
 * kkkkkkkkkkkkkkkkvvvvvvvvvvvvvvvv
 * nppppppppppppppppppppppppppppppp
 *
 * kkkkkkkkkkkkkkkkvvvvvvvvvvvvvvvv
 * pppppppppppppppppppppppppppppppn
 *
 * vvvvvvvvvvvvvvvvkkkkkkkkkkkkkkkk
 * nppppppppppppppppppppppppppppppp
 *
 * vvvvvvvvvvvvvvvvkkkkkkkkkkkkkkkk
 * pppppppppppppppppppppppppppppppn   (usual i386 format)
 *
 * If the entry is in old format, for the first entry "pos" must be 0.
 * We can obviously see that either keylen or vallen must be >32768
 * for there to be any ambiguity (which is why lengths less than that
 * are fasttracked in hstore.h) Since "pos"==0, the "v" field in the
 * new-format interpretation can only be 0 or 1, which constrains all
 * but three bits of the old-format's k and v fields. But in addition
 * to all of this, the data length implied by the keylen and vallen
 * must fit in the varlena size. So the only ambiguous edge case for
 * hstores with only one entry occurs between a new-format entry with
 * an excess (~32k) of padding, and an old-format entry. But we know
 * which format to use in that case based on how we were compiled, so
 * no actual data corruption can occur.
 *
 * If there is more than one entry, the requirement that keys do not
 * decrease in length, and that positions increase contiguously, and
 * that the end of the data not be beyond the end of the varlena
 * itself, disambiguates in almost all other cases. There is a small
 * set of ambiguous cases which could occur if the old-format value
 * has a large excess of padding and just the right pattern of key
 * sizes, but these are also handled based on how we were compiled.
 *
 * The otherwise undocumented function hstore_version_diag is provided
 * for testing purposes.
 */
#include "postgres.h"


#include "hstore.h"

/*
 * This is the structure used for entries in the old contrib/hstore
 * implementation. Notice that this is the same size as the new entry
 * (two 32-bit words per key/value pair) and that the header is the
 * same, so the old and new versions of ARRPTR, STRPTR, CALCDATASIZE
 * etc. are compatible.
 *
 * If the above statement isn't true on some bizarre platform, we're
 * a bit hosed (see StaticAssertStmt in hstoreValidOldFormat).
 */
typedef struct
{
	uint16		keylen;
	uint16		vallen;
	uint32
				valisnull:1,
				pos:31;
} HOldEntry;

static int	hstoreValidNewFormat(HStore *hs);
static int	hstoreValidOldFormat(HStore *hs);


/*
 * Validity test for a new-format hstore.
 *	0 = not valid
 *	1 = valid but with "slop" in the length
 *	2 = exactly valid
 */
static int
hstoreValidNewFormat(HStore *hs)
{
	int			count = HS_COUNT(hs);
	HEntry	   *entries = ARRPTR(hs);
	int			buflen = (count) ? HSE_ENDPOS(entries[2 * (count) - 1]) : 0;
	int			vsize = CALCDATASIZE(count, buflen);
	int			i;

	if (hs->size_ & HS_FLAG_NEWVERSION)
		return 2;

	if (count == 0)
		return 2;

	if (!HSE_ISFIRST(entries[0]))
		return 0;

	if (vsize > VARSIZE(hs))
		return 0;

	/* entry position must be nondecreasing */

	for (i = 1; i < 2 * count; ++i)
	{
		if (HSE_ISFIRST(entries[i])
			|| (HSE_ENDPOS(entries[i]) < HSE_ENDPOS(entries[i - 1])))
			return 0;
	}

	/* key length must be nondecreasing and keys must not be null */

	for (i = 1; i < count; ++i)
	{
		if (HS_KEYLEN(entries, i) < HS_KEYLEN(entries, i - 1))
			return 0;
		if (HSE_ISNULL(entries[2 * i]))
			return 0;
	}

	if (vsize != VARSIZE(hs))
		return 1;

	return 2;
}

/*
 * Validity test for an old-format hstore.
 *	0 = not valid
 *	1 = valid but with "slop" in the length
 *	2 = exactly valid
 */
static int
hstoreValidOldFormat(HStore *hs)
{
	int			count = hs->size_;
	HOldEntry  *entries = (HOldEntry *) ARRPTR(hs);
	int			vsize;
	int			lastpos = 0;
	int			i;

	if (hs->size_ & HS_FLAG_NEWVERSION)
		return 0;

	/* New format uses an HEntry for key and another for value */
	StaticAssertStmt(sizeof(HOldEntry) == 2 * sizeof(HEntry),
					 "old hstore format is not upward-compatible");

	if (count == 0)
		return 2;

	if (count > 0xFFFFFFF)
		return 0;

	if (CALCDATASIZE(count, 0) > VARSIZE(hs))
		return 0;

	if (entries[0].pos != 0)
		return 0;

	/* key length must be nondecreasing */

	for (i = 1; i < count; ++i)
	{
		if (entries[i].keylen < entries[i - 1].keylen)
			return 0;
	}

	/*
	 * entry position must be strictly increasing, except for the first entry
	 * (which can be ""=>"" and thus zero-length); and all entries must be
	 * properly contiguous
	 */

	for (i = 0; i < count; ++i)
	{
		if (entries[i].pos != lastpos)
			return 0;
		lastpos += (entries[i].keylen
					+ ((entries[i].valisnull) ? 0 : entries[i].vallen));
	}

	vsize = CALCDATASIZE(count, lastpos);

	if (vsize > VARSIZE(hs))
		return 0;

	if (vsize != VARSIZE(hs))
		return 1;

	return 2;
}


/*
 * hstoreUpgrade: PG_DETOAST_DATUM plus support for conversion of old hstores
 */
HStore *
hstoreUpgrade(Datum orig)
{
	HStore	   *hs = (HStore *) PG_DETOAST_DATUM(orig);
	int			valid_new;
	int			valid_old;

	/* Return immediately if no conversion needed */
	if (hs->size_ & HS_FLAG_NEWVERSION)
		return hs;

	/* Do we have a writable copy? If not, make one. */
	if ((void *) hs == (void *) DatumGetPointer(orig))
		hs = (HStore *) PG_DETOAST_DATUM_COPY(orig);

	if (hs->size_ == 0 ||
		(VARSIZE(hs) < 32768 && HSE_ISFIRST((ARRPTR(hs)[0]))))
	{
		HS_SETCOUNT(hs, HS_COUNT(hs));
		HS_FIXSIZE(hs, HS_COUNT(hs));
		return hs;
	}

	valid_new = hstoreValidNewFormat(hs);
	valid_old = hstoreValidOldFormat(hs);

	if (!valid_old || hs->size_ == 0)
	{
		if (valid_new)
		{
			/*
			 * force the "new version" flag and the correct varlena length.
			 */
			HS_SETCOUNT(hs, HS_COUNT(hs));
			HS_FIXSIZE(hs, HS_COUNT(hs));
			return hs;
		}
		else
		{
			elog(ERROR, "invalid hstore value found");
		}
	}

	/*
	 * this is the tricky edge case. It is only possible in some quite extreme
	 * cases (the hstore must have had a lot of wasted padding space at the
	 * end). But the only way a "new" hstore value could get here is if we're
	 * upgrading in place from a pre-release version of hstore-new (NOT
	 * contrib/hstore), so we work off the following assumptions: 1. If you're
	 * moving from old contrib/hstore to hstore-new, you're required to fix up
	 * any potential conflicts first, e.g. by running ALTER TABLE ... USING
	 * col::text::hstore; on all hstore columns before upgrading. 2. If you're
	 * moving from old contrib/hstore to new contrib/hstore, then "new" values
	 * are impossible here 3. If you're moving from pre-release hstore-new to
	 * hstore-new, then "old" values are impossible here 4. If you're moving
	 * from pre-release hstore-new to new contrib/hstore, you're not doing so
	 * as an in-place upgrade, so there is no issue So the upshot of all this
	 * is that we can treat all the edge cases as "new" if we're being built
	 * as hstore-new, and "old" if we're being built as contrib/hstore.
	 *
	 * XXX the WARNING can probably be downgraded to DEBUG1 once this has been
	 * beta-tested. But for now, it would be very useful to know if anyone can
	 * actually reach this case in a non-contrived setting.
	 */

	if (valid_new)
	{
#if HSTORE_IS_HSTORE_NEW
		elog(WARNING, "ambiguous hstore value resolved as hstore-new");

		/*
		 * force the "new version" flag and the correct varlena length.
		 */
		HS_SETCOUNT(hs, HS_COUNT(hs));
		HS_FIXSIZE(hs, HS_COUNT(hs));
		return hs;
#else
		elog(WARNING, "ambiguous hstore value resolved as hstore-old");
#endif
	}

	/*
	 * must have an old-style value. Overwrite it in place as a new-style one.
	 */
	{
		int			count = hs->size_;
		HEntry	   *new_entries = ARRPTR(hs);
		HOldEntry  *old_entries = (HOldEntry *) ARRPTR(hs);
		int			i;

		for (i = 0; i < count; ++i)
		{
			uint32		pos = old_entries[i].pos;
			uint32		keylen = old_entries[i].keylen;
			uint32		vallen = old_entries[i].vallen;
			bool		isnull = old_entries[i].valisnull;

			if (isnull)
				vallen = 0;

			new_entries[2 * i].entry = (pos + keylen) & HENTRY_POSMASK;
			new_entries[2 * i + 1].entry = (((pos + keylen + vallen) & HENTRY_POSMASK)
											| ((isnull) ? HENTRY_ISNULL : 0));
		}

		if (count)
			new_entries[0].entry |= HENTRY_ISFIRST;
		HS_SETCOUNT(hs, count);
		HS_FIXSIZE(hs, count);
	}

	return hs;
}


PG_FUNCTION_INFO_V1(hstore_version_diag);
Datum
hstore_version_diag(PG_FUNCTION_ARGS)
{
	HStore	   *hs = (HStore *) PG_DETOAST_DATUM(PG_GETARG_DATUM(0));
	int			valid_new = hstoreValidNewFormat(hs);
	int			valid_old = hstoreValidOldFormat(hs);

	PG_RETURN_INT32(valid_old * 10 + valid_new);
}
