/*-------------------------------------------------------------------------
 *
 * prepagg.c
 *	  Routines to preprocess aggregate function calls
 *
 * If there are identical aggregate calls in the query, they only need to
 * be computed once.  Also, some aggregate functions can share the same
 * transition state, so that we only need to call the final function for
 * them separately.  These optimizations are independent of how the
 * aggregates are executed.
 *
 * preprocess_aggrefs() detects those cases, creates AggInfo and
 * AggTransInfo structs for each aggregate and transition state that needs
 * to be computed, and sets the 'aggno' and 'transno' fields in the Aggrefs
 * accordingly.  It also resolves polymorphic transition types, and sets
 * the 'aggtranstype' fields accordingly.
 *
 * XXX: The AggInfo and AggTransInfo structs are thrown away after
 * planning, so executor startup has to perform some of the same lookups
 * of transition functions and initial values that we do here.  One day, we
 * might want to carry that information to the Agg nodes to save the effort
 * at executor startup.  The Agg nodes are constructed much later in the
 * planning, however, so it's not trivial.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/prepagg.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "nodes/pathnodes.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/plancat.h"
#include "optimizer/prep.h"
#include "parser/parse_agg.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

static bool preprocess_aggrefs_walker(Node *node, PlannerInfo *root);
static int	find_compatible_agg(PlannerInfo *root, Aggref *newagg,
								List **same_input_transnos);
static int	find_compatible_trans(PlannerInfo *root, Aggref *newagg,
								  bool shareable,
								  Oid aggtransfn, Oid aggtranstype,
								  int transtypeLen, bool transtypeByVal,
								  Oid aggcombinefn,
								  Oid aggserialfn, Oid aggdeserialfn,
								  Datum initValue, bool initValueIsNull,
								  List *transnos);
static Datum GetAggInitVal(Datum textInitVal, Oid transtype);

/* -----------------
 * Resolve the transition type of all Aggrefs, and determine which Aggrefs
 * can share aggregate or transition state.
 *
 * Information about the aggregates and transition functions are collected
 * in the root->agginfos and root->aggtransinfos lists.  The 'aggtranstype',
 * 'aggno', and 'aggtransno' fields of each Aggref are filled in.
 *
 * NOTE: This modifies the Aggrefs in the input expression in-place!
 *
 * We try to optimize by detecting duplicate aggregate functions so that
 * their state and final values are re-used, rather than needlessly being
 * re-calculated independently.  We also detect aggregates that are not
 * the same, but which can share the same transition state.
 *
 * Scenarios:
 *
 * 1. Identical aggregate function calls appear in the query:
 *
 *	  SELECT SUM(x) FROM ... HAVING SUM(x) > 0
 *
 *	  Since these aggregates are identical, we only need to calculate
 *	  the value once.  Both aggregates will share the same 'aggno' value.
 *
 * 2. Two different aggregate functions appear in the query, but the
 *	  aggregates have the same arguments, transition functions and
 *	  initial values (and, presumably, different final functions):
 *
 *	  SELECT AVG(x), STDDEV(x) FROM ...
 *
 *	  In this case we must create a new AggInfo for the varying aggregate,
 *	  and we need to call the final functions separately, but we need
 *	  only run the transition function once.  (This requires that the
 *	  final functions be nondestructive of the transition state, but
 *	  that's required anyway for other reasons.)
 *
 * For either of these optimizations to be valid, all aggregate properties
 * used in the transition phase must be the same, including any modifiers
 * such as ORDER BY, DISTINCT and FILTER, and the arguments mustn't
 * contain any volatile functions.
 * -----------------
 */
void
preprocess_aggrefs(PlannerInfo *root, Node *clause)
{
	(void) preprocess_aggrefs_walker(clause, root);
}

