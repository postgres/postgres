/*--------------------------------------------------------------------------
 *
 * spgist_name_ops.c
 *		Test opclass for SP-GiST
 *
 * This indexes input values of type "name", but the index storage is "text",
 * with the same choices as made in the core SP-GiST text_ops opclass.
 * Much of the code is identical to src/backend/access/spgist/spgtextproc.c,
 * which see for a more detailed header comment.
 *
 * Unlike spgtextproc.c, we don't bother with collation-aware logic.
 *
 *
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/test/modules/spgist_name_ops/spgist_name_ops.c
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/spgist.h"
#include "catalog/pg_type.h"
#include "utils/datum.h"

PG_MODULE_MAGIC;


PG_FUNCTION_INFO_V1(spgist_name_config);
Datum
spgist_name_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = TEXTOID;
	cfg->labelType = INT2OID;
	cfg->leafType = TEXTOID;
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
 * Binary search an array of int16 datums for a match to c
 *
 * On success, *i gets the match location; on failure, it gets where to insert
 */
static bool
searchChar(Datum *nodeLabels, int nNodes, int16 c, int *i)
{
	int			StopLow = 0,
				StopHigh = nNodes;

	while (StopLow < StopHigh)
	{
		int			StopMiddle = (StopLow + StopHigh) >> 1;
		int16		middle = DatumGetInt16(nodeLabels[StopMiddle]);

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

PG_FUNCTION_INFO_V1(spgist_name_choose);
Datum
spgist_name_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	Name		inName = DatumGetName(in->datum);
	char	   *inStr = NameStr(*inName);
	int			inSize = strlen(inStr);
	char	   *prefixStr = NULL;
	int			prefixSize = 0;
	int			commonLen = 0;
	int16		nodeChar = 0;
	int			i = 0;

	/* Check for prefix match, set nodeChar to first byte after prefix */
	if (in->hasPrefix)
	{
		text	   *prefixText = DatumGetTextPP(in->prefixDatum);

		prefixStr = VARDATA_ANY(prefixText);
		prefixSize = VARSIZE_ANY_EXHDR(prefixText);

		commonLen = commonPrefix(inStr + in->level,
								 prefixStr,
								 inSize - in->level,
								 prefixSize);

		if (commonLen == prefixSize)
		{
			if (inSize - in->level > commonLen)
				nodeChar = *(unsigned char *) (inStr + in->level + commonLen);
			else
				nodeChar = -1;
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
			out->result.splitTuple.prefixNNodes = 1;
			out->result.splitTuple.prefixNodeLabels =
				(Datum *) palloc(sizeof(Datum));
			out->result.splitTuple.prefixNodeLabels[0] =
				Int16GetDatum(*(unsigned char *) (prefixStr + commonLen));

			out->result.splitTuple.childNodeN = 0;

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
		nodeChar = *(unsigned char *) (inStr + in->level);
	}
	else
	{
		nodeChar = -1;
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
		int			levelAdd;

		out->resultType = spgMatchNode;
		out->result.matchNode.nodeN = i;
		levelAdd = commonLen;
		if (nodeChar >= 0)
			levelAdd++;
		out->result.matchNode.levelAdd = levelAdd;
		if (inSize - in->level - levelAdd > 0)
			out->result.matchNode.restDatum =
				formTextDatum(inStr + in->level + levelAdd,
							  inSize - in->level - levelAdd);
		else
			out->result.matchNode.restDatum =
				formTextDatum(NULL, 0);
	}
	else if (in->allTheSame)
	{
		/*
		 * Can't use AddNode action, so split the tuple.  The upper tuple has
		 * the same prefix as before and uses a dummy node label -2 for the
		 * lower tuple.  The lower tuple has no prefix and the same node
		 * labels as the original tuple.
		 *
		 * Note: it might seem tempting to shorten the upper tuple's prefix,
		 * if it has one, then use its last byte as label for the lower tuple.
		 * But that doesn't win since we know the incoming value matches the
		 * whole prefix: we'd just end up splitting the lower tuple again.
		 */
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = in->hasPrefix;
		out->result.splitTuple.prefixPrefixDatum = in->prefixDatum;
		out->result.splitTuple.prefixNNodes = 1;
		out->result.splitTuple.prefixNodeLabels = (Datum *) palloc(sizeof(Datum));
		out->result.splitTuple.prefixNodeLabels[0] = Int16GetDatum(-2);
		out->result.splitTuple.childNodeN = 0;
		out->result.splitTuple.postfixHasPrefix = false;
	}
	else
	{
		/* Add a node for the not-previously-seen nodeChar value */
		out->resultType = spgAddNode;
		out->result.addNode.nodeLabel = Int16GetDatum(nodeChar);
		out->result.addNode.nodeN = i;
	}

	PG_RETURN_VOID();
}

/* The picksplit function is identical to the core opclass, so just use that */

PG_FUNCTION_INFO_V1(spgist_name_inner_consistent);
Datum
spgist_name_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	text	   *reconstructedValue;
	text	   *reconstrText;
	int			maxReconstrLen;
	text	   *prefixText = NULL;
	int			prefixSize = 0;
	int			i;

	/*
	 * Reconstruct values represented at this tuple, including parent data,
	 * prefix of this tuple if any, and the node label if it's non-dummy.
	 * in->level should be the length of the previously reconstructed value,
	 * and the number of bytes added here is prefixSize or prefixSize + 1.
	 *
	 * Recall that reconstructedValues are assumed to be the same type as leaf
	 * datums, so we must use "text" not "name" for them.
	 *
	 * Note: we assume that in->reconstructedValue isn't toasted and doesn't
	 * have a short varlena header.  This is okay because it must have been
	 * created by a previous invocation of this routine, and we always emit
	 * long-format reconstructed values.
	 */
	reconstructedValue = (text *) DatumGetPointer(in->reconstructedValue);
	Assert(reconstructedValue == NULL ? in->level == 0 :
		   VARSIZE_ANY_EXHDR(reconstructedValue) == in->level);

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
			   VARDATA(reconstructedValue),
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
		int16		nodeChar = DatumGetInt16(in->nodeLabels[i]);
		int			thisLen;
		bool		res = true;
		int			j;

		/* If nodeChar is a dummy value, don't include it in data */
		if (nodeChar <= 0)
			thisLen = maxReconstrLen - 1;
		else
		{
			((unsigned char *) VARDATA(reconstrText))[maxReconstrLen - 1] = nodeChar;
			thisLen = maxReconstrLen;
		}

		for (j = 0; j < in->nkeys; j++)
		{
			StrategyNumber strategy = in->scankeys[j].sk_strategy;
			Name		inName;
			char	   *inStr;
			int			inSize;
			int			r;

			inName = DatumGetName(in->scankeys[j].sk_argument);
			inStr = NameStr(*inName);
			inSize = strlen(inStr);

			r = memcmp(VARDATA(reconstrText), inStr,
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

PG_FUNCTION_INFO_V1(spgist_name_leaf_consistent);
Datum
spgist_name_leaf_consistent(PG_FUNCTION_ARGS)
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

	/* As above, in->reconstructedValue isn't toasted or short. */
	if (DatumGetPointer(in->reconstructedValue))
		reconstrValue = (text *) DatumGetPointer(in->reconstructedValue);

	Assert(reconstrValue == NULL ? level == 0 :
		   VARSIZE_ANY_EXHDR(reconstrValue) == level);

	/* Reconstruct the Name represented by this leaf tuple */
	fullValue = palloc0(NAMEDATALEN);
	fullLen = level + VARSIZE_ANY_EXHDR(leafValue);
	Assert(fullLen < NAMEDATALEN);
	if (VARSIZE_ANY_EXHDR(leafValue) == 0 && level > 0)
	{
		memcpy(fullValue, VARDATA(reconstrValue),
			   VARSIZE_ANY_EXHDR(reconstrValue));
	}
	else
	{
		if (level)
			memcpy(fullValue, VARDATA(reconstrValue), level);
		if (VARSIZE_ANY_EXHDR(leafValue) > 0)
			memcpy(fullValue + level, VARDATA_ANY(leafValue),
				   VARSIZE_ANY_EXHDR(leafValue));
	}
	out->leafValue = PointerGetDatum(fullValue);

	/* Perform the required comparison(s) */
	res = true;
	for (j = 0; j < in->nkeys; j++)
	{
		StrategyNumber strategy = in->scankeys[j].sk_strategy;
		Name		queryName = DatumGetName(in->scankeys[j].sk_argument);
		char	   *queryStr = NameStr(*queryName);
		int			queryLen = strlen(queryStr);
		int			r;

		/* Non-collation-aware comparison */
		r = memcmp(fullValue, queryStr, Min(queryLen, fullLen));

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

PG_FUNCTION_INFO_V1(spgist_name_compress);
Datum
spgist_name_compress(PG_FUNCTION_ARGS)
{
	Name		inName = PG_GETARG_NAME(0);
	char	   *inStr = NameStr(*inName);

	PG_RETURN_DATUM(formTextDatum(inStr, strlen(inStr)));
}
