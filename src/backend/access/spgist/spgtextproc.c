/*-------------------------------------------------------------------------
 *
 * spgtextproc.c
 *	  implementation of radix tree (compressed trie) over text
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/access/spgist/spgtextproc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/spgist.h"
#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/pg_locale.h"


/*
 * In the worst case, a inner tuple in a text radix tree could have as many
 * as 256 nodes (one for each possible byte value).  Each node can take 16
 * bytes on MAXALIGN=8 machines.  The inner tuple must fit on an index page
 * of size BLCKSZ.	Rather than assuming we know the exact amount of overhead
 * imposed by page headers, tuple headers, etc, we leave 100 bytes for that
 * (the actual overhead should be no more than 56 bytes at this writing, so
 * there is slop in this number).  So we can safely create prefixes up to
 * BLCKSZ - 256 * 16 - 100 bytes long.	Unfortunately, because 256 * 16 is
 * already 4K, there is no safe prefix length when BLCKSZ is less than 8K;
 * it is always possible to get "SPGiST inner tuple size exceeds maximum"
 * if there are too many distinct next-byte values at a given place in the
 * tree.  Since use of nonstandard block sizes appears to be negligible in
 * the field, we just live with that fact for now, choosing a max prefix
 * size of 32 bytes when BLCKSZ is configured smaller than default.
 */
#define SPGIST_MAX_PREFIX_LENGTH	Max((int) (BLCKSZ - 256 * 16 - 100), 32)

/* Struct for sorting values in picksplit */
typedef struct spgNodePtr
{
	Datum		d;
	int			i;
	uint8		c;
} spgNodePtr;


Datum
spg_text_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = TEXTOID;
	cfg->labelType = CHAROID;
	cfg->canReturnData = true;
	cfg->longValuesOK = true;	/* suffixing will shorten long values */
	PG_RETURN_VOID();
}

/*
 * Form a text datum from the given not-necessarily-null-terminated string,
 * using short varlena header format if possible
 */
static Datum
formTextDatum(const char *data, int datalen)
{
	char	   *p;

	p = (char *) palloc(datalen + VARHDRSZ);

	if (datalen + VARHDRSZ_SHORT <= VARATT_SHORT_MAX)
	{
		SET_VARSIZE_SHORT(p, datalen + VARHDRSZ_SHORT);
		if (datalen)
			memcpy(p + VARHDRSZ_SHORT, data, datalen);
	}
	else
	{
		SET_VARSIZE(p, datalen + VARHDRSZ);
		memcpy(p + VARHDRSZ, data, datalen);
	}

	return PointerGetDatum(p);
}

/*
 * Find the length of the common prefix of a and b
 */
static int
commonPrefix(const char *a, const char *b, int lena, int lenb)
{
	int			i = 0;

	while (i < lena && i < lenb && *a == *b)
	{
		a++;
		b++;
		i++;
	}

	return i;
}

/*
 * Binary search an array of uint8 datums for a match to c
 *
 * On success, *i gets the match location; on failure, it gets where to insert
 */
static bool
searchChar(Datum *nodeLabels, int nNodes, uint8 c, int *i)
{
	int			StopLow = 0,
				StopHigh = nNodes;

	while (StopLow < StopHigh)
	{
		int			StopMiddle = (StopLow + StopHigh) >> 1;
		uint8		middle = DatumGetUInt8(nodeLabels[StopMiddle]);

		if (c < middle)
			StopHigh = StopMiddle;
		else if (c > middle)
			StopLow = StopMiddle + 1;
		else
		{
			*i = StopMiddle;
			return true;
		}
	}

	*i = StopHigh;
	return false;
}

