/*-------------------------------------------------------------------------
 *
 * selfuncs.c
 *	  Selectivity functions and index cost estimation functions for
 *	  standard operators and index access methods.
 *
 *	  Selectivity routines are registered in the pg_operator catalog
 *	  in the "oprrest" and "oprjoin" attributes.
 *
 *	  Index cost functions are located via the index AM's API struct,
 *	  which is obtained from the handler function registered in pg_am.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/selfuncs.c
 *
 *-------------------------------------------------------------------------
 */

/*----------
 * Operator selectivity estimation functions are called to estimate the
 * selectivity of WHERE clauses whose top-level operator is their operator.
 * We divide the problem into two cases:
 *		Restriction clause estimation: the clause involves vars of just
 *			one relation.
 *		Join clause estimation: the clause involves vars of multiple rels.
 * Join selectivity estimation is far more difficult and usually less accurate
 * than restriction estimation.
 *
 * When dealing with the inner scan of a nestloop join, we consider the
 * join's joinclauses as restriction clauses for the inner relation, and
 * treat vars of the outer relation as parameters (a/k/a constants of unknown
 * values).  So, restriction estimators need to be able to accept an argument
 * telling which relation is to be treated as the variable.
 *
 * The call convention for a restriction estimator (oprrest function) is
 *
 *		Selectivity oprrest (PlannerInfo *root,
 *							 Oid operator,
 *							 List *args,
 *							 int varRelid);
 *
 * root: general information about the query (rtable and RelOptInfo lists
 * are particularly important for the estimator).
 * operator: OID of the specific operator in question.
 * args: argument list from the operator clause.
 * varRelid: if not zero, the relid (rtable index) of the relation to
 * be treated as the variable relation.  May be zero if the args list
 * is known to contain vars of only one relation.
 *
 * This is represented at the SQL level (in pg_proc) as
 *
 *		float8 oprrest (internal, oid, internal, int4);
 *
 * The result is a selectivity, that is, a fraction (0 to 1) of the rows
 * of the relation that are expected to produce a TRUE result for the
 * given operator.
 *
 * The call convention for a join estimator (oprjoin function) is similar
 * except that varRelid is not needed, and instead join information is
 * supplied:
 *
 *		Selectivity oprjoin (PlannerInfo *root,
 *							 Oid operator,
 *							 List *args,
 *							 JoinType jointype,
 *							 SpecialJoinInfo *sjinfo);
 *
 *		float8 oprjoin (internal, oid, internal, int2, internal);
 *
 * (Before Postgres 8.4, join estimators had only the first four of these
 * parameters.  That signature is still allowed, but deprecated.)  The
 * relationship between jointype and sjinfo is explained in the comments for
 * clause_selectivity() --- the short version is that jointype is usually
 * best ignored in favor of examining sjinfo.
 *
 * Join selectivity for regular inner and outer joins is defined as the
 * fraction (0 to 1) of the cross product of the relations that is expected
 * to produce a TRUE result for the given operator.  For both semi and anti
 * joins, however, the selectivity is defined as the fraction of the left-hand
 * side relation's rows that are expected to have a match (ie, at least one
 * row with a TRUE result) in the right-hand side.
 *
 * For both oprrest and oprjoin functions, the operator's input collation OID
 * (if any) is passed using the standard fmgr mechanism, so that the estimator
 * function can fetch it with PG_GET_COLLATION().  Note, however, that all
 * statistics in pg_statistic are currently built using the relevant column's
 * collation.
 *----------
 */

#include "postgres.h"

#include <ctype.h>
#include <math.h>

#include "access/brin.h"
#include "access/brin_page.h"
#include "access/gin.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/visibilitymap.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "catalog/pg_statistic_ext.h"
#include "executor/nodeAgg.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "parser/parse_clause.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "statistics/statistics.h"
#include "storage/bufmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"
#include "utils/spccache.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/typcache.h"

#define DEFAULT_PAGE_CPU_MULTIPLIER 50.0

/* Hooks for plugins to get control when we ask for stats */
get_relation_stats_hook_type get_relation_stats_hook = NULL;
get_index_stats_hook_type get_index_stats_hook = NULL;

static double eqsel_internal(PG_FUNCTION_ARGS, bool negate);
static double eqjoinsel_inner(Oid opfuncoid, Oid collation,
							  VariableStatData *vardata1, VariableStatData *vardata2,
							  double nd1, double nd2,
							  bool isdefault1, bool isdefault2,
							  AttStatsSlot *sslot1, AttStatsSlot *sslot2,
							  Form_pg_statistic stats1, Form_pg_statistic stats2,
							  bool have_mcvs1, bool have_mcvs2);
static double eqjoinsel_semi(Oid opfuncoid, Oid collation,
							 VariableStatData *vardata1, VariableStatData *vardata2,
							 double nd1, double nd2,
							 bool isdefault1, bool isdefault2,
							 AttStatsSlot *sslot1, AttStatsSlot *sslot2,
							 Form_pg_statistic stats1, Form_pg_statistic stats2,
							 bool have_mcvs1, bool have_mcvs2,
							 RelOptInfo *inner_rel);
static bool estimate_multivariate_ndistinct(PlannerInfo *root,
											RelOptInfo *rel, List **varinfos, double *ndistinct);
static bool convert_to_scalar(Datum value, Oid valuetypid, Oid collid,
							  double *scaledvalue,
							  Datum lobound, Datum hibound, Oid boundstypid,
							  double *scaledlobound, double *scaledhibound);
static double convert_numeric_to_scalar(Datum value, Oid typid, bool *failure);
static void convert_string_to_scalar(char *value,
									 double *scaledvalue,
									 char *lobound,
									 double *scaledlobound,
									 char *hibound,
									 double *scaledhibound);
static void convert_bytea_to_scalar(Datum value,
									double *scaledvalue,
									Datum lobound,
									double *scaledlobound,
									Datum hibound,
									double *scaledhibound);
static double convert_one_string_to_scalar(char *value,
										   int rangelo, int rangehi);
static double convert_one_bytea_to_scalar(unsigned char *value, int valuelen,
										  int rangelo, int rangehi);
static char *convert_string_datum(Datum value, Oid typid, Oid collid,
								  bool *failure);
static double convert_timevalue_to_scalar(Datum value, Oid typid,
										  bool *failure);
static void examine_simple_variable(PlannerInfo *root, Var *var,
									VariableStatData *vardata);
static void examine_indexcol_variable(PlannerInfo *root, IndexOptInfo *index,
									  int indexcol, VariableStatData *vardata);
static bool get_variable_range(PlannerInfo *root, VariableStatData *vardata,
							   Oid sortop, Oid collation,
							   Datum *min, Datum *max);
static void get_stats_slot_range(AttStatsSlot *sslot,
								 Oid opfuncoid, FmgrInfo *opproc,
								 Oid collation, int16 typLen, bool typByVal,
								 Datum *min, Datum *max, bool *p_have_data);
static bool get_actual_variable_range(PlannerInfo *root,
									  VariableStatData *vardata,
									  Oid sortop, Oid collation,
									  Datum *min, Datum *max);
static bool get_actual_variable_endpoint(Relation heapRel,
										 Relation indexRel,
										 ScanDirection indexscandir,
										 ScanKey scankeys,
										 int16 typLen,
										 bool typByVal,
										 TupleTableSlot *tableslot,
										 MemoryContext outercontext,
										 Datum *endpointDatum);
static RelOptInfo *find_join_input_rel(PlannerInfo *root, Relids relids);
static double btcost_correlation(IndexOptInfo *index,
								 VariableStatData *vardata);


/*
 *		eqsel			- Selectivity of "=" for any data types.
 *
 * Note: this routine is also used to estimate selectivity for some
 * operators that are not "=" but have comparable selectivity behavior,
 * such as "~=" (geometric approximate-match).  Even for "=", we must
 * keep in mind that the left and right datatypes may differ.
 */
Datum
eqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8((float8) eqsel_internal(fcinfo, false));
}

/*
 * Common code for eqsel() and neqsel()
 */
static double
eqsel_internal(PG_FUNCTION_ARGS, bool negate)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	double		selec;

	/*
	 * When asked about <>, we do the estimation using the corresponding =
	 * operator, then convert to <> via "1.0 - eq_selectivity - nullfrac".
	 */
	if (negate)
	{
		operator = get_negator(operator);
		if (!OidIsValid(operator))
		{
			/* Use default selectivity (should we raise an error instead?) */
			return 1.0 - DEFAULT_EQ_SEL;
		}
	}

	/*
	 * If expression is not variable = something or something = variable, then
	 * punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return negate ? (1.0 - DEFAULT_EQ_SEL) : DEFAULT_EQ_SEL;

	/*
	 * We can do a lot better if the something is a constant.  (Note: the
	 * Const might result from estimation rather than being a simple constant
	 * in the query.)
	 */
	if (IsA(other, Const))
		selec = var_eq_const(&vardata, operator, collation,
							 ((Const *) other)->constvalue,
							 ((Const *) other)->constisnull,
							 varonleft, negate);
	else
		selec = var_eq_non_const(&vardata, operator, collation, other,
								 varonleft, negate);

	ReleaseVariableStats(vardata);

	return selec;
}

/*
 * var_eq_const --- eqsel for var = const case
 *
 * This is exported so that some other estimation functions can use it.
 */
double
var_eq_const(VariableStatData *vardata, Oid oproid, Oid collation,
			 Datum constval, bool constisnull,
			 bool varonleft, bool negate)
{
	double		selec;
	double		nullfrac = 0.0;
	bool		isdefault;
	Oid			opfuncoid;

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.  (It's zero even for a negator op.)
	 */
	if (constisnull)
		return 0.0;

	/*
	 * Grab the nullfrac for use below.  Note we allow use of nullfrac
	 * regardless of security check.
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * If we matched the var to a unique index, DISTINCT or GROUP-BY clause,
	 * assume there is exactly one match regardless of anything else.  (This
	 * is slightly bogus, since the index or clause's equality operator might
	 * be different from ours, but it's much more likely to be right than
	 * ignoring the information.)
	 */
	if (vardata->isunique && vardata->rel && vardata->rel->tuples >= 1.0)
	{
		selec = 1.0 / vardata->rel->tuples;
	}
	else if (HeapTupleIsValid(vardata->statsTuple) &&
			 statistic_proc_security_check(vardata,
										   (opfuncoid = get_opcode(oproid))))
	{
		AttStatsSlot sslot;
		bool		match = false;
		int			i;

		/*
		 * Is the constant "=" to any of the column's most common values?
		 * (Although the given operator may not really be "=", we will assume
		 * that seeing whether it returns TRUE is an appropriate test.  If you
		 * don't like this, maybe you shouldn't be using eqsel for your
		 * operator...)
		 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
		{
			LOCAL_FCINFO(fcinfo, 2);
			FmgrInfo	eqproc;

			fmgr_info(opfuncoid, &eqproc);

			/*
			 * Save a few cycles by setting up the fcinfo struct just once.
			 * Using FunctionCallInvoke directly also avoids failure if the
			 * eqproc returns NULL, though really equality functions should
			 * never do that.
			 */
			InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			/* be careful to apply operator right way 'round */
			if (varonleft)
				fcinfo->args[1].value = constval;
			else
				fcinfo->args[0].value = constval;

			for (i = 0; i < sslot.nvalues; i++)
			{
				Datum		fresult;

				if (varonleft)
					fcinfo->args[0].value = sslot.values[i];
				else
					fcinfo->args[1].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					match = true;
					break;
				}
			}
		}
		else
		{
			/* no most-common-value info available */
			i = 0;				/* keep compiler quiet */
		}

		if (match)
		{
			/*
			 * Constant is "=" to this common value.  We know selectivity
			 * exactly (or as exactly as ANALYZE could calculate it, anyway).
			 */
			selec = sslot.numbers[i];
		}
		else
		{
			/*
			 * Comparison is against a constant that is neither NULL nor any
			 * of the common values.  Its selectivity cannot be more than
			 * this:
			 */
			double		sumcommon = 0.0;
			double		otherdistinct;

			for (i = 0; i < sslot.nnumbers; i++)
				sumcommon += sslot.numbers[i];
			selec = 1.0 - sumcommon - nullfrac;
			CLAMP_PROBABILITY(selec);

			/*
			 * and in fact it's probably a good deal less. We approximate that
			 * all the not-common values share this remaining fraction
			 * equally, so we divide by the number of other distinct values.
			 */
			otherdistinct = get_variable_numdistinct(vardata, &isdefault) -
				sslot.nnumbers;
			if (otherdistinct > 1)
				selec /= otherdistinct;

			/*
			 * Another cross-check: selectivity shouldn't be estimated as more
			 * than the least common "most common value".
			 */
			if (sslot.nnumbers > 0 && selec > sslot.numbers[sslot.nnumbers - 1])
				selec = sslot.numbers[sslot.nnumbers - 1];
		}

		free_attstatsslot(&sslot);
	}
	else
	{
		/*
		 * No ANALYZE stats available, so make a guess using estimated number
		 * of distinct values and assuming they are equally common. (The guess
		 * is unlikely to be very good, but we do know a few special cases.)
		 */
		selec = 1.0 / get_variable_numdistinct(vardata, &isdefault);
	}

	/* now adjust if we wanted <> rather than = */
	if (negate)
		selec = 1.0 - selec - nullfrac;

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 * var_eq_non_const --- eqsel for var = something-other-than-const case
 *
 * This is exported so that some other estimation functions can use it.
 */
double
var_eq_non_const(VariableStatData *vardata, Oid oproid, Oid collation,
				 Node *other,
				 bool varonleft, bool negate)
{
	double		selec;
	double		nullfrac = 0.0;
	bool		isdefault;

	/*
	 * Grab the nullfrac for use below.
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		nullfrac = stats->stanullfrac;
	}

	/*
	 * If we matched the var to a unique index, DISTINCT or GROUP-BY clause,
	 * assume there is exactly one match regardless of anything else.  (This
	 * is slightly bogus, since the index or clause's equality operator might
	 * be different from ours, but it's much more likely to be right than
	 * ignoring the information.)
	 */
	if (vardata->isunique && vardata->rel && vardata->rel->tuples >= 1.0)
	{
		selec = 1.0 / vardata->rel->tuples;
	}
	else if (HeapTupleIsValid(vardata->statsTuple))
	{
		double		ndistinct;
		AttStatsSlot sslot;

		/*
		 * Search is for a value that we do not know a priori, but we will
		 * assume it is not NULL.  Estimate the selectivity as non-null
		 * fraction divided by number of distinct values, so that we get a
		 * result averaged over all possible values whether common or
		 * uncommon.  (Essentially, we are assuming that the not-yet-known
		 * comparison value is equally likely to be any of the possible
		 * values, regardless of their frequency in the table.  Is that a good
		 * idea?)
		 */
		selec = 1.0 - nullfrac;
		ndistinct = get_variable_numdistinct(vardata, &isdefault);
		if (ndistinct > 1)
			selec /= ndistinct;

		/*
		 * Cross-check: selectivity should never be estimated as more than the
		 * most common value's.
		 */
		if (get_attstatsslot(&sslot, vardata->statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			if (sslot.nnumbers > 0 && selec > sslot.numbers[0])
				selec = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
	}
	else
	{
		/*
		 * No ANALYZE stats available, so make a guess using estimated number
		 * of distinct values and assuming they are equally common. (The guess
		 * is unlikely to be very good, but we do know a few special cases.)
		 */
		selec = 1.0 / get_variable_numdistinct(vardata, &isdefault);
	}

	/* now adjust if we wanted <> rather than = */
	if (negate)
		selec = 1.0 - selec - nullfrac;

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *		neqsel			- Selectivity of "!=" for any data types.
 *
 * This routine is also used for some operators that are not "!="
 * but have comparable selectivity behavior.  See above comments
 * for eqsel().
 */
Datum
neqsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8((float8) eqsel_internal(fcinfo, true));
}

/*
 *	scalarineqsel		- Selectivity of "<", "<=", ">", ">=" for scalars.
 *
 * This is the guts of scalarltsel/scalarlesel/scalargtsel/scalargesel.
 * The isgt and iseq flags distinguish which of the four cases apply.
 *
 * The caller has commuted the clause, if necessary, so that we can treat
 * the variable as being on the left.  The caller must also make sure that
 * the other side of the clause is a non-null Const, and dissect that into
 * a value and datatype.  (This definition simplifies some callers that
 * want to estimate against a computed value instead of a Const node.)
 *
 * This routine works for any datatype (or pair of datatypes) known to
 * convert_to_scalar().  If it is applied to some other datatype,
 * it will return an approximate estimate based on assuming that the constant
 * value falls in the middle of the bin identified by binary search.
 */
static double
scalarineqsel(PlannerInfo *root, Oid operator, bool isgt, bool iseq,
			  Oid collation,
			  VariableStatData *vardata, Datum constval, Oid consttype)
{
	Form_pg_statistic stats;
	FmgrInfo	opproc;
	double		mcv_selec,
				hist_selec,
				sumcommon;
	double		selec;

	if (!HeapTupleIsValid(vardata->statsTuple))
	{
		/*
		 * No stats are available.  Typically this means we have to fall back
		 * on the default estimate; but if the variable is CTID then we can
		 * make an estimate based on comparing the constant to the table size.
		 */
		if (vardata->var && IsA(vardata->var, Var) &&
			((Var *) vardata->var)->varattno == SelfItemPointerAttributeNumber)
		{
			ItemPointer itemptr;
			double		block;
			double		density;

			/*
			 * If the relation's empty, we're going to include all of it.
			 * (This is mostly to avoid divide-by-zero below.)
			 */
			if (vardata->rel->pages == 0)
				return 1.0;

			itemptr = (ItemPointer) DatumGetPointer(constval);
			block = ItemPointerGetBlockNumberNoCheck(itemptr);

			/*
			 * Determine the average number of tuples per page (density).
			 *
			 * Since the last page will, on average, be only half full, we can
			 * estimate it to have half as many tuples as earlier pages.  So
			 * give it half the weight of a regular page.
			 */
			density = vardata->rel->tuples / (vardata->rel->pages - 0.5);

			/* If target is the last page, use half the density. */
			if (block >= vardata->rel->pages - 1)
				density *= 0.5;

			/*
			 * Using the average tuples per page, calculate how far into the
			 * page the itemptr is likely to be and adjust block accordingly,
			 * by adding that fraction of a whole block (but never more than a
			 * whole block, no matter how high the itemptr's offset is).  Here
			 * we are ignoring the possibility of dead-tuple line pointers,
			 * which is fairly bogus, but we lack the info to do better.
			 */
			if (density > 0.0)
			{
				OffsetNumber offset = ItemPointerGetOffsetNumberNoCheck(itemptr);

				block += Min(offset / density, 1.0);
			}

			/*
			 * Convert relative block number to selectivity.  Again, the last
			 * page has only half weight.
			 */
			selec = block / (vardata->rel->pages - 0.5);

			/*
			 * The calculation so far gave us a selectivity for the "<=" case.
			 * We'll have one fewer tuple for "<" and one additional tuple for
			 * ">=", the latter of which we'll reverse the selectivity for
			 * below, so we can simply subtract one tuple for both cases.  The
			 * cases that need this adjustment can be identified by iseq being
			 * equal to isgt.
			 */
			if (iseq == isgt && vardata->rel->tuples >= 1.0)
				selec -= (1.0 / vardata->rel->tuples);

			/* Finally, reverse the selectivity for the ">", ">=" cases. */
			if (isgt)
				selec = 1.0 - selec;

			CLAMP_PROBABILITY(selec);
			return selec;
		}

		/* no stats available, so default result */
		return DEFAULT_INEQ_SEL;
	}
	stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);

	fmgr_info(get_opcode(operator), &opproc);

	/*
	 * If we have most-common-values info, add up the fractions of the MCV
	 * entries that satisfy MCV OP CONST.  These fractions contribute directly
	 * to the result selectivity.  Also add up the total fraction represented
	 * by MCV entries.
	 */
	mcv_selec = mcv_selectivity(vardata, &opproc, collation, constval, true,
								&sumcommon);

	/*
	 * If there is a histogram, determine which bin the constant falls in, and
	 * compute the resulting contribution to selectivity.
	 */
	hist_selec = ineq_histogram_selectivity(root, vardata,
											operator, &opproc, isgt, iseq,
											collation,
											constval, consttype);

	/*
	 * Now merge the results from the MCV and histogram calculations,
	 * realizing that the histogram covers only the non-null values that are
	 * not listed in MCV.
	 */
	selec = 1.0 - stats->stanullfrac - sumcommon;

	if (hist_selec >= 0.0)
		selec *= hist_selec;
	else
	{
		/*
		 * If no histogram but there are values not accounted for by MCV,
		 * arbitrarily assume half of them will match.
		 */
		selec *= 0.5;
	}

	selec += mcv_selec;

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *	mcv_selectivity			- Examine the MCV list for selectivity estimates
 *
 * Determine the fraction of the variable's MCV population that satisfies
 * the predicate (VAR OP CONST), or (CONST OP VAR) if !varonleft.  Also
 * compute the fraction of the total column population represented by the MCV
 * list.  This code will work for any boolean-returning predicate operator.
 *
 * The function result is the MCV selectivity, and the fraction of the
 * total population is returned into *sumcommonp.  Zeroes are returned
 * if there is no MCV list.
 */
double
mcv_selectivity(VariableStatData *vardata, FmgrInfo *opproc, Oid collation,
				Datum constval, bool varonleft,
				double *sumcommonp)
{
	double		mcv_selec,
				sumcommon;
	AttStatsSlot sslot;
	int			i;

	mcv_selec = 0.0;
	sumcommon = 0.0;

	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_MCV, InvalidOid,
						 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS))
	{
		LOCAL_FCINFO(fcinfo, 2);

		/*
		 * We invoke the opproc "by hand" so that we won't fail on NULL
		 * results.  Such cases won't arise for normal comparison functions,
		 * but generic_restriction_selectivity could perhaps be used with
		 * operators that can return NULL.  A small side benefit is to not
		 * need to re-initialize the fcinfo struct from scratch each time.
		 */
		InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;
		/* be careful to apply operator right way 'round */
		if (varonleft)
			fcinfo->args[1].value = constval;
		else
			fcinfo->args[0].value = constval;

		for (i = 0; i < sslot.nvalues; i++)
		{
			Datum		fresult;

			if (varonleft)
				fcinfo->args[0].value = sslot.values[i];
			else
				fcinfo->args[1].value = sslot.values[i];
			fcinfo->isnull = false;
			fresult = FunctionCallInvoke(fcinfo);
			if (!fcinfo->isnull && DatumGetBool(fresult))
				mcv_selec += sslot.numbers[i];
			sumcommon += sslot.numbers[i];
		}
		free_attstatsslot(&sslot);
	}

	*sumcommonp = sumcommon;
	return mcv_selec;
}

/*
 *	histogram_selectivity	- Examine the histogram for selectivity estimates
 *
 * Determine the fraction of the variable's histogram entries that satisfy
 * the predicate (VAR OP CONST), or (CONST OP VAR) if !varonleft.
 *
 * This code will work for any boolean-returning predicate operator, whether
 * or not it has anything to do with the histogram sort operator.  We are
 * essentially using the histogram just as a representative sample.  However,
 * small histograms are unlikely to be all that representative, so the caller
 * should be prepared to fall back on some other estimation approach when the
 * histogram is missing or very small.  It may also be prudent to combine this
 * approach with another one when the histogram is small.
 *
 * If the actual histogram size is not at least min_hist_size, we won't bother
 * to do the calculation at all.  Also, if the n_skip parameter is > 0, we
 * ignore the first and last n_skip histogram elements, on the grounds that
 * they are outliers and hence not very representative.  Typical values for
 * these parameters are 10 and 1.
 *
 * The function result is the selectivity, or -1 if there is no histogram
 * or it's smaller than min_hist_size.
 *
 * The output parameter *hist_size receives the actual histogram size,
 * or zero if no histogram.  Callers may use this number to decide how
 * much faith to put in the function result.
 *
 * Note that the result disregards both the most-common-values (if any) and
 * null entries.  The caller is expected to combine this result with
 * statistics for those portions of the column population.  It may also be
 * prudent to clamp the result range, ie, disbelieve exact 0 or 1 outputs.
 */
double
histogram_selectivity(VariableStatData *vardata,
					  FmgrInfo *opproc, Oid collation,
					  Datum constval, bool varonleft,
					  int min_hist_size, int n_skip,
					  int *hist_size)
{
	double		result;
	AttStatsSlot sslot;

	/* check sanity of parameters */
	Assert(n_skip >= 0);
	Assert(min_hist_size > 2 * n_skip);

	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		*hist_size = sslot.nvalues;
		if (sslot.nvalues >= min_hist_size)
		{
			LOCAL_FCINFO(fcinfo, 2);
			int			nmatch = 0;
			int			i;

			/*
			 * We invoke the opproc "by hand" so that we won't fail on NULL
			 * results.  Such cases won't arise for normal comparison
			 * functions, but generic_restriction_selectivity could perhaps be
			 * used with operators that can return NULL.  A small side benefit
			 * is to not need to re-initialize the fcinfo struct from scratch
			 * each time.
			 */
			InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			/* be careful to apply operator right way 'round */
			if (varonleft)
				fcinfo->args[1].value = constval;
			else
				fcinfo->args[0].value = constval;

			for (i = n_skip; i < sslot.nvalues - n_skip; i++)
			{
				Datum		fresult;

				if (varonleft)
					fcinfo->args[0].value = sslot.values[i];
				else
					fcinfo->args[1].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
					nmatch++;
			}
			result = ((double) nmatch) / ((double) (sslot.nvalues - 2 * n_skip));
		}
		else
			result = -1;
		free_attstatsslot(&sslot);
	}
	else
	{
		*hist_size = 0;
		result = -1;
	}

	return result;
}

/*
 *	generic_restriction_selectivity		- Selectivity for almost anything
 *
 * This function estimates selectivity for operators that we don't have any
 * special knowledge about, but are on data types that we collect standard
 * MCV and/or histogram statistics for.  (Additional assumptions are that
 * the operator is strict and immutable, or at least stable.)
 *
 * If we have "VAR OP CONST" or "CONST OP VAR", selectivity is estimated by
 * applying the operator to each element of the column's MCV and/or histogram
 * stats, and merging the results using the assumption that the histogram is
 * a reasonable random sample of the column's non-MCV population.  Note that
 * if the operator's semantics are related to the histogram ordering, this
 * might not be such a great assumption; other functions such as
 * scalarineqsel() are probably a better match in such cases.
 *
 * Otherwise, fall back to the default selectivity provided by the caller.
 */
