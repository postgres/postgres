/*-------------------------------------------------------------------------
 *
 * network_gist.c
 *	  GiST support for network types.
 *
 * The key thing to understand about this code is the definition of the
 * "union" of a set of INET/CIDR values.  It works like this:
 * 1. If the values are not all of the same IP address family, the "union"
 * is a dummy value with family number zero, minbits zero, commonbits zero,
 * address all zeroes.  Otherwise:
 * 2. The union has the common IP address family number.
 * 3. The union's minbits value is the smallest netmask length ("ip_bits")
 * of all the input values.
 * 4. Let C be the number of leading address bits that are in common among
 * all the input values (C ranges from 0 to ip_maxbits for the family).
 * 5. The union's commonbits value is C.
 * 6. The union's address value is the same as the common prefix for its
 * first C bits, and is zeroes to the right of that.  The physical width
 * of the address value is ip_maxbits for the address family.
 *
 * In a leaf index entry (representing a single key), commonbits is equal to
 * ip_maxbits for the address family, minbits is the same as the represented
 * value's ip_bits, and the address is equal to the represented address.
 * Although it may appear that we're wasting a byte by storing the union
 * format and not just the represented INET/CIDR value in leaf keys, the
 * extra byte is actually "free" because of alignment considerations.
 *
 * Note that this design tracks minbits and commonbits independently; in any
 * given union value, either might be smaller than the other.  This does not
 * help us much when descending the tree, because of the way inet comparison
 * is defined: at non-leaf nodes we can't compare more than minbits bits
 * even if we know them.  However, it greatly improves the quality of split
 * decisions.  Preliminary testing suggests that searches are as much as
 * twice as fast as for a simpler design in which a single field doubles as
 * the common prefix length and the minimum ip_bits value.
 *
 * Portions Copyright (c) 1996-2020, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/network_gist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/socket.h>

#include "access/gist.h"
#include "access/stratnum.h"
#include "utils/builtins.h"
#include "utils/inet.h"

/*
 * Operator strategy numbers used in the GiST inet_ops opclass
 */
#define INETSTRAT_OVERLAPS		RTOverlapStrategyNumber
#define INETSTRAT_EQ			RTEqualStrategyNumber
#define INETSTRAT_NE			RTNotEqualStrategyNumber
#define INETSTRAT_LT			RTLessStrategyNumber
#define INETSTRAT_LE			RTLessEqualStrategyNumber
#define INETSTRAT_GT			RTGreaterStrategyNumber
#define INETSTRAT_GE			RTGreaterEqualStrategyNumber
#define INETSTRAT_SUB			RTSubStrategyNumber
#define INETSTRAT_SUBEQ			RTSubEqualStrategyNumber
#define INETSTRAT_SUP			RTSuperStrategyNumber
#define INETSTRAT_SUPEQ			RTSuperEqualStrategyNumber


/*
 * Representation of a GiST INET/CIDR index key.  This is not identical to
 * INET/CIDR because we need to keep track of the length of the common address
 * prefix as well as the minimum netmask length.  However, as long as it
 * follows varlena header rules, the core GiST code won't know the difference.
 * For simplicity we always use 1-byte-header varlena format.
 */
typedef struct GistInetKey
{
	uint8		va_header;		/* varlena header --- don't touch directly */
	unsigned char family;		/* PGSQL_AF_INET, PGSQL_AF_INET6, or zero */
	unsigned char minbits;		/* minimum number of bits in netmask */
	unsigned char commonbits;	/* number of common prefix bits in addresses */
	unsigned char ipaddr[16];	/* up to 128 bits of common address */
} GistInetKey;

#define DatumGetInetKeyP(X) ((GistInetKey *) DatumGetPointer(X))
#define InetKeyPGetDatum(X) PointerGetDatum(X)

/*
 * Access macros; not really exciting, but we use these for notational
 * consistency with access to INET/CIDR values.  Note that family-zero values
 * are stored with 4 bytes of address, not 16.
 */
#define gk_ip_family(gkptr)		((gkptr)->family)
#define gk_ip_minbits(gkptr)	((gkptr)->minbits)
#define gk_ip_commonbits(gkptr) ((gkptr)->commonbits)
#define gk_ip_addr(gkptr)		((gkptr)->ipaddr)
#define ip_family_maxbits(fam)	((fam) == PGSQL_AF_INET6 ? 128 : 32)

/* These require that the family field has been set: */
#define gk_ip_addrsize(gkptr) \
	(gk_ip_family(gkptr) == PGSQL_AF_INET6 ? 16 : 4)