Datum
spg_text_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	text	   *inText = DatumGetTextPP(in->datum);
	char	   *inStr = VARDATA_ANY(inText);
	int			inSize = VARSIZE_ANY_EXHDR(inText);
	uint8		nodeChar = '\0';
	int			i = 0;
	int			commonLen = 0;

	/* Check for prefix match, set nodeChar to first byte after prefix */
	if (in->hasPrefix)
	{
		text	   *prefixText = DatumGetTextPP(in->prefixDatum);
		char	   *prefixStr = VARDATA_ANY(prefixText);
		int			prefixSize = VARSIZE_ANY_EXHDR(prefixText);

		commonLen = commonPrefix(inStr + in->level,
								 prefixStr,
								 inSize - in->level,
								 prefixSize);

		if (commonLen == prefixSize)
		{
			if (inSize - in->level > commonLen)
				nodeChar = *(uint8 *) (inStr + in->level + commonLen);
			else
				nodeChar = '\0';
		}
		else
		{
			/* Must split tuple because incoming value doesn't match prefix */
			out->resultType = spgSplitTuple;

			if (commonLen == 0)
			{
				out->result.splitTuple.prefixHasPrefix = false;
			}
			else
			{
				out->result.splitTuple.prefixHasPrefix = true;
				out->result.splitTuple.prefixPrefixDatum =
					formTextDatum(prefixStr, commonLen);
			}
			out->result.splitTuple.nodeLabel =
				UInt8GetDatum(*(prefixStr + commonLen));

			if (prefixSize - commonLen == 1)
			{
				out->result.splitTuple.postfixHasPrefix = false;
			}
			else
			{
				out->result.splitTuple.postfixHasPrefix = true;
				out->result.splitTuple.postfixPrefixDatum =
					formTextDatum(prefixStr + commonLen + 1,
								  prefixSize - commonLen - 1);
			}

			PG_RETURN_VOID();
		}
	}
	else if (inSize > in->level)
	{
		nodeChar = *(uint8 *) (inStr + in->level);
	}
	else
	{
		nodeChar = '\0';
	}

	/* Look up nodeChar in the node label array */
	if (searchChar(in->nodeLabels, in->nNodes, nodeChar, &i))
	{
		/*
		 * Descend to existing node.  (If in->allTheSame, the core code will
		 * ignore our nodeN specification here, but that's OK.  We still have
		 * to provide the correct levelAdd and restDatum values, and those are
		 * the same regardless of which node gets chosen by core.)
		 */
		out->resultType = spgMatchNode;
		out->result.matchNode.nodeN = i;
		out->result.matchNode.levelAdd = commonLen + 1;
		if (inSize - in->level - commonLen - 1 > 0)
			out->result.matchNode.restDatum =
				formTextDatum(inStr + in->level + commonLen + 1,
							  inSize - in->level - commonLen - 1);
		else
			out->result.matchNode.restDatum =
				formTextDatum(NULL, 0);
	}
	else if (in->allTheSame)
	{
		/*
		 * Can't use AddNode action, so split the tuple.  The upper tuple has
		 * the same prefix as before and uses an empty node label for the
		 * lower tuple.  The lower tuple has no prefix and the same node
		 * labels as the original tuple.
		 */
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = in->hasPrefix;
		out->result.splitTuple.prefixPrefixDatum = in->prefixDatum;
		out->result.splitTuple.nodeLabel = UInt8GetDatum('\0');
		out->result.splitTuple.postfixHasPrefix = false;
	}
	else
	{
		/* Add a node for the not-previously-seen nodeChar value */
		out->resultType = spgAddNode;
		out->result.addNode.nodeLabel = UInt8GetDatum(nodeChar);
		out->result.addNode.nodeN = i;
	}

	PG_RETURN_VOID();
}

/* qsort comparator to sort spgNodePtr structs by "c" */
static int
cmpNodePtr(const void *a, const void *b)
{
	const spgNodePtr *aa = (const spgNodePtr *) a;
	const spgNodePtr *bb = (const spgNodePtr *) b;

	if (aa->c == bb->c)
		return 0;
	else if (aa->c > bb->c)
		return 1;
	else
		return -1;
}

Datum
spg_text_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	text	   *text0 = DatumGetTextPP(in->datums[0]);
	int			i,
				commonLen;
	spgNodePtr *nodes;

	/* Identify longest common prefix, if any */
	commonLen = VARSIZE_ANY_EXHDR(text0);
	for (i = 1; i < in->nTuples && commonLen > 0; i++)
	{
		text	   *texti = DatumGetTextPP(in->datums[i]);
		int			tmp = commonPrefix(VARDATA_ANY(text0),
									   VARDATA_ANY(texti),
									   VARSIZE_ANY_EXHDR(text0),
									   VARSIZE_ANY_EXHDR(texti));

		if (tmp < commonLen)
			commonLen = tmp;
	}

	/*
	 * Limit the prefix length, if necessary, to ensure that the resulting
	 * inner tuple will fit on a page.
	 */
	commonLen = Min(commonLen, SPGIST_MAX_PREFIX_LENGTH);

	/* Set node prefix to be that string, if it's not empty */
	if (commonLen == 0)
	{
		out->hasPrefix = false;
	}
	else
	{
		out->hasPrefix = true;
		out->prefixDatum = formTextDatum(VARDATA_ANY(text0), commonLen);
	}

	/* Extract the node label (first non-common byte) from each value */
	nodes = (spgNodePtr *) palloc(sizeof(spgNodePtr) * in->nTuples);

	for (i = 0; i < in->nTuples; i++)
	{
		text	   *texti = DatumGetTextPP(in->datums[i]);

		if (commonLen < VARSIZE_ANY_EXHDR(texti))
			nodes[i].c = *(uint8 *) (VARDATA_ANY(texti) + commonLen);
		else
			nodes[i].c = '\0';	/* use \0 if string is all common */
		nodes[i].i = i;
		nodes[i].d = in->datums[i];
	}

	/*
	 * Sort by label bytes so that we can group the values into nodes.	This
	 * also ensures that the nodes are ordered by label value, allowing the
	 * use of binary search in searchChar.
	 */
	qsort(nodes, in->nTuples, sizeof(*nodes), cmpNodePtr);

	/* And emit results */
	out->nNodes = 0;
	out->nodeLabels = (Datum *) palloc(sizeof(Datum) * in->nTuples);
	out->mapTuplesToNodes = (int *) palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = (Datum *) palloc(sizeof(Datum) * in->nTuples);

	for (i = 0; i < in->nTuples; i++)
	{
		text	   *texti = DatumGetTextPP(nodes[i].d);
		Datum		leafD;

		if (i == 0 || nodes[i].c != nodes[i - 1].c)
		{
			out->nodeLabels[out->nNodes] = UInt8GetDatum(nodes[i].c);
			out->nNodes++;
		}

		if (commonLen < VARSIZE_ANY_EXHDR(texti))
			leafD = formTextDatum(VARDATA_ANY(texti) + commonLen + 1,
								  VARSIZE_ANY_EXHDR(texti) - commonLen - 1);
		else
			leafD = formTextDatum(NULL, 0);

		out->leafTupleDatums[nodes[i].i] = leafD;
		out->mapTuplesToNodes[nodes[i].i] = out->nNodes - 1;
	}

	PG_RETURN_VOID();
}