static void
preprocess_aggref(Aggref *aggref, PlannerInfo *root)
{
	HeapTuple	aggTuple;
	Form_pg_aggregate aggform;
	Oid			aggtransfn;
	Oid			aggfinalfn;
	Oid			aggcombinefn;
	Oid			aggserialfn;
	Oid			aggdeserialfn;
	Oid			aggtranstype;
	int32		aggtranstypmod;
	int32		aggtransspace;
	bool		shareable;
	int			aggno;
	int			transno;
	List	   *same_input_transnos;
	int16		resulttypeLen;
	bool		resulttypeByVal;
	Datum		textInitVal;
	Datum		initValue;
	bool		initValueIsNull;
	bool		transtypeByVal;
	int16		transtypeLen;
	Oid			inputTypes[FUNC_MAX_ARGS];
	int			numArguments;

	Assert(aggref->agglevelsup == 0);

	/*
	 * Fetch info about the aggregate from pg_aggregate.  Note it's correct to
	 * ignore the moving-aggregate variant, since what we're concerned with
	 * here is aggregates not window functions.
	 */
	aggTuple = SearchSysCache1(AGGFNOID,
							   ObjectIdGetDatum(aggref->aggfnoid));
	if (!HeapTupleIsValid(aggTuple))
		elog(ERROR, "cache lookup failed for aggregate %u",
			 aggref->aggfnoid);
	aggform = (Form_pg_aggregate) GETSTRUCT(aggTuple);
	aggtransfn = aggform->aggtransfn;
	aggfinalfn = aggform->aggfinalfn;
	aggcombinefn = aggform->aggcombinefn;
	aggserialfn = aggform->aggserialfn;
	aggdeserialfn = aggform->aggdeserialfn;
	aggtranstype = aggform->aggtranstype;
	aggtransspace = aggform->aggtransspace;

	/*
	 * Resolve the possibly-polymorphic aggregate transition type.
	 */

	/* extract argument types (ignoring any ORDER BY expressions) */
	numArguments = get_aggregate_argtypes(aggref, inputTypes);

	/* resolve actual type of transition state, if polymorphic */
	aggtranstype = resolve_aggregate_transtype(aggref->aggfnoid,
											   aggtranstype,
											   inputTypes,
											   numArguments);
	aggref->aggtranstype = aggtranstype;

	/*
	 * If transition state is of same type as first aggregated input, assume
	 * it's the same typmod (same width) as well.  This works for cases like
	 * MAX/MIN and is probably somewhat reasonable otherwise.
	 */
	aggtranstypmod = -1;
	if (aggref->args)
	{
		TargetEntry *tle = (TargetEntry *) linitial(aggref->args);

		if (aggtranstype == exprType((Node *) tle->expr))
			aggtranstypmod = exprTypmod((Node *) tle->expr);
	}

	/*
	 * If finalfn is marked read-write, we can't share transition states; but
	 * it is okay to share states for AGGMODIFY_SHAREABLE aggs.
	 *
	 * In principle, in a partial aggregate, we could share the transition
	 * state even if the final function is marked as read-write, because the
	 * partial aggregate doesn't execute the final function.  But it's too
	 * early to know whether we're going perform a partial aggregate.
	 */
	shareable = (aggform->aggfinalmodify != AGGMODIFY_READ_WRITE);

	/* get info about the output value's datatype */
	get_typlenbyval(aggref->aggtype,
					&resulttypeLen,
					&resulttypeByVal);

	/* get initial value */
	textInitVal = SysCacheGetAttr(AGGFNOID, aggTuple,
								  Anum_pg_aggregate_agginitval,
								  &initValueIsNull);
	if (initValueIsNull)
		initValue = (Datum) 0;
	else
		initValue = GetAggInitVal(textInitVal, aggtranstype);

	ReleaseSysCache(aggTuple);

	/*
	 * 1. See if this is identical to another aggregate function call that
	 * we've seen already.
	 */
	aggno = find_compatible_agg(root, aggref, &same_input_transnos);
	if (aggno != -1)
	{
		AggInfo    *agginfo = list_nth_node(AggInfo, root->agginfos, aggno);

		agginfo->aggrefs = lappend(agginfo->aggrefs, aggref);
		transno = agginfo->transno;
	}
	else
	{
		AggInfo    *agginfo = makeNode(AggInfo);

		agginfo->finalfn_oid = aggfinalfn;
		agginfo->aggrefs = list_make1(aggref);
		agginfo->shareable = shareable;

		aggno = list_length(root->agginfos);
		root->agginfos = lappend(root->agginfos, agginfo);

		/*
		 * Count it, and check for cases requiring ordered input.  Note that
		 * ordered-set aggs always have nonempty aggorder.  Any ordered-input
		 * case also defeats partial aggregation.
		 */
		if (aggref->aggorder != NIL || aggref->aggdistinct != NIL)
		{
			root->numOrderedAggs++;
			root->hasNonPartialAggs = true;
		}

		get_typlenbyval(aggtranstype,
						&transtypeLen,
						&transtypeByVal);

		/*
		 * 2. See if this aggregate can share transition state with another
		 * aggregate that we've initialized already.
		 */
		transno = find_compatible_trans(root, aggref, shareable,
										aggtransfn, aggtranstype,
										transtypeLen, transtypeByVal,
										aggcombinefn,
										aggserialfn, aggdeserialfn,
										initValue, initValueIsNull,
										same_input_transnos);
		if (transno == -1)
		{
			AggTransInfo *transinfo = makeNode(AggTransInfo);

			transinfo->args = aggref->args;
			transinfo->aggfilter = aggref->aggfilter;
			transinfo->transfn_oid = aggtransfn;
			transinfo->combinefn_oid = aggcombinefn;
			transinfo->serialfn_oid = aggserialfn;
			transinfo->deserialfn_oid = aggdeserialfn;
			transinfo->aggtranstype = aggtranstype;
			transinfo->aggtranstypmod = aggtranstypmod;
			transinfo->transtypeLen = transtypeLen;
			transinfo->transtypeByVal = transtypeByVal;
			transinfo->aggtransspace = aggtransspace;
			transinfo->initValue = initValue;
			transinfo->initValueIsNull = initValueIsNull;

			transno = list_length(root->aggtransinfos);
			root->aggtransinfos = lappend(root->aggtransinfos, transinfo);

			/*
			 * Check whether partial aggregation is feasible, unless we
			 * already found out that we can't do it.
			 */
			if (!root->hasNonPartialAggs)
			{
				/*
				 * If there is no combine function, then partial aggregation
				 * is not possible.
				 */
				if (!OidIsValid(transinfo->combinefn_oid))
					root->hasNonPartialAggs = true;

				/*
				 * If we have any aggs with transtype INTERNAL then we must
				 * check whether they have serialization/deserialization
				 * functions; if not, we can't serialize partial-aggregation
				 * results.
				 */
				else if (transinfo->aggtranstype == INTERNALOID)
				{

					if (!OidIsValid(transinfo->serialfn_oid) ||
						!OidIsValid(transinfo->deserialfn_oid))
						root->hasNonSerialAggs = true;

					/*
					 * array_agg_serialize and array_agg_deserialize make use
					 * of the aggregate non-byval input type's send and
					 * receive functions.  There's a chance that the type
					 * being aggregated has one or both of these functions
					 * missing.  In this case we must not allow the
					 * aggregate's serial and deserial functions to be used.
					 * It would be nice not to have special case this and
					 * instead provide some sort of supporting function within
					 * the aggregate to do this, but for now, that seems like
					 * overkill for this one case.
					 */
					if ((transinfo->serialfn_oid == F_ARRAY_AGG_SERIALIZE ||
						 transinfo->deserialfn_oid == F_ARRAY_AGG_DESERIALIZE) &&
						!agg_args_support_sendreceive(aggref))
						root->hasNonSerialAggs = true;
				}
			}
		}
		agginfo->transno = transno;
	}

	/*
	 * Fill in the fields in the Aggref (aggtranstype was set above already)
	 */
	aggref->aggno = aggno;
	aggref->aggtransno = transno;
}