#define gk_ip_maxbits(gkptr) \
	ip_family_maxbits(gk_ip_family(gkptr))
#define SET_GK_VARSIZE(dst) \
	SET_VARSIZE_SHORT(dst, offsetof(GistInetKey, ipaddr) + gk_ip_addrsize(dst))


/*
 * The GiST query consistency check
 */
Datum
inet_gist_consistent(PG_FUNCTION_ARGS)
{
	GISTENTRY  *ent = (GISTENTRY *) PG_GETARG_POINTER(0);
	inet	   *query = PG_GETARG_INET_PP(1);
	StrategyNumber strategy = (StrategyNumber) PG_GETARG_UINT16(2);

	/* Oid		subtype = PG_GETARG_OID(3); */
	bool	   *recheck = (bool *) PG_GETARG_POINTER(4);
	GistInetKey *key = DatumGetInetKeyP(ent->key);
	int			minbits,
				order;

	/* All operators served by this function are exact. */
	*recheck = false;

	/*
	 * Check 0: different families
	 *
	 * If key represents multiple address families, its children could match
	 * anything.  This can only happen on an inner index page.
	 */
	if (gk_ip_family(key) == 0)
	{
		Assert(!GIST_LEAF(ent));
		PG_RETURN_BOOL(true);
	}

	/*
	 * Check 1: different families
	 *
	 * Matching families do not help any of the strategies.
	 */
	if (gk_ip_family(key) != ip_family(query))
	{
		switch (strategy)
		{
			case INETSTRAT_LT:
			case INETSTRAT_LE:
				if (gk_ip_family(key) < ip_family(query))
					PG_RETURN_BOOL(true);
				break;

			case INETSTRAT_GE:
			case INETSTRAT_GT:
				if (gk_ip_family(key) > ip_family(query))
					PG_RETURN_BOOL(true);
				break;

			case INETSTRAT_NE:
				PG_RETURN_BOOL(true);
		}
		/* For all other cases, we can be sure there is no match */
		PG_RETURN_BOOL(false);
	}

	/*
	 * Check 2: network bit count
	 *
	 * Network bit count (ip_bits) helps to check leaves for sub network and
	 * sup network operators.  At non-leaf nodes, we know every child value
	 * has ip_bits >= gk_ip_minbits(key), so we can avoid descending in some
	 * cases too.
	 */
	switch (strategy)
	{
		case INETSTRAT_SUB:
			if (GIST_LEAF(ent) && gk_ip_minbits(key) <= ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_SUBEQ:
			if (GIST_LEAF(ent) && gk_ip_minbits(key) < ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_SUPEQ:
		case INETSTRAT_EQ:
			if (gk_ip_minbits(key) > ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_SUP:
			if (gk_ip_minbits(key) >= ip_bits(query))
				PG_RETURN_BOOL(false);
			break;
	}

	/*
	 * Check 3: common network bits
	 *
	 * Compare available common prefix bits to the query, but not beyond
	 * either the query's netmask or the minimum netmask among the represented
	 * values.  If these bits don't match the query, we have our answer (and
	 * may or may not need to descend, depending on the operator).  If they do
	 * match, and we are not at a leaf, we descend in all cases.
	 *
	 * Note this is the final check for operators that only consider the
	 * network part of the address.
	 */
	minbits = Min(gk_ip_commonbits(key), gk_ip_minbits(key));
	minbits = Min(minbits, ip_bits(query));

	order = bitncmp(gk_ip_addr(key), ip_addr(query), minbits);

	switch (strategy)
	{
		case INETSTRAT_SUB:
		case INETSTRAT_SUBEQ:
		case INETSTRAT_OVERLAPS:
		case INETSTRAT_SUPEQ:
		case INETSTRAT_SUP:
			PG_RETURN_BOOL(order == 0);

		case INETSTRAT_LT:
		case INETSTRAT_LE:
			if (order > 0)
				PG_RETURN_BOOL(false);
			if (order < 0 || !GIST_LEAF(ent))
				PG_RETURN_BOOL(true);
			break;

		case INETSTRAT_EQ:
			if (order != 0)
				PG_RETURN_BOOL(false);
			if (!GIST_LEAF(ent))
				PG_RETURN_BOOL(true);
			break;

		case INETSTRAT_GE:
		case INETSTRAT_GT:
			if (order < 0)
				PG_RETURN_BOOL(false);
			if (order > 0 || !GIST_LEAF(ent))
				PG_RETURN_BOOL(true);
			break;

		case INETSTRAT_NE:
			if (order != 0 || !GIST_LEAF(ent))
				PG_RETURN_BOOL(true);
			break;
	}

	/*
	 * Remaining checks are only for leaves and basic comparison strategies.
	 * See network_cmp_internal() in network.c for the implementation we need
	 * to match.  Note that in a leaf key, commonbits should equal the address
	 * length, so we compared the whole network parts above.
	 */
	Assert(GIST_LEAF(ent));

	/*
	 * Check 4: network bit count
	 *
	 * Next step is to compare netmask widths.
	 */
	switch (strategy)
	{
		case INETSTRAT_LT:
		case INETSTRAT_LE:
			if (gk_ip_minbits(key) < ip_bits(query))
				PG_RETURN_BOOL(true);
			if (gk_ip_minbits(key) > ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_EQ:
			if (gk_ip_minbits(key) != ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_GE:
		case INETSTRAT_GT:
			if (gk_ip_minbits(key) > ip_bits(query))
				PG_RETURN_BOOL(true);
			if (gk_ip_minbits(key) < ip_bits(query))
				PG_RETURN_BOOL(false);
			break;

		case INETSTRAT_NE:
			if (gk_ip_minbits(key) != ip_bits(query))
				PG_RETURN_BOOL(true);
			break;
	}

	/*
	 * Check 5: whole address
	 *
	 * Netmask bit counts are the same, so check all the address bits.
	 */
	order = bitncmp(gk_ip_addr(key), ip_addr(query), gk_ip_maxbits(key));

	switch (strategy)
	{
		case INETSTRAT_LT:
			PG_RETURN_BOOL(order < 0);

		case INETSTRAT_LE:
			PG_RETURN_BOOL(order <= 0);

		case INETSTRAT_EQ:
			PG_RETURN_BOOL(order == 0);

		case INETSTRAT_GE:
			PG_RETURN_BOOL(order >= 0);

		case INETSTRAT_GT:
			PG_RETURN_BOOL(order > 0);

		case INETSTRAT_NE:
			PG_RETURN_BOOL(order != 0);
	}

	elog(ERROR, "unknown strategy for inet GiST");
	PG_RETURN_BOOL(false);		/* keep compiler quiet */
}

/*
 * Calculate parameters of the union of some GistInetKeys.
 *
 * Examine the keys in elements m..n inclusive of the GISTENTRY array,
 * and compute these output parameters:
 * *minfamily_p = minimum IP address family number
 * *maxfamily_p = maximum IP address family number
 * *minbits_p = minimum netmask width
 * *commonbits_p = number of leading bits in common among the addresses
 *
 * minbits and commonbits are forced to zero if there's more than one
 * address family.
 */
static void
calc_inet_union_params(GISTENTRY *ent,
					   int m, int n,
					   int *minfamily_p,
					   int *maxfamily_p,
					   int *minbits_p,
					   int *commonbits_p)
{
	int			minfamily,
				maxfamily,
				minbits,
				commonbits;
	unsigned char *addr;
	GistInetKey *tmp;
	int			i;

	/* Must be at least one key. */
	Assert(m <= n);

	/* Initialize variables using the first key. */
	tmp = DatumGetInetKeyP(ent[m].key);
	minfamily = maxfamily = gk_ip_family(tmp);
	minbits = gk_ip_minbits(tmp);
	commonbits = gk_ip_commonbits(tmp);
	addr = gk_ip_addr(tmp);

	/* Scan remaining keys. */
	for (i = m + 1; i <= n; i++)
	{
		tmp = DatumGetInetKeyP(ent[i].key);

		/* Determine range of family numbers */
		if (minfamily > gk_ip_family(tmp))
			minfamily = gk_ip_family(tmp);
		if (maxfamily < gk_ip_family(tmp))
			maxfamily = gk_ip_family(tmp);

		/* Find minimum minbits */
		if (minbits > gk_ip_minbits(tmp))
			minbits = gk_ip_minbits(tmp);

		/* Find minimum number of bits in common */
		if (commonbits > gk_ip_commonbits(tmp))
			commonbits = gk_ip_commonbits(tmp);
		if (commonbits > 0)
			commonbits = bitncommon(addr, gk_ip_addr(tmp), commonbits);
	}

	/* Force minbits/commonbits to zero if more than one family. */
	if (minfamily != maxfamily)
		minbits = commonbits = 0;

	*minfamily_p = minfamily;
	*maxfamily_p = maxfamily;
	*minbits_p = minbits;
	*commonbits_p = commonbits;
}

/*
 * Same as above, but the GISTENTRY elements to examine are those with
 * indices listed in the offsets[] array.
 */
static void
calc_inet_union_params_indexed(GISTENTRY *ent,
							   OffsetNumber *offsets, int noffsets,
							   int *minfamily_p,
							   int *maxfamily_p,
							   int *minbits_p,
							   int *commonbits_p)
{
	int			minfamily,
				maxfamily,
				minbits,
				commonbits;
	unsigned char *addr;
	GistInetKey *tmp;
	int			i;

	/* Must be at least one key. */
	Assert(noffsets > 0);

	/* Initialize variables using the first key. */
	tmp = DatumGetInetKeyP(ent[offsets[0]].key);
	minfamily = maxfamily = gk_ip_family(tmp);
	minbits = gk_ip_minbits(tmp);
	commonbits = gk_ip_commonbits(tmp);
	addr = gk_ip_addr(tmp);

	/* Scan remaining keys. */
	for (i = 1; i < noffsets; i++)
	{
		tmp = DatumGetInetKeyP(ent[offsets[i]].key);

		/* Determine range of family numbers */
		if (minfamily > gk_ip_family(tmp))
			minfamily = gk_ip_family(tmp);
		if (maxfamily < gk_ip_family(tmp))
			maxfamily = gk_ip_family(tmp);

		/* Find minimum minbits */
		if (minbits > gk_ip_minbits(tmp))
			minbits = gk_ip_minbits(tmp);

		/* Find minimum number of bits in common */
		if (commonbits > gk_ip_commonbits(tmp))
			commonbits = gk_ip_commonbits(tmp);
		if (commonbits > 0)
			commonbits = bitncommon(addr, gk_ip_addr(tmp), commonbits);
	}

	/* Force minbits/commonbits to zero if more than one family. */
	if (minfamily != maxfamily)
		minbits = commonbits = 0;

	*minfamily_p = minfamily;
	*maxfamily_p = maxfamily;
	*minbits_p = minbits;
	*commonbits_p = commonbits;
}

/*
 * Construct a GistInetKey representing a union value.
 *
 * Inputs are the family/minbits/commonbits values to use, plus a pointer to
 * the address field of one of the union inputs.  (Since we're going to copy
 * just the bits-in-common, it doesn't matter which one.)
 */
static GistInetKey *
build_inet_union_key(int family, int minbits, int commonbits,
					 unsigned char *addr)
{
	GistInetKey *result;

	/* Make sure any unused bits are zeroed. */
	result = (GistInetKey *) palloc0(sizeof(GistInetKey));

	gk_ip_family(result) = family;
	gk_ip_minbits(result) = minbits;
	gk_ip_commonbits(result) = commonbits;

	/* Clone appropriate bytes of the address. */
	if (commonbits > 0)
		memcpy(gk_ip_addr(result), addr, (commonbits + 7) / 8);

	/* Clean any unwanted bits in the last partial byte. */
	if (commonbits % 8 != 0)
		gk_ip_addr(result)[commonbits / 8] &= ~(0xFF >> (commonbits % 8));

	/* Set varlena header correctly. */
	SET_GK_VARSIZE(result);

	return result;
}


/*
 * The GiST union function
 *
 * See comments at head of file for the definition of the union.
 */
Datum
inet_gist_union(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GISTENTRY  *ent = entryvec->vector;
	int			minfamily,
				maxfamily,
				minbits,
				commonbits;
	unsigned char *addr;
	GistInetKey *tmp,
			   *result;

	/* Determine parameters of the union. */
	calc_inet_union_params(ent, 0, entryvec->n - 1,
						   &minfamily, &maxfamily,
						   &minbits, &commonbits);

	/* If more than one family, emit family number zero. */
	if (minfamily != maxfamily)
		minfamily = 0;

	/* Initialize address using the first key. */
	tmp = DatumGetInetKeyP(ent[0].key);
	addr = gk_ip_addr(tmp);

	/* Construct the union value. */
	result = build_inet_union_key(minfamily, minbits, commonbits, addr);

	PG_RETURN_POINTER(result);
}

/*
 * The GiST compress function
 *
 * Convert an inet value to GistInetKey.
 */
Datum
inet_gist_compress(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *retval;

	if (entry->leafkey)
	{
		retval = palloc(sizeof(GISTENTRY));
		if (DatumGetPointer(entry->key) != NULL)
		{
			inet	   *in = DatumGetInetPP(entry->key);
			GistInetKey *r;

			r = (GistInetKey *) palloc0(sizeof(GistInetKey));

			gk_ip_family(r) = ip_family(in);
			gk_ip_minbits(r) = ip_bits(in);
			gk_ip_commonbits(r) = gk_ip_maxbits(r);
			memcpy(gk_ip_addr(r), ip_addr(in), gk_ip_addrsize(r));
			SET_GK_VARSIZE(r);

			gistentryinit(*retval, PointerGetDatum(r),
						  entry->rel, entry->page,
						  entry->offset, false);
		}
		else
		{
			gistentryinit(*retval, (Datum) 0,
						  entry->rel, entry->page,
						  entry->offset, false);
		}
	}
	else
		retval = entry;
	PG_RETURN_POINTER(retval);
}

/*
 * We do not need a decompress function, because the other GiST inet
 * support functions work with the GistInetKey representation.
 */

/*
 * The GiST fetch function
 *
 * Reconstruct the original inet datum from a GistInetKey.
 */
Datum
inet_gist_fetch(PG_FUNCTION_ARGS)
{
	GISTENTRY  *entry = (GISTENTRY *) PG_GETARG_POINTER(0);
	GistInetKey *key = DatumGetInetKeyP(entry->key);
	GISTENTRY  *retval;
	inet	   *dst;

	dst = (inet *) palloc0(sizeof(inet));

	ip_family(dst) = gk_ip_family(key);
	ip_bits(dst) = gk_ip_minbits(key);
	memcpy(ip_addr(dst), gk_ip_addr(key), ip_addrsize(dst));
	SET_INET_VARSIZE(dst);

	retval = palloc(sizeof(GISTENTRY));
	gistentryinit(*retval, InetPGetDatum(dst), entry->rel, entry->page,
				  entry->offset, false);

	PG_RETURN_POINTER(retval);
}

/*
 * The GiST page split penalty function
 *
 * Charge a large penalty if address family doesn't match, or a somewhat
 * smaller one if the new value would degrade the union's minbits
 * (minimum netmask width).  Otherwise, penalty is inverse of the
 * new number of common address bits.
 */
Datum
inet_gist_penalty(PG_FUNCTION_ARGS)
{
	GISTENTRY  *origent = (GISTENTRY *) PG_GETARG_POINTER(0);
	GISTENTRY  *newent = (GISTENTRY *) PG_GETARG_POINTER(1);
	float	   *penalty = (float *) PG_GETARG_POINTER(2);
	GistInetKey *orig = DatumGetInetKeyP(origent->key),
			   *new = DatumGetInetKeyP(newent->key);
	int			commonbits;

	if (gk_ip_family(orig) == gk_ip_family(new))
	{
		if (gk_ip_minbits(orig) <= gk_ip_minbits(new))
		{
			commonbits = bitncommon(gk_ip_addr(orig), gk_ip_addr(new),
									Min(gk_ip_commonbits(orig),
										gk_ip_commonbits(new)));
			if (commonbits > 0)
				*penalty = 1.0f / commonbits;
			else
				*penalty = 2;
		}
		else
			*penalty = 3;
	}
	else
		*penalty = 4;

	PG_RETURN_POINTER(penalty);
}

/*
 * The GiST PickSplit method
 *
 * There are two ways to split. First one is to split by address families,
 * if there are multiple families appearing in the input.
 *
 * The second and more common way is to split by addresses. To achieve this,
 * determine the number of leading bits shared by all the keys, then split on
 * the next bit.  (We don't currently consider the netmask widths while doing
 * this; should we?)  If we fail to get a nontrivial split that way, split
 * 50-50.
 */
Datum
inet_gist_picksplit(PG_FUNCTION_ARGS)
{
	GistEntryVector *entryvec = (GistEntryVector *) PG_GETARG_POINTER(0);
	GIST_SPLITVEC *splitvec = (GIST_SPLITVEC *) PG_GETARG_POINTER(1);
	GISTENTRY  *ent = entryvec->vector;
	int			minfamily,
				maxfamily,
				minbits,
				commonbits;
	unsigned char *addr;
	GistInetKey *tmp,
			   *left_union,
			   *right_union;
	int			maxoff,
				nbytes;
	OffsetNumber i,
			   *left,
			   *right;

	maxoff = entryvec->n - 1;
	nbytes = (maxoff + 1) * sizeof(OffsetNumber);

	left = (OffsetNumber *) palloc(nbytes);
	right = (OffsetNumber *) palloc(nbytes);

	splitvec->spl_left = left;
	splitvec->spl_right = right;

	splitvec->spl_nleft = 0;
	splitvec->spl_nright = 0;

	/* Determine parameters of the union of all the inputs. */
	calc_inet_union_params(ent, FirstOffsetNumber, maxoff,
						   &minfamily, &maxfamily,
						   &minbits, &commonbits);

	if (minfamily != maxfamily)
	{
		/* Multiple families, so split by family. */
		for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
		{
			/*
			 * If there's more than 2 families, all but maxfamily go into the
			 * left union.  This could only happen if the inputs include some
			 * IPv4, some IPv6, and some already-multiple-family unions.
			 */
			tmp = DatumGetInetKeyP(ent[i].key);
			if (gk_ip_family(tmp) != maxfamily)
				left[splitvec->spl_nleft++] = i;
			else
				right[splitvec->spl_nright++] = i;
		}
	}
	else
	{
		/*
		 * Split on the next bit after the common bits.  If that yields a
		 * trivial split, try the next bit position to the right.  Repeat till
		 * success; or if we run out of bits, do an arbitrary 50-50 split.
		 */
		int			maxbits = ip_family_maxbits(minfamily);

		while (commonbits < maxbits)
		{
			/* Split using the commonbits'th bit position. */
			int			bitbyte = commonbits / 8;
			int			bitmask = 0x80 >> (commonbits % 8);

			splitvec->spl_nleft = splitvec->spl_nright = 0;

			for (i = FirstOffsetNumber; i <= maxoff; i = OffsetNumberNext(i))
			{
				tmp = DatumGetInetKeyP(ent[i].key);
				addr = gk_ip_addr(tmp);
				if ((addr[bitbyte] & bitmask) == 0)
					left[splitvec->spl_nleft++] = i;
				else
					right[splitvec->spl_nright++] = i;
			}

			if (splitvec->spl_nleft > 0 && splitvec->spl_nright > 0)
				break;			/* success */
			commonbits++;
		}

		if (commonbits >= maxbits)
		{
			/* Failed ... do a 50-50 split. */
			splitvec->spl_nleft = splitvec->spl_nright = 0;

			for (i = FirstOffsetNumber; i <= maxoff / 2; i = OffsetNumberNext(i))
			{
				left[splitvec->spl_nleft++] = i;
			}
			for (; i <= maxoff; i = OffsetNumberNext(i))
			{
				right[splitvec->spl_nright++] = i;
			}
		}
	}

	/*
	 * Compute the union value for each side from scratch.  In most cases we
	 * could approximate the union values with what we already know, but this
	 * ensures that each side has minbits and commonbits set as high as
	 * possible.
	 */
	calc_inet_union_params_indexed(ent, left, splitvec->spl_nleft,
								   &minfamily, &maxfamily,
								   &minbits, &commonbits);
	if (minfamily != maxfamily)
		minfamily = 0;
	tmp = DatumGetInetKeyP(ent[left[0]].key);
	addr = gk_ip_addr(tmp);
	left_union = build_inet_union_key(minfamily, minbits, commonbits, addr);
	splitvec->spl_ldatum = PointerGetDatum(left_union);

	calc_inet_union_params_indexed(ent, right, splitvec->spl_nright,
								   &minfamily, &maxfamily,
								   &minbits, &commonbits);
	if (minfamily != maxfamily)
		minfamily = 0;
	tmp = DatumGetInetKeyP(ent[right[0]].key);
	addr = gk_ip_addr(tmp);
	right_union = build_inet_union_key(minfamily, minbits, commonbits, addr);
	splitvec->spl_rdatum = PointerGetDatum(right_union);

	PG_RETURN_POINTER(splitvec);
}

/*
 * The GiST equality function
 */
Datum
inet_gist_same(PG_FUNCTION_ARGS)
{
	GistInetKey *left = DatumGetInetKeyP(PG_GETARG_DATUM(0));
	GistInetKey *right = DatumGetInetKeyP(PG_GETARG_DATUM(1));
	bool	   *result = (bool *) PG_GETARG_POINTER(2);

	*result = (gk_ip_family(left) == gk_ip_family(right) &&
			   gk_ip_minbits(left) == gk_ip_minbits(right) &&
			   gk_ip_commonbits(left) == gk_ip_commonbits(right) &&
			   memcmp(gk_ip_addr(left), gk_ip_addr(right),
					  gk_ip_addrsize(left)) == 0);

	PG_RETURN_POINTER(result);
}