Datum
spg_text_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	bool		collate_is_c = lc_collate_is_c(PG_GET_COLLATION());
	text	   *reconstrText = NULL;
	int			maxReconstrLen = 0;
	text	   *prefixText = NULL;
	int			prefixSize = 0;
	int			i;

	/*
	 * Reconstruct values represented at this tuple, including parent data,
	 * prefix of this tuple if any, and the node label if any.	in->level
	 * should be the length of the previously reconstructed value, and the
	 * number of bytes added here is prefixSize or prefixSize + 1.
	 *
	 * Note: we assume that in->reconstructedValue isn't toasted and doesn't
	 * have a short varlena header.  This is okay because it must have been
	 * created by a previous invocation of this routine, and we always emit
	 * long-format reconstructed values.
	 */
	Assert(in->level == 0 ? DatumGetPointer(in->reconstructedValue) == NULL :
	VARSIZE_ANY_EXHDR(DatumGetPointer(in->reconstructedValue)) == in->level);

	maxReconstrLen = in->level + 1;
	if (in->hasPrefix)
	{
		prefixText = DatumGetTextPP(in->prefixDatum);
		prefixSize = VARSIZE_ANY_EXHDR(prefixText);
		maxReconstrLen += prefixSize;
	}

	reconstrText = palloc(VARHDRSZ + maxReconstrLen);
	SET_VARSIZE(reconstrText, VARHDRSZ + maxReconstrLen);

	if (in->level)
		memcpy(VARDATA(reconstrText),
			   VARDATA(DatumGetPointer(in->reconstructedValue)),
			   in->level);
	if (prefixSize)
		memcpy(((char *) VARDATA(reconstrText)) + in->level,
			   VARDATA_ANY(prefixText),
			   prefixSize);
	/* last byte of reconstrText will be filled in below */

	/*
	 * Scan the child nodes.  For each one, complete the reconstructed value
	 * and see if it's consistent with the query.  If so, emit an entry into
	 * the output arrays.
	 */
	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
	out->levelAdds = (int *) palloc(sizeof(int) * in->nNodes);
	out->reconstructedValues = (Datum *) palloc(sizeof(Datum) * in->nNodes);
	out->nNodes = 0;

	for (i = 0; i < in->nNodes; i++)
	{
		uint8		nodeChar = DatumGetUInt8(in->nodeLabels[i]);
		int			thisLen;
		bool		res = true;
		int			j;

		/* If nodeChar is zero, don't include it in data */
		if (nodeChar == '\0')
			thisLen = maxReconstrLen - 1;
		else
		{
			((char *) VARDATA(reconstrText))[maxReconstrLen - 1] = nodeChar;
			thisLen = maxReconstrLen;
		}

		for (j = 0; j < in->nkeys; j++)
		{
			StrategyNumber strategy = in->scankeys[j].sk_strategy;
			text	   *inText;
			int			inSize;
			int			r;

			/*
			 * If it's a collation-aware operator, but the collation is C, we
			 * can treat it as non-collation-aware.  With non-C collation we
			 * need to traverse whole tree :-( so there's no point in making
			 * any check here.
			 */
			if (strategy > 10)
			{
				if (collate_is_c)
					strategy -= 10;
				else
					continue;
			}

			inText = DatumGetTextPP(in->scankeys[j].sk_argument);
			inSize = VARSIZE_ANY_EXHDR(inText);

			r = memcmp(VARDATA(reconstrText), VARDATA_ANY(inText),
					   Min(inSize, thisLen));

			switch (strategy)
			{
				case BTLessStrategyNumber:
				case BTLessEqualStrategyNumber:
					if (r > 0)
						res = false;
					break;
				case BTEqualStrategyNumber:
					if (r != 0 || inSize < thisLen)
						res = false;
					break;
				case BTGreaterEqualStrategyNumber:
				case BTGreaterStrategyNumber:
					if (r < 0)
						res = false;
					break;
				default:
					elog(ERROR, "unrecognized strategy number: %d",
						 in->scankeys[j].sk_strategy);
					break;
			}

			if (!res)
				break;			/* no need to consider remaining conditions */
		}

		if (res)
		{
			out->nodeNumbers[out->nNodes] = i;
			out->levelAdds[out->nNodes] = thisLen - in->level;
			SET_VARSIZE(reconstrText, VARHDRSZ + thisLen);
			out->reconstructedValues[out->nNodes] =
				datumCopy(PointerGetDatum(reconstrText), false, -1);
			out->nNodes++;
		}
	}

	PG_RETURN_VOID();
}