static bool
preprocess_aggrefs_walker(Node *node, PlannerInfo *root)
{
	if (node == NULL)
		return false;
	if (IsA(node, Aggref))
	{
		Aggref	   *aggref = (Aggref *) node;

		preprocess_aggref(aggref, root);

		/*
		 * We assume that the parser checked that there are no aggregates (of
		 * this level anyway) in the aggregated arguments, direct arguments,
		 * or filter clause.  Hence, we need not recurse into any of them.
		 */
		return false;
	}
	Assert(!IsA(node, SubLink));
	return expression_tree_walker(node, preprocess_aggrefs_walker, root);
}


/*
 * find_compatible_agg - search for a previously initialized per-Agg struct
 *
 * Searches the previously looked at aggregates to find one which is compatible
 * with this one, with the same input parameters.  If no compatible aggregate
 * can be found, returns -1.
 *
 * As a side-effect, this also collects a list of existing, shareable per-Trans
 * structs with matching inputs.  If no identical Aggref is found, the list is
 * passed later to find_compatible_trans, to see if we can at least reuse
 * the state value of another aggregate.
 */
static int
find_compatible_agg(PlannerInfo *root, Aggref *newagg,
					List **same_input_transnos)
{
	ListCell   *lc;
	int			aggno;

	*same_input_transnos = NIL;

	/* we mustn't reuse the aggref if it contains volatile function calls */
	if (contain_volatile_functions((Node *) newagg))
		return -1;

	/*
	 * Search through the list of already seen aggregates.  If we find an
	 * existing identical aggregate call, then we can re-use that one.  While
	 * searching, we'll also collect a list of Aggrefs with the same input
	 * parameters.  If no matching Aggref is found, the caller can potentially
	 * still re-use the transition state of one of them.  (At this stage we
	 * just compare the parsetrees; whether different aggregates share the
	 * same transition function will be checked later.)
	 */
	aggno = -1;
	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *existingRef;

		aggno++;

		existingRef = linitial_node(Aggref, agginfo->aggrefs);

		/* all of the following must be the same or it's no match */
		if (newagg->inputcollid != existingRef->inputcollid ||
			newagg->aggtranstype != existingRef->aggtranstype ||
			newagg->aggstar != existingRef->aggstar ||
			newagg->aggvariadic != existingRef->aggvariadic ||
			newagg->aggkind != existingRef->aggkind ||
			!equal(newagg->args, existingRef->args) ||
			!equal(newagg->aggorder, existingRef->aggorder) ||
			!equal(newagg->aggdistinct, existingRef->aggdistinct) ||
			!equal(newagg->aggfilter, existingRef->aggfilter))
			continue;

		/* if it's the same aggregate function then report exact match */
		if (newagg->aggfnoid == existingRef->aggfnoid &&
			newagg->aggtype == existingRef->aggtype &&
			newagg->aggcollid == existingRef->aggcollid &&
			equal(newagg->aggdirectargs, existingRef->aggdirectargs))
		{
			list_free(*same_input_transnos);
			*same_input_transnos = NIL;
			return aggno;
		}

		/*
		 * Not identical, but it had the same inputs.  If the final function
		 * permits sharing, return its transno to the caller, in case we can
		 * re-use its per-trans state.  (If there's already sharing going on,
		 * we might report a transno more than once.  find_compatible_trans is
		 * cheap enough that it's not worth spending cycles to avoid that.)
		 */
		if (agginfo->shareable)
			*same_input_transnos = lappend_int(*same_input_transnos,
											   agginfo->transno);
	}

	return -1;
}