double
generic_restriction_selectivity(PlannerInfo *root, Oid oproid, Oid collation,
								List *args, int varRelid,
								double default_selectivity)
{
	double		selec;
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;

	/*
	 * If expression is not variable OP something or something OP variable,
	 * then punt and return the default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		return default_selectivity;

	/*
	 * If the something is a NULL constant, assume operator is strict and
	 * return zero, ie, operator will never return TRUE.
	 */
	if (IsA(other, Const) &&
		((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		return 0.0;
	}

	if (IsA(other, Const))
	{
		/* Variable is being compared to a known non-null constant */
		Datum		constval = ((Const *) other)->constvalue;
		FmgrInfo	opproc;
		double		mcvsum;
		double		mcvsel;
		double		nullfrac;
		int			hist_size;

		fmgr_info(get_opcode(oproid), &opproc);

		/*
		 * Calculate the selectivity for the column's most common values.
		 */
		mcvsel = mcv_selectivity(&vardata, &opproc, collation,
								 constval, varonleft,
								 &mcvsum);

		/*
		 * If the histogram is large enough, see what fraction of it matches
		 * the query, and assume that's representative of the non-MCV
		 * population.  Otherwise use the default selectivity for the non-MCV
		 * population.
		 */
		selec = histogram_selectivity(&vardata, &opproc, collation,
									  constval, varonleft,
									  10, 1, &hist_size);
		if (selec < 0)
		{
			/* Nope, fall back on default */
			selec = default_selectivity;
		}
		else if (hist_size < 100)
		{
			/*
			 * For histogram sizes from 10 to 100, we combine the histogram
			 * and default selectivities, putting increasingly more trust in
			 * the histogram for larger sizes.
			 */
			double		hist_weight = hist_size / 100.0;

			selec = selec * hist_weight +
				default_selectivity * (1.0 - hist_weight);
		}

		/* In any case, don't believe extremely small or large estimates. */
		if (selec < 0.0001)
			selec = 0.0001;
		else if (selec > 0.9999)
			selec = 0.9999;

		/* Don't forget to account for nulls. */
		if (HeapTupleIsValid(vardata.statsTuple))
			nullfrac = ((Form_pg_statistic) GETSTRUCT(vardata.statsTuple))->stanullfrac;
		else
			nullfrac = 0.0;

		/*
		 * Now merge the results from the MCV and histogram calculations,
		 * realizing that the histogram covers only the non-null values that
		 * are not listed in MCV.
		 */
		selec *= 1.0 - nullfrac - mcvsum;
		selec += mcvsel;
	}
	else
	{
		/* Comparison value is not constant, so we can't do anything */
		selec = default_selectivity;
	}

	ReleaseVariableStats(vardata);

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return selec;
}

/*
 *	ineq_histogram_selectivity	- Examine the histogram for scalarineqsel
 *
 * Determine the fraction of the variable's histogram population that
 * satisfies the inequality condition, ie, VAR < (or <=, >, >=) CONST.
 * The isgt and iseq flags distinguish which of the four cases apply.
 *
 * While opproc could be looked up from the operator OID, common callers
 * also need to call it separately, so we make the caller pass both.
 *
 * Returns -1 if there is no histogram (valid results will always be >= 0).
 *
 * Note that the result disregards both the most-common-values (if any) and
 * null entries.  The caller is expected to combine this result with
 * statistics for those portions of the column population.
 *
 * This is exported so that some other estimation functions can use it.
 */
double
ineq_histogram_selectivity(PlannerInfo *root,
						   VariableStatData *vardata,
						   Oid opoid, FmgrInfo *opproc, bool isgt, bool iseq,
						   Oid collation,
						   Datum constval, Oid consttype)
{
	double		hist_selec;
	AttStatsSlot sslot;

	hist_selec = -1.0;

	/*
	 * Someday, ANALYZE might store more than one histogram per rel/att,
	 * corresponding to more than one possible sort ordering defined for the
	 * column type.  Right now, we know there is only one, so just grab it and
	 * see if it matches the query.
	 *
	 * Note that we can't use opoid as search argument; the staop appearing in
	 * pg_statistic will be for the relevant '<' operator, but what we have
	 * might be some other inequality operator such as '>='.  (Even if opoid
	 * is a '<' operator, it could be cross-type.)  Hence we must use
	 * comparison_ops_are_compatible() to see if the operators match.
	 */
	if (HeapTupleIsValid(vardata->statsTuple) &&
		statistic_proc_security_check(vardata, opproc->fn_oid) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		if (sslot.nvalues > 1 &&
			sslot.stacoll == collation &&
			comparison_ops_are_compatible(sslot.staop, opoid))
		{
			/*
			 * Use binary search to find the desired location, namely the
			 * right end of the histogram bin containing the comparison value,
			 * which is the leftmost entry for which the comparison operator
			 * succeeds (if isgt) or fails (if !isgt).
			 *
			 * In this loop, we pay no attention to whether the operator iseq
			 * or not; that detail will be mopped up below.  (We cannot tell,
			 * anyway, whether the operator thinks the values are equal.)
			 *
			 * If the binary search accesses the first or last histogram
			 * entry, we try to replace that endpoint with the true column min
			 * or max as found by get_actual_variable_range().  This
			 * ameliorates misestimates when the min or max is moving as a
			 * result of changes since the last ANALYZE.  Note that this could
			 * result in effectively including MCVs into the histogram that
			 * weren't there before, but we don't try to correct for that.
			 */
			double		histfrac;
			int			lobound = 0;	/* first possible slot to search */
			int			hibound = sslot.nvalues;	/* last+1 slot to search */
			bool		have_end = false;

			/*
			 * If there are only two histogram entries, we'll want up-to-date
			 * values for both.  (If there are more than two, we need at most
			 * one of them to be updated, so we deal with that within the
			 * loop.)
			 */
			if (sslot.nvalues == 2)
				have_end = get_actual_variable_range(root,
													 vardata,
													 sslot.staop,
													 collation,
													 &sslot.values[0],
													 &sslot.values[1]);

			while (lobound < hibound)
			{
				int			probe = (lobound + hibound) / 2;
				bool		ltcmp;

				/*
				 * If we find ourselves about to compare to the first or last
				 * histogram entry, first try to replace it with the actual
				 * current min or max (unless we already did so above).
				 */
				if (probe == 0 && sslot.nvalues > 2)
					have_end = get_actual_variable_range(root,
														 vardata,
														 sslot.staop,
														 collation,
														 &sslot.values[0],
														 NULL);
				else if (probe == sslot.nvalues - 1 && sslot.nvalues > 2)
					have_end = get_actual_variable_range(root,
														 vardata,
														 sslot.staop,
														 collation,
														 NULL,
														 &sslot.values[probe]);

				ltcmp = DatumGetBool(FunctionCall2Coll(opproc,
													   collation,
													   sslot.values[probe],
													   constval));
				if (isgt)
					ltcmp = !ltcmp;
				if (ltcmp)
					lobound = probe + 1;
				else
					hibound = probe;
			}

			if (lobound <= 0)
			{
				/*
				 * Constant is below lower histogram boundary.  More
				 * precisely, we have found that no entry in the histogram
				 * satisfies the inequality clause (if !isgt) or they all do
				 * (if isgt).  We estimate that that's true of the entire
				 * table, so set histfrac to 0.0 (which we'll flip to 1.0
				 * below, if isgt).
				 */
				histfrac = 0.0;
			}
			else if (lobound >= sslot.nvalues)
			{
				/*
				 * Inverse case: constant is above upper histogram boundary.
				 */
				histfrac = 1.0;
			}
			else
			{
				/* We have values[i-1] <= constant <= values[i]. */
				int			i = lobound;
				double		eq_selec = 0;
				double		val,
							high,
							low;
				double		binfrac;

				/*
				 * In the cases where we'll need it below, obtain an estimate
				 * of the selectivity of "x = constval".  We use a calculation
				 * similar to what var_eq_const() does for a non-MCV constant,
				 * ie, estimate that all distinct non-MCV values occur equally
				 * often.  But multiplication by "1.0 - sumcommon - nullfrac"
				 * will be done by our caller, so we shouldn't do that here.
				 * Therefore we can't try to clamp the estimate by reference
				 * to the least common MCV; the result would be too small.
				 *
				 * Note: since this is effectively assuming that constval
				 * isn't an MCV, it's logically dubious if constval in fact is
				 * one.  But we have to apply *some* correction for equality,
				 * and anyway we cannot tell if constval is an MCV, since we
				 * don't have a suitable equality operator at hand.
				 */
				if (i == 1 || isgt == iseq)
				{
					double		otherdistinct;
					bool		isdefault;
					AttStatsSlot mcvslot;

					/* Get estimated number of distinct values */
					otherdistinct = get_variable_numdistinct(vardata,
															 &isdefault);

					/* Subtract off the number of known MCVs */
					if (get_attstatsslot(&mcvslot, vardata->statsTuple,
										 STATISTIC_KIND_MCV, InvalidOid,
										 ATTSTATSSLOT_NUMBERS))
					{
						otherdistinct -= mcvslot.nnumbers;
						free_attstatsslot(&mcvslot);
					}

					/* If result doesn't seem sane, leave eq_selec at 0 */
					if (otherdistinct > 1)
						eq_selec = 1.0 / otherdistinct;
				}

				/*
				 * Convert the constant and the two nearest bin boundary
				 * values to a uniform comparison scale, and do a linear
				 * interpolation within this bin.
				 */
				if (convert_to_scalar(constval, consttype, collation,
									  &val,
									  sslot.values[i - 1], sslot.values[i],
									  vardata->vartype,
									  &low, &high))
				{
					if (high <= low)
					{
						/* cope if bin boundaries appear identical */
						binfrac = 0.5;
					}
					else if (val <= low)
						binfrac = 0.0;
					else if (val >= high)
						binfrac = 1.0;
					else
					{
						binfrac = (val - low) / (high - low);

						/*
						 * Watch out for the possibility that we got a NaN or
						 * Infinity from the division.  This can happen
						 * despite the previous checks, if for example "low"
						 * is -Infinity.
						 */
						if (isnan(binfrac) ||
							binfrac < 0.0 || binfrac > 1.0)
							binfrac = 0.5;
					}
				}
				else
				{
					/*
					 * Ideally we'd produce an error here, on the grounds that
					 * the given operator shouldn't have scalarXXsel
					 * registered as its selectivity func unless we can deal
					 * with its operand types.  But currently, all manner of
					 * stuff is invoking scalarXXsel, so give a default
					 * estimate until that can be fixed.
					 */
					binfrac = 0.5;
				}

				/*
				 * Now, compute the overall selectivity across the values
				 * represented by the histogram.  We have i-1 full bins and
				 * binfrac partial bin below the constant.
				 */
				histfrac = (double) (i - 1) + binfrac;
				histfrac /= (double) (sslot.nvalues - 1);

				/*
				 * At this point, histfrac is an estimate of the fraction of
				 * the population represented by the histogram that satisfies
				 * "x <= constval".  Somewhat remarkably, this statement is
				 * true regardless of which operator we were doing the probes
				 * with, so long as convert_to_scalar() delivers reasonable
				 * results.  If the probe constant is equal to some histogram
				 * entry, we would have considered the bin to the left of that
				 * entry if probing with "<" or ">=", or the bin to the right
				 * if probing with "<=" or ">"; but binfrac would have come
				 * out as 1.0 in the first case and 0.0 in the second, leading
				 * to the same histfrac in either case.  For probe constants
				 * between histogram entries, we find the same bin and get the
				 * same estimate with any operator.
				 *
				 * The fact that the estimate corresponds to "x <= constval"
				 * and not "x < constval" is because of the way that ANALYZE
				 * constructs the histogram: each entry is, effectively, the
				 * rightmost value in its sample bucket.  So selectivity
				 * values that are exact multiples of 1/(histogram_size-1)
				 * should be understood as estimates including a histogram
				 * entry plus everything to its left.
				 *
				 * However, that breaks down for the first histogram entry,
				 * which necessarily is the leftmost value in its sample
				 * bucket.  That means the first histogram bin is slightly
				 * narrower than the rest, by an amount equal to eq_selec.
				 * Another way to say that is that we want "x <= leftmost" to
				 * be estimated as eq_selec not zero.  So, if we're dealing
				 * with the first bin (i==1), rescale to make that true while
				 * adjusting the rest of that bin linearly.
				 */
				if (i == 1)
					histfrac += eq_selec * (1.0 - binfrac);

				/*
				 * "x <= constval" is good if we want an estimate for "<=" or
				 * ">", but if we are estimating for "<" or ">=", we now need
				 * to decrease the estimate by eq_selec.
				 */
				if (isgt == iseq)
					histfrac -= eq_selec;
			}

			/*
			 * Now the estimate is finished for "<" and "<=" cases.  If we are
			 * estimating for ">" or ">=", flip it.
			 */
			hist_selec = isgt ? (1.0 - histfrac) : histfrac;

			/*
			 * The histogram boundaries are only approximate to begin with,
			 * and may well be out of date anyway.  Therefore, don't believe
			 * extremely small or large selectivity estimates --- unless we
			 * got actual current endpoint values from the table, in which
			 * case just do the usual sanity clamp.  Somewhat arbitrarily, we
			 * set the cutoff for other cases at a hundredth of the histogram
			 * resolution.
			 */
			if (have_end)
				CLAMP_PROBABILITY(hist_selec);
			else
			{
				double		cutoff = 0.01 / (double) (sslot.nvalues - 1);

				if (hist_selec < cutoff)
					hist_selec = cutoff;
				else if (hist_selec > 1.0 - cutoff)
					hist_selec = 1.0 - cutoff;
			}
		}
		else if (sslot.nvalues > 1)
		{
			/*
			 * If we get here, we have a histogram but it's not sorted the way
			 * we want.  Do a brute-force search to see how many of the
			 * entries satisfy the comparison condition, and take that
			 * fraction as our estimate.  (This is identical to the inner loop
			 * of histogram_selectivity; maybe share code?)
			 */
			LOCAL_FCINFO(fcinfo, 2);
			int			nmatch = 0;

			InitFunctionCallInfoData(*fcinfo, opproc, 2, collation,
									 NULL, NULL);
			fcinfo->args[0].isnull = false;
			fcinfo->args[1].isnull = false;
			fcinfo->args[1].value = constval;
			for (int i = 0; i < sslot.nvalues; i++)
			{
				Datum		fresult;

				fcinfo->args[0].value = sslot.values[i];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
					nmatch++;
			}
			hist_selec = ((double) nmatch) / ((double) sslot.nvalues);

			/*
			 * As above, clamp to a hundredth of the histogram resolution.
			 * This case is surely even less trustworthy than the normal one,
			 * so we shouldn't believe exact 0 or 1 selectivity.  (Maybe the
			 * clamp should be more restrictive in this case?)
			 */
			{
				double		cutoff = 0.01 / (double) (sslot.nvalues - 1);

				if (hist_selec < cutoff)
					hist_selec = cutoff;
				else if (hist_selec > 1.0 - cutoff)
					hist_selec = 1.0 - cutoff;
			}
		}

		free_attstatsslot(&sslot);
	}

	return hist_selec;
}

/*
 * Common wrapper function for the selectivity estimators that simply
 * invoke scalarineqsel().
 */
static Datum
scalarineqsel_wrapper(PG_FUNCTION_ARGS, bool isgt, bool iseq)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	VariableStatData vardata;
	Node	   *other;
	bool		varonleft;
	Datum		constval;
	Oid			consttype;
	double		selec;

	/*
	 * If expression is not variable op something or something op variable,
	 * then punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
	}

	/*
	 * If the constant is NULL, assume operator is strict and return zero, ie,
	 * operator will never return TRUE.
	 */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}
	constval = ((Const *) other)->constvalue;
	consttype = ((Const *) other)->consttype;

	/*
	 * Force the var to be on the left to simplify logic in scalarineqsel.
	 */
	if (!varonleft)
	{
		operator = get_commutator(operator);
		if (!operator)
		{
			/* Use default selectivity (should we raise an error instead?) */
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
		}
		isgt = !isgt;
	}

	/* The rest of the work is done by scalarineqsel(). */
	selec = scalarineqsel(root, operator, isgt, iseq, collation,
						  &vardata, constval, consttype);

	ReleaseVariableStats(vardata);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 *		scalarltsel		- Selectivity of "<" for scalars.
 */
Datum
scalarltsel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, false, false);
}

/*
 *		scalarlesel		- Selectivity of "<=" for scalars.
 */
Datum
scalarlesel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, false, true);
}

/*
 *		scalargtsel		- Selectivity of ">" for scalars.
 */
Datum
scalargtsel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, true, false);
}

/*
 *		scalargesel		- Selectivity of ">=" for scalars.
 */
Datum
scalargesel(PG_FUNCTION_ARGS)
{
	return scalarineqsel_wrapper(fcinfo, true, true);
}

/*
 *		boolvarsel		- Selectivity of Boolean variable.
 *
 * This can actually be called on any boolean-valued expression.  If it
 * involves only Vars of the specified relation, and if there are statistics
 * about the Var or expression (the latter is possible if it's indexed) then
 * we'll produce a real estimate; otherwise it's just a default.
 */
Selectivity
boolvarsel(PlannerInfo *root, Node *arg, int varRelid)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		/*
		 * A boolean variable V is equivalent to the clause V = 't', so we
		 * compute the selectivity as if that is what we have.
		 */
		selec = var_eq_const(&vardata, BooleanEqualOperator, InvalidOid,
							 BoolGetDatum(true), false, true, false);
	}
	else
	{
		/* Otherwise, the default estimate is 0.5 */
		selec = 0.5;
	}
	ReleaseVariableStats(vardata);
	return selec;
}

/*
 *		booltestsel		- Selectivity of BooleanTest Node.
 */
Selectivity
booltestsel(PlannerInfo *root, BoolTestType booltesttype, Node *arg,
			int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;
		double		freq_null;
		AttStatsSlot sslot;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		freq_null = stats->stanullfrac;

		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS)
			&& sslot.nnumbers > 0)
		{
			double		freq_true;
			double		freq_false;

			/*
			 * Get first MCV frequency and derive frequency for true.
			 */
			if (DatumGetBool(sslot.values[0]))
				freq_true = sslot.numbers[0];
			else
				freq_true = 1.0 - sslot.numbers[0] - freq_null;

			/*
			 * Next derive frequency for false. Then use these as appropriate
			 * to derive frequency for each case.
			 */
			freq_false = 1.0 - freq_true - freq_null;

			switch (booltesttype)
			{
				case IS_UNKNOWN:
					/* select only NULL values */
					selec = freq_null;
					break;
				case IS_NOT_UNKNOWN:
					/* select non-NULL values */
					selec = 1.0 - freq_null;
					break;
				case IS_TRUE:
					/* select only TRUE values */
					selec = freq_true;
					break;
				case IS_NOT_TRUE:
					/* select non-TRUE values */
					selec = 1.0 - freq_true;
					break;
				case IS_FALSE:
					/* select only FALSE values */
					selec = freq_false;
					break;
				case IS_NOT_FALSE:
					/* select non-FALSE values */
					selec = 1.0 - freq_false;
					break;
				default:
					elog(ERROR, "unrecognized booltesttype: %d",
						 (int) booltesttype);
					selec = 0.0;	/* Keep compiler quiet */
					break;
			}

			free_attstatsslot(&sslot);
		}
		else
		{
			/*
			 * No most-common-value info available. Still have null fraction
			 * information, so use it for IS [NOT] UNKNOWN. Otherwise adjust
			 * for null fraction and assume a 50-50 split of TRUE and FALSE.
			 */
			switch (booltesttype)
			{
				case IS_UNKNOWN:
					/* select only NULL values */
					selec = freq_null;
					break;
				case IS_NOT_UNKNOWN:
					/* select non-NULL values */
					selec = 1.0 - freq_null;
					break;
				case IS_TRUE:
				case IS_FALSE:
					/* Assume we select half of the non-NULL values */
					selec = (1.0 - freq_null) / 2.0;
					break;
				case IS_NOT_TRUE:
				case IS_NOT_FALSE:
					/* Assume we select NULLs plus half of the non-NULLs */
					/* equiv. to freq_null + (1.0 - freq_null) / 2.0 */
					selec = (freq_null + 1.0) / 2.0;
					break;
				default:
					elog(ERROR, "unrecognized booltesttype: %d",
						 (int) booltesttype);
					selec = 0.0;	/* Keep compiler quiet */
					break;
			}
		}
	}
	else
	{
		/*
		 * If we can't get variable statistics for the argument, perhaps
		 * clause_selectivity can do something with it.  We ignore the
		 * possibility of a NULL value when using clause_selectivity, and just
		 * assume the value is either TRUE or FALSE.
		 */
		switch (booltesttype)
		{
			case IS_UNKNOWN:
				selec = DEFAULT_UNK_SEL;
				break;
			case IS_NOT_UNKNOWN:
				selec = DEFAULT_NOT_UNK_SEL;
				break;
			case IS_TRUE:
			case IS_NOT_FALSE:
				selec = (double) clause_selectivity(root, arg,
													varRelid,
													jointype, sjinfo);
				break;
			case IS_FALSE:
			case IS_NOT_TRUE:
				selec = 1.0 - (double) clause_selectivity(root, arg,
														  varRelid,
														  jointype, sjinfo);
				break;
			default:
				elog(ERROR, "unrecognized booltesttype: %d",
					 (int) booltesttype);
				selec = 0.0;	/* Keep compiler quiet */
				break;
		}
	}

	ReleaseVariableStats(vardata);

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return (Selectivity) selec;
}

/*
 *		nulltestsel		- Selectivity of NullTest Node.
 */
Selectivity
nulltestsel(PlannerInfo *root, NullTestType nulltesttype, Node *arg,
			int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	VariableStatData vardata;
	double		selec;

	examine_variable(root, arg, varRelid, &vardata);

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;
		double		freq_null;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		freq_null = stats->stanullfrac;

		switch (nulltesttype)
		{
			case IS_NULL:

				/*
				 * Use freq_null directly.
				 */
				selec = freq_null;
				break;
			case IS_NOT_NULL:

				/*
				 * Select not unknown (not null) values. Calculate from
				 * freq_null.
				 */
				selec = 1.0 - freq_null;
				break;
			default:
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) nulltesttype);
				return (Selectivity) 0; /* keep compiler quiet */
		}
	}
	else if (vardata.var && IsA(vardata.var, Var) &&
			 ((Var *) vardata.var)->varattno < 0)
	{
		/*
		 * There are no stats for system columns, but we know they are never
		 * NULL.
		 */
		selec = (nulltesttype == IS_NULL) ? 0.0 : 1.0;
	}
	else
	{
		/*
		 * No ANALYZE stats available, so make a guess
		 */
		switch (nulltesttype)
		{
			case IS_NULL:
				selec = DEFAULT_UNK_SEL;
				break;
			case IS_NOT_NULL:
				selec = DEFAULT_NOT_UNK_SEL;
				break;
			default:
				elog(ERROR, "unrecognized nulltesttype: %d",
					 (int) nulltesttype);
				return (Selectivity) 0; /* keep compiler quiet */
		}
	}

	ReleaseVariableStats(vardata);

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	return (Selectivity) selec;
}

/*
 * strip_array_coercion - strip binary-compatible relabeling from an array expr
 *
 * For array values, the parser normally generates ArrayCoerceExpr conversions,
 * but it seems possible that RelabelType might show up.  Also, the planner
 * is not currently tense about collapsing stacked ArrayCoerceExpr nodes,
 * so we need to be ready to deal with more than one level.
 */
static Node *
strip_array_coercion(Node *node)
{
	for (;;)
	{
		if (node && IsA(node, ArrayCoerceExpr))
		{
			ArrayCoerceExpr *acoerce = (ArrayCoerceExpr *) node;

			/*
			 * If the per-element expression is just a RelabelType on top of
			 * CaseTestExpr, then we know it's a binary-compatible relabeling.
			 */
			if (IsA(acoerce->elemexpr, RelabelType) &&
				IsA(((RelabelType *) acoerce->elemexpr)->arg, CaseTestExpr))
				node = (Node *) acoerce->arg;
			else
				break;
		}
		else if (node && IsA(node, RelabelType))
		{
			/* We don't really expect this case, but may as well cope */
			node = (Node *) ((RelabelType *) node)->arg;
		}
		else
			break;
	}
	return node;
}

/*
 *		scalararraysel		- Selectivity of ScalarArrayOpExpr Node.
 */
Selectivity
scalararraysel(PlannerInfo *root,
			   ScalarArrayOpExpr *clause,
			   bool is_join_clause,
			   int varRelid,
			   JoinType jointype,
			   SpecialJoinInfo *sjinfo)
{
	Oid			operator = clause->opno;
	bool		useOr = clause->useOr;
	bool		isEquality = false;
	bool		isInequality = false;
	Node	   *leftop;
	Node	   *rightop;
	Oid			nominal_element_type;
	Oid			nominal_element_collation;
	TypeCacheEntry *typentry;
	RegProcedure oprsel;
	FmgrInfo	oprselproc;
	Selectivity s1;
	Selectivity s1disjoint;

	/* First, deconstruct the expression */
	Assert(list_length(clause->args) == 2);
	leftop = (Node *) linitial(clause->args);
	rightop = (Node *) lsecond(clause->args);

	/* aggressively reduce both sides to constants */
	leftop = estimate_expression_value(root, leftop);
	rightop = estimate_expression_value(root, rightop);

	/* get nominal (after relabeling) element type of rightop */
	nominal_element_type = get_base_element_type(exprType(rightop));
	if (!OidIsValid(nominal_element_type))
		return (Selectivity) 0.5;	/* probably shouldn't happen */
	/* get nominal collation, too, for generating constants */
	nominal_element_collation = exprCollation(rightop);

	/* look through any binary-compatible relabeling of rightop */
	rightop = strip_array_coercion(rightop);

	/*
	 * Detect whether the operator is the default equality or inequality
	 * operator of the array element type.
	 */
	typentry = lookup_type_cache(nominal_element_type, TYPECACHE_EQ_OPR);
	if (OidIsValid(typentry->eq_opr))
	{
		if (operator == typentry->eq_opr)
			isEquality = true;
		else if (get_negator(operator) == typentry->eq_opr)
			isInequality = true;
	}

	/*
	 * If it is equality or inequality, we might be able to estimate this as a
	 * form of array containment; for instance "const = ANY(column)" can be
	 * treated as "ARRAY[const] <@ column".  scalararraysel_containment tries
	 * that, and returns the selectivity estimate if successful, or -1 if not.
	 */
	if ((isEquality || isInequality) && !is_join_clause)
	{
		s1 = scalararraysel_containment(root, leftop, rightop,
										nominal_element_type,
										isEquality, useOr, varRelid);
		if (s1 >= 0.0)
			return s1;
	}

	/*
	 * Look up the underlying operator's selectivity estimator. Punt if it
	 * hasn't got one.
	 */
	if (is_join_clause)
		oprsel = get_oprjoin(operator);
	else
		oprsel = get_oprrest(operator);
	if (!oprsel)
		return (Selectivity) 0.5;
	fmgr_info(oprsel, &oprselproc);

	/*
	 * In the array-containment check above, we must only believe that an
	 * operator is equality or inequality if it is the default btree equality
	 * operator (or its negator) for the element type, since those are the
	 * operators that array containment will use.  But in what follows, we can
	 * be a little laxer, and also believe that any operators using eqsel() or
	 * neqsel() as selectivity estimator act like equality or inequality.
	 */
	if (oprsel == F_EQSEL || oprsel == F_EQJOINSEL)
		isEquality = true;
	else if (oprsel == F_NEQSEL || oprsel == F_NEQJOINSEL)
		isInequality = true;

	/*
	 * We consider three cases:
	 *
	 * 1. rightop is an Array constant: deconstruct the array, apply the
	 * operator's selectivity function for each array element, and merge the
	 * results in the same way that clausesel.c does for AND/OR combinations.
	 *
	 * 2. rightop is an ARRAY[] construct: apply the operator's selectivity
	 * function for each element of the ARRAY[] construct, and merge.
	 *
	 * 3. otherwise, make a guess ...
	 */
	if (rightop && IsA(rightop, Const))
	{
		Datum		arraydatum = ((Const *) rightop)->constvalue;
		bool		arrayisnull = ((Const *) rightop)->constisnull;
		ArrayType  *arrayval;
		int16		elmlen;
		bool		elmbyval;
		char		elmalign;
		int			num_elems;
		Datum	   *elem_values;
		bool	   *elem_nulls;
		int			i;

		if (arrayisnull)		/* qual can't succeed if null array */
			return (Selectivity) 0.0;
		arrayval = DatumGetArrayTypeP(arraydatum);
		get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
							 &elmlen, &elmbyval, &elmalign);
		deconstruct_array(arrayval,
						  ARR_ELEMTYPE(arrayval),
						  elmlen, elmbyval, elmalign,
						  &elem_values, &elem_nulls, &num_elems);

		/*
		 * For generic operators, we assume the probability of success is
		 * independent for each array element.  But for "= ANY" or "<> ALL",
		 * if the array elements are distinct (which'd typically be the case)
		 * then the probabilities are disjoint, and we should just sum them.
		 *
		 * If we were being really tense we would try to confirm that the
		 * elements are all distinct, but that would be expensive and it
		 * doesn't seem to be worth the cycles; it would amount to penalizing
		 * well-written queries in favor of poorly-written ones.  However, we
		 * do protect ourselves a little bit by checking whether the
		 * disjointness assumption leads to an impossible (out of range)
		 * probability; if so, we fall back to the normal calculation.
		 */
		s1 = s1disjoint = (useOr ? 0.0 : 1.0);

		for (i = 0; i < num_elems; i++)
		{
			List	   *args;
			Selectivity s2;

			args = list_make2(leftop,
							  makeConst(nominal_element_type,
										-1,
										nominal_element_collation,
										elmlen,
										elem_values[i],
										elem_nulls[i],
										elmbyval));
			if (is_join_clause)
				s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int16GetDatum(jointype),
													  PointerGetDatum(sjinfo)));
			else
				s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int32GetDatum(varRelid)));

			if (useOr)
			{
				s1 = s1 + s2 - s1 * s2;
				if (isEquality)
					s1disjoint += s2;
			}
			else
			{
				s1 = s1 * s2;
				if (isInequality)
					s1disjoint += s2 - 1.0;
			}
		}

		/* accept disjoint-probability estimate if in range */
		if ((useOr ? isEquality : isInequality) &&
			s1disjoint >= 0.0 && s1disjoint <= 1.0)
			s1 = s1disjoint;
	}
	else if (rightop && IsA(rightop, ArrayExpr) &&
			 !((ArrayExpr *) rightop)->multidims)
	{
		ArrayExpr  *arrayexpr = (ArrayExpr *) rightop;
		int16		elmlen;
		bool		elmbyval;
		ListCell   *l;

		get_typlenbyval(arrayexpr->element_typeid,
						&elmlen, &elmbyval);

		/*
		 * We use the assumption of disjoint probabilities here too, although
		 * the odds of equal array elements are rather higher if the elements
		 * are not all constants (which they won't be, else constant folding
		 * would have reduced the ArrayExpr to a Const).  In this path it's
		 * critical to have the sanity check on the s1disjoint estimate.
		 */
		s1 = s1disjoint = (useOr ? 0.0 : 1.0);

		foreach(l, arrayexpr->elements)
		{
			Node	   *elem = (Node *) lfirst(l);
			List	   *args;
			Selectivity s2;

			/*
			 * Theoretically, if elem isn't of nominal_element_type we should
			 * insert a RelabelType, but it seems unlikely that any operator
			 * estimation function would really care ...
			 */
			args = list_make2(leftop, elem);
			if (is_join_clause)
				s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int16GetDatum(jointype),
													  PointerGetDatum(sjinfo)));
			else
				s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
													  clause->inputcollid,
													  PointerGetDatum(root),
													  ObjectIdGetDatum(operator),
													  PointerGetDatum(args),
													  Int32GetDatum(varRelid)));

			if (useOr)
			{
				s1 = s1 + s2 - s1 * s2;
				if (isEquality)
					s1disjoint += s2;
			}
			else
			{
				s1 = s1 * s2;
				if (isInequality)
					s1disjoint += s2 - 1.0;
			}
		}

		/* accept disjoint-probability estimate if in range */
		if ((useOr ? isEquality : isInequality) &&
			s1disjoint >= 0.0 && s1disjoint <= 1.0)
			s1 = s1disjoint;
	}
	else
	{
		CaseTestExpr *dummyexpr;
		List	   *args;
		Selectivity s2;
		int			i;

		/*
		 * We need a dummy rightop to pass to the operator selectivity
		 * routine.  It can be pretty much anything that doesn't look like a
		 * constant; CaseTestExpr is a convenient choice.
		 */
		dummyexpr = makeNode(CaseTestExpr);
		dummyexpr->typeId = nominal_element_type;
		dummyexpr->typeMod = -1;
		dummyexpr->collation = clause->inputcollid;
		args = list_make2(leftop, dummyexpr);
		if (is_join_clause)
			s2 = DatumGetFloat8(FunctionCall5Coll(&oprselproc,
												  clause->inputcollid,
												  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
												  PointerGetDatum(args),
												  Int16GetDatum(jointype),
												  PointerGetDatum(sjinfo)));
		else
			s2 = DatumGetFloat8(FunctionCall4Coll(&oprselproc,
												  clause->inputcollid,
												  PointerGetDatum(root),
												  ObjectIdGetDatum(operator),
												  PointerGetDatum(args),
												  Int32GetDatum(varRelid)));
		s1 = useOr ? 0.0 : 1.0;

		/*
		 * Arbitrarily assume 10 elements in the eventual array value (see
		 * also estimate_array_length).  We don't risk an assumption of
		 * disjoint probabilities here.
		 */
		for (i = 0; i < 10; i++)
		{
			if (useOr)
				s1 = s1 + s2 - s1 * s2;
			else
				s1 = s1 * s2;
		}
	}

	/* result should be in range, but make sure... */
	CLAMP_PROBABILITY(s1);

	return s1;
}

/*
 * Estimate number of elements in the array yielded by an expression.
 *
 * Note: the result is integral, but we use "double" to avoid overflow
 * concerns.  Most callers will use it in double-type expressions anyway.
 *
 * Note: in some code paths root can be passed as NULL, resulting in
 * slightly worse estimates.
 */
double
estimate_array_length(PlannerInfo *root, Node *arrayexpr)
{
	/* look through any binary-compatible relabeling of arrayexpr */
	arrayexpr = strip_array_coercion(arrayexpr);

	if (arrayexpr && IsA(arrayexpr, Const))
	{
		Datum		arraydatum = ((Const *) arrayexpr)->constvalue;
		bool		arrayisnull = ((Const *) arrayexpr)->constisnull;
		ArrayType  *arrayval;

		if (arrayisnull)
			return 0;
		arrayval = DatumGetArrayTypeP(arraydatum);
		return ArrayGetNItems(ARR_NDIM(arrayval), ARR_DIMS(arrayval));
	}
	else if (arrayexpr && IsA(arrayexpr, ArrayExpr) &&
			 !((ArrayExpr *) arrayexpr)->multidims)
	{
		return list_length(((ArrayExpr *) arrayexpr)->elements);
	}
	else if (arrayexpr && root)
	{
		/* See if we can find any statistics about it */
		VariableStatData vardata;
		AttStatsSlot sslot;
		double		nelem = 0;

		examine_variable(root, arrayexpr, 0, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple))
		{
			/*
			 * Found stats, so use the average element count, which is stored
			 * in the last stanumbers element of the DECHIST statistics.
			 * Actually that is the average count of *distinct* elements;
			 * perhaps we should scale it up somewhat?
			 */
			if (get_attstatsslot(&sslot, vardata.statsTuple,
								 STATISTIC_KIND_DECHIST, InvalidOid,
								 ATTSTATSSLOT_NUMBERS))
			{
				if (sslot.nnumbers > 0)
					nelem = clamp_row_est(sslot.numbers[sslot.nnumbers - 1]);
				free_attstatsslot(&sslot);
			}
		}
		ReleaseVariableStats(vardata);

		if (nelem > 0)
			return nelem;
	}

	/* Else use a default guess --- this should match scalararraysel */
	return 10;
}

/*
 *		rowcomparesel		- Selectivity of RowCompareExpr Node.
 *
 * We estimate RowCompare selectivity by considering just the first (high
 * order) columns, which makes it equivalent to an ordinary OpExpr.  While
 * this estimate could be refined by considering additional columns, it
 * seems unlikely that we could do a lot better without multi-column
 * statistics.
 */
Selectivity
rowcomparesel(PlannerInfo *root,
			  RowCompareExpr *clause,
			  int varRelid, JoinType jointype, SpecialJoinInfo *sjinfo)
{
	Selectivity s1;
	Oid			opno = linitial_oid(clause->opnos);
	Oid			inputcollid = linitial_oid(clause->inputcollids);
	List	   *opargs;
	bool		is_join_clause;

	/* Build equivalent arg list for single operator */
	opargs = list_make2(linitial(clause->largs), linitial(clause->rargs));

	/*
	 * Decide if it's a join clause.  This should match clausesel.c's
	 * treat_as_join_clause(), except that we intentionally consider only the
	 * leading columns and not the rest of the clause.
	 */
	if (varRelid != 0)
	{
		/*
		 * Caller is forcing restriction mode (eg, because we are examining an
		 * inner indexscan qual).
		 */
		is_join_clause = false;
	}
	else if (sjinfo == NULL)
	{
		/*
		 * It must be a restriction clause, since it's being evaluated at a
		 * scan node.
		 */
		is_join_clause = false;
	}
	else
	{
		/*
		 * Otherwise, it's a join if there's more than one base relation used.
		 */
		is_join_clause = (NumRelids(root, (Node *) opargs) > 1);
	}

	if (is_join_clause)
	{
		/* Estimate selectivity for a join clause. */
		s1 = join_selectivity(root, opno,
							  opargs,
							  inputcollid,
							  jointype,
							  sjinfo);
	}
	else
	{
		/* Estimate selectivity for a restriction clause. */
		s1 = restriction_selectivity(root, opno,
									 opargs,
									 inputcollid,
									 varRelid);
	}

	return s1;
}

/*
 *		eqjoinsel		- Join selectivity of "="
 */
Datum
eqjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);

