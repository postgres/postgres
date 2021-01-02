/*-------------------------------------------------------------------------
 *
 * network_spgist.c
 *	  SP-GiST support for network types.
 *
 * We split inet index entries first by address family (IPv4 or IPv6).
 * If the entries below a given inner tuple are all of the same family,
 * we identify their common prefix and split by the next bit of the address,
 * and by whether their masklens exceed the length of the common prefix.
 *
 * An inner tuple that has both IPv4 and IPv6 children has a null prefix
 * and exactly two nodes, the first being for IPv4 and the second for IPv6.
 *
 * Otherwise, the prefix is a CIDR value representing the common prefix,
 * and there are exactly four nodes.  Node numbers 0 and 1 are for addresses
 * with the same masklen as the prefix, while node numbers 2 and 3 are for
 * addresses with larger masklen.  (We do not allow a tuple to contain
 * entries with masklen smaller than its prefix's.)  Node numbers 0 and 1
 * are distinguished by the next bit of the address after the common prefix,
 * and likewise for node numbers 2 and 3.  If there are no more bits in
 * the address family, everything goes into node 0 (which will probably
 * lead to creating an allTheSame tuple).
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/network_spgist.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/socket.h>

#include "access/spgist.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/inet.h"


static int	inet_spg_node_number(const inet *val, int commonbits);
static int	inet_spg_consistent_bitmap(const inet *prefix, int nkeys,
									   ScanKey scankeys, bool leaf);

/*
 * The SP-GiST configuration function
 */
Datum
inet_spg_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = CIDROID;
	cfg->labelType = VOIDOID;
	cfg->canReturnData = true;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

/*
 * The SP-GiST choose function
 */
Datum
inet_spg_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	inet	   *val = DatumGetInetPP(in->datum),
			   *prefix;
	int			commonbits;

	/*
	 * If we're looking at a tuple that splits by address family, choose the
	 * appropriate subnode.
	 */
	if (!in->hasPrefix)
	{
		/* allTheSame isn't possible for such a tuple */
		Assert(!in->allTheSame);
		Assert(in->nNodes == 2);

		out->resultType = spgMatchNode;
		out->result.matchNode.nodeN = (ip_family(val) == PGSQL_AF_INET) ? 0 : 1;
		out->result.matchNode.restDatum = InetPGetDatum(val);

		PG_RETURN_VOID();
	}

	/* Else it must split by prefix */
	Assert(in->nNodes == 4 || in->allTheSame);

	prefix = DatumGetInetPP(in->prefixDatum);
	commonbits = ip_bits(prefix);

	/*
	 * We cannot put addresses from different families under the same inner
	 * node, so we have to split if the new value's family is different.
	 */
	if (ip_family(val) != ip_family(prefix))
	{
		/* Set up 2-node tuple */
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = false;
		out->result.splitTuple.prefixNNodes = 2;
		out->result.splitTuple.prefixNodeLabels = NULL;

		/* Identify which node the existing data goes into */
		out->result.splitTuple.childNodeN =
			(ip_family(prefix) == PGSQL_AF_INET) ? 0 : 1;

		out->result.splitTuple.postfixHasPrefix = true;
		out->result.splitTuple.postfixPrefixDatum = InetPGetDatum(prefix);

		PG_RETURN_VOID();
	}

	/*
	 * If the new value does not match the existing prefix, we have to split.
	 */
	if (ip_bits(val) < commonbits ||
		bitncmp(ip_addr(prefix), ip_addr(val), commonbits) != 0)
	{
		/* Determine new prefix length for the split tuple */
		commonbits = bitncommon(ip_addr(prefix), ip_addr(val),
								Min(ip_bits(val), commonbits));

		/* Set up 4-node tuple */
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = true;
		out->result.splitTuple.prefixPrefixDatum =
			InetPGetDatum(cidr_set_masklen_internal(val, commonbits));
		out->result.splitTuple.prefixNNodes = 4;
		out->result.splitTuple.prefixNodeLabels = NULL;

		/* Identify which node the existing data goes into */
		out->result.splitTuple.childNodeN =
			inet_spg_node_number(prefix, commonbits);

		out->result.splitTuple.postfixHasPrefix = true;
		out->result.splitTuple.postfixPrefixDatum = InetPGetDatum(prefix);

		PG_RETURN_VOID();
	}

	/*
	 * All OK, choose the node to descend into.  (If this tuple is marked
	 * allTheSame, the core code will ignore our choice of nodeN; but we need
	 * not account for that case explicitly here.)
	 */
	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = inet_spg_node_number(val, commonbits);
	out->result.matchNode.restDatum = InetPGetDatum(val);

	PG_RETURN_VOID();
}