Datum
spg_text_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	int			level = in->level;
	text	   *leafValue,
			   *reconstrValue = NULL;
	char	   *fullValue;
	int			fullLen;
	bool		res;
	int			j;

	/* all tests are exact */
	out->recheck = false;

	leafValue = DatumGetTextPP(in->leafDatum);

	if (DatumGetPointer(in->reconstructedValue))
		reconstrValue = DatumGetTextP(in->reconstructedValue);

	Assert(level == 0 ? reconstrValue == NULL :
		   VARSIZE_ANY_EXHDR(reconstrValue) == level);

	/* Reconstruct the full string represented by this leaf tuple */
	fullLen = level + VARSIZE_ANY_EXHDR(leafValue);
	if (VARSIZE_ANY_EXHDR(leafValue) == 0 && level > 0)
	{
		fullValue = VARDATA(reconstrValue);
		out->leafValue = PointerGetDatum(reconstrValue);
	}
	else
	{
		text	   *fullText = palloc(VARHDRSZ + fullLen);

		SET_VARSIZE(fullText, VARHDRSZ + fullLen);
		fullValue = VARDATA(fullText);
		if (level)
			memcpy(fullValue, VARDATA(reconstrValue), level);
		if (VARSIZE_ANY_EXHDR(leafValue) > 0)
			memcpy(fullValue + level, VARDATA_ANY(leafValue),
				   VARSIZE_ANY_EXHDR(leafValue));
		out->leafValue = PointerGetDatum(fullText);
	}

	/* Perform the required comparison(s) */
	res = true;
	for (j = 0; j < in->nkeys; j++)
	{
		StrategyNumber strategy = in->scankeys[j].sk_strategy;
		text	   *query = DatumGetTextPP(in->scankeys[j].sk_argument);
		int			queryLen = VARSIZE_ANY_EXHDR(query);
		int			r;

		if (strategy > 10)
		{
			/* Collation-aware comparison */
			strategy -= 10;

			/* If asserts enabled, verify encoding of reconstructed string */
			Assert(pg_verifymbstr(fullValue, fullLen, false));

			r = varstr_cmp(fullValue, Min(queryLen, fullLen),
						   VARDATA_ANY(query), Min(queryLen, fullLen),
						   PG_GET_COLLATION());
		}
		else
		{
			/* Non-collation-aware comparison */
			r = memcmp(fullValue, VARDATA_ANY(query), Min(queryLen, fullLen));
		}

		if (r == 0)
		{
			if (queryLen > fullLen)
				r = -1;
			else if (queryLen < fullLen)
				r = 1;
		}

		switch (strategy)
		{
			case BTLessStrategyNumber:
				res = (r < 0);
				break;
			case BTLessEqualStrategyNumber:
				res = (r <= 0);
				break;
			case BTEqualStrategyNumber:
				res = (r == 0);
				break;
			case BTGreaterEqualStrategyNumber:
				res = (r >= 0);
				break;
			case BTGreaterStrategyNumber:
				res = (r > 0);
				break;
			default:
				elog(ERROR, "unrecognized strategy number: %d",
					 in->scankeys[j].sk_strategy);
				res = false;
				break;
		}

		if (!res)
			break;				/* no need to consider remaining conditions */
	}

	PG_RETURN_BOOL(res);
}