#ifdef NOT_USED
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
#endif
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	Oid			collation = PG_GET_COLLATION();
	double		selec;
	double		selec_inner;
	VariableStatData vardata1;
	VariableStatData vardata2;
	double		nd1;
	double		nd2;
	bool		isdefault1;
	bool		isdefault2;
	Oid			opfuncoid;
	AttStatsSlot sslot1;
	AttStatsSlot sslot2;
	Form_pg_statistic stats1 = NULL;
	Form_pg_statistic stats2 = NULL;
	bool		have_mcvs1 = false;
	bool		have_mcvs2 = false;
	bool		get_mcv_stats;
	bool		join_is_reversed;
	RelOptInfo *inner_rel;

	get_join_variables(root, args, sjinfo,
					   &vardata1, &vardata2, &join_is_reversed);

	nd1 = get_variable_numdistinct(&vardata1, &isdefault1);
	nd2 = get_variable_numdistinct(&vardata2, &isdefault2);

	opfuncoid = get_opcode(operator);

	memset(&sslot1, 0, sizeof(sslot1));
	memset(&sslot2, 0, sizeof(sslot2));

	/*
	 * There is no use in fetching one side's MCVs if we lack MCVs for the
	 * other side, so do a quick check to verify that both stats exist.
	 */
	get_mcv_stats = (HeapTupleIsValid(vardata1.statsTuple) &&
					 HeapTupleIsValid(vardata2.statsTuple) &&
					 get_attstatsslot(&sslot1, vardata1.statsTuple,
									  STATISTIC_KIND_MCV, InvalidOid,
									  0) &&
					 get_attstatsslot(&sslot2, vardata2.statsTuple,
									  STATISTIC_KIND_MCV, InvalidOid,
									  0));

	if (HeapTupleIsValid(vardata1.statsTuple))
	{
		/* note we allow use of nullfrac regardless of security check */
		stats1 = (Form_pg_statistic) GETSTRUCT(vardata1.statsTuple);
		if (get_mcv_stats &&
			statistic_proc_security_check(&vardata1, opfuncoid))
			have_mcvs1 = get_attstatsslot(&sslot1, vardata1.statsTuple,
										  STATISTIC_KIND_MCV, InvalidOid,
										  ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
	}

	if (HeapTupleIsValid(vardata2.statsTuple))
	{
		/* note we allow use of nullfrac regardless of security check */
		stats2 = (Form_pg_statistic) GETSTRUCT(vardata2.statsTuple);
		if (get_mcv_stats &&
			statistic_proc_security_check(&vardata2, opfuncoid))
			have_mcvs2 = get_attstatsslot(&sslot2, vardata2.statsTuple,
										  STATISTIC_KIND_MCV, InvalidOid,
										  ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS);
	}

	/* We need to compute the inner-join selectivity in all cases */
	selec_inner = eqjoinsel_inner(opfuncoid, collation,
								  &vardata1, &vardata2,
								  nd1, nd2,
								  isdefault1, isdefault2,
								  &sslot1, &sslot2,
								  stats1, stats2,
								  have_mcvs1, have_mcvs2);

	switch (sjinfo->jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
		case JOIN_FULL:
			selec = selec_inner;
			break;
		case JOIN_SEMI:
		case JOIN_ANTI:

			/*
			 * Look up the join's inner relation.  min_righthand is sufficient
			 * information because neither SEMI nor ANTI joins permit any
			 * reassociation into or out of their RHS, so the righthand will
			 * always be exactly that set of rels.
			 */
			inner_rel = find_join_input_rel(root, sjinfo->min_righthand);

			if (!join_is_reversed)
				selec = eqjoinsel_semi(opfuncoid, collation,
									   &vardata1, &vardata2,
									   nd1, nd2,
									   isdefault1, isdefault2,
									   &sslot1, &sslot2,
									   stats1, stats2,
									   have_mcvs1, have_mcvs2,
									   inner_rel);
			else
			{
				Oid			commop = get_commutator(operator);
				Oid			commopfuncoid = OidIsValid(commop) ? get_opcode(commop) : InvalidOid;

				selec = eqjoinsel_semi(commopfuncoid, collation,
									   &vardata2, &vardata1,
									   nd2, nd1,
									   isdefault2, isdefault1,
									   &sslot2, &sslot1,
									   stats2, stats1,
									   have_mcvs2, have_mcvs1,
									   inner_rel);
			}

			/*
			 * We should never estimate the output of a semijoin to be more
			 * rows than we estimate for an inner join with the same input
			 * rels and join condition; it's obviously impossible for that to
			 * happen.  The former estimate is N1 * Ssemi while the latter is
			 * N1 * N2 * Sinner, so we may clamp Ssemi <= N2 * Sinner.  Doing
			 * this is worthwhile because of the shakier estimation rules we
			 * use in eqjoinsel_semi, particularly in cases where it has to
			 * punt entirely.
			 */
			selec = Min(selec, inner_rel->rows * selec_inner);
			break;
		default:
			/* other values not expected here */
			elog(ERROR, "unrecognized join type: %d",
				 (int) sjinfo->jointype);
			selec = 0;			/* keep compiler quiet */
			break;
	}

	free_attstatsslot(&sslot1);
	free_attstatsslot(&sslot2);

	ReleaseVariableStats(vardata1);
	ReleaseVariableStats(vardata2);

	CLAMP_PROBABILITY(selec);

	PG_RETURN_FLOAT8((float8) selec);
}

/*
 * eqjoinsel_inner --- eqjoinsel for normal inner join
 *
 * We also use this for LEFT/FULL outer joins; it's not presently clear
 * that it's worth trying to distinguish them here.
 */
static double
eqjoinsel_inner(Oid opfuncoid, Oid collation,
				VariableStatData *vardata1, VariableStatData *vardata2,
				double nd1, double nd2,
				bool isdefault1, bool isdefault2,
				AttStatsSlot *sslot1, AttStatsSlot *sslot2,
				Form_pg_statistic stats1, Form_pg_statistic stats2,
				bool have_mcvs1, bool have_mcvs2)
{
	double		selec;

	if (have_mcvs1 && have_mcvs2)
	{
		/*
		 * We have most-common-value lists for both relations.  Run through
		 * the lists to see which MCVs actually join to each other with the
		 * given operator.  This allows us to determine the exact join
		 * selectivity for the portion of the relations represented by the MCV
		 * lists.  We still have to estimate for the remaining population, but
		 * in a skewed distribution this gives us a big leg up in accuracy.
		 * For motivation see the analysis in Y. Ioannidis and S.
		 * Christodoulakis, "On the propagation of errors in the size of join
		 * results", Technical Report 1018, Computer Science Dept., University
		 * of Wisconsin, Madison, March 1991 (available from ftp.cs.wisc.edu).
		 */
		LOCAL_FCINFO(fcinfo, 2);
		FmgrInfo	eqproc;
		bool	   *hasmatch1;
		bool	   *hasmatch2;
		double		nullfrac1 = stats1->stanullfrac;
		double		nullfrac2 = stats2->stanullfrac;
		double		matchprodfreq,
					matchfreq1,
					matchfreq2,
					unmatchfreq1,
					unmatchfreq2,
					otherfreq1,
					otherfreq2,
					totalsel1,
					totalsel2;
		int			i,
					nmatches;

		fmgr_info(opfuncoid, &eqproc);

		/*
		 * Save a few cycles by setting up the fcinfo struct just once. Using
		 * FunctionCallInvoke directly also avoids failure if the eqproc
		 * returns NULL, though really equality functions should never do
		 * that.
		 */
		InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;

		hasmatch1 = (bool *) palloc0(sslot1->nvalues * sizeof(bool));
		hasmatch2 = (bool *) palloc0(sslot2->nvalues * sizeof(bool));

		/*
		 * Note we assume that each MCV will match at most one member of the
		 * other MCV list.  If the operator isn't really equality, there could
		 * be multiple matches --- but we don't look for them, both for speed
		 * and because the math wouldn't add up...
		 */
		matchprodfreq = 0.0;
		nmatches = 0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			int			j;

			fcinfo->args[0].value = sslot1->values[i];

			for (j = 0; j < sslot2->nvalues; j++)
			{
				Datum		fresult;

				if (hasmatch2[j])
					continue;
				fcinfo->args[1].value = sslot2->values[j];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					hasmatch1[i] = hasmatch2[j] = true;
					matchprodfreq += sslot1->numbers[i] * sslot2->numbers[j];
					nmatches++;
					break;
				}
			}
		}
		CLAMP_PROBABILITY(matchprodfreq);
		/* Sum up frequencies of matched and unmatched MCVs */
		matchfreq1 = unmatchfreq1 = 0.0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			if (hasmatch1[i])
				matchfreq1 += sslot1->numbers[i];
			else
				unmatchfreq1 += sslot1->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq1);
		CLAMP_PROBABILITY(unmatchfreq1);
		matchfreq2 = unmatchfreq2 = 0.0;
		for (i = 0; i < sslot2->nvalues; i++)
		{
			if (hasmatch2[i])
				matchfreq2 += sslot2->numbers[i];
			else
				unmatchfreq2 += sslot2->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq2);
		CLAMP_PROBABILITY(unmatchfreq2);
		pfree(hasmatch1);
		pfree(hasmatch2);

		/*
		 * Compute total frequency of non-null values that are not in the MCV
		 * lists.
		 */
		otherfreq1 = 1.0 - nullfrac1 - matchfreq1 - unmatchfreq1;
		otherfreq2 = 1.0 - nullfrac2 - matchfreq2 - unmatchfreq2;
		CLAMP_PROBABILITY(otherfreq1);
		CLAMP_PROBABILITY(otherfreq2);

		/*
		 * We can estimate the total selectivity from the point of view of
		 * relation 1 as: the known selectivity for matched MCVs, plus
		 * unmatched MCVs that are assumed to match against random members of
		 * relation 2's non-MCV population, plus non-MCV values that are
		 * assumed to match against random members of relation 2's unmatched
		 * MCVs plus non-MCV values.
		 */
		totalsel1 = matchprodfreq;
		if (nd2 > sslot2->nvalues)
			totalsel1 += unmatchfreq1 * otherfreq2 / (nd2 - sslot2->nvalues);
		if (nd2 > nmatches)
			totalsel1 += otherfreq1 * (otherfreq2 + unmatchfreq2) /
				(nd2 - nmatches);
		/* Same estimate from the point of view of relation 2. */
		totalsel2 = matchprodfreq;
		if (nd1 > sslot1->nvalues)
			totalsel2 += unmatchfreq2 * otherfreq1 / (nd1 - sslot1->nvalues);
		if (nd1 > nmatches)
			totalsel2 += otherfreq2 * (otherfreq1 + unmatchfreq1) /
				(nd1 - nmatches);

		/*
		 * Use the smaller of the two estimates.  This can be justified in
		 * essentially the same terms as given below for the no-stats case: to
		 * a first approximation, we are estimating from the point of view of
		 * the relation with smaller nd.
		 */
		selec = (totalsel1 < totalsel2) ? totalsel1 : totalsel2;
	}
	else
	{
		/*
		 * We do not have MCV lists for both sides.  Estimate the join
		 * selectivity as MIN(1/nd1,1/nd2)*(1-nullfrac1)*(1-nullfrac2). This
		 * is plausible if we assume that the join operator is strict and the
		 * non-null values are about equally distributed: a given non-null
		 * tuple of rel1 will join to either zero or N2*(1-nullfrac2)/nd2 rows
		 * of rel2, so total join rows are at most
		 * N1*(1-nullfrac1)*N2*(1-nullfrac2)/nd2 giving a join selectivity of
		 * not more than (1-nullfrac1)*(1-nullfrac2)/nd2. By the same logic it
		 * is not more than (1-nullfrac1)*(1-nullfrac2)/nd1, so the expression
		 * with MIN() is an upper bound.  Using the MIN() means we estimate
		 * from the point of view of the relation with smaller nd (since the
		 * larger nd is determining the MIN).  It is reasonable to assume that
		 * most tuples in this rel will have join partners, so the bound is
		 * probably reasonably tight and should be taken as-is.
		 *
		 * XXX Can we be smarter if we have an MCV list for just one side? It
		 * seems that if we assume equal distribution for the other side, we
		 * end up with the same answer anyway.
		 */
		double		nullfrac1 = stats1 ? stats1->stanullfrac : 0.0;
		double		nullfrac2 = stats2 ? stats2->stanullfrac : 0.0;

		selec = (1.0 - nullfrac1) * (1.0 - nullfrac2);
		if (nd1 > nd2)
			selec /= nd1;
		else
			selec /= nd2;
	}

	return selec;
}

/*
 * eqjoinsel_semi --- eqjoinsel for semi join
 *
 * (Also used for anti join, which we are supposed to estimate the same way.)
 * Caller has ensured that vardata1 is the LHS variable.
 * Unlike eqjoinsel_inner, we have to cope with opfuncoid being InvalidOid.
 */
static double
eqjoinsel_semi(Oid opfuncoid, Oid collation,
			   VariableStatData *vardata1, VariableStatData *vardata2,
			   double nd1, double nd2,
			   bool isdefault1, bool isdefault2,
			   AttStatsSlot *sslot1, AttStatsSlot *sslot2,
			   Form_pg_statistic stats1, Form_pg_statistic stats2,
			   bool have_mcvs1, bool have_mcvs2,
			   RelOptInfo *inner_rel)
{
	double		selec;

	/*
	 * We clamp nd2 to be not more than what we estimate the inner relation's
	 * size to be.  This is intuitively somewhat reasonable since obviously
	 * there can't be more than that many distinct values coming from the
	 * inner rel.  The reason for the asymmetry (ie, that we don't clamp nd1
	 * likewise) is that this is the only pathway by which restriction clauses
	 * applied to the inner rel will affect the join result size estimate,
	 * since set_joinrel_size_estimates will multiply SEMI/ANTI selectivity by
	 * only the outer rel's size.  If we clamped nd1 we'd be double-counting
	 * the selectivity of outer-rel restrictions.
	 *
	 * We can apply this clamping both with respect to the base relation from
	 * which the join variable comes (if there is just one), and to the
	 * immediate inner input relation of the current join.
	 *
	 * If we clamp, we can treat nd2 as being a non-default estimate; it's not
	 * great, maybe, but it didn't come out of nowhere either.  This is most
	 * helpful when the inner relation is empty and consequently has no stats.
	 */
	if (vardata2->rel)
	{
		if (nd2 >= vardata2->rel->rows)
		{
			nd2 = vardata2->rel->rows;
			isdefault2 = false;
		}
	}
	if (nd2 >= inner_rel->rows)
	{
		nd2 = inner_rel->rows;
		isdefault2 = false;
	}

	if (have_mcvs1 && have_mcvs2 && OidIsValid(opfuncoid))
	{
		/*
		 * We have most-common-value lists for both relations.  Run through
		 * the lists to see which MCVs actually join to each other with the
		 * given operator.  This allows us to determine the exact join
		 * selectivity for the portion of the relations represented by the MCV
		 * lists.  We still have to estimate for the remaining population, but
		 * in a skewed distribution this gives us a big leg up in accuracy.
		 */
		LOCAL_FCINFO(fcinfo, 2);
		FmgrInfo	eqproc;
		bool	   *hasmatch1;
		bool	   *hasmatch2;
		double		nullfrac1 = stats1->stanullfrac;
		double		matchfreq1,
					uncertainfrac,
					uncertain;
		int			i,
					nmatches,
					clamped_nvalues2;

		/*
		 * The clamping above could have resulted in nd2 being less than
		 * sslot2->nvalues; in which case, we assume that precisely the nd2
		 * most common values in the relation will appear in the join input,
		 * and so compare to only the first nd2 members of the MCV list.  Of
		 * course this is frequently wrong, but it's the best bet we can make.
		 */
		clamped_nvalues2 = Min(sslot2->nvalues, nd2);

		fmgr_info(opfuncoid, &eqproc);

		/*
		 * Save a few cycles by setting up the fcinfo struct just once. Using
		 * FunctionCallInvoke directly also avoids failure if the eqproc
		 * returns NULL, though really equality functions should never do
		 * that.
		 */
		InitFunctionCallInfoData(*fcinfo, &eqproc, 2, collation,
								 NULL, NULL);
		fcinfo->args[0].isnull = false;
		fcinfo->args[1].isnull = false;

		hasmatch1 = (bool *) palloc0(sslot1->nvalues * sizeof(bool));
		hasmatch2 = (bool *) palloc0(clamped_nvalues2 * sizeof(bool));

		/*
		 * Note we assume that each MCV will match at most one member of the
		 * other MCV list.  If the operator isn't really equality, there could
		 * be multiple matches --- but we don't look for them, both for speed
		 * and because the math wouldn't add up...
		 */
		nmatches = 0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			int			j;

			fcinfo->args[0].value = sslot1->values[i];

			for (j = 0; j < clamped_nvalues2; j++)
			{
				Datum		fresult;

				if (hasmatch2[j])
					continue;
				fcinfo->args[1].value = sslot2->values[j];
				fcinfo->isnull = false;
				fresult = FunctionCallInvoke(fcinfo);
				if (!fcinfo->isnull && DatumGetBool(fresult))
				{
					hasmatch1[i] = hasmatch2[j] = true;
					nmatches++;
					break;
				}
			}
		}
		/* Sum up frequencies of matched MCVs */
		matchfreq1 = 0.0;
		for (i = 0; i < sslot1->nvalues; i++)
		{
			if (hasmatch1[i])
				matchfreq1 += sslot1->numbers[i];
		}
		CLAMP_PROBABILITY(matchfreq1);
		pfree(hasmatch1);
		pfree(hasmatch2);

		/*
		 * Now we need to estimate the fraction of relation 1 that has at
		 * least one join partner.  We know for certain that the matched MCVs
		 * do, so that gives us a lower bound, but we're really in the dark
		 * about everything else.  Our crude approach is: if nd1 <= nd2 then
		 * assume all non-null rel1 rows have join partners, else assume for
		 * the uncertain rows that a fraction nd2/nd1 have join partners. We
		 * can discount the known-matched MCVs from the distinct-values counts
		 * before doing the division.
		 *
		 * Crude as the above is, it's completely useless if we don't have
		 * reliable ndistinct values for both sides.  Hence, if either nd1 or
		 * nd2 is default, punt and assume half of the uncertain rows have
		 * join partners.
		 */
		if (!isdefault1 && !isdefault2)
		{
			nd1 -= nmatches;
			nd2 -= nmatches;
			if (nd1 <= nd2 || nd2 < 0)
				uncertainfrac = 1.0;
			else
				uncertainfrac = nd2 / nd1;
		}
		else
			uncertainfrac = 0.5;
		uncertain = 1.0 - matchfreq1 - nullfrac1;
		CLAMP_PROBABILITY(uncertain);
		selec = matchfreq1 + uncertainfrac * uncertain;
	}
	else
	{
		/*
		 * Without MCV lists for both sides, we can only use the heuristic
		 * about nd1 vs nd2.
		 */
		double		nullfrac1 = stats1 ? stats1->stanullfrac : 0.0;

		if (!isdefault1 && !isdefault2)
		{
			if (nd1 <= nd2 || nd2 < 0)
				selec = 1.0 - nullfrac1;
			else
				selec = (nd2 / nd1) * (1.0 - nullfrac1);
		}
		else
			selec = 0.5 * (1.0 - nullfrac1);
	}

	return selec;
}

/*
 *		neqjoinsel		- Join selectivity of "!="
 */
Datum
neqjoinsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	JoinType	jointype = (JoinType) PG_GETARG_INT16(3);
	SpecialJoinInfo *sjinfo = (SpecialJoinInfo *) PG_GETARG_POINTER(4);
	Oid			collation = PG_GET_COLLATION();
	float8		result;

	if (jointype == JOIN_SEMI || jointype == JOIN_ANTI)
	{
		/*
		 * For semi-joins, if there is more than one distinct value in the RHS
		 * relation then every non-null LHS row must find a row to join since
		 * it can only be equal to one of them.  We'll assume that there is
		 * always more than one distinct RHS value for the sake of stability,
		 * though in theory we could have special cases for empty RHS
		 * (selectivity = 0) and single-distinct-value RHS (selectivity =
		 * fraction of LHS that has the same value as the single RHS value).
		 *
		 * For anti-joins, if we use the same assumption that there is more
		 * than one distinct key in the RHS relation, then every non-null LHS
		 * row must be suppressed by the anti-join.
		 *
		 * So either way, the selectivity estimate should be 1 - nullfrac.
		 */
		VariableStatData leftvar;
		VariableStatData rightvar;
		bool		reversed;
		HeapTuple	statsTuple;
		double		nullfrac;

		get_join_variables(root, args, sjinfo, &leftvar, &rightvar, &reversed);
		statsTuple = reversed ? rightvar.statsTuple : leftvar.statsTuple;
		if (HeapTupleIsValid(statsTuple))
			nullfrac = ((Form_pg_statistic) GETSTRUCT(statsTuple))->stanullfrac;
		else
			nullfrac = 0.0;
		ReleaseVariableStats(leftvar);
		ReleaseVariableStats(rightvar);

		result = 1.0 - nullfrac;
	}
	else
	{
		/*
		 * We want 1 - eqjoinsel() where the equality operator is the one
		 * associated with this != operator, that is, its negator.
		 */
		Oid			eqop = get_negator(operator);

		if (eqop)
		{
			result =
				DatumGetFloat8(DirectFunctionCall5Coll(eqjoinsel,
													   collation,
													   PointerGetDatum(root),
													   ObjectIdGetDatum(eqop),
													   PointerGetDatum(args),
													   Int16GetDatum(jointype),
													   PointerGetDatum(sjinfo)));
		}
		else
		{
			/* Use default selectivity (should we raise an error instead?) */
			result = DEFAULT_EQ_SEL;
		}
		result = 1.0 - result;
	}

	PG_RETURN_FLOAT8(result);
}

/*
 *		scalarltjoinsel - Join selectivity of "<" for scalars
 */
Datum
scalarltjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalarlejoinsel - Join selectivity of "<=" for scalars
 */
Datum
scalarlejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalargtjoinsel - Join selectivity of ">" for scalars
 */
Datum
scalargtjoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}

/*
 *		scalargejoinsel - Join selectivity of ">=" for scalars
 */
Datum
scalargejoinsel(PG_FUNCTION_ARGS)
{
	PG_RETURN_FLOAT8(DEFAULT_INEQ_SEL);
}


/*
 * mergejoinscansel			- Scan selectivity of merge join.
 *
 * A merge join will stop as soon as it exhausts either input stream.
 * Therefore, if we can estimate the ranges of both input variables,
 * we can estimate how much of the input will actually be read.  This
 * can have a considerable impact on the cost when using indexscans.
 *
 * Also, we can estimate how much of each input has to be read before the
 * first join pair is found, which will affect the join's startup time.
 *
 * clause should be a clause already known to be mergejoinable.  opfamily,
 * cmptype, and nulls_first specify the sort ordering being used.
 *
 * The outputs are:
 *		*leftstart is set to the fraction of the left-hand variable expected
 *		 to be scanned before the first join pair is found (0 to 1).
 *		*leftend is set to the fraction of the left-hand variable expected
 *		 to be scanned before the join terminates (0 to 1).
 *		*rightstart, *rightend similarly for the right-hand variable.
 */
void
mergejoinscansel(PlannerInfo *root, Node *clause,
				 Oid opfamily, CompareType cmptype, bool nulls_first,
				 Selectivity *leftstart, Selectivity *leftend,
				 Selectivity *rightstart, Selectivity *rightend)
{
	Node	   *left,
			   *right;
	VariableStatData leftvar,
				rightvar;
	Oid			opmethod;
	int			op_strategy;
	Oid			op_lefttype;
	Oid			op_righttype;
	Oid			opno,
				collation,
				lsortop,
				rsortop,
				lstatop,
				rstatop,
				ltop,
				leop,
				revltop,
				revleop;
	StrategyNumber ltstrat,
				lestrat,
				gtstrat,
				gestrat;
	bool		isgt;
	Datum		leftmin,
				leftmax,
				rightmin,
				rightmax;
	double		selec;

	/* Set default results if we can't figure anything out. */
	/* XXX should default "start" fraction be a bit more than 0? */
	*leftstart = *rightstart = 0.0;
	*leftend = *rightend = 1.0;

	/* Deconstruct the merge clause */
	if (!is_opclause(clause))
		return;					/* shouldn't happen */
	opno = ((OpExpr *) clause)->opno;
	collation = ((OpExpr *) clause)->inputcollid;
	left = get_leftop((Expr *) clause);
	right = get_rightop((Expr *) clause);
	if (!right)
		return;					/* shouldn't happen */

	/* Look for stats for the inputs */
	examine_variable(root, left, 0, &leftvar);
	examine_variable(root, right, 0, &rightvar);

	opmethod = get_opfamily_method(opfamily);

	/* Extract the operator's declared left/right datatypes */
	get_op_opfamily_properties(opno, opfamily, false,
							   &op_strategy,
							   &op_lefttype,
							   &op_righttype);
	Assert(IndexAmTranslateStrategy(op_strategy, opmethod, opfamily, true) == COMPARE_EQ);

	/*
	 * Look up the various operators we need.  If we don't find them all, it
	 * probably means the opfamily is broken, but we just fail silently.
	 *
	 * Note: we expect that pg_statistic histograms will be sorted by the '<'
	 * operator, regardless of which sort direction we are considering.
	 */
	switch (cmptype)
	{
		case COMPARE_LT:
			isgt = false;
			ltstrat = IndexAmTranslateCompareType(COMPARE_LT, opmethod, opfamily, true);
			lestrat = IndexAmTranslateCompareType(COMPARE_LE, opmethod, opfamily, true);
			if (op_lefttype == op_righttype)
			{
				/* easy case */
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   ltstrat);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   lestrat);
				lsortop = ltop;
				rsortop = ltop;
				lstatop = lsortop;
				rstatop = rsortop;
				revltop = ltop;
				revleop = leop;
			}
			else
			{
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   ltstrat);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   lestrat);
				lsortop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  ltstrat);
				rsortop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  ltstrat);
				lstatop = lsortop;
				rstatop = rsortop;
				revltop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  ltstrat);
				revleop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  lestrat);
			}
			break;
		case COMPARE_GT:
			/* descending-order case */
			isgt = true;
			ltstrat = IndexAmTranslateCompareType(COMPARE_LT, opmethod, opfamily, true);
			gtstrat = IndexAmTranslateCompareType(COMPARE_GT, opmethod, opfamily, true);
			gestrat = IndexAmTranslateCompareType(COMPARE_GE, opmethod, opfamily, true);
			if (op_lefttype == op_righttype)
			{
				/* easy case */
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   gtstrat);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   gestrat);
				lsortop = ltop;
				rsortop = ltop;
				lstatop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  ltstrat);
				rstatop = lstatop;
				revltop = ltop;
				revleop = leop;
			}
			else
			{
				ltop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   gtstrat);
				leop = get_opfamily_member(opfamily,
										   op_lefttype, op_righttype,
										   gestrat);
				lsortop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  gtstrat);
				rsortop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  gtstrat);
				lstatop = get_opfamily_member(opfamily,
											  op_lefttype, op_lefttype,
											  ltstrat);
				rstatop = get_opfamily_member(opfamily,
											  op_righttype, op_righttype,
											  ltstrat);
				revltop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  gtstrat);
				revleop = get_opfamily_member(opfamily,
											  op_righttype, op_lefttype,
											  gestrat);
			}
			break;
		default:
			goto fail;			/* shouldn't get here */
	}

	if (!OidIsValid(lsortop) ||
		!OidIsValid(rsortop) ||
		!OidIsValid(lstatop) ||
		!OidIsValid(rstatop) ||
		!OidIsValid(ltop) ||
		!OidIsValid(leop) ||
		!OidIsValid(revltop) ||
		!OidIsValid(revleop))
		goto fail;				/* insufficient info in catalogs */

	/* Try to get ranges of both inputs */
	if (!isgt)
	{
		if (!get_variable_range(root, &leftvar, lstatop, collation,
								&leftmin, &leftmax))
			goto fail;			/* no range available from stats */
		if (!get_variable_range(root, &rightvar, rstatop, collation,
								&rightmin, &rightmax))
			goto fail;			/* no range available from stats */
	}
	else
	{
		/* need to swap the max and min */
		if (!get_variable_range(root, &leftvar, lstatop, collation,
								&leftmax, &leftmin))
			goto fail;			/* no range available from stats */
		if (!get_variable_range(root, &rightvar, rstatop, collation,
								&rightmax, &rightmin))
			goto fail;			/* no range available from stats */
	}

	/*
	 * Now, the fraction of the left variable that will be scanned is the
	 * fraction that's <= the right-side maximum value.  But only believe
	 * non-default estimates, else stick with our 1.0.
	 */
	selec = scalarineqsel(root, leop, isgt, true, collation, &leftvar,
						  rightmax, op_righttype);
	if (selec != DEFAULT_INEQ_SEL)
		*leftend = selec;

	/* And similarly for the right variable. */
	selec = scalarineqsel(root, revleop, isgt, true, collation, &rightvar,
						  leftmax, op_lefttype);
	if (selec != DEFAULT_INEQ_SEL)
		*rightend = selec;

	/*
	 * Only one of the two "end" fractions can really be less than 1.0;
	 * believe the smaller estimate and reset the other one to exactly 1.0. If
	 * we get exactly equal estimates (as can easily happen with self-joins),
	 * believe neither.
	 */
	if (*leftend > *rightend)
		*leftend = 1.0;
	else if (*leftend < *rightend)
		*rightend = 1.0;
	else
		*leftend = *rightend = 1.0;

	/*
	 * Also, the fraction of the left variable that will be scanned before the
	 * first join pair is found is the fraction that's < the right-side
	 * minimum value.  But only believe non-default estimates, else stick with
	 * our own default.
	 */
	selec = scalarineqsel(root, ltop, isgt, false, collation, &leftvar,
						  rightmin, op_righttype);
	if (selec != DEFAULT_INEQ_SEL)
		*leftstart = selec;

	/* And similarly for the right variable. */
	selec = scalarineqsel(root, revltop, isgt, false, collation, &rightvar,
						  leftmin, op_lefttype);
	if (selec != DEFAULT_INEQ_SEL)
		*rightstart = selec;

	/*
	 * Only one of the two "start" fractions can really be more than zero;
	 * believe the larger estimate and reset the other one to exactly 0.0. If
	 * we get exactly equal estimates (as can easily happen with self-joins),
	 * believe neither.
	 */
	if (*leftstart < *rightstart)
		*leftstart = 0.0;
	else if (*leftstart > *rightstart)
		*rightstart = 0.0;
	else
		*leftstart = *rightstart = 0.0;

	/*
	 * If the sort order is nulls-first, we're going to have to skip over any
	 * nulls too.  These would not have been counted by scalarineqsel, and we
	 * can safely add in this fraction regardless of whether we believe
	 * scalarineqsel's results or not.  But be sure to clamp the sum to 1.0!
	 */
	if (nulls_first)
	{
		Form_pg_statistic stats;

		if (HeapTupleIsValid(leftvar.statsTuple))
		{
			stats = (Form_pg_statistic) GETSTRUCT(leftvar.statsTuple);
			*leftstart += stats->stanullfrac;
			CLAMP_PROBABILITY(*leftstart);
			*leftend += stats->stanullfrac;
			CLAMP_PROBABILITY(*leftend);
		}
		if (HeapTupleIsValid(rightvar.statsTuple))
		{
			stats = (Form_pg_statistic) GETSTRUCT(rightvar.statsTuple);
			*rightstart += stats->stanullfrac;
			CLAMP_PROBABILITY(*rightstart);
			*rightend += stats->stanullfrac;
			CLAMP_PROBABILITY(*rightend);
		}
	}

	/* Disbelieve start >= end, just in case that can happen */
	if (*leftstart >= *leftend)
	{
		*leftstart = 0.0;
		*leftend = 1.0;
	}
	if (*rightstart >= *rightend)
	{
		*rightstart = 0.0;
		*rightend = 1.0;
	}

fail:
	ReleaseVariableStats(leftvar);
	ReleaseVariableStats(rightvar);
}


/*
 *	matchingsel -- generic matching-operator selectivity support
 *
 * Use these for any operators that (a) are on data types for which we collect
 * standard statistics, and (b) have behavior for which the default estimate
 * (twice DEFAULT_EQ_SEL) is sane.  Typically that is good for match-like
 * operators.
 */

Datum
matchingsel(PG_FUNCTION_ARGS)
{
	PlannerInfo *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid			operator = PG_GETARG_OID(1);
	List	   *args = (List *) PG_GETARG_POINTER(2);
	int			varRelid = PG_GETARG_INT32(3);
	Oid			collation = PG_GET_COLLATION();
	double		selec;

	/* Use generic restriction selectivity logic. */
	selec = generic_restriction_selectivity(root, operator, collation,
											args, varRelid,
											DEFAULT_MATCHING_SEL);

	PG_RETURN_FLOAT8((float8) selec);
}

Datum
matchingjoinsel(PG_FUNCTION_ARGS)
{
	/* Just punt, for the moment. */
	PG_RETURN_FLOAT8(DEFAULT_MATCHING_SEL);
}


/*
 * Helper routine for estimate_num_groups: add an item to a list of
 * GroupVarInfos, but only if it's not known equal to any of the existing
 * entries.
 */
typedef struct
{
	Node	   *var;			/* might be an expression, not just a Var */
	RelOptInfo *rel;			/* relation it belongs to */
	double		ndistinct;		/* # distinct values */
	bool		isdefault;		/* true if DEFAULT_NUM_DISTINCT was used */
} GroupVarInfo;

static List *
add_unique_group_var(PlannerInfo *root, List *varinfos,
					 Node *var, VariableStatData *vardata)
{
	GroupVarInfo *varinfo;
	double		ndistinct;
	bool		isdefault;
	ListCell   *lc;

	ndistinct = get_variable_numdistinct(vardata, &isdefault);

	/*
	 * The nullingrels bits within the var could cause the same var to be
	 * counted multiple times if it's marked with different nullingrels.  They
	 * could also prevent us from matching the var to the expressions in
	 * extended statistics (see estimate_multivariate_ndistinct).  So strip
	 * them out first.
	 */
	var = remove_nulling_relids(var, root->outer_join_rels, NULL);

	foreach(lc, varinfos)
	{
		varinfo = (GroupVarInfo *) lfirst(lc);

		/* Drop exact duplicates */
		if (equal(var, varinfo->var))
			return varinfos;

		/*
		 * Drop known-equal vars, but only if they belong to different
		 * relations (see comments for estimate_num_groups).  We aren't too
		 * fussy about the semantics of "equal" here.
		 */
		if (vardata->rel != varinfo->rel &&
			exprs_known_equal(root, var, varinfo->var, InvalidOid))
		{
			if (varinfo->ndistinct <= ndistinct)
			{
				/* Keep older item, forget new one */
				return varinfos;
			}
			else
			{
				/* Delete the older item */
				varinfos = foreach_delete_current(varinfos, lc);
			}
		}
	}

	varinfo = (GroupVarInfo *) palloc(sizeof(GroupVarInfo));

	varinfo->var = var;
	varinfo->rel = vardata->rel;
	varinfo->ndistinct = ndistinct;
	varinfo->isdefault = isdefault;
	varinfos = lappend(varinfos, varinfo);
	return varinfos;
}