/*
 * find_compatible_trans - search for a previously initialized per-Trans
 * struct
 *
 * Searches the list of transnos for a per-Trans struct with the same
 * transition function and initial condition. (The inputs have already been
 * verified to match.)
 */
static int
find_compatible_trans(PlannerInfo *root, Aggref *newagg, bool shareable,
					  Oid aggtransfn, Oid aggtranstype,
					  int transtypeLen, bool transtypeByVal,
					  Oid aggcombinefn,
					  Oid aggserialfn, Oid aggdeserialfn,
					  Datum initValue, bool initValueIsNull,
					  List *transnos)
{
	ListCell   *lc;

	/* If this aggregate can't share transition states, give up */
	if (!shareable)
		return -1;

	foreach(lc, transnos)
	{
		int			transno = lfirst_int(lc);
		AggTransInfo *pertrans = list_nth_node(AggTransInfo,
											   root->aggtransinfos,
											   transno);

		/*
		 * if the transfns or transition state types are not the same then the
		 * state can't be shared.
		 */
		if (aggtransfn != pertrans->transfn_oid ||
			aggtranstype != pertrans->aggtranstype)
			continue;

		/*
		 * The serialization and deserialization functions must match, if
		 * present, as we're unable to share the trans state for aggregates
		 * which will serialize or deserialize into different formats.
		 * Remember that these will be InvalidOid if they're not required for
		 * this agg node.
		 */
		if (aggserialfn != pertrans->serialfn_oid ||
			aggdeserialfn != pertrans->deserialfn_oid)
			continue;

		/*
		 * Combine function must also match.  We only care about the combine
		 * function with partial aggregates, but it's too early in the
		 * planning to know if we will do partial aggregation, so be
		 * conservative.
		 */
		if (aggcombinefn != pertrans->combinefn_oid)
			continue;

		/*
		 * Check that the initial condition matches, too.
		 */
		if (initValueIsNull && pertrans->initValueIsNull)
			return transno;

		if (!initValueIsNull && !pertrans->initValueIsNull &&
			datumIsEqual(initValue, pertrans->initValue,
						 transtypeByVal, transtypeLen))
			return transno;
	}
	return -1;
}

