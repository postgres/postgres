/*-------------------------------------------------------------------------
 *
 * ts_selfuncs.c
 *	  Selectivity estimation functions for text search operators.
 *
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/tsearch/ts_selfuncs.c,v 1.2 2009/01/01 17:23:48 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_statistic.h"
#include "catalog/pg_type.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "tsearch/ts_type.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"


/*
 * The default text search selectivity is chosen to be small enough to
 * encourage indexscans for typical table densities.  See selfuncs.h and
 * DEFAULT_EQ_SEL for details.
 */
#define DEFAULT_TS_MATCH_SEL 0.005

/* lookup table type for binary searching through MCELEMs */
typedef struct
{
	text   *element;
	float4	frequency;
} TextFreq;

/* type of keys for bsearch'ing through an array of TextFreqs */
typedef struct
{
	char	*lexeme;
	int		length;
} LexemeKey;

static Selectivity tsquerysel(VariableStatData *vardata, Datum constval);
static Selectivity mcelem_tsquery_selec(TSQuery query,
										Datum *mcelem, int nmcelem,
										float4 *numbers, int nnumbers);
static Selectivity tsquery_opr_selec(QueryItem *item, char *operand,
								 TextFreq *lookup, int length, float4 minfreq);
static int	compare_lexeme_textfreq(const void *e1, const void *e2);


/*
 *	tsmatchsel -- Selectivity of "@@"
 *
 * restriction selectivity function for tsvector @@ tsquery and
 * tsquery @@ tsvector
 */
Datum
tsmatchsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
#ifdef NOT_USED
	Oid			operator = PG_GETARG_OID(1);
#endif
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Selectivity	selec;

	/*
	 * If expression is not variable = something or something = variable, then
	 * punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_TS_MATCH_SEL);

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_TS_MATCH_SEL);
	}

	/*
	 * The "@@" operator is strict, so we can cope with NULL right away
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	/*
	 * OK, there's a Var and a Const we're dealing with here. We need the Var
	 * to be a TSVector (or else we don't have any useful statistic for it).
	 * We have to check this because the Var might be the TSQuery not the
	 * TSVector.
	 */
	if (vardata.vartype == TSVECTOROID)
	{
		/* tsvector @@ tsquery or the other way around */
		Assert(((Const *) other)->consttype == TSQUERYOID);

		selec = tsquerysel(&vardata, ((Const *) other)->constvalue);
	}
	else
	{
		/* The Var is something we don't have useful statistics for */
		selec = DEFAULT_TS_MATCH_SEL;
	}

	ReleaseVariableStats(vardata);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}


/*
 *	tsmatchjoinsel -- join selectivity of "@@"
 *
 * join selectivity function for tsvector @@ tsquery and tsquery @@ tsvector
 */
Datum
tsmatchjoinsel(PG_FUNCTION_ARGS)
{
	/* for the moment we just punt */
	PG_RETURN_FLOAT8(DEFAULT_TS_MATCH_SEL);
}


/*
 * @@ selectivity for tsvector var vs tsquery constant
 */
static Selectivity
tsquerysel(VariableStatData *vardata, Datum constval)
{
	Selectivity			selec;

	if (HeapTupleIsValid(vardata->statsTuple))
	{
		TSQuery				query;
		Form_pg_statistic	stats;
		Datum				*values;
		int					nvalues;
		float4				*numbers;
		int					nnumbers;

		/* The caller made sure the const is a TSQuery, so get it now */
		query = DatumGetTSQuery(constval);

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

		/* MCELEM will be an array of TEXT elements for a tsvector column */
		if (get_attstatsslot(vardata->statsTuple,
							 TEXTOID, -1,
							 STATISTIC_KIND_MCELEM, InvalidOid,
							 &values, &nvalues,
							 &numbers, &nnumbers))
		{
			/*
			 * There is a most-common-elements slot for the tsvector Var, so
			 * use that.
			 */
			selec = mcelem_tsquery_selec(query, values, nvalues,
										 numbers, nnumbers);
			free_attstatsslot(TEXTOID, values, nvalues, numbers, nnumbers);
		}
		else
		{
			/* No most-common-elements info, so we must punt */
			selec = (Selectivity) DEFAULT_TS_MATCH_SEL;
		}
	}
	else
	{
		/* No stats at all, so we must punt */
		selec = (Selectivity) DEFAULT_TS_MATCH_SEL;
	}

	return selec;
}

/*
 * Extract data from the pg_statistic arrays into useful format.
 */