/*
 * estimate_num_groups		- Estimate number of groups in a grouped query
 *
 * Given a query having a GROUP BY clause, estimate how many groups there
 * will be --- ie, the number of distinct combinations of the GROUP BY
 * expressions.
 *
 * This routine is also used to estimate the number of rows emitted by
 * a DISTINCT filtering step; that is an isomorphic problem.  (Note:
 * actually, we only use it for DISTINCT when there's no grouping or
 * aggregation ahead of the DISTINCT.)
 *
 * Inputs:
 *	root - the query
 *	groupExprs - list of expressions being grouped by
 *	input_rows - number of rows estimated to arrive at the group/unique
 *		filter step
 *	pgset - NULL, or a List** pointing to a grouping set to filter the
 *		groupExprs against
 *
 * Outputs:
 *	estinfo - When passed as non-NULL, the function will set bits in the
 *		"flags" field in order to provide callers with additional information
 *		about the estimation.  Currently, we only set the SELFLAG_USED_DEFAULT
 *		bit if we used any default values in the estimation.
 *
 * Given the lack of any cross-correlation statistics in the system, it's
 * impossible to do anything really trustworthy with GROUP BY conditions
 * involving multiple Vars.  We should however avoid assuming the worst
 * case (all possible cross-product terms actually appear as groups) since
 * very often the grouped-by Vars are highly correlated.  Our current approach
 * is as follows:
 *	1.  Expressions yielding boolean are assumed to contribute two groups,
 *		independently of their content, and are ignored in the subsequent
 *		steps.  This is mainly because tests like "col IS NULL" break the
 *		heuristic used in step 2 especially badly.
 *	2.  Reduce the given expressions to a list of unique Vars used.  For
 *		example, GROUP BY a, a + b is treated the same as GROUP BY a, b.
 *		It is clearly correct not to count the same Var more than once.
 *		It is also reasonable to treat f(x) the same as x: f() cannot
 *		increase the number of distinct values (unless it is volatile,
 *		which we consider unlikely for grouping), but it probably won't
 *		reduce the number of distinct values much either.
 *		As a special case, if a GROUP BY expression can be matched to an
 *		expressional index for which we have statistics, then we treat the
 *		whole expression as though it were just a Var.
 *	3.  If the list contains Vars of different relations that are known equal
 *		due to equivalence classes, then drop all but one of the Vars from each
 *		known-equal set, keeping the one with smallest estimated # of values
 *		(since the extra values of the others can't appear in joined rows).
 *		Note the reason we only consider Vars of different relations is that
 *		if we considered ones of the same rel, we'd be double-counting the
 *		restriction selectivity of the equality in the next step.
 *	4.  For Vars within a single source rel, we multiply together the numbers
 *		of values, clamp to the number of rows in the rel (divided by 10 if
 *		more than one Var), and then multiply by a factor based on the
 *		selectivity of the restriction clauses for that rel.  When there's
 *		more than one Var, the initial product is probably too high (it's the
 *		worst case) but clamping to a fraction of the rel's rows seems to be a
 *		helpful heuristic for not letting the estimate get out of hand.  (The
 *		factor of 10 is derived from pre-Postgres-7.4 practice.)  The factor
 *		we multiply by to adjust for the restriction selectivity assumes that
 *		the restriction clauses are independent of the grouping, which may not
 *		be a valid assumption, but it's hard to do better.
 *	5.  If there are Vars from multiple rels, we repeat step 4 for each such
 *		rel, and multiply the results together.
 * Note that rels not containing grouped Vars are ignored completely, as are
 * join clauses.  Such rels cannot increase the number of groups, and we
 * assume such clauses do not reduce the number either (somewhat bogus,
 * but we don't have the info to do better).
 */
double
estimate_num_groups(PlannerInfo *root, List *groupExprs, double input_rows,
					List **pgset, EstimationInfo *estinfo)
{
	List	   *varinfos = NIL;
	double		srf_multiplier = 1.0;
	double		numdistinct;
	ListCell   *l;
	int			i;

	/* Zero the estinfo output parameter, if non-NULL */
	if (estinfo != NULL)
		memset(estinfo, 0, sizeof(EstimationInfo));

	/*
	 * We don't ever want to return an estimate of zero groups, as that tends
	 * to lead to division-by-zero and other unpleasantness.  The input_rows
	 * estimate is usually already at least 1, but clamp it just in case it
	 * isn't.
	 */
	input_rows = clamp_row_est(input_rows);

	/*
	 * If no grouping columns, there's exactly one group.  (This can't happen
	 * for normal cases with GROUP BY or DISTINCT, but it is possible for
	 * corner cases with set operations.)
	 */
	if (groupExprs == NIL || (pgset && *pgset == NIL))
		return 1.0;

	/*
	 * Count groups derived from boolean grouping expressions.  For other
	 * expressions, find the unique Vars used, treating an expression as a Var
	 * if we can find stats for it.  For each one, record the statistical
	 * estimate of number of distinct values (total in its table, without
	 * regard for filtering).
	 */
	numdistinct = 1.0;

	i = 0;
	foreach(l, groupExprs)
	{
		Node	   *groupexpr = (Node *) lfirst(l);
		double		this_srf_multiplier;
		VariableStatData vardata;
		List	   *varshere;
		ListCell   *l2;

		/* is expression in this grouping set? */
		if (pgset && !list_member_int(*pgset, i++))
			continue;

		/*
		 * Set-returning functions in grouping columns are a bit problematic.
		 * The code below will effectively ignore their SRF nature and come up
		 * with a numdistinct estimate as though they were scalar functions.
		 * We compensate by scaling up the end result by the largest SRF
		 * rowcount estimate.  (This will be an overestimate if the SRF
		 * produces multiple copies of any output value, but it seems best to
		 * assume the SRF's outputs are distinct.  In any case, it's probably
		 * pointless to worry too much about this without much better
		 * estimates for SRF output rowcounts than we have today.)
		 */
		this_srf_multiplier = expression_returns_set_rows(root, groupexpr);
		if (srf_multiplier < this_srf_multiplier)
			srf_multiplier = this_srf_multiplier;

		/* Short-circuit for expressions returning boolean */
		if (exprType(groupexpr) == BOOLOID)
		{
			numdistinct *= 2.0;
			continue;
		}

		/*
		 * If examine_variable is able to deduce anything about the GROUP BY
		 * expression, treat it as a single variable even if it's really more
		 * complicated.
		 *
		 * XXX This has the consequence that if there's a statistics object on
		 * the expression, we don't split it into individual Vars. This
		 * affects our selection of statistics in
		 * estimate_multivariate_ndistinct, because it's probably better to
		 * use more accurate estimate for each expression and treat them as
		 * independent, than to combine estimates for the extracted variables
		 * when we don't know how that relates to the expressions.
		 */
		examine_variable(root, groupexpr, 0, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple) || vardata.isunique)
		{
			varinfos = add_unique_group_var(root, varinfos,
											groupexpr, &vardata);
			ReleaseVariableStats(vardata);
			continue;
		}
		ReleaseVariableStats(vardata);

		/*
		 * Else pull out the component Vars.  Handle PlaceHolderVars by
		 * recursing into their arguments (effectively assuming that the
		 * PlaceHolderVar doesn't change the number of groups, which boils
		 * down to ignoring the possible addition of nulls to the result set).
		 */
		varshere = pull_var_clause(groupexpr,
								   PVC_RECURSE_AGGREGATES |
								   PVC_RECURSE_WINDOWFUNCS |
								   PVC_RECURSE_PLACEHOLDERS);

		/*
		 * If we find any variable-free GROUP BY item, then either it is a
		 * constant (and we can ignore it) or it contains a volatile function;
		 * in the latter case we punt and assume that each input row will
		 * yield a distinct group.
		 */
		if (varshere == NIL)
		{
			if (contain_volatile_functions(groupexpr))
				return input_rows;
			continue;
		}

		/*
		 * Else add variables to varinfos list
		 */
		foreach(l2, varshere)
		{
			Node	   *var = (Node *) lfirst(l2);

			examine_variable(root, var, 0, &vardata);
			varinfos = add_unique_group_var(root, varinfos, var, &vardata);
			ReleaseVariableStats(vardata);
		}
	}

	/*
	 * If now no Vars, we must have an all-constant or all-boolean GROUP BY
	 * list.
	 */
	if (varinfos == NIL)
	{
		/* Apply SRF multiplier as we would do in the long path */
		numdistinct *= srf_multiplier;
		/* Round off */
		numdistinct = ceil(numdistinct);
		/* Guard against out-of-range answers */
		if (numdistinct > input_rows)
			numdistinct = input_rows;
		if (numdistinct < 1.0)
			numdistinct = 1.0;
		return numdistinct;
	}

	/*
	 * Group Vars by relation and estimate total numdistinct.
	 *
	 * For each iteration of the outer loop, we process the frontmost Var in
	 * varinfos, plus all other Vars in the same relation.  We remove these
	 * Vars from the newvarinfos list for the next iteration. This is the
	 * easiest way to group Vars of same rel together.
	 */
	do
	{
		GroupVarInfo *varinfo1 = (GroupVarInfo *) linitial(varinfos);
		RelOptInfo *rel = varinfo1->rel;
		double		reldistinct = 1;
		double		relmaxndistinct = reldistinct;
		int			relvarcount = 0;
		List	   *newvarinfos = NIL;
		List	   *relvarinfos = NIL;

		/*
		 * Split the list of varinfos in two - one for the current rel, one
		 * for remaining Vars on other rels.
		 */
		relvarinfos = lappend(relvarinfos, varinfo1);
		for_each_from(l, varinfos, 1)
		{
			GroupVarInfo *varinfo2 = (GroupVarInfo *) lfirst(l);

			if (varinfo2->rel == varinfo1->rel)
			{
				/* varinfos on current rel */
				relvarinfos = lappend(relvarinfos, varinfo2);
			}
			else
			{
				/* not time to process varinfo2 yet */
				newvarinfos = lappend(newvarinfos, varinfo2);
			}
		}

		/*
		 * Get the numdistinct estimate for the Vars of this rel.  We
		 * iteratively search for multivariate n-distinct with maximum number
		 * of vars; assuming that each var group is independent of the others,
		 * we multiply them together.  Any remaining relvarinfos after no more
		 * multivariate matches are found are assumed independent too, so
		 * their individual ndistinct estimates are multiplied also.
		 *
		 * While iterating, count how many separate numdistinct values we
		 * apply.  We apply a fudge factor below, but only if we multiplied
		 * more than one such values.
		 */
		while (relvarinfos)
		{
			double		mvndistinct;

			if (estimate_multivariate_ndistinct(root, rel, &relvarinfos,
												&mvndistinct))
			{
				reldistinct *= mvndistinct;
				if (relmaxndistinct < mvndistinct)
					relmaxndistinct = mvndistinct;
				relvarcount++;
			}
			else
			{
				foreach(l, relvarinfos)
				{
					GroupVarInfo *varinfo2 = (GroupVarInfo *) lfirst(l);

					reldistinct *= varinfo2->ndistinct;
					if (relmaxndistinct < varinfo2->ndistinct)
						relmaxndistinct = varinfo2->ndistinct;
					relvarcount++;

					/*
					 * When varinfo2's isdefault is set then we'd better set
					 * the SELFLAG_USED_DEFAULT bit in the EstimationInfo.
					 */
					if (estinfo != NULL && varinfo2->isdefault)
						estinfo->flags |= SELFLAG_USED_DEFAULT;
				}

				/* we're done with this relation */
				relvarinfos = NIL;
			}
		}

		/*
		 * Sanity check --- don't divide by zero if empty relation.
		 */
		Assert(IS_SIMPLE_REL(rel));
		if (rel->tuples > 0)
		{
			/*
			 * Clamp to size of rel, or size of rel / 10 if multiple Vars. The
			 * fudge factor is because the Vars are probably correlated but we
			 * don't know by how much.  We should never clamp to less than the
			 * largest ndistinct value for any of the Vars, though, since
			 * there will surely be at least that many groups.
			 */
			double		clamp = rel->tuples;

			if (relvarcount > 1)
			{
				clamp *= 0.1;
				if (clamp < relmaxndistinct)
				{
					clamp = relmaxndistinct;
					/* for sanity in case some ndistinct is too large: */
					if (clamp > rel->tuples)
						clamp = rel->tuples;
				}
			}
			if (reldistinct > clamp)
				reldistinct = clamp;

			/*
			 * Update the estimate based on the restriction selectivity,
			 * guarding against division by zero when reldistinct is zero.
			 * Also skip this if we know that we are returning all rows.
			 */
			if (reldistinct > 0 && rel->rows < rel->tuples)
			{
				/*
				 * Given a table containing N rows with n distinct values in a
				 * uniform distribution, if we select p rows at random then
				 * the expected number of distinct values selected is
				 *
				 * n * (1 - product((N-N/n-i)/(N-i), i=0..p-1))
				 *
				 * = n * (1 - (N-N/n)! / (N-N/n-p)! * (N-p)! / N!)
				 *
				 * See "Approximating block accesses in database
				 * organizations", S. B. Yao, Communications of the ACM,
				 * Volume 20 Issue 4, April 1977 Pages 260-261.
				 *
				 * Alternatively, re-arranging the terms from the factorials,
				 * this may be written as
				 *
				 * n * (1 - product((N-p-i)/(N-i), i=0..N/n-1))
				 *
				 * This form of the formula is more efficient to compute in
				 * the common case where p is larger than N/n.  Additionally,
				 * as pointed out by Dell'Era, if i << N for all terms in the
				 * product, it can be approximated by
				 *
				 * n * (1 - ((N-p)/N)^(N/n))
				 *
				 * See "Expected distinct values when selecting from a bag
				 * without replacement", Alberto Dell'Era,
				 * http://www.adellera.it/investigations/distinct_balls/.
				 *
				 * The condition i << N is equivalent to n >> 1, so this is a
				 * good approximation when the number of distinct values in
				 * the table is large.  It turns out that this formula also
				 * works well even when n is small.
				 */
				reldistinct *=
					(1 - pow((rel->tuples - rel->rows) / rel->tuples,
							 rel->tuples / reldistinct));
			}
			reldistinct = clamp_row_est(reldistinct);

			/*
			 * Update estimate of total distinct groups.
			 */
			numdistinct *= reldistinct;
		}

		varinfos = newvarinfos;
	} while (varinfos != NIL);

	/* Now we can account for the effects of any SRFs */
	numdistinct *= srf_multiplier;

	/* Round off */
	numdistinct = ceil(numdistinct);

	/* Guard against out-of-range answers */
	if (numdistinct > input_rows)
		numdistinct = input_rows;
	if (numdistinct < 1.0)
		numdistinct = 1.0;

	return numdistinct;
}

/*
 * Try to estimate the bucket size of the hash join inner side when the join
 * condition contains two or more clauses by employing extended statistics.
 *
 * The main idea of this approach is that the distinct value generated by
 * multivariate estimation on two or more columns would provide less bucket size
 * than estimation on one separate column.
 *
 * IMPORTANT: It is crucial to synchronize the approach of combining different
 * estimations with the caller's method.
 *
 * Return a list of clauses that didn't fetch any extended statistics.
 */
List *
estimate_multivariate_bucketsize(PlannerInfo *root, RelOptInfo *inner,
								 List *hashclauses,
								 Selectivity *innerbucketsize)
{
	List	   *clauses = list_copy(hashclauses);
	List	   *otherclauses = NIL;
	double		ndistinct = 1.0;

	if (list_length(hashclauses) <= 1)

		/*
		 * Nothing to do for a single clause.  Could we employ univariate
		 * extended stat here?
		 */
		return hashclauses;

	while (clauses != NIL)
	{
		ListCell   *lc;
		int			relid = -1;
		List	   *varinfos = NIL;
		List	   *origin_rinfos = NIL;
		double		mvndistinct;
		List	   *origin_varinfos;
		int			group_relid = -1;
		RelOptInfo *group_rel = NULL;
		ListCell   *lc1,
				   *lc2;

		/*
		 * Find clauses, referencing the same single base relation and try to
		 * estimate such a group with extended statistics.  Create varinfo for
		 * an approved clause, push it to otherclauses, if it can't be
		 * estimated here or ignore to process at the next iteration.
		 */
		foreach(lc, clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
			Node	   *expr;
			Relids		relids;
			GroupVarInfo *varinfo;

			/*
			 * Find the inner side of the join, which we need to estimate the
			 * number of buckets.  Use outer_is_left because the
			 * clause_sides_match_join routine has called on hash clauses.
			 */
			relids = rinfo->outer_is_left ?
				rinfo->right_relids : rinfo->left_relids;
			expr = rinfo->outer_is_left ?
				get_rightop(rinfo->clause) : get_leftop(rinfo->clause);

			if (bms_get_singleton_member(relids, &relid) &&
				root->simple_rel_array[relid]->statlist != NIL)
			{
				bool		is_duplicate = false;

				/*
				 * This inner-side expression references only one relation.
				 * Extended statistics on this clause can exist.
				 */
				if (group_relid < 0)
				{
					RangeTblEntry *rte = root->simple_rte_array[relid];

					if (!rte || (rte->relkind != RELKIND_RELATION &&
								 rte->relkind != RELKIND_MATVIEW &&
								 rte->relkind != RELKIND_FOREIGN_TABLE &&
								 rte->relkind != RELKIND_PARTITIONED_TABLE))
					{
						/* Extended statistics can't exist in principle */
						otherclauses = lappend(otherclauses, rinfo);
						clauses = foreach_delete_current(clauses, lc);
						continue;
					}

					group_relid = relid;
					group_rel = root->simple_rel_array[relid];
				}
				else if (group_relid != relid)

					/*
					 * Being in the group forming state we don't need other
					 * clauses.
					 */
					continue;

				/*
				 * We're going to add the new clause to the varinfos list.  We
				 * might re-use add_unique_group_var(), but we don't do so for
				 * two reasons.
				 *
				 * 1) We must keep the origin_rinfos list ordered exactly the
				 * same way as varinfos.
				 *
				 * 2) add_unique_group_var() is designed for
				 * estimate_num_groups(), where a larger number of groups is
				 * worse.   While estimating the number of hash buckets, we
				 * have the opposite: a lesser number of groups is worse.
				 * Therefore, we don't have to remove "known equal" vars: the
				 * removed var may valuably contribute to the multivariate
				 * statistics to grow the number of groups.
				 */

				/*
				 * Clear nullingrels to correctly match hash keys.  See
				 * add_unique_group_var()'s comment for details.
				 */
				expr = remove_nulling_relids(expr, root->outer_join_rels, NULL);

				/*
				 * Detect and exclude exact duplicates from the list of hash
				 * keys (like add_unique_group_var does).
				 */
				foreach(lc1, varinfos)
				{
					varinfo = (GroupVarInfo *) lfirst(lc1);

					if (!equal(expr, varinfo->var))
						continue;

					is_duplicate = true;
					break;
				}

				if (is_duplicate)
				{
					/*
					 * Skip exact duplicates. Adding them to the otherclauses
					 * list also doesn't make sense.
					 */
					continue;
				}

				/*
				 * Initialize GroupVarInfo.  We only use it to call
				 * estimate_multivariate_ndistinct(), which doesn't care about
				 * ndistinct and isdefault fields.  Thus, skip these fields.
				 */
				varinfo = (GroupVarInfo *) palloc0(sizeof(GroupVarInfo));
				varinfo->var = expr;
				varinfo->rel = root->simple_rel_array[relid];
				varinfos = lappend(varinfos, varinfo);

				/*
				 * Remember the link to RestrictInfo for the case the clause
				 * is failed to be estimated.
				 */
				origin_rinfos = lappend(origin_rinfos, rinfo);
			}
			else
			{
				/* This clause can't be estimated with extended statistics */
				otherclauses = lappend(otherclauses, rinfo);
			}

			clauses = foreach_delete_current(clauses, lc);
		}

		if (list_length(varinfos) < 2)
		{
			/*
			 * Multivariate statistics doesn't apply to single columns except
			 * for expressions, but it has not been implemented yet.
			 */
			otherclauses = list_concat(otherclauses, origin_rinfos);
			list_free_deep(varinfos);
			list_free(origin_rinfos);
			continue;
		}

		Assert(group_rel != NULL);

		/* Employ the extended statistics. */
		origin_varinfos = varinfos;
		for (;;)
		{
			bool		estimated = estimate_multivariate_ndistinct(root,
																	group_rel,
																	&varinfos,
																	&mvndistinct);

			if (!estimated)
				break;

			/*
			 * We've got an estimation.  Use ndistinct value in a consistent
			 * way - according to the caller's logic (see
			 * final_cost_hashjoin).
			 */
			if (ndistinct < mvndistinct)
				ndistinct = mvndistinct;
			Assert(ndistinct >= 1.0);
		}

		Assert(list_length(origin_varinfos) == list_length(origin_rinfos));

		/* Collect unmatched clauses as otherclauses. */
		forboth(lc1, origin_varinfos, lc2, origin_rinfos)
		{
			GroupVarInfo *vinfo = lfirst(lc1);

			if (!list_member_ptr(varinfos, vinfo))
				/* Already estimated */
				continue;

			/* Can't be estimated here - push to the returning list */
			otherclauses = lappend(otherclauses, lfirst(lc2));
		}
	}

	*innerbucketsize = 1.0 / ndistinct;
	return otherclauses;
}

/*
 * Estimate hash bucket statistics when the specified expression is used
 * as a hash key for the given number of buckets.
 *
 * This attempts to determine two values:
 *
 * 1. The frequency of the most common value of the expression (returns
 * zero into *mcv_freq if we can't get that).
 *
 * 2. The "bucketsize fraction", ie, average number of entries in a bucket
 * divided by total tuples in relation.
 *
 * XXX This is really pretty bogus since we're effectively assuming that the
 * distribution of hash keys will be the same after applying restriction
 * clauses as it was in the underlying relation.  However, we are not nearly
 * smart enough to figure out how the restrict clauses might change the
 * distribution, so this will have to do for now.
 *
 * We are passed the number of buckets the executor will use for the given
 * input relation.  If the data were perfectly distributed, with the same
 * number of tuples going into each available bucket, then the bucketsize
 * fraction would be 1/nbuckets.  But this happy state of affairs will occur
 * only if (a) there are at least nbuckets distinct data values, and (b)
 * we have a not-too-skewed data distribution.  Otherwise the buckets will
 * be nonuniformly occupied.  If the other relation in the join has a key
 * distribution similar to this one's, then the most-loaded buckets are
 * exactly those that will be probed most often.  Therefore, the "average"
 * bucket size for costing purposes should really be taken as something close
 * to the "worst case" bucket size.  We try to estimate this by adjusting the
 * fraction if there are too few distinct data values, and then scaling up
 * by the ratio of the most common value's frequency to the average frequency.
 *
 * If no statistics are available, use a default estimate of 0.1.  This will
 * discourage use of a hash rather strongly if the inner relation is large,
 * which is what we want.  We do not want to hash unless we know that the
 * inner rel is well-dispersed (or the alternatives seem much worse).
 *
 * The caller should also check that the mcv_freq is not so large that the
 * most common value would by itself require an impractically large bucket.
 * In a hash join, the executor can split buckets if they get too big, but
 * obviously that doesn't help for a bucket that contains many duplicates of
 * the same value.
 */
void
estimate_hash_bucket_stats(PlannerInfo *root, Node *hashkey, double nbuckets,
						   Selectivity *mcv_freq,
						   Selectivity *bucketsize_frac)
{
	VariableStatData vardata;
	double		estfract,
				ndistinct,
				stanullfrac,
				avgfreq;
	bool		isdefault;
	AttStatsSlot sslot;

	examine_variable(root, hashkey, 0, &vardata);

	/* Look up the frequency of the most common value, if available */
	*mcv_freq = 0.0;

	if (HeapTupleIsValid(vardata.statsTuple))
	{
		if (get_attstatsslot(&sslot, vardata.statsTuple,
							 STATISTIC_KIND_MCV, InvalidOid,
							 ATTSTATSSLOT_NUMBERS))
		{
			/*
			 * The first MCV stat is for the most common value.
			 */
			if (sslot.nnumbers > 0)
				*mcv_freq = sslot.numbers[0];
			free_attstatsslot(&sslot);
		}
	}

	/* Get number of distinct values */
	ndistinct = get_variable_numdistinct(&vardata, &isdefault);

	/*
	 * If ndistinct isn't real, punt.  We normally return 0.1, but if the
	 * mcv_freq is known to be even higher than that, use it instead.
	 */
	if (isdefault)
	{
		*bucketsize_frac = (Selectivity) Max(0.1, *mcv_freq);
		ReleaseVariableStats(vardata);
		return;
	}

	/* Get fraction that are null */
	if (HeapTupleIsValid(vardata.statsTuple))
	{
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);
		stanullfrac = stats->stanullfrac;
	}
	else
		stanullfrac = 0.0;

	/* Compute avg freq of all distinct data values in raw relation */
	avgfreq = (1.0 - stanullfrac) / ndistinct;

	/*
	 * Adjust ndistinct to account for restriction clauses.  Observe we are
	 * assuming that the data distribution is affected uniformly by the
	 * restriction clauses!
	 *
	 * XXX Possibly better way, but much more expensive: multiply by
	 * selectivity of rel's restriction clauses that mention the target Var.
	 */
	if (vardata.rel && vardata.rel->tuples > 0)
	{
		ndistinct *= vardata.rel->rows / vardata.rel->tuples;
		ndistinct = clamp_row_est(ndistinct);
	}

	/*
	 * Initial estimate of bucketsize fraction is 1/nbuckets as long as the
	 * number of buckets is less than the expected number of distinct values;
	 * otherwise it is 1/ndistinct.
	 */
	if (ndistinct > nbuckets)
		estfract = 1.0 / nbuckets;
	else
		estfract = 1.0 / ndistinct;

	/*
	 * Adjust estimated bucketsize upward to account for skewed distribution.
	 */
	if (avgfreq > 0.0 && *mcv_freq > avgfreq)
		estfract *= *mcv_freq / avgfreq;

	/*
	 * Clamp bucketsize to sane range (the above adjustment could easily
	 * produce an out-of-range result).  We set the lower bound a little above
	 * zero, since zero isn't a very sane result.
	 */
	if (estfract < 1.0e-6)
		estfract = 1.0e-6;
	else if (estfract > 1.0)
		estfract = 1.0;

	*bucketsize_frac = (Selectivity) estfract;

	ReleaseVariableStats(vardata);
}

/*
 * estimate_hashagg_tablesize
 *	  estimate the number of bytes that a hash aggregate hashtable will
 *	  require based on the agg_costs, path width and number of groups.
 *
 * We return the result as "double" to forestall any possible overflow
 * problem in the multiplication by dNumGroups.
 *
 * XXX this may be over-estimating the size now that hashagg knows to omit
 * unneeded columns from the hashtable.  Also for mixed-mode grouping sets,
 * grouping columns not in the hashed set are counted here even though hashagg
 * won't store them.  Is this a problem?
 */
double
estimate_hashagg_tablesize(PlannerInfo *root, Path *path,
						   const AggClauseCosts *agg_costs, double dNumGroups)
{
	Size		hashentrysize;

	hashentrysize = hash_agg_entry_size(list_length(root->aggtransinfos),
										path->pathtarget->width,
										agg_costs->transitionSpace);

	/*
	 * Note that this disregards the effect of fill-factor and growth policy
	 * of the hash table.  That's probably ok, given that the default
	 * fill-factor is relatively high.  It'd be hard to meaningfully factor in
	 * "double-in-size" growth policies here.
	 */
	return hashentrysize * dNumGroups;
}


/*-------------------------------------------------------------------------
 *
 * Support routines
 *
 *-------------------------------------------------------------------------
 */

/*
 * Find the best matching ndistinct extended statistics for the given list of
 * GroupVarInfos.
 *
 * Callers must ensure that the given GroupVarInfos all belong to 'rel' and
 * the GroupVarInfos list does not contain any duplicate Vars or expressions.
 *
 * When statistics are found that match > 1 of the given GroupVarInfo, the
 * *ndistinct parameter is set according to the ndistinct estimate and a new
 * list is built with the matching GroupVarInfos removed, which is output via
 * the *varinfos parameter before returning true.  When no matching stats are
 * found, false is returned and the *varinfos and *ndistinct parameters are
 * left untouched.
 */
static bool
estimate_multivariate_ndistinct(PlannerInfo *root, RelOptInfo *rel,
								List **varinfos, double *ndistinct)
{
	ListCell   *lc;
	int			nmatches_vars;
	int			nmatches_exprs;
	Oid			statOid = InvalidOid;
	MVNDistinct *stats;
	StatisticExtInfo *matched_info = NULL;
	RangeTblEntry *rte = planner_rt_fetch(rel->relid, root);

	/* bail out immediately if the table has no extended statistics */
	if (!rel->statlist)
		return false;

	/* look for the ndistinct statistics object matching the most vars */
	nmatches_vars = 0;			/* we require at least two matches */
	nmatches_exprs = 0;
	foreach(lc, rel->statlist)
	{
		ListCell   *lc2;
		StatisticExtInfo *info = (StatisticExtInfo *) lfirst(lc);
		int			nshared_vars = 0;
		int			nshared_exprs = 0;

		/* skip statistics of other kinds */
		if (info->kind != STATS_EXT_NDISTINCT)
			continue;

		/* skip statistics with mismatching stxdinherit value */
		if (info->inherit != rte->inh)
			continue;

		/*
		 * Determine how many expressions (and variables in non-matched
		 * expressions) match. We'll then use these numbers to pick the
		 * statistics object that best matches the clauses.
		 */
		foreach(lc2, *varinfos)
		{
			ListCell   *lc3;
			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc2);
			AttrNumber	attnum;

			Assert(varinfo->rel == rel);

			/* simple Var, search in statistics keys directly */
			if (IsA(varinfo->var, Var))
			{
				attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * Ignore system attributes - we don't support statistics on
				 * them, so can't match them (and it'd fail as the values are
				 * negative).
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
					continue;

				if (bms_is_member(attnum, info->keys))
					nshared_vars++;

				continue;
			}

			/* expression - see if it's in the statistics object */
			foreach(lc3, info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					nshared_exprs++;
					break;
				}
			}
		}

		/*
		 * The ndistinct extended statistics contain estimates for a minimum
		 * of pairs of columns which the statistics are defined on and
		 * certainly not single columns.  Here we skip unless we managed to
		 * match to at least two columns.
		 */
		if (nshared_vars + nshared_exprs < 2)
			continue;

		/*
		 * Check if these statistics are a better match than the previous best
		 * match and if so, take note of the StatisticExtInfo.
		 *
		 * The statslist is sorted by statOid, so the StatisticExtInfo we
		 * select as the best match is deterministic even when multiple sets
		 * of statistics match equally as well.
		 */
		if ((nshared_exprs > nmatches_exprs) ||
			(((nshared_exprs == nmatches_exprs)) && (nshared_vars > nmatches_vars)))
		{
			statOid = info->statOid;
			nmatches_vars = nshared_vars;
			nmatches_exprs = nshared_exprs;
			matched_info = info;
		}
	}

	/* No match? */
	if (statOid == InvalidOid)
		return false;

	Assert(nmatches_vars + nmatches_exprs > 1);

	stats = statext_ndistinct_load(statOid, rte->inh);

	/*
	 * If we have a match, search it for the specific item that matches (there
	 * must be one), and construct the output values.
	 */
	if (stats)
	{
		int			i;
		List	   *newlist = NIL;
		MVNDistinctItem *item = NULL;
		ListCell   *lc2;
		Bitmapset  *matched = NULL;
		AttrNumber	attnum_offset;

		/*
		 * How much we need to offset the attnums? If there are no
		 * expressions, no offset is needed. Otherwise offset enough to move
		 * the lowest one (which is equal to number of expressions) to 1.
		 */
		if (matched_info->exprs)
			attnum_offset = (list_length(matched_info->exprs) + 1);
		else
			attnum_offset = 0;

		/* see what actually matched */
		foreach(lc2, *varinfos)
		{
			ListCell   *lc3;
			int			idx;
			bool		found = false;

			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc2);

			/*
			 * Process a simple Var expression, by matching it to keys
			 * directly. If there's a matching expression, we'll try matching
			 * it later.
			 */
			if (IsA(varinfo->var, Var))
			{
				AttrNumber	attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * Ignore expressions on system attributes. Can't rely on the
				 * bms check for negative values.
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
					continue;

				/* Is the variable covered by the statistics object? */
				if (!bms_is_member(attnum, matched_info->keys))
					continue;

				attnum = attnum + attnum_offset;

				/* ensure sufficient offset */
				Assert(AttrNumberIsForUserDefinedAttr(attnum));

				matched = bms_add_member(matched, attnum);

				found = true;
			}

			/*
			 * XXX Maybe we should allow searching the expressions even if we
			 * found an attribute matching the expression? That would handle
			 * trivial expressions like "(a)" but it seems fairly useless.
			 */
			if (found)
				continue;

			/* expression - see if it's in the statistics object */
			idx = 0;
			foreach(lc3, matched_info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					AttrNumber	attnum = -(idx + 1);

					attnum = attnum + attnum_offset;

					/* ensure sufficient offset */
					Assert(AttrNumberIsForUserDefinedAttr(attnum));

					matched = bms_add_member(matched, attnum);

					/* there should be just one matching expression */
					break;
				}

				idx++;
			}
		}

		/* Find the specific item that exactly matches the combination */
		for (i = 0; i < stats->nitems; i++)
		{
			int			j;
			MVNDistinctItem *tmpitem = &stats->items[i];

			if (tmpitem->nattributes != bms_num_members(matched))
				continue;

			/* assume it's the right item */
			item = tmpitem;

			/* check that all item attributes/expressions fit the match */
			for (j = 0; j < tmpitem->nattributes; j++)
			{
				AttrNumber	attnum = tmpitem->attributes[j];

				/*
				 * Thanks to how we constructed the matched bitmap above, we
				 * can just offset all attnums the same way.
				 */
				attnum = attnum + attnum_offset;

				if (!bms_is_member(attnum, matched))
				{
					/* nah, it's not this item */
					item = NULL;
					break;
				}
			}

			/*
			 * If the item has all the matched attributes, we know it's the
			 * right one - there can't be a better one. matching more.
			 */
			if (item)
				break;
		}

		/*
		 * Make sure we found an item. There has to be one, because ndistinct
		 * statistics includes all combinations of attributes.
		 */
		if (!item)
			elog(ERROR, "corrupt MVNDistinct entry");

		/* Form the output varinfo list, keeping only unmatched ones */
		foreach(lc, *varinfos)
		{
			GroupVarInfo *varinfo = (GroupVarInfo *) lfirst(lc);
			ListCell   *lc3;
			bool		found = false;

			/*
			 * Let's look at plain variables first, because it's the most
			 * common case and the check is quite cheap. We can simply get the
			 * attnum and check (with an offset) matched bitmap.
			 */
			if (IsA(varinfo->var, Var))
			{
				AttrNumber	attnum = ((Var *) varinfo->var)->varattno;

				/*
				 * If it's a system attribute, we're done. We don't support
				 * extended statistics on system attributes, so it's clearly
				 * not matched. Just keep the expression and continue.
				 */
				if (!AttrNumberIsForUserDefinedAttr(attnum))
				{
					newlist = lappend(newlist, varinfo);
					continue;
				}

				/* apply the same offset as above */
				attnum += attnum_offset;

				/* if it's not matched, keep the varinfo */
				if (!bms_is_member(attnum, matched))
					newlist = lappend(newlist, varinfo);

				/* The rest of the loop deals with complex expressions. */
				continue;
			}

			/*
			 * Process complex expressions, not just simple Vars.
			 *
			 * First, we search for an exact match of an expression. If we
			 * find one, we can just discard the whole GroupVarInfo, with all
			 * the variables we extracted from it.
			 *
			 * Otherwise we inspect the individual vars, and try matching it
			 * to variables in the item.
			 */
			foreach(lc3, matched_info->exprs)
			{
				Node	   *expr = (Node *) lfirst(lc3);

				if (equal(varinfo->var, expr))
				{
					found = true;
					break;
				}
			}

			/* found exact match, skip */
			if (found)
				continue;

			newlist = lappend(newlist, varinfo);
		}

		*varinfos = newlist;
		*ndistinct = item->ndistinct;
		return true;
	}

	return false;
}