/*
 * The GiST PickSplit method
 */
Datum
inet_spg_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	inet	   *prefix,
			   *tmp;
	int			i,
				commonbits;
	bool		differentFamilies = false;

	/* Initialize the prefix with the first item */
	prefix = DatumGetInetPP(in->datums[0]);
	commonbits = ip_bits(prefix);

	/* Examine remaining items to discover minimum common prefix length */
	for (i = 1; i < in->nTuples; i++)
	{
		tmp = DatumGetInetPP(in->datums[i]);

		if (ip_family(tmp) != ip_family(prefix))
		{
			differentFamilies = true;
			break;
		}

		if (ip_bits(tmp) < commonbits)
			commonbits = ip_bits(tmp);
		commonbits = bitncommon(ip_addr(prefix), ip_addr(tmp), commonbits);
		if (commonbits == 0)
			break;
	}

	/* Don't need labels; allocate output arrays */
	out->nodeLabels = NULL;
	out->mapTuplesToNodes = (int *) palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = (Datum *) palloc(sizeof(Datum) * in->nTuples);

	if (differentFamilies)
	{
		/* Set up 2-node tuple */
		out->hasPrefix = false;
		out->nNodes = 2;

		for (i = 0; i < in->nTuples; i++)
		{
			tmp = DatumGetInetPP(in->datums[i]);
			out->mapTuplesToNodes[i] =
				(ip_family(tmp) == PGSQL_AF_INET) ? 0 : 1;
			out->leafTupleDatums[i] = InetPGetDatum(tmp);
		}
	}
	else
	{
		/* Set up 4-node tuple */
		out->hasPrefix = true;
		out->prefixDatum =
			InetPGetDatum(cidr_set_masklen_internal(prefix, commonbits));
		out->nNodes = 4;

		for (i = 0; i < in->nTuples; i++)
		{
			tmp = DatumGetInetPP(in->datums[i]);
			out->mapTuplesToNodes[i] = inet_spg_node_number(tmp, commonbits);
			out->leafTupleDatums[i] = InetPGetDatum(tmp);
		}
	}

	PG_RETURN_VOID();
}

/*
 * The SP-GiST query consistency check for inner tuples
 */
Datum
inet_spg_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int			i;
	int			which;

	if (!in->hasPrefix)
	{
		Assert(!in->allTheSame);
		Assert(in->nNodes == 2);

		/* Identify which child nodes need to be visited */
		which = 1 | (1 << 1);

		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy = in->scankeys[i].sk_strategy;
			inet	   *argument = DatumGetInetPP(in->scankeys[i].sk_argument);

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (ip_family(argument) == PGSQL_AF_INET)
						which &= 1;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (ip_family(argument) == PGSQL_AF_INET6)
						which &= (1 << 1);
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					/* all other ops can only match addrs of same family */
					if (ip_family(argument) == PGSQL_AF_INET)
						which &= 1;
					else
						which &= (1 << 1);
					break;
			}
		}
	}
	else if (!in->allTheSame)
	{
		Assert(in->nNodes == 4);

		/* Identify which child nodes need to be visited */
		which = inet_spg_consistent_bitmap(DatumGetInetPP(in->prefixDatum),
										   in->nkeys, in->scankeys, false);
	}
	else
	{
		/* Must visit all nodes; we assume there are less than 32 of 'em */
		which = ~0;
	}

	out->nNodes = 0;

	if (which)
	{
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);

		for (i = 0; i < in->nNodes; i++)
		{
			if (which & (1 << i))
			{
				out->nodeNumbers[out->nNodes] = i;
				out->nNodes++;
			}
		}
	}

	PG_RETURN_VOID();
}

/*
 * The SP-GiST query consistency check for leaf tuples
 */