static Selectivity
mcelem_tsquery_selec(TSQuery query, Datum *mcelem, int nmcelem,
					 float4 *numbers, int nnumbers)
{
	float4			minfreq;
	TextFreq		*lookup;
	Selectivity		selec;
	int				i;

	/*
	 * There should be two more Numbers than Values, because the last two
	 * cells are taken for minimal and maximal frequency.  Punt if not.
	 */
	if (nnumbers != nmcelem + 2)
		return DEFAULT_TS_MATCH_SEL;

	/*
	 * Transpose the data into a single array so we can use bsearch().
	 */
	lookup = (TextFreq *) palloc(sizeof(TextFreq) * nmcelem);
	for (i = 0; i < nmcelem; i++)
	{
		/*
		 * The text Datums came from an array, so it cannot be compressed
		 * or stored out-of-line -- it's safe to use VARSIZE_ANY*.
		 */
		Assert(!VARATT_IS_COMPRESSED(mcelem[i]) && !VARATT_IS_EXTERNAL(mcelem[i]));
		lookup[i].element = (text *) DatumGetPointer(mcelem[i]);
		lookup[i].frequency = numbers[i];
	}

	/*
	 * Grab the lowest frequency. compute_tsvector_stats() stored it for us in
	 * the one before the last cell of the Numbers array. See ts_typanalyze.c
	 */
	minfreq = numbers[nnumbers - 2];

	selec = tsquery_opr_selec(GETQUERY(query), GETOPERAND(query), lookup,
							  nmcelem, minfreq);

	pfree(lookup);

	return selec;
}

/*
 * Traverse the tsquery in preorder, calculating selectivity as:
 *
 *   selec(left_oper) * selec(right_oper) in AND nodes,
 *
 *   selec(left_oper) + selec(right_oper) -
 *      selec(left_oper) * selec(right_oper) in OR nodes,
 *
 *   1 - select(oper) in NOT nodes
 *
 *   freq[val] in VAL nodes, if the value is in MCELEM
 *   min(freq[MCELEM]) / 2 in VAL nodes, if it is not
 *
 *
 * The MCELEM array is already sorted (see ts_typanalyze.c), so we can use
 * binary search for determining freq[MCELEM].
 */
static Selectivity
tsquery_opr_selec(QueryItem *item, char *operand,
				  TextFreq *lookup, int length, float4 minfreq)
{
	LexemeKey	key;
	TextFreq	*searchres;
	Selectivity	selec, s1, s2;

	/* since this function recurses, it could be driven to stack overflow */
	check_stack_depth();

	if (item->type == QI_VAL)
	{
		QueryOperand *oper = (QueryOperand *) item;

		/*
		 * Prepare the key for bsearch().
		 */
		key.lexeme = operand + oper->distance;
		key.length = oper->length;

		searchres = (TextFreq *) bsearch(&key, lookup, length,
										 sizeof(TextFreq),
										 compare_lexeme_textfreq);

		if (searchres)
		{
			/*
			 * The element is in MCELEM. Return precise selectivity (or at
			 * least as precise as ANALYZE could find out).
			 */
			return (Selectivity) searchres->frequency;
		}
		else
		{
			/*
			 * The element is not in MCELEM. Punt, but assert that the
			 * selectivity cannot be more than minfreq / 2.
			 */
			return (Selectivity) Min(DEFAULT_TS_MATCH_SEL, minfreq / 2);
		}
	}

	/* Current TSQuery node is an operator */
	switch (item->operator.oper)
	{
		case OP_NOT:
			selec =  1.0 - tsquery_opr_selec(item + 1, operand,
											 lookup, length, minfreq);
			break;

		case OP_AND:
			s1 = tsquery_opr_selec(item + 1, operand,
									 lookup, length, minfreq);
			s2 = tsquery_opr_selec(item + item->operator.left, operand,
								   lookup, length, minfreq);
			selec = s1 * s2;
			break;

		case OP_OR:
			s1 = tsquery_opr_selec(item + 1, operand,
								   lookup, length, minfreq);
			s2 = tsquery_opr_selec(item + item->operator.left, operand,
								   lookup, length, minfreq);
			selec = s1 + s2 - s1 * s2;
			break;

		default:
			elog(ERROR, "unrecognized operator: %d", item->operator.oper);
			selec = 0;			/* keep compiler quiet */
			break;
	}

	/* Clamp intermediate results to stay sane despite roundoff error */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * bsearch() comparator for a lexeme (non-NULL terminated string with length)
 * and a TextFreq. Use length, then byte-for-byte comparison, because that's
 * how ANALYZE code sorted data before storing it in a statistic tuple.
 * See ts_typanalyze.c for details.
 */
static int
compare_lexeme_textfreq(const void *e1, const void *e2)
{
	const LexemeKey	*key = (const LexemeKey *) e1;
	const TextFreq	*t = (const TextFreq *) e2;
	int				len1,
					len2;

	len1 = key->length;
	len2 = VARSIZE_ANY_EXHDR(t->element);

	/* Compare lengths first, possibly avoiding a strncmp call */
	if (len1 > len2)
		return 1;
	else if (len1 < len2)
		return -1;

	/* Fall back on byte-for-byte comparison */
	return strncmp(key->lexeme, VARDATA_ANY(t->element), len1);
}