/*
 * convert_to_scalar
 *	  Convert non-NULL values of the indicated types to the comparison
 *	  scale needed by scalarineqsel().
 *	  Returns "true" if successful.
 *
 * XXX this routine is a hack: ideally we should look up the conversion
 * subroutines in pg_type.
 *
 * All numeric datatypes are simply converted to their equivalent
 * "double" values.  (NUMERIC values that are outside the range of "double"
 * are clamped to +/- HUGE_VAL.)
 *
 * String datatypes are converted by convert_string_to_scalar(),
 * which is explained below.  The reason why this routine deals with
 * three values at a time, not just one, is that we need it for strings.
 *
 * The bytea datatype is just enough different from strings that it has
 * to be treated separately.
 *
 * The several datatypes representing absolute times are all converted
 * to Timestamp, which is actually an int64, and then we promote that to
 * a double.  Note this will give correct results even for the "special"
 * values of Timestamp, since those are chosen to compare correctly;
 * see timestamp_cmp.
 *
 * The several datatypes representing relative times (intervals) are all
 * converted to measurements expressed in seconds.
 */
static bool
convert_to_scalar(Datum value, Oid valuetypid, Oid collid, double *scaledvalue,
				  Datum lobound, Datum hibound, Oid boundstypid,
				  double *scaledlobound, double *scaledhibound)
{
	bool		failure = false;

	/*
	 * Both the valuetypid and the boundstypid should exactly match the
	 * declared input type(s) of the operator we are invoked for.  However,
	 * extensions might try to use scalarineqsel as estimator for operators
	 * with input type(s) we don't handle here; in such cases, we want to
	 * return false, not fail.  In any case, we mustn't assume that valuetypid
	 * and boundstypid are identical.
	 *
	 * XXX The histogram we are interpolating between points of could belong
	 * to a column that's only binary-compatible with the declared type. In
	 * essence we are assuming that the semantics of binary-compatible types
	 * are enough alike that we can use a histogram generated with one type's
	 * operators to estimate selectivity for the other's.  This is outright
	 * wrong in some cases --- in particular signed versus unsigned
	 * interpretation could trip us up.  But it's useful enough in the
	 * majority of cases that we do it anyway.  Should think about more
	 * rigorous ways to do it.
	 */
	switch (valuetypid)
	{
			/*
			 * Built-in numeric types
			 */
		case BOOLOID:
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
		case REGDATABASEOID:
			*scaledvalue = convert_numeric_to_scalar(value, valuetypid,
													 &failure);
			*scaledlobound = convert_numeric_to_scalar(lobound, boundstypid,
													   &failure);
			*scaledhibound = convert_numeric_to_scalar(hibound, boundstypid,
													   &failure);
			return !failure;

			/*
			 * Built-in string types
			 */
		case CHAROID:
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		case NAMEOID:
			{
				char	   *valstr = convert_string_datum(value, valuetypid,
														  collid, &failure);
				char	   *lostr = convert_string_datum(lobound, boundstypid,
														 collid, &failure);
				char	   *histr = convert_string_datum(hibound, boundstypid,
														 collid, &failure);

				/*
				 * Bail out if any of the values is not of string type.  We
				 * might leak converted strings for the other value(s), but
				 * that's not worth troubling over.
				 */
				if (failure)
					return false;

				convert_string_to_scalar(valstr, scaledvalue,
										 lostr, scaledlobound,
										 histr, scaledhibound);
				pfree(valstr);
				pfree(lostr);
				pfree(histr);
				return true;
			}

			/*
			 * Built-in bytea type
			 */
		case BYTEAOID:
			{
				/* We only support bytea vs bytea comparison */
				if (boundstypid != BYTEAOID)
					return false;
				convert_bytea_to_scalar(value, scaledvalue,
										lobound, scaledlobound,
										hibound, scaledhibound);
				return true;
			}

			/*
			 * Built-in time types
			 */
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case DATEOID:
		case INTERVALOID:
		case TIMEOID:
		case TIMETZOID:
			*scaledvalue = convert_timevalue_to_scalar(value, valuetypid,
													   &failure);
			*scaledlobound = convert_timevalue_to_scalar(lobound, boundstypid,
														 &failure);
			*scaledhibound = convert_timevalue_to_scalar(hibound, boundstypid,
														 &failure);
			return !failure;

			/*
			 * Built-in network types
			 */
		case INETOID:
		case CIDROID:
		case MACADDROID:
		case MACADDR8OID:
			*scaledvalue = convert_network_to_scalar(value, valuetypid,
													 &failure);
			*scaledlobound = convert_network_to_scalar(lobound, boundstypid,
													   &failure);
			*scaledhibound = convert_network_to_scalar(hibound, boundstypid,
													   &failure);
			return !failure;
	}
	/* Don't know how to convert */
	*scaledvalue = *scaledlobound = *scaledhibound = 0;
	return false;
}

/*
 * Do convert_to_scalar()'s work for any numeric data type.
 *
 * On failure (e.g., unsupported typid), set *failure to true;
 * otherwise, that variable is not changed.
 */
static double
convert_numeric_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case BOOLOID:
			return (double) DatumGetBool(value);
		case INT2OID:
			return (double) DatumGetInt16(value);
		case INT4OID:
			return (double) DatumGetInt32(value);
		case INT8OID:
			return (double) DatumGetInt64(value);
		case FLOAT4OID:
			return (double) DatumGetFloat4(value);
		case FLOAT8OID:
			return (double) DatumGetFloat8(value);
		case NUMERICOID:
			/* Note: out-of-range values will be clamped to +-HUGE_VAL */
			return (double)
				DatumGetFloat8(DirectFunctionCall1(numeric_float8_no_overflow,
												   value));
		case OIDOID:
		case REGPROCOID:
		case REGPROCEDUREOID:
		case REGOPEROID:
		case REGOPERATOROID:
		case REGCLASSOID:
		case REGTYPEOID:
		case REGCOLLATIONOID:
		case REGCONFIGOID:
		case REGDICTIONARYOID:
		case REGROLEOID:
		case REGNAMESPACEOID:
		case REGDATABASEOID:
			/* we can treat OIDs as integers... */
			return (double) DatumGetObjectId(value);
	}

	*failure = true;
	return 0;
}

/*
 * Do convert_to_scalar()'s work for any character-string data type.
 *
 * String datatypes are converted to a scale that ranges from 0 to 1,
 * where we visualize the bytes of the string as fractional digits.
 *
 * We do not want the base to be 256, however, since that tends to
 * generate inflated selectivity estimates; few databases will have
 * occurrences of all 256 possible byte values at each position.
 * Instead, use the smallest and largest byte values seen in the bounds
 * as the estimated range for each byte, after some fudging to deal with
 * the fact that we probably aren't going to see the full range that way.
 *
 * An additional refinement is that we discard any common prefix of the
 * three strings before computing the scaled values.  This allows us to
 * "zoom in" when we encounter a narrow data range.  An example is a phone
 * number database where all the values begin with the same area code.
 * (Actually, the bounds will be adjacent histogram-bin-boundary values,
 * so this is more likely to happen than you might think.)
 */
static void
convert_string_to_scalar(char *value,
						 double *scaledvalue,
						 char *lobound,
						 double *scaledlobound,
						 char *hibound,
						 double *scaledhibound)
{
	int			rangelo,
				rangehi;
	char	   *sptr;

	rangelo = rangehi = (unsigned char) hibound[0];
	for (sptr = lobound; *sptr; sptr++)
	{
		if (rangelo > (unsigned char) *sptr)
			rangelo = (unsigned char) *sptr;
		if (rangehi < (unsigned char) *sptr)
			rangehi = (unsigned char) *sptr;
	}
	for (sptr = hibound; *sptr; sptr++)
	{
		if (rangelo > (unsigned char) *sptr)
			rangelo = (unsigned char) *sptr;
		if (rangehi < (unsigned char) *sptr)
			rangehi = (unsigned char) *sptr;
	}
	/* If range includes any upper-case ASCII chars, make it include all */
	if (rangelo <= 'Z' && rangehi >= 'A')
	{
		if (rangelo > 'A')
			rangelo = 'A';
		if (rangehi < 'Z')
			rangehi = 'Z';
	}
	/* Ditto lower-case */
	if (rangelo <= 'z' && rangehi >= 'a')
	{
		if (rangelo > 'a')
			rangelo = 'a';
		if (rangehi < 'z')
			rangehi = 'z';
	}
	/* Ditto digits */
	if (rangelo <= '9' && rangehi >= '0')
	{
		if (rangelo > '0')
			rangelo = '0';
		if (rangehi < '9')
			rangehi = '9';
	}

	/*
	 * If range includes less than 10 chars, assume we have not got enough
	 * data, and make it include regular ASCII set.
	 */
	if (rangehi - rangelo < 9)
	{
		rangelo = ' ';
		rangehi = 127;
	}

	/*
	 * Now strip any common prefix of the three strings.
	 */
	while (*lobound)
	{
		if (*lobound != *hibound || *lobound != *value)
			break;
		lobound++, hibound++, value++;
	}

	/*
	 * Now we can do the conversions.
	 */
	*scaledvalue = convert_one_string_to_scalar(value, rangelo, rangehi);
	*scaledlobound = convert_one_string_to_scalar(lobound, rangelo, rangehi);
	*scaledhibound = convert_one_string_to_scalar(hibound, rangelo, rangehi);
}

static double
convert_one_string_to_scalar(char *value, int rangelo, int rangehi)
{
	int			slen = strlen(value);
	double		num,
				denom,
				base;

	if (slen <= 0)
		return 0.0;				/* empty string has scalar value 0 */

	/*
	 * There seems little point in considering more than a dozen bytes from
	 * the string.  Since base is at least 10, that will give us nominal
	 * resolution of at least 12 decimal digits, which is surely far more
	 * precision than this estimation technique has got anyway (especially in
	 * non-C locales).  Also, even with the maximum possible base of 256, this
	 * ensures denom cannot grow larger than 256^13 = 2.03e31, which will not
	 * overflow on any known machine.
	 */
	if (slen > 12)
		slen = 12;

	/* Convert initial characters to fraction */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (slen-- > 0)
	{
		int			ch = (unsigned char) *value++;

		if (ch < rangelo)
			ch = rangelo - 1;
		else if (ch > rangehi)
			ch = rangehi + 1;
		num += ((double) (ch - rangelo)) / denom;
		denom *= base;
	}

	return num;
}

/*
 * Convert a string-type Datum into a palloc'd, null-terminated string.
 *
 * On failure (e.g., unsupported typid), set *failure to true;
 * otherwise, that variable is not changed.  (We'll return NULL on failure.)
 *
 * When using a non-C locale, we must pass the string through pg_strxfrm()
 * before continuing, so as to generate correct locale-specific results.
 */
static char *
convert_string_datum(Datum value, Oid typid, Oid collid, bool *failure)
{
	char	   *val;
	pg_locale_t mylocale;

	switch (typid)
	{
		case CHAROID:
			val = (char *) palloc(2);
			val[0] = DatumGetChar(value);
			val[1] = '\0';
			break;
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
			val = TextDatumGetCString(value);
			break;
		case NAMEOID:
			{
				NameData   *nm = (NameData *) DatumGetPointer(value);

				val = pstrdup(NameStr(*nm));
				break;
			}
		default:
			*failure = true;
			return NULL;
	}

	mylocale = pg_newlocale_from_collation(collid);

	if (!mylocale->collate_is_c)
	{
		char	   *xfrmstr;
		size_t		xfrmlen;
		size_t		xfrmlen2 PG_USED_FOR_ASSERTS_ONLY;

		/*
		 * XXX: We could guess at a suitable output buffer size and only call
		 * pg_strxfrm() twice if our guess is too small.
		 *
		 * XXX: strxfrm doesn't support UTF-8 encoding on Win32, it can return
		 * bogus data or set an error. This is not really a problem unless it
		 * crashes since it will only give an estimation error and nothing
		 * fatal.
		 *
		 * XXX: we do not check pg_strxfrm_enabled(). On some platforms and in
		 * some cases, libc strxfrm() may return the wrong results, but that
		 * will only lead to an estimation error.
		 */
		xfrmlen = pg_strxfrm(NULL, val, 0, mylocale);
#ifdef WIN32

		/*
		 * On Windows, strxfrm returns INT_MAX when an error occurs. Instead
		 * of trying to allocate this much memory (and fail), just return the
		 * original string unmodified as if we were in the C locale.
		 */
		if (xfrmlen == INT_MAX)
			return val;
#endif
		xfrmstr = (char *) palloc(xfrmlen + 1);
		xfrmlen2 = pg_strxfrm(xfrmstr, val, xfrmlen + 1, mylocale);

		/*
		 * Some systems (e.g., glibc) can return a smaller value from the
		 * second call than the first; thus the Assert must be <= not ==.
		 */
		Assert(xfrmlen2 <= xfrmlen);
		pfree(val);
		val = xfrmstr;
	}

	return val;
}

/*
 * Do convert_to_scalar()'s work for any bytea data type.
 *
 * Very similar to convert_string_to_scalar except we can't assume
 * null-termination and therefore pass explicit lengths around.
 *
 * Also, assumptions about likely "normal" ranges of characters have been
 * removed - a data range of 0..255 is always used, for now.  (Perhaps
 * someday we will add information about actual byte data range to
 * pg_statistic.)
 */
static void
convert_bytea_to_scalar(Datum value,
						double *scaledvalue,
						Datum lobound,
						double *scaledlobound,
						Datum hibound,
						double *scaledhibound)
{
	bytea	   *valuep = DatumGetByteaPP(value);
	bytea	   *loboundp = DatumGetByteaPP(lobound);
	bytea	   *hiboundp = DatumGetByteaPP(hibound);
	int			rangelo,
				rangehi,
				valuelen = VARSIZE_ANY_EXHDR(valuep),
				loboundlen = VARSIZE_ANY_EXHDR(loboundp),
				hiboundlen = VARSIZE_ANY_EXHDR(hiboundp),
				i,
				minlen;
	unsigned char *valstr = (unsigned char *) VARDATA_ANY(valuep);
	unsigned char *lostr = (unsigned char *) VARDATA_ANY(loboundp);
	unsigned char *histr = (unsigned char *) VARDATA_ANY(hiboundp);

	/*
	 * Assume bytea data is uniformly distributed across all byte values.
	 */
	rangelo = 0;
	rangehi = 255;

	/*
	 * Now strip any common prefix of the three strings.
	 */
	minlen = Min(Min(valuelen, loboundlen), hiboundlen);
	for (i = 0; i < minlen; i++)
	{
		if (*lostr != *histr || *lostr != *valstr)
			break;
		lostr++, histr++, valstr++;
		loboundlen--, hiboundlen--, valuelen--;
	}

	/*
	 * Now we can do the conversions.
	 */
	*scaledvalue = convert_one_bytea_to_scalar(valstr, valuelen, rangelo, rangehi);
	*scaledlobound = convert_one_bytea_to_scalar(lostr, loboundlen, rangelo, rangehi);
	*scaledhibound = convert_one_bytea_to_scalar(histr, hiboundlen, rangelo, rangehi);
}

static double
convert_one_bytea_to_scalar(unsigned char *value, int valuelen,
							int rangelo, int rangehi)
{
	double		num,
				denom,
				base;

	if (valuelen <= 0)
		return 0.0;				/* empty string has scalar value 0 */

	/*
	 * Since base is 256, need not consider more than about 10 chars (even
	 * this many seems like overkill)
	 */
	if (valuelen > 10)
		valuelen = 10;

	/* Convert initial characters to fraction */
	base = rangehi - rangelo + 1;
	num = 0.0;
	denom = base;
	while (valuelen-- > 0)
	{
		int			ch = *value++;

		if (ch < rangelo)
			ch = rangelo - 1;
		else if (ch > rangehi)
			ch = rangehi + 1;
		num += ((double) (ch - rangelo)) / denom;
		denom *= base;
	}

	return num;
}

/*
 * Do convert_to_scalar()'s work for any timevalue data type.
 *
 * On failure (e.g., unsupported typid), set *failure to true;
 * otherwise, that variable is not changed.
 */
static double
convert_timevalue_to_scalar(Datum value, Oid typid, bool *failure)
{
	switch (typid)
	{
		case TIMESTAMPOID:
			return DatumGetTimestamp(value);
		case TIMESTAMPTZOID:
			return DatumGetTimestampTz(value);
		case DATEOID:
			return date2timestamp_no_overflow(DatumGetDateADT(value));
		case INTERVALOID:
			{
				Interval   *interval = DatumGetIntervalP(value);

				/*
				 * Convert the month part of Interval to days using assumed
				 * average month length of 365.25/12.0 days.  Not too
				 * accurate, but plenty good enough for our purposes.
				 *
				 * This also works for infinite intervals, which just have all
				 * fields set to INT_MIN/INT_MAX, and so will produce a result
				 * smaller/larger than any finite interval.
				 */
				return interval->time + interval->day * (double) USECS_PER_DAY +
					interval->month * ((DAYS_PER_YEAR / (double) MONTHS_PER_YEAR) * USECS_PER_DAY);
			}
		case TIMEOID:
			return DatumGetTimeADT(value);
		case TIMETZOID:
			{
				TimeTzADT  *timetz = DatumGetTimeTzADTP(value);

				/* use GMT-equivalent time */
				return (double) (timetz->time + (timetz->zone * 1000000.0));
			}
	}

	*failure = true;
	return 0;
}


/*
 * get_restriction_variable
 *		Examine the args of a restriction clause to see if it's of the
 *		form (variable op pseudoconstant) or (pseudoconstant op variable),
 *		where "variable" could be either a Var or an expression in vars of a
 *		single relation.  If so, extract information about the variable,
 *		and also indicate which side it was on and the other argument.
 *
 * Inputs:
 *	root: the planner info
 *	args: clause argument list
 *	varRelid: see specs for restriction selectivity functions
 *
 * Outputs: (these are valid only if true is returned)
 *	*vardata: gets information about variable (see examine_variable)
 *	*other: gets other clause argument, aggressively reduced to a constant
 *	*varonleft: set true if variable is on the left, false if on the right
 *
 * Returns true if a variable is identified, otherwise false.
 *
 * Note: if there are Vars on both sides of the clause, we must fail, because
 * callers are expecting that the other side will act like a pseudoconstant.
 */
bool
get_restriction_variable(PlannerInfo *root, List *args, int varRelid,
						 VariableStatData *vardata, Node **other,
						 bool *varonleft)
{
	Node	   *left,
			   *right;
	VariableStatData rdata;

	/* Fail if not a binary opclause (probably shouldn't happen) */
	if (list_length(args) != 2)
		return false;

	left = (Node *) linitial(args);
	right = (Node *) lsecond(args);

	/*
	 * Examine both sides.  Note that when varRelid is nonzero, Vars of other
	 * relations will be treated as pseudoconstants.
	 */
	examine_variable(root, left, varRelid, vardata);
	examine_variable(root, right, varRelid, &rdata);

	/*
	 * If one side is a variable and the other not, we win.
	 */
	if (vardata->rel && rdata.rel == NULL)
	{
		*varonleft = true;
		*other = estimate_expression_value(root, rdata.var);
		/* Assume we need no ReleaseVariableStats(rdata) here */
		return true;
	}

	if (vardata->rel == NULL && rdata.rel)
	{
		*varonleft = false;
		*other = estimate_expression_value(root, vardata->var);
		/* Assume we need no ReleaseVariableStats(*vardata) here */
		*vardata = rdata;
		return true;
	}

	/* Oops, clause has wrong structure (probably var op var) */
	ReleaseVariableStats(*vardata);
	ReleaseVariableStats(rdata);

	return false;
}

/*
 * get_join_variables
 *		Apply examine_variable() to each side of a join clause.
 *		Also, attempt to identify whether the join clause has the same
 *		or reversed sense compared to the SpecialJoinInfo.
 *
 * We consider the join clause "normal" if it is "lhs_var OP rhs_var",
 * or "reversed" if it is "rhs_var OP lhs_var".  In complicated cases
 * where we can't tell for sure, we default to assuming it's normal.
 */
void
get_join_variables(PlannerInfo *root, List *args, SpecialJoinInfo *sjinfo,
				   VariableStatData *vardata1, VariableStatData *vardata2,
				   bool *join_is_reversed)
{
	Node	   *left,
			   *right;

	if (list_length(args) != 2)
		elog(ERROR, "join operator should take two arguments");

	left = (Node *) linitial(args);
	right = (Node *) lsecond(args);

	examine_variable(root, left, 0, vardata1);
	examine_variable(root, right, 0, vardata2);

	if (vardata1->rel &&
		bms_is_subset(vardata1->rel->relids, sjinfo->syn_righthand))
		*join_is_reversed = true;	/* var1 is on RHS */
	else if (vardata2->rel &&
			 bms_is_subset(vardata2->rel->relids, sjinfo->syn_lefthand))
		*join_is_reversed = true;	/* var2 is on LHS */
	else
		*join_is_reversed = false;
}

/* statext_expressions_load copies the tuple, so just pfree it. */
static void
ReleaseDummy(HeapTuple tuple)
{
	pfree(tuple);
}

/*
 * examine_variable
 *		Try to look up statistical data about an expression.
 *		Fill in a VariableStatData struct to describe the expression.
 *
 * Inputs:
 *	root: the planner info
 *	node: the expression tree to examine
 *	varRelid: see specs for restriction selectivity functions
 *
 * Outputs: *vardata is filled as follows:
 *	var: the input expression (with any binary relabeling stripped, if
 *		it is or contains a variable; but otherwise the type is preserved)
 *	rel: RelOptInfo for relation containing variable; NULL if expression
 *		contains no Vars (NOTE this could point to a RelOptInfo of a
 *		subquery, not one in the current query).
 *	statsTuple: the pg_statistic entry for the variable, if one exists;
 *		otherwise NULL.
 *	freefunc: pointer to a function to release statsTuple with.
 *	vartype: exposed type of the expression; this should always match
 *		the declared input type of the operator we are estimating for.
 *	atttype, atttypmod: actual type/typmod of the "var" expression.  This is
 *		commonly the same as the exposed type of the variable argument,
 *		but can be different in binary-compatible-type cases.
 *	isunique: true if we were able to match the var to a unique index, a
 *		single-column DISTINCT or GROUP-BY clause, implying its values are
 *		unique for this query.  (Caution: this should be trusted for
 *		statistical purposes only, since we do not check indimmediate nor
 *		verify that the exact same definition of equality applies.)
 *	acl_ok: true if current user has permission to read the column(s)
 *		underlying the pg_statistic entry.  This is consulted by
 *		statistic_proc_security_check().
 *
 * Caller is responsible for doing ReleaseVariableStats() before exiting.
 */
void
examine_variable(PlannerInfo *root, Node *node, int varRelid,
				 VariableStatData *vardata)
{
	Node	   *basenode;
	Relids		varnos;
	Relids		basevarnos;
	RelOptInfo *onerel;

	/* Make sure we don't return dangling pointers in vardata */
	MemSet(vardata, 0, sizeof(VariableStatData));

	/* Save the exposed type of the expression */
	vardata->vartype = exprType(node);

	/* Look inside any binary-compatible relabeling */

	if (IsA(node, RelabelType))
		basenode = (Node *) ((RelabelType *) node)->arg;
	else
		basenode = node;

	/* Fast path for a simple Var */

	if (IsA(basenode, Var) &&
		(varRelid == 0 || varRelid == ((Var *) basenode)->varno))
	{
		Var		   *var = (Var *) basenode;

		/* Set up result fields other than the stats tuple */
		vardata->var = basenode;	/* return Var without relabeling */
		vardata->rel = find_base_rel(root, var->varno);
		vardata->atttype = var->vartype;
		vardata->atttypmod = var->vartypmod;
		vardata->isunique = has_unique_index(vardata->rel, var->varattno);

		/* Try to locate some stats */
		examine_simple_variable(root, var, vardata);

		return;
	}

	/*
	 * Okay, it's a more complicated expression.  Determine variable
	 * membership.  Note that when varRelid isn't zero, only vars of that
	 * relation are considered "real" vars.
	 */
	varnos = pull_varnos(root, basenode);
	basevarnos = bms_difference(varnos, root->outer_join_rels);

	onerel = NULL;

	if (bms_is_empty(basevarnos))
	{
		/* No Vars at all ... must be pseudo-constant clause */
	}
	else
	{
		int			relid;

		/* Check if the expression is in vars of a single base relation */
		if (bms_get_singleton_member(basevarnos, &relid))
		{
			if (varRelid == 0 || varRelid == relid)
			{
				onerel = find_base_rel(root, relid);
				vardata->rel = onerel;
				node = basenode;	/* strip any relabeling */
			}
			/* else treat it as a constant */
		}
		else
		{
			/* varnos has multiple relids */
			if (varRelid == 0)
			{
				/* treat it as a variable of a join relation */
				vardata->rel = find_join_rel(root, varnos);
				node = basenode;	/* strip any relabeling */
			}
			else if (bms_is_member(varRelid, varnos))
			{
				/* ignore the vars belonging to other relations */
				vardata->rel = find_base_rel(root, varRelid);
				node = basenode;	/* strip any relabeling */
				/* note: no point in expressional-index search here */
			}
			/* else treat it as a constant */
		}
	}

	bms_free(basevarnos);

	vardata->var = node;
	vardata->atttype = exprType(node);
	vardata->atttypmod = exprTypmod(node);

	if (onerel)
	{
		/*
		 * We have an expression in vars of a single relation.  Try to match
		 * it to expressional index columns, in hopes of finding some
		 * statistics.
		 *
		 * Note that we consider all index columns including INCLUDE columns,
		 * since there could be stats for such columns.  But the test for
		 * uniqueness needs to be warier.
		 *
		 * XXX it's conceivable that there are multiple matches with different
		 * index opfamilies; if so, we need to pick one that matches the
		 * operator we are estimating for.  FIXME later.
		 */
		ListCell   *ilist;
		ListCell   *slist;
		Oid			userid;

		/*
		 * The nullingrels bits within the expression could prevent us from
		 * matching it to expressional index columns or to the expressions in
		 * extended statistics.  So strip them out first.
		 */
		if (bms_overlap(varnos, root->outer_join_rels))
			node = remove_nulling_relids(node, root->outer_join_rels, NULL);

		/*
		 * Determine the user ID to use for privilege checks: either
		 * onerel->userid if it's set (e.g., in case we're accessing the table
		 * via a view), or the current user otherwise.
		 *
		 * If we drill down to child relations, we keep using the same userid:
		 * it's going to be the same anyway, due to how we set up the relation
		 * tree (q.v. build_simple_rel).
		 */
		userid = OidIsValid(onerel->userid) ? onerel->userid : GetUserId();

		foreach(ilist, onerel->indexlist)
		{
			IndexOptInfo *index = (IndexOptInfo *) lfirst(ilist);
			ListCell   *indexpr_item;
			int			pos;

			indexpr_item = list_head(index->indexprs);
			if (indexpr_item == NULL)
				continue;		/* no expressions here... */

			for (pos = 0; pos < index->ncolumns; pos++)
			{
				if (index->indexkeys[pos] == 0)
				{
					Node	   *indexkey;

					if (indexpr_item == NULL)
						elog(ERROR, "too few entries in indexprs list");
					indexkey = (Node *) lfirst(indexpr_item);
					if (indexkey && IsA(indexkey, RelabelType))
						indexkey = (Node *) ((RelabelType *) indexkey)->arg;
					if (equal(node, indexkey))
					{
						/*
						 * Found a match ... is it a unique index? Tests here
						 * should match has_unique_index().
						 */
						if (index->unique &&
							index->nkeycolumns == 1 &&
							pos == 0 &&
							(index->indpred == NIL || index->predOK))
							vardata->isunique = true;

						/*
						 * Has it got stats?  We only consider stats for
						 * non-partial indexes, since partial indexes probably
						 * don't reflect whole-relation statistics; the above
						 * check for uniqueness is the only info we take from
						 * a partial index.
						 *
						 * An index stats hook, however, must make its own
						 * decisions about what to do with partial indexes.
						 */
						if (get_index_stats_hook &&
							(*get_index_stats_hook) (root, index->indexoid,
													 pos + 1, vardata))
						{
							/*
							 * The hook took control of acquiring a stats
							 * tuple.  If it did supply a tuple, it'd better
							 * have supplied a freefunc.
							 */
							if (HeapTupleIsValid(vardata->statsTuple) &&
								!vardata->freefunc)
								elog(ERROR, "no function provided to release variable stats with");
						}
						else if (index->indpred == NIL)
						{
							vardata->statsTuple =
								SearchSysCache3(STATRELATTINH,
												ObjectIdGetDatum(index->indexoid),
												Int16GetDatum(pos + 1),
												BoolGetDatum(false));
							vardata->freefunc = ReleaseSysCache;

							if (HeapTupleIsValid(vardata->statsTuple))
							{
								/* Get index's table for permission check */
								RangeTblEntry *rte;

								rte = planner_rt_fetch(index->rel->relid, root);
								Assert(rte->rtekind == RTE_RELATION);

								/*
								 * For simplicity, we insist on the whole
								 * table being selectable, rather than trying
								 * to identify which column(s) the index
								 * depends on.  Also require all rows to be
								 * selectable --- there must be no
								 * securityQuals from security barrier views
								 * or RLS policies.
								 */
								vardata->acl_ok =
									rte->securityQuals == NIL &&
									(pg_class_aclcheck(rte->relid, userid,
													   ACL_SELECT) == ACLCHECK_OK);

								/*
								 * If the user doesn't have permissions to
								 * access an inheritance child relation, check
								 * the permissions of the table actually
								 * mentioned in the query, since most likely
								 * the user does have that permission.  Note
								 * that whole-table select privilege on the
								 * parent doesn't quite guarantee that the
								 * user could read all columns of the child.
								 * But in practice it's unlikely that any
								 * interesting security violation could result
								 * from allowing access to the expression
								 * index's stats, so we allow it anyway.  See
								 * similar code in examine_simple_variable()
								 * for additional comments.
								 */
								if (!vardata->acl_ok &&
									root->append_rel_array != NULL)
								{
									AppendRelInfo *appinfo;
									Index		varno = index->rel->relid;

									appinfo = root->append_rel_array[varno];
									while (appinfo &&
										   planner_rt_fetch(appinfo->parent_relid,
															root)->rtekind == RTE_RELATION)
									{
										varno = appinfo->parent_relid;
										appinfo = root->append_rel_array[varno];
									}
									if (varno != index->rel->relid)
									{
										/* Repeat access check on this rel */
										rte = planner_rt_fetch(varno, root);
										Assert(rte->rtekind == RTE_RELATION);

										vardata->acl_ok =
											rte->securityQuals == NIL &&
											(pg_class_aclcheck(rte->relid,
															   userid,
															   ACL_SELECT) == ACLCHECK_OK);
									}
								}
							}
							else
							{
								/* suppress leakproofness checks later */
								vardata->acl_ok = true;
							}
						}
						if (vardata->statsTuple)
							break;
					}
					indexpr_item = lnext(index->indexprs, indexpr_item);
				}
			}
			if (vardata->statsTuple)
				break;
		}

		/*
		 * Search extended statistics for one with a matching expression.
		 * There might be multiple ones, so just grab the first one. In the
		 * future, we might consider the statistics target (and pick the most
		 * accurate statistics) and maybe some other parameters.
		 */
		foreach(slist, onerel->statlist)
		{
			StatisticExtInfo *info = (StatisticExtInfo *) lfirst(slist);
			RangeTblEntry *rte = planner_rt_fetch(onerel->relid, root);
			ListCell   *expr_item;
			int			pos;

			/*
			 * Stop once we've found statistics for the expression (either
			 * from extended stats, or for an index in the preceding loop).
			 */
			if (vardata->statsTuple)
				break;

			/* skip stats without per-expression stats */
			if (info->kind != STATS_EXT_EXPRESSIONS)
				continue;

			/* skip stats with mismatching stxdinherit value */
			if (info->inherit != rte->inh)
				continue;

			pos = 0;
			foreach(expr_item, info->exprs)
			{
				Node	   *expr = (Node *) lfirst(expr_item);

				Assert(expr);

				/* strip RelabelType before comparing it */
				if (expr && IsA(expr, RelabelType))
					expr = (Node *) ((RelabelType *) expr)->arg;

				/* found a match, see if we can extract pg_statistic row */
				if (equal(node, expr))
				{
					/*
					 * XXX Not sure if we should cache the tuple somewhere.
					 * Now we just create a new copy every time.
					 */
					vardata->statsTuple =
						statext_expressions_load(info->statOid, rte->inh, pos);

					vardata->freefunc = ReleaseDummy;

					/*
					 * For simplicity, we insist on the whole table being
					 * selectable, rather than trying to identify which
					 * column(s) the statistics object depends on.  Also
					 * require all rows to be selectable --- there must be no
					 * securityQuals from security barrier views or RLS
					 * policies.
					 */
					vardata->acl_ok =
						rte->securityQuals == NIL &&
						(pg_class_aclcheck(rte->relid, userid,
										   ACL_SELECT) == ACLCHECK_OK);

					/*
					 * If the user doesn't have permissions to access an
					 * inheritance child relation, check the permissions of
					 * the table actually mentioned in the query, since most
					 * likely the user does have that permission.  Note that
					 * whole-table select privilege on the parent doesn't
					 * quite guarantee that the user could read all columns of
					 * the child. But in practice it's unlikely that any
					 * interesting security violation could result from
					 * allowing access to the expression stats, so we allow it
					 * anyway.  See similar code in examine_simple_variable()
					 * for additional comments.
					 */
					if (!vardata->acl_ok &&
						root->append_rel_array != NULL)
					{
						AppendRelInfo *appinfo;
						Index		varno = onerel->relid;

						appinfo = root->append_rel_array[varno];
						while (appinfo &&
							   planner_rt_fetch(appinfo->parent_relid,
												root)->rtekind == RTE_RELATION)
						{
							varno = appinfo->parent_relid;
							appinfo = root->append_rel_array[varno];
						}
						if (varno != onerel->relid)
						{
							/* Repeat access check on this rel */
							rte = planner_rt_fetch(varno, root);
							Assert(rte->rtekind == RTE_RELATION);

							vardata->acl_ok =
								rte->securityQuals == NIL &&
								(pg_class_aclcheck(rte->relid,
												   userid,
												   ACL_SELECT) == ACLCHECK_OK);
						}
					}

					break;
				}

				pos++;
			}
		}
	}

	bms_free(varnos);
}