Datum
inet_spg_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	inet	   *leaf = DatumGetInetPP(in->leafDatum);

	/* All tests are exact. */
	out->recheck = false;

	/* Leaf is what it is... */
	out->leafValue = InetPGetDatum(leaf);

	/* Use common code to apply the tests. */
	PG_RETURN_BOOL(inet_spg_consistent_bitmap(leaf, in->nkeys, in->scankeys,
											  true));
}

/*
 * Calculate node number (within a 4-node, single-family inner index tuple)
 *
 * The value must have the same family as the node's prefix, and
 * commonbits is the mask length of the prefix.  We use even or odd
 * nodes according to the next address bit after the commonbits,
 * and low or high nodes according to whether the value's mask length
 * is larger than commonbits.
 */
static int
inet_spg_node_number(const inet *val, int commonbits)
{
	int			nodeN = 0;

	if (commonbits < ip_maxbits(val) &&
		ip_addr(val)[commonbits / 8] & (1 << (7 - commonbits % 8)))
		nodeN |= 1;
	if (commonbits < ip_bits(val))
		nodeN |= 2;

	return nodeN;
}

/*
 * Calculate bitmap of node numbers that are consistent with the query
 *
 * This can be used either at a 4-way inner tuple, or at a leaf tuple.
 * In the latter case, we should return a boolean result (0 or 1)
 * not a bitmap.
 *
 * This definition is pretty odd, but the inner and leaf consistency checks
 * are mostly common and it seems best to keep them in one function.
 */