static Datum
GetAggInitVal(Datum textInitVal, Oid transtype)
{
	Oid			typinput,
				typioparam;
	char	   *strInitVal;
	Datum		initVal;

	getTypeInputInfo(transtype, &typinput, &typioparam);
	strInitVal = TextDatumGetCString(textInitVal);
	initVal = OidInputFunctionCall(typinput, strInitVal,
								   typioparam, -1);
	pfree(strInitVal);
	return initVal;
}


/*
 * get_agg_clause_costs
 *	  Process the PlannerInfo's 'aggtransinfos' and 'agginfos' lists
 *	  accumulating the cost information about them.
 *
 * 'aggsplit' tells us the expected partial-aggregation mode, which affects
 * the cost estimates.
 *
 * NOTE that the costs are ADDED to those already in *costs ... so the caller
 * is responsible for zeroing the struct initially.
 *
 * For each AggTransInfo, we add the cost of an aggregate transition using
 * either the transfn or combinefn depending on the 'aggsplit' value.  We also
 * account for the costs of any aggfilters and any serializations and
 * deserializations of the transition state and also estimate the total space
 * needed for the transition states as if each aggregate's state was stored in
 * memory concurrently (as would be done in a HashAgg plan).
 *
 * For each AggInfo in the 'agginfos' list we add the cost of running the
 * final function and the direct args, if any.
 */