/*
 * examine_simple_variable
 *		Handle a simple Var for examine_variable
 *
 * This is split out as a subroutine so that we can recurse to deal with
 * Vars referencing subqueries (either sub-SELECT-in-FROM or CTE style).
 *
 * We already filled in all the fields of *vardata except for the stats tuple.
 */
static void
examine_simple_variable(PlannerInfo *root, Var *var,
						VariableStatData *vardata)
{
	RangeTblEntry *rte = root->simple_rte_array[var->varno];

	Assert(IsA(rte, RangeTblEntry));

	if (get_relation_stats_hook &&
		(*get_relation_stats_hook) (root, rte, var->varattno, vardata))
	{
		/*
		 * The hook took control of acquiring a stats tuple.  If it did supply
		 * a tuple, it'd better have supplied a freefunc.
		 */
		if (HeapTupleIsValid(vardata->statsTuple) &&
			!vardata->freefunc)
			elog(ERROR, "no function provided to release variable stats with");
	}
	else if (rte->rtekind == RTE_RELATION)
	{
		/*
		 * Plain table or parent of an inheritance appendrel, so look up the
		 * column in pg_statistic
		 */
		vardata->statsTuple = SearchSysCache3(STATRELATTINH,
											  ObjectIdGetDatum(rte->relid),
											  Int16GetDatum(var->varattno),
											  BoolGetDatum(rte->inh));
		vardata->freefunc = ReleaseSysCache;

		if (HeapTupleIsValid(vardata->statsTuple))
		{
			RelOptInfo *onerel = find_base_rel_noerr(root, var->varno);
			Oid			userid;

			/*
			 * Check if user has permission to read this column.  We require
			 * all rows to be accessible, so there must be no securityQuals
			 * from security barrier views or RLS policies.
			 *
			 * Normally the Var will have an associated RelOptInfo from which
			 * we can find out which userid to do the check as; but it might
			 * not if it's a RETURNING Var for an INSERT target relation.  In
			 * that case use the RTEPermissionInfo associated with the RTE.
			 */
			if (onerel)
				userid = onerel->userid;
			else
			{
				RTEPermissionInfo *perminfo;

				perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
				userid = perminfo->checkAsUser;
			}
			if (!OidIsValid(userid))
				userid = GetUserId();

			vardata->acl_ok =
				rte->securityQuals == NIL &&
				((pg_class_aclcheck(rte->relid, userid,
									ACL_SELECT) == ACLCHECK_OK) ||
				 (pg_attribute_aclcheck(rte->relid, var->varattno, userid,
										ACL_SELECT) == ACLCHECK_OK));

			/*
			 * If the user doesn't have permissions to access an inheritance
			 * child relation or specifically this attribute, check the
			 * permissions of the table/column actually mentioned in the
			 * query, since most likely the user does have that permission
			 * (else the query will fail at runtime), and if the user can read
			 * the column there then he can get the values of the child table
			 * too.  To do that, we must find out which of the root parent's
			 * attributes the child relation's attribute corresponds to.
			 */
			if (!vardata->acl_ok && var->varattno > 0 &&
				root->append_rel_array != NULL)
			{
				AppendRelInfo *appinfo;
				Index		varno = var->varno;
				int			varattno = var->varattno;
				bool		found = false;

				appinfo = root->append_rel_array[varno];

				/*
				 * Partitions are mapped to their immediate parent, not the
				 * root parent, so must be ready to walk up multiple
				 * AppendRelInfos.  But stop if we hit a parent that is not
				 * RTE_RELATION --- that's a flattened UNION ALL subquery, not
				 * an inheritance parent.
				 */
				while (appinfo &&
					   planner_rt_fetch(appinfo->parent_relid,
										root)->rtekind == RTE_RELATION)
				{
					int			parent_varattno;

					found = false;
					if (varattno <= 0 || varattno > appinfo->num_child_cols)
						break;	/* safety check */
					parent_varattno = appinfo->parent_colnos[varattno - 1];
					if (parent_varattno == 0)
						break;	/* Var is local to child */

					varno = appinfo->parent_relid;
					varattno = parent_varattno;
					found = true;

					/* If the parent is itself a child, continue up. */
					appinfo = root->append_rel_array[varno];
				}

				/*
				 * In rare cases, the Var may be local to the child table, in
				 * which case, we've got to live with having no access to this
				 * column's stats.
				 */
				if (!found)
					return;

				/* Repeat the access check on this parent rel & column */
				rte = planner_rt_fetch(varno, root);
				Assert(rte->rtekind == RTE_RELATION);

				/*
				 * Fine to use the same userid as it's the same in all
				 * relations of a given inheritance tree.
				 */
				vardata->acl_ok =
					rte->securityQuals == NIL &&
					((pg_class_aclcheck(rte->relid, userid,
										ACL_SELECT) == ACLCHECK_OK) ||
					 (pg_attribute_aclcheck(rte->relid, varattno, userid,
											ACL_SELECT) == ACLCHECK_OK));
			}
		}
		else
		{
			/* suppress any possible leakproofness checks later */
			vardata->acl_ok = true;
		}
	}
	else if ((rte->rtekind == RTE_SUBQUERY && !rte->inh) ||
			 (rte->rtekind == RTE_CTE && !rte->self_reference))
	{
		/*
		 * Plain subquery (not one that was converted to an appendrel) or
		 * non-recursive CTE.  In either case, we can try to find out what the
		 * Var refers to within the subquery.  We skip this for appendrel and
		 * recursive-CTE cases because any column stats we did find would
		 * likely not be very relevant.
		 */
		PlannerInfo *subroot;
		Query	   *subquery;
		List	   *subtlist;
		TargetEntry *ste;

		/*
		 * Punt if it's a whole-row var rather than a plain column reference.
		 */
		if (var->varattno == InvalidAttrNumber)
			return;

		/*
		 * Otherwise, find the subquery's planner subroot.
		 */
		if (rte->rtekind == RTE_SUBQUERY)
		{
			RelOptInfo *rel;

			/*
			 * Fetch RelOptInfo for subquery.  Note that we don't change the
			 * rel returned in vardata, since caller expects it to be a rel of
			 * the caller's query level.  Because we might already be
			 * recursing, we can't use that rel pointer either, but have to
			 * look up the Var's rel afresh.
			 */
			rel = find_base_rel(root, var->varno);

			subroot = rel->subroot;
		}
		else
		{
			/* CTE case is more difficult */
			PlannerInfo *cteroot;
			Index		levelsup;
			int			ndx;
			int			plan_id;
			ListCell   *lc;

			/*
			 * Find the referenced CTE, and locate the subroot previously made
			 * for it.
			 */
			levelsup = rte->ctelevelsup;
			cteroot = root;
			while (levelsup-- > 0)
			{
				cteroot = cteroot->parent_root;
				if (!cteroot)	/* shouldn't happen */
					elog(ERROR, "bad levelsup for CTE \"%s\"", rte->ctename);
			}

			/*
			 * Note: cte_plan_ids can be shorter than cteList, if we are still
			 * working on planning the CTEs (ie, this is a side-reference from
			 * another CTE).  So we mustn't use forboth here.
			 */
			ndx = 0;
			foreach(lc, cteroot->parse->cteList)
			{
				CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

				if (strcmp(cte->ctename, rte->ctename) == 0)
					break;
				ndx++;
			}
			if (lc == NULL)		/* shouldn't happen */
				elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
			if (ndx >= list_length(cteroot->cte_plan_ids))
				elog(ERROR, "could not find plan for CTE \"%s\"", rte->ctename);
			plan_id = list_nth_int(cteroot->cte_plan_ids, ndx);
			if (plan_id <= 0)
				elog(ERROR, "no plan was made for CTE \"%s\"", rte->ctename);
			subroot = list_nth(root->glob->subroots, plan_id - 1);
		}

		/* If the subquery hasn't been planned yet, we have to punt */
		if (subroot == NULL)
			return;
		Assert(IsA(subroot, PlannerInfo));

		/*
		 * We must use the subquery parsetree as mangled by the planner, not
		 * the raw version from the RTE, because we need a Var that will refer
		 * to the subroot's live RelOptInfos.  For instance, if any subquery
		 * pullup happened during planning, Vars in the targetlist might have
		 * gotten replaced, and we need to see the replacement expressions.
		 */
		subquery = subroot->parse;
		Assert(IsA(subquery, Query));

		/*
		 * Punt if subquery uses set operations or grouping sets, as these
		 * will mash underlying columns' stats beyond recognition.  (Set ops
		 * are particularly nasty; if we forged ahead, we would return stats
		 * relevant to only the leftmost subselect...)	DISTINCT is also
		 * problematic, but we check that later because there is a possibility
		 * of learning something even with it.
		 */
		if (subquery->setOperations ||
			subquery->groupingSets)
			return;

		/* Get the subquery output expression referenced by the upper Var */
		if (subquery->returningList)
			subtlist = subquery->returningList;
		else
			subtlist = subquery->targetList;
		ste = get_tle_by_resno(subtlist, var->varattno);
		if (ste == NULL || ste->resjunk)
			elog(ERROR, "subquery %s does not have attribute %d",
				 rte->eref->aliasname, var->varattno);
		var = (Var *) ste->expr;

		/*
		 * If subquery uses DISTINCT, we can't make use of any stats for the
		 * variable ... but, if it's the only DISTINCT column, we are entitled
		 * to consider it unique.  We do the test this way so that it works
		 * for cases involving DISTINCT ON.
		 */
		if (subquery->distinctClause)
		{
			if (list_length(subquery->distinctClause) == 1 &&
				targetIsInSortList(ste, InvalidOid, subquery->distinctClause))
				vardata->isunique = true;
			/* cannot go further */
			return;
		}

		/* The same idea as with DISTINCT clause works for a GROUP-BY too */
		if (subquery->groupClause)
		{
			if (list_length(subquery->groupClause) == 1 &&
				targetIsInSortList(ste, InvalidOid, subquery->groupClause))
				vardata->isunique = true;
			/* cannot go further */
			return;
		}

		/*
		 * If the sub-query originated from a view with the security_barrier
		 * attribute, we must not look at the variable's statistics, though it
		 * seems all right to notice the existence of a DISTINCT clause. So
		 * stop here.
		 *
		 * This is probably a harsher restriction than necessary; it's
		 * certainly OK for the selectivity estimator (which is a C function,
		 * and therefore omnipotent anyway) to look at the statistics.  But
		 * many selectivity estimators will happily *invoke the operator
		 * function* to try to work out a good estimate - and that's not OK.
		 * So for now, don't dig down for stats.
		 */
		if (rte->security_barrier)
			return;

		/* Can only handle a simple Var of subquery's query level */
		if (var && IsA(var, Var) &&
			var->varlevelsup == 0)
		{
			/*
			 * OK, recurse into the subquery.  Note that the original setting
			 * of vardata->isunique (which will surely be false) is left
			 * unchanged in this situation.  That's what we want, since even
			 * if the underlying column is unique, the subquery may have
			 * joined to other tables in a way that creates duplicates.
			 */
			examine_simple_variable(subroot, var, vardata);
		}
	}
	else
	{
		/*
		 * Otherwise, the Var comes from a FUNCTION or VALUES RTE.  (We won't
		 * see RTE_JOIN here because join alias Vars have already been
		 * flattened.)	There's not much we can do with function outputs, but
		 * maybe someday try to be smarter about VALUES.
		 */
	}
}

/*
 * examine_indexcol_variable
 *		Try to look up statistical data about an index column/expression.
 *		Fill in a VariableStatData struct to describe the column.
 *
 * Inputs:
 *	root: the planner info
 *	index: the index whose column we're interested in
 *	indexcol: 0-based index column number (subscripts index->indexkeys[])
 *
 * Outputs: *vardata is filled as follows:
 *	var: the input expression (with any binary relabeling stripped, if
 *		it is or contains a variable; but otherwise the type is preserved)
 *	rel: RelOptInfo for table relation containing variable.
 *	statsTuple: the pg_statistic entry for the variable, if one exists;
 *		otherwise NULL.
 *	freefunc: pointer to a function to release statsTuple with.
 *
 * Caller is responsible for doing ReleaseVariableStats() before exiting.
 */
static void
examine_indexcol_variable(PlannerInfo *root, IndexOptInfo *index,
						  int indexcol, VariableStatData *vardata)
{
	AttrNumber	colnum;
	Oid			relid;

	if (index->indexkeys[indexcol] != 0)
	{
		/* Simple variable --- look to stats for the underlying table */
		RangeTblEntry *rte = planner_rt_fetch(index->rel->relid, root);

		Assert(rte->rtekind == RTE_RELATION);
		relid = rte->relid;
		Assert(relid != InvalidOid);
		colnum = index->indexkeys[indexcol];
		vardata->rel = index->rel;

		if (get_relation_stats_hook &&
			(*get_relation_stats_hook) (root, rte, colnum, vardata))
		{
			/*
			 * The hook took control of acquiring a stats tuple.  If it did
			 * supply a tuple, it'd better have supplied a freefunc.
			 */
			if (HeapTupleIsValid(vardata->statsTuple) &&
				!vardata->freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata->statsTuple = SearchSysCache3(STATRELATTINH,
												  ObjectIdGetDatum(relid),
												  Int16GetDatum(colnum),
												  BoolGetDatum(rte->inh));
			vardata->freefunc = ReleaseSysCache;
		}
	}
	else
	{
		/* Expression --- maybe there are stats for the index itself */
		relid = index->indexoid;
		colnum = indexcol + 1;

		if (get_index_stats_hook &&
			(*get_index_stats_hook) (root, relid, colnum, vardata))
		{
			/*
			 * The hook took control of acquiring a stats tuple.  If it did
			 * supply a tuple, it'd better have supplied a freefunc.
			 */
			if (HeapTupleIsValid(vardata->statsTuple) &&
				!vardata->freefunc)
				elog(ERROR, "no function provided to release variable stats with");
		}
		else
		{
			vardata->statsTuple = SearchSysCache3(STATRELATTINH,
												  ObjectIdGetDatum(relid),
												  Int16GetDatum(colnum),
												  BoolGetDatum(false));
			vardata->freefunc = ReleaseSysCache;
		}
	}
}

/*
 * Check whether it is permitted to call func_oid passing some of the
 * pg_statistic data in vardata.  We allow this either if the user has SELECT
 * privileges on the table or column underlying the pg_statistic data or if
 * the function is marked leakproof.
 */
bool
statistic_proc_security_check(VariableStatData *vardata, Oid func_oid)
{
	if (vardata->acl_ok)
		return true;

	if (!OidIsValid(func_oid))
		return false;

	if (get_func_leakproof(func_oid))
		return true;

	ereport(DEBUG2,
			(errmsg_internal("not using statistics because function \"%s\" is not leakproof",
							 get_func_name(func_oid))));
	return false;
}

/*
 * get_variable_numdistinct
 *	  Estimate the number of distinct values of a variable.
 *
 * vardata: results of examine_variable
 * *isdefault: set to true if the result is a default rather than based on
 * anything meaningful.
 *
 * NB: be careful to produce a positive integral result, since callers may
 * compare the result to exact integer counts, or might divide by it.
 */
double
get_variable_numdistinct(VariableStatData *vardata, bool *isdefault)
{
	double		stadistinct;
	double		stanullfrac = 0.0;
	double		ntuples;

	*isdefault = false;

	/*
	 * Determine the stadistinct value to use.  There are cases where we can
	 * get an estimate even without a pg_statistic entry, or can get a better
	 * value than is in pg_statistic.  Grab stanullfrac too if we can find it
	 * (otherwise, assume no nulls, for lack of any better idea).
	 */
	if (HeapTupleIsValid(vardata->statsTuple))
	{
		/* Use the pg_statistic entry */
		Form_pg_statistic stats;

		stats = (Form_pg_statistic) GETSTRUCT(vardata->statsTuple);
		stadistinct = stats->stadistinct;
		stanullfrac = stats->stanullfrac;
	}
	else if (vardata->vartype == BOOLOID)
	{
		/*
		 * Special-case boolean columns: presumably, two distinct values.
		 *
		 * Are there any other datatypes we should wire in special estimates
		 * for?
		 */
		stadistinct = 2.0;
	}
	else if (vardata->rel && vardata->rel->rtekind == RTE_VALUES)
	{
		/*
		 * If the Var represents a column of a VALUES RTE, assume it's unique.
		 * This could of course be very wrong, but it should tend to be true
		 * in well-written queries.  We could consider examining the VALUES'
		 * contents to get some real statistics; but that only works if the
		 * entries are all constants, and it would be pretty expensive anyway.
		 */
		stadistinct = -1.0;		/* unique (and all non null) */
	}
	else
	{
		/*
		 * We don't keep statistics for system columns, but in some cases we
		 * can infer distinctness anyway.
		 */
		if (vardata->var && IsA(vardata->var, Var))
		{
			switch (((Var *) vardata->var)->varattno)
			{
				case SelfItemPointerAttributeNumber:
					stadistinct = -1.0; /* unique (and all non null) */
					break;
				case TableOidAttributeNumber:
					stadistinct = 1.0;	/* only 1 value */
					break;
				default:
					stadistinct = 0.0;	/* means "unknown" */
					break;
			}
		}
		else
			stadistinct = 0.0;	/* means "unknown" */

		/*
		 * XXX consider using estimate_num_groups on expressions?
		 */
	}

	/*
	 * If there is a unique index, DISTINCT or GROUP-BY clause for the
	 * variable, assume it is unique no matter what pg_statistic says; the
	 * statistics could be out of date, or we might have found a partial
	 * unique index that proves the var is unique for this query.  However,
	 * we'd better still believe the null-fraction statistic.
	 */
	if (vardata->isunique)
		stadistinct = -1.0 * (1.0 - stanullfrac);

	/*
	 * If we had an absolute estimate, use that.
	 */
	if (stadistinct > 0.0)
		return clamp_row_est(stadistinct);

	/*
	 * Otherwise we need to get the relation size; punt if not available.
	 */
	if (vardata->rel == NULL)
	{
		*isdefault = true;
		return DEFAULT_NUM_DISTINCT;
	}
	ntuples = vardata->rel->tuples;
	if (ntuples <= 0.0)
	{
		*isdefault = true;
		return DEFAULT_NUM_DISTINCT;
	}

	/*
	 * If we had a relative estimate, use that.
	 */
	if (stadistinct < 0.0)
		return clamp_row_est(-stadistinct * ntuples);

	/*
	 * With no data, estimate ndistinct = ntuples if the table is small, else
	 * use default.  We use DEFAULT_NUM_DISTINCT as the cutoff for "small" so
	 * that the behavior isn't discontinuous.
	 */
	if (ntuples < DEFAULT_NUM_DISTINCT)
		return clamp_row_est(ntuples);

	*isdefault = true;
	return DEFAULT_NUM_DISTINCT;
}

/*
 * get_variable_range
 *		Estimate the minimum and maximum value of the specified variable.
 *		If successful, store values in *min and *max, and return true.
 *		If no data available, return false.
 *
 * sortop is the "<" comparison operator to use.  This should generally
 * be "<" not ">", as only the former is likely to be found in pg_statistic.
 * The collation must be specified too.
 */
static bool
get_variable_range(PlannerInfo *root, VariableStatData *vardata,
				   Oid sortop, Oid collation,
				   Datum *min, Datum *max)
{
	Datum		tmin = 0;
	Datum		tmax = 0;
	bool		have_data = false;
	int16		typLen;
	bool		typByVal;
	Oid			opfuncoid;
	FmgrInfo	opproc;
	AttStatsSlot sslot;

	/*
	 * XXX It's very tempting to try to use the actual column min and max, if
	 * we can get them relatively-cheaply with an index probe.  However, since
	 * this function is called many times during join planning, that could
	 * have unpleasant effects on planning speed.  Need more investigation
	 * before enabling this.
	 */
#ifdef NOT_USED
	if (get_actual_variable_range(root, vardata, sortop, collation, min, max))
		return true;
#endif

	if (!HeapTupleIsValid(vardata->statsTuple))
	{
		/* no stats available, so default result */
		return false;
	}

	/*
	 * If we can't apply the sortop to the stats data, just fail.  In
	 * principle, if there's a histogram and no MCVs, we could return the
	 * histogram endpoints without ever applying the sortop ... but it's
	 * probably not worth trying, because whatever the caller wants to do with
	 * the endpoints would likely fail the security check too.
	 */
	if (!statistic_proc_security_check(vardata,
									   (opfuncoid = get_opcode(sortop))))
		return false;

	opproc.fn_oid = InvalidOid; /* mark this as not looked up yet */

	get_typlenbyval(vardata->atttype, &typLen, &typByVal);

	/*
	 * If there is a histogram with the ordering we want, grab the first and
	 * last values.
	 */
	if (get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, sortop,
						 ATTSTATSSLOT_VALUES))
	{
		if (sslot.stacoll == collation && sslot.nvalues > 0)
		{
			tmin = datumCopy(sslot.values[0], typByVal, typLen);
			tmax = datumCopy(sslot.values[sslot.nvalues - 1], typByVal, typLen);
			have_data = true;
		}
		free_attstatsslot(&sslot);
	}

	/*
	 * Otherwise, if there is a histogram with some other ordering, scan it
	 * and get the min and max values according to the ordering we want.  This
	 * of course may not find values that are really extremal according to our
	 * ordering, but it beats ignoring available data.
	 */
	if (!have_data &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_HISTOGRAM, InvalidOid,
						 ATTSTATSSLOT_VALUES))
	{
		get_stats_slot_range(&sslot, opfuncoid, &opproc,
							 collation, typLen, typByVal,
							 &tmin, &tmax, &have_data);
		free_attstatsslot(&sslot);
	}

	/*
	 * If we have most-common-values info, look for extreme MCVs.  This is
	 * needed even if we also have a histogram, since the histogram excludes
	 * the MCVs.  However, if we *only* have MCVs and no histogram, we should
	 * be pretty wary of deciding that that is a full representation of the
	 * data.  Proceed only if the MCVs represent the whole table (to within
	 * roundoff error).
	 */
	if (get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_MCV, InvalidOid,
						 have_data ? ATTSTATSSLOT_VALUES :
						 (ATTSTATSSLOT_VALUES | ATTSTATSSLOT_NUMBERS)))
	{
		bool		use_mcvs = have_data;

		if (!have_data)
		{
			double		sumcommon = 0.0;
			double		nullfrac;
			int			i;

			for (i = 0; i < sslot.nnumbers; i++)
				sumcommon += sslot.numbers[i];
			nullfrac = ((Form_pg_statistic) GETSTRUCT(vardata->statsTuple))->stanullfrac;
			if (sumcommon + nullfrac > 0.99999)
				use_mcvs = true;
		}

		if (use_mcvs)
			get_stats_slot_range(&sslot, opfuncoid, &opproc,
								 collation, typLen, typByVal,
								 &tmin, &tmax, &have_data);
		free_attstatsslot(&sslot);
	}

	*min = tmin;
	*max = tmax;
	return have_data;
}

/*
 * get_stats_slot_range: scan sslot for min/max values
 *
 * Subroutine for get_variable_range: update min/max/have_data according
 * to what we find in the statistics array.
 */
static void
get_stats_slot_range(AttStatsSlot *sslot, Oid opfuncoid, FmgrInfo *opproc,
					 Oid collation, int16 typLen, bool typByVal,
					 Datum *min, Datum *max, bool *p_have_data)
{
	Datum		tmin = *min;
	Datum		tmax = *max;
	bool		have_data = *p_have_data;
	bool		found_tmin = false;
	bool		found_tmax = false;

	/* Look up the comparison function, if we didn't already do so */
	if (opproc->fn_oid != opfuncoid)
		fmgr_info(opfuncoid, opproc);

	/* Scan all the slot's values */
	for (int i = 0; i < sslot->nvalues; i++)
	{
		if (!have_data)
		{
			tmin = tmax = sslot->values[i];
			found_tmin = found_tmax = true;
			*p_have_data = have_data = true;
			continue;
		}
		if (DatumGetBool(FunctionCall2Coll(opproc,
										   collation,
										   sslot->values[i], tmin)))
		{
			tmin = sslot->values[i];
			found_tmin = true;
		}
		if (DatumGetBool(FunctionCall2Coll(opproc,
										   collation,
										   tmax, sslot->values[i])))
		{
			tmax = sslot->values[i];
			found_tmax = true;
		}
	}

	/*
	 * Copy the slot's values, if we found new extreme values.
	 */
	if (found_tmin)
		*min = datumCopy(tmin, typByVal, typLen);
	if (found_tmax)
		*max = datumCopy(tmax, typByVal, typLen);
}


/*
 * get_actual_variable_range
 *		Attempt to identify the current *actual* minimum and/or maximum
 *		of the specified variable, by looking for a suitable btree index
 *		and fetching its low and/or high values.
 *		If successful, store values in *min and *max, and return true.
 *		(Either pointer can be NULL if that endpoint isn't needed.)
 *		If unsuccessful, return false.
 *
 * sortop is the "<" comparison operator to use.
 * collation is the required collation.
 */
static bool
get_actual_variable_range(PlannerInfo *root, VariableStatData *vardata,
						  Oid sortop, Oid collation,
						  Datum *min, Datum *max)
{
	bool		have_data = false;
	RelOptInfo *rel = vardata->rel;
	RangeTblEntry *rte;
	ListCell   *lc;

	/* No hope if no relation or it doesn't have indexes */
	if (rel == NULL || rel->indexlist == NIL)
		return false;
	/* If it has indexes it must be a plain relation */
	rte = root->simple_rte_array[rel->relid];
	Assert(rte->rtekind == RTE_RELATION);

	/* ignore partitioned tables.  Any indexes here are not real indexes */
	if (rte->relkind == RELKIND_PARTITIONED_TABLE)
		return false;

	/* Search through the indexes to see if any match our problem */
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *index = (IndexOptInfo *) lfirst(lc);
		ScanDirection indexscandir;
		StrategyNumber strategy;

		/* Ignore non-ordering indexes */
		if (index->sortopfamily == NULL)
			continue;

		/*
		 * Ignore partial indexes --- we only want stats that cover the entire
		 * relation.
		 */
		if (index->indpred != NIL)
			continue;

		/*
		 * The index list might include hypothetical indexes inserted by a
		 * get_relation_info hook --- don't try to access them.
		 */
		if (index->hypothetical)
			continue;

		/*
		 * The first index column must match the desired variable, sortop, and
		 * collation --- but we can use a descending-order index.
		 */
		if (collation != index->indexcollations[0])
			continue;			/* test first 'cause it's cheapest */
		if (!match_index_to_operand(vardata->var, 0, index))
			continue;
		strategy = get_op_opfamily_strategy(sortop, index->sortopfamily[0]);
		switch (IndexAmTranslateStrategy(strategy, index->relam, index->sortopfamily[0], true))
		{
			case COMPARE_LT:
				if (index->reverse_sort[0])
					indexscandir = BackwardScanDirection;
				else
					indexscandir = ForwardScanDirection;
				break;
			case COMPARE_GT:
				if (index->reverse_sort[0])
					indexscandir = ForwardScanDirection;
				else
					indexscandir = BackwardScanDirection;
				break;
			default:
				/* index doesn't match the sortop */
				continue;
		}

		/*
		 * Found a suitable index to extract data from.  Set up some data that
		 * can be used by both invocations of get_actual_variable_endpoint.
		 */
		{
			MemoryContext tmpcontext;
			MemoryContext oldcontext;
			Relation	heapRel;
			Relation	indexRel;
			TupleTableSlot *slot;
			int16		typLen;
			bool		typByVal;
			ScanKeyData scankeys[1];

			/* Make sure any cruft gets recycled when we're done */
			tmpcontext = AllocSetContextCreate(CurrentMemoryContext,
											   "get_actual_variable_range workspace",
											   ALLOCSET_DEFAULT_SIZES);
			oldcontext = MemoryContextSwitchTo(tmpcontext);

			/*
			 * Open the table and index so we can read from them.  We should
			 * already have some type of lock on each.
			 */
			heapRel = table_open(rte->relid, NoLock);
			indexRel = index_open(index->indexoid, NoLock);

			/* build some stuff needed for indexscan execution */
			slot = table_slot_create(heapRel, NULL);
			get_typlenbyval(vardata->atttype, &typLen, &typByVal);

			/* set up an IS NOT NULL scan key so that we ignore nulls */
			ScanKeyEntryInitialize(&scankeys[0],
								   SK_ISNULL | SK_SEARCHNOTNULL,
								   1,	/* index col to scan */
								   InvalidStrategy, /* no strategy */
								   InvalidOid,	/* no strategy subtype */
								   InvalidOid,	/* no collation */
								   InvalidOid,	/* no reg proc for this */
								   (Datum) 0);	/* constant */

			/* If min is requested ... */
			if (min)
			{
				have_data = get_actual_variable_endpoint(heapRel,
														 indexRel,
														 indexscandir,
														 scankeys,
														 typLen,
														 typByVal,
														 slot,
														 oldcontext,
														 min);
			}
			else
			{
				/* If min not requested, still want to fetch max */
				have_data = true;
			}

			/* If max is requested, and we didn't already fail ... */
			if (max && have_data)
			{
				/* scan in the opposite direction; all else is the same */
				have_data = get_actual_variable_endpoint(heapRel,
														 indexRel,
														 -indexscandir,
														 scankeys,
														 typLen,
														 typByVal,
														 slot,
														 oldcontext,
														 max);
			}

			/* Clean everything up */
			ExecDropSingleTupleTableSlot(slot);

			index_close(indexRel, NoLock);
			table_close(heapRel, NoLock);

			MemoryContextSwitchTo(oldcontext);
			MemoryContextDelete(tmpcontext);

			/* And we're done */
			break;
		}
	}

	return have_data;
}