static int
inet_spg_consistent_bitmap(const inet *prefix, int nkeys, ScanKey scankeys,
						   bool leaf)
{
	int			bitmap;
	int			commonbits,
				i;

	/* Initialize result to allow visiting all children */
	if (leaf)
		bitmap = 1;
	else
		bitmap = 1 | (1 << 1) | (1 << 2) | (1 << 3);

	commonbits = ip_bits(prefix);

	for (i = 0; i < nkeys; i++)
	{
		inet	   *argument = DatumGetInetPP(scankeys[i].sk_argument);
		StrategyNumber strategy = scankeys[i].sk_strategy;
		int			order;

		/*
		 * Check 0: different families
		 *
		 * Matching families do not help any of the strategies.
		 */
		if (ip_family(argument) != ip_family(prefix))
		{
			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (ip_family(argument) < ip_family(prefix))
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (ip_family(argument) > ip_family(prefix))
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					/* For all other cases, we can be sure there is no match */
					bitmap = 0;
					break;
			}

			if (!bitmap)
				break;

			/* Other checks make no sense with different families. */
			continue;
		}

		/*
		 * Check 1: network bit count
		 *
		 * Network bit count (ip_bits) helps to check leaves for sub network
		 * and sup network operators.  At non-leaf nodes, we know every child
		 * value has greater ip_bits, so we can avoid descending in some cases
		 * too.
		 *
		 * This check is less expensive than checking the address bits, so we
		 * are doing this before, but it has to be done after for the basic
		 * comparison strategies, because ip_bits only affect their results
		 * when the common network bits are the same.
		 */
		switch (strategy)
		{
			case RTSubStrategyNumber:
				if (commonbits <= ip_bits(argument))
					bitmap &= (1 << 2) | (1 << 3);
				break;

			case RTSubEqualStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= (1 << 2) | (1 << 3);
				break;

			case RTSuperStrategyNumber:
				if (commonbits == ip_bits(argument) - 1)
					bitmap &= 1 | (1 << 1);
				else if (commonbits >= ip_bits(argument))
					bitmap = 0;
				break;

			case RTSuperEqualStrategyNumber:
				if (commonbits == ip_bits(argument))
					bitmap &= 1 | (1 << 1);
				else if (commonbits > ip_bits(argument))
					bitmap = 0;
				break;

			case RTEqualStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= (1 << 2) | (1 << 3);
				else if (commonbits == ip_bits(argument))
					bitmap &= 1 | (1 << 1);
				else
					bitmap = 0;
				break;
		}

		if (!bitmap)
			break;

		/*
		 * Check 2: common network bits
		 *
		 * Compare available common prefix bits to the query, but not beyond
		 * either the query's netmask or the minimum netmask among the
		 * represented values.  If these bits don't match the query, we can
		 * eliminate some cases.
		 */
		order = bitncmp(ip_addr(prefix), ip_addr(argument),
						Min(commonbits, ip_bits(argument)));

		if (order != 0)
		{
			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (order > 0)
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (order < 0)
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					/* For all other cases, we can be sure there is no match */
					bitmap = 0;
					break;
			}

			if (!bitmap)
				break;

			/*
			 * Remaining checks make no sense when common bits don't match.
			 */
			continue;
		}

		/*
		 * Check 3: next network bit
		 *
		 * We can filter out branch 2 or 3 using the next network bit of the
		 * argument, if it is available.
		 *
		 * This check matters for the performance of the search. The results
		 * would be correct without it.
		 */
		if (bitmap & ((1 << 2) | (1 << 3)) &&
			commonbits < ip_bits(argument))
		{
			int			nextbit;

			nextbit = ip_addr(argument)[commonbits / 8] &
				(1 << (7 - commonbits % 8));

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (!nextbit)
						bitmap &= 1 | (1 << 1) | (1 << 2);
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (nextbit)
						bitmap &= 1 | (1 << 1) | (1 << 3);
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					if (!nextbit)
						bitmap &= 1 | (1 << 1) | (1 << 2);
					else
						bitmap &= 1 | (1 << 1) | (1 << 3);
					break;
			}

			if (!bitmap)
				break;
		}

		/*
		 * Remaining checks are only for the basic comparison strategies. This
		 * test relies on the strategy number ordering defined in stratnum.h.
		 */
		if (strategy < RTEqualStrategyNumber ||
			strategy > RTGreaterEqualStrategyNumber)
			continue;

		/*
		 * Check 4: network bit count
		 *
		 * At this point, we know that the common network bits of the prefix
		 * and the argument are the same, so we can go forward and check the
		 * ip_bits.
		 */
		switch (strategy)
		{
			case RTLessStrategyNumber:
			case RTLessEqualStrategyNumber:
				if (commonbits == ip_bits(argument))
					bitmap &= 1 | (1 << 1);
				else if (commonbits > ip_bits(argument))
					bitmap = 0;
				break;

			case RTGreaterEqualStrategyNumber:
			case RTGreaterStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= (1 << 2) | (1 << 3);
				break;
		}

		if (!bitmap)
			break;

		/* Remaining checks don't make sense with different ip_bits. */
		if (commonbits != ip_bits(argument))
			continue;

		/*
		 * Check 5: next host bit
		 *
		 * We can filter out branch 0 or 1 using the next host bit of the
		 * argument, if it is available.
		 *
		 * This check matters for the performance of the search. The results
		 * would be correct without it.  There is no point in running it for
		 * leafs as we have to check the whole address on the next step.
		 */
		if (!leaf && bitmap & (1 | (1 << 1)) &&
			commonbits < ip_maxbits(argument))
		{
			int			nextbit;

			nextbit = ip_addr(argument)[commonbits / 8] &
				(1 << (7 - commonbits % 8));

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (!nextbit)
						bitmap &= 1 | (1 << 2) | (1 << 3);
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (nextbit)
						bitmap &= (1 << 1) | (1 << 2) | (1 << 3);
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					if (!nextbit)
						bitmap &= 1 | (1 << 2) | (1 << 3);
					else
						bitmap &= (1 << 1) | (1 << 2) | (1 << 3);
					break;
			}

			if (!bitmap)
				break;
		}

		/*
		 * Check 6: whole address
		 *
		 * This is the last check for correctness of the basic comparison
		 * strategies.  It's only appropriate at leaf entries.
		 */
		if (leaf)
		{
			/* Redo ordering comparison using all address bits */
			order = bitncmp(ip_addr(prefix), ip_addr(argument),
							ip_maxbits(prefix));

			switch (strategy)
			{
				case RTLessStrategyNumber:
					if (order >= 0)
						bitmap = 0;
					break;

				case RTLessEqualStrategyNumber:
					if (order > 0)
						bitmap = 0;
					break;

				case RTEqualStrategyNumber:
					if (order != 0)
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
					if (order < 0)
						bitmap = 0;
					break;

				case RTGreaterStrategyNumber:
					if (order <= 0)
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					if (order == 0)
						bitmap = 0;
					break;
			}

			if (!bitmap)
				break;
		}
	}

	return bitmap;
}