void
get_agg_clause_costs(PlannerInfo *root, AggSplit aggsplit, AggClauseCosts *costs)
{
	ListCell   *lc;

	foreach(lc, root->aggtransinfos)
	{
		AggTransInfo *transinfo = lfirst_node(AggTransInfo, lc);

		/*
		 * Add the appropriate component function execution costs to
		 * appropriate totals.
		 */
		if (DO_AGGSPLIT_COMBINE(aggsplit))
		{
			/* charge for combining previously aggregated states */
			add_function_cost(root, transinfo->combinefn_oid, NULL,
							  &costs->transCost);
		}
		else
			add_function_cost(root, transinfo->transfn_oid, NULL,
							  &costs->transCost);
		if (DO_AGGSPLIT_DESERIALIZE(aggsplit) &&
			OidIsValid(transinfo->deserialfn_oid))
			add_function_cost(root, transinfo->deserialfn_oid, NULL,
							  &costs->transCost);
		if (DO_AGGSPLIT_SERIALIZE(aggsplit) &&
			OidIsValid(transinfo->serialfn_oid))
			add_function_cost(root, transinfo->serialfn_oid, NULL,
							  &costs->finalCost);

		/*
		 * These costs are incurred only by the initial aggregate node, so we
		 * mustn't include them again at upper levels.
		 */
		if (!DO_AGGSPLIT_COMBINE(aggsplit))
		{
			/* add the input expressions' cost to per-input-row costs */
			QualCost	argcosts;

			cost_qual_eval_node(&argcosts, (Node *) transinfo->args, root);
			costs->transCost.startup += argcosts.startup;
			costs->transCost.per_tuple += argcosts.per_tuple;

			/*
			 * Add any filter's cost to per-input-row costs.
			 *
			 * XXX Ideally we should reduce input expression costs according
			 * to filter selectivity, but it's not clear it's worth the
			 * trouble.
			 */
			if (transinfo->aggfilter)
			{
				cost_qual_eval_node(&argcosts, (Node *) transinfo->aggfilter,
									root);
				costs->transCost.startup += argcosts.startup;
				costs->transCost.per_tuple += argcosts.per_tuple;
			}
		}

		/*
		 * If the transition type is pass-by-value then it doesn't add
		 * anything to the required size of the hashtable.  If it is
		 * pass-by-reference then we have to add the estimated size of the
		 * value itself, plus palloc overhead.
		 */
		if (!transinfo->transtypeByVal)
		{
			int32		avgwidth;

			/* Use average width if aggregate definition gave one */
			if (transinfo->aggtransspace > 0)
				avgwidth = transinfo->aggtransspace;
			else if (transinfo->transfn_oid == F_ARRAY_APPEND)
			{
				/*
				 * If the transition function is array_append(), it'll use an
				 * expanded array as transvalue, which will occupy at least
				 * ALLOCSET_SMALL_INITSIZE and possibly more.  Use that as the
				 * estimate for lack of a better idea.
				 */
				avgwidth = ALLOCSET_SMALL_INITSIZE;
			}
			else
			{
				avgwidth = get_typavgwidth(transinfo->aggtranstype, transinfo->aggtranstypmod);
			}

			avgwidth = MAXALIGN(avgwidth);
			costs->transitionSpace += avgwidth + 2 * sizeof(void *);
		}
		else if (transinfo->aggtranstype == INTERNALOID)
		{
			/*
			 * INTERNAL transition type is a special case: although INTERNAL
			 * is pass-by-value, it's almost certainly being used as a pointer
			 * to some large data structure.  The aggregate definition can
			 * provide an estimate of the size.  If it doesn't, then we assume
			 * ALLOCSET_DEFAULT_INITSIZE, which is a good guess if the data is
			 * being kept in a private memory context, as is done by
			 * array_agg() for instance.
			 */
			if (transinfo->aggtransspace > 0)
				costs->transitionSpace += transinfo->aggtransspace;
			else
				costs->transitionSpace += ALLOCSET_DEFAULT_INITSIZE;
		}
	}

	foreach(lc, root->agginfos)
	{
		AggInfo    *agginfo = lfirst_node(AggInfo, lc);
		Aggref	   *aggref = linitial_node(Aggref, agginfo->aggrefs);

		/*
		 * Add the appropriate component function execution costs to
		 * appropriate totals.
		 */
		if (!DO_AGGSPLIT_SKIPFINAL(aggsplit) &&
			OidIsValid(agginfo->finalfn_oid))
			add_function_cost(root, agginfo->finalfn_oid, NULL,
							  &costs->finalCost);

		/*
		 * If there are direct arguments, treat their evaluation cost like the
		 * cost of the finalfn.
		 */
		if (aggref->aggdirectargs)
		{
			QualCost	argcosts;

			cost_qual_eval_node(&argcosts, (Node *) aggref->aggdirectargs,
								root);
			costs->finalCost.startup += argcosts.startup;
			costs->finalCost.per_tuple += argcosts.per_tuple;
		}
	}
}