/*
 * Get one endpoint datum (min or max depending on indexscandir) from the
 * specified index.  Return true if successful, false if not.
 * On success, endpoint value is stored to *endpointDatum (and copied into
 * outercontext).
 *
 * scankeys is a 1-element scankey array set up to reject nulls.
 * typLen/typByVal describe the datatype of the index's first column.
 * tableslot is a slot suitable to hold table tuples, in case we need
 * to probe the heap.
 * (We could compute these values locally, but that would mean computing them
 * twice when get_actual_variable_range needs both the min and the max.)
 *
 * Failure occurs either when the index is empty, or we decide that it's
 * taking too long to find a suitable tuple.
 */
static bool
get_actual_variable_endpoint(Relation heapRel,
							 Relation indexRel,
							 ScanDirection indexscandir,
							 ScanKey scankeys,
							 int16 typLen,
							 bool typByVal,
							 TupleTableSlot *tableslot,
							 MemoryContext outercontext,
							 Datum *endpointDatum)
{
	bool		have_data = false;
	SnapshotData SnapshotNonVacuumable;
	IndexScanDesc index_scan;
	Buffer		vmbuffer = InvalidBuffer;
	BlockNumber last_heap_block = InvalidBlockNumber;
	int			n_visited_heap_pages = 0;
	ItemPointer tid;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
	MemoryContext oldcontext;

	/*
	 * We use the index-only-scan machinery for this.  With mostly-static
	 * tables that's a win because it avoids a heap visit.  It's also a win
	 * for dynamic data, but the reason is less obvious; read on for details.
	 *
	 * In principle, we should scan the index with our current active
	 * snapshot, which is the best approximation we've got to what the query
	 * will see when executed.  But that won't be exact if a new snap is taken
	 * before running the query, and it can be very expensive if a lot of
	 * recently-dead or uncommitted rows exist at the beginning or end of the
	 * index (because we'll laboriously fetch each one and reject it).
	 * Instead, we use SnapshotNonVacuumable.  That will accept recently-dead
	 * and uncommitted rows as well as normal visible rows.  On the other
	 * hand, it will reject known-dead rows, and thus not give a bogus answer
	 * when the extreme value has been deleted (unless the deletion was quite
	 * recent); that case motivates not using SnapshotAny here.
	 *
	 * A crucial point here is that SnapshotNonVacuumable, with
	 * GlobalVisTestFor(heapRel) as horizon, yields the inverse of the
	 * condition that the indexscan will use to decide that index entries are
	 * killable (see heap_hot_search_buffer()).  Therefore, if the snapshot
	 * rejects a tuple (or more precisely, all tuples of a HOT chain) and we
	 * have to continue scanning past it, we know that the indexscan will mark
	 * that index entry killed.  That means that the next
	 * get_actual_variable_endpoint() call will not have to re-consider that
	 * index entry.  In this way we avoid repetitive work when this function
	 * is used a lot during planning.
	 *
	 * But using SnapshotNonVacuumable creates a hazard of its own.  In a
	 * recently-created index, some index entries may point at "broken" HOT
	 * chains in which not all the tuple versions contain data matching the
	 * index entry.  The live tuple version(s) certainly do match the index,
	 * but SnapshotNonVacuumable can accept recently-dead tuple versions that
	 * don't match.  Hence, if we took data from the selected heap tuple, we
	 * might get a bogus answer that's not close to the index extremal value,
	 * or could even be NULL.  We avoid this hazard because we take the data
	 * from the index entry not the heap.
	 *
	 * Despite all this care, there are situations where we might find many
	 * non-visible tuples near the end of the index.  We don't want to expend
	 * a huge amount of time here, so we give up once we've read too many heap
	 * pages.  When we fail for that reason, the caller will end up using
	 * whatever extremal value is recorded in pg_statistic.
	 */
	InitNonVacuumableSnapshot(SnapshotNonVacuumable,
							  GlobalVisTestFor(heapRel));

	index_scan = index_beginscan(heapRel, indexRel,
								 &SnapshotNonVacuumable, NULL,
								 1, 0);
	/* Set it up for index-only scan */
	index_scan->xs_want_itup = true;
	index_rescan(index_scan, scankeys, 1, NULL, 0);

	/* Fetch first/next tuple in specified direction */
	while ((tid = index_getnext_tid(index_scan, indexscandir)) != NULL)
	{
		BlockNumber block = ItemPointerGetBlockNumber(tid);

		if (!VM_ALL_VISIBLE(heapRel,
							block,
							&vmbuffer))
		{
			/* Rats, we have to visit the heap to check visibility */
			if (!index_fetch_heap(index_scan, tableslot))
			{
				/*
				 * No visible tuple for this index entry, so we need to
				 * advance to the next entry.  Before doing so, count heap
				 * page fetches and give up if we've done too many.
				 *
				 * We don't charge a page fetch if this is the same heap page
				 * as the previous tuple.  This is on the conservative side,
				 * since other recently-accessed pages are probably still in
				 * buffers too; but it's good enough for this heuristic.
				 */
#define VISITED_PAGES_LIMIT 100

				if (block != last_heap_block)
				{
					last_heap_block = block;
					n_visited_heap_pages++;
					if (n_visited_heap_pages > VISITED_PAGES_LIMIT)
						break;
				}

				continue;		/* no visible tuple, try next index entry */
			}

			/* We don't actually need the heap tuple for anything */
			ExecClearTuple(tableslot);

			/*
			 * We don't care whether there's more than one visible tuple in
			 * the HOT chain; if any are visible, that's good enough.
			 */
		}

		/*
		 * We expect that the index will return data in IndexTuple not
		 * HeapTuple format.
		 */
		if (!index_scan->xs_itup)
			elog(ERROR, "no data returned for index-only scan");

		/*
		 * We do not yet support recheck here.
		 */
		if (index_scan->xs_recheck)
			break;

		/* OK to deconstruct the index tuple */
		index_deform_tuple(index_scan->xs_itup,
						   index_scan->xs_itupdesc,
						   values, isnull);

		/* Shouldn't have got a null, but be careful */
		if (isnull[0])
			elog(ERROR, "found unexpected null value in index \"%s\"",
				 RelationGetRelationName(indexRel));

		/* Copy the index column value out to caller's context */
		oldcontext = MemoryContextSwitchTo(outercontext);
		*endpointDatum = datumCopy(values[0], typByVal, typLen);
		MemoryContextSwitchTo(oldcontext);
		have_data = true;
		break;
	}

	if (vmbuffer != InvalidBuffer)
		ReleaseBuffer(vmbuffer);
	index_endscan(index_scan);

	return have_data;
}

/*
 * find_join_input_rel
 *		Look up the input relation for a join.
 *
 * We assume that the input relation's RelOptInfo must have been constructed
 * already.
 */
static RelOptInfo *
find_join_input_rel(PlannerInfo *root, Relids relids)
{
	RelOptInfo *rel = NULL;

	if (!bms_is_empty(relids))
	{
		int			relid;

		if (bms_get_singleton_member(relids, &relid))
			rel = find_base_rel(root, relid);
		else
			rel = find_join_rel(root, relids);
	}

	if (rel == NULL)
		elog(ERROR, "could not find RelOptInfo for given relids");

	return rel;
}


/*-------------------------------------------------------------------------
 *
 * Index cost estimation functions
 *
 *-------------------------------------------------------------------------
 */

/*
 * Extract the actual indexquals (as RestrictInfos) from an IndexClause list
 */
List *
get_quals_from_indexclauses(List *indexclauses)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach(lc, indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);

			result = lappend(result, rinfo);
		}
	}
	return result;
}

/*
 * Compute the total evaluation cost of the comparison operands in a list
 * of index qual expressions.  Since we know these will be evaluated just
 * once per scan, there's no need to distinguish startup from per-row cost.
 *
 * This can be used either on the result of get_quals_from_indexclauses(),
 * or directly on an indexorderbys list.  In both cases, we expect that the
 * index key expression is on the left side of binary clauses.
 */
Cost
index_other_operands_eval_cost(PlannerInfo *root, List *indexquals)
{
	Cost		qual_arg_cost = 0;
	ListCell   *lc;

	foreach(lc, indexquals)
	{
		Expr	   *clause = (Expr *) lfirst(lc);
		Node	   *other_operand;
		QualCost	index_qual_cost;

		/*
		 * Index quals will have RestrictInfos, indexorderbys won't.  Look
		 * through RestrictInfo if present.
		 */
		if (IsA(clause, RestrictInfo))
			clause = ((RestrictInfo *) clause)->clause;

		if (IsA(clause, OpExpr))
		{
			OpExpr	   *op = (OpExpr *) clause;

			other_operand = (Node *) lsecond(op->args);
		}
		else if (IsA(clause, RowCompareExpr))
		{
			RowCompareExpr *rc = (RowCompareExpr *) clause;

			other_operand = (Node *) rc->rargs;
		}
		else if (IsA(clause, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;

			other_operand = (Node *) lsecond(saop->args);
		}
		else if (IsA(clause, NullTest))
		{
			other_operand = NULL;
		}
		else
		{
			elog(ERROR, "unsupported indexqual type: %d",
				 (int) nodeTag(clause));
			other_operand = NULL;	/* keep compiler quiet */
		}

		cost_qual_eval_node(&index_qual_cost, other_operand, root);
		qual_arg_cost += index_qual_cost.startup + index_qual_cost.per_tuple;
	}
	return qual_arg_cost;
}

void
genericcostestimate(PlannerInfo *root,
					IndexPath *path,
					double loop_count,
					GenericCosts *costs)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	List	   *indexOrderBys = path->indexorderbys;
	Cost		indexStartupCost;
	Cost		indexTotalCost;
	Selectivity indexSelectivity;
	double		indexCorrelation;
	double		numIndexPages;
	double		numIndexTuples;
	double		spc_random_page_cost;
	double		num_sa_scans;
	double		num_outer_scans;
	double		num_scans;
	double		qual_op_cost;
	double		qual_arg_cost;
	List	   *selectivityQuals;
	ListCell   *l;

	/*
	 * If the index is partial, AND the index predicate with the explicitly
	 * given indexquals to produce a more accurate idea of the index
	 * selectivity.
	 */
	selectivityQuals = add_predicate_to_index_quals(index, indexQuals);

	/*
	 * If caller didn't give us an estimate for ScalarArrayOpExpr index scans,
	 * just assume that the number of index descents is the number of distinct
	 * combinations of array elements from all of the scan's SAOP clauses.
	 */
	num_sa_scans = costs->num_sa_scans;
	if (num_sa_scans < 1)
	{
		num_sa_scans = 1;
		foreach(l, indexQuals)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

			if (IsA(rinfo->clause, ScalarArrayOpExpr))
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) rinfo->clause;
				double		alength = estimate_array_length(root, lsecond(saop->args));

				if (alength > 1)
					num_sa_scans *= alength;
			}
		}
	}

	/* Estimate the fraction of main-table tuples that will be visited */
	indexSelectivity = clauselist_selectivity(root, selectivityQuals,
											  index->rel->relid,
											  JOIN_INNER,
											  NULL);

	/*
	 * If caller didn't give us an estimate, estimate the number of index
	 * tuples that will be visited.  We do it in this rather peculiar-looking
	 * way in order to get the right answer for partial indexes.
	 */
	numIndexTuples = costs->numIndexTuples;
	if (numIndexTuples <= 0.0)
	{
		numIndexTuples = indexSelectivity * index->rel->tuples;

		/*
		 * The above calculation counts all the tuples visited across all
		 * scans induced by ScalarArrayOpExpr nodes.  We want to consider the
		 * average per-indexscan number, so adjust.  This is a handy place to
		 * round to integer, too.  (If caller supplied tuple estimate, it's
		 * responsible for handling these considerations.)
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * We can bound the number of tuples by the index size in any case. Also,
	 * always estimate at least one tuple is touched, even when
	 * indexSelectivity estimate is tiny.
	 */
	if (numIndexTuples > index->tuples)
		numIndexTuples = index->tuples;
	if (numIndexTuples < 1.0)
		numIndexTuples = 1.0;

	/*
	 * Estimate the number of index pages that will be retrieved.
	 *
	 * We use the simplistic method of taking a pro-rata fraction of the total
	 * number of index pages.  In effect, this counts only leaf pages and not
	 * any overhead such as index metapage or upper tree levels.
	 *
	 * In practice access to upper index levels is often nearly free because
	 * those tend to stay in cache under load; moreover, the cost involved is
	 * highly dependent on index type.  We therefore ignore such costs here
	 * and leave it to the caller to add a suitable charge if needed.
	 */
	if (index->pages > 1 && index->tuples > 1)
		numIndexPages = ceil(numIndexTuples * index->pages / index->tuples);
	else
		numIndexPages = 1.0;

	/* fetch estimated page cost for tablespace containing index */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * Now compute the disk access costs.
	 *
	 * The above calculations are all per-index-scan.  However, if we are in a
	 * nestloop inner scan, we can expect the scan to be repeated (with
	 * different search keys) for each row of the outer relation.  Likewise,
	 * ScalarArrayOpExpr quals result in multiple index scans.  This creates
	 * the potential for cache effects to reduce the number of disk page
	 * fetches needed.  We want to estimate the average per-scan I/O cost in
	 * the presence of caching.
	 *
	 * We use the Mackert-Lohman formula (see costsize.c for details) to
	 * estimate the total number of page fetches that occur.  While this
	 * wasn't what it was designed for, it seems a reasonable model anyway.
	 * Note that we are counting pages not tuples anymore, so we take N = T =
	 * index size, as if there were one "tuple" per page.
	 */
	num_outer_scans = loop_count;
	num_scans = num_sa_scans * num_outer_scans;

	if (num_scans > 1)
	{
		double		pages_fetched;

		/* total page fetches ignoring cache effects */
		pages_fetched = numIndexPages * num_scans;

		/* use Mackert and Lohman formula to adjust for cache effects */
		pages_fetched = index_pages_fetched(pages_fetched,
											index->pages,
											(double) index->pages,
											root);

		/*
		 * Now compute the total disk access cost, and then report a pro-rated
		 * share for each outer scan.  (Don't pro-rate for ScalarArrayOpExpr,
		 * since that's internal to the indexscan.)
		 */
		indexTotalCost = (pages_fetched * spc_random_page_cost)
			/ num_outer_scans;
	}
	else
	{
		/*
		 * For a single index scan, we just charge spc_random_page_cost per
		 * page touched.
		 */
		indexTotalCost = numIndexPages * spc_random_page_cost;
	}

	/*
	 * CPU cost: any complex expressions in the indexquals will need to be
	 * evaluated once at the start of the scan to reduce them to runtime keys
	 * to pass to the index AM (see nodeIndexscan.c).  We model the per-tuple
	 * CPU costs as cpu_index_tuple_cost plus one cpu_operator_cost per
	 * indexqual operator.  Because we have numIndexTuples as a per-scan
	 * number, we have to multiply by num_sa_scans to get the correct result
	 * for ScalarArrayOpExpr cases.  Similarly add in costs for any index
	 * ORDER BY expressions.
	 *
	 * Note: this neglects the possible costs of rechecking lossy operators.
	 * Detecting that that might be needed seems more expensive than it's
	 * worth, though, considering all the other inaccuracies here ...
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals) +
		index_other_operands_eval_cost(root, indexOrderBys);
	qual_op_cost = cpu_operator_cost *
		(list_length(indexQuals) + list_length(indexOrderBys));

	indexStartupCost = qual_arg_cost;
	indexTotalCost += qual_arg_cost;
	indexTotalCost += numIndexTuples * num_sa_scans * (cpu_index_tuple_cost + qual_op_cost);

	/*
	 * Generic assumption about index correlation: there isn't any.
	 */
	indexCorrelation = 0.0;

	/*
	 * Return everything to caller.
	 */
	costs->indexStartupCost = indexStartupCost;
	costs->indexTotalCost = indexTotalCost;
	costs->indexSelectivity = indexSelectivity;
	costs->indexCorrelation = indexCorrelation;
	costs->numIndexPages = numIndexPages;
	costs->numIndexTuples = numIndexTuples;
	costs->spc_random_page_cost = spc_random_page_cost;
	costs->num_sa_scans = num_sa_scans;
}

/*
 * If the index is partial, add its predicate to the given qual list.
 *
 * ANDing the index predicate with the explicitly given indexquals produces
 * a more accurate idea of the index's selectivity.  However, we need to be
 * careful not to insert redundant clauses, because clauselist_selectivity()
 * is easily fooled into computing a too-low selectivity estimate.  Our
 * approach is to add only the predicate clause(s) that cannot be proven to
 * be implied by the given indexquals.  This successfully handles cases such
 * as a qual "x = 42" used with a partial index "WHERE x >= 40 AND x < 50".
 * There are many other cases where we won't detect redundancy, leading to a
 * too-low selectivity estimate, which will bias the system in favor of using
 * partial indexes where possible.  That is not necessarily bad though.
 *
 * Note that indexQuals contains RestrictInfo nodes while the indpred
 * does not, so the output list will be mixed.  This is OK for both
 * predicate_implied_by() and clauselist_selectivity(), but might be
 * problematic if the result were passed to other things.
 */
List *
add_predicate_to_index_quals(IndexOptInfo *index, List *indexQuals)
{
	List	   *predExtraQuals = NIL;
	ListCell   *lc;

	if (index->indpred == NIL)
		return indexQuals;

	foreach(lc, index->indpred)
	{
		Node	   *predQual = (Node *) lfirst(lc);
		List	   *oneQual = list_make1(predQual);

		if (!predicate_implied_by(oneQual, indexQuals, false))
			predExtraQuals = list_concat(predExtraQuals, oneQual);
	}
	return list_concat(predExtraQuals, indexQuals);
}

/*
 * Estimate correlation of btree index's first column.
 *
 * If we can get an estimate of the first column's ordering correlation C
 * from pg_statistic, estimate the index correlation as C for a single-column
 * index, or C * 0.75 for multiple columns.  The idea here is that multiple
 * columns dilute the importance of the first column's ordering, but don't
 * negate it entirely.
 *
 * We already filled in the stats tuple for *vardata when called.
 */
static double
btcost_correlation(IndexOptInfo *index, VariableStatData *vardata)
{
	Oid			sortop;
	AttStatsSlot sslot;
	double		indexCorrelation = 0;

	Assert(HeapTupleIsValid(vardata->statsTuple));

	sortop = get_opfamily_member(index->opfamily[0],
								 index->opcintype[0],
								 index->opcintype[0],
								 BTLessStrategyNumber);
	if (OidIsValid(sortop) &&
		get_attstatsslot(&sslot, vardata->statsTuple,
						 STATISTIC_KIND_CORRELATION, sortop,
						 ATTSTATSSLOT_NUMBERS))
	{
		double		varCorrelation;

		Assert(sslot.nnumbers == 1);
		varCorrelation = sslot.numbers[0];

		if (index->reverse_sort[0])
			varCorrelation = -varCorrelation;

		if (index->nkeycolumns > 1)
			indexCorrelation = varCorrelation * 0.75;
		else
			indexCorrelation = varCorrelation;

		free_attstatsslot(&sslot);
	}

	return indexCorrelation;
}

void
btcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
			   Cost *indexStartupCost, Cost *indexTotalCost,
			   Selectivity *indexSelectivity, double *indexCorrelation,
			   double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	VariableStatData vardata = {0};
	double		numIndexTuples;
	Cost		descentCost;
	List	   *indexBoundQuals;
	List	   *indexSkipQuals;
	int			indexcol;
	bool		eqQualHere;
	bool		found_row_compare;
	bool		found_array;
	bool		found_is_null_op;
	bool		have_correlation = false;
	double		num_sa_scans;
	double		correlation = 0.0;
	ListCell   *lc;

	/*
	 * For a btree scan, only leading '=' quals plus inequality quals for the
	 * immediately next attribute contribute to index selectivity (these are
	 * the "boundary quals" that determine the starting and stopping points of
	 * the index scan).  Additional quals can suppress visits to the heap, so
	 * it's OK to count them in indexSelectivity, but they should not count
	 * for estimating numIndexTuples.  So we must examine the given indexquals
	 * to find out which ones count as boundary quals.  We rely on the
	 * knowledge that they are given in index column order.  Note that nbtree
	 * preprocessing can add skip arrays that act as leading '=' quals in the
	 * absence of ordinary input '=' quals, so in practice _most_ input quals
	 * are able to act as index bound quals (which we take into account here).
	 *
	 * For a RowCompareExpr, we consider only the first column, just as
	 * rowcomparesel() does.
	 *
	 * If there's a SAOP or skip array in the quals, we'll actually perform up
	 * to N index descents (not just one), but the underlying array key's
	 * operator can be considered to act the same as it normally does.
	 */
	indexBoundQuals = NIL;
	indexSkipQuals = NIL;
	indexcol = 0;
	eqQualHere = false;
	found_row_compare = false;
	found_array = false;
	found_is_null_op = false;
	num_sa_scans = 1;
	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		if (indexcol < iclause->indexcol)
		{
			double		num_sa_scans_prev_cols = num_sa_scans;

			/*
			 * Beginning of a new column's quals.
			 *
			 * Skip scans use skip arrays, which are ScalarArrayOp style
			 * arrays that generate their elements procedurally and on demand.
			 * Given a multi-column index on "(a, b)", and an SQL WHERE clause
			 * "WHERE b = 42", a skip scan will effectively use an indexqual
			 * "WHERE a = ANY('{every col a value}') AND b = 42".  (Obviously,
			 * the array on "a" must also return "IS NULL" matches, since our
			 * WHERE clause used no strict operator on "a").
			 *
			 * Here we consider how nbtree will backfill skip arrays for any
			 * index columns that lacked an '=' qual.  This maintains our
			 * num_sa_scans estimate, and determines if this new column (the
			 * "iclause->indexcol" column, not the prior "indexcol" column)
			 * can have its RestrictInfos/quals added to indexBoundQuals.
			 *
			 * We'll need to handle columns that have inequality quals, where
			 * the skip array generates values from a range constrained by the
			 * quals (not every possible value).  We've been maintaining
			 * indexSkipQuals to help with this; it will now contain all of
			 * the prior column's quals (that is, indexcol's quals) when they
			 * might be used for this.
			 */
			if (found_row_compare)
			{
				/*
				 * Skip arrays can't be added after a RowCompare input qual
				 * due to limitations in nbtree
				 */
				break;
			}
			if (eqQualHere)
			{
				/*
				 * Don't need to add a skip array for an indexcol that already
				 * has an '=' qual/equality constraint
				 */
				indexcol++;
				indexSkipQuals = NIL;
			}
			eqQualHere = false;

			while (indexcol < iclause->indexcol)
			{
				double		ndistinct;
				bool		isdefault = true;

				found_array = true;

				/*
				 * A skipped attribute's ndistinct forms the basis of our
				 * estimate of the total number of "array elements" used by
				 * its skip array at runtime.  Look that up first.
				 */
				examine_indexcol_variable(root, index, indexcol, &vardata);
				ndistinct = get_variable_numdistinct(&vardata, &isdefault);

				if (indexcol == 0)
				{
					/*
					 * Get an estimate of the leading column's correlation in
					 * passing (avoids rereading variable stats below)
					 */
					if (HeapTupleIsValid(vardata.statsTuple))
						correlation = btcost_correlation(index, &vardata);
					have_correlation = true;
				}

				ReleaseVariableStats(vardata);

				/*
				 * If ndistinct is a default estimate, conservatively assume
				 * that no skipping will happen at runtime
				 */
				if (isdefault)
				{
					num_sa_scans = num_sa_scans_prev_cols;
					break;		/* done building indexBoundQuals */
				}

				/*
				 * Apply indexcol's indexSkipQuals selectivity to ndistinct
				 */
				if (indexSkipQuals != NIL)
				{
					List	   *partialSkipQuals;
					Selectivity ndistinctfrac;

					/*
					 * If the index is partial, AND the index predicate with
					 * the index-bound quals to produce a more accurate idea
					 * of the number of distinct values for prior indexcol
					 */
					partialSkipQuals = add_predicate_to_index_quals(index,
																	indexSkipQuals);

					ndistinctfrac = clauselist_selectivity(root, partialSkipQuals,
														   index->rel->relid,
														   JOIN_INNER,
														   NULL);

					/*
					 * If ndistinctfrac is selective (on its own), the scan is
					 * unlikely to benefit from repositioning itself using
					 * later quals.  Do not allow iclause->indexcol's quals to
					 * be added to indexBoundQuals (it would increase descent
					 * costs, without lowering numIndexTuples costs by much).
					 */
					if (ndistinctfrac < DEFAULT_RANGE_INEQ_SEL)
					{
						num_sa_scans = num_sa_scans_prev_cols;
						break;	/* done building indexBoundQuals */
					}

					/* Adjust ndistinct downward */
					ndistinct = rint(ndistinct * ndistinctfrac);
					ndistinct = Max(ndistinct, 1);
				}

				/*
				 * When there's no inequality quals, account for the need to
				 * find an initial value by counting -inf/+inf as a value.
				 *
				 * We don't charge anything extra for possible next/prior key
				 * index probes, which are sometimes used to find the next
				 * valid skip array element (ahead of using the located
				 * element value to relocate the scan to the next position
				 * that might contain matching tuples).  It seems hard to do
				 * better here.  Use of the skip support infrastructure often
				 * avoids most next/prior key probes.  But even when it can't,
				 * there's a decent chance that most individual next/prior key
				 * probes will locate a leaf page whose key space overlaps all
				 * of the scan's keys (even the lower-order keys) -- which
				 * also avoids the need for a separate, extra index descent.
				 * Note also that these probes are much cheaper than non-probe
				 * primitive index scans: they're reliably very selective.
				 */
				if (indexSkipQuals == NIL)
					ndistinct += 1;

				/*
				 * Update num_sa_scans estimate by multiplying by ndistinct.
				 *
				 * We make the pessimistic assumption that there is no
				 * naturally occurring cross-column correlation.  This is
				 * often wrong, but it seems best to err on the side of not
				 * expecting skipping to be helpful...
				 */
				num_sa_scans *= ndistinct;

				/*
				 * ...but back out of adding this latest group of 1 or more
				 * skip arrays when num_sa_scans exceeds the total number of
				 * index pages (revert to num_sa_scans from before indexcol).
				 * This causes a sharp discontinuity in cost (as a function of
				 * the indexcol's ndistinct), but that is representative of
				 * actual runtime costs.
				 *
				 * Note that skipping is helpful when each primitive index
				 * scan only manages to skip over 1 or 2 irrelevant leaf pages
				 * on average.  Skip arrays bring savings in CPU costs due to
				 * the scan not needing to evaluate indexquals against every
				 * tuple, which can greatly exceed any savings in I/O costs.
				 * This test is a test of whether num_sa_scans implies that
				 * we're past the point where the ability to skip ceases to
				 * lower the scan's costs (even qual evaluation CPU costs).
				 */
				if (index->pages < num_sa_scans)
				{
					num_sa_scans = num_sa_scans_prev_cols;
					break;		/* done building indexBoundQuals */
				}

				indexcol++;
				indexSkipQuals = NIL;
			}

			/*
			 * Finished considering the need to add skip arrays to bridge an
			 * initial eqQualHere gap between the old and new index columns
			 * (or there was no initial eqQualHere gap in the first place).
			 *
			 * If an initial gap could not be bridged, then new column's quals
			 * (i.e. iclause->indexcol's quals) won't go into indexBoundQuals,
			 * and so won't affect our final numIndexTuples estimate.
			 */
			if (indexcol != iclause->indexcol)
				break;			/* done building indexBoundQuals */
		}

		Assert(indexcol == iclause->indexcol);

		/* Examine each indexqual associated with this index clause */
		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Expr	   *clause = rinfo->clause;
			Oid			clause_op = InvalidOid;
			int			op_strategy;

			if (IsA(clause, OpExpr))
			{
				OpExpr	   *op = (OpExpr *) clause;

				clause_op = op->opno;
			}
			else if (IsA(clause, RowCompareExpr))
			{
				RowCompareExpr *rc = (RowCompareExpr *) clause;

				clause_op = linitial_oid(rc->opnos);
				found_row_compare = true;
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
				Node	   *other_operand = (Node *) lsecond(saop->args);
				double		alength = estimate_array_length(root, other_operand);

				clause_op = saop->opno;
				found_array = true;
				/* estimate SA descents by indexBoundQuals only */
				if (alength > 1)
					num_sa_scans *= alength;
			}
			else if (IsA(clause, NullTest))
			{
				NullTest   *nt = (NullTest *) clause;

				if (nt->nulltesttype == IS_NULL)
				{
					found_is_null_op = true;
					/* IS NULL is like = for selectivity/skip scan purposes */
					eqQualHere = true;
				}
			}
			else
				elog(ERROR, "unsupported indexqual type: %d",
					 (int) nodeTag(clause));

			/* check for equality operator */
			if (OidIsValid(clause_op))
			{
				op_strategy = get_op_opfamily_strategy(clause_op,
													   index->opfamily[indexcol]);
				Assert(op_strategy != 0);	/* not a member of opfamily?? */
				if (op_strategy == BTEqualStrategyNumber)
					eqQualHere = true;
			}

			indexBoundQuals = lappend(indexBoundQuals, rinfo);

			/*
			 * We apply inequality selectivities to estimate index descent
			 * costs with scans that use skip arrays.  Save this indexcol's
			 * RestrictInfos if it looks like they'll be needed for that.
			 */
			if (!eqQualHere && !found_row_compare &&
				indexcol < index->nkeycolumns - 1)
				indexSkipQuals = lappend(indexSkipQuals, rinfo);
		}
	}

	/*
	 * If index is unique and we found an '=' clause for each column, we can
	 * just assume numIndexTuples = 1 and skip the expensive
	 * clauselist_selectivity calculations.  However, an array or NullTest
	 * always invalidates that theory (even when eqQualHere has been set).
	 */
	if (index->unique &&
		indexcol == index->nkeycolumns - 1 &&
		eqQualHere &&
		!found_array &&
		!found_is_null_op)
		numIndexTuples = 1.0;
	else
	{
		List	   *selectivityQuals;
		Selectivity btreeSelectivity;

		/*
		 * If the index is partial, AND the index predicate with the
		 * index-bound quals to produce a more accurate idea of the number of
		 * rows covered by the bound conditions.
		 */
		selectivityQuals = add_predicate_to_index_quals(index, indexBoundQuals);

		btreeSelectivity = clauselist_selectivity(root, selectivityQuals,
												  index->rel->relid,
												  JOIN_INNER,
												  NULL);
		numIndexTuples = btreeSelectivity * index->rel->tuples;

		/*
		 * btree automatically combines individual array element primitive
		 * index scans whenever the tuples covered by the next set of array
		 * keys are close to tuples covered by the current set.  That puts a
		 * natural ceiling on the worst case number of descents -- there
		 * cannot possibly be more than one descent per leaf page scanned.
		 *
		 * Clamp the number of descents to at most 1/3 the number of index
		 * pages.  This avoids implausibly high estimates with low selectivity
		 * paths, where scans usually require only one or two descents.  This
		 * is most likely to help when there are several SAOP clauses, where
		 * naively accepting the total number of distinct combinations of
		 * array elements as the number of descents would frequently lead to
		 * wild overestimates.
		 *
		 * We somewhat arbitrarily don't just make the cutoff the total number
		 * of leaf pages (we make it 1/3 the total number of pages instead) to
		 * give the btree code credit for its ability to continue on the leaf
		 * level with low selectivity scans.
		 *
		 * Note: num_sa_scans includes both ScalarArrayOp array elements and
		 * skip array elements whose qual affects our numIndexTuples estimate.
		 */
		num_sa_scans = Min(num_sa_scans, ceil(index->pages * 0.3333333));
		num_sa_scans = Max(num_sa_scans, 1);

		/*
		 * As in genericcostestimate(), we have to adjust for any array quals
		 * included in indexBoundQuals, and then round to integer.
		 *
		 * It is tempting to make genericcostestimate behave as if array
		 * clauses work in almost the same way as scalar operators during
		 * btree scans, making the top-level scan look like a continuous scan
		 * (as opposed to num_sa_scans-many primitive index scans).  After
		 * all, btree scans mostly work like that at runtime.  However, such a
		 * scheme would badly bias genericcostestimate's simplistic approach
		 * to calculating numIndexPages through prorating.
		 *
		 * Stick with the approach taken by non-native SAOP scans for now.
		 * genericcostestimate will use the Mackert-Lohman formula to
		 * compensate for repeat page fetches, even though that definitely
		 * won't happen during btree scans (not for leaf pages, at least).
		 * We're usually very pessimistic about the number of primitive index
		 * scans that will be required, but it's not clear how to do better.
		 */
		numIndexTuples = rint(numIndexTuples / num_sa_scans);
	}

	/*
	 * Now do generic index cost estimation.
	 */
	costs.numIndexTuples = numIndexTuples;
	costs.num_sa_scans = num_sa_scans;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * Add a CPU-cost component to represent the costs of initial btree
	 * descent.  We don't charge any I/O cost for touching upper btree levels,
	 * since they tend to stay in cache, but we still have to do about log2(N)
	 * comparisons to descend a btree of N leaf tuples.  We charge one
	 * cpu_operator_cost per comparison.
	 *
	 * If there are SAOP or skip array keys, charge this once per estimated
	 * index descent.  The ones after the first one are not startup cost so
	 * far as the overall plan goes, so just add them to "total" cost.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples) / log(2.0)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Even though we're not charging I/O cost for touching upper btree pages,
	 * it's still reasonable to charge some CPU cost per page descended
	 * through.  Moreover, if we had no such charge at all, bloated indexes
	 * would appear to have the same search cost as unbloated ones, at least
	 * in cases where only a single leaf page is expected to be visited.  This
	 * cost is somewhat arbitrarily set at 50x cpu_operator_cost per page
	 * touched.  The number of such pages is btree tree height plus one (ie,
	 * we charge for the leaf page too).  As above, charge once per estimated
	 * SAOP/skip array descent.
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	if (!have_correlation)
	{
		examine_indexcol_variable(root, index, 0, &vardata);
		if (HeapTupleIsValid(vardata.statsTuple))
			costs.indexCorrelation = btcost_correlation(index, &vardata);
		ReleaseVariableStats(vardata);
	}
	else
	{
		/* btcost_correlation already called earlier on */
		costs.indexCorrelation = correlation;
	}

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
hashcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	GenericCosts costs = {0};

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * A hash index has no descent costs as such, since the index AM can go
	 * directly to the target bucket after computing the hash value.  There
	 * are a couple of other hash-specific costs that we could conceivably add
	 * here, though:
	 *
	 * Ideally we'd charge spc_random_page_cost for each page in the target
	 * bucket, not just the numIndexPages pages that genericcostestimate
	 * thought we'd visit.  However in most cases we don't know which bucket
	 * that will be.  There's no point in considering the average bucket size
	 * because the hash AM makes sure that's always one page.
	 *
	 * Likewise, we could consider charging some CPU for each index tuple in
	 * the bucket, if we knew how many there were.  But the per-tuple cost is
	 * just a hash value comparison, not a general datatype-dependent
	 * comparison, so any such charge ought to be quite a bit less than
	 * cpu_operator_cost; which makes it probably not worth worrying about.
	 *
	 * A bigger issue is that chance hash-value collisions will result in
	 * wasted probes into the heap.  We don't currently attempt to model this
	 * cost on the grounds that it's rare, but maybe it's not rare enough.
	 * (Any fix for this ought to consider the generic lossy-operator problem,
	 * though; it's not entirely hash-specific.)
	 */

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
gistcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Cost		descentCost;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * We model index descent costs similarly to those for btree, but to do
	 * that we first need an idea of the tree height.  We somewhat arbitrarily
	 * assume that the fanout is 100, meaning the tree height is at most
	 * log100(index->pages).
	 *
	 * Although this computation isn't really expensive enough to require
	 * caching, we might as well use index->tree_height to cache it.
	 */
	if (index->tree_height < 0) /* unknown? */
	{
		if (index->pages > 1)	/* avoid computing log(0) */
			index->tree_height = (int) (log(index->pages) / log(100.0));
		else
			index->tree_height = 0;
	}

	/*
	 * Add a CPU-cost component to represent the costs of initial descent. We
	 * just use log(N) here not log2(N) since the branching factor isn't
	 * necessarily two anyway.  As for btree, charge once per SA scan.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Likewise add a per-page charge, calculated the same as for btrees.
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}

void
spgcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	GenericCosts costs = {0};
	Cost		descentCost;

	genericcostestimate(root, path, loop_count, &costs);

	/*
	 * We model index descent costs similarly to those for btree, but to do
	 * that we first need an idea of the tree height.  We somewhat arbitrarily
	 * assume that the fanout is 100, meaning the tree height is at most
	 * log100(index->pages).
	 *
	 * Although this computation isn't really expensive enough to require
	 * caching, we might as well use index->tree_height to cache it.
	 */
	if (index->tree_height < 0) /* unknown? */
	{
		if (index->pages > 1)	/* avoid computing log(0) */
			index->tree_height = (int) (log(index->pages) / log(100.0));
		else
			index->tree_height = 0;
	}

	/*
	 * Add a CPU-cost component to represent the costs of initial descent. We
	 * just use log(N) here not log2(N) since the branching factor isn't
	 * necessarily two anyway.  As for btree, charge once per SA scan.
	 */
	if (index->tuples > 1)		/* avoid computing log(0) */
	{
		descentCost = ceil(log(index->tuples)) * cpu_operator_cost;
		costs.indexStartupCost += descentCost;
		costs.indexTotalCost += costs.num_sa_scans * descentCost;
	}

	/*
	 * Likewise add a per-page charge, calculated the same as for btrees.
	 */
	descentCost = (index->tree_height + 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	costs.indexStartupCost += descentCost;
	costs.indexTotalCost += costs.num_sa_scans * descentCost;

	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = costs.indexCorrelation;
	*indexPages = costs.numIndexPages;
}


/*
 * Support routines for gincostestimate
 */

typedef struct
{
	bool		attHasFullScan[INDEX_MAX_KEYS];
	bool		attHasNormalScan[INDEX_MAX_KEYS];
	double		partialEntries;
	double		exactEntries;
	double		searchEntries;
	double		arrayScans;
} GinQualCounts;

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN query, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 */
static bool
gincost_pattern(IndexOptInfo *index, int indexcol,
				Oid clause_op, Datum query,
				GinQualCounts *counts)
{
	FmgrInfo	flinfo;
	Oid			extractProcOid;
	Oid			collation;
	int			strategy_op;
	Oid			lefttype,
				righttype;
	int32		nentries = 0;
	bool	   *partial_matches = NULL;
	Pointer    *extra_data = NULL;
	bool	   *nullFlags = NULL;
	int32		searchMode = GIN_SEARCH_MODE_DEFAULT;
	int32		i;

	Assert(indexcol < index->nkeycolumns);

	/*
	 * Get the operator's strategy number and declared input data types within
	 * the index opfamily.  (We don't need the latter, but we use
	 * get_op_opfamily_properties because it will throw error if it fails to
	 * find a matching pg_amop entry.)
	 */
	get_op_opfamily_properties(clause_op, index->opfamily[indexcol], false,
							   &strategy_op, &lefttype, &righttype);

	/*
	 * GIN always uses the "default" support functions, which are those with
	 * lefttype == righttype == the opclass' opcintype (see
	 * IndexSupportInitialize in relcache.c).
	 */
	extractProcOid = get_opfamily_proc(index->opfamily[indexcol],
									   index->opcintype[indexcol],
									   index->opcintype[indexcol],
									   GIN_EXTRACTQUERY_PROC);

	if (!OidIsValid(extractProcOid))
	{
		/* should not happen; throw same error as index_getprocinfo */
		elog(ERROR, "missing support function %d for attribute %d of index \"%s\"",
			 GIN_EXTRACTQUERY_PROC, indexcol + 1,
			 get_rel_name(index->indexoid));
	}

	/*
	 * Choose collation to pass to extractProc (should match initGinState).
	 */
	if (OidIsValid(index->indexcollations[indexcol]))
		collation = index->indexcollations[indexcol];
	else
		collation = DEFAULT_COLLATION_OID;

	fmgr_info(extractProcOid, &flinfo);

	set_fn_opclass_options(&flinfo, index->opclassoptions[indexcol]);

	FunctionCall7Coll(&flinfo,
					  collation,
					  query,
					  PointerGetDatum(&nentries),
					  UInt16GetDatum(strategy_op),
					  PointerGetDatum(&partial_matches),
					  PointerGetDatum(&extra_data),
					  PointerGetDatum(&nullFlags),
					  PointerGetDatum(&searchMode));

	if (nentries <= 0 && searchMode == GIN_SEARCH_MODE_DEFAULT)
	{
		/* No match is possible */
		return false;
	}

	for (i = 0; i < nentries; i++)
	{
		/*
		 * For partial match we haven't any information to estimate number of
		 * matched entries in index, so, we just estimate it as 100
		 */
		if (partial_matches && partial_matches[i])
			counts->partialEntries += 100;
		else
			counts->exactEntries++;

		counts->searchEntries++;
	}

	if (searchMode == GIN_SEARCH_MODE_DEFAULT)
	{
		counts->attHasNormalScan[indexcol] = true;
	}
	else if (searchMode == GIN_SEARCH_MODE_INCLUDE_EMPTY)
	{
		/* Treat "include empty" like an exact-match item */
		counts->attHasNormalScan[indexcol] = true;
		counts->exactEntries++;
		counts->searchEntries++;
	}
	else
	{
		/* It's GIN_SEARCH_MODE_ALL */
		counts->attHasFullScan[indexcol] = true;
	}

	return true;
}

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN index clause, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 */
static bool
gincost_opexpr(PlannerInfo *root,
			   IndexOptInfo *index,
			   int indexcol,
			   OpExpr *clause,
			   GinQualCounts *counts)
{
	Oid			clause_op = clause->opno;
	Node	   *operand = (Node *) lsecond(clause->args);

	/* aggressively reduce to a constant, and look through relabeling */
	operand = estimate_expression_value(root, operand);

	if (IsA(operand, RelabelType))
		operand = (Node *) ((RelabelType *) operand)->arg;

	/*
	 * It's impossible to call extractQuery method for unknown operand. So
	 * unless operand is a Const we can't do much; just assume there will be
	 * one ordinary search entry from the operand at runtime.
	 */
	if (!IsA(operand, Const))
	{
		counts->exactEntries++;
		counts->searchEntries++;
		return true;
	}

	/* If Const is null, there can be no matches */
	if (((Const *) operand)->constisnull)
		return false;

	/* Otherwise, apply extractQuery and get the actual term counts */
	return gincost_pattern(index, indexcol, clause_op,
						   ((Const *) operand)->constvalue,
						   counts);
}

/*
 * Estimate the number of index terms that need to be searched for while
 * testing the given GIN index clause, and increment the counts in *counts
 * appropriately.  If the query is unsatisfiable, return false.
 *
 * A ScalarArrayOpExpr will give rise to N separate indexscans at runtime,
 * each of which involves one value from the RHS array, plus all the
 * non-array quals (if any).  To model this, we average the counts across
 * the RHS elements, and add the averages to the counts in *counts (which
 * correspond to per-indexscan costs).  We also multiply counts->arrayScans
 * by N, causing gincostestimate to scale up its estimates accordingly.
 */
static bool
gincost_scalararrayopexpr(PlannerInfo *root,
						  IndexOptInfo *index,
						  int indexcol,
						  ScalarArrayOpExpr *clause,
						  double numIndexEntries,
						  GinQualCounts *counts)
{
	Oid			clause_op = clause->opno;
	Node	   *rightop = (Node *) lsecond(clause->args);
	ArrayType  *arrayval;
	int16		elmlen;
	bool		elmbyval;
	char		elmalign;
	int			numElems;
	Datum	   *elemValues;
	bool	   *elemNulls;
	GinQualCounts arraycounts;
	int			numPossible = 0;
	int			i;

	Assert(clause->useOr);

	/* aggressively reduce to a constant, and look through relabeling */
	rightop = estimate_expression_value(root, rightop);

	if (IsA(rightop, RelabelType))
		rightop = (Node *) ((RelabelType *) rightop)->arg;

	/*
	 * It's impossible to call extractQuery method for unknown operand. So
	 * unless operand is a Const we can't do much; just assume there will be
	 * one ordinary search entry from each array entry at runtime, and fall
	 * back on a probably-bad estimate of the number of array entries.
	 */
	if (!IsA(rightop, Const))
	{
		counts->exactEntries++;
		counts->searchEntries++;
		counts->arrayScans *= estimate_array_length(root, rightop);
		return true;
	}

	/* If Const is null, there can be no matches */
	if (((Const *) rightop)->constisnull)
		return false;

	/* Otherwise, extract the array elements and iterate over them */
	arrayval = DatumGetArrayTypeP(((Const *) rightop)->constvalue);
	get_typlenbyvalalign(ARR_ELEMTYPE(arrayval),
						 &elmlen, &elmbyval, &elmalign);
	deconstruct_array(arrayval,
					  ARR_ELEMTYPE(arrayval),
					  elmlen, elmbyval, elmalign,
					  &elemValues, &elemNulls, &numElems);

	memset(&arraycounts, 0, sizeof(arraycounts));

	for (i = 0; i < numElems; i++)
	{
		GinQualCounts elemcounts;

		/* NULL can't match anything, so ignore, as the executor will */
		if (elemNulls[i])
			continue;

		/* Otherwise, apply extractQuery and get the actual term counts */
		memset(&elemcounts, 0, sizeof(elemcounts));

		if (gincost_pattern(index, indexcol, clause_op, elemValues[i],
							&elemcounts))
		{
			/* We ignore array elements that are unsatisfiable patterns */
			numPossible++;

			if (elemcounts.attHasFullScan[indexcol] &&
				!elemcounts.attHasNormalScan[indexcol])
			{
				/*
				 * Full index scan will be required.  We treat this as if
				 * every key in the index had been listed in the query; is
				 * that reasonable?
				 */
				elemcounts.partialEntries = 0;
				elemcounts.exactEntries = numIndexEntries;
				elemcounts.searchEntries = numIndexEntries;
			}
			arraycounts.partialEntries += elemcounts.partialEntries;
			arraycounts.exactEntries += elemcounts.exactEntries;
			arraycounts.searchEntries += elemcounts.searchEntries;
		}
	}

	if (numPossible == 0)
	{
		/* No satisfiable patterns in the array */
		return false;
	}

	/*
	 * Now add the averages to the global counts.  This will give us an
	 * estimate of the average number of terms searched for in each indexscan,
	 * including contributions from both array and non-array quals.
	 */
	counts->partialEntries += arraycounts.partialEntries / numPossible;
	counts->exactEntries += arraycounts.exactEntries / numPossible;
	counts->searchEntries += arraycounts.searchEntries / numPossible;

	counts->arrayScans *= numPossible;

	return true;
}

/*
 * GIN has search behavior completely different from other index types
 */
void
gincostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				Cost *indexStartupCost, Cost *indexTotalCost,
				Selectivity *indexSelectivity, double *indexCorrelation,
				double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	List	   *selectivityQuals;
	double		numPages = index->pages,
				numTuples = index->tuples;
	double		numEntryPages,
				numDataPages,
				numPendingPages,
				numEntries;
	GinQualCounts counts;
	bool		matchPossible;
	bool		fullIndexScan;
	double		partialScale;
	double		entryPagesFetched,
				dataPagesFetched,
				dataPagesFetchedBySel;
	double		qual_op_cost,
				qual_arg_cost,
				spc_random_page_cost,
				outer_scans;
	Cost		descentCost;
	Relation	indexRel;
	GinStatsData ginStats;
	ListCell   *lc;
	int			i;

	/*
	 * Obtain statistical information from the meta page, if possible.  Else
	 * set ginStats to zeroes, and we'll cope below.
	 */
	if (!index->hypothetical)
	{
		/* Lock should have already been obtained in plancat.c */
		indexRel = index_open(index->indexoid, NoLock);
		ginGetStats(indexRel, &ginStats);
		index_close(indexRel, NoLock);
	}
	else
	{
		memset(&ginStats, 0, sizeof(ginStats));
	}

	/*
	 * Assuming we got valid (nonzero) stats at all, nPendingPages can be
	 * trusted, but the other fields are data as of the last VACUUM.  We can
	 * scale them up to account for growth since then, but that method only
	 * goes so far; in the worst case, the stats might be for a completely
	 * empty index, and scaling them will produce pretty bogus numbers.
	 * Somewhat arbitrarily, set the cutoff for doing scaling at 4X growth; if
	 * it's grown more than that, fall back to estimating things only from the
	 * assumed-accurate index size.  But we'll trust nPendingPages in any case
	 * so long as it's not clearly insane, ie, more than the index size.
	 */
	if (ginStats.nPendingPages < numPages)
		numPendingPages = ginStats.nPendingPages;
	else
		numPendingPages = 0;

	if (numPages > 0 && ginStats.nTotalPages <= numPages &&
		ginStats.nTotalPages > numPages / 4 &&
		ginStats.nEntryPages > 0 && ginStats.nEntries > 0)
	{
		/*
		 * OK, the stats seem close enough to sane to be trusted.  But we
		 * still need to scale them by the ratio numPages / nTotalPages to
		 * account for growth since the last VACUUM.
		 */
		double		scale = numPages / ginStats.nTotalPages;

		numEntryPages = ceil(ginStats.nEntryPages * scale);
		numDataPages = ceil(ginStats.nDataPages * scale);
		numEntries = ceil(ginStats.nEntries * scale);
		/* ensure we didn't round up too much */
		numEntryPages = Min(numEntryPages, numPages - numPendingPages);
		numDataPages = Min(numDataPages,
						   numPages - numPendingPages - numEntryPages);
	}
	else
	{
		/*
		 * We might get here because it's a hypothetical index, or an index
		 * created pre-9.1 and never vacuumed since upgrading (in which case
		 * its stats would read as zeroes), or just because it's grown too
		 * much since the last VACUUM for us to put our faith in scaling.
		 *
		 * Invent some plausible internal statistics based on the index page
		 * count (and clamp that to at least 10 pages, just in case).  We
		 * estimate that 90% of the index is entry pages, and the rest is data
		 * pages.  Estimate 100 entries per entry page; this is rather bogus
		 * since it'll depend on the size of the keys, but it's more robust
		 * than trying to predict the number of entries per heap tuple.
		 */
		numPages = Max(numPages, 10);
		numEntryPages = floor((numPages - numPendingPages) * 0.90);
		numDataPages = numPages - numPendingPages - numEntryPages;
		numEntries = floor(numEntryPages * 100);
	}

	/* In an empty index, numEntries could be zero.  Avoid divide-by-zero */
	if (numEntries < 1)
		numEntries = 1;

	/*
	 * If the index is partial, AND the index predicate with the index-bound
	 * quals to produce a more accurate idea of the number of rows covered by
	 * the bound conditions.
	 */
	selectivityQuals = add_predicate_to_index_quals(index, indexQuals);

	/* Estimate the fraction of main-table tuples that will be visited */
	*indexSelectivity = clauselist_selectivity(root, selectivityQuals,
											   index->rel->relid,
											   JOIN_INNER,
											   NULL);

	/* fetch estimated page cost for tablespace containing index */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  NULL);

	/*
	 * Generic assumption about index correlation: there isn't any.
	 */
	*indexCorrelation = 0.0;

	/*
	 * Examine quals to estimate number of search entries & partial matches
	 */
	memset(&counts, 0, sizeof(counts));
	counts.arrayScans = 1;
	matchPossible = true;

	foreach(lc, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, lc);
		ListCell   *lc2;

		foreach(lc2, iclause->indexquals)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc2);
			Expr	   *clause = rinfo->clause;

			if (IsA(clause, OpExpr))
			{
				matchPossible = gincost_opexpr(root,
											   index,
											   iclause->indexcol,
											   (OpExpr *) clause,
											   &counts);
				if (!matchPossible)
					break;
			}
			else if (IsA(clause, ScalarArrayOpExpr))
			{
				matchPossible = gincost_scalararrayopexpr(root,
														  index,
														  iclause->indexcol,
														  (ScalarArrayOpExpr *) clause,
														  numEntries,
														  &counts);
				if (!matchPossible)
					break;
			}
			else
			{
				/* shouldn't be anything else for a GIN index */
				elog(ERROR, "unsupported GIN indexqual type: %d",
					 (int) nodeTag(clause));
			}
		}
	}

	/* Fall out if there were any provably-unsatisfiable quals */
	if (!matchPossible)
	{
		*indexStartupCost = 0;
		*indexTotalCost = 0;
		*indexSelectivity = 0;
		return;
	}

	/*
	 * If attribute has a full scan and at the same time doesn't have normal
	 * scan, then we'll have to scan all non-null entries of that attribute.
	 * Currently, we don't have per-attribute statistics for GIN.  Thus, we
	 * must assume the whole GIN index has to be scanned in this case.
	 */
	fullIndexScan = false;
	for (i = 0; i < index->nkeycolumns; i++)
	{
		if (counts.attHasFullScan[i] && !counts.attHasNormalScan[i])
		{
			fullIndexScan = true;
			break;
		}
	}

	if (fullIndexScan || indexQuals == NIL)
	{
		/*
		 * Full index scan will be required.  We treat this as if every key in
		 * the index had been listed in the query; is that reasonable?
		 */
		counts.partialEntries = 0;
		counts.exactEntries = numEntries;
		counts.searchEntries = numEntries;
	}

	/* Will we have more than one iteration of a nestloop scan? */
	outer_scans = loop_count;

	/*
	 * Compute cost to begin scan, first of all, pay attention to pending
	 * list.
	 */
	entryPagesFetched = numPendingPages;

	/*
	 * Estimate number of entry pages read.  We need to do
	 * counts.searchEntries searches.  Use a power function as it should be,
	 * but tuples on leaf pages usually is much greater. Here we include all
	 * searches in entry tree, including search of first entry in partial
	 * match algorithm
	 */
	entryPagesFetched += ceil(counts.searchEntries * rint(pow(numEntryPages, 0.15)));

	/*
	 * Add an estimate of entry pages read by partial match algorithm. It's a
	 * scan over leaf pages in entry tree.  We haven't any useful stats here,
	 * so estimate it as proportion.  Because counts.partialEntries is really
	 * pretty bogus (see code above), it's possible that it is more than
	 * numEntries; clamp the proportion to ensure sanity.
	 */
	partialScale = counts.partialEntries / numEntries;
	partialScale = Min(partialScale, 1.0);

	entryPagesFetched += ceil(numEntryPages * partialScale);

	/*
	 * Partial match algorithm reads all data pages before doing actual scan,
	 * so it's a startup cost.  Again, we haven't any useful stats here, so
	 * estimate it as proportion.
	 */
	dataPagesFetched = ceil(numDataPages * partialScale);

	*indexStartupCost = 0;
	*indexTotalCost = 0;

	/*
	 * Add a CPU-cost component to represent the costs of initial entry btree
	 * descent.  We don't charge any I/O cost for touching upper btree levels,
	 * since they tend to stay in cache, but we still have to do about log2(N)
	 * comparisons to descend a btree of N leaf tuples.  We charge one
	 * cpu_operator_cost per comparison.
	 *
	 * If there are ScalarArrayOpExprs, charge this once per SA scan.  The
	 * ones after the first one are not startup cost so far as the overall
	 * plan is concerned, so add them only to "total" cost.
	 */
	if (numEntries > 1)			/* avoid computing log(0) */
	{
		descentCost = ceil(log(numEntries) / log(2.0)) * cpu_operator_cost;
		*indexStartupCost += descentCost * counts.searchEntries;
		*indexTotalCost += counts.arrayScans * descentCost * counts.searchEntries;
	}

	/*
	 * Add a cpu cost per entry-page fetched. This is not amortized over a
	 * loop.
	 */
	*indexStartupCost += entryPagesFetched * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;
	*indexTotalCost += entryPagesFetched * counts.arrayScans * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/*
	 * Add a cpu cost per data-page fetched. This is also not amortized over a
	 * loop. Since those are the data pages from the partial match algorithm,
	 * charge them as startup cost.
	 */
	*indexStartupCost += DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost * dataPagesFetched;

	/*
	 * Since we add the startup cost to the total cost later on, remove the
	 * initial arrayscan from the total.
	 */
	*indexTotalCost += dataPagesFetched * (counts.arrayScans - 1) * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/*
	 * Calculate cache effects if more than one scan due to nestloops or array
	 * quals.  The result is pro-rated per nestloop scan, but the array qual
	 * factor shouldn't be pro-rated (compare genericcostestimate).
	 */
	if (outer_scans > 1 || counts.arrayScans > 1)
	{
		entryPagesFetched *= outer_scans * counts.arrayScans;
		entryPagesFetched = index_pages_fetched(entryPagesFetched,
												(BlockNumber) numEntryPages,
												numEntryPages, root);
		entryPagesFetched /= outer_scans;
		dataPagesFetched *= outer_scans * counts.arrayScans;
		dataPagesFetched = index_pages_fetched(dataPagesFetched,
											   (BlockNumber) numDataPages,
											   numDataPages, root);
		dataPagesFetched /= outer_scans;
	}

	/*
	 * Here we use random page cost because logically-close pages could be far
	 * apart on disk.
	 */
	*indexStartupCost += (entryPagesFetched + dataPagesFetched) * spc_random_page_cost;

	/*
	 * Now compute the number of data pages fetched during the scan.
	 *
	 * We assume every entry to have the same number of items, and that there
	 * is no overlap between them. (XXX: tsvector and array opclasses collect
	 * statistics on the frequency of individual keys; it would be nice to use
	 * those here.)
	 */
	dataPagesFetched = ceil(numDataPages * counts.exactEntries / numEntries);

	/*
	 * If there is a lot of overlap among the entries, in particular if one of
	 * the entries is very frequent, the above calculation can grossly
	 * under-estimate.  As a simple cross-check, calculate a lower bound based
	 * on the overall selectivity of the quals.  At a minimum, we must read
	 * one item pointer for each matching entry.
	 *
	 * The width of each item pointer varies, based on the level of
	 * compression.  We don't have statistics on that, but an average of
	 * around 3 bytes per item is fairly typical.
	 */
	dataPagesFetchedBySel = ceil(*indexSelectivity *
								 (numTuples / (BLCKSZ / 3)));
	if (dataPagesFetchedBySel > dataPagesFetched)
		dataPagesFetched = dataPagesFetchedBySel;

	/* Add one page cpu-cost to the startup cost */
	*indexStartupCost += DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost * counts.searchEntries;

	/*
	 * Add once again a CPU-cost for those data pages, before amortizing for
	 * cache.
	 */
	*indexTotalCost += dataPagesFetched * counts.arrayScans * DEFAULT_PAGE_CPU_MULTIPLIER * cpu_operator_cost;

	/* Account for cache effects, the same as above */
	if (outer_scans > 1 || counts.arrayScans > 1)
	{
		dataPagesFetched *= outer_scans * counts.arrayScans;
		dataPagesFetched = index_pages_fetched(dataPagesFetched,
											   (BlockNumber) numDataPages,
											   numDataPages, root);
		dataPagesFetched /= outer_scans;
	}

	/* And apply random_page_cost as the cost per page */
	*indexTotalCost += *indexStartupCost +
		dataPagesFetched * spc_random_page_cost;

	/*
	 * Add on index qual eval costs, much as in genericcostestimate. We charge
	 * cpu but we can disregard indexorderbys, since GIN doesn't support
	 * those.
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals);
	qual_op_cost = cpu_operator_cost * list_length(indexQuals);

	*indexStartupCost += qual_arg_cost;
	*indexTotalCost += qual_arg_cost;

	/*
	 * Add a cpu cost per search entry, corresponding to the actual visited
	 * entries.
	 */
	*indexTotalCost += (counts.searchEntries * counts.arrayScans) * (qual_op_cost);
	/* Now add a cpu cost per tuple in the posting lists / trees */
	*indexTotalCost += (numTuples * *indexSelectivity) * (cpu_index_tuple_cost);
	*indexPages = dataPagesFetched;
}

/*
 * BRIN has search behavior completely different from other index types
 */
void
brincostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
				 Cost *indexStartupCost, Cost *indexTotalCost,
				 Selectivity *indexSelectivity, double *indexCorrelation,
				 double *indexPages)
{
	IndexOptInfo *index = path->indexinfo;
	List	   *indexQuals = get_quals_from_indexclauses(path->indexclauses);
	double		numPages = index->pages;
	RelOptInfo *baserel = index->rel;
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
	Cost		spc_seq_page_cost;
	Cost		spc_random_page_cost;
	double		qual_arg_cost;
	double		qualSelectivity;
	BrinStatsData statsData;
	double		indexRanges;
	double		minimalRanges;
	double		estimatedRanges;
	double		selec;
	Relation	indexRel;
	ListCell   *l;
	VariableStatData vardata;

	Assert(rte->rtekind == RTE_RELATION);

	/* fetch estimated page cost for the tablespace containing the index */
	get_tablespace_page_costs(index->reltablespace,
							  &spc_random_page_cost,
							  &spc_seq_page_cost);

	/*
	 * Obtain some data from the index itself, if possible.  Otherwise invent
	 * some plausible internal statistics based on the relation page count.
	 */
	if (!index->hypothetical)
	{
		/*
		 * A lock should have already been obtained on the index in plancat.c.
		 */
		indexRel = index_open(index->indexoid, NoLock);
		brinGetStats(indexRel, &statsData);
		index_close(indexRel, NoLock);

		/* work out the actual number of ranges in the index */
		indexRanges = Max(ceil((double) baserel->pages /
							   statsData.pagesPerRange), 1.0);
	}
	else
	{
		/*
		 * Assume default number of pages per range, and estimate the number
		 * of ranges based on that.
		 */
		indexRanges = Max(ceil((double) baserel->pages /
							   BRIN_DEFAULT_PAGES_PER_RANGE), 1.0);

		statsData.pagesPerRange = BRIN_DEFAULT_PAGES_PER_RANGE;
		statsData.revmapNumPages = (indexRanges / REVMAP_PAGE_MAXITEMS) + 1;
	}

	/*
	 * Compute index correlation
	 *
	 * Because we can use all index quals equally when scanning, we can use
	 * the largest correlation (in absolute value) among columns used by the
	 * query.  Start at zero, the worst possible case.  If we cannot find any
	 * correlation statistics, we will keep it as 0.
	 */
	*indexCorrelation = 0;

	foreach(l, path->indexclauses)
	{
		IndexClause *iclause = lfirst_node(IndexClause, l);
		AttrNumber	attnum = index->indexkeys[iclause->indexcol];

		/* attempt to lookup stats in relation for this index column */
		if (attnum != 0)
		{
			/* Simple variable -- look to stats for the underlying table */
			if (get_relation_stats_hook &&
				(*get_relation_stats_hook) (root, rte, attnum, &vardata))
			{
				/*
				 * The hook took control of acquiring a stats tuple.  If it
				 * did supply a tuple, it'd better have supplied a freefunc.
				 */
				if (HeapTupleIsValid(vardata.statsTuple) && !vardata.freefunc)
					elog(ERROR,
						 "no function provided to release variable stats with");
			}
			else
			{
				vardata.statsTuple =
					SearchSysCache3(STATRELATTINH,
									ObjectIdGetDatum(rte->relid),
									Int16GetDatum(attnum),
									BoolGetDatum(false));
				vardata.freefunc = ReleaseSysCache;
			}
		}
		else
		{
			/*
			 * Looks like we've found an expression column in the index. Let's
			 * see if there's any stats for it.
			 */

			/* get the attnum from the 0-based index. */
			attnum = iclause->indexcol + 1;

			if (get_index_stats_hook &&
				(*get_index_stats_hook) (root, index->indexoid, attnum, &vardata))
			{
				/*
				 * The hook took control of acquiring a stats tuple.  If it
				 * did supply a tuple, it'd better have supplied a freefunc.
				 */
				if (HeapTupleIsValid(vardata.statsTuple) &&
					!vardata.freefunc)
					elog(ERROR, "no function provided to release variable stats with");
			}
			else
			{
				vardata.statsTuple = SearchSysCache3(STATRELATTINH,
													 ObjectIdGetDatum(index->indexoid),
													 Int16GetDatum(attnum),
													 BoolGetDatum(false));
				vardata.freefunc = ReleaseSysCache;
			}
		}

		if (HeapTupleIsValid(vardata.statsTuple))
		{
			AttStatsSlot sslot;

			if (get_attstatsslot(&sslot, vardata.statsTuple,
								 STATISTIC_KIND_CORRELATION, InvalidOid,
								 ATTSTATSSLOT_NUMBERS))
			{
				double		varCorrelation = 0.0;

				if (sslot.nnumbers > 0)
					varCorrelation = fabs(sslot.numbers[0]);

				if (varCorrelation > *indexCorrelation)
					*indexCorrelation = varCorrelation;

				free_attstatsslot(&sslot);
			}
		}

		ReleaseVariableStats(vardata);
	}

	qualSelectivity = clauselist_selectivity(root, indexQuals,
											 baserel->relid,
											 JOIN_INNER, NULL);

	/*
	 * Now calculate the minimum possible ranges we could match with if all of
	 * the rows were in the perfect order in the table's heap.
	 */
	minimalRanges = ceil(indexRanges * qualSelectivity);

	/*
	 * Now estimate the number of ranges that we'll touch by using the
	 * indexCorrelation from the stats. Careful not to divide by zero (note
	 * we're using the absolute value of the correlation).
	 */
	if (*indexCorrelation < 1.0e-10)
		estimatedRanges = indexRanges;
	else
		estimatedRanges = Min(minimalRanges / *indexCorrelation, indexRanges);

	/* we expect to visit this portion of the table */
	selec = estimatedRanges / indexRanges;

	CLAMP_PROBABILITY(selec);

	*indexSelectivity = selec;

	/*
	 * Compute the index qual costs, much as in genericcostestimate, to add to
	 * the index costs.  We can disregard indexorderbys, since BRIN doesn't
	 * support those.
	 */
	qual_arg_cost = index_other_operands_eval_cost(root, indexQuals);

	/*
	 * Compute the startup cost as the cost to read the whole revmap
	 * sequentially, including the cost to execute the index quals.
	 */
	*indexStartupCost =
		spc_seq_page_cost * statsData.revmapNumPages * loop_count;
	*indexStartupCost += qual_arg_cost;

	/*
	 * To read a BRIN index there might be a bit of back and forth over
	 * regular pages, as revmap might point to them out of sequential order;
	 * calculate the total cost as reading the whole index in random order.
	 */
	*indexTotalCost = *indexStartupCost +
		spc_random_page_cost * (numPages - statsData.revmapNumPages) * loop_count;

	/*
	 * Charge a small amount per range tuple which we expect to match to. This
	 * is meant to reflect the costs of manipulating the bitmap. The BRIN scan
	 * will set a bit for each page in the range when we find a matching
	 * range, so we must multiply the charge by the number of pages in the
	 * range.
	 */
	*indexTotalCost += 0.1 * cpu_operator_cost * estimatedRanges *
		statsData.pagesPerRange;

	*indexPages = index->pages;
}
